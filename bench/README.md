# llbt benchmarks

A small, honest microbenchmark for the `llbt::core` store. It measures the
operations that matter for an embedded copy-on-write page store, across the
dimensions that actually change the numbers.

## How this kind of code is benchmarked

Embedded storage engines (LMDB, bolt/bbolt, BerkeleyDB, RocksDB/LevelDB,
WiredTiger, and llbt's own ancestor Realm) are all benchmarked the same way.
This harness follows that playbook:

- **Operations** — the primitives, in isolation: bulk/sequential write,
  batched commit, random read, sequential scan, random update, and a realistic
  sorted-key KV workload. (LMDB's `microbench`, bolt's `bench`, RocksDB's
  `db_bench --benchmarks=fillseq,fillrandom,readrandom,readseq,...`.)
- **Dimensions** — the knobs that dominate:
  - **durability mode**: `sync` (crash-safe, fsync on commit), `nosync`
    (skip fsync — fast, file stays consistent but the last commits can be lost
    on power failure), `memory` (no file at all).
  - **batch size**: rows per commit. Commit cost is a fixed per-commit tax
    (an fsync in `sync` mode); batching amortizes it.
  - value/key size and dataset size are fixed here but scale with the `scale`
    argument.
- **Metrics** — throughput (ops/sec), average latency, and **p50/p95/p99**
  on the commit path (where tail latency is the whole story). Plus file size
  and bytes/entry for space.
- **Rigor** — fixed RNG seed, a warm-up pass before every timed section, a
  monotonic clock, and a `volatile` sink so reads can't be optimized away.

There is a **cross-engine baseline** vs LMDB (`bench_compare.cpp`, see below) —
the thing that makes absolute numbers mean something. One standard next step
remains: **concurrency / YCSB-style mixes** (N reader threads + 1 writer, or a
read/write ratio like YCSB workloads A–F). llbt is single-writer, many-reader;
a reader-scaling curve is the natural concurrency test.

## Build and run

```sh
cmake -B build -DLLBT_BUILD_BENCH=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --target llbt-bench
./build/bench/llbt-bench          # default scale
./build/bench/llbt-bench 4        # 4x the dataset/op counts
```

Release matters — a debug build measures the debug build, not the engine.

## Reading the output

Each row is one `(benchmark, mode)`. `ops/s` and `avg` are throughput and
average latency; `p50/p95/p99` are per-commit latency (commit rows only).
For `commit(batch=B)`, `ops` counts **commits**, so durable rows/sec is
`ops/s × B` — that's the point of the batch rows: they show the amortization.

## Representative results

Apple M1 (8 core), 16 GB, macOS 26.5, APFS SSD. `scale=1` → 1M-row dataset.
Release. Warm cache, single process. **Your numbers will differ** — treat
these as relative, not absolute.

```
benchmark              mode            ops       ops/s        avg        p50        p95        p99
bulk-load(append)      sync        1000000   10.18 M/s      98 ns
bulk-load(append)      nosync      1000000   30.07 M/s      33 ns
bulk-load(append)      memory      1000000   40.17 M/s      25 ns

commit(batch=1)        sync           1000     1.2 k/s  819.45 us  758.04 us    1.51 ms    1.79 ms
commit(batch=1)        nosync         1000    17.4 k/s   57.59 us   55.79 us   76.58 us   79.38 us
commit(batch=1)        memory         1000   102.2 k/s    9.79 us    9.67 us    9.88 us   10.08 us
commit(batch=100)      sync           1000     2.1 k/s  476.42 us  390.58 us  778.50 us    1.61 ms
commit(batch=100)      nosync         1000    14.7 k/s   67.89 us
commit(batch=100)      memory         1000    77.3 k/s   12.93 us

rand-read(get)         memory      1000000   52.22 M/s      19 ns
rand-read(get)         sync        1000000   43.29 M/s      23 ns
seq-scan(cursor)       memory      1000000  782.40 M/s       1 ns
seq-scan(cursor)       sync        1000000  581.66 M/s       2 ns

rand-update(set)       sync         500000   188.5 k/s    5.30 us   (batch=1000)
rand-update(set)       nosync       500000   290.8 k/s    3.44 us
rand-update(set)       memory       500000    2.52 M/s     396 ns

kv-string-build        sync         200000   171.9 k/s    5.82 us   (27.3 MB, 136 B/entry)
kv-string-lookup       sync         500000   916.7 k/s    1.09 us   (16B key, 100B val)
```

## What the numbers say

- **Durable commits are fsync-bound.** One crash-safe commit ≈ 800 µs
  (~1.2k/s) — that is the price of the fsync, not of the work inside the
  commit. This is the single most important number and it's a property of the
  disk, not the code.
- **Batching is the lever.** `batch=1` sync ≈ 1.2k rows/s durable;
  `batch=100` sync ≈ 210k rows/s (2.1k commits × 100). Same durability,
  ~170× the throughput. Write in transactions, not per row.
- **`nosync` and `memory` trade durability for speed** — ~15k and ~100k
  commits/s. Right for caches, scratch data, tests, and rebuildable indexes.
- **Reads run at memory-map speed** — ~45M random gets/s (~20 ns each),
  ~600M/s sequential scan. A warm file is within ~15% of pure memory.
- **Updates are copy-on-write**, so they cost more than reads (~5 µs each in
  `sync`, batched), and `memory` is ~13× faster with no disk in the loop.

## Side by side vs LMDB

LMDB is the truest peer — an embedded, memory-mapped, copy-on-write B+tree with
MVCC, same architecture as llbt. `bench_compare.cpp` runs the identical
workloads through both. Install LMDB (`brew install lmdb` /
`apt install liblmdb-dev`); CMake finds it and builds the extra target:

```sh
./build/bench/llbt-bench-compare        # or `... 4` for 4x scale
```

Two fairness notes baked into the harness output:
1. **`sync` durability is not identical.** llbt commits with Apple's
   `F_BARRIERFSYNC` (an ordered barrier — crash-consistent); LMDB uses
   `fsync()`, which on macOS does *not* flush the drive cache. llbt's `sync` is
   the stronger, and here also the faster, guarantee. The `nosync` rows (both
   skip the flush) isolate raw engine overhead.
2. **Data models differ.** llbt `Tree<T>` is a positional sequence (index *is*
   the key); LMDB is a sorted key→value map. Dense integer keys coincide; for
   string keys llbt has no native sorted map, so it builds one by hand.

Representative, same machine as above, `scale=1` (1M rows):

```
workload         mode          llbt         lmdb    winner       notes
seq-write        sync      11.58 M/s     8.37 M/s   llbt 1.4x    4.2 MB vs 26.1 MB
seq-write        nosync    33.61 M/s     8.57 M/s   llbt 3.9x
commit(b=1)      sync         1.4 k/s       252 /s  llbt 5.6x    p50 534us vs 3976us
commit(b=1)      nosync      14.0 k/s    204.6 k/s   lmdb 15x    p50 66us vs 5us
commit(b=100)    sync         1.9 k/s       224 /s  llbt 8.5x
commit(b=100)    nosync      10.2 k/s     48.2 k/s  lmdb 4.8x
rand-read        -         43.93 M/s     1.66 M/s   llbt 26x     (see below)
seq-scan         -        489.5  M/s   116.0  M/s   llbt 4.2x
kv-str-build     sync      170.4 k/s     1.03 M/s   lmdb 6.1x    27.3 MB vs 36.5 MB
kv-str-lookup    -         930.7 k/s     1.87 M/s   lmdb 2.0x
```

**What it says — llbt is faster on its natural workloads, LMDB on keyed maps:**

- **Durable commits:** llbt wins 5–8× *and* with a stronger guarantee — its
  barrier-fsync is cheaper than LMDB's full fsync on this OS.
- **Space:** llbt is ~6× smaller for int64 (packed values vs key+value+page
  overhead). That is not just a disk win: the whole llbt tree (~8 MB) fits in
  the M1's L2 cache while LMDB's 26 MB does not — which is *why* llbt's random
  read is 26× faster. Compactness buys cache residency buys read speed.
- **Scans** win 4× (packed leaves vs key/value `MDB_val` pairs).
- **LMDB wins** the bare per-commit overhead (`nosync`, ~15× — Realm's commit
  machinery has more fixed cost) and **string-key maps** (build 6×, lookup 2× —
  llbt has no native sorted map; you assemble it, LMDB does it in one descent).

Bottom line: for positional / append / scan / dense-integer / space-sensitive
work, llbt is the faster engine here; for arbitrary-key sorted maps, LMDB's
native model wins. Pick by workload.

## Caveats

Single machine, single filesystem, warm cache, one process. fsync cost is
wildly OS/FS/hardware dependent (a spinning disk, or `F_FULLFSYNC`, would be far
slower than this APFS SSD). LMDB numbers use its defaults (no `MDB_WRITEMAP`);
its `sync` uses `fsync()` which is weaker than llbt's barrier on macOS, so the
`sync` rows are not a like-for-like durability comparison — read `nosync` for
engine overhead. Use this to compare **engine vs engine**, **mode vs mode**,
and **batch vs batch** on your own hardware, not as absolute specs.
