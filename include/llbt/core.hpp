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
#ifndef LLBT_CORE_HPP
#define LLBT_CORE_HPP

// llbt::core — a single-file embedded page store.
//
//   * one file, copy-on-write pages, crash-safe commits
//   * single writer, many concurrent readers (MVCC snapshots)
//   * group commit: concurrent Store::write() calls coalesce into one
//     physical commit, so N threads pay ~one fsync instead of N
//   * named roots ("buckets"): durable anchors for YOUR data structures
//   * Tree<T>: a durable B+tree sequence with a Cursor
//   * raw mode: build any structure out of Array nodes and anchor it
//
// Rules of the road:
//   * A Tx must not outlive its Store.
//   * Tree handles and NodeRefs are valid only inside their Tx.
//   * Open at most one handle per root per write Tx.
//   * set_root/erase_root FREE the structure they replace; never reuse
//     nodes from a replaced structure.
//
// Everything here wraps the battle-tested storage engine imported from
// Barq Core / Realm Core; this layer adds no clever
// logic of its own — it only curates.

#include <llbt/db.hpp>
#include <llbt/transaction.hpp>
#include <llbt/group.hpp>
#include <llbt/array.hpp>
#include <llbt/array_basic.hpp>
#include <llbt/array_integer.hpp>
#include <llbt/array_string.hpp>
#include <llbt/array_binary.hpp>
#include <llbt/array_timestamp.hpp>
#include <llbt/bplustree.hpp>
#include <llbt/core/roots_replication.hpp>
#include <llbt/exceptions.hpp>
#include <llbt/util/function_ref.hpp>

#include <memory>
#include <optional>
#include <string>

namespace llbt::core {

using NodeRef = llbt::ref_type; // a page-tree reference inside the file

struct Options {
    /// 64-byte AES key, or null. Requires LLBT_ENABLE_ENCRYPTION.
    const char* encryption_key = nullptr;
    /// If true, commits skip storage synchronization. Fast, and normally safe
    /// across a process crash, but an OS crash or power loss can lose recent
    /// commits or leave the file unusable. Use only for rebuildable data.
    bool no_sync = false;
    /// Promise that this file is only ever opened from one process at a
    /// time (threads are fine). Commits then skip the per-operation file
    /// locks that cross-process safety costs on Apple/Android — noticeably
    /// faster. Opening the same file from two processes with this set is
    /// undefined behavior. In-memory stores always run in this mode.
    bool single_process = false;
};

namespace detail {

// Maps Tree<T> element types to a tag stored in the registry, so opening a
// root with the wrong type fails loudly instead of misreading pages.
template <class T> struct type_tag;
template <> struct type_tag<int64_t> { static constexpr int64_t value = 1; };
template <> struct type_tag<float> { static constexpr int64_t value = 2; };
template <> struct type_tag<double> { static constexpr int64_t value = 3; };
template <> struct type_tag<StringData> { static constexpr int64_t value = 4; };
template <> struct type_tag<BinaryData> { static constexpr int64_t value = 5; };
template <> struct type_tag<Timestamp> { static constexpr int64_t value = 6; };
constexpr int64_t raw_type_tag = 0;

/// The named-root registry, anchored at the history slot of the file's top
/// array (see roots_replication.hpp for the layout). Heap-allocated so the
/// parent pointers handed to user trees stay stable.
struct Registry {
    Array top;
    BPlusTree<StringData> names;
    Array roots;
    Array types;
    bool writable = false;

    explicit Registry(Allocator& alloc)
        : top(alloc)
        , names(alloc)
        , roots(alloc)
        , types(alloc)
    {
    }

    // Bind read-only from the current snapshot; no-op registry if absent.
    void bind_read(Transaction& tr);
    // Bind for writing; creates the registry on first use.
    void bind_write(Transaction& tr);

    size_t find(StringData name) const noexcept;
    size_t count() const noexcept;
    StringData name(size_t i) const noexcept;
    NodeRef root(size_t i) const noexcept;
    int64_t type(size_t i) const noexcept;

    // write ops (registry must be bound writable)
    size_t add(StringData name, int64_t type_tag);        // returns new slot
    void set_root_ref(size_t i, NodeRef ref);             // frees the old tree
    void erase(size_t i);                                 // frees + removes
};

} // namespace detail

template <class T> class Cursor;
template <class T> class Tree;
class Store;
using StoreRef = std::shared_ptr<Store>;

/// A transaction. Read transactions are immutable snapshots and never block.
/// Write transactions are exclusive; changes become durable at commit().
/// RAII: an uncommitted write Tx rolls back on destruction.
class Tx {
public:
    Tx(Tx&&) noexcept = default;
    Tx& operator=(Tx&&) noexcept = default;
    ~Tx()
    {
        if (m_tr && m_writable && m_tr->get_transact_stage() == DB::transact_Writing)
            m_tr->rollback();
    }

    bool writable() const noexcept { return m_writable; }

