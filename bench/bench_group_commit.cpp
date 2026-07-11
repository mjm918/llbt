// llbt group-commit benchmark.
//
// The claim under test: with T threads doing small DURABLE transactions,
// Store::write() coalesces concurrent commits into one physical commit —
// one storage sync for the whole batch — while plain begin_write()/commit()
// pays one sync per transaction.
//
//   plain    : each thread loops  begin_write / set one slot / commit
//   grouped  : each thread loops  store->write([&](Tx&){ set one slot })
//
// "writes/commit" is the batching factor actually achieved by the grouped
// mode (physical commits counted by store version delta). With one thread
// there is nothing to batch, so grouped must match plain — that line doubles
// as the no-regression check, and a nosync single-thread pass makes the same
// point where commits are cheap and batching can't hide overhead.
//
// Build: cmake -B build -DLLBT_BUILD_BENCH=ON -DCMAKE_BUILD_TYPE=Release
//        ./build/bench/llbt-bench-group-commit [dur_ms]
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
static double secs(Clock::duration d)
{
    return std::chrono::duration<double>(d).count();
}
static constexpr size_t slots = 4096;

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

enum class Mode { plain, grouped };

// One writer: tiny transactions (set one slot) until told to stop.
static uint64_t writer_thread(StoreRef store, Mode mode, int tid)
{
    uint64_t s = 0x9e3779b97f4a7c15ull ^ (uint64_t(tid + 1) * 0x100000001b3ull);
    auto next = [&]() -> uint64_t { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; };

    uint64_t ops = 0;
    int64_t x = int64_t(tid + 1) << 32;
    g_ready.fetch_add(1, std::memory_order_release);
    while (!g_go.load(std::memory_order_acquire))
        std::this_thread::yield();
    while (!g_stop.load(std::memory_order_relaxed)) {
        size_t slot = size_t(next() % slots);
        if (mode == Mode::plain) {
            Tx tx = store->begin_write();
            tx.tree<int64_t>("w").set(slot, x++);
            tx.commit();
        }
        else {
            store->write([&](Tx& tx) {
                tx.tree<int64_t>("w").set(slot, x++);
            });
        }
        ++ops;
    }
    return ops;
}

struct Run {
    double writes_per_s;
    double writes_per_commit;
};

static uint64_t current_version(const StoreRef& store)
{
    return store->begin_read().raw().get_version();
}

static Run run_writers(StoreRef store, Mode mode, int T, int dur_ms)
{
    g_go.store(false);
    g_stop.store(false);
    g_ready.store(0);

    std::vector<uint64_t> res(T, 0);
    std::vector<std::thread> ths;
    ths.reserve(T);
    for (int i = 0; i < T; ++i)
        ths.emplace_back([&, i] { res[size_t(i)] = writer_thread(store, mode, i); });

    while (g_ready.load(std::memory_order_acquire) < T) // wait until all at the barrier
        std::this_thread::yield();

    uint64_t v0 = current_version(store);
    auto t0 = Clock::now();
    g_go.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::milliseconds(dur_ms));
    g_stop.store(true, std::memory_order_relaxed);
    for (auto& t : ths)
        t.join();
    // walls close after join so the in-flight tail counts in both ops and time
    auto t1 = Clock::now();
    uint64_t v1 = current_version(store);

    double wall = secs(t1 - t0);
    uint64_t total = 0;
    for (auto v : res)
        total += v;
    uint64_t commits = v1 > v0 ? v1 - v0 : 1;
    return {total / wall, double(total) / double(commits)};
}

static Run median_of(StoreRef store, Mode mode, int T, int dur_ms, int iters)
{
    std::vector<Run> runs;
    runs.reserve(iters);
    for (int i = 0; i < iters; ++i)
        runs.push_back(run_writers(store, mode, T, dur_ms));
    std::sort(runs.begin(), runs.end(), [](const Run& a, const Run& b) {
        return a.writes_per_s < b.writes_per_s;
    });
    return runs[runs.size() / 2];
}

static StoreRef make_store(const std::string& dir, const char* name, bool no_sync)
{
    Options o;
    o.no_sync = no_sync;
    o.single_process = true;
    StoreRef store = Store::open(util::File::resolve(name, dir), o);
    Tx tx = store->begin_write();
    Tree<int64_t> w = tx.tree<int64_t>("w");
    for (size_t i = 0; i < slots; ++i)
        w.add(0);
    tx.commit();
    return store;
}

int main(int argc, char** argv)
{
    int dur_ms = (argc > 1) ? std::atoi(argv[1]) : 400;
    if (dur_ms <= 0)
        dur_ms = 400;

    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0)
        hw = 8;
    std::vector<int> tc;
    for (int t : {1, 2, 4, 8, 16})
        if (t <= int(hw) && (tc.empty() || tc.back() != t))
            tc.push_back(t);

    std::string dir = util::make_temp_dir();
    std::printf("llbt group commit — small txns (set one int64 slot of %zu), %dms windows, median of 3\n", slots,
                dur_ms);
    std::printf("plain = one durable commit per txn; grouped = Store::write(), one durable commit per batch.\n\n");

    // no-batching overhead check: nosync, single thread — commits are ~free,
    // so any bookkeeping write() adds would show up right here
    {
        StoreRef store = make_store(dir, "gc-nosync.llbt", true);
        median_of(store, Mode::plain, 1, 150, 1); // warmup (discarded)
        Run p = median_of(store, Mode::plain, 1, dur_ms, 3);
        Run g = median_of(store, Mode::grouped, 1, dur_ms, 3);
        std::printf("overhead check (nosync, 1 thread): plain %s | grouped %s (%.0f%%)\n\n",
                    rate(p.writes_per_s).c_str(), rate(g.writes_per_s).c_str(),
                    100.0 * g.writes_per_s / p.writes_per_s);
    }

    // the headline: durable commits, growing writer counts
    StoreRef store = make_store(dir, "gc-durable.llbt", false);
    median_of(store, Mode::grouped, 2, 150, 1); // warmup (discarded)

    std::printf("%-8s %13s %14s %9s %14s\n", "threads", "plain wr/s", "grouped wr/s", "speedup", "writes/commit");
    std::printf("--------------------------------------------------------------\n");
    for (int T : tc) {
        Run p = median_of(store, Mode::plain, T, dur_ms, 3);
        Run g = median_of(store, Mode::grouped, T, dur_ms, 3);
        char sp[16], wc[16];
        std::snprintf(sp, sizeof sp, "%.2fx", g.writes_per_s / p.writes_per_s);
        std::snprintf(wc, sizeof wc, "%.1f", g.writes_per_commit);
        std::printf("%-8d %13s %14s %9s %14s\n", T, rate(p.writes_per_s).c_str(), rate(g.writes_per_s).c_str(), sp,
                    wc);
    }

    store.reset();
    util::try_remove_dir_recursive(dir);
    return 0;
}
