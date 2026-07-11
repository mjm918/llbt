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

// C facade over llbt::core. Every fallible entry point runs its body inside a
// try/catch that turns C++ exceptions into an llbt_status plus a thread-local
// message, so no exception ever crosses the C boundary.

#include <llbt/c_api.h>

#include <llbt/core.hpp>
#include <llbt/exceptions.hpp>
#include <llbt/timestamp.hpp>
#include <llbt/string_data.hpp>
#include <llbt/binary_data.hpp>
#include <llbt/version.hpp>

#include <cstring>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

using llbt::BinaryData;
using llbt::ErrorCodes;
using llbt::Exception;
using llbt::FileAccessError;
using llbt::StringData;
using llbt::Timestamp;
using llbt::core::Store;
using llbt::core::StoreRef;
using llbt::core::Tree;
using llbt::core::Tx;

// ------------------------------------------------------------------ handles

struct llbt_store {
    StoreRef ref;
    std::string path; // cached so llbt_store_path can hand back a stable pointer
};

struct llbt_txn {
    Tx tx;
    explicit llbt_txn(Tx&& t)
        : tx(std::move(t))
    {
    }
};

using TreeVariant = std::variant<Tree<int64_t>, Tree<float>, Tree<double>, Tree<StringData>, Tree<BinaryData>,
                                 Tree<Timestamp>>;

struct llbt_tree {
    llbt_type type;
    TreeVariant v;
    template <class T>
    llbt_tree(llbt_type ty, Tree<T>&& tr)
        : type(ty)
        , v(std::move(tr))
    {
    }
};

struct llbt_cursor {
    llbt_tree* tree;
    size_t pos;
};

// -------------------------------------------------------------- error state

namespace {

thread_local std::string g_last_error;

llbt_status ok()
{
    g_last_error.clear();
    return LLBT_OK;
}

llbt_status fail(llbt_status status, const char* msg)
{
    g_last_error = msg ? msg : "";
    return status;
}

llbt_status map_code(ErrorCodes::Error code)
{
    switch (code) {
        case ErrorCodes::WrongTransactionState:
            return LLBT_E_WRONG_TXN_STATE;
        case ErrorCodes::IllegalOperation:
            return LLBT_E_ILLEGAL_OP;
        case ErrorCodes::InvalidArgument:
            return LLBT_E_INVALID_ARG;
        case ErrorCodes::OutOfBounds:
        case ErrorCodes::RangeError:
            return LLBT_E_OUT_OF_BOUNDS;
        case ErrorCodes::NoSuchTable:
            return LLBT_E_NOT_FOUND;
        default:
            return LLBT_E_UNKNOWN;
    }
}

// Call from a catch(...) block: rethrows to classify the in-flight exception.
llbt_status current_exception_to_status()
{
    try {
        throw;
    }
    catch (const FileAccessError& e) {
        return fail(LLBT_E_IO, e.what());
    }
    catch (const Exception& e) {
        return fail(map_code(e.code()), e.what());
    }
    catch (const std::exception& e) {
        return fail(LLBT_E_UNKNOWN, e.what());
    }
    catch (...) {
        return fail(LLBT_E_UNKNOWN, "unknown error");
    }
}

// ---- value marshalling ----

template <class>
struct elem_of;
template <class U>
struct elem_of<Tree<U>> {
    using type = U;
};

// C value -> engine value (overloaded on the destination type). Returns false
// if the caller's value type does not match the tree's element type.
bool val_in(const llbt_value* v, int64_t& out)
{
    if (v->type != LLBT_TYPE_INT64)
        return false;
    out = v->as.i64;
    return true;
}
bool val_in(const llbt_value* v, float& out)
{
    if (v->type != LLBT_TYPE_FLOAT)
        return false;
    out = v->as.f32;
    return true;
}
bool val_in(const llbt_value* v, double& out)
{
    if (v->type != LLBT_TYPE_DOUBLE)
        return false;
    out = v->as.f64;
    return true;
}
bool val_in(const llbt_value* v, StringData& out)
{
    if (v->type != LLBT_TYPE_STRING)
        return false;
    out = v->as.bytes.data ? StringData(v->as.bytes.data, v->as.bytes.size) : StringData();
    return true;
}
bool val_in(const llbt_value* v, BinaryData& out)
{
    if (v->type != LLBT_TYPE_BINARY)
        return false;
    out = v->as.bytes.data ? BinaryData(v->as.bytes.data, v->as.bytes.size) : BinaryData();
    return true;
}
bool val_in(const llbt_value* v, Timestamp& out)
{
    if (v->type != LLBT_TYPE_TIMESTAMP)
        return false;
    out = Timestamp(v->as.ts.seconds, v->as.ts.nanoseconds);
    return true;
}

// engine value -> C value (overloaded on the source type).
llbt_value val_out(int64_t v)
{
    return llbt_int64(v);
}
llbt_value val_out(float v)
{
    return llbt_float(v);
}
llbt_value val_out(double v)
{
    return llbt_double(v);
}
llbt_value val_out(StringData v)
{
    llbt_value x;
    x.type = LLBT_TYPE_STRING;
    x.as.bytes.data = v.data(); // NULL for a null string; points into the txn's memory otherwise
    x.as.bytes.size = v.size();
    return x;
}
llbt_value val_out(BinaryData v)
{
    llbt_value x;
    x.type = LLBT_TYPE_BINARY;
    x.as.bytes.data = v.data();
    x.as.bytes.size = v.size();
    return x;
}
llbt_value val_out(Timestamp v)
{
    llbt_value x;
    x.type = LLBT_TYPE_TIMESTAMP;
    if (v.is_null()) {
        x.as.ts.seconds = 0;
        x.as.ts.nanoseconds = 0;
    }
    else {
        x.as.ts.seconds = v.get_seconds();
        x.as.ts.nanoseconds = v.get_nanoseconds();
    }
    return x;
}

size_t tree_size(llbt_tree* tree)
{
    return std::visit([](auto& t) { return t.size(); }, tree->v);
}

} // namespace

