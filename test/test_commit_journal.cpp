/*
** llbt — Low Level Binary Tree
** Copyright (c) 2026 Mohammad Julfikar
** Dedicated to the public domain. See LICENSE and NOTICE.
*/
#include "testsettings.hpp"
#ifdef TEST_CORE_STORE

#include <llbt/core.hpp>
#include <llbt/alloc_slab.hpp>
#include <llbt/array.hpp>
#include <llbt/group.hpp>
#include <llbt/util/file.hpp>

#include "test.hpp"

#include <cstring>

using namespace llbt;
using namespace llbt::core;
using namespace llbt::test_util;

namespace {

uint64_t read_u64(const char* bytes)
{
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i)
        value |= uint64_t(uint8_t(bytes[i])) << (i * 8);
    return value;
}

} // namespace

TEST(CommitJournal_InteriorPageCorruptionIsRejected)
{
    TEST_PATH(path);
    Options options;
    options.durability = Durability::Unsafe;
    {
        StoreRef store = Store::open(path, options);
        store->write([](Tx& tx) {
            tx.tree<int64_t>("values").add(1);
        });
        store->write([](Tx& tx) {
            tx.tree<int64_t>("values").add(2);
        });
    }

    ref_type changed_array = 0;
    {
        SlabAlloc alloc;
        SlabAlloc::Config config;
        config.read_only = true;
        config.no_create = true;
        ref_type checkpoint_top = alloc.attach_file(path, config);
        ref_type registry_ref = _impl::GroupFriend::get_history_ref(alloc, checkpoint_top);
        Array registry(alloc);
        registry.init_from_ref(registry_ref);
        Array descriptor(alloc);
        descriptor.init_from_ref(registry.get_as_ref(3));
        const char* first_record = alloc.translate(descriptor.get_as_ref(0)) + NodeHeader::header_size;
        changed_array = ref_type(read_u64(first_record + 64));
        alloc.detach();
    }
    CHECK_NOT_EQUAL(changed_array, ref_type(0));

    util::File file(path, util::File::mode_Update);
    char byte = 0;
    CHECK_EQUAL(file.read(changed_array + 4, &byte, 1), size_t(1));
    byte ^= char(0x40);
    file.write(changed_array + 4, &byte, 1);
    file.sync();
    file.close();

    CHECK_THROW(Store::open(path, options), InvalidDatabase);
}

TEST(CommitJournal_CheckpointAfter4096Commits)
{
    TEST_PATH(path);
    Options options;
    options.durability = Durability::Unsafe;
    {
        StoreRef store = Store::open(path, options);
        for (int64_t i = 0; i < 4100; ++i) {
            store->write([&](Tx& tx) {
                tx.tree<int64_t>("versions").add(i);
            });
        }
    }
    StoreRef reopened = Store::open(path, options);
    Tree<int64_t> values = reopened->begin_read().tree<int64_t>("versions");
    CHECK_EQUAL(values.size(), size_t(4100));
    CHECK_EQUAL(values.get(4099), int64_t(4099));
}

TEST(CommitJournal_CheckpointBoundsRetainedCowBytes)
{
    TEST_PATH(path);
    Options options;
    options.durability = Durability::Unsafe;
    std::string payload(5 * 1024 * 1024, 'p');
    {
        StoreRef store = Store::open(path, options);
        for (int64_t i = 0; i < 14; ++i) {
            store->write([&](Tx& tx) {
                tx.tree<BinaryData>("large").add(BinaryData(payload));
            });
        }
    }
    StoreRef reopened = Store::open(path, options);
    CHECK_EQUAL(reopened->begin_read().tree<BinaryData>("large").size(), size_t(14));
}

#endif
