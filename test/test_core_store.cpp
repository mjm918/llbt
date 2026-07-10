/*
** llbt — Low Level Binary Tree
**
** The files authored by the llbt project (such as this one) are dedicated
** to the public domain. To the extent possible under law, all copyright and
** related rights are waived. You may use, copy, modify, merge, publish,
** distribute, sublicense, sell, fork, or build upon them for any purpose,
** commercial or not, with or without attribution, with no conditions and no
** warranty.
**
** This applies only to files authored by the llbt project. The imported
** storage engine underneath keeps its own copyright and the Apache License
** 2.0; see LICENSE and NOTICE.
*/
#include "testsettings.hpp"
#ifdef TEST_CORE_STORE

#include <llbt/core.hpp>
#include <llbt/array_binary.hpp>

#include "test.hpp"

using namespace llbt;
using namespace llbt::core;
using namespace llbt::test_util;

// The public core layer: Store/Tx/named roots/Tree/Cursor.
// Everything runs against a real file through the full commit machinery.

TEST(CoreStore_TreeDurabilityAcrossReopen)
{
    TEST_PATH(path);
    // Session 1: create, fill, commit.
    {
        StoreRef store = Store::open(path);
        Tx tx = store->begin_write();
        Tree<int64_t> t = tx.tree<int64_t>("scores");
        for (int64_t i = 0; i < 10000; ++i)
            t.add(i * 7);
        CHECK_EQUAL(t.size(), 10000);
        tx.commit();
    }
    // Session 2: fresh handle (as after restart), verify, mutate, verify.
    {
        StoreRef store = Store::open(path);
        {
            Tx tx = store->begin_read();
            Tree<int64_t> t = tx.tree<int64_t>("scores");
            CHECK_EQUAL(t.size(), 10000);
            CHECK_EQUAL(t.get(0), 0);
            CHECK_EQUAL(t.get(9999), 9999 * 7);
            CHECK_EQUAL(t.find_first(21), 3);
        }
        {
            Tx tx = store->begin_write();
            Tree<int64_t> t = tx.tree<int64_t>("scores");
            t.erase(0);
            t.insert(0, -1);
            t.set(1, -2);
            tx.commit();
        }
        Tx tx = store->begin_read();
        Tree<int64_t> t = tx.tree<int64_t>("scores");
        CHECK_EQUAL(t.get(0), -1);
        CHECK_EQUAL(t.get(1), -2);
        CHECK_EQUAL(t.size(), 10000);
    }
}

TEST(CoreStore_NamedRootsDirectory)
{
    TEST_PATH(path);
    StoreRef store = Store::open(path);
    {
        Tx tx = store->begin_write();
        tx.tree<int64_t>("alpha").add(1);
        tx.tree<StringData>("beta").add("hello");
        tx.tree<double>("gamma").add(3.25);
        CHECK_EQUAL(tx.root_count(), 3);
        tx.commit();
    }
    {
        Tx tx = store->begin_write();
        CHECK_EQUAL(tx.root_count(), 3);
        CHECK(tx.get_root("alpha"));
        CHECK(!tx.get_root("nope"));
        // type safety: alpha is a Tree<int64_t>
        CHECK_THROW_ANY(tx.tree<StringData>("alpha"));
        CHECK(tx.erase_root("alpha"));
        CHECK(!tx.erase_root("alpha"));
        CHECK_EQUAL(tx.root_count(), 2);
        tx.commit();
    }
    {
        Tx tx = store->begin_read();
        CHECK_EQUAL(tx.root_count(), 2);
        CHECK(!tx.get_root("alpha"));
        Tree<StringData> b = tx.tree<StringData>("beta");
        CHECK_EQUAL(b.get(0), "hello");
    }
}

TEST(CoreStore_CursorIteration)
{
    TEST_PATH(path);
    StoreRef store = Store::open(path);
    {
        Tx tx = store->begin_write();
        Tree<StringData> t = tx.tree<StringData>("words");
        // keep it sorted ourselves — our data structure, our rules
        t.add("apple");
        t.add("banana");
        t.add("cherry");
        t.add("damson");
        tx.commit();
    }
    Tx tx = store->begin_read();
    Tree<StringData> t = tx.tree<StringData>("words");
    std::string walked;
    for (auto cur = t.cursor(); cur.valid(); cur.next())
        walked += std::string(cur.value()) + ",";
    CHECK_EQUAL(walked, "apple,banana,cherry,damson,");

    auto cur = t.cursor();
    cur.last();
    CHECK_EQUAL(cur.value(), "damson");
    cur.prev();
    CHECK_EQUAL(cur.value(), "cherry");
    cur.seek(0);
    CHECK_EQUAL(cur.value(), "apple");
}