// ------------------------------------------------------------------ library

extern "C" const char* llbt_version(void)
{
    static const std::string v = llbt::Version::get_version();
    return v.c_str();
}

extern "C" const char* llbt_status_str(llbt_status status)
{
    switch (status) {
        case LLBT_OK:
            return "LLBT_OK";
        case LLBT_E_INVALID_ARG:
            return "LLBT_E_INVALID_ARG";
        case LLBT_E_TYPE_MISMATCH:
            return "LLBT_E_TYPE_MISMATCH";
        case LLBT_E_OUT_OF_BOUNDS:
            return "LLBT_E_OUT_OF_BOUNDS";
        case LLBT_E_NOT_FOUND:
            return "LLBT_E_NOT_FOUND";
        case LLBT_E_WRONG_TXN_STATE:
            return "LLBT_E_WRONG_TXN_STATE";
        case LLBT_E_ILLEGAL_OP:
            return "LLBT_E_ILLEGAL_OP";
        case LLBT_E_IO:
            return "LLBT_E_IO";
        case LLBT_E_UNKNOWN:
            return "LLBT_E_UNKNOWN";
    }
    return "LLBT_E_UNKNOWN";
}

extern "C" const char* llbt_last_error(void)
{
    return g_last_error.c_str();
}

// -------------------------------------------------------------------- store

extern "C" llbt_status llbt_store_open(const char* path, const llbt_options* opts, llbt_store** out)
{
    if (!path || !out)
        return fail(LLBT_E_INVALID_ARG, "null argument");
    *out = nullptr;
    try {
        llbt::core::Options o;
        if (opts) {
            o.encryption_key = opts->encryption_key;
            o.no_sync = opts->no_sync != 0;
            o.single_process = opts->single_process != 0;
        }
        StoreRef ref = Store::open(path, o);
        auto* s = new llbt_store{std::move(ref), std::string()};
        s->path = s->ref->path();
        *out = s;
        return ok();
    }
    catch (...) {
        return current_exception_to_status();
    }
}

extern "C" llbt_status llbt_store_open_in_memory(llbt_store** out)
{
    if (!out)
        return fail(LLBT_E_INVALID_ARG, "null argument");
    *out = nullptr;
    try {
        StoreRef ref = Store::open_in_memory();
        auto* s = new llbt_store{std::move(ref), std::string()};
        s->path = s->ref->path();
        *out = s;
        return ok();
    }
    catch (...) {
        return current_exception_to_status();
    }
}

