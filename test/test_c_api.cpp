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

#include "testsettings.hpp"
#ifdef TEST_C_API

#include <llbt/c_api.h>

#include "test.hpp"

#include <cstring>
#include <string>

// Exercises the extern "C" surface directly (as a C++ TU, so we can use the
// unit-test harness, but every call goes through the C API).

namespace {
std::string bytes_str(const llbt_value& v)
{
    return std::string(v.as.bytes.data ? v.as.bytes.data : "", v.as.bytes.size);
}
} // namespace

TEST(CApi_Version)
{
    const char* v = llbt_version();
    CHECK(v != nullptr);
    CHECK(std::strlen(v) > 0);
    CHECK_EQUAL(std::string(llbt_status_str(LLBT_OK)), std::string("LLBT_OK"));
    CHECK_EQUAL(std::string(llbt_status_str(LLBT_E_IO)), std::string("LLBT_E_IO"));
}

TEST(CApi_Int64Roundtrip)
{
    llbt_store* store = nullptr;
    CHECK_EQUAL(llbt_store_open_in_memory(&store), LLBT_OK);
    CHECK(llbt_store_is_in_memory(store));

    llbt_txn* w = nullptr;
    CHECK_EQUAL(llbt_txn_begin_write(store, &w), LLBT_OK);
    CHECK(llbt_txn_is_writable(w));

    llbt_tree* t = nullptr;
    CHECK_EQUAL(llbt_tree_open(w, "nums", LLBT_TYPE_INT64, &t), LLBT_OK);
    CHECK_EQUAL(int(llbt_tree_type(t)), int(LLBT_TYPE_INT64));
    for (int64_t i = 0; i < 100; ++i) {
        llbt_value v = llbt_int64(i * 2);
        CHECK_EQUAL(llbt_tree_add(t, &v), LLBT_OK);
    }
    CHECK_EQUAL(llbt_tree_size(t), size_t(100));

    llbt_value got;
    CHECK_EQUAL(llbt_tree_get(t, 10, &got), LLBT_OK);
    CHECK_EQUAL(int(got.type), int(LLBT_TYPE_INT64));
    CHECK_EQUAL(got.as.i64, int64_t(20));

    size_t idx = LLBT_NPOS;
    llbt_value key = llbt_int64(20);
    CHECK_EQUAL(llbt_tree_find_first(t, &key, &idx), LLBT_OK);
    CHECK_EQUAL(idx, size_t(10));

    llbt_value missing = llbt_int64(7); // odd, never inserted
    CHECK_EQUAL(llbt_tree_find_first(t, &missing, &idx), LLBT_OK);
    CHECK_EQUAL(idx, LLBT_NPOS);

    llbt_tree_destroy(t);
    uint64_t version = 0;
    CHECK_EQUAL(llbt_txn_commit(w, &version), LLBT_OK);
    CHECK(version > 0);

    llbt_txn* r = nullptr;
    CHECK_EQUAL(llbt_txn_begin_read(store, &r), LLBT_OK);
    CHECK(!llbt_txn_is_writable(r));
    CHECK_EQUAL(llbt_tree_open(r, "nums", LLBT_TYPE_INT64, &t), LLBT_OK);

    llbt_cursor* cur = nullptr;
    CHECK_EQUAL(llbt_tree_cursor(t, &cur), LLBT_OK);
    int64_t expect = 0, count = 0;
    for (llbt_cursor_first(cur); llbt_cursor_valid(cur); llbt_cursor_next(cur)) {
        llbt_value v;
        CHECK_EQUAL(llbt_cursor_value(cur, &v), LLBT_OK);
        CHECK_EQUAL(v.as.i64, expect);
        expect += 2;
        ++count;
    }
    CHECK_EQUAL(count, int64_t(100));
    llbt_cursor_destroy(cur);
    llbt_tree_destroy(t);
    llbt_txn_abort(r);
    llbt_store_close(store);
}

