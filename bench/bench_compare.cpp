// llbt vs LMDB, side by side.
//
// LMDB is the truest peer: an embedded, memory-mapped, copy-on-write B+tree
// with MVCC — the same architecture as llbt::core. Same workloads, same
// dataset, same machine, so the numbers mean something relative to each other.
//
// Fairness notes (read these before trusting a row):
//   * Durability is NOT identical in native sync mode. On macOS llbt uses
//     F_BARRIERFSYNC (write ordering), while stock LMDB uses F_FULLFSYNC
//     (stronger persistence at return). Native-sync rows are shown but are
//     deliberately not scored. The no-flush rows are the closer comparison.
//   * Data model differs for dense integers. llbt::core Tree<T> stores only a
//     value and uses its position as the key; LMDB stores and searches an
//     explicit key plus value. Those native-API rows characterize each engine
//     but are not scored as equivalent KV work. The string rows do represent
//     the same logical 16-byte-key -> 100-byte-value map.
//
// Build: needs LMDB (brew install lmdb / apt install liblmdb-dev). CMake finds
// it automatically and builds this target when present.
#include <llbt/api.hpp>
#include <lmdb.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace llbt;
using namespace llbt::core;
using Clock = std::chrono::steady_clock;

static volatile int64_t g_sink = 0;
static double secs(Clock::duration d) { return std::chrono::duration<double>(d).count(); }

#define MCHECK(x)                                                                                            \
    do {                                                                                                     \
        int rc__ = (x);                                                                                      \
        if (rc__) {                                                                                          \
            std::fprintf(stderr, "lmdb error %d: %s\n", rc__, mdb_strerror(rc__));                           \
            std::abort();                                                                                    \
        }                                                                                                    \
    } while (0)

static std::string rate(double v)
{
    char b[32];
    if (v >= 1e6)      std::snprintf(b, sizeof b, "%.2f M/s", v / 1e6);
    else if (v >= 1e3) std::snprintf(b, sizeof b, "%.1f k/s", v / 1e3);
    else               std::snprintf(b, sizeof b, "%.0f /s", v);
    return b;
}

static std::string t_str(double s)
{
    char b[32];
    if (s < 0)          return "";
    if (s >= 1e-3)      std::snprintf(b, sizeof b, "%.0fus", s * 1e6); // sub-ms shown in us for width
    else if (s >= 1e-6) std::snprintf(b, sizeof b, "%.0fus", s * 1e6);
    else                std::snprintf(b, sizeof b, "%.0fns", s * 1e9);
    return b;
}

static double pctl(std::vector<double>& v, double p)
{
    if (v.empty()) return -1;
    size_t i = size_t(p * (v.size() - 1));
    std::nth_element(v.begin(), v.begin() + i, v.end());
    return v[i];
}

struct R {
    double ops = 0;
    double p50 = -1;
    std::string note;
};

static std::string g_dir;
static int g_seq = 0;
static std::string fresh(const char* tag) { return util::File::resolve(std::string(tag) + std::to_string(g_seq++) + ".db", g_dir); }

// ---- llbt side -----------------------------------------------------------

static StoreRef llbt_open(bool nosync)
{
    Options o;
    o.no_sync = nosync;
    // the LMDB side runs MDB_NOLOCK; this is the matching configuration
    o.single_process = true;
    return Store::open(fresh("llbt"), o);
}

static R llbt_seq_write(bool nosync, size_t N)
{
    StoreRef s = llbt_open(nosync);
    auto t0 = Clock::now();
    {
        Tx tx = s->begin_write();
        Tree<int64_t> t = tx.tree<int64_t>("k");
        for (size_t i = 0; i < N; ++i)
            t.add(int64_t(i));
        tx.commit();
    }
    double sec = secs(Clock::now() - t0);
    char nb[32];
    std::snprintf(nb, sizeof nb, "%.1f MB", util::File::get_size_static(s->path()) / 1e6);
    return {N / sec, -1, nb};
}