extern "C" void llbt_store_close(llbt_store* store)
{
    delete store;
}

extern "C" llbt_status llbt_store_compact(llbt_store* store, int* out_compacted)
{
    if (!store)
        return fail(LLBT_E_INVALID_ARG, "null store");
    try {
        bool did = store->ref->compact();
        if (out_compacted)
            *out_compacted = did ? 1 : 0;
        return ok();
    }
    catch (...) {
        return current_exception_to_status();
    }
}

extern "C" const char* llbt_store_path(llbt_store* store)
{
    return store ? store->path.c_str() : "";
}

extern "C" int llbt_store_is_in_memory(llbt_store* store)
{
    return (store && store->ref->is_in_memory()) ? 1 : 0;
}

// ------------------------------------------------------------ transactions

extern "C" llbt_status llbt_txn_begin_read(llbt_store* store, llbt_txn** out)
{
    if (!store || !out)
        return fail(LLBT_E_INVALID_ARG, "null argument");
    *out = nullptr;
    try {
        *out = new llbt_txn(store->ref->begin_read());
        return ok();
    }
    catch (...) {
        return current_exception_to_status();
    }
}

extern "C" llbt_status llbt_txn_begin_write(llbt_store* store, llbt_txn** out)
{
    if (!store || !out)
        return fail(LLBT_E_INVALID_ARG, "null argument");
    *out = nullptr;
    try {
        *out = new llbt_txn(store->ref->begin_write());
        return ok();
    }
    catch (...) {
        return current_exception_to_status();
    }
}

extern "C" int llbt_txn_is_writable(llbt_txn* txn)
{
    return (txn && txn->tx.writable()) ? 1 : 0;
}

extern "C" llbt_status llbt_txn_commit(llbt_txn* txn, uint64_t* out_version)
{
    if (!txn)
        return fail(LLBT_E_INVALID_ARG, "null txn");
    std::unique_ptr<llbt_txn> owner(txn); // freed on every path; rolls back if commit throws
    try {
        uint64_t version = txn->tx.commit();
        if (out_version)
            *out_version = version;
        return ok();
    }
    catch (...) {
        return current_exception_to_status();
    }
}

extern "C" void llbt_txn_abort(llbt_txn* txn)
{
    try {
        delete txn; // ~Tx rolls back a still-open write transaction
    }
    catch (...) {
        // never let an exception escape the C boundary
    }
}

extern "C" size_t llbt_txn_root_count(llbt_txn* txn)
{
    return txn ? txn->tx.root_count() : 0;
}

extern "C" llbt_status llbt_txn_root_name(llbt_txn* txn, size_t index, char* buf, size_t buf_size, size_t* out_len)
{
    if (!txn)
        return fail(LLBT_E_INVALID_ARG, "null txn");
    try {
        if (index >= txn->tx.root_count())
            return fail(LLBT_E_OUT_OF_BOUNDS, "root index out of bounds");
        std::string name = txn->tx.root_name(index);
        if (out_len)
            *out_len = name.size();
        if (buf && buf_size > 0) {
            size_t n = name.size() < buf_size - 1 ? name.size() : buf_size - 1;
            std::memcpy(buf, name.data(), n);
            buf[n] = '\0';
        }
        return ok();
    }
    catch (...) {
        return current_exception_to_status();
    }
}

extern "C" int llbt_txn_has_root(llbt_txn* txn, const char* name)
{
    if (!txn || !name)
        return 0;
    try {
        return txn->tx.get_root(name) ? 1 : 0;
    }
    catch (...) {
        return 0;
    }
}

extern "C" llbt_status llbt_txn_erase_root(llbt_txn* txn, const char* name, int* out_erased)
{
    if (!txn || !name)
        return fail(LLBT_E_INVALID_ARG, "null argument");
    try {
        bool erased = txn->tx.erase_root(name);
        if (out_erased)
            *out_erased = erased ? 1 : 0;
        return ok();
    }
    catch (...) {
        return current_exception_to_status();
    }
}

// -------------------------------------------------------------------- trees

