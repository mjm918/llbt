/*
** llbt — Low Level Binary Tree
**
** The files authored by the llbt project (such as this one) are dedicated
** to the public domain. To the extent possible under law, all copyright and
** related rights are waived. You may use, copy, modify, merge, publish,
** distribute, sublicense, sell, fork, or build upon them for any purpose,
** commercial or not, with or without attribution, with no conditions and no
** warranty.
**
** This applies only to files authored by the llbt project. The imported
** storage engine underneath keeps its own copyright and the Apache License
** 2.0; see LICENSE and NOTICE.
*/
#ifndef LLBT_C_API_H
#define LLBT_C_API_H

/*
 * A pure C surface over llbt::core — the single-file embedded page store.
 *
 * Design in one paragraph: every fallible call returns an `llbt_status`
 * (0 == LLBT_OK) and writes its result through an out-parameter. On a
 * non-OK return, `llbt_last_error()` gives a human-readable message for the
 * failing call on the current thread. Handles are opaque and freed with
 * their matching *_close / *_destroy / commit / abort call. Values move
 * through a small tagged union (`llbt_value`); strings and binaries are
 * (pointer,length) pairs, never assumed NUL-terminated. Data returned by a
 * read (a get / cursor value) points into the transaction's mapped memory
 * and stays valid until the transaction ends or the tree is modified — copy
 * it if you need it longer.
 *
 * Lifetime rules mirror the C++ API:
 *   - one write transaction at a time; readers are wait-free snapshots;
 *   - a tree/cursor handle is valid only inside the transaction it came from
 *     and must be destroyed before that transaction ends;
 *   - committing or aborting a transaction frees the transaction handle.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ status */

typedef enum llbt_status {
    LLBT_OK = 0,
    LLBT_E_INVALID_ARG = 1,     /* a NULL/blatantly wrong argument           */
    LLBT_E_TYPE_MISMATCH = 2,   /* value type != the tree's element type     */
    LLBT_E_OUT_OF_BOUNDS = 3,   /* index outside the tree                     */
    LLBT_E_NOT_FOUND = 4,       /* named root / entry absent                  */
    LLBT_E_WRONG_TXN_STATE = 5, /* e.g. writing through a read transaction    */
    LLBT_E_ILLEGAL_OP = 6,      /* e.g. reopening a root at a different type  */
    LLBT_E_IO = 7,              /* file access / OS error                     */
    LLBT_E_UNKNOWN = 8          /* anything else (see llbt_last_error)        */
} llbt_status;

/* ------------------------------------------------------------------- types */

typedef enum llbt_type {
    LLBT_TYPE_INT64 = 1,
    LLBT_TYPE_FLOAT = 2,
    LLBT_TYPE_DOUBLE = 3,
    LLBT_TYPE_STRING = 4,
    LLBT_TYPE_BINARY = 5,
    LLBT_TYPE_TIMESTAMP = 6
} llbt_type;

/* "no position" — returned by find_first / lower_bound with no match, and the
 * value lower_bound yields when every element compares less than the key. */
#define LLBT_NPOS ((size_t)-1)

typedef struct llbt_value {
    llbt_type type;
    union {
        int64_t i64;
        float f32;
        double f64;
        /* STRING or BINARY. `data` may be NULL to mean a null value; a
         * non-NULL `data` with size 0 is an empty (non-null) value. */
        struct {
            const char* data;
            size_t size;
        } bytes;
        struct {
            int64_t seconds;
            int32_t nanoseconds;
        } ts;
    } as;
} llbt_value;