TEST(CApi_AllTypes)
{
    llbt_store* store = nullptr;
    CHECK_EQUAL(llbt_store_open_in_memory(&store), LLBT_OK);
    llbt_txn* w = nullptr;
    CHECK_EQUAL(llbt_txn_begin_write(store, &w), LLBT_OK);
    llbt_tree* t = nullptr;
    llbt_value v, g;

    CHECK_EQUAL(llbt_tree_open(w, "i", LLBT_TYPE_INT64, &t), LLBT_OK);
    v = llbt_int64(-123);
    CHECK_EQUAL(llbt_tree_add(t, &v), LLBT_OK);
    CHECK_EQUAL(llbt_tree_get(t, 0, &g), LLBT_OK);
    CHECK_EQUAL(g.as.i64, int64_t(-123));
    llbt_tree_destroy(t);

    CHECK_EQUAL(llbt_tree_open(w, "f", LLBT_TYPE_FLOAT, &t), LLBT_OK);
    v = llbt_float(2.5f);
    CHECK_EQUAL(llbt_tree_add(t, &v), LLBT_OK);
    CHECK_EQUAL(llbt_tree_get(t, 0, &g), LLBT_OK);
    CHECK_EQUAL(g.as.f32, 2.5f);
    llbt_tree_destroy(t);

    CHECK_EQUAL(llbt_tree_open(w, "d", LLBT_TYPE_DOUBLE, &t), LLBT_OK);
    v = llbt_double(3.125);
    CHECK_EQUAL(llbt_tree_add(t, &v), LLBT_OK);
    CHECK_EQUAL(llbt_tree_get(t, 0, &g), LLBT_OK);
    CHECK_EQUAL(g.as.f64, 3.125);
    llbt_tree_destroy(t);

    CHECK_EQUAL(llbt_tree_open(w, "s", LLBT_TYPE_STRING, &t), LLBT_OK);
    v = llbt_string("hello", 5);
    CHECK_EQUAL(llbt_tree_add(t, &v), LLBT_OK);
    CHECK_EQUAL(llbt_tree_get(t, 0, &g), LLBT_OK);
    CHECK_EQUAL(int(g.type), int(LLBT_TYPE_STRING));
    CHECK_EQUAL(bytes_str(g), std::string("hello"));
    llbt_tree_destroy(t);

    CHECK_EQUAL(llbt_tree_open(w, "b", LLBT_TYPE_BINARY, &t), LLBT_OK);
    const char raw[4] = {'a', 0, 'b', 1}; // embedded NUL — length, not termination, matters
    v = llbt_binary(raw, 4);
    CHECK_EQUAL(llbt_tree_add(t, &v), LLBT_OK);
    CHECK_EQUAL(llbt_tree_get(t, 0, &g), LLBT_OK);
    CHECK_EQUAL(g.as.bytes.size, size_t(4));
    CHECK(std::memcmp(g.as.bytes.data, raw, 4) == 0);
    llbt_tree_destroy(t);

    CHECK_EQUAL(llbt_tree_open(w, "ts", LLBT_TYPE_TIMESTAMP, &t), LLBT_OK);
    v = llbt_timestamp(1700000000, 250);
    CHECK_EQUAL(llbt_tree_add(t, &v), LLBT_OK);
    CHECK_EQUAL(llbt_tree_get(t, 0, &g), LLBT_OK);
    CHECK_EQUAL(int(g.type), int(LLBT_TYPE_TIMESTAMP));
    CHECK_EQUAL(g.as.ts.seconds, int64_t(1700000000));
    CHECK_EQUAL(g.as.ts.nanoseconds, int32_t(250));
    llbt_tree_destroy(t);

    CHECK_EQUAL(llbt_txn_commit(w, nullptr), LLBT_OK);
    llbt_store_close(store);
}

