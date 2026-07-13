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
** Copyright (c) 2026 Mohammad Julfikar
*/
#include "testsettings.hpp"
#ifdef TEST_CORE_STORE

#include <llbt/array_packed_blobs.hpp>
#include <llbt/core.hpp>

#include "test.hpp"

#include <string>
#include <vector>

using namespace llbt;
using namespace llbt::core;
using namespace llbt::test_util;

TEST(ArrayPackedBlobs_BlockBoundariesNullsAndDeltas)
{
    Allocator& alloc = Allocator::get_default();
    std::vector<std::string> storage(700);
    std::vector<BinaryData> values;
    values.reserve(storage.size());
    for (size_t i = 0; i < storage.size(); ++i) {
        if (i % 97 == 0) {
            values.emplace_back();
        }
        else {
            storage[i].assign(i % 11 == 0 ? 0 : 1024 + (i % 17), char('a' + i % 26));
            values.emplace_back(storage[i]);
        }
    }

    ArrayPackedBlobs blobs(alloc);
    blobs.create(values.data(), values.size());
    CHECK_EQUAL(blobs.size(), values.size());
    for (size_t i = 0; i < values.size(); ++i)
        CHECK_EQUAL(blobs.get(i), values[i]);

    const size_t positions[] = {0, 249, 250, 511, 699};
    std::vector<std::string> replacement_storage = {
        std::string(), std::string(2048, 'x'), std::string(70, 'y'), std::string(1024, 'z'), std::string(1, 'q')};
    std::vector<BinaryData> replacements = {
        BinaryData("", 0), BinaryData(replacement_storage[1]), BinaryData(replacement_storage[2]),
        BinaryData(replacement_storage[3]), BinaryData(replacement_storage[4])};
    blobs.set_many(positions, replacements.data(), 5, 0);
    for (size_t i = 0; i < 5; ++i)
        CHECK_EQUAL(blobs.get(positions[i]), replacements[i]);

    blobs.erase(250);
    CHECK_EQUAL(blobs.size(), size_t(699));
    CHECK_EQUAL(blobs.get(249), replacements[1]);
    CHECK_EQUAL(blobs.get(250), values[251]);

    std::vector<std::string> appended_storage(300, std::string(1032, 'n'));
    std::vector<BinaryData> appended;
    for (const auto& value : appended_storage)
        appended.emplace_back(value);
    blobs.append(appended.data(), appended.size());
    CHECK_EQUAL(blobs.size(), size_t(999));
    CHECK_EQUAL(blobs.get(998), appended.back());
    blobs.verify();
    blobs.destroy();

    std::string large_storage(2 * 1024 * 1024, 'L');
    BinaryData large(large_storage);
    ArrayPackedBlobs large_blobs(alloc);
    large_blobs.create(&large, 1);
    CHECK_EQUAL(large_blobs.get(0), large);
    large_blobs.destroy();
}

TEST(CoreStore_PackedBinarySnapshotsCompactAndReopen)
{
    TEST_PATH(path);
    Options options;
    options.single_process = true;
    options.durability = Durability::Unsafe;
    StoreRef store = Store::open(path, options);

    std::vector<std::string> storage(2500);
    std::vector<BinaryData> values;
    values.reserve(storage.size());
    for (size_t i = 0; i < storage.size(); ++i) {
        storage[i] = std::to_string(i) + std::string(1024, 'a');
        values.emplace_back(storage[i]);
    }
    store->write([&](Tx& tx) {
        tx.tree<BinaryData>("packed").add_range(values.data(), values.size());
    });

    std::string replacement(1032, 'r');
    const size_t positions[] = {0, 999, 1000, 2499};
    BinaryData replacements[] = {BinaryData(replacement), BinaryData(replacement), BinaryData(replacement),
                                 BinaryData(replacement)};
    {
        Tx snapshot = store->begin_read();
        store->write([&](Tx& tx) {
            tx.tree<BinaryData>("packed").set_many(positions, replacements, 4);
        });

        CHECK_EQUAL(snapshot.tree<BinaryData>("packed").get(0), values[0]);
        {
            Tx current = store->begin_read();
            auto current_tree = current.tree<BinaryData>("packed");
            for (size_t position : positions)
                CHECK_EQUAL(current_tree.get(position), BinaryData(replacement));
        }
    }

    CHECK(store->compact());
    store.reset();
    store = Store::open(path, options);
    Tx reopened = store->begin_read();
    auto tree = reopened.tree<BinaryData>("packed");
    CHECK_EQUAL(tree.size(), size_t(2500));
    for (size_t position : positions)
        CHECK_EQUAL(tree.get(position), BinaryData(replacement));
}

TEST(CoreStore_PackedInsertCommitBudget)
{
    TEST_PATH(path);
    Options options;
    options.single_process = true;
    options.durability = Durability::Unsafe;
    StoreRef store = Store::open(path, options);
    store->reserve(8 * 1024 * 1024);

    std::string value(1032, 'p');
    std::vector<BinaryData> values(5000, BinaryData(value));
    store->write([&](Tx& tx) {
        tx.tree<BinaryData>("packed").add_range(values.data(), values.size());
    });
    CommitMetrics metrics = store->last_commit_metrics();
    CHECK_LESS_EQUAL(metrics.arrays_written, uint64_t(64));
    CHECK_LESS_EQUAL(metrics.cow_bytes_written, uint64_t(double(values.size() * value.size()) * 1.15));
}

#endif // TEST_CORE_STORE
