// Bring your own data structure: assemble a two-level structure from raw
// copy-on-write page nodes — an index array pointing at per-bucket blob
// arrays — anchor it under a named root, and read it back after "restart".
// No Tree<T> involved; these are the same primitives llbt itself is built on.
#include <llbt/api.hpp>
#include <cstdio>
#include <string>

using namespace llbt;
using namespace llbt::core;

int main()
{
    std::string path = util::File::resolve("llbt_example_byo.llbt", util::make_temp_dir());

    {
        StoreRef store = Store::open(path);
        Tx tx = store->begin_write();
        Allocator& alloc = tx.alloc();

        // Two leaf arrays of binary blobs...
        ArrayBinary bucket_a(alloc);
        bucket_a.create();
        bucket_a.add(BinaryData("alpha", 5));
        bucket_a.add(BinaryData("amber", 5));

        ArrayBinary bucket_b(alloc);
        bucket_b.create();
        bucket_b.add(BinaryData("bravo", 5));

        // ...stitched together by a parent node holding refs (COW-tracked).
        Array index(alloc);
        index.create(NodeHeader::type_HasRefs);
        index.add(RefOrTagged::make_ref(bucket_a.get_ref()));
        index.add(RefOrTagged::make_ref(bucket_b.get_ref()));

        tx.set_root("buckets", index.get_ref());
        tx.commit(); // durable: crash after this point loses nothing
    }

    {
        StoreRef store = Store::open(path); // fresh handle, as after restart
        Tx tx = store->begin_read();
        Array index(tx.alloc());
        index.init_from_ref(*tx.get_root("buckets"));

        for (size_t b = 0; b < index.size(); ++b) {
            ArrayBinary bucket(tx.alloc());
            bucket.init_from_ref(index.get_as_ref(b));
            for (size_t i = 0; i < bucket.size(); ++i) {
                BinaryData blob = bucket.get(i);
                std::printf("bucket %zu: %.*s\n", b, int(blob.size()), blob.data());
            }
        }
    }

    util::File::remove(path);
    std::printf("ok\n");
    return 0;
}