TEST(CApi_Errors)
{
    llbt_store* store = nullptr;
    CHECK_EQUAL(llbt_store_open_in_memory(&store), LLBT_OK);
    llbt_txn* w = nullptr;
    CHECK_EQUAL(llbt_txn_begin_write(store, &w), LLBT_OK);
    llbt_tree* t = nullptr;
    CHECK_EQUAL(llbt_tree_open(w, "nums", LLBT_TYPE_INT64, &t), LLBT_OK);
    llbt_value n = llbt_int64(1);
    CHECK_EQUAL(llbt_tree_add(t, &n), LLBT_OK);

    llbt_value str = llbt_string("x", 1);
    CHECK_EQUAL(llbt_tree_add(t, &str), LLBT_E_TYPE_MISMATCH);
    CHECK(std::strlen(llbt_last_error()) > 0);

    llbt_value g;
    CHECK_EQUAL(llbt_tree_get(t, 5, &g), LLBT_E_OUT_OF_BOUNDS);

    llbt_tree* t2 = nullptr;
    CHECK_EQUAL(llbt_tree_open(w, "nums", LLBT_TYPE_STRING, &t2), LLBT_E_ILLEGAL_OP);

    CHECK_EQUAL(llbt_tree_add(nullptr, &n), LLBT_E_INVALID_ARG);

    llbt_tree_destroy(t);
    CHECK_EQUAL(llbt_txn_commit(w, nullptr), LLBT_OK);

    // mutating through a read snapshot is refused, not crashed
    llbt_txn* r = nullptr;
    CHECK_EQUAL(llbt_txn_begin_read(store, &r), LLBT_OK);
    CHECK_EQUAL(llbt_tree_open(r, "nums", LLBT_TYPE_INT64, &t), LLBT_OK);
    CHECK_EQUAL(llbt_tree_add(t, &n), LLBT_E_WRONG_TXN_STATE);
    llbt_tree_destroy(t);
    llbt_txn_abort(r);
    llbt_store_close(store);
}

TEST(CApi_Roots)
{
    llbt_store* store = nullptr;
    CHECK_EQUAL(llbt_store_open_in_memory(&store), LLBT_OK);
    llbt_txn* w = nullptr;
    CHECK_EQUAL(llbt_txn_begin_write(store, &w), LLBT_OK);

    llbt_tree* a = nullptr;
    CHECK_EQUAL(llbt_tree_open(w, "alpha", LLBT_TYPE_INT64, &a), LLBT_OK);
    llbt_value v = llbt_int64(1);
    CHECK_EQUAL(llbt_tree_add(a, &v), LLBT_OK);
    llbt_tree_destroy(a);
    llbt_tree* b = nullptr;
    CHECK_EQUAL(llbt_tree_open(w, "beta", LLBT_TYPE_DOUBLE, &b), LLBT_OK);
    llbt_tree_destroy(b);

    CHECK_EQUAL(llbt_txn_root_count(w), size_t(2));
    CHECK(llbt_txn_has_root(w, "alpha"));
    CHECK(llbt_txn_has_root(w, "beta"));
    CHECK(!llbt_txn_has_root(w, "gamma"));

    char name[32];
    size_t len = 0;
    CHECK_EQUAL(llbt_txn_root_name(w, 0, name, sizeof(name), &len), LLBT_OK);
    CHECK(len > 0);
    CHECK_EQUAL(std::strlen(name), len);

    int erased = 0;
    CHECK_EQUAL(llbt_txn_erase_root(w, "alpha", &erased), LLBT_OK);
    CHECK(erased);
    CHECK(!llbt_txn_has_root(w, "alpha"));
    CHECK_EQUAL(llbt_txn_root_count(w), size_t(1));

    CHECK_EQUAL(llbt_txn_commit(w, nullptr), LLBT_OK);
    llbt_store_close(store);
}

TEST(CApi_FilePersistence)
{
    TEST_PATH(path);
    std::string p(path);
    {
        llbt_store* store = nullptr;
        CHECK_EQUAL(llbt_store_open(p.c_str(), nullptr, &store), LLBT_OK);
        CHECK(!llbt_store_is_in_memory(store));
        llbt_txn* w = nullptr;
        CHECK_EQUAL(llbt_txn_begin_write(store, &w), LLBT_OK);
        llbt_tree* t = nullptr;
        CHECK_EQUAL(llbt_tree_open(w, "k", LLBT_TYPE_INT64, &t), LLBT_OK);
        for (int64_t i = 0; i < 50; ++i) {
            llbt_value v = llbt_int64(i);
            CHECK_EQUAL(llbt_tree_add(t, &v), LLBT_OK);
        }
        llbt_tree_destroy(t);
        CHECK_EQUAL(llbt_txn_commit(w, nullptr), LLBT_OK);
        int compacted = 0;
        CHECK_EQUAL(llbt_store_compact(store, &compacted), LLBT_OK);
        CHECK(compacted);
        llbt_store_close(store);
    }
    {
        llbt_store* store = nullptr;
        CHECK_EQUAL(llbt_store_open(p.c_str(), nullptr, &store), LLBT_OK);
        llbt_txn* r = nullptr;
        CHECK_EQUAL(llbt_txn_begin_read(store, &r), LLBT_OK);
        llbt_tree* t = nullptr;
        CHECK_EQUAL(llbt_tree_open(r, "k", LLBT_TYPE_INT64, &t), LLBT_OK);
        CHECK_EQUAL(llbt_tree_size(t), size_t(50));
        llbt_value g;
        CHECK_EQUAL(llbt_tree_get(t, 49, &g), LLBT_OK);
        CHECK_EQUAL(g.as.i64, int64_t(49));
        llbt_tree_destroy(t);
        llbt_txn_abort(r);
        llbt_store_close(store);
    }
}

