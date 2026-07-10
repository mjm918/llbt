// llbt vs LMDB, side by side.
//
// LMDB is the truest peer: an embedded, memory-mapped, copy-on-write B+tree
// with MVCC — the same architecture as llbt::core. Same workloads, same
// dataset, same machine, so the numbers mean something relative to each other.
//
// Fairness notes (read these before trusting a row):
//   * Durability is NOT identical in `sync` mode. llbt commits with Apple's
//     F_BARRIERFSYNC (an ordered barrier — crash-consistent). LMDB uses
//     fsync(), which on macOS does NOT flush the drive's write cache. So in
//     `sync` mode llbt is doing a stronger (slower) guarantee. The `nosync`
//     rows (both skip the durability flush) isolate raw engine overhead and
//     are the cleaner apples-to-apples.
//   * Data model differs. llbt::core Tree<T> is a POSITIONAL sequence (index
//     is the key); LMDB is a sorted key->value map. For dense integer keys
//     (0..N-1) these coincide. For string keys llbt has no native sorted map,
//     so we build one by hand (binary-search insert) — LMDB does it in one
//     operation, and that gap is real and worth seeing.
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

static R llbt_commit(bool nosync, size_t C, size_t B)
{
    StoreRef s = llbt_open(nosync);
    std::vector<double> lat;
    lat.reserve(C);
    int64_t k = 0;
    for (int w = 0; w < 3; ++w) {
        Tx tx = s->begin_write();
        Tree<int64_t> t = tx.tree<int64_t>("k");
        for (size_t b = 0; b < B; ++b)
            t.add(k++);
        tx.commit();
    }
    auto t0 = Clock::now();
    for (size_t c = 0; c < C; ++c) {
        auto c0 = Clock::now();
        Tx tx = s->begin_write();
        Tree<int64_t> t = tx.tree<int64_t>("k");
        for (size_t b = 0; b < B; ++b)
            t.add(k++);
        tx.commit();
        lat.push_back(secs(Clock::now() - c0));
    }
    double tot = secs(Clock::now() - t0);
    return {C / tot, pctl(lat, 0.50), ""};
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

static R llbt_scan(size_t N)
{
    StoreRef s = llbt_populate(false, N);
    Tx tx = s->begin_read();
    Tree<int64_t> t = tx.tree<int64_t>("k");
    int64_t sink = 0;
    auto t0 = Clock::now();
    for (auto c = t.cursor(); c.valid(); c.next())
        sink += c.value();
    double sec = secs(Clock::now() - t0);
    g_sink += sink;
    return {N / sec, -1, ""};
}

struct KV {
    R build, lookup;
};

static KV llbt_kv(size_t K, size_t L, size_t vsize, const std::vector<std::string>& keys, const std::vector<size_t>& which)
{
    StoreRef s = llbt_open(false);
    std::string val(vsize, 'x');
    auto lb = [](Tree<StringData>& t, StringData k) {
        size_t lo = 0, hi = t.size();
        while (lo < hi) {
            size_t m = (lo + hi) / 2;
            if (t.get(m) < k)
                lo = m + 1;
            else
                hi = m;
        }
        return lo;
    };
    auto t0 = Clock::now();
    {
        Tx tx = s->begin_write();
        Tree<StringData> tk = tx.tree<StringData>("k");
        Tree<StringData> tv = tx.tree<StringData>("v");
        for (size_t i = 0; i < K; ++i) {
            size_t p = lb(tk, StringData(keys[i]));
            tk.insert(p, StringData(keys[i]));
            tv.insert(p, StringData(val));
        }
        tx.commit();
    }
    double bsec = secs(Clock::now() - t0);
    char nb[32];
    std::snprintf(nb, sizeof nb, "%.1f MB", util::File::get_size_static(s->path()) / 1e6);

    Tx tx = s->begin_read();
    Tree<StringData> tk = tx.tree<StringData>("k");
    Tree<StringData> tv = tx.tree<StringData>("v");
    int64_t sink = 0;
    for (size_t i = 0; i < L / 10; ++i) {
        size_t p = lb(tk, StringData(keys[which[i]]));
        sink += tv.get(p).size();
    }
    auto t1 = Clock::now();
    for (size_t i = 0; i < L; ++i) {
        size_t p = lb(tk, StringData(keys[which[i]]));
        sink += tv.get(p).size();
    }
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

static R lmdb_commit(unsigned flags, size_t C, size_t B)
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
    lat.reserve(C);
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
    for (size_t c = 0; c < C; ++c)
        one(true);
    double tot = secs(Clock::now() - t0);
    return {C / tot, pctl(lat, 0.50), ""};
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

static R lmdb_scan(size_t N)
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
    auto t0 = Clock::now();
    int rc = mdb_cursor_get(cur, &k, &v, MDB_FIRST);
    while (rc == 0) {
        int64_t val;
        std::memcpy(&val, v.mv_data, sizeof val);
        sink += val;
        rc = mdb_cursor_get(cur, &k, &v, MDB_NEXT);
    }
    double sec = secs(Clock::now() - t0);
    mdb_cursor_close(cur);
    mdb_txn_abort(rt);
    g_sink += sink;
    return {N / sec, -1, ""};
}

static KV lmdb_kv(size_t K, size_t L, size_t vsize, const std::vector<std::string>& keys, const std::vector<size_t>& which)
{
    LmdbEnv e(0, size_t(1) << 30);
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
        if (mdb_get(rt, dbi, &mk, &mv) == 0)
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

static void header()
{
    std::printf("%-16s %-7s %12s %12s   %-13s  %s\n", "workload", "mode", "llbt", "lmdb", "winner", "notes");
    std::printf("------------------------------------------------------------------------------------------------\n");
}

static void cmp(const char* wl, const char* mode, const R& a, const R& b)
{
    const char* win = a.ops >= b.ops ? "llbt" : "lmdb";
    double x = a.ops >= b.ops ? a.ops / b.ops : b.ops / a.ops;
    char wbuf[24];
    std::snprintf(wbuf, sizeof wbuf, "%s %.2fx", win, x);

    std::string note;
    if (!a.note.empty())
        note = "llbt " + a.note;
    if (!b.note.empty())
        note += (note.empty() ? "" : " vs ") + std::string("lmdb ") + b.note;
    if (a.p50 >= 0 || b.p50 >= 0)
        note += (note.empty() ? "" : "  ") + std::string("p50 ") + t_str(a.p50) + "/" + t_str(b.p50);

    std::printf("%-16s %-7s %12s %12s   %-13s  %s\n", wl, mode, rate(a.ops).c_str(), rate(b.ops).c_str(), wbuf,
                note.c_str());
}

int main(int argc, char** argv)
{
    double scale = (argc > 1) ? std::atof(argv[1]) : 1.0;
    if (scale <= 0)
        scale = 1.0;
    auto S = [&](size_t base) { return size_t(base * scale); };

    g_dir = util::make_temp_dir();

    const size_t N = S(1'000'000);
    const size_t READS = S(1'000'000);
    const size_t C = S(1'000);
    const size_t KVK = S(200'000);
    const size_t KVL = S(500'000);

    std::mt19937_64 rng(12345);
    std::uniform_int_distribution<uint32_t> d(0, uint32_t(N - 1));
    std::vector<uint32_t> idx(READS);
    for (auto& x : idx)
        x = d(rng);
    std::vector<std::string> keys(KVK);
    for (auto& s : keys) {
        s.resize(16);
        for (auto& c : s)
            c = char('a' + rng() % 26);
    }
    std::vector<size_t> which(KVL);
    {
        std::uniform_int_distribution<size_t> p(0, KVK - 1);
        for (auto& x : which)
            x = p(rng);
    }

    std::printf("llbt vs LMDB %s   Apple/arm64, %u cores, scale=%.2f, Release, warm cache\n", MDB_VERSION_STRING,
                std::thread::hardware_concurrency(), scale);
    std::printf("sync: llbt=F_BARRIERFSYNC (stronger), lmdb=fsync() (weaker on macOS) — not identical durability.\n");
    std::printf("nosync rows isolate raw engine overhead (both skip the durability flush).\n\n");

    header();
    cmp("seq-write", "sync", llbt_seq_write(false, N), lmdb_seq_write(0, N));
    cmp("seq-write", "nosync", llbt_seq_write(true, N), lmdb_seq_write(MDB_NOSYNC, N));
    std::printf("\n");
    cmp("commit(b=1)", "sync", llbt_commit(false, C, 1), lmdb_commit(0, C, 1));
    cmp("commit(b=1)", "nosync", llbt_commit(true, C, 1), lmdb_commit(MDB_NOSYNC, C, 1));
    cmp("commit(b=100)", "sync", llbt_commit(false, C, 100), lmdb_commit(0, C, 100));
    cmp("commit(b=100)", "nosync", llbt_commit(true, C, 100), lmdb_commit(MDB_NOSYNC, C, 100));
    std::printf("\n");
    cmp("rand-read", "-", llbt_rand_read(N, idx), lmdb_rand_read(N, idx));
    cmp("seq-scan", "-", llbt_scan(N), lmdb_scan(N));
    std::printf("\n");
    {
        KV a = llbt_kv(KVK, KVL, 100, keys, which);
        KV b = lmdb_kv(KVK, KVL, 100, keys, which);
        cmp("kv-str-build", "sync", a.build, b.build);
        cmp("kv-str-lookup", "-", a.lookup, b.lookup);
    }

    std::printf("\n(sink=%lld)\n", (long long)g_sink);
    return 0;
}