static R llbt_commit(bool nosync, size_t min_commits, size_t B, double min_seconds)
{
    StoreRef s = llbt_open(nosync);
    std::vector<double> lat;
    lat.reserve(min_commits);
    int64_t k = 0;
    for (int w = 0; w < 3; ++w) {
        Tx tx = s->begin_write();
        Tree<int64_t> t = tx.tree<int64_t>("k");
        for (size_t b = 0; b < B; ++b)
            t.add(k++);
        tx.commit();
    }
    auto t0 = Clock::now();
    size_t commits = 0;
    double tot = 0;
    do {
        for (size_t chunk = 0; chunk < 64; ++chunk) {
            auto c0 = Clock::now();
            Tx tx = s->begin_write();
            Tree<int64_t> t = tx.tree<int64_t>("k");
            for (size_t b = 0; b < B; ++b)
                t.add(k++);
            tx.commit();
            lat.push_back(secs(Clock::now() - c0));
            ++commits;
        }
        tot = secs(Clock::now() - t0);
    } while (commits < min_commits || tot < min_seconds);
    return {commits / tot, pctl(lat, 0.50), ""};
}

static StoreRef llbt_populate(bool nosync, size_t N)
{
    StoreRef s = llbt_open(nosync);
    Tx tx = s->begin_write();
    Tree<int64_t> t = tx.tree<int64_t>("k");
    for (size_t i = 0; i < N; ++i)
        t.add(int64_t(i));
    tx.commit();
    return s;
}

static R llbt_rand_read(size_t N, const std::vector<uint32_t>& idx)
{
    StoreRef s = llbt_populate(false, N);
    Tx tx = s->begin_read();
    Tree<int64_t> t = tx.tree<int64_t>("k");
    int64_t sink = 0;
    size_t M = idx.size();
    for (size_t i = 0; i < M / 10; ++i)
        sink += t.get(idx[i]);
    auto t0 = Clock::now();
    for (size_t i = 0; i < M; ++i)
        sink += t.get(idx[i]);
    double sec = secs(Clock::now() - t0);
    g_sink += sink;
    return {M / sec, -1, ""};
}

static R llbt_scan(size_t N, size_t passes)
{
    StoreRef s = llbt_populate(false, N);
    Tx tx = s->begin_read();
    Tree<int64_t> t = tx.tree<int64_t>("k");
    int64_t sink = 0;
    for (auto c = t.cursor(); c.valid(); c.next())
        sink += c.value(); // warm-up
    auto t0 = Clock::now();
    for (size_t pass = 0; pass < passes; ++pass) {
        for (auto c = t.cursor(); c.valid(); c.next())
            sink += c.value();
    }
    double sec = secs(Clock::now() - t0);
    g_sink += sink;
    return {double(N) * passes / sec, -1, ""};
}

struct KV {
    R build, lookup;
};

static KV llbt_kv(bool nosync, size_t K, size_t L, size_t vsize, const std::vector<std::string>& keys,
                  const std::vector<size_t>& which)
{
    StoreRef s = llbt_open(nosync);
    std::string val(vsize, 'x');
    // One tree of packed key+value records. Records sort by key because the
    // fixed-width key is the record prefix. lower_bound(key) finds a candidate;
    // the lookup below still verifies that prefix before reading the value.
    const size_t ksz = keys.empty() ? 0 : keys[0].size();
    std::string rec;
    auto t0 = Clock::now();
    {
        Tx tx = s->begin_write();
        Tree<StringData> t = tx.tree<StringData>("kv");
        for (size_t i = 0; i < K; ++i) {
            rec.assign(keys[i]);
            rec += val;
            size_t p = t.lower_bound(StringData(keys[i]));
            t.insert(p, StringData(rec));
        }
        tx.commit();
    }
    double bsec = secs(Clock::now() - t0);
    char nb[32];
    std::snprintf(nb, sizeof nb, "%.1f MB", util::File::get_size_static(s->path()) / 1e6);

    Tx tx = s->begin_read();
    Tree<StringData> t = tx.tree<StringData>("kv");
    int64_t sink = 0;
    auto look = [&](size_t i) {
        const std::string& key = keys[which[i]];
        size_t p = t.lower_bound(StringData(key));
        if (p == t.size())
            std::abort();
        StringData record = t.get(p);
        if (record.size() < ksz || std::memcmp(record.data(), key.data(), ksz) != 0)
            std::abort();
        sink += int64_t(record.size() - ksz);
    };
    for (size_t i = 0; i < L / 10; ++i) {
        look(i);
    }
    auto t1 = Clock::now();
    for (size_t i = 0; i < L; ++i)
        look(i);
    double lsec = secs(Clock::now() - t1);
    g_sink += sink;
    return {{K / bsec, -1, nb}, {L / lsec, -1, ""}};
}

// ---- LMDB side -----------------------------------------------------------

