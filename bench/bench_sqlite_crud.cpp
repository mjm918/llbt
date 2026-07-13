/*
** llbt — Low Level Binary Tree
** Copyright (c) 2026 Mohammad Julfikar
**
** Dedicated to the public domain under the same terms as the llbt-authored
** benchmark files. See LICENSE and NOTICE.
*/
#include <llbt/api.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <random>
#include <string>
#include <vector>

using namespace llbt;
using namespace llbt::core;
using Clock = std::chrono::steady_clock;

namespace {

struct Phase {
    double seconds = 0;
    double p95_ms = 0;
    double end_to_end_seconds = 0;
    uint64_t arrays_max = 0;
    double cow_ratio_max = 0;
    double tree_write_ms_median = 0;
};

struct Run {
    Phase insert;
    Phase update;
    Phase erase;
    uint64_t file_after_update = 0;
    uint64_t file_after_compact = 0;
};

double seconds_since(Clock::time_point start)
{
    return std::chrono::duration<double>(Clock::now() - start).count();
}

double percentile(std::vector<double> values, double p)
{
    if (values.empty())
        return 0;
    size_t index = size_t(p * double(values.size() - 1));
    std::nth_element(values.begin(), values.begin() + index, values.end());
    return values[index];
}

std::string key_prefix(uint64_t value)
{
    std::string out(8, '\0');
    for (int i = 7; i >= 0; --i) {
        out[size_t(i)] = char(value & 0xff);
        value >>= 8;
    }
    return out;
}

std::vector<uint64_t> shuffled_keys(size_t rows)
{
    std::vector<uint64_t> keys(rows);
    std::iota(keys.begin(), keys.end(), uint64_t(0));
    std::mt19937_64 rng(0x4c4c4254);
    std::shuffle(keys.begin(), keys.end(), rng);
    return keys;
}

Run run_llbt(const std::string& path, size_t rows, size_t batch,
             const std::vector<uint64_t>& order, bool insert_only)
{
    util::File::try_remove(path);
    util::File::try_remove(path + ".lock");
    Options options;
    options.single_process = true;
    options.durability = Durability::Strict;
    StoreRef store = Store::open(path, options);
    store->reserve(rows * size_t(1200));

    Run result;
    std::vector<double> latency;
    {
        Tx tx = store->begin_write();
        (void)tx.tree<BinaryData>("records");
        tx.commit();
    }
    auto phase_start = Clock::now();
    double engine_seconds = 0;
    uint64_t arrays_max = 0;
    double cow_ratio_max = 0;
    std::vector<double> tree_write_latency;
    for (size_t offset = 0; offset < rows; offset += batch) {
        size_t count = std::min(batch, rows - offset);
        std::vector<std::string> storage(count);
        std::vector<BinaryData> values;
        values.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            storage[i] = key_prefix(offset + i);
            storage[i].append(1024, 'i');
            values.emplace_back(storage[i]);
        }
        auto engine_start = Clock::now();
        Tx tx = store->begin_write();
        tx.tree<BinaryData>("records").add_range(values.data(), values.size());
        tx.commit();
        double engine_elapsed = seconds_since(engine_start);
        engine_seconds += engine_elapsed;
        latency.push_back(engine_elapsed * 1000.0);
        CommitMetrics metrics = store->last_commit_metrics();
        arrays_max = std::max(arrays_max, metrics.arrays_written);
        cow_ratio_max = std::max(cow_ratio_max, double(metrics.cow_bytes_written) / double(count * 1032));
        tree_write_latency.push_back(double(metrics.tree_write_us) / 1000.0);
    }
    result.insert = {engine_seconds, percentile(latency, .95), seconds_since(phase_start), arrays_max,
                     cow_ratio_max, percentile(tree_write_latency, .5)};
    if (insert_only) {
        store.reset();
        util::File::try_remove(path);
        util::File::try_remove(path + ".lock");
        return result;
    }