extern "C" llbt_status llbt_tree_open(llbt_txn* txn, const char* name, llbt_type type, llbt_tree** out)
{
    if (!txn || !name || !out)
        return fail(LLBT_E_INVALID_ARG, "null argument");
    *out = nullptr;
    try {
        llbt_tree* t = nullptr;
        switch (type) {
            case LLBT_TYPE_INT64:
                t = new llbt_tree(type, txn->tx.tree<int64_t>(name));
                break;
            case LLBT_TYPE_FLOAT:
                t = new llbt_tree(type, txn->tx.tree<float>(name));
                break;
            case LLBT_TYPE_DOUBLE:
                t = new llbt_tree(type, txn->tx.tree<double>(name));
                break;
            case LLBT_TYPE_STRING:
                t = new llbt_tree(type, txn->tx.tree<StringData>(name));
                break;
            case LLBT_TYPE_BINARY:
                t = new llbt_tree(type, txn->tx.tree<BinaryData>(name));
                break;
            case LLBT_TYPE_TIMESTAMP:
                t = new llbt_tree(type, txn->tx.tree<Timestamp>(name));
                break;
            default:
                return fail(LLBT_E_INVALID_ARG, "unknown element type");
        }
        *out = t;
        return ok();
    }
    catch (...) {
        return current_exception_to_status();
    }
}

extern "C" void llbt_tree_destroy(llbt_tree* tree)
{
    delete tree;
}

extern "C" llbt_type llbt_tree_type(llbt_tree* tree)
{
    return tree->type;
}

extern "C" size_t llbt_tree_size(llbt_tree* tree)
{
    return tree ? tree_size(tree) : 0;
}

extern "C" int llbt_tree_is_empty(llbt_tree* tree)
{
    return (tree && tree_size(tree) == 0) ? 1 : 0;
}

extern "C" llbt_status llbt_tree_add(llbt_tree* tree, const llbt_value* v)
{
    if (!tree || !v)
        return fail(LLBT_E_INVALID_ARG, "null argument");
    try {
        return std::visit(
            [&](auto& t) -> llbt_status {
                using U = typename elem_of<std::decay_t<decltype(t)>>::type;
                U val;
                if (!val_in(v, val))
                    return fail(LLBT_E_TYPE_MISMATCH, "value type does not match tree");
                t.add(val);
                return ok();
            },
            tree->v);
    }
    catch (...) {
        return current_exception_to_status();
    }
}

extern "C" llbt_status llbt_tree_insert(llbt_tree* tree, size_t index, const llbt_value* v)
{
    if (!tree || !v)
        return fail(LLBT_E_INVALID_ARG, "null argument");
    try {
        return std::visit(
            [&](auto& t) -> llbt_status {
                using U = typename elem_of<std::decay_t<decltype(t)>>::type;
                if (index > t.size())
                    return fail(LLBT_E_OUT_OF_BOUNDS, "insert index out of bounds");
                U val;
                if (!val_in(v, val))
                    return fail(LLBT_E_TYPE_MISMATCH, "value type does not match tree");
                t.insert(index, val);
                return ok();
            },
            tree->v);
    }
    catch (...) {
        return current_exception_to_status();
    }
}

extern "C" llbt_status llbt_tree_set(llbt_tree* tree, size_t index, const llbt_value* v)
{
    if (!tree || !v)
        return fail(LLBT_E_INVALID_ARG, "null argument");
    try {
        return std::visit(
            [&](auto& t) -> llbt_status {
                using U = typename elem_of<std::decay_t<decltype(t)>>::type;
                if (index >= t.size())
                    return fail(LLBT_E_OUT_OF_BOUNDS, "index out of bounds");
                U val;
                if (!val_in(v, val))
                    return fail(LLBT_E_TYPE_MISMATCH, "value type does not match tree");
                t.set(index, val);
                return ok();
            },
            tree->v);
    }
    catch (...) {
        return current_exception_to_status();
    }
}

extern "C" llbt_status llbt_tree_get(llbt_tree* tree, size_t index, llbt_value* out)
{
    if (!tree || !out)
        return fail(LLBT_E_INVALID_ARG, "null argument");
    try {
        return std::visit(
            [&](auto& t) -> llbt_status {
                if (index >= t.size())
                    return fail(LLBT_E_OUT_OF_BOUNDS, "index out of bounds");
                *out = val_out(t.get(index));
                return ok();
            },
            tree->v);
    }
    catch (...) {
        return current_exception_to_status();
    }
}