struct LmdbEnv {
    MDB_env* env = nullptr;
    std::string path;
    LmdbEnv(unsigned flags, size_t mapsize)
    {
        path = fresh("lmdb");
        MCHECK(mdb_env_create(&env));
        MCHECK(mdb_env_set_mapsize(env, mapsize));
        MCHECK(mdb_env_open(env, path.c_str(), flags | MDB_NOSUBDIR | MDB_NOLOCK, 0664));
    }
    ~LmdbEnv()
    {
        if (env)
            mdb_env_close(env);
    }
    double used_mb()
    {
        MDB_stat st;
        MDB_envinfo info;
        mdb_env_stat(env, &st);
        mdb_env_info(env, &info);
        return double(info.me_last_pgno + 1) * st.ms_psize / 1e6;
    }
};

static R lmdb_seq_write(unsigned flags, size_t N)
{
    LmdbEnv e(flags, size_t(1) << 30);
    MDB_txn* txn;
    MDB_dbi dbi;
    auto t0 = Clock::now();
    MCHECK(mdb_txn_begin(e.env, nullptr, 0, &txn));
    MCHECK(mdb_dbi_open(txn, nullptr, MDB_INTEGERKEY | MDB_CREATE, &dbi));
    for (size_t i = 0; i < N; ++i) {
        size_t k = i;
        int64_t v = int64_t(i);
        MDB_val mk{sizeof k, &k}, mv{sizeof v, &v};
        MCHECK(mdb_put(txn, dbi, &mk, &mv, MDB_APPEND));
    }
    MCHECK(mdb_txn_commit(txn));
    double sec = secs(Clock::now() - t0);
    char nb[32];
    std::snprintf(nb, sizeof nb, "%.1f MB", e.used_mb());
    return {N / sec, -1, nb};
}

static R lmdb_commit(unsigned flags, size_t min_commits, size_t B, double min_seconds)
{
    LmdbEnv e(flags, size_t(1) << 30);
    MDB_dbi dbi;
    {
        MDB_txn* t;
        MCHECK(mdb_txn_begin(e.env, nullptr, 0, &t));
        MCHECK(mdb_dbi_open(t, nullptr, MDB_INTEGERKEY | MDB_CREATE, &dbi));
        MCHECK(mdb_txn_commit(t));
    }
    std::vector<double> lat;
    lat.reserve(min_commits);
    size_t k = 0;
    auto one = [&](bool timed) {
        auto c0 = Clock::now();
        MDB_txn* txn;
        MCHECK(mdb_txn_begin(e.env, nullptr, 0, &txn));
        for (size_t b = 0; b < B; ++b) {
            size_t kk = k++;
            int64_t v = int64_t(kk);
            MDB_val mk{sizeof kk, &kk}, mv{sizeof v, &v};
            MCHECK(mdb_put(txn, dbi, &mk, &mv, 0));
        }
        MCHECK(mdb_txn_commit(txn));
        if (timed)
            lat.push_back(secs(Clock::now() - c0));
    };
    for (int w = 0; w < 3; ++w)
        one(false);
    auto t0 = Clock::now();
    size_t commits = 0;
    double tot = 0;
    do {
        for (size_t chunk = 0; chunk < 64; ++chunk) {
            one(true);
            ++commits;
        }
        tot = secs(Clock::now() - t0);
    } while (commits < min_commits || tot < min_seconds);
    return {commits / tot, pctl(lat, 0.50), ""};
}

static void lmdb_fill(LmdbEnv& e, size_t N, MDB_dbi& dbi)
{
    MDB_txn* txn;
    MCHECK(mdb_txn_begin(e.env, nullptr, 0, &txn));
    MCHECK(mdb_dbi_open(txn, nullptr, MDB_INTEGERKEY | MDB_CREATE, &dbi));
    for (size_t i = 0; i < N; ++i) {
        size_t k = i;
        int64_t v = int64_t(i);
        MDB_val mk{sizeof k, &k}, mv{sizeof v, &v};
        MCHECK(mdb_put(txn, dbi, &mk, &mv, MDB_APPEND));
    }
    MCHECK(mdb_txn_commit(txn));
}