    latency.clear();
    phase_start = Clock::now();
    for (size_t offset = 0; offset < rows; offset += batch) {
        auto batch_start = Clock::now();
        size_t count = std::min(batch, rows - offset);
        std::vector<std::pair<size_t, std::string>> pending;
        pending.reserve(count);
        Tx tx = store->begin_write();
        Tree<BinaryData> tree = tx.tree<BinaryData>("records");
        for (size_t i = 0; i < count; ++i) {
            uint64_t key = order[offset + i];
            std::string prefix = key_prefix(key);
            size_t position = tree.lower_bound(BinaryData(prefix));
            if (position == tree.size())
                std::abort();
            prefix.append(1024, 'u');
            pending.emplace_back(position, std::move(prefix));
        }
        std::sort(pending.begin(), pending.end());
        std::vector<size_t> positions(count);
        std::vector<BinaryData> values;
        values.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            positions[i] = pending[i].first;
            values.emplace_back(pending[i].second);
        }
        tree.set_many(positions.data(), values.data(), count);
        tx.commit();
        latency.push_back(seconds_since(batch_start) * 1000.0);
    }
    result.update = {seconds_since(phase_start), percentile(latency, .95)};

    {
        util::File file(path);
        result.file_after_update = file.get_size();
    }
    store->compact();
    {
        util::File file(path);
        result.file_after_compact = file.get_size();
    }

    latency.clear();
    phase_start = Clock::now();
    for (size_t offset = 0; offset < rows; offset += batch) {
        auto batch_start = Clock::now();
        size_t count = std::min(batch, rows - offset);
        Tx tx = store->begin_write();
        Tree<BinaryData> tree = tx.tree<BinaryData>("records");
        std::vector<size_t> positions;
        positions.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            std::string prefix = key_prefix(order[offset + i]);
            size_t position = tree.lower_bound(BinaryData(prefix));
            if (position == tree.size())
                std::abort();
            positions.push_back(position);
        }
        std::sort(positions.begin(), positions.end());
        tree.erase_many(positions.data(), positions.size());
        tx.commit();
        latency.push_back(seconds_since(batch_start) * 1000.0);
    }
    result.erase = {seconds_since(phase_start), percentile(latency, .95)};
    util::File::try_remove(path);
    util::File::try_remove(path + ".lock");
    return result;
}

void sql_check(sqlite3* db, int rc, const char* action)
{
    if (rc == SQLITE_OK || rc == SQLITE_DONE)
        return;
    std::fprintf(stderr, "SQLite %s failed: %s\n", action, sqlite3_errmsg(db));
    std::abort();
}

void sql_exec(sqlite3* db, const char* sql)
{
    char* error = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    if (rc != SQLITE_OK) {
        std::fprintf(stderr, "SQLite exec failed: %s\n", error ? error : "unknown");
        sqlite3_free(error);
        std::abort();
    }
}

