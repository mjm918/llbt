// A crash-safe ordered KV store on the llbt core API: two parallel
// durable trees (keys kept sorted by us — our data structure, our rules),
// binary search for lookup, a cursor for range walks, durable commits.
#include <llbt/api.hpp>
#include <cstdio>
#include <string>

using namespace llbt;
using namespace llbt::core;

// lower_bound over a sorted Tree<StringData>
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

static void put(Tx& tx, StringData k, StringData v)
{
    Tree<StringData> keys = tx.tree<StringData>("kv.keys");
    Tree<StringData> vals = tx.tree<StringData>("kv.vals");
    size_t i = lower_bound(keys, k);
    if (i < keys.size() && keys.get(i) == k) {
        vals.set(i, v);
    }
    else {
        keys.insert(i, k);
        vals.insert(i, v);
    }
}

int main()
{
    std::string path = util::File::resolve("llbt_example_kv.llbt", util::make_temp_dir());
    StoreRef store = Store::open(path);

    // All-or-nothing write; durable after commit().
    {
        Tx tx = store->begin_write();
        put(tx, "banana", "yellow");
        put(tx, "apple", "green");
        put(tx, "cherry", "red");
        put(tx, "apple", "red"); // overwrite
        tx.commit();
    }

    // Snapshot read: point lookup + ordered range walk with a cursor.
    {
        Tx tx = store->begin_read();
        Tree<StringData> keys = tx.tree<StringData>("kv.keys");
        Tree<StringData> vals = tx.tree<StringData>("kv.vals");

        size_t i = lower_bound(keys, "apple");
        std::printf("apple = %s\n", std::string(vals.get(i)).c_str());

        for (auto cur = keys.cursor(); cur.valid(); cur.next())
            std::printf("%s -> %s\n", std::string(cur.value()).c_str(),
                        std::string(vals.get(cur.pos())).c_str());
    }

    util::File::remove(path);
    std::printf("ok\n");
    return 0;
}
