// llbt microbenchmark.
//
// Methodology mirrors the suites used by comparable embedded stores — LMDB's
// microbench, bolt's `bench`, RocksDB's db_bench, and llbt's own ancestor
// Realm: a handful of core operations measured across the dimensions that
// actually move the numbers.
//
//   operations   bulk append, batched commit, random read, sequential scan,
//                random update, and a sorted string-key KV workload
//   dimensions   durability mode (sync / nosync / memory), batch size
//   metrics      throughput (ops/sec), average latency, and — where it
//                matters most, the commit path — p50/p95/p99 latency
//
// Rigor: fixed RNG seed, a warm-up pass before each timed section, a
// monotonic clock, and a volatile sink so the optimizer can't delete the
// reads. Numbers are single-process, single-machine, warm-cache — useful for
// relative comparison (mode vs mode, batch vs batch), not as absolute specs.
//
// Build Release or the numbers are meaningless:
//   cmake -B build -DLLBT_BUILD_BENCH=ON -DCMAKE_BUILD_TYPE=Release
//   cmake --build build --target llbt-bench
//   ./build/bench/llbt-bench [scale]        # scale defaults to 1
#include <llbt/api.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace llbt;
using namespace llbt::core;
using Clock = std::chrono::steady_clock;

static volatile int64_t g_sink = 0; // defeats dead-code elimination of reads

static double secs(Clock::duration d) { return std::chrono::duration<double>(d).count(); }

static std::string rate_str(double ops_per_sec)
{
    char b[32];
    if (ops_per_sec >= 1e6)      std::snprintf(b, sizeof b, "%.2f M/s", ops_per_sec / 1e6);
    else if (ops_per_sec >= 1e3) std::snprintf(b, sizeof b, "%.1f k/s", ops_per_sec / 1e3);
    else                         std::snprintf(b, sizeof b, "%.0f /s", ops_per_sec);
    return b;
}

static std::string time_str(double s)
{
    char b[32];
    if (s < 0)             std::snprintf(b, sizeof b, "-");
    else if (s >= 1e-3)    std::snprintf(b, sizeof b, "%.2f ms", s * 1e3);
    else if (s >= 1e-6)    std::snprintf(b, sizeof b, "%.2f us", s * 1e6);
    else                   std::snprintf(b, sizeof b, "%.0f ns", s * 1e9);
    return b;
}

// percentile of a sample (0..1); reorders v.
static double pct(std::vector<double>& v, double p)
{
    if (v.empty()) return -1;
    size_t i = size_t(p * (v.size() - 1));
    std::nth_element(v.begin(), v.begin() + i, v.end());
    return v[i];
}

static void header()
{
    std::printf("%-22s %-7s %11s %11s %10s %10s %10s %10s  %s\n", "benchmark", "mode", "ops", "ops/s",
                "avg", "p50", "p95", "p99", "notes");
    std::printf("--------------------------------------------------------------------------------------------------------------\n");
}

static void row(const std::string& name, const char* mode, uint64_t ops, double total_s, double p50 = -1,
                double p95 = -1, double p99 = -1, const std::string& notes = "")
{
    std::printf("%-22s %-7s %11llu %11s %10s %10s %10s %10s  %s\n", name.c_str(), mode,
                (unsigned long long)ops, rate_str(ops / total_s).c_str(), time_str(total_s / ops).c_str(),
                time_str(p50).c_str(), time_str(p95).c_str(), time_str(p99).c_str(), notes.c_str());
}

enum Mode { SYNC, NOSYNC, MEMORY };
static const char* mode_name(Mode m) { return m == SYNC ? "sync" : m == NOSYNC ? "nosync" : "memory"; }

static std::string g_dir;
static int g_seq = 0;

static StoreRef open_fresh(Mode m)
{
    if (m == MEMORY)
        return Store::open_in_memory();
    Options o;
    o.no_sync = (m == NOSYNC);
    std::string path = util::File::resolve("db" + std::to_string(g_seq++) + ".llbt", g_dir);
    return Store::open(path, o);
}

static std::string file_size_note(const StoreRef& s, uint64_t entries)
{
    if (s->is_in_memory())
        return "no file";
    auto bytes = util::File::get_size_static(s->path());
    char b[64];
    std::snprintf(b, sizeof b, "%.1f MB file, %.1f B/entry", bytes / 1e6, double(bytes) / entries);
    return b;
}

