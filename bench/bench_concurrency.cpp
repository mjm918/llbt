// llbt reader-scaling benchmark.
//
// llbt::core is single-writer / many-reader with MVCC snapshots: a reader
// opens an immutable snapshot and takes NO lock on the data. So read
// throughput should scale with cores, and — the point of this benchmark — it
// should keep scaling even while a writer commits as fast as it can, because
// readers never block the writer and the writer never blocks readers.
//
// Two scenarios per thread count:
//   * readers alone            — aggregate random reads/sec vs thread count
//   * readers + 1 live writer  — same, with a writer committing continuously
//
// The "vs alone" column is the money number: if it stays near 100%, the
// concurrent writer is not slowing readers down (that is MVCC working).
//
// Build: cmake -B build -DLLBT_BUILD_BENCH=ON -DCMAKE_BUILD_TYPE=Release
//        ./build/bench/llbt-bench-concurrency [scale]
#include <llbt/api.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace llbt;
using namespace llbt::core;
using Clock = std::chrono::steady_clock;
static double secs(Clock::duration d) { return std::chrono::duration<double>(d).count(); }
static std::atomic<uint64_t> g_sink{0};
static constexpr size_t writer_slots = 4096;

// simple start/stop barrier shared by all worker threads
static std::atomic<bool> g_go{false};
static std::atomic<bool> g_stop{false};
static std::atomic<int> g_ready{0};

static std::string rate(double v)
{
    char b[32];
    if (v >= 1e6)      std::snprintf(b, sizeof b, "%.2f M/s", v / 1e6);
    else if (v >= 1e3) std::snprintf(b, sizeof b, "%.1f k/s", v / 1e3);
    else               std::snprintf(b, sizeof b, "%.0f /s", v);
    return b;
}

// A reader: own snapshot, own accessor, random get() until told to stop.
static uint64_t reader_loop(StoreRef store, size_t N, int tid)
{
    uint64_t s = 0x9e3779b97f4a7c15ull ^ (uint64_t(tid + 1) * 0x100000001b3ull);
    auto next = [&]() -> uint64_t { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; };

    Tx tx = store->begin_read();
    Tree<int64_t> t = tx.tree<int64_t>("k");
    uint64_t sink = 0;
    uint64_t ops = 0;
    g_ready.fetch_add(1, std::memory_order_release);
    while (!g_go.load(std::memory_order_acquire))
        std::this_thread::yield();
    while (!g_stop.load(std::memory_order_relaxed)) {
        for (int b = 0; b < 512; ++b) // amortize the stop check
            sink += t.get(size_t(next() % N));
        ops += 512;
    }
    g_sink.fetch_add(sink, std::memory_order_relaxed);
    return ops;
}

// The single writer: commit small batches to its own tree as fast as it can.
static uint64_t writer_loop(StoreRef store)
{
    uint64_t commits = 0;
    int64_t x = 0;
    size_t pos = 0;
    g_ready.fetch_add(1, std::memory_order_release);
    while (!g_go.load(std::memory_order_acquire))
        std::this_thread::yield();
    while (!g_stop.load(std::memory_order_relaxed)) {
        Tx tx = store->begin_write();
        Tree<int64_t> t = tx.tree<int64_t>("w");
        for (int i = 0; i < 10; ++i)
            t.set(pos++ % writer_slots, x++);
        tx.commit();
        ++commits;
    }
    return commits;
}

struct Run {
    double reads_per_s;
    double writer_commits_per_s;
};

static Run run_scenario(StoreRef store, size_t N, int T, int dur_ms, bool with_writer)
{
    g_go.store(false);
    g_stop.store(false);
    g_ready.store(0);

    std::vector<uint64_t> res(T, 0);
    std::vector<std::thread> ths;
    ths.reserve(T);
    for (int i = 0; i < T; ++i)
        ths.emplace_back([&, i] { res[i] = reader_loop(store, N, i); });

    uint64_t wc = 0;
    std::thread wth;
    int expect = T + (with_writer ? 1 : 0);
    if (with_writer)
        wth = std::thread([&] { wc = writer_loop(store); });

    while (g_ready.load(std::memory_order_acquire) < expect) // wait until all at the barrier
        std::this_thread::yield();

    auto t0 = Clock::now();
    g_go.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::milliseconds(dur_ms));
    g_stop.store(true, std::memory_order_relaxed);
    auto t1 = Clock::now();

    for (auto& t : ths)
        t.join();
    if (with_writer)
        wth.join();

    double wall = secs(t1 - t0);
    uint64_t total = 0;
    for (auto v : res)
        total += v;
    return {total / wall, with_writer ? wc / wall : 0.0};
}