TEST(CoreStore_MVCCSnapshotIsolation)
{
    TEST_PATH(path);
    StoreRef store = Store::open(path);
    {
        Tx tx = store->begin_write();
        Tree<int64_t> t = tx.tree<int64_t>("n");
        for (int64_t i = 0; i < 100; ++i)
            t.add(i);
        tx.commit();
    }

    Tx reader = store->begin_read(); // pins version with 100 elements
    {
        Tx writer = store->begin_write();
        Tree<int64_t> t = writer.tree<int64_t>("n");
        for (int64_t i = 100; i < 200; ++i)
            t.add(i);
        writer.commit();
    }
    // the pinned snapshot is unchanged; a fresh one sees the commit
    CHECK_EQUAL(reader.tree<int64_t>("n").size(), 100);
    CHECK_EQUAL(store->begin_read().tree<int64_t>("n").size(), 200);
}

TEST(CoreStore_RollbackAndRAII)
{
    TEST_PATH(path);
    StoreRef store = Store::open(path);
    {
        Tx tx = store->begin_write();
        tx.tree<int64_t>("kept").add(1);
        tx.commit();
    }
    {
        Tx tx = store->begin_write();
        tx.tree<int64_t>("kept").add(2);
        tx.rollback();
    }
    {
        // uncommitted write Tx rolls back when it goes out of scope
        Tx tx = store->begin_write();
        tx.tree<int64_t>("kept").add(3);
    }
    Tx tx = store->begin_read();
    CHECK_EQUAL(tx.tree<int64_t>("kept").size(), 1);
    // read transactions refuse writes
    Tree<int64_t> t = tx.tree<int64_t>("kept");
    CHECK_THROW_ANY(t.add(4));
}

// "Bring your own data structure": raw Array nodes anchored under a named
// root — a list of binary blobs assembled by hand, no Tree<T> involved.
TEST(CoreStore_BringYourOwnStructure)
{
    TEST_PATH(path);
    StoreRef store = Store::open(path);
    {
        Tx tx = store->begin_write();
        Allocator& alloc = tx.alloc();

        ArrayBinary blobs(alloc);
        blobs.create();
        blobs.add(BinaryData("first blob", 10));
        blobs.add(BinaryData("second", 6));

        tx.set_root("my-blobs", blobs.get_ref());
        tx.commit();
    }
    {
        StoreRef reopened = Store::open(path);
        Tx tx = reopened->begin_read();
        auto ref = tx.get_root("my-blobs");
        CHECK(bool(ref));

        ArrayBinary blobs(tx.alloc());
        blobs.init_from_ref(*ref);
        CHECK_EQUAL(blobs.size(), 2);
        CHECK_EQUAL(blobs.get(0), BinaryData("first blob", 10));
        CHECK_EQUAL(blobs.get(1), BinaryData("second", 6));
    }
}

// Replacing and erasing roots must return their pages to the free list —
// the file must not grow when the same amount of data is rewritten.
TEST(CoreStore_ReplacedRootsDoNotLeak)
{
    TEST_PATH(path);
    StoreRef store = Store::open(path);

    auto write_generation = [&](int64_t seed) {
        Tx tx = store->begin_write();
        tx.erase_root("gen");
        Tree<int64_t> t = tx.tree<int64_t>("gen");
        for (int64_t i = 0; i < 5000; ++i)
            t.add(seed + i);
        tx.commit();
    };

    // Warm up: let the file reach its steady working size.
    for (int64_t g = 0; g < 3; ++g)
        write_generation(g * 1000);
    util::File f(store->path());
    auto steady_size = f.get_size();

    // 20 more full rewrites of the same live payload. If replaced trees were
    // not freed back to the free list, the file would grow ~linearly with
    // generations; with correct freeing it plateaus.
    for (int64_t g = 3; g <= 23; ++g)
        write_generation(g * 1000);
    auto size_after_many = f.get_size();
    CHECK_LESS_EQUAL(size_after_many, steady_size * 2);

    // And a compacted file with identical live data stays the same size
    // no matter how many generations preceded it.
    store->compact();
    auto compacted = f.get_size();
    CHECK_LESS_EQUAL(compacted, steady_size);
}