static R lmdb_rand_read(size_t N, const std::vector<uint32_t>& idx)
{
    LmdbEnv e(0, size_t(1) << 30);
    MDB_dbi dbi;
    lmdb_fill(e, N, dbi);
    MDB_txn* rt;
    MCHECK(mdb_txn_begin(e.env, nullptr, MDB_RDONLY, &rt));
    int64_t sink = 0;
    size_t M = idx.size();
    auto get = [&](size_t i) {
        size_t k = idx[i];
        MDB_val mk{sizeof k, &k}, mv;
        MCHECK(mdb_get(rt, dbi, &mk, &mv));
        int64_t v;
        std::memcpy(&v, mv.mv_data, sizeof v);
        sink += v;
    };
    for (size_t i = 0; i < M / 10; ++i)
        get(i);
    auto t0 = Clock::now();
    for (size_t i = 0; i < M; ++i)
        get(i);
    double sec = secs(Clock::now() - t0);
    mdb_txn_abort(rt);
    g_sink += sink;
    return {M / sec, -1, ""};
}

static R lmdb_scan(size_t N, size_t passes)
{
    LmdbEnv e(0, size_t(1) << 30);
    MDB_dbi dbi;
    lmdb_fill(e, N, dbi);
    MDB_txn* rt;
    MCHECK(mdb_txn_begin(e.env, nullptr, MDB_RDONLY, &rt));
    MDB_cursor* cur;
    MCHECK(mdb_cursor_open(rt, dbi, &cur));
    int64_t sink = 0;
    MDB_val k, v;
    auto scan_once = [&] {
        int rc = mdb_cursor_get(cur, &k, &v, MDB_FIRST);
        while (rc == 0) {
            int64_t val;
            std::memcpy(&val, v.mv_data, sizeof val);
            sink += val;
            rc = mdb_cursor_get(cur, &k, &v, MDB_NEXT);
        }
        if (rc != MDB_NOTFOUND)
            MCHECK(rc);
    };
    scan_once(); // warm-up
    auto t0 = Clock::now();
    for (size_t pass = 0; pass < passes; ++pass)
        scan_once();
    double sec = secs(Clock::now() - t0);
    mdb_cursor_close(cur);
    mdb_txn_abort(rt);
    g_sink += sink;
    return {double(N) * passes / sec, -1, ""};
}

static KV lmdb_kv(bool nosync, size_t K, size_t L, size_t vsize, const std::vector<std::string>& keys,
                  const std::vector<size_t>& which)
{
    LmdbEnv e(nosync ? MDB_NOSYNC : 0, size_t(1) << 30);
    std::string val(vsize, 'x');
    MDB_dbi dbi;
    auto t0 = Clock::now();
    {
        MDB_txn* txn;
        MCHECK(mdb_txn_begin(e.env, nullptr, 0, &txn));
        MCHECK(mdb_dbi_open(txn, nullptr, MDB_CREATE, &dbi));
        for (size_t i = 0; i < K; ++i) {
            MDB_val mk{keys[i].size(), (void*)keys[i].data()}, mv{val.size(), (void*)val.data()};
            int rc = mdb_put(txn, dbi, &mk, &mv, 0);
            if (rc && rc != MDB_KEYEXIST)
                MCHECK(rc);
        }
        MCHECK(mdb_txn_commit(txn));
    }
    double bsec = secs(Clock::now() - t0);
    char nb[32];
    std::snprintf(nb, sizeof nb, "%.1f MB", e.used_mb());

    MDB_txn* rt;
    MCHECK(mdb_txn_begin(e.env, nullptr, MDB_RDONLY, &rt));
    int64_t sink = 0;
    auto look = [&](size_t i) {
        MDB_val mk{keys[which[i]].size(), (void*)keys[which[i]].data()}, mv;
        MCHECK(mdb_get(rt, dbi, &mk, &mv));
        if (mv.mv_size != vsize)
            std::abort();
        sink += mv.mv_size;
    };
    for (size_t i = 0; i < L / 10; ++i)
        look(i);
    auto t1 = Clock::now();
    for (size_t i = 0; i < L; ++i)
        look(i);
    double lsec = secs(Clock::now() - t1);
    mdb_txn_abort(rt);
    g_sink += sink;
    return {{K / bsec, -1, nb}, {L / lsec, -1, ""}};
}

// ---- side-by-side printing -----------------------------------------------

enum class Basis { Comparable, DifferentSync, DifferentModel };

struct PairResult {
    R llbt;
    R lmdb;
    bool same_winner = false;
};

static R median(std::vector<R> runs)
{
    std::sort(runs.begin(), runs.end(), [](const R& a, const R& b) {
        return a.ops < b.ops;
    });
    return runs[runs.size() / 2];
}