// ---- benchmarks -----------------------------------------------------------

// One big write Tx appending N int64 — the bulk-load / append-log path.
static void bench_bulk_load(Mode m, size_t N)
{
    StoreRef store = open_fresh(m);
    auto t0 = Clock::now();
    {
        Tx tx = store->begin_write();
        Tree<int64_t> t = tx.tree<int64_t>("k");
        for (size_t i = 0; i < N; ++i)
            t.add(int64_t(i));
        tx.commit();
    }
    double s = secs(Clock::now() - t0);
    row("bulk-load(append)", mode_name(m), N, s, -1, -1, -1, file_size_note(store, N));
}

// C commits of B appends each — isolates the commit (fsync) cost. This is the
// row that explains llbt's durable-write throughput.
static void bench_commit(Mode m, size_t C, size_t B)
{
    StoreRef store = open_fresh(m);
    std::vector<double> lat;
    lat.reserve(C);
    int64_t k = 0;
    // warm-up
    for (size_t w = 0; w < 3; ++w) {
        Tx tx = store->begin_write();
        Tree<int64_t> t = tx.tree<int64_t>("k");
        for (size_t b = 0; b < B; ++b)
            t.add(k++);
        tx.commit();
    }
    auto t0 = Clock::now();
    for (size_t c = 0; c < C; ++c) {
        auto c0 = Clock::now();
        Tx tx = store->begin_write();
        Tree<int64_t> t = tx.tree<int64_t>("k");
        for (size_t b = 0; b < B; ++b)
            t.add(k++);
        tx.commit();
        lat.push_back(secs(Clock::now() - c0));
    }
    double total = secs(Clock::now() - t0);
    std::string nm = "commit(batch=" + std::to_string(B) + ")";
    row(nm, mode_name(m), C, total, pct(lat, 0.50), pct(lat, 0.95), pct(lat, 0.99),
        std::to_string(C * B) + " rows");
}

static StoreRef make_populated(Mode m, size_t N)
{
    StoreRef store = open_fresh(m);
    Tx tx = store->begin_write();
    Tree<int64_t> t = tx.tree<int64_t>("k");
    for (size_t i = 0; i < N; ++i)
        t.add(int64_t(i));
    tx.commit();
    return store;
}

// M random positional reads over a populated tree (B+tree traversal per get).
static void bench_rand_read(Mode m, size_t N, size_t M)
{
    StoreRef store = make_populated(m, N);
    std::mt19937_64 rng(12345);
    std::uniform_int_distribution<uint32_t> d(0, uint32_t(N - 1));
    std::vector<uint32_t> idx(M);
    for (auto& x : idx)
        x = d(rng);

    Tx tx = store->begin_read();
    Tree<int64_t> t = tx.tree<int64_t>("k");
    int64_t sink = 0;
    for (size_t i = 0; i < M / 10; ++i) // warm-up
        sink += t.get(idx[i]);
    auto t0 = Clock::now();
    for (size_t i = 0; i < M; ++i)
        sink += t.get(idx[i]);
    double s = secs(Clock::now() - t0);
    g_sink += sink;
    row("rand-read(get)", mode_name(m), M, s);
}

// Full ordered walk with a Cursor.
static void bench_scan(Mode m, size_t N)
{
    StoreRef store = make_populated(m, N);
    Tx tx = store->begin_read();
    Tree<int64_t> t = tx.tree<int64_t>("k");
    int64_t sink = 0;
    auto t0 = Clock::now();
    for (auto c = t.cursor(); c.valid(); c.next())
        sink += c.value();
    double s = secs(Clock::now() - t0);
    g_sink += sink;
    row("seq-scan(cursor)", mode_name(m), N, s);
}

// M random in-place updates, committed in batches of B (amortized commit).
static void bench_rand_update(Mode m, size_t N, size_t M, size_t B)
{
    StoreRef store = make_populated(m, N);
    std::mt19937_64 rng(777);
    std::uniform_int_distribution<uint32_t> d(0, uint32_t(N - 1));
    std::vector<uint32_t> idx(M);
    for (auto& x : idx)
        x = d(rng);

    auto t0 = Clock::now();
    size_t done = 0;
    while (done < M) {
        Tx tx = store->begin_write();
        Tree<int64_t> t = tx.tree<int64_t>("k");
        size_t end = std::min(done + B, M);
        for (; done < end; ++done)
            t.set(idx[done], int64_t(done));
        tx.commit();
    }
    double s = secs(Clock::now() - t0);
    row("rand-update(set)", mode_name(m), M, s, -1, -1, -1, "batch=" + std::to_string(B));
}