// Median of paired-length runs is less sensitive to one lucky scheduler slice
// than selecting the fastest result.
static Run median_of(StoreRef store, size_t N, int T, int dur_ms, bool with_writer, int iters)
{
    std::vector<Run> runs;
    runs.reserve(iters);
    for (int i = 0; i < iters; ++i)
        runs.push_back(run_scenario(store, N, T, dur_ms, with_writer));
    std::sort(runs.begin(), runs.end(), [](const Run& a, const Run& b) {
        return a.reads_per_s < b.reads_per_s;
    });
    return runs[runs.size() / 2];
}

int main(int argc, char** argv)
{
    double scale = (argc > 1) ? std::atof(argv[1]) : 1.0;
    if (scale <= 0)
        scale = 1.0;
    size_t N = size_t(1'000'000 * scale);
    int dur_ms = 400;

    std::string dir = util::make_temp_dir();
    Options o;
    o.no_sync = true; // writer isn't fsync-bound here — we want it hammering commits to stress readers
    o.single_process = true;
    StoreRef store = Store::open(util::File::resolve("conc.llbt", dir), o);
    {
        Tx tx = store->begin_write();
        Tree<int64_t> t = tx.tree<int64_t>("k");
        for (size_t i = 0; i < N; ++i)
            t.add(int64_t(i));
        Tree<int64_t> w = tx.tree<int64_t>("w");
        for (size_t i = 0; i < writer_slots; ++i)
            w.add(0);
        tx.commit();
    }

    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0)
        hw = 8;
    std::vector<int> tc;
    for (int t : {1, 2, 4, 8, 16, 32})
        if (t <= int(hw) && (tc.empty() || tc.back() != t))
            tc.push_back(t);
    if (tc.empty() || tc.back() != int(hw))
        tc.push_back(int(hw));

    std::printf("llbt reader scaling — N=%zu int64 (~%zu MB), %dms windows, MVCC snapshots\n", N, N * 8 / (1 << 20),
                dur_ms);
    std::printf("single-writer/many-reader: readers hold an immutable snapshot and take no data lock.\n");
    std::printf("hw concurrency=%u. 'vs alone' = reader throughput retained while a writer commits nonstop.\n\n", hw);

    std::printf("%-8s %13s %9s   %16s %9s %13s\n", "threads", "reads/s", "scaling", "reads/s+writer", "vs alone",
                "wr commits/s");
    std::printf("-------------------------------------------------------------------------------------\n");
    median_of(store, N, 2, 150, true, 1); // warm up mmap, threads, writer path (discarded)
    double base = 0;
    for (int T : tc) {
        Run alone = median_of(store, N, T, dur_ms, false, 3);
        Run withw = median_of(store, N, T, dur_ms, true, 3);
        if (base == 0)
            base = alone.reads_per_s;
        char sc[16], vs[16];
        std::snprintf(sc, sizeof sc, "%.2fx", alone.reads_per_s / base);
        std::snprintf(vs, sizeof vs, "%.0f%%", 100.0 * withw.reads_per_s / alone.reads_per_s);
        std::printf("%-8d %13s %9s   %16s %9s %13s\n", T, rate(alone.reads_per_s).c_str(), sc,
                    rate(withw.reads_per_s).c_str(), vs, rate(withw.writer_commits_per_s).c_str());
    }
    std::printf("\n(sink=%llu)\n", (unsigned long long)g_sink.load(std::memory_order_relaxed));
    store.reset();
    util::try_remove_dir_recursive(dir);
    return 0;
}