template <class LlbtFn, class LmdbFn>
static PairResult measure_pair(int samples, LlbtFn&& run_llbt, LmdbFn&& run_lmdb)
{
    std::vector<R> llbt_runs, lmdb_runs;
    llbt_runs.reserve(samples);
    lmdb_runs.reserve(samples);
    bool direction = false;
    bool same_winner = true;
    for (int sample = 0; sample < samples; ++sample) {
        R a, b;
        if ((sample & 1) == 0) {
            a = run_llbt();
            b = run_lmdb();
        }
        else {
            b = run_lmdb();
            a = run_llbt();
        }
        const bool this_direction = a.ops >= b.ops;
        if (sample == 0)
            direction = this_direction;
        else if (this_direction != direction)
            same_winner = false;
        llbt_runs.push_back(std::move(a));
        lmdb_runs.push_back(std::move(b));
    }
    return {median(std::move(llbt_runs)), median(std::move(lmdb_runs)), same_winner};
}

struct KVPairResult {
    PairResult build;
    PairResult lookup;
};

template <class LlbtFn, class LmdbFn>
static KVPairResult measure_kv_pair(int samples, LlbtFn&& run_llbt, LmdbFn&& run_lmdb)
{
    std::vector<R> ab, al, bb, bl;
    bool build_direction = false, lookup_direction = false;
    bool build_same = true, lookup_same = true;
    for (int sample = 0; sample < samples; ++sample) {
        KV a, b;
        if ((sample & 1) == 0) {
            a = run_llbt();
            b = run_lmdb();
        }
        else {
            b = run_lmdb();
            a = run_llbt();
        }
        const bool bd = a.build.ops >= b.build.ops;
        const bool ld = a.lookup.ops >= b.lookup.ops;
        if (sample == 0) {
            build_direction = bd;
            lookup_direction = ld;
        }
        else {
            build_same = build_same && bd == build_direction;
            lookup_same = lookup_same && ld == lookup_direction;
        }
        ab.push_back(std::move(a.build));
        al.push_back(std::move(a.lookup));
        bb.push_back(std::move(b.build));
        bl.push_back(std::move(b.lookup));
    }
    return {{median(std::move(ab)), median(std::move(bb)), build_same},
            {median(std::move(al)), median(std::move(bl)), lookup_same}};
}

static void header()
{
    std::printf("%-16s %-9s %12s %12s   %-16s  %s\n", "workload", "mode", "llbt", "lmdb", "result", "notes");
    std::printf("-----------------------------------------------------------------------------------------------------\n");
}

static void cmp(const char* wl, const char* mode, const PairResult& pair, Basis basis)
{
    const R& a = pair.llbt;
    const R& b = pair.lmdb;
    char result[32];
    if (basis == Basis::DifferentSync) {
        std::snprintf(result, sizeof result, "not comparable");
    }
    else if (basis == Basis::DifferentModel) {
        std::snprintf(result, sizeof result, "native APIs");
    }
    else if (!pair.same_winner) {
        std::snprintf(result, sizeof result, "mixed samples");
    }
    else {
        const char* win = a.ops >= b.ops ? "llbt" : "lmdb";
        double x = a.ops >= b.ops ? a.ops / b.ops : b.ops / a.ops;
        std::snprintf(result, sizeof result, "%s %.2fx", win, x);
    }

    std::string note;
    if (!a.note.empty())
        note = "llbt " + a.note;
    if (!b.note.empty())
        note += (note.empty() ? "" : " vs ") + std::string("lmdb ") + b.note;
    if (a.p50 >= 0 || b.p50 >= 0)
        note += (note.empty() ? "" : "  ") + std::string("p50 ") + t_str(a.p50) + "/" + t_str(b.p50);

    std::printf("%-16s %-9s %12s %12s   %-16s  %s\n", wl, mode, rate(a.ops).c_str(), rate(b.ops).c_str(),
                result, note.c_str());
}