Run run_sqlite(const std::string& path, size_t rows, size_t batch,
               const std::vector<uint64_t>& order, bool insert_only)
{
    util::File::try_remove(path);
    util::File::try_remove(path + "-wal");
    util::File::try_remove(path + "-shm");
    sqlite3* db = nullptr;
    sql_check(db, sqlite3_open(path.c_str(), &db), "open");
    sql_exec(db, "PRAGMA journal_mode=WAL");
    sql_exec(db, "PRAGMA synchronous=FULL");
    // LLBT Strict uses F_FULLFSYNC on Apple. SQLite only selects that stronger
    // primitive when fullfsync is enabled explicitly.
    sql_exec(db, "PRAGMA fullfsync=ON");
    sql_exec(db, "PRAGMA checkpoint_fullfsync=ON");
    sql_exec(db, "PRAGMA temp_store=MEMORY");
    sql_exec(db, "PRAGMA cache_size=-262144");
    sql_exec(db, "CREATE TABLE records(id INTEGER PRIMARY KEY, payload BLOB NOT NULL)");

    sqlite3_stmt* insert = nullptr;
    sql_check(db, sqlite3_prepare_v2(db, "INSERT INTO records VALUES(?,?)", -1, &insert, nullptr), "prepare insert");
    std::string payload(1024, 'i');
    Run result;
    std::vector<double> latency;
    auto phase_start = Clock::now();
    double engine_seconds = 0;
    for (size_t offset = 0; offset < rows; offset += batch) {
        size_t count = std::min(batch, rows - offset);
        std::vector<std::string> storage(count);
        for (size_t i = 0; i < count; ++i) {
            storage[i] = key_prefix(offset + i);
            storage[i].append(1024, 'i');
        }
        auto engine_start = Clock::now();
        sql_exec(db, "BEGIN");
        for (size_t i = 0; i < count; ++i) {
            sqlite3_bind_int64(insert, 1, sqlite3_int64(offset + i));
            sqlite3_bind_blob(insert, 2, storage[i].data() + 8, 1024, SQLITE_STATIC);
            sql_check(db, sqlite3_step(insert), "insert");
            sqlite3_reset(insert);
            sqlite3_clear_bindings(insert);
        }
        sql_exec(db, "COMMIT");
        double engine_elapsed = seconds_since(engine_start);
        engine_seconds += engine_elapsed;
        latency.push_back(engine_elapsed * 1000.0);
    }
    result.insert = {engine_seconds, percentile(latency, .95), seconds_since(phase_start)};
    sqlite3_finalize(insert);
    if (insert_only) {
        sqlite3_close(db);
        util::File::try_remove(path);
        util::File::try_remove(path + "-wal");
        util::File::try_remove(path + "-shm");
        return result;
    }

    sqlite3_stmt* update = nullptr;
    sql_check(db, sqlite3_prepare_v2(db, "UPDATE records SET payload=? WHERE id=?", -1, &update, nullptr), "prepare update");
    payload.assign(1024, 'u');
    latency.clear();
    phase_start = Clock::now();
    for (size_t offset = 0; offset < rows; offset += batch) {
        auto batch_start = Clock::now();
        sql_exec(db, "BEGIN");
        size_t count = std::min(batch, rows - offset);
        for (size_t i = 0; i < count; ++i) {
            sqlite3_bind_blob(update, 1, payload.data(), int(payload.size()), SQLITE_STATIC);
            sqlite3_bind_int64(update, 2, sqlite3_int64(order[offset + i]));
            sql_check(db, sqlite3_step(update), "update");
            sqlite3_reset(update);
            sqlite3_clear_bindings(update);
        }
        sql_exec(db, "COMMIT");
        latency.push_back(seconds_since(batch_start) * 1000.0);
    }
    result.update = {seconds_since(phase_start), percentile(latency, .95)};
    sqlite3_finalize(update);

    sqlite3_stmt* erase = nullptr;
    sql_check(db, sqlite3_prepare_v2(db, "DELETE FROM records WHERE id=?", -1, &erase, nullptr), "prepare delete");
    latency.clear();
    phase_start = Clock::now();
    for (size_t offset = 0; offset < rows; offset += batch) {
        auto batch_start = Clock::now();
        sql_exec(db, "BEGIN");
        size_t count = std::min(batch, rows - offset);
        for (size_t i = 0; i < count; ++i) {
            sqlite3_bind_int64(erase, 1, sqlite3_int64(order[offset + i]));
            sql_check(db, sqlite3_step(erase), "delete");
            sqlite3_reset(erase);
            sqlite3_clear_bindings(erase);
        }
        sql_exec(db, "COMMIT");
        latency.push_back(seconds_since(batch_start) * 1000.0);
    }
    result.erase = {seconds_since(phase_start), percentile(latency, .95)};
    sqlite3_finalize(erase);
    sqlite3_close(db);
    util::File::try_remove(path);
    util::File::try_remove(path + "-wal");
    util::File::try_remove(path + "-shm");
    return result;
}