// Realistic KV: K sorted 16-byte string keys -> vsize-byte values, built with
// binary-search inserts, then `lookups` random point lookups (lower_bound).
static void bench_kv_string(Mode m, size_t K, size_t lookups, size_t vsize)
{
    StoreRef store = open_fresh(m);
    std::mt19937_64 rng(999);
    std::vector<std::string> keys(K);
    for (auto& s : keys) {
        s.resize(16);
        for (auto& c : s)
            c = char('a' + rng() % 26);
    }
    std::string val(vsize, 'x');

    auto lower_bound = [](Tree<StringData>& t, StringData k) {
        size_t lo = 0, hi = t.size();
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (t.get(mid) < k)
                lo = mid + 1;
            else
                hi = mid;
        }
        return lo;
    };

    auto t0 = Clock::now();
    {
        Tx tx = store->begin_write();
        Tree<StringData> tk = tx.tree<StringData>("keys");
        Tree<StringData> tv = tx.tree<StringData>("vals");
        for (size_t i = 0; i < K; ++i) {
            size_t p = lower_bound(tk, StringData(keys[i]));
            tk.insert(p, StringData(keys[i]));
            tv.insert(p, StringData(val));
        }
        tx.commit();
    }
    double build = secs(Clock::now() - t0);
    std::string nm = "kv-string-build";
    row(nm, mode_name(m), K, build, -1, -1, -1, file_size_note(store, K));

    // random existing-key lookups
    std::uniform_int_distribution<size_t> pick(0, K - 1);
    std::vector<size_t> which(lookups);
    for (auto& x : which)
        x = pick(rng);
    Tx tx = store->begin_read();
    Tree<StringData> tk = tx.tree<StringData>("keys");
    Tree<StringData> tv = tx.tree<StringData>("vals");
    int64_t sink = 0;
    for (size_t i = 0; i < lookups / 10; ++i) { // warm-up
        size_t p = lower_bound(tk, StringData(keys[which[i]]));
        sink += tv.get(p).size();
    }
    auto t1 = Clock::now();
    for (size_t i = 0; i < lookups; ++i) {
        size_t p = lower_bound(tk, StringData(keys[which[i]]));
        sink += tv.get(p).size();
    }
    double look = secs(Clock::now() - t1);
    g_sink += sink;
    row("kv-string-lookup", mode_name(m), lookups, look, -1, -1, -1,
        "16B key, " + std::to_string(vsize) + "B val");
}

int main(int argc, char** argv)
{
    double scale = (argc > 1) ? std::atof(argv[1]) : 1.0;
    if (scale <= 0)
        scale = 1.0;
    auto S = [&](size_t base) { return size_t(base * scale); };

    g_dir = util::make_temp_dir();

    std::printf("llbt microbenchmark\n");
    std::printf("cores(hw)=%u  scale=%.2f  (Release build assumed; warm-cache, single process)\n\n",
                std::thread::hardware_concurrency(), scale);

    const size_t N = S(1'000'000);   // dataset for load/read/scan/update
    const size_t READS = S(1'000'000);
    const size_t COMMITS = S(1'000);
    const size_t UPDATES = S(500'000);
    const size_t KVK = S(200'000);
    const size_t KVLOOK = S(500'000);

    header();
    for (Mode m : {SYNC, NOSYNC, MEMORY})
        bench_bulk_load(m, N);
    std::printf("\n");
    for (Mode m : {SYNC, NOSYNC, MEMORY})
        bench_commit(m, COMMITS, 1);
    for (Mode m : {SYNC, NOSYNC, MEMORY})
        bench_commit(m, COMMITS, 100);
    std::printf("\n");
    for (Mode m : {MEMORY, SYNC})
        bench_rand_read(m, N, READS);
    for (Mode m : {MEMORY, SYNC})
        bench_scan(m, N);
    std::printf("\n");
    for (Mode m : {SYNC, NOSYNC, MEMORY})
        bench_rand_update(m, N, UPDATES, 1000);
    std::printf("\n");
    for (Mode m : {SYNC, MEMORY})
        bench_kv_string(m, KVK, KVLOOK, 100);

    std::printf("\n(sink=%lld)\n", (long long)g_sink);
    return 0;
}
