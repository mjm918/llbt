---
name: verify
description: How to verify llbt changes end-to-end — build standalone programs against the public surface, crash-test durability, and count real sync syscalls. Use when verifying storage-engine changes at runtime instead of via unit tests.
---

# Verifying llbt at runtime

llbt is a library; its surface is the public headers + `libllbt-core.a`.
Verify by building small standalone programs against that surface and running
them as separate processes — never by re-running the unit tests.

## Build a standalone program against the lib

```sh
R=<repo root>   # must have a configured build/ (Release) with libllbt-core.a
c++ -std=c++17 -O2 -I$R/src -I$R/include -I$R/build/generated \
    prog.cpp $R/build/libllbt-core.a \
    -framework Foundation -framework CoreFoundation -lpthread
```

Pure-C programs against `<llbt/c_api.h>` need only `-I$R/include` to compile,
but link with `c++` (same libs) because the .a is C++.

## Durability / crash protocol (the one that matters)

Writer process: after each committed write, append the value to an ack file
(`fprintf` + `fflush`). Parent: `kill -9` at a random moment, then a FRESH
process opens the store and asserts every acked value is present (extras =
committed-but-unacked are fine). Repeat ~8 rounds. Atomicity: have each txn
write two trees that must stay identical (size/sum/xor) and check both.
Cross-check contents through the C API — it's an independent read path.
Note: kill -9 proves process-crash durability and commit-before-ack ordering;
true power-loss needs a VM or hardware rig.

## Counting real syncs (bench verification)

`DYLD_INSERT_LIBRARIES` a dylib that interposes `msync`, `fsync`, and
`fcntl` (count `F_FULLFSYNC`/`F_BARRIERFSYNC` cmds) via the
`__DATA,__interpose` section; export a `synccount_snapshot(long[4])` the
driver reads via `dlsym` around the measured window. As of 2026-07, a durable
commit on macOS = 3 msync + 2 F_BARRIERFSYNC (~1 ms on APFS).

## Gotchas

- The Bash tool shell is zsh: unquoted `$VAR` does NOT word-split. Put
  multi-flag variables in a `#!/bin/sh` script, or inline them.
- `cmake --build build` fails if cwd is already `build/` — use absolute paths.
- Ready-made harness sources (gc_writer/gc_check/gc_capi_check/synccount/
  gc_synccount) existed in the 2026-07 session scratchpad; rebuild from this
  recipe if gone.