/* Convenience value builders (header-only, usable from C and C++). */
static inline llbt_value llbt_int64(int64_t v)
{
    llbt_value x;
    x.type = LLBT_TYPE_INT64;
    x.as.i64 = v;
    return x;
}
static inline llbt_value llbt_float(float v)
{
    llbt_value x;
    x.type = LLBT_TYPE_FLOAT;
    x.as.f32 = v;
    return x;
}
static inline llbt_value llbt_double(double v)
{
    llbt_value x;
    x.type = LLBT_TYPE_DOUBLE;
    x.as.f64 = v;
    return x;
}
static inline llbt_value llbt_string(const char* data, size_t size)
{
    llbt_value x;
    x.type = LLBT_TYPE_STRING;
    x.as.bytes.data = data;
    x.as.bytes.size = size;
    return x;
}
static inline llbt_value llbt_binary(const void* data, size_t size)
{
    llbt_value x;
    x.type = LLBT_TYPE_BINARY;
    x.as.bytes.data = (const char*)data;
    x.as.bytes.size = size;
    return x;
}
static inline llbt_value llbt_timestamp(int64_t seconds, int32_t nanoseconds)
{
    llbt_value x;
    x.type = LLBT_TYPE_TIMESTAMP;
    x.as.ts.seconds = seconds;
    x.as.ts.nanoseconds = nanoseconds;
    return x;
}

/* ----------------------------------------------------------------- handles */

typedef struct llbt_store llbt_store;
typedef struct llbt_txn llbt_txn;
typedef struct llbt_tree llbt_tree;
typedef struct llbt_cursor llbt_cursor;

typedef struct llbt_options {
    const char* encryption_key; /* 64 bytes, or NULL. Needs LLBT_ENABLE_ENCRYPTION. */
    int no_sync;                /* if non-zero, commits skip fsync (rebuildable data only) */
    int single_process;         /* if non-zero, promise single-process access (faster) */
} llbt_options;

/* ----------------------------------------------------------------- library */

/* Version string, e.g. "3.0.0". Never NULL, statically owned. */
const char* llbt_version(void);
/* Stable name for a status code, e.g. "LLBT_ERR_IO". Never NULL. */
const char* llbt_status_str(llbt_status status);
/* Message for the most recent failing call ON THIS THREAD; "" if none.
 * Valid until the next llbt_* call on the same thread. Never NULL. */
const char* llbt_last_error(void);

/* ------------------------------------------------------------------- store */

/* Open (creating if needed) a store backed by `path`. `opts` may be NULL. */
llbt_status llbt_store_open(const char* path, const llbt_options* opts, llbt_store** out);
/* Open an ephemeral in-memory store (nothing touches disk). */
llbt_status llbt_store_open_in_memory(llbt_store** out);
/* Release the store handle. Any open transactions/trees from it become
 * invalid. Safe to pass NULL. */
void llbt_store_close(llbt_store* store);
/* Rewrite the file to its minimal size (needs no live transactions).
 * `out_compacted` (nullable) is set to 1 if it ran, 0 for an in-memory store. */
llbt_status llbt_store_compact(llbt_store* store, int* out_compacted);
/* Backing path, or "<llbt in-memory>". Valid for the store's lifetime. */
const char* llbt_store_path(llbt_store* store);
/* Non-zero if the store has no backing file. */
int llbt_store_is_in_memory(llbt_store* store);

/* ------------------------------------------------------------ transactions */

/* Begin a read snapshot (never blocks). */
llbt_status llbt_txn_begin_read(llbt_store* store, llbt_txn** out);
/* Begin the exclusive writer (blocks while another writer is open). */
llbt_status llbt_txn_begin_write(llbt_store* store, llbt_txn** out);
/* Non-zero if this is a write transaction. */
int llbt_txn_is_writable(llbt_txn* txn);
/* Commit; `out_version` (nullable) receives the new version. Frees `txn`
 * (invalid afterward, even on error). */
llbt_status llbt_txn_commit(llbt_txn* txn, uint64_t* out_version);
/* Discard a write transaction (or end a read snapshot) and free `txn`.
 * Safe to pass NULL. */
void llbt_txn_abort(llbt_txn* txn);

/* Callback for llbt_write(): do your writes on `txn` and return LLBT_OK to
 * commit. Any other status fails THIS entry alone — its changes roll back
 * and the status comes back from its own llbt_write() call.
 * Rules: the txn belongs to the batch, so llbt_txn_commit / llbt_txn_abort
 * on it are rejected (do not free it). The callback may run on another
 * llbt_write()-calling thread, and may run MORE THAN ONCE if a batch-mate
 * fails (all effects roll back before the re-run) — keep side effects
 * inside the store. */
typedef llbt_status (*llbt_write_fn)(llbt_txn* txn, void* user);