int main(int argc, char** argv)
{
    double scale = (argc > 1) ? std::atof(argv[1]) : 1.0;
    if (scale <= 0)
        scale = 1.0;
    auto S = [&](size_t base) { return std::max(size_t(1), size_t(base * scale)); };

    g_dir = util::make_temp_dir();

    const int SAMPLES = 3;
    const size_t N = S(1'000'000);
    const size_t READS = S(3'000'000);
    const size_t MIN_COMMITS = S(128);
    const double MIN_COMMIT_SECONDS = 0.35 * std::max(0.25, scale);
    const size_t SCAN_PASSES = S(50);
    const size_t KVK = S(200'000);
    const size_t KVL = S(1'000'000);

    std::mt19937_64 rng(12345);
    std::uniform_int_distribution<uint32_t> d(0, uint32_t(N - 1));
    std::vector<uint32_t> idx(READS);
    for (auto& x : idx)
        x = d(rng);
    std::vector<std::string> keys(KVK);
    for (size_t i = 0; i < keys.size(); ++i) {
        char key[17];
        std::snprintf(key, sizeof key, "%016llx", (unsigned long long)i);
        keys[i] = key;
    }
    std::shuffle(keys.begin(), keys.end(), rng);
    std::vector<size_t> which(KVL);
    {
        std::uniform_int_distribution<size_t> p(0, KVK - 1);
        for (auto& x : which)
            x = p(rng);
    }

    std::printf("llbt vs LMDB %s   %u cores, scale=%.2f, Release\n", MDB_VERSION_STRING,
                std::thread::hardware_concurrency(), scale);
    std::printf("median of %d fresh-DB samples; engine order alternates; commit samples run at least %.2fs.\n",
                SAMPLES, MIN_COMMIT_SECONDS);
#if defined(__APPLE__)
    std::printf("native sync is unscored: llbt=F_BARRIERFSYNC ordering, LMDB=F_FULLFSYNC persistence.\n");
#else
    std::printf("native sync is unscored because the engines use different synchronization paths.\n");
#endif
    std::printf("integer rows are unscored native APIs: llbt positional values vs LMDB explicit key/value.\n");
    std::printf("single-process tuning: llbt process mutexes; LMDB MDB_NOLOCK.\n\n");

    header();
    cmp("seq-write", "native",
        measure_pair(SAMPLES, [&] { return llbt_seq_write(false, N); }, [&] { return lmdb_seq_write(0, N); }),
        Basis::DifferentSync);
    cmp("seq-write", "no-flush",
        measure_pair(SAMPLES, [&] { return llbt_seq_write(true, N); },
                     [&] { return lmdb_seq_write(MDB_NOSYNC, N); }),
        Basis::DifferentModel);
    std::printf("\n");
    cmp("commit(b=1)", "native",
        measure_pair(SAMPLES, [&] { return llbt_commit(false, MIN_COMMITS, 1, MIN_COMMIT_SECONDS); },
                     [&] { return lmdb_commit(0, MIN_COMMITS, 1, MIN_COMMIT_SECONDS); }),
        Basis::DifferentSync);
    cmp("commit(b=1)", "no-flush",
        measure_pair(SAMPLES, [&] { return llbt_commit(true, MIN_COMMITS, 1, MIN_COMMIT_SECONDS); },
                     [&] { return lmdb_commit(MDB_NOSYNC, MIN_COMMITS, 1, MIN_COMMIT_SECONDS); }),
        Basis::DifferentModel);
    cmp("commit(b=100)", "native",
        measure_pair(SAMPLES, [&] { return llbt_commit(false, MIN_COMMITS, 100, MIN_COMMIT_SECONDS); },
                     [&] { return lmdb_commit(0, MIN_COMMITS, 100, MIN_COMMIT_SECONDS); }),
        Basis::DifferentSync);
    cmp("commit(b=100)", "no-flush",
        measure_pair(SAMPLES, [&] { return llbt_commit(true, MIN_COMMITS, 100, MIN_COMMIT_SECONDS); },
                     [&] { return lmdb_commit(MDB_NOSYNC, MIN_COMMITS, 100, MIN_COMMIT_SECONDS); }),
        Basis::DifferentModel);
    std::printf("\n");
    cmp("rand-read", "native",
        measure_pair(SAMPLES, [&] { return llbt_rand_read(N, idx); }, [&] { return lmdb_rand_read(N, idx); }),
        Basis::DifferentModel);
    cmp("seq-scan", "native",
        measure_pair(SAMPLES, [&] { return llbt_scan(N, SCAN_PASSES); },
                     [&] { return lmdb_scan(N, SCAN_PASSES); }),
        Basis::DifferentModel);
    std::printf("\n");
    {
        KVPairResult kv = measure_kv_pair(
            SAMPLES, [&] { return llbt_kv(true, KVK, KVL, 100, keys, which); },
            [&] { return lmdb_kv(true, KVK, KVL, 100, keys, which); });
        cmp("kv-str-build", "no-flush", kv.build, Basis::Comparable);
        cmp("kv-str-lookup", "-", kv.lookup, Basis::Comparable);
    }

    std::printf("\n(sink=%lld)\n", (long long)g_sink);
    util::try_remove_dir_recursive(g_dir);
    return 0;
}
