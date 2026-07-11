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
#include <llbt/impl/simulated_failure.hpp>

#include "test.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <set>
#include <thread>
#include <vector>

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

TEST(CoreStore_TreeLowerBound)
{
    TEST_PATH(path);
    StoreRef store = Store::open(path);

    // empty tree
    {
        Tx tx = store->begin_write();
        Tree<StringData> t = tx.tree<StringData>("map");
        CHECK_EQUAL(t.lower_bound("anything"), 0);
        tx.commit();
    }

    // build a sorted map from shuffled keys via insert(lower_bound(k), k) —
    // enough keys to force a multi-level tree (> LLBT_MAX_BPNODE_SIZE)
    const size_t N = 5000;
    std::vector<std::string> keys(N);
    std::mt19937_64 rng(4711);
    for (auto& s : keys) {
        s.resize(16);
        for (auto& c : s)
            c = char('a' + rng() % 26);
    }
    {
        Tx tx = store->begin_write();
        Tree<StringData> t = tx.tree<StringData>("map");
        for (auto& k : keys)
            t.insert(t.lower_bound(StringData(k)), StringData(k));
        CHECK_EQUAL(t.size(), N);
        tx.commit();
    }

    std::vector<std::string> sorted = keys;
    std::sort(sorted.begin(), sorted.end());

    Tx tx = store->begin_read();
    Tree<StringData> t = tx.tree<StringData>("map");
    // the tree must have ended up in sorted order
    for (size_t i = 0; i < N; ++i)
        CHECK_EQUAL(t.get(i), StringData(sorted[i]));
    // lower_bound agrees with std::lower_bound for present keys...
    for (size_t i = 0; i < N; i += 7) {
        size_t expected = std::lower_bound(sorted.begin(), sorted.end(), keys[i]) - sorted.begin();
        CHECK_EQUAL(t.lower_bound(StringData(keys[i])), expected);
    }
    // ...and for absent probes, including before-first and past-last
    std::vector<std::string> probes = {"", "a", "zzzzzzzzzzzzzzzzzz", "mmmmmmmm"};
    for (size_t i = 0; i < 100; ++i) {
        std::string p(1 + rng() % 20, ' ');
        for (auto& c : p)
            c = char('a' + rng() % 26);
        probes.push_back(p);
    }
    for (auto& p : probes) {
        size_t expected = std::lower_bound(sorted.begin(), sorted.end(), p) - sorted.begin();
        CHECK_EQUAL(t.lower_bound(StringData(p)), expected);
    }
    // duplicates: lower_bound must point at the first occurrence
    {
        Tx wx = store->begin_write();
        Tree<StringData> w = wx.tree<StringData>("map");
        StringData dup(sorted[N / 2]);
        w.insert(w.lower_bound(dup), dup);
        w.insert(w.lower_bound(dup), dup);
        size_t first = w.lower_bound(dup);
        CHECK_EQUAL(w.get(first), dup);
        CHECK_EQUAL(w.get(first + 1), dup);
        CHECK_EQUAL(w.get(first + 2), dup);
        CHECK(first == 0 || w.get(first - 1) < dup);

        // Force equal values across several leaf boundaries. The inner-node
        // search must enter the child before the first child beginning with
        // the probe, because that earlier child can end with duplicates.
        Tree<StringData> dups = wx.tree<StringData>("dups");
        for (size_t i = 0; i < 1500; ++i)
            dups.add("a");
        for (size_t i = 0; i < 2500; ++i)
            dups.add("m");
        for (size_t i = 0; i < 1500; ++i)
            dups.add("z");
        CHECK_EQUAL(dups.lower_bound("a"), 0);
        CHECK_EQUAL(dups.lower_bound("m"), 1500);
        CHECK_EQUAL(dups.lower_bound("z"), 4000);
        CHECK_EQUAL(dups.lower_bound("zz"), 5500);

        // lower_bound is part of the generic Tree<T> API, not string-only.
        Tree<int64_t> ints = wx.tree<int64_t>("ints");
        for (int64_t i = 0; i < 3000; ++i)
            ints.add(i * 2);
        CHECK_EQUAL(ints.lower_bound(-1), 0);
        CHECK_EQUAL(ints.lower_bound(0), 0);
        CHECK_EQUAL(ints.lower_bound(1999), 1000);
        CHECK_EQUAL(ints.lower_bound(6000), 3000);
        wx.rollback();
    }
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
    // Read the on-disk size through a fresh, immediately-closed handle. On
    // Windows compact() replaces the file and cannot proceed while any other
    // handle to it is open, so we must not hold one across the call (POSIX is
    // lenient about this, which is why it only bites on Windows).
    auto file_size = [&] {
        util::File f(store->path());
        return f.get_size();
    };
    auto steady_size = file_size();

    // 20 more full rewrites of the same live payload. If replaced trees were
    // not freed back to the free list, the file would grow ~linearly with
    // generations; with correct freeing it plateaus.
    for (int64_t g = 3; g <= 23; ++g)
        write_generation(g * 1000);
    auto size_after_many = file_size();
    CHECK_LESS_EQUAL(size_after_many, steady_size * 2);

    // And a compacted file with identical live data stays the same size
    // no matter how many generations preceded it.
    store->compact();
    auto compacted = file_size();
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

#if LLBT_ENABLE_ENCRYPTION
TEST(CoreStore_EncryptedAsyncCommitFlushesSharedWindows)
{
    TEST_PATH(path);
    Options options;
    options.encryption_key = crypt_key(true);
    options.no_sync = true;
    options.single_process = true;
    {
        StoreRef store = Store::open(path, options);
        Tx tx = store->begin_write();
        Tree<int64_t> values = tx.tree<int64_t>("values");
        for (int64_t i = 0; i < 5000; ++i)
            values.add(i * 7);
        tx.raw().commit_and_continue_as_read(false);
        tx.raw().prepare_for_close();
    }
    {
        StoreRef store = Store::open(path, options);
        Tx tx = store->begin_read();
        Tree<int64_t> values = tx.tree<int64_t>("values");
        CHECK_EQUAL(values.size(), 5000);
        CHECK_EQUAL(values.get(4999), 4999 * 7);
    }
}

TEST_IF(CoreStore_EncryptedFailedCommitDropsWindowCache, _impl::SimulatedFailure::is_enabled())
{
    TEST_PATH(path);
    Options options;
    options.encryption_key = crypt_key(true);
    options.no_sync = true;
    options.single_process = true;
    StoreRef a = Store::open(path, options);
    StoreRef b = Store::open(path, options);
    {
        Tx tx = a->begin_write();
        Tree<int64_t> values = tx.tree<int64_t>("values");
        for (int64_t i = 0; i < 5000; ++i)
            values.add(i);
        tx.commit();
    }
    {
        Tx tx = a->begin_write();
        Tree<int64_t> values = tx.tree<int64_t>("values");
        for (size_t i = 0; i < values.size(); ++i)
            values.set(i, 111111);
        _impl::SimulatedFailure::OneShotPrimeGuard fail(_impl::SimulatedFailure::group_writer__commit);
        CHECK_THROW_ANY(tx.commit());
        tx.rollback();
    }
    {
        Tx tx = b->begin_write();
        Tree<int64_t> values = tx.tree<int64_t>("values");
        for (size_t i = 0; i < values.size(); ++i)
            values.set(i, 222222);
        tx.commit();
    }

    // Close the failed writer after the successful one committed. A stale
    // encrypted window cache must not overwrite the later commit here.
    a.reset();
    b.reset();
    StoreRef reopened = Store::open(path, options);
    Tx tx = reopened->begin_read();
    Tree<int64_t> values = tx.tree<int64_t>("values");
    CHECK_EQUAL(values.size(), 5000);
    for (size_t i = 0; i < values.size(); ++i)
        CHECK_EQUAL(values.get(i), 222222);
}
#endif

// ---- group commit ----------------------------------------------------------

TEST(CoreStore_GroupCommitSingleThread)
{
    TEST_PATH(path);
    {
        StoreRef store = Store::open(path);
        uint64_t v1 = store->write([](Tx& tx) {
            tx.tree<int64_t>("n").add(7);
        });
        uint64_t v2 = store->write([](Tx& tx) {
            Tree<int64_t> t = tx.tree<int64_t>("n");
            t.add(t.get(0) * 2);
        });
        CHECK_EQUAL(v2, v1 + 1);

        Tx tx = store->begin_read();
        Tree<int64_t> t = tx.tree<int64_t>("n");
        CHECK_EQUAL(t.size(), 2);
        CHECK_EQUAL(t.get(0), 7);
        CHECK_EQUAL(t.get(1), 14);
    }
    // grouped writes are as durable as plain ones: fresh session sees them
    StoreRef store = Store::open(path);
    Tx tx = store->begin_read();
    CHECK_EQUAL(tx.tree<int64_t>("n").size(), 2);
}

TEST(CoreStore_GroupCommitManyThreads)
{
    TEST_PATH(path);
    Options options;
    options.no_sync = true; // batching logic is identical; keeps CI fast
    StoreRef store = Store::open(path, options);

    constexpr int num_threads = 8;
    constexpr int writes_per_thread = 50;
    std::atomic<int> failures{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < num_threads; ++t) {
        workers.emplace_back([&, t] {
            for (int i = 0; i < writes_per_thread; ++i) {
                int64_t value = int64_t(t) * 1000 + i;
                try {
                    store->write([&](Tx& tx) {
                        Tree<int64_t> tree = tx.tree<int64_t>("values");
                        tree.insert(tree.lower_bound(value), value);
                    });
                }
                catch (...) {
                    failures.fetch_add(1);
                }
            }
        });
    }
    for (auto& w : workers)
        w.join();
    CHECK_EQUAL(failures.load(), 0);

    // every submitted value landed exactly once (distinct values + exact size)
    Tx tx = store->begin_read();
    Tree<int64_t> tree = tx.tree<int64_t>("values");
    CHECK_EQUAL(tree.size(), num_threads * writes_per_thread);
    for (int t = 0; t < num_threads; ++t)
        for (int i = 0; i < writes_per_thread; ++i)
            CHECK_NOT_EQUAL(tree.find_first(int64_t(t) * 1000 + i), Tx::npos);
}

TEST(CoreStore_GroupCommitBatchesUnderContention)
{
    TEST_PATH(path);
    StoreRef store = Store::open(path);

    constexpr int num_threads = 8;
    std::vector<std::thread> workers;
    std::vector<uint64_t> versions(num_threads, 0);
    std::atomic<int> started{0};

    Tx blocker = store->begin_write(); // holds the engine write lock
    for (int t = 0; t < num_threads; ++t) {
        workers.emplace_back([&, t] {
            started.fetch_add(1);
            versions[size_t(t)] = store->write([t](Tx& tx) {
                tx.tree<int64_t>("batched").add(t);
            });
        });
    }
    while (started.load() < num_threads)
        std::this_thread::yield();
    // let every worker park in the group-commit queue behind the blocker
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    uint64_t blocked_version = blocker.commit();
    for (auto& w : workers)
        w.join();

    // Everything queued while the lock was held rides in the batch that forms
    // when the lock frees up: 8 writes in one commit (two, if a worker thread
    // was still warming up when the batch drained).
    std::set<uint64_t> distinct(versions.begin(), versions.end());
    CHECK_LESS_EQUAL(distinct.size(), 2);
    for (int t = 0; t < num_threads; ++t)
        CHECK_GREATER(versions[size_t(t)], blocked_version);

    Tx tx = store->begin_read();
    Tree<int64_t> tree = tx.tree<int64_t>("batched");
    CHECK_EQUAL(tree.size(), num_threads);
    for (int t = 0; t < num_threads; ++t)
        CHECK_NOT_EQUAL(tree.find_first(t), Tx::npos);
}

TEST(CoreStore_GroupCommitExceptionIsolation)
{
    TEST_PATH(path);
    StoreRef store = Store::open(path);

    struct Boom : std::exception {
    };

    constexpr int num_threads = 4;
    std::vector<std::thread> workers;
    std::array<std::exception_ptr, num_threads> errors;
    std::atomic<int> started{0};

    Tx blocker = store->begin_write(); // force the four writes into one batch
    for (int t = 0; t < num_threads; ++t) {
        workers.emplace_back([&, t] {
            started.fetch_add(1);
            try {
                store->write([t](Tx& tx) {
                    tx.tree<int64_t>("iso").add(100 * (t + 1));
                    if (t == 1)
                        throw Boom();
                });
            }
            catch (...) {
                errors[size_t(t)] = std::current_exception();
            }
        });
    }
    while (started.load() < num_threads)
        std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    blocker.commit();
    for (auto& w : workers)
        w.join();

    // the thrower fails with its own exception; everyone else commits
    CHECK(!errors[0]);
    CHECK(bool(errors[1]));
    CHECK(!errors[2]);
    CHECK(!errors[3]);
    bool got_boom = false;
    if (errors[1]) {
        try {
            std::rethrow_exception(errors[1]);
        }
        catch (const Boom&) {
            got_boom = true;
        }
        catch (...) {
        }
    }
    CHECK(got_boom);

    // survivors may have run twice (batch attempt + solo re-run after the
    // poisoned batch rolled back) but their effect lands exactly once, and
    // nothing of the thrower's work survives
    Tx tx = store->begin_read();
    Tree<int64_t> tree = tx.tree<int64_t>("iso");
    CHECK_EQUAL(tree.size(), 3);
    CHECK_NOT_EQUAL(tree.find_first(100), Tx::npos);
    CHECK_EQUAL(tree.find_first(200), Tx::npos);
    CHECK_NOT_EQUAL(tree.find_first(300), Tx::npos);
    CHECK_NOT_EQUAL(tree.find_first(400), Tx::npos);
}

TEST(CoreStore_GroupCommitClosureRules)
{
    TEST_PATH(path);
    StoreRef store = Store::open(path);
    StoreRef other = Store::open_in_memory();

    uint64_t version = store->write([&](Tx& tx) {
        tx.tree<int64_t>("rules").add(1);
        // the Tx belongs to the whole batch: no commit/rollback from a closure
        CHECK_THROW(tx.commit(), LogicError);
        CHECK_THROW(tx.rollback(), LogicError);
        // nested writes on the same store would self-deadlock: refused
        CHECK_THROW(store->write([](Tx&) {}), LogicError);
        CHECK_THROW(store->begin_write(), LogicError);
        // writing to a DIFFERENT store from inside a closure stays legal
        other->write([](Tx& o) {
            o.tree<int64_t>("side").add(9);
        });
    });
    CHECK_GREATER(version, 0);

    // the guard throws survived inside the closure without poisoning it
    CHECK_EQUAL(store->begin_read().tree<int64_t>("rules").size(), 1);
    CHECK_EQUAL(other->begin_read().tree<int64_t>("side").size(), 1);
}

TEST(CoreStore_GroupCommitInMemory)
{
    StoreRef store = Store::open_in_memory();

    constexpr int num_threads = 4;
    constexpr int writes_per_thread = 25;
    std::atomic<int> failures{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < num_threads; ++t) {
        workers.emplace_back([&, t] {
            for (int i = 0; i < writes_per_thread; ++i) {
                try {
                    store->write([&](Tx& tx) {
                        tx.tree<int64_t>("mem").add(int64_t(t) * 1000 + i);
                    });
                }
                catch (...) {
                    failures.fetch_add(1);
                }
            }
        });
    }
    for (auto& w : workers)
        w.join();
    CHECK_EQUAL(failures.load(), 0);
    CHECK_EQUAL(store->begin_read().tree<int64_t>("mem").size(), num_threads * writes_per_thread);
}

TEST(CoreStore_GroupCommitInteropWithPlainWriters)
{
    TEST_PATH(path);
    Options options;
    options.no_sync = true;
    StoreRef store = Store::open(path, options);

    constexpr int writes_per_thread = 100;
    std::atomic<int> failures{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < 2; ++t) {
        // plain writers take the engine lock directly...
        workers.emplace_back([&, t] {
            try {
                for (int i = 0; i < writes_per_thread; ++i) {
                    Tx tx = store->begin_write();
                    tx.tree<int64_t>("plain").add(int64_t(t) * 1000 + i);
                    tx.commit();
                }
            }
            catch (...) {
                failures.fetch_add(1);
            }
        });
        // ...while grouped writers batch behind whoever holds it
        workers.emplace_back([&, t] {
            try {
                for (int i = 0; i < writes_per_thread; ++i) {
                    store->write([&](Tx& tx) {
                        tx.tree<int64_t>("grouped").add(int64_t(t) * 1000 + i);
                    });
                }
            }
            catch (...) {
                failures.fetch_add(1);
            }
        });
    }
    for (auto& w : workers)
        w.join();
    CHECK_EQUAL(failures.load(), 0);

    Tx tx = store->begin_read();
    CHECK_EQUAL(tx.tree<int64_t>("plain").size(), 2 * writes_per_thread);
    CHECK_EQUAL(tx.tree<int64_t>("grouped").size(), 2 * writes_per_thread);
}

#endif // TEST_CORE_STORE