/* Run `fn` inside a write transaction and commit it, batching concurrent
 * llbt_write() calls into ONE durable commit (group commit): N threads doing
 * small writes pay ~one storage sync instead of N. A lone caller costs the
 * same as begin_write + commit. On success `out_version` (nullable) receives
 * the version the changes landed in; batch-mates see the same version. */
llbt_status llbt_write(llbt_store* store, llbt_write_fn fn, void* user, uint64_t* out_version);

/* Number of named roots visible in this transaction. */
size_t llbt_txn_root_count(llbt_txn* txn);
/* Copy the name of root #index into `buf` (NUL-terminated, truncated to
 * `buf_size`). `out_len` (nullable) receives the full length. Pass buf=NULL,
 * buf_size=0 to query the length only. */
llbt_status llbt_txn_root_name(llbt_txn* txn, size_t index, char* buf, size_t buf_size, size_t* out_len);
/* Non-zero if a root named `name` exists. */
int llbt_txn_has_root(llbt_txn* txn, const char* name);
/* Erase a root and free its whole structure (write txn). `out_erased`
 * (nullable) is set to 1 if it existed, 0 otherwise. */
llbt_status llbt_txn_erase_root(llbt_txn* txn, const char* name, int* out_erased);

/* ------------------------------------------------------------------- trees */

/* Open (create in a write txn) the typed tree named `name`. If the root
 * already exists with a different element type, fails LLBT_ERR_ILLEGAL_OP. */
llbt_status llbt_tree_open(llbt_txn* txn, const char* name, llbt_type type, llbt_tree** out);
/* Release a tree handle (must happen before its transaction ends). NULL-safe. */
void llbt_tree_destroy(llbt_tree* tree);
/* The tree's element type. */
llbt_type llbt_tree_type(llbt_tree* tree);
/* Number of elements. */
size_t llbt_tree_size(llbt_tree* tree);
/* Non-zero if empty. */
int llbt_tree_is_empty(llbt_tree* tree);

/* Append `v` (write txn). `v->type` must match the tree's type. */
llbt_status llbt_tree_add(llbt_tree* tree, const llbt_value* v);
/* Insert `v` at `index` (0..size). */
llbt_status llbt_tree_insert(llbt_tree* tree, size_t index, const llbt_value* v);
/* Overwrite element `index`. */
llbt_status llbt_tree_set(llbt_tree* tree, size_t index, const llbt_value* v);
/* Read element `index` into `*out`. For STRING/BINARY, `out->as.bytes.data`
 * points into the transaction's memory (see the lifetime note up top). */
llbt_status llbt_tree_get(llbt_tree* tree, size_t index, llbt_value* out);
/* Erase element `index`. */
llbt_status llbt_tree_erase(llbt_tree* tree, size_t index);
/* Remove all elements. */
llbt_status llbt_tree_clear(llbt_tree* tree);
/* Index of the first element equal to `v`, or LLBT_NPOS. */
llbt_status llbt_tree_find_first(llbt_tree* tree, const llbt_value* v, size_t* out_index);
/* Index of the first element >= `v` (like std::lower_bound). Only meaningful
 * while the tree is kept sorted. */
llbt_status llbt_tree_lower_bound(llbt_tree* tree, const llbt_value* v, size_t* out_index);

/* ----------------------------------------------------------------- cursors */

/* Create a forward/backward cursor positioned at 0. */
llbt_status llbt_tree_cursor(llbt_tree* tree, llbt_cursor** out);
/* Release a cursor. NULL-safe. */
void llbt_cursor_destroy(llbt_cursor* cur);
/* Non-zero while the cursor points at a valid element. */
int llbt_cursor_valid(llbt_cursor* cur);
/* Current position. */
size_t llbt_cursor_pos(llbt_cursor* cur);
/* Value at the current position (see the get lifetime note). */
llbt_status llbt_cursor_value(llbt_cursor* cur, llbt_value* out);
void llbt_cursor_first(llbt_cursor* cur);
void llbt_cursor_last(llbt_cursor* cur);
void llbt_cursor_next(llbt_cursor* cur);
void llbt_cursor_prev(llbt_cursor* cur);
void llbt_cursor_seek(llbt_cursor* cur, size_t pos);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LLBT_C_API_H */
