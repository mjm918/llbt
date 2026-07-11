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

/* The C API in one file: open a store, write two typed trees in a
 * transaction, then read them back through a cursor — all from plain C. */

#include <llbt/c_api.h>

#include <stdio.h>
#include <string.h>

static int oops(const char* what, llbt_status s)
{
    fprintf(stderr, "%s failed: %s — %s\n", what, llbt_status_str(s), llbt_last_error());
    return 1;
}

int main(void)
{
    printf("llbt version %s\n", llbt_version());

    llbt_store* store = NULL;
    llbt_status s = llbt_store_open_in_memory(&store);
    if (s != LLBT_OK)
        return oops("open_in_memory", s);
    printf("opened %s (in-memory=%d)\n", llbt_store_path(store), llbt_store_is_in_memory(store));

    /* ---- write transaction ---- */
    llbt_txn* w = NULL;
    s = llbt_txn_begin_write(store, &w);
    if (s != LLBT_OK)
        return oops("begin_write", s);

    llbt_tree* scores = NULL;
    s = llbt_tree_open(w, "scores", LLBT_TYPE_INT64, &scores);
    if (s != LLBT_OK)
        return oops("open scores", s);

    static const int64_t nums[] = {7, 42, 13, 99, 5};
    size_t i;
    for (i = 0; i < sizeof(nums) / sizeof(nums[0]); ++i) {
        llbt_value v = llbt_int64(nums[i]);
        s = llbt_tree_add(scores, &v);
        if (s != LLBT_OK)
            return oops("add score", s);
    }

    llbt_tree* names = NULL;
    s = llbt_tree_open(w, "names", LLBT_TYPE_STRING, &names);
    if (s != LLBT_OK)
        return oops("open names", s);

    static const char* who[] = {"ada", "grace", "linus"};
    for (i = 0; i < 3; ++i) {
        llbt_value v = llbt_string(who[i], strlen(who[i]));
        s = llbt_tree_add(names, &v);
        if (s != LLBT_OK)
            return oops("add name", s);
    }

    /* a type mismatch is reported as a status, never a crash */
    llbt_value wrong = llbt_string("nope", 4);
    printf("adding a string to an int64 tree -> %s\n", llbt_status_str(llbt_tree_add(scores, &wrong)));

    llbt_tree_destroy(scores);
    llbt_tree_destroy(names);

    uint64_t version = 0;
    s = llbt_txn_commit(w, &version);
    if (s != LLBT_OK)
        return oops("commit", s);
    printf("committed as version %llu\n", (unsigned long long)version);

    /* ---- read snapshot ---- */
    llbt_txn* r = NULL;
    s = llbt_txn_begin_read(store, &r);
    if (s != LLBT_OK)
        return oops("begin_read", s);

    printf("%zu named roots:\n", llbt_txn_root_count(r));
    for (i = 0; i < llbt_txn_root_count(r); ++i) {
        char name[64];
        llbt_txn_root_name(r, i, name, sizeof(name), NULL);
        printf("  - %s\n", name);
    }

    s = llbt_tree_open(r, "scores", LLBT_TYPE_INT64, &scores);
    if (s != LLBT_OK)
        return oops("reopen scores", s);

    printf("scores via cursor:");
    llbt_cursor* cur = NULL;
    llbt_tree_cursor(scores, &cur);
    for (llbt_cursor_first(cur); llbt_cursor_valid(cur); llbt_cursor_next(cur)) {
        llbt_value v;
        llbt_cursor_value(cur, &v);
        printf(" %lld", (long long)v.as.i64);
    }
    printf("\n");
    llbt_cursor_destroy(cur);

    size_t idx = LLBT_NPOS;
    llbt_value key = llbt_int64(13);
    llbt_tree_find_first(scores, &key, &idx);
    printf("find_first(13) -> index %zu\n", idx);
    llbt_tree_destroy(scores);

    s = llbt_tree_open(r, "names", LLBT_TYPE_STRING, &names);
    if (s != LLBT_OK)
        return oops("reopen names", s);
    printf("names:");
    for (i = 0; i < llbt_tree_size(names); ++i) {
        llbt_value v;
        llbt_tree_get(names, i, &v);
        printf(" %.*s", (int)v.as.bytes.size, v.as.bytes.data);
    }
    printf("\n");
    llbt_tree_destroy(names);

    llbt_txn_abort(r);
    llbt_store_close(store);
    printf("done\n");
    return 0;
}
