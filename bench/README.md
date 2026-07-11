# llbt benchmarks

This folder has three small benchmarks:

- `llbt-bench` measures llbt by itself.
- `llbt-bench-compare` compares llbt with LMDB.
- `llbt-bench-concurrency` measures reader scaling, with and without a writer.

These are microbenchmarks. They help explain engine costs, but they are not a
full application workload such as YCSB.

## Build and run

```sh
cmake -B build -DLLBT_BUILD_BENCH=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build

./build/bench/llbt-bench
./build/bench/llbt-bench-concurrency
./build/bench/llbt-bench-compare
```

Each program accepts an optional scale, for example
`./build/bench/llbt-bench-compare 4`. Use a Release build.

The LMDB comparison target is only built when LMDB is installed. On macOS:

```sh
brew install lmdb
```

## llbt-only benchmark

`llbt-bench` covers append, commit, random read, scan, random update, and a
sorted string map. It reports throughput and, for commits, p50/p95/p99
latency.

Its modes are:

- `sync`: llbt's normal commit path.
- `nosync`: no storage synchronization. This is for rebuildable data only. An
  OS crash or power loss can lose commits or leave the file unusable.
- `memory`: an anonymous in-memory store.

The standalone rows use default cross-process locking. Setting
`Options::single_process = true` removes file-lock syscalls on platforms that
emulate robust process mutexes. It is only safe when no other process opens
the file.

## What the LMDB comparison actually measures

`llbt-bench-compare` now runs three fresh-database samples, alternates which
engine runs first, and reports the median. Commit samples run for at least
0.35 seconds. String lookups verify the returned key and value.

Not every row is a fair head-to-head test:

1. **Native sync is different on macOS.** llbt uses `F_BARRIERFSYNC`, which
   orders writes. Stock LMDB 0.9.35 uses `F_FULLFSYNC`, which asks for stronger
   persistence before return. Those rows are shown but have no winner.
2. **Dense integer rows use different data models.** llbt stores one value and
   uses its position as the key. LMDB stores and searches an explicit 8-byte
   key plus an 8-byte value. Append, commit, read, scan, and file-size results
   describe each engine's native API; they are not equivalent KV work.
3. **The lock tuning is similar in purpose, not identical.** llbt uses
   `single_process` process-local mutexes. LMDB uses `MDB_NOLOCK`, so the
   caller is responsible for serialization.
4. **The string rows are the closest matched workload.** Both engines build
   and query the same logical random-order map: unique 16-byte keys and
   100-byte values. Both use no-flush mode for the timed build.

## Verified comparison

Apple M1, macOS 26.5, APFS, LMDB 0.9.35, Release, scale 1. These are medians
of three alternating-order samples from the corrected harness. Your numbers
will differ.

```text
workload         mode              llbt         lmdb   result
seq-write        native       10.78 M/s    12.09 M/s   not comparable
seq-write        no-flush     37.98 M/s    12.18 M/s   native APIs

commit(b=1)      native         1.3 k/s       214 /s   not comparable
commit(b=1)      no-flush     275.8 k/s    214.3 k/s   native APIs
commit(b=100)    native          446 /s       199 /s   not comparable
commit(b=100)    no-flush      62.2 k/s     40.4 k/s   native APIs

rand-read        native       50.15 M/s     1.89 M/s   native APIs
seq-scan         native      701.42 M/s   144.60 M/s   native APIs

kv-str-build     no-flush     884.3 k/s     1.38 M/s   LMDB 1.56x
kv-str-lookup    -             1.29 M/s     2.00 M/s   LMDB 1.55x
```

The honest conclusion is narrower than “llbt beats LMDB.” llbt is very fast
and compact for its native positional sequence. In the matched string KV
workload, LMDB won both random-order build and lookup in this run. Native sync
cannot be ranked until both engines use the same durability guarantee.

The string map also does more than one tree walk per successful llbt lookup:
`lower_bound()` finds a position, key equality is checked, and `get()` reads
the record. The current B+tree has no stored fence keys in inner nodes, so it
should not be described as a single keyed descent.

## Reader scaling

`llbt-bench-concurrency` opens one immutable snapshot per reader and reports
aggregate reads per second for 1, 2, 4, and up to the machine's hardware
thread count. It repeats each case three times and uses the median.

The writer case updates a fixed 4,096-value tree in small commits. It does not
grow an unbounded tree across samples. The `vs alone` column is the share of
reader throughput retained while that writer runs.

## Group commit

`llbt-bench-group-commit` measures write throughput for T threads doing tiny
durable transactions (set one slot of a 4,096-value tree), two ways: `plain`
opens and commits its own transaction per write (one storage sync each);
`grouped` goes through `Store::write()`, which coalesces concurrent writers
into one physical commit per batch. `writes/commit` is the achieved batching
factor, counted from the store's version delta.

Representative run (Apple Silicon, APFS, 400 ms windows, median of 3):

```
overhead check (nosync, 1 thread): plain 317.5 k/s | grouped 312.0 k/s (98%)

threads     plain wr/s   grouped wr/s   speedup  writes/commit
--------------------------------------------------------------
1              1.1 k/s        1.0 k/s     0.97x            1.0
2              1.1 k/s        1.8 k/s     1.61x            1.5
4              1.0 k/s        2.6 k/s     2.49x            2.5
8              1.0 k/s        4.0 k/s     3.96x            5.1
```

The reading: plain writers stay flat as threads grow — they serialize on the
write lock and each pays a full durable sync. Grouped writers scale with the
batch size because the whole batch shares one sync. A lone writer loses
nothing (0.97x durable, 98% nosync), so `write()` is safe to use as the
default write path.

## Caveats

- One machine and one filesystem are not a product-wide result.
- CPU load, thermals, filesystem state, and OS updates move the numbers.
- LMDB uses its default non-`MDB_WRITEMAP` write path here.
- The comparison still lacks a matched explicit integer-key map for llbt.
- A real decision should also test your value sizes, read/write mix,
  concurrency, database size, and crash requirements.