Run median(std::vector<Run> runs)
{
    auto phase_median = [&](auto member) {
        std::sort(runs.begin(), runs.end(), [&](const Run& a, const Run& b) {
            return (a.*member).seconds < (b.*member).seconds;
        });
        return (runs[runs.size() / 2].*member);
    };
    Run result{phase_median(&Run::insert), phase_median(&Run::update), phase_median(&Run::erase)};
    auto size_median = [&](auto member) {
        std::sort(runs.begin(), runs.end(), [&](const Run& a, const Run& b) {
            return a.*member < b.*member;
        });
        return runs[runs.size() / 2].*member;
    };
    result.file_after_update = size_median(&Run::file_after_update);
    result.file_after_compact = size_median(&Run::file_after_compact);
    return result;
}

void print_phase(const char* name, const Phase& llbt, const Phase& sqlite, size_t rows)
{
    double lr = double(rows) / llbt.seconds;
    double sr = double(rows) / sqlite.seconds;
    std::printf("%-8s llbt %10.0f/s p95 %7.2f ms | sqlite %10.0f/s p95 %7.2f ms | %.2fx\n",
                name, lr, llbt.p95_ms, sr, sqlite.p95_ms, lr / sr);
}

} // namespace

int main(int argc, char** argv)
{
    size_t rows = argc > 1 ? std::stoull(argv[1]) : 100000;
    size_t batch = argc > 2 ? std::stoull(argv[2]) : 5000;
    size_t samples = argc > 3 ? std::stoull(argv[3]) : 5;
    bool insert_only = argc > 4 && std::string(argv[4]) == "insert-only";
    std::string dir = util::make_temp_dir();
    std::vector<uint64_t> order = shuffled_keys(rows);
    std::vector<Run> llbt_runs;
    std::vector<Run> sqlite_runs;
    for (size_t sample = 0; sample < samples; ++sample) {
        if (sample % 2 == 0) {
            llbt_runs.push_back(run_llbt(util::File::resolve("crud.llbt", dir), rows, batch, order, insert_only));
            sqlite_runs.push_back(run_sqlite(util::File::resolve("crud.sqlite", dir), rows, batch, order, insert_only));
        }
        else {
            sqlite_runs.push_back(run_sqlite(util::File::resolve("crud.sqlite", dir), rows, batch, order, insert_only));
            llbt_runs.push_back(run_llbt(util::File::resolve("crud.llbt", dir), rows, batch, order, insert_only));
        }
    }
    Run llbt = median(std::move(llbt_runs));
    Run sqlite = median(std::move(sqlite_runs));
    std::printf("LLBT vs SQLite strict CRUD: %zu rows, 1 KiB values, batch %zu, median of %zu\n",
                rows, batch, samples);
    print_phase("insert", llbt.insert, sqlite.insert, rows);
    std::printf("insert-e2e llbt %10.0f/s | sqlite %10.0f/s | %.2fx\n",
                double(rows) / llbt.insert.end_to_end_seconds,
                double(rows) / sqlite.insert.end_to_end_seconds,
                sqlite.insert.end_to_end_seconds / llbt.insert.end_to_end_seconds);
    std::printf("insert-prep llbt %.2f ms | sqlite %.2f ms | arrays max %llu | COW %.2fx | tree median %.2f ms\n",
                (llbt.insert.end_to_end_seconds - llbt.insert.seconds) * 1000.0,
                (sqlite.insert.end_to_end_seconds - sqlite.insert.seconds) * 1000.0,
                static_cast<unsigned long long>(llbt.insert.arrays_max), llbt.insert.cow_ratio_max,
                llbt.insert.tree_write_ms_median);
    if (!insert_only) {
        print_phase("update", llbt.update, sqlite.update, rows);
        print_phase("delete", llbt.erase, sqlite.erase, rows);
    }
    double live_bytes = double(rows) * 1032.0;
    if (!insert_only) {
        std::printf("llbt file after update %.1f MiB (%.2fx live), after compact %.1f MiB (%.2fx live)\n",
                    double(llbt.file_after_update) / (1024.0 * 1024.0),
                    double(llbt.file_after_update) / live_bytes,
                    double(llbt.file_after_compact) / (1024.0 * 1024.0),
                    double(llbt.file_after_compact) / live_bytes);
    }
    util::try_remove_dir_recursive(dir);
}