// In-memory store: the same engine with no backing file. Everything works
// exactly as on disk, but nothing is persisted and no file is created.
TEST(CoreStore_InMemoryBasics)
{
    StoreRef store = Store::open_in_memory();
    CHECK(store->is_in_memory());
    CHECK_EQUAL(store->path(), "<llbt in-memory>");
    CHECK(!util::File::exists(store->path())); // no file on disk

    // Two typed trees and a hand-built blob root, all in one commit.
    {
        Tx tx = store->begin_write();
        Tree<int64_t> nums = tx.tree<int64_t>("nums");
        for (int64_t i = 0; i < 5000; ++i)
            nums.add(i * 3);
        tx.tree<StringData>("words").add("hello");

        ArrayBinary blobs(tx.alloc());
        blobs.create();
        blobs.add(BinaryData("blob", 4));
        tx.set_root("blobs", blobs.get_ref());

        CHECK_EQUAL(tx.root_count(), 3);
        tx.commit();
    }
    // Read it back on a fresh snapshot from the same store.
    {
        Tx tx = store->begin_read();
        Tree<int64_t> nums = tx.tree<int64_t>("nums");
        CHECK_EQUAL(nums.size(), 5000);
        CHECK_EQUAL(nums.get(4999), 4999 * 3);
        CHECK_EQUAL(nums.find_first(9), 3);
        CHECK_EQUAL(tx.tree<StringData>("words").get(0), "hello");

        auto ref = tx.get_root("blobs");
        CHECK(bool(ref));
        ArrayBinary blobs(tx.alloc());
        blobs.init_from_ref(*ref);
        CHECK_EQUAL(blobs.get(0), BinaryData("blob", 4));
    }
    // Uncommitted write rolls back; compact() is a safe no-op in memory.
    {
        Tx tx = store->begin_write();
        tx.tree<int64_t>("nums").add(-1);
        tx.rollback();
    }
    CHECK_EQUAL(store->begin_read().tree<int64_t>("nums").size(), 5000);
    CHECK(!store->compact()); // no file to rewrite
}

// In-memory data lives and dies with the StoreRef, and two in-memory stores
// never share state (each gets its own anonymous buffer).
TEST(CoreStore_InMemoryNoPersistAndIsolation)
{
    // Non-persistence: fill one store, drop it, open a new in-memory store —
    // it starts empty. There is no file to reopen.
    {
        StoreRef store = Store::open_in_memory();
        Tx tx = store->begin_write();
        tx.tree<int64_t>("x").add(42);
        tx.commit();
    }
    {
        StoreRef fresh = Store::open_in_memory();
        Tx tx = fresh->begin_read();
        CHECK_EQUAL(tx.root_count(), 0);
        CHECK(!tx.get_root("x"));
    }
    // Isolation: two live in-memory stores don't see each other's writes.
    StoreRef a = Store::open_in_memory();
    StoreRef b = Store::open_in_memory();
    {
        Tx tx = a->begin_write();
        tx.tree<int64_t>("only_in_a").add(1);
        tx.commit();
    }
    CHECK_EQUAL(a->begin_read().root_count(), 1);
    CHECK_EQUAL(b->begin_read().root_count(), 0);
}

// MVCC snapshot isolation holds in memory just as on a file: a pinned reader
// keeps seeing its version while a writer commits a newer one.
TEST(CoreStore_InMemoryMVCC)
{
    StoreRef store = Store::open_in_memory();
    {
        Tx tx = store->begin_write();
        Tree<int64_t> t = tx.tree<int64_t>("n");
        for (int64_t i = 0; i < 100; ++i)
            t.add(i);
        tx.commit();
    }
    Tx reader = store->begin_read(); // pins the 100-element version
    {
        Tx writer = store->begin_write();
        Tree<int64_t> t = writer.tree<int64_t>("n");
        for (int64_t i = 100; i < 200; ++i)
            t.add(i);
        writer.commit();
    }
    CHECK_EQUAL(reader.tree<int64_t>("n").size(), 100);
    CHECK_EQUAL(store->begin_read().tree<int64_t>("n").size(), 200);
}

#endif // TEST_CORE_STORE