extern "C" llbt_status llbt_tree_erase(llbt_tree* tree, size_t index)
{
    if (!tree)
        return fail(LLBT_E_INVALID_ARG, "null tree");
    try {
        return std::visit(
            [&](auto& t) -> llbt_status {
                if (index >= t.size())
                    return fail(LLBT_E_OUT_OF_BOUNDS, "index out of bounds");
                t.erase(index);
                return ok();
            },
            tree->v);
    }
    catch (...) {
        return current_exception_to_status();
    }
}

extern "C" llbt_status llbt_tree_clear(llbt_tree* tree)
{
    if (!tree)
        return fail(LLBT_E_INVALID_ARG, "null tree");
    try {
        std::visit([](auto& t) { t.clear(); }, tree->v);
        return ok();
    }
    catch (...) {
        return current_exception_to_status();
    }
}

extern "C" llbt_status llbt_tree_find_first(llbt_tree* tree, const llbt_value* v, size_t* out_index)
{
    if (!tree || !v || !out_index)
        return fail(LLBT_E_INVALID_ARG, "null argument");
    try {
        return std::visit(
            [&](auto& t) -> llbt_status {
                using U = typename elem_of<std::decay_t<decltype(t)>>::type;
                U val;
                if (!val_in(v, val))
                    return fail(LLBT_E_TYPE_MISMATCH, "value type does not match tree");
                *out_index = t.find_first(val);
                return ok();
            },
            tree->v);
    }
    catch (...) {
        return current_exception_to_status();
    }
}

extern "C" llbt_status llbt_tree_lower_bound(llbt_tree* tree, const llbt_value* v, size_t* out_index)
{
    if (!tree || !v || !out_index)
        return fail(LLBT_E_INVALID_ARG, "null argument");
    try {
        return std::visit(
            [&](auto& t) -> llbt_status {
                using U = typename elem_of<std::decay_t<decltype(t)>>::type;
                U val;
                if (!val_in(v, val))
                    return fail(LLBT_E_TYPE_MISMATCH, "value type does not match tree");
                *out_index = t.lower_bound(val);
                return ok();
            },
            tree->v);
    }
    catch (...) {
        return current_exception_to_status();
    }
}

// ------------------------------------------------------------------ cursors

extern "C" llbt_status llbt_tree_cursor(llbt_tree* tree, llbt_cursor** out)
{
    if (!tree || !out)
        return fail(LLBT_E_INVALID_ARG, "null argument");
    *out = new llbt_cursor{tree, 0};
    return ok();
}

extern "C" void llbt_cursor_destroy(llbt_cursor* cur)
{
    delete cur;
}

extern "C" int llbt_cursor_valid(llbt_cursor* cur)
{
    return (cur && cur->pos < tree_size(cur->tree)) ? 1 : 0;
}

extern "C" size_t llbt_cursor_pos(llbt_cursor* cur)
{
    return cur ? cur->pos : LLBT_NPOS;
}

extern "C" llbt_status llbt_cursor_value(llbt_cursor* cur, llbt_value* out)
{
    if (!cur || !out)
        return fail(LLBT_E_INVALID_ARG, "null argument");
    return llbt_tree_get(cur->tree, cur->pos, out);
}

extern "C" void llbt_cursor_first(llbt_cursor* cur)
{
    if (cur)
        cur->pos = 0;
}

extern "C" void llbt_cursor_last(llbt_cursor* cur)
{
    if (cur) {
        size_t n = tree_size(cur->tree);
        cur->pos = n ? n - 1 : 0;
    }
}

extern "C" void llbt_cursor_next(llbt_cursor* cur)
{
    if (cur)
        ++cur->pos;
}

extern "C" void llbt_cursor_prev(llbt_cursor* cur)
{
    if (cur)
        cur->pos = cur->pos ? cur->pos - 1 : LLBT_NPOS;
}

extern "C" void llbt_cursor_seek(llbt_cursor* cur, size_t pos)
{
    if (cur)
        cur->pos = pos;
}