namespace {
// llbt_write callbacks (plain C shape: no captures, context through `user`)
struct GroupedWriteCtx {
    int64_t value = 0;
    llbt_status inner_commit_status = LLBT_OK;
    int runs = 0;
};

llbt_status grouped_add_cb(llbt_txn* txn, void* user)
{
    auto* ctx = static_cast<GroupedWriteCtx*>(user);
    ++ctx->runs;
    llbt_tree* t = nullptr;
    llbt_status st = llbt_tree_open(txn, "gnums", LLBT_TYPE_INT64, &t);
    if (st != LLBT_OK)
        return st;
    llbt_value v = llbt_int64(ctx->value);
    st = llbt_tree_add(t, &v);
    llbt_tree_destroy(t);
    return st;
}

llbt_status grouped_misuse_cb(llbt_txn* txn, void* user)
{
    auto* ctx = static_cast<GroupedWriteCtx*>(user);
    // the txn is the batch's: a stray commit/abort must bounce off harmlessly
    ctx->inner_commit_status = llbt_txn_commit(txn, nullptr);
    llbt_txn_abort(txn); // must be a no-op, not a free
    return grouped_add_cb(txn, user);
}

llbt_status grouped_fail_cb(llbt_txn* txn, void* user)
{
    grouped_add_cb(txn, user);
    return LLBT_E_ILLEGAL_OP; // fail this entry: its add must roll back
}
} // namespace

TEST(CApi_GroupedWrite)
{
    llbt_store* store = nullptr;
    CHECK_EQUAL(llbt_store_open_in_memory(&store), LLBT_OK);

    // two grouped writes commit and report growing versions
    GroupedWriteCtx a;
    a.value = 7;
    uint64_t v1 = 0;
    CHECK_EQUAL(llbt_write(store, grouped_add_cb, &a, &v1), LLBT_OK);
    CHECK_EQUAL(a.runs, 1);
    CHECK(v1 > 0);

    GroupedWriteCtx b;
    b.value = 9;
    uint64_t v2 = 0;
    CHECK_EQUAL(llbt_write(store, grouped_misuse_cb, &b, &v2), LLBT_OK);
    CHECK_EQUAL(v2, v1 + 1);
    // the in-callback commit was rejected cleanly, the write still landed
    CHECK_EQUAL(b.inner_commit_status, LLBT_E_WRONG_TXN_STATE);

    // a failing callback returns its status and leaves no trace
    GroupedWriteCtx c;
    c.value = 11;
    CHECK_EQUAL(llbt_write(store, grouped_fail_cb, &c, nullptr), LLBT_E_ILLEGAL_OP);

    llbt_txn* r = nullptr;
    CHECK_EQUAL(llbt_txn_begin_read(store, &r), LLBT_OK);
    llbt_tree* t = nullptr;
    CHECK_EQUAL(llbt_tree_open(r, "gnums", LLBT_TYPE_INT64, &t), LLBT_OK);
    CHECK_EQUAL(llbt_tree_size(t), size_t(2)); // 7 and 9; the failed 11 rolled back
    llbt_value out;
    CHECK_EQUAL(llbt_tree_get(t, 0, &out), LLBT_OK);
    CHECK_EQUAL(out.as.i64, int64_t(7));
    CHECK_EQUAL(llbt_tree_get(t, 1, &out), LLBT_OK);
    CHECK_EQUAL(out.as.i64, int64_t(9));
    llbt_tree_destroy(t);
    llbt_txn_abort(r);

    // null-argument hygiene
    CHECK_EQUAL(llbt_write(nullptr, grouped_add_cb, &a, nullptr), LLBT_E_INVALID_ARG);
    CHECK_EQUAL(llbt_write(store, nullptr, &a, nullptr), LLBT_E_INVALID_ARG);

    llbt_store_close(store);
}

#endif // TEST_C_API
