/*************************************************************************
 *
 * Copyright 2022 Realm Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 **************************************************************************/

#include <llbt/transaction.hpp>
#include <llbt/group_writer.hpp>

namespace {

using namespace llbt;
// llbt scope cut: object-graph replication helpers removed with the table layer.

} // namespace

namespace llbt {

std::map<DB::TransactStage, const char*> log_stage = {
    {DB::TransactStage::transact_Frozen, "frozen"},
    {DB::TransactStage::transact_Writing, "write"},
    {DB::TransactStage::transact_Reading, "read"},
};

Transaction::Transaction(DBRef _db, SlabAlloc* alloc, DB::ReadLockInfo& rli, DB::TransactStage stage)
    : Group(alloc)
    , db(_db)
    , m_read_lock(rli)
    , m_log_id(util::gen_log_id(this))
{
    bool writable = stage == DB::transact_Writing;
    m_transact_stage = DB::transact_Ready;
    set_transact_stage(stage);
    attach_shared(m_read_lock.m_top_ref, m_read_lock.m_file_size, writable,
                  VersionID{rli.m_version, rli.m_reader_idx});
    if (db->m_logger) {
        db->m_logger->log(util::LogCategory::transaction, util::Logger::Level::trace, "Start %1 %2: %3 ref %4",
                          log_stage[stage], m_log_id, rli.m_version, m_read_lock.m_top_ref);
    }
}

Transaction::~Transaction()
{
    // Note that this does not call close() - calling close() is done
    // implicitly by the deleter.
}

void Transaction::close()
{
    if (m_transact_stage == DB::transact_Writing) {
        rollback();
    }
    if (m_transact_stage == DB::transact_Reading || m_transact_stage == DB::transact_Frozen) {
        do_end_read();
    }
}

size_t Transaction::get_commit_size() const
{
    size_t sz = 0;
    if (m_transact_stage == DB::transact_Writing) {
        sz = m_alloc.get_commit_size();
    }
    return sz;
}

DB::version_type Transaction::commit()
{
    check_attached();

    if (m_transact_stage != DB::transact_Writing)
        throw WrongTransactionState("Not a write transaction");

    LLBT_ASSERT(is_attached());

    // before committing, allow any accessors at group level or below to sync
    flush_accessors_for_commit();

    DB::version_type new_version = db->do_commit(*this); // Throws

    // We need to set m_read_lock in order for wait_for_change to work.
    // low_level_commit recorded the committed snapshot while the write lock
    // still made it the newest version, so no grab/release round trip
    // through the version list is needed (a grabbed lock would be released
    // right away anyway — m_read_lock only carries the version numbers).
    DB::ReadLockInfo lock_after_commit = m_post_commit_read_lock;

    db->end_write_on_correct_thread();

    DBRef db_ref = db; // do_end_read() drops our DB reference; the epilogue needs it

    do_end_read();
    m_read_lock = lock_after_commit;

    // The transaction just ended — a quiet moment for any compaction that
    // in-transaction maintenance (e.g. a vector-index rebuild) asked for.
    db_ref->try_hinted_compaction();

    return new_version;
}

void Transaction::rollback()
{
    // rollback may happen as a consequence of exception handling in cases where
    // the DB has detached. If so, just back out without trying to change state.
    // the DB object has already been closed and no further processing is possible.
    if (!is_attached())
        return;
    if (m_transact_stage == DB::transact_Ready)
        return; // Idempotency

    if (m_transact_stage != DB::transact_Writing)
        throw WrongTransactionState("Not a write transaction");
    db->reset_free_space_tracking();
    if (!holds_write_mutex())
        db->end_write_on_correct_thread();

    do_end_read();
}

void Transaction::end_read()
{
    if (m_transact_stage == DB::transact_Ready)
        return;
    if (m_transact_stage == DB::transact_Writing)
        throw WrongTransactionState("Illegal end_read when in write mode");
    do_end_read();
}

VersionID Transaction::commit_and_continue_as_read(bool commit_to_disk)
{
    check_attached();
    if (m_transact_stage != DB::transact_Writing)
        throw WrongTransactionState("Not a write transaction");

    flush_accessors_for_commit();

    DB::version_type version = db->do_commit(*this, commit_to_disk); // Throws

    // advance read lock but dont update accessors:
    // As this is done under lock, along with the addition above of the newest commit,
    // we know for certain that the read lock we will grab WILL refer to our own newly
    // completed commit.

    try {
        // Grabbing the new lock before releasing the old one prevents m_transaction_count
        // from going shortly to zero
        DB::ReadLockInfo new_read_lock = db->grab_read_lock(DB::ReadLockInfo::Live, VersionID()); // Throws

        m_history = nullptr;
        set_transact_stage(DB::transact_Reading);

        if (commit_to_disk || m_oldest_version_not_persisted) {
            // Here we are either committing to disk or we are already
            // holding on to an older version. In either case there is
            // no need to hold onto this now historic version.
            db->release_read_lock(m_read_lock);
        }
        else {
            // We are not commiting to disk and there is no older
            // version not persisted, so hold onto this one
            m_oldest_version_not_persisted = m_read_lock;
        }

        if (commit_to_disk && m_oldest_version_not_persisted) {
            // We are committing to disk so we can release the
            // version we are holding on to
            db->release_read_lock(*m_oldest_version_not_persisted);
            m_oldest_version_not_persisted.reset();
        }
        m_read_lock = new_read_lock;
        // We can be sure that m_read_lock != m_oldest_version_not_persisted
        // because m_oldest_version_not_persisted is either equal to former m_read_lock
        // or older and former m_read_lock is older than current m_read_lock
        LLBT_ASSERT(!m_oldest_version_not_persisted ||
                     m_read_lock.m_version != m_oldest_version_not_persisted->m_version);

        {
            util::CheckedLockGuard lock(m_async_mutex);
            LLBT_ASSERT(m_async_stage != AsyncState::Syncing);
            if (commit_to_disk) {
                if (m_async_stage == AsyncState::Requesting) {
                    m_async_stage = AsyncState::HasLock;
                }
                else {
                    db->end_write_on_correct_thread();
                    m_async_stage = AsyncState::Idle;
                }
            }
            else {
                m_async_stage = AsyncState::HasCommits;
            }
        }

        // Remap file if it has grown, and update refs in underlying node structure.
        remap_and_update_refs(m_read_lock.m_top_ref, m_read_lock.m_file_size, false); // Throws
        return VersionID{version, new_read_lock.m_reader_idx};
    }
    catch (std::exception& e) {
        if (db->m_logger) {
            db->m_logger->log(util::LogCategory::transaction, util::Logger::Level::error,
                              "Tr %1: Commit failed with exception: \"%2\"", m_log_id, e.what());
        }
        // In case of failure, further use of the transaction for reading is unsafe
        set_transact_stage(DB::transact_Ready);
        throw;
    }
}

VersionID Transaction::commit_and_continue_writing()
{
    check_attached();
    if (m_transact_stage != DB::transact_Writing)
        throw WrongTransactionState("Not a write transaction");

    // before committing, allow any accessors at group level or below to sync
    flush_accessors_for_commit();

    DB::version_type version = db->do_commit(*this); // Throws

    // We need to set m_read_lock in order for wait_for_change to work.
    // To set it, we grab a readlock on the latest available snapshot
    // and release it again.
    DB::ReadLockInfo lock_after_commit = db->grab_read_lock(DB::ReadLockInfo::Live, VersionID());
    db->release_read_lock(m_read_lock);
    m_read_lock = lock_after_commit;
    if (Replication* repl = db->get_replication()) {
        bool history_updated = false;
        repl->initiate_transact(*this, lock_after_commit.m_version, history_updated); // Throws
    }

    bool writable = true;
    remap_and_update_refs(m_read_lock.m_top_ref, m_read_lock.m_file_size, writable); // Throws
    return VersionID{version, lock_after_commit.m_reader_idx};
}

TransactionRef Transaction::freeze()
{
    if (m_transact_stage != DB::transact_Reading)
        throw WrongTransactionState("Can only freeze a read transaction");
    auto version = VersionID(m_read_lock.m_version, m_read_lock.m_reader_idx);
    return db->start_frozen(version);
}

// llbt scope cut: removed with the table layer.

// llbt scope cut: removed with the table layer.

_impl::History* Transaction::get_history() const
{
    if (!m_history) {
        if (auto repl = db->get_replication()) {
            switch (m_transact_stage) {
                case DB::transact_Reading:
                case DB::transact_Frozen:
                    if (!m_history_read)
                        m_history_read = repl->_create_history_read();
                    m_history = m_history_read.get();
                    m_history->set_group(const_cast<Transaction*>(this), false);
                    break;
                case DB::transact_Writing:
                    m_history = repl->_get_history_write();
                    break;
                case DB::transact_Ready:
                    break;
            }
        }
    }
    return m_history;
}

// llbt scope cut: accessor handover removed with the table layer.

void Transaction::upgrade_file_format(int target_file_format_version)
{
    // llbt scope cut: upgrades from table-era file formats need the removed
    // table layer. llbt creates format-%1 files only; refuse anything older.
    LLBT_ASSERT(is_attached());
    if (fake_target_file_format && *fake_target_file_format == target_file_format_version) {
        return; // Testing, mockup scenario, not a real upgrade. Just pretend we're done!
    }
    if (get_file_format_version() != target_file_format_version) {
        throw FileAccessError(ErrorCodes::FileFormatUpgradeRequired,
                              "llbt cannot upgrade files created by the table-era format", db->get_path());
    }
}

void Transaction::promote_to_async()
{
    util::CheckedLockGuard lck(m_async_mutex);
    if (m_async_stage == AsyncState::Idle) {
        m_async_stage = AsyncState::HasLock;
    }
}

// llbt scope cut: replicate() removed with the table layer.

void Transaction::complete_async_commit()
{
    // sync to disk:
    DB::ReadLockInfo read_lock;
    try {
        read_lock = db->grab_read_lock(DB::ReadLockInfo::Live, VersionID());
        if (db->m_logger) {
            db->m_logger->log(util::LogCategory::transaction, util::Logger::Level::trace,
                              "Tr %1: Committing ref %2 to disk", m_log_id, read_lock.m_top_ref);
        }
        // Async commits first publish an in-memory version with Unsafe
        // durability, then this path makes it durable. Reuse the DB's window
        // cache so encrypted/data pages are flushed before the header.
        GroupCommitter out(*this, DBOptions::Durability::Full, nullptr, db->m_write_window_mgr.get());
        out.commit(read_lock.m_top_ref); // Throws
        // we must release the write mutex before the callback, because the callback
        // is allowed to re-request it.
        db->release_read_lock(read_lock);
        if (m_oldest_version_not_persisted) {
            db->release_read_lock(*m_oldest_version_not_persisted);
            m_oldest_version_not_persisted.reset();
        }
    }
    catch (const std::exception& e) {
        if (db->m_write_window_mgr) {
            db->m_write_window_mgr->clear_failed_mappings();
            db->m_write_window_mgr.reset();
        }
        m_commit_exception = std::current_exception();
        if (db->m_logger) {
            db->m_logger->log(util::LogCategory::transaction, util::Logger::Level::error,
                              "Tr %1: Committing to disk failed with exception: \"%2\"", m_log_id, e.what());
        }
        m_async_commit_has_failed = true;
        db->release_read_lock(read_lock);
    }
}

void Transaction::async_complete_writes(util::UniqueFunction<void()> when_synchronized)
{
    util::CheckedLockGuard lck(m_async_mutex);
    if (m_async_stage == AsyncState::HasLock) {
        // Nothing to commit to disk - just release write lock
        m_async_stage = AsyncState::Idle;
        db->async_end_write();
    }
    else if (m_async_stage == AsyncState::HasCommits) {
        m_async_stage = AsyncState::Syncing;
        m_commit_exception = std::exception_ptr();
        // get a callback on the helper thread, in which to sync to disk
        db->async_sync_to_disk([this, cb = std::move(when_synchronized)]() noexcept {
            complete_async_commit();
            util::CheckedLockGuard lck(m_async_mutex);
            m_async_stage = AsyncState::Idle;
            if (m_waiting_for_sync) {
                m_waiting_for_sync = false;
                m_async_cv.notify_all();
            }
            else {
                cb();
            }
        });
    }
}

void Transaction::prepare_for_close()
{
    util::CheckedLockGuard lck(m_async_mutex);
    switch (m_async_stage) {
        case AsyncState::Idle:
            break;

        case AsyncState::Requesting:
            // We don't have the ability to cancel a wait on the write lock, so
            // unfortunately we have to wait for it to be acquired.
            LLBT_ASSERT(m_transact_stage == DB::transact_Reading);
            LLBT_ASSERT(!m_oldest_version_not_persisted);
            m_waiting_for_write_lock = true;
            m_async_cv.wait(lck.native_handle(), [this]() REQUIRES(m_async_mutex) {
                return !m_waiting_for_write_lock;
            });
            db->end_write_on_correct_thread();
            break;

        case AsyncState::HasLock:
            // We have the lock and are currently in a write transaction, and
            // also may have some pending previous commits to write
            if (m_transact_stage == DB::transact_Writing) {
                db->reset_free_space_tracking();
                m_transact_stage = DB::transact_Reading;
            }
            if (m_oldest_version_not_persisted) {
                complete_async_commit();
            }
            db->end_write_on_correct_thread();
            break;

        case AsyncState::HasCommits:
            // We have commits which need to be synced to disk, so do that
            LLBT_ASSERT(m_transact_stage == DB::transact_Reading);
            complete_async_commit();
            db->end_write_on_correct_thread();
            break;

        case AsyncState::Syncing:
            // The worker thread is currently writing, so wait for it to complete
            LLBT_ASSERT(m_transact_stage == DB::transact_Reading);
            m_waiting_for_sync = true;
            m_async_cv.wait(lck.native_handle(), [this]() REQUIRES(m_async_mutex) {
                return !m_waiting_for_sync;
            });
            break;
    }
    m_async_stage = AsyncState::Idle;
}

void Transaction::acquire_write_lock()
{
    util::CheckedUniqueLock lck(m_async_mutex);
    switch (m_async_stage) {
        case AsyncState::Idle:
            lck.unlock();
            db->do_begin_possibly_async_write();
            return;

        case AsyncState::Requesting:
            m_waiting_for_write_lock = true;
            m_async_cv.wait(lck.native_handle(), [this]() REQUIRES(m_async_mutex) {
                return !m_waiting_for_write_lock;
            });
            return;

        case AsyncState::HasLock:
        case AsyncState::HasCommits:
            return;

        case AsyncState::Syncing:
            m_waiting_for_sync = true;
            m_async_cv.wait(lck.native_handle(), [this]() REQUIRES(m_async_mutex) {
                return !m_waiting_for_sync;
            });
            lck.unlock();
            db->do_begin_possibly_async_write();
            break;
    }
}

void Transaction::do_end_read() noexcept
{
    if (db->m_logger)
        db->m_logger->log(util::LogCategory::transaction, util::Logger::Level::trace, "End transaction %1", m_log_id);

    prepare_for_close();
    detach();

    // We should always be ensuring that async commits finish before we get here,
    // but if the fsync() failed or we failed to update the top pointer then
    // there's not much we can do and we have to just accept that we're losing
    // those commits.
    if (m_oldest_version_not_persisted) {
        LLBT_ASSERT(m_async_commit_has_failed);
        // We need to not release our read lock on m_oldest_version_not_persisted
        // as that's the version the top pointer is referencing and overwriting
        // that version will corrupt the Barq file.
        db->leak_read_lock(*m_oldest_version_not_persisted);
    }
    db->release_read_lock(m_read_lock);

    set_transact_stage(DB::transact_Ready);
    // reset the std::shared_ptr to allow the DB object to release resources
    // as early as possible.
    db.reset();
}

// This is the same as do_end_read() above, but with the requirement that
// 1) This is called with the db->mutex locked already
// 2) No async commits outstanding
void Transaction::close_read_with_lock()
{
    LLBT_ASSERT(m_transact_stage == DB::transact_Reading);
    {
        util::CheckedLockGuard lck(m_async_mutex);
        LLBT_ASSERT_EX(m_async_stage == AsyncState::Idle, size_t(m_async_stage));
    }

    detach();
    LLBT_ASSERT_EX(!m_oldest_version_not_persisted, m_oldest_version_not_persisted->m_type,
                    m_oldest_version_not_persisted->m_version, m_oldest_version_not_persisted->m_top_ref,
                    m_oldest_version_not_persisted->m_file_size);
    db->do_release_read_lock(m_read_lock);

    set_transact_stage(DB::transact_Ready);
    // reset the std::shared_ptr to allow the DB object to release resources
    // as early as possible.
    db.reset();
}


void Transaction::initialize_replication()
{
    if (m_transact_stage == DB::transact_Writing) {
        if (Replication* repl = get_replication()) {
            auto current_version = m_read_lock.m_version;
            bool history_updated = false;
            repl->initiate_transact(*this, current_version, history_updated); // Throws
        }
    }
}

void Transaction::set_transact_stage(DB::TransactStage stage) noexcept
{
    m_transact_stage = stage;
}

class NodeTree {
public:
    NodeTree(size_t evac_limit, size_t work_limit)
        : m_evac_limit(evac_limit)
        , m_work_limit(int64_t(work_limit))
        , m_moved(0)
    {
    }
    ~NodeTree()
    {
        // std::cout << "Moved: " << m_moved << std::endl;
    }

