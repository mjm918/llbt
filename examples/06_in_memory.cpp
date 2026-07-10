// An in-memory store: the same crash-safe page engine, but with no file
// behind it. Nothing is written to disk, and everything is freed when the
// StoreRef drops. Good for tests, caches, and scratch computations.
//
// The ONLY change from a file-backed store is the open call:
//     Store::open(path)       -> durable, on disk
//     Store::open_in_memory() -> ephemeral, in RAM
// Every other line — Tx, trees, cursors, named roots — is identical.
#include <llbt/api.hpp>
#include <cstdio>
#include <string>

using namespace llbt;
using namespace llbt::core;

int main()
{
    StoreRef store = Store::open_in_memory();
    std::printf("in_memory=%s path=%s\n",
                store->is_in_memory() ? "yes" : "no", store->path().c_str());

    // Build a small scoreboard in memory, two parallel trees (name -> score).
    {
        Tx tx = store->begin_write();
        Tree<StringData> names = tx.tree<StringData>("names");
        Tree<int64_t> scores = tx.tree<int64_t>("scores");
        auto put = [&](StringData who, int64_t score) {
            names.add(who);
            scores.add(score);
        };
        put("ada", 90);
        put("bao", 75);
        put("cira", 88);
        tx.commit();
    }

    bool ok = true;

    // Read + query on an immutable snapshot, exactly as with a file store.
    {
        Tx tx = store->begin_read();
        Tree<StringData> names = tx.tree<StringData>("names");
        Tree<int64_t> scores = tx.tree<int64_t>("scores");

        std::printf("-- scoreboard --\n");
        int64_t total = 0;
        for (auto cur = names.cursor(); cur.valid(); cur.next()) {
            int64_t s = scores.get(cur.pos());
            total += s;
            std::printf("%-6s %lld\n", std::string(cur.value()).c_str(), (long long)s);
        }
        ok = ok && names.size() == 3 && total == 253;

        size_t i = names.find_first("cira");
        ok = ok && i != Tx::npos && scores.get(i) == 88;
        std::printf("cira -> %lld\n", i == Tx::npos ? -1 : (long long)scores.get(i));
    }

    // Proof it never touched disk: no file exists at the store's path label.
    ok = ok && store->is_in_memory() && !util::File::exists(store->path());

    // When `store` drops here the whole thing is gone — no file to clean up.
    std::printf(ok ? "ok\n" : "FAIL\n");
    return ok ? 0 : 1;
}