    // ---- named roots (the bucket directory) ----
    size_t root_count() const noexcept { return m_reg->count(); }
    std::string root_name(size_t i) const { return std::string(m_reg->name(i)); }
    /// Ref anchored under `name`, or none if absent.
    std::optional<NodeRef> get_root(StringData name) const
    {
        size_t i = m_reg->find(name);
        if (i == npos)
            return std::nullopt;
        return m_reg->root(i);
    }

    /// Anchor a structure you built with alloc()/Array under `name`.
    /// Replaces (and frees) whatever was anchored there before.
    void set_root(StringData name, NodeRef ref)
    {
        require_write();
        size_t i = m_reg->find(name);
        if (i == npos)
            i = m_reg->add(name, detail::raw_type_tag);
        else if (m_reg->type(i) != detail::raw_type_tag)
            throw LogicError(ErrorCodes::IllegalOperation, "root holds a typed Tree; erase it first");
        m_reg->set_root_ref(i, ref);
    }

    /// Remove `name` and free its whole structure. Invalidates other root
    /// handles obtained from this Tx — re-fetch them.
    bool erase_root(StringData name)
    {
        require_write();
        size_t i = m_reg->find(name);
        if (i == npos)
            return false;
        m_reg->erase(i);
        return true;
    }

    /// Open (or create, in a write Tx) the durable Tree<T> named `name`.
    template <class T>
    Tree<T> tree(StringData name);

    // ---- raw mode ----
    /// The file-backed allocator: build your own Array/BPlusTree structures
    /// against it, then anchor them with set_root().
    Allocator& alloc() noexcept { return _impl::GroupFriend::get_alloc(*m_tr); }

    // ---- lifecycle ----
    /// Commit and return the new version. The Tx is done afterwards.
    /// Not available inside a Store::write() closure — there the store
    /// commits the whole batch after the last closure returns.
    uint64_t commit()
    {
        require_write();
        require_not_batched();
        return m_tr->commit();
    }
    void rollback()
    {
        require_write();
        require_not_batched();
        m_tr->rollback();
    }

    /// The underlying engine transaction (advanced use).
    Transaction& raw() noexcept { return *m_tr; }

    static constexpr size_t npos = size_t(-1);

private:
    friend class Store;
    template <class U> friend class Tree;

    Tx(TransactionRef tr, StoreRef keep_alive, bool writable)
        : m_tr(std::move(tr))
        , m_store(std::move(keep_alive))
        , m_writable(writable)
        , m_reg(std::make_unique<detail::Registry>(_impl::GroupFriend::get_alloc(*m_tr)))
    {
        if (m_writable)
            m_reg->bind_write(*m_tr);
        else
            m_reg->bind_read(*m_tr);
    }

    void require_write() const
    {
        if (!m_writable)
            throw LogicError(ErrorCodes::WrongTransactionState, "read-only transaction");
    }

    void require_not_batched() const
    {
        if (m_batched)
            throw LogicError(ErrorCodes::WrongTransactionState,
                             "no commit/rollback inside a Store::write() closure — "
                             "the store commits the batch when every closure is done");
    }

    TransactionRef m_tr;
    StoreRef m_store; // keeps the Store (and file) alive for the Tx lifetime
    bool m_writable;
    bool m_batched = false; // Tx is shared by a Store::write() batch
    std::unique_ptr<detail::Registry> m_reg;
};

/// A durable B+tree sequence of T, anchored under a named root.
/// Positional like an array (insert/erase by index), with a Cursor for
/// ordered iteration. Keep it sorted yourself if you need a sorted map —
/// or don't; it's your data structure.
template <class T>
class Tree {
public:
    size_t size() const noexcept { return m_bt->size(); }
    bool empty() const noexcept { return size() == 0; }
    T get(size_t i) const { return m_bt->get(i); }

    void add(T value) { require_write(); m_bt->add(value); }
    void insert(size_t i, T value) { require_write(); m_bt->insert(i, value); }
    void set(size_t i, T value) { require_write(); m_bt->set(i, value); }
    void erase(size_t i) { require_write(); m_bt->erase(i); }
    void clear() { require_write(); m_bt->clear(); }

    /// Index of the first element equal to `value`, or Tx::npos.
    size_t find_first(T value) const noexcept { return m_bt->find_first(value); }

    /// Index of the first element >= `value` (like std::lower_bound). Only
    /// meaningful while you keep the tree sorted — the usual pattern is
    /// `insert(lower_bound(v), v)`.
    size_t lower_bound(T value) const { return m_bt->lower_bound(value); }

    Cursor<T> cursor() const;

    /// This tree's current root ref (changes as the tree grows).
    NodeRef ref() const noexcept { return m_bt->get_ref(); }

