// MVCC on the core API: a reader's snapshot stays frozen while a writer
// commits; fresh readers see the new version.
#include <llbt/api.hpp>
#include <cstdio>
#include <thread>

using namespace llbt;
using namespace llbt::core;

int main()
{
    std::string path = util::File::resolve("llbt_example_mvcc.llbt", util::make_temp_dir());
    StoreRef store = Store::open(path);

    {
        Tx tx = store->begin_write();
        Tree<int64_t> t = tx.tree<int64_t>("numbers");
        for (int64_t i = 0; i < 100; ++i)
            t.add(i);
        tx.commit();
    }

    Tx snapshot = store->begin_read(); // pins the 100-row version

    std::thread writer([&] {
        Tx tx = store->begin_write();
        Tree<int64_t> t = tx.tree<int64_t>("numbers");
        for (int64_t i = 100; i < 200; ++i)
            t.add(i);
        tx.commit();
    });
    writer.join();

    size_t pinned = snapshot.tree<int64_t>("numbers").size();
    size_t fresh = store->begin_read().tree<int64_t>("numbers").size();
    std::printf("pinned snapshot: %zu rows, fresh read: %zu rows\n", pinned, fresh);

    bool ok = pinned == 100 && fresh == 200;
    util::File::remove(path);
    std::printf(ok ? "ok\n" : "FAIL\n");
    return ok ? 0 : 1;
}
