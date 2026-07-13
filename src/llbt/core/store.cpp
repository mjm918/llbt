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
** Copyright (c) 2026 Mohammad Julfikar
*/
#include <llbt/core.hpp>

#include <llbt/group.hpp>
#include <llbt/db_options.hpp>
#include <llbt/core/commit_journal.hpp>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>

using namespace llbt;
using namespace llbt::core;
using gf = llbt::_impl::GroupFriend;

// ---- Registry -------------------------------------------------------------

namespace llbt::core::detail {

void Registry::bind_read(Transaction& tr)
{
    writable = false;
    ref_type ref = gf::get_history_ref(tr);
    if (!ref)
        return; // fresh file: empty registry
    top.init_from_ref(ref);
    if (auto names_ref = top.get_as_ref(0))
        names.init_from_ref(names_ref);
    if (auto roots_ref = top.get_as_ref(1))
        roots.init_from_ref(roots_ref);
    if (auto types_ref = top.get_as_ref(2))
        types.init_from_ref(types_ref);
}

void Registry::bind_write(Transaction& tr)
{
    writable = true;
    ref_type ref = gf::get_history_ref(tr);
    if (ref) {
        // Anchor the accessor chain at the history slot, then bind children.
        gf::set_history_parent(tr, top); // parents `top` at the history slot
        top.init_from_ref(ref);
        names.set_parent(&top, 0);
        roots.set_parent(&top, 1);
        types.set_parent(&top, 2);
        names.init_from_ref(top.get_as_ref(0));
        roots.init_from_ref(top.get_as_ref(1));
        types.init_from_ref(top.get_as_ref(2));
        return;
    }

    // First write to this file: create the registry and anchor it.
    gf::prepare_history_parent(tr, top, Replication::hist_InBarq,
                               core_detail::g_llbt_roots_schema_version, 0);
    top.create(NodeHeader::type_HasRefs, false, core_detail::g_llbt_registry_size, 0);
    top.update_parent();

    names.set_parent(&top, 0);
    names.create();
    roots.set_parent(&top, 1);
    roots.create(NodeHeader::type_HasRefs);
    roots.update_parent();
    types.set_parent(&top, 2);
    types.create(NodeHeader::type_Normal);
    types.update_parent();
}

size_t Registry::count() const noexcept
{
    return names.is_attached() ? names.size() : 0;
}

size_t Registry::find(StringData name_value) const noexcept
{
    if (!names.is_attached())
        return size_t(-1);
    return names.find_first(name_value);
}

StringData Registry::name(size_t i) const noexcept
{
    return names.get(i);
}

NodeRef Registry::root(size_t i) const noexcept
{
    return roots.get_as_ref(i);
}

int64_t Registry::type(size_t i) const noexcept
{
    return types.get(i);
}

size_t Registry::add(StringData name_value, int64_t type_tag)
{
    names.add(name_value);
    roots.add(0);
    types.add(type_tag);
    return names.size() - 1;
}

void Registry::set_root_ref(size_t i, NodeRef ref)
{
    ref_type old = roots.get_as_ref(i);
    if (old && old != ref)
        Array::destroy_deep(old, roots.get_alloc());
    roots.set_as_ref(i, ref);
}

void Registry::erase(size_t i)
{
    if (ref_type old = roots.get_as_ref(i))
        Array::destroy_deep(old, roots.get_alloc());
    names.erase(i);
    roots.erase(i);
    types.erase(i);
}

} // namespace llbt::core::detail

// ---- Store ----------------------------------------------------------------

namespace llbt::core {

namespace {
// The store whose write() closure is running on this thread, if any. Lets a
// nested write()/begin_write() on the same store fail loudly instead of
// self-deadlocking on the engine write mutex (writes to a DIFFERENT store
// from inside a closure stay legal).
thread_local const Store* t_closure_store = nullptr;

struct ClosureGuard {
    const Store* prev;
    explicit ClosureGuard(const Store* s) noexcept
        : prev(t_closure_store)
    {
        t_closure_store = s;
    }
    ~ClosureGuard()
    {
        t_closure_store = prev;
    }
};
} // anonymous namespace

/// Group commit. Every write() call parks a Pending node on the queue; the
/// first thread to find no active leader leads the next batch. The leader
/// takes the engine write lock FIRST and drains the queue after — so every
/// caller that piled up while the previous commit (or an unrelated
/// begin_write()) was in flight lands in one shared transaction and is made
/// durable by one commit. Waiters block on the condvar; results travel back
/// through their own stack-allocated Pending.
struct Store::GroupCommitter {
    struct Pending {
        util::FunctionRef<void(Tx&)> fn;
        uint64_t version = 0;
        std::exception_ptr error;
        bool done = false;