    /// The wrapped engine tree (advanced use).
    BPlusTree<T>& raw() noexcept { return *m_bt; }

private:
    friend class Tx;
    Tree(std::unique_ptr<BPlusTree<T>> bt, bool writable)
        : m_bt(std::move(bt))
        , m_writable(writable)
    {
    }
    void require_write() const
    {
        if (!m_writable)
            throw LogicError(ErrorCodes::WrongTransactionState, "read-only transaction");
    }
    std::unique_ptr<BPlusTree<T>> m_bt;
    bool m_writable;
};

/// Ordered iteration over a Tree<T>: first()/next()/valid().
template <class T>
class Cursor {
public:
    bool valid() const noexcept { return m_pos < m_tree->size(); }
    size_t pos() const noexcept { return m_pos; }
    T value() const { return m_tree->get(m_pos); }

    void first() noexcept { m_pos = 0; }
    void last() noexcept { m_pos = m_tree->size() ? m_tree->size() - 1 : 0; }
    void next() noexcept { ++m_pos; }
    void prev() noexcept { m_pos = m_pos ? m_pos - 1 : size_t(-1); }
    void seek(size_t pos) noexcept { m_pos = pos; }

private:
    template <class U> friend class Tree;
    explicit Cursor(const Tree<T>* tree)
        : m_tree(tree)
    {
    }
    const Tree<T>* m_tree;
    size_t m_pos = 0;
};

template <class T>
Cursor<T> Tree<T>::cursor() const
{
    return Cursor<T>(this);
}

/// One store = one file. Open once, share the StoreRef; every Tx starts here.
class Store : public std::enable_shared_from_this<Store> {
public:
    static StoreRef open(const std::string& path, const Options& options = {});
    /// Open a purely in-memory store: no file is created and nothing is
    /// written to disk. The same copy-on-write MVCC engine runs against an
    /// anonymous memory buffer, so Tx / commit / named roots / Tree all
    /// behave exactly as on a file — but when the last StoreRef drops, the
    /// data is gone. `Options` do not apply here (there is no file to
    /// encrypt, and nothing to fsync).
    static StoreRef open_in_memory(const Options& options = {});
    ~Store();

    /// Immutable snapshot; cheap, never blocks, many at once.
    Tx begin_read();
    /// Exclusive writer; blocks while another write Tx is open.
    Tx begin_write();

    /// Run `fn` inside a write transaction and commit it — with group
    /// commit: write() calls arriving while a commit is in flight are run
    /// back-to-back in ONE shared transaction and made durable by ONE
    /// commit (one fsync for the whole batch). A lone caller behaves
    /// exactly like begin_write() + commit(). Returns the version the
    /// changes landed in; batch-mates report the same version.
    ///
    /// Rules for `fn`:
    ///  * It may run on another write()-calling thread, not necessarily
    ///    the submitting one.
    ///  * If a batch-mate throws, the shared transaction rolls back and
    ///    every innocent closure re-runs alone — so `fn` can execute more
    ///    than once, and must not have side effects outside the store.
    ///  * A closure that throws fails only itself: its exception is
    ///    rethrown from its own write() call, the rest still commit.
    ///  * Don't commit/rollback the Tx you are given, and don't call
    ///    write()/begin_write() on the same store from inside `fn` —
    ///    both throw LogicError (the second would self-deadlock).
    uint64_t write(util::FunctionRef<void(Tx&)> fn);

    /// Rewrite the file to its minimal size (needs no live transactions).
    /// A no-op returning false for an in-memory store (nothing to rewrite).
    bool compact();
    /// The backing file path, or "<llbt in-memory>" for an in-memory store.
    std::string path() const;
    /// True if this store has no backing file (opened with open_in_memory).
    bool is_in_memory() const noexcept { return m_in_memory; }

    /// The underlying engine handle (advanced use).
    DB& raw() noexcept { return *m_db; }

private:
    Store() = default;

    /// Group-commit state (queue, leadership); see store.cpp.
    struct GroupCommitter;
    /// Take the engine write lock, drain the queue, run the batch, commit.
    void lead_batch(GroupCommitter& gc);

    std::unique_ptr<Replication> m_repl; // owns the replication in file mode
    DBRef m_db;                          // owns it in-memory (moved in at open)
    std::unique_ptr<GroupCommitter> m_gc;
    bool m_in_memory = false;
};

template <class T>
Tree<T> Tx::tree(StringData name)
{
    constexpr int64_t tag = detail::type_tag<T>::value;
    size_t i = m_reg->find(name);
    if (i == npos) {
        require_write();
        i = m_reg->add(name, tag);
    }
    else if (m_reg->type(i) != tag) {
        throw LogicError(ErrorCodes::IllegalOperation, "root exists with a different type");
    }
    auto bt = std::make_unique<BPlusTree<T>>(_impl::GroupFriend::get_alloc(*m_tr));
    if (m_writable)
        bt->set_parent(&m_reg->roots, i);
    NodeRef ref = m_reg->root(i);
    if (ref) {
        bt->init_from_ref(ref);
    }
    else {
        require_write(); // an empty root can only materialize under a writer
        bt->create();
    }
    return Tree<T>(std::move(bt), m_writable);
}

} // namespace llbt::core

#endif // LLBT_CORE_HPP
