# llbt — Low Level Binary Tree

**A single-file embedded page store for C++ — not limited to KV.** One file, memory-mapped,
copy-on-write pages, crash-safe commits, single writer with MVCC snapshot
readers, B+tree cursors. You bring the data structure; llbt gives you the
hard parts: raw file access, durable pages, and iteration. Nothing else —
no tables, no query engine, no schema.

llbt is not written from scratch. It is the storage layer of
[Barq](https://github.com/BarqDB/barq-core) (a fork of Realm Core) extracted
into a standalone library, imported line by line with its original test
suite — **174 tests, ~46 million checks** green on every commit. Code that
has already survived production on millions of devices, cut down to the raw
essentials (~55k lines).

## The API in 30 seconds

```cpp
#include <llbt/api.hpp>
using namespace llbt::core;

StoreRef store = Store::open("data.llbt");   // durable, on disk
// or: Store::open_in_memory()               // ephemeral — nothing hits disk

// exclusive writer: all-or-nothing, durable at commit()
{
    Tx tx = store->begin_write();
    Tree<int64_t> scores = tx.tree<int64_t>("scores");   // named root
    scores.add(42);
    scores.insert(0, 7);
    tx.commit();
}

// readers get frozen snapshots; they never block, writers never wait on them
{
    Tx tx = store->begin_read();
    Tree<int64_t> scores = tx.tree<int64_t>("scores");
    for (auto cur = scores.cursor(); cur.valid(); cur.next())
        use(cur.value());
}
```

**Bring your own data structure.** Named roots anchor *anything* built from
the engine's copy-on-write page nodes, not just `Tree<T>`:

```cpp
Tx tx = store->begin_write();
ArrayBinary blobs(tx.alloc());     // raw COW page nodes
blobs.create();
blobs.add(BinaryData("payload", 7));
tx.set_root("my-blobs", blobs.get_ref());   // now durable + crash-safe
tx.commit();
```

## What's in the box

| Layer | Types | What it does |
|---|---|---|
| Files | `util::File`, `File::Map<T>` | open/resize/lock/mmap, cross-platform, optional AES page encryption |
| Store | `core::Store`, `core::Tx` | crash-safe copy-on-write commits, MVCC snapshots, compaction — on a file or fully in-memory |
| Roots | `tx.tree<T>()`, `tx.set_root()` | durable named anchors — the bucket directory |
| Trees | `core::Tree<T>`, `core::Cursor<T>` | B+tree sequences of int64/float/double/string/binary/timestamp |
| Raw | `tx.alloc()`, `Array`, `BPlusTree<T>` | page-level building blocks for your own structures |
| C API | `<llbt/c_api.h>` | a pure-C `extern "C"` surface over Store/Tx/Tree/Cursor — for FFI and other languages |

## From C

The store, transactions, typed trees and cursors are all reachable from plain
C through `<llbt/c_api.h>`: opaque handles, a small tagged-union value type,
and status codes instead of exceptions.

```c
#include <llbt/c_api.h>

llbt_store* store;
llbt_store_open_in_memory(&store);

llbt_txn* tx;
llbt_txn_begin_write(store, &tx);
llbt_tree* scores;
llbt_tree_open(tx, "scores", LLBT_TYPE_INT64, &scores);
llbt_value v = llbt_int64(42);
llbt_tree_add(scores, &v);
llbt_tree_destroy(scores);
llbt_txn_commit(tx, NULL);          /* frees tx */

llbt_store_close(store);
```

Every fallible call returns `llbt_status` (0 == `LLBT_OK`); `llbt_last_error()`
carries the message. Full walk-through in [examples/07_c_api.c](examples/07_c_api.c).

## Build

```sh
cmake -B build -DLLBT_BUILD_EXAMPLES=ON
cmake --build build --parallel
ctest --test-dir build            # the imported upstream test suite
```

CMake ≥ 3.20, C++17, no external dependencies by default. Options:
`LLBT_ENABLE_ENCRYPTION` (AES pages; CommonCrypto/OpenSSL),
`LLBT_ENABLE_ASSERTIONS`.

FetchContent:

```cmake
FetchContent_Declare(llbt GIT_REPOSITORY https://github.com/mjm918/llbt.git GIT_TAG v3.0.0)
FetchContent_MakeAvailable(llbt)
target_link_libraries(your_target PRIVATE llbt::core)
```

Runnable examples in [examples/](examples/): files+mmap, an ordered KV store
with cursors, MVCC concurrent readers, a hand-built structure from raw page
nodes, iterating/querying data (point lookups, range scans, filters), an
in-memory store with no file behind it, and the whole thing driven from plain C.

## Semantics worth knowing

- One write Tx at a time; readers are wait-free snapshots.
- An uncommitted write Tx rolls back when it goes out of scope (RAII).
- Tree handles and refs are valid only within their Tx.
- `set_root` / `erase_root` free the structure they replace — space returns
  to the file's free list; `Store::compact()` hands it back to the OS.
- Typed roots remember their element type; opening with the wrong type throws.

## Provenance and license

Apache License 2.0. llbt contains code derived from Barq Core, which is a
modified fork of Realm Core; original copyright headers are retained. See
[NOTICE](NOTICE) and [THIRD-PARTY-NOTICES](THIRD-PARTY-NOTICES).

Files authored by the llbt project itself (the core API, examples, tooling)
are released into the public domain: to the extent possible under law, all
copyright and related rights are waived, so anyone may use, modify, fork, or
build on them for any purpose, with no conditions and no warranty.
