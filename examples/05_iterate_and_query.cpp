// Iterating and querying data in llbt. There is no query engine — and none
// needed: point lookups, range scans, and filters compose from two
// primitives you already have, cursors and sorted trees.
//
//   * full scan        cursor from first() to !valid()
//   * point lookup     binary search over a sorted tree (lower_bound)
//   * range scan       lower_bound(from) .. lower_bound(to) slice
//   * predicate filter walk values, keep what matches
//   * exact match      Tree<T>::find_first(value)
#include <llbt/api.hpp>
#include <cstdio>
#include <string>

using namespace llbt;
using namespace llbt::core;

// First position whose key is >= k, over a tree we keep sorted ourselves.
static size_t lower_bound(const Tree<StringData>& keys, StringData k)
{
    size_t lo = 0, hi = keys.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (keys.get(mid) < k)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

// Sorted insert into two parallel trees (name -> population).
static void put(Tree<StringData>& names, Tree<int64_t>& pops, StringData city, int64_t pop)
{
    size_t i = lower_bound(names, city);
    if (i < names.size() && names.get(i) == city) {
        pops.set(i, pop);
    }
    else {
        names.insert(i, city);
        pops.insert(i, pop);
    }
}

int main()
{
    std::string path = util::File::resolve("llbt_example_query.llbt", util::make_temp_dir());
    StoreRef store = Store::open(path);

    // Seed: city -> population (millions), kept sorted by name.
    {
        Tx tx = store->begin_write();
        Tree<StringData> names = tx.tree<StringData>("cities.names");
        Tree<int64_t> pops = tx.tree<int64_t>("cities.pop");
        put(names, pops, "Tokyo", 37);
        put(names, pops, "Delhi", 33);
        put(names, pops, "Shanghai", 29);
        put(names, pops, "Dhaka", 23);
        put(names, pops, "Cairo", 22);
        put(names, pops, "Lagos", 16);
        put(names, pops, "Karachi", 17);
        put(names, pops, "Kinshasa", 16);
        tx.commit();
    }

    // All queries below run on one immutable snapshot.
    Tx tx = store->begin_read();
    Tree<StringData> names = tx.tree<StringData>("cities.names");
    Tree<int64_t> pops = tx.tree<int64_t>("cities.pop");
    bool ok = true;

    // 1) Full scan: iterate everything in name order.
    std::printf("-- all cities --\n");
    for (auto cur = names.cursor(); cur.valid(); cur.next())
        std::printf("%-10s %lld M\n", std::string(cur.value()).c_str(),
                    (long long)pops.get(cur.pos()));

    // 2) Point lookup: binary search, O(log n) tree probes.
    size_t i = lower_bound(names, "Lagos");
    bool found = i < names.size() && names.get(i) == "Lagos";
    std::printf("-- point lookup: Lagos = %lld M\n", found ? (long long)pops.get(i) : -1);
    ok = ok && found && pops.get(i) == 16;

    // 3) Range scan: every city in [K, M) — seek once, walk the slice.
    std::printf("-- range [K, M) --\n");
    std::string in_range;
    auto cur = names.cursor();
    for (cur.seek(lower_bound(names, "K")); cur.valid() && cur.value() < "M"; cur.next()) {
        std::printf("%s\n", std::string(cur.value()).c_str());
        in_range += std::string(cur.value()) + ",";
    }
    ok = ok && in_range == "Karachi,Kinshasa,Lagos,";

    // 4) Predicate filter: cities with population above 20 M.
    std::printf("-- filter: pop > 20 M --\n");
    size_t matches = 0;
    for (auto pc = pops.cursor(); pc.valid(); pc.next()) {
        if (pc.value() > 20) {
            std::printf("%s (%lld M)\n", std::string(names.get(pc.pos())).c_str(),
                        (long long)pc.value());
            ++matches;
        }
    }
    ok = ok && matches == 5; // Tokyo, Delhi, Shanghai, Dhaka, Cairo

    // 5) Exact-match search on values: who has exactly 33 M?
    size_t hit = pops.find_first(33);
    std::printf("-- find_first(33) -> %s\n",
                hit == Tx::npos ? "none" : std::string(names.get(hit)).c_str());
    ok = ok && hit != Tx::npos && names.get(hit) == "Delhi";

    util::File::remove(path);
    std::printf(ok ? "ok\n" : "FAIL\n");
    return ok ? 0 : 1;
}