        explicit Pending(util::FunctionRef<void(Tx&)> f) noexcept
            : fn(f)
        {
        }
    };

    /// Latency guard, not a throughput knob: a huge backlog is chopped into
    /// commits of this size so early submitters aren't held hostage while
    /// stragglers keep arriving.
    static constexpr size_t max_batch = 64;

    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Pending*> queue;
    bool leader_active = false;
};

StoreRef Store::open(const std::string& path, const Options& options)
{
    auto store = StoreRef(new Store());
    store->m_repl = core_detail::make_roots_replication();
    store->m_gc = std::make_unique<GroupCommitter>();

    DBOptions db_options;
    db_options.encryption_key = options.encryption_key;
    if (options.no_sync || options.durability == Durability::Unsafe)
        db_options.durability = DBOptions::Durability::Unsafe;
    else if (options.durability == Durability::Strict)
        db_options.durability = DBOptions::Durability::Strict;
    else
        db_options.durability = DBOptions::Durability::Full;
    db_options.single_process = options.single_process;
    db_options.allow_file_format_upgrade = options.allow_file_format_upgrade;
    store->m_db = DB::create(*store->m_repl, path, db_options);
    if (store->m_db->current_file_format_version() >= 25 && !store->m_db->has_commit_journal()) {
        Tx setup = store->begin_write();
        if (core_detail::CommitJournal::ensure(setup.m_reg->top))
            setup.commit();
        else
            setup.rollback();
        store->m_db->refresh_commit_journal();
    }
    return store;
}

StoreRef Store::open_in_memory(const Options&)
{
    auto store = StoreRef(new Store());
    store->m_in_memory = true;
    store->m_gc = std::make_unique<GroupCommitter>();

    // No file: the roots replication is owned by the DB (moved in), and the
    // allocator runs on an anonymous in-memory buffer. Durability::MemOnly is
    // the only mode that makes sense with no disk behind it.
    DBOptions db_options;
    db_options.durability = DBOptions::Durability::MemOnly;
    store->m_db = DB::create_in_memory(core_detail::make_roots_replication(),
                                       "<llbt in-memory>", db_options);
    return store;
}

Store::~Store() = default;

Tx Store::begin_read()
{
    return Tx(m_db->start_read(), shared_from_this(), false);
}

Tx Store::begin_write()
{
    if (t_closure_store == this)
        throw LogicError(ErrorCodes::WrongTransactionState,
                         "begin_write() inside a Store::write() closure would deadlock — "
                         "use the Tx the closure was given");
    return Tx(m_db->start_write(), shared_from_this(), true);
}

uint64_t Store::write(util::FunctionRef<void(Tx&)> fn)
{
    if (t_closure_store == this)
        throw LogicError(ErrorCodes::WrongTransactionState,
                         "write() inside a Store::write() closure would deadlock — "
                         "use the Tx the closure was given");

    GroupCommitter& gc = *m_gc;
    GroupCommitter::Pending me(fn);

    std::unique_lock<std::mutex> lock(gc.mutex);
    gc.queue.push_back(&me);
    while (!me.done) {
        if (gc.leader_active) {
            gc.cv.wait(lock, [&] {
                return me.done || !gc.leader_active;
            });
            continue;
        }
        // No commit in flight: this thread leads the next batch (which is
        // guaranteed to contain at least its own entry, in queue order).
        gc.leader_active = true;
        lock.unlock();
        lead_batch(gc);
        lock.lock();
        gc.leader_active = false;
        gc.cv.notify_all();
    }
    lock.unlock();

    if (me.error)
        std::rethrow_exception(me.error);
    return me.version;
}

void Store::lead_batch(GroupCommitter& gc)
{
    using Pending = GroupCommitter::Pending;
    Pending* batch[GroupCommitter::max_batch];
    size_t n = 0;

    auto drain = [&]() noexcept {
        std::lock_guard<std::mutex> lg(gc.mutex);
        while (n < GroupCommitter::max_batch && !gc.queue.empty()) {
            batch[n++] = gc.queue.front();
            gc.queue.pop_front();
        }
    };
    auto finish = [&](auto&& set_result) {
        std::lock_guard<std::mutex> lg(gc.mutex);
        for (size_t i = 0; i < n; ++i) {
            set_result(*batch[i]);
            batch[i]->done = true;
        }
    };

    // The engine write lock first, the queue second: everything that queued
    // up while we waited for the lock rides along in this batch.
    std::optional<Tx> tx;
    try {
        tx.emplace(begin_write());
    }
    catch (...) {
        // Nobody can run. Fail everything queued so far with the same cause;
        // later arrivals elect a fresh leader and try again.
        drain();
        auto err = std::current_exception();
        finish([&](Pending& p) {
            p.error = err;
        });
        return;
    }
    drain();
    tx->m_batched = true; // closures must not commit/rollback the shared Tx

    // Run the whole batch in the shared transaction, then commit once.
    std::exception_ptr batch_error;
    size_t thrower = n; // index of a throwing closure; n = none
    bool physical_commit_failed = false;
    {
        ClosureGuard guard(this);
        for (size_t i = 0; i < n; ++i) {
            try {
                batch[i]->fn(*tx);
            }
            catch (...) {
                batch_error = std::current_exception();
                thrower = i;
                break;
            }
        }
    }
    if (thrower == n) {
        try {
            tx->m_batched = false;
            uint64_t version = tx->commit();
            finish([&](Pending& p) {
                p.version = version;
            });
            return;
        }
        catch (...) {
            batch_error = std::current_exception();
            physical_commit_failed = true;
        }
    }

    // A closure threw, or the commit itself failed: every effect of the
    // batch is rolled back (Tx dtor), and each entry gets an individual
    // outcome. A throwing closure is NOT re-run — its fate is decided.
    tx.reset();

    // A disk, journal, or sync failure has one physical outcome for the whole
    // group. Retrying closures as separate commits could duplicate a commit
    // whose sync completed but whose publication failed.
    if (physical_commit_failed) {
        finish([&](Pending& p) {
            p.error = batch_error;
        });
        return;
    }

    if (n == 1) {
        finish([&](Pending& p) {
            p.error = batch_error;
        });
        return;
    }

    auto run_one = [&](Pending& p) noexcept {
        try {
            Tx own = Tx(m_db->start_write(), shared_from_this(), true);
            own.m_batched = true;
            {
                ClosureGuard guard(this);
                p.fn(own);
            }
            own.m_batched = false;
            p.version = own.commit();
        }
        catch (...) {
            p.error = std::current_exception();
        }
    };
    for (size_t i = 0; i < n; ++i) {
        if (i == thrower)
            batch[i]->error = batch_error;
        else
            run_one(*batch[i]);
    }
    finish([](Pending&) {});
}

bool Store::compact()
{
    if (m_in_memory)
        return false; // nothing to rewrite: no backing file
    // Packed binary leaves keep immutable base and delta blocks so updates and
    // deletes stay cheap. Fold those deltas into fresh base blocks before the
    // physical copy, otherwise compact() would preserve stale payload bytes.
    {
        Tx tx = begin_write();
        bool changed = false;
        const size_t roots = tx.m_reg->count();
        for (size_t i = 0; i < roots; ++i) {
            if (tx.m_reg->type(i) != detail::type_tag<BinaryData>::value)
                continue;
            std::string name(tx.m_reg->name(i));
            auto tree = tx.tree<BinaryData>(name);
            changed = tree.raw().normalize_packed_blobs() || changed;
        }
        if (changed)
            tx.commit();
        else
            tx.rollback();
    }
    return m_db->compact();
}

void Store::reserve(size_t bytes)
{
    if (!m_in_memory)
        m_db->reserve(bytes);
}

CommitMetrics Store::last_commit_metrics() const
{
    return m_db->get_last_commit_metrics();
}

std::string Store::path() const
{
    return m_db->get_path();
}

} // namespace llbt::core