    /// Function used to traverse the node tree and "copy on write" nodes
    /// that are found above the evac_limit. The function will return
    /// when either the whole tree has been travesed or when the work_limit
    /// has been reached.
    /// \param current_node - node to process.
    /// \param level - the level at which current_node is placed in the tree
    /// \param progress - When the traversal is initiated, this vector identifies at which
    ///                   node the process should be resumed. It is subesequently updated
    ///                   to point to the node we have just processed
    bool trv(Array& current_node, unsigned level, std::vector<size_t>& progress)
    {
        if (m_work_limit < 0) {
            return false;
        }
        if (current_node.is_read_only()) {
            size_t byte_size = current_node.get_byte_size();
            if ((current_node.get_ref() + byte_size) > m_evac_limit) {
                current_node.copy_on_write();
                m_moved++;
                m_work_limit -= byte_size;
            }
        }

        if (current_node.has_refs()) {
            auto sz = current_node.size();
            m_work_limit -= sz;
            if (progress.size() == level) {
                progress.push_back(0);
            }
            LLBT_ASSERT_EX(level < progress.size(), level, progress.size());
            size_t ndx = progress[level];
            while (ndx < sz) {
                auto val = current_node.get(ndx);
                if (val && !(val & 1)) {
                    Array arr(current_node.get_alloc());
                    arr.set_parent(&current_node, ndx);
                    arr.init_from_parent();
                    if (!trv(arr, level + 1, progress)) {
                        return false;
                    }
                }
                ndx = ++progress[level];
            }
            while (progress.size() > level)
                progress.pop_back();
        }
        return true;
    }

private:
    size_t m_evac_limit;
    int64_t m_work_limit;
    size_t m_moved;
};


void Transaction::cow_outliers(std::vector<size_t>& progress, size_t evac_limit, size_t work_limit)
{
    NodeTree node_tree(evac_limit, work_limit);
    if (progress.empty()) {
        progress.push_back(s_table_name_ndx);
    }
    if (progress[0] == s_table_name_ndx) {
        if (!node_tree.trv(m_table_names, 1, progress))
            return;
        progress.back() = s_table_refs_ndx; // Handle tables next
    }
    if (progress[0] == s_table_refs_ndx) {
        if (!node_tree.trv(m_tables, 1, progress))
            return;
        progress.back() = s_hist_ref_ndx; // Handle history next
    }
    if (progress[0] == s_hist_ref_ndx && m_top.get(s_hist_ref_ndx)) {
        Array hist_arr(m_top.get_alloc());
        hist_arr.set_parent(&m_top, s_hist_ref_ndx);
        hist_arr.init_from_parent();
        if (!node_tree.trv(hist_arr, 1, progress))
            return;
    }
    progress.clear();
}

} // namespace llbt
