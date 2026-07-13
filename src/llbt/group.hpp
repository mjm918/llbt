/*************************************************************************
 *
 * Copyright 2016 Realm Inc.
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
 * Copyright (c) 2026 Mohammad Julfikar
 **************************************************************************/

#ifndef LLBT_GROUP_HPP
#define LLBT_GROUP_HPP

#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <set>
#include <chrono>

#include <llbt/alloc_slab.hpp>
#include <llbt/array_string_short.hpp>
#include <llbt/keys.hpp>
#include <llbt/table_ref.hpp>
#include <llbt/exceptions.hpp>
#include <llbt/impl/cont_transact_hist.hpp>
#include <llbt/impl/output_stream.hpp>
#include <llbt/util/features.h>
#include <llbt/util/input_stream.hpp>

namespace llbt {

class DB;
class Replication;

namespace _impl {
class GroupFriend;
} // namespace _impl

/// A group is the root accessor of a storage file: the top array, the
/// free lists, the version, and the history slot live here. A group is a collection of named tables.
///
class Group : public ArrayParent {
    static constexpr StringData g_class_name_prefix = "class_";

public:
    /// Construct a free-standing group. This group instance will be
    /// in the attached state, but neither associated with a file, nor
    /// with an external memory buffer.
    Group();

    /// Attach this Group instance to the specified database file.
    ///
    /// The specified file is opened in read-only mode. This allows opening
    /// a file even when the caller lacks permission to write to that file.
    /// The opened group may still be modified freely, but the changes cannot be
    /// written back to the same file. Tt is an error if the specified
    /// file does not already exist in the file system.
    ///
    /// The file must contain a valid Barq database. In many cases invalidity
    /// will be detected and cause the InvalidDatabase exception to be thrown,
    /// but you should not rely on it.
    ///
    /// You may call write() to write the entire database to a new file. Writing
    /// the database to a new file does not end, or in any other way
    /// change the association between the Group instance and the file
    /// that was specified in the call to open().
    ///
    /// A Barq file that contains a history (see Replication::HistoryType) may
    /// be opened via Group::open(), as long as the application can ensure that
    /// there is no concurrent access to the file (see below for more on
    /// concurrency).
    ///
    /// A file that is passed to Group::open(), may not be modified by
    /// a third party until after the Group object is
    /// destroyed. Behavior is undefined if a file is modified by a
    /// third party while any Group object is associated with it.
    ///
    /// Calling open() on a Group instance that is already in the
    /// attached state has undefined behavior.
    ///
    /// Accessing a Barq database file through manual construction
    /// of a Group object does not offer any level of thread safety or
    /// transaction safety. When any of those kinds of safety are a
    /// concern, consider using a DB instead. When accessing
    /// a database file in read/write mode through a manually
    /// constructed Group object, it is entirely the responsibility of
    /// the application that the file is not accessed in any way by a
    /// third party during the life-time of that group object. It is,
    /// on the other hand, safe to concurrently access a database file
    /// by multiple manually created Group objects, as long as all of
    /// them are opened in read-only mode, and there is no other party
    /// that modifies the file concurrently.
    ///
    /// Do not call this function on a group instance that is managed
    /// by a shared group. Doing so will result in undefined behavior.
    ///
    /// Even if this function throws, it may have the side-effect of
    /// creating the specified file, and the file may get left behind
    /// in an invalid state. Of course, this can only happen if
    /// read/write mode (mode_ReadWrite) was requested, and the file
    /// did not already exist.
    ///
    /// \param file File system path to a Barq database file.
    ///
    /// \param encryption_key 32-byte key used to encrypt and decrypt
    /// the database file, or nullptr to disable encryption.
    ///
    /// \throw FileAccessError If the file could not be
    /// opened. If the reason corresponds to one of the exception
    /// types that are derived from FileAccessError, the
    /// derived exception type is thrown. Note that InvalidDatabase is
    /// among these derived exception types.
    explicit Group(const std::string& file, const char* encryption_key = nullptr);

    /// Attach this Group instance to the specified memory buffer.
    ///
    /// This is similar to constructing a group from a file except
    /// that in this case the database is assumed to be stored in the
    /// specified memory buffer.
    ///
    /// If \a take_ownership is `true`, you pass the ownership of the
    /// specified buffer to the group. In this case the buffer will
    /// eventually be freed using std::free(), so the buffer you pass,
    /// must have been allocated using std::malloc().
    ///
    /// On the other hand, if \a take_ownership is set to `false`, it
    /// is your responsibility to keep the memory buffer alive during
    /// the lifetime of the group, and in case the buffer needs to be
    /// deallocated afterwards, that is your responsibility too.
    ///
    /// If this function throws, the ownership of the memory buffer
    /// will remain with the caller, regardless of whether \a
    /// take_ownership is set to `true` or `false`.
    ///
    /// Calling open() on a Group instance that is already in the
    /// attached state has undefined behavior.
    ///
    /// Do not call this function on a group instance that is managed
    /// by a shared group. Doing so will result in undefined behavior.
    ///
    /// \throw InvalidDatabase If the specified buffer does not appear
    /// to contain a valid database.
    /// Note that if this constructor throws, the
    /// ownership of the memory buffer will remain with the caller,
    /// regardless of whether \a take_ownership is set to `true` or
    /// `false`.
    explicit Group(BinaryData, bool take_ownership = true);

    Group(const Group&) = delete;
    Group& operator=(const Group&) = delete;

    ~Group() noexcept override;

    /// A group may be created in the unattached state, and then later
    /// attached to a file with a call to open(). Calling any method
    /// other than open(), and is_attached() on an unattached instance
    /// results in undefined behavior.
    bool is_attached() const noexcept;
    /// A group is frozen only if it is actually a frozen transaction.
    virtual bool is_frozen() const noexcept
    {
        return false;
    }

    static int get_current_file_format_version()
    {
        return g_current_file_format_version;
    }

    int get_history_schema_version() noexcept;

    Replication* get_replication() const
    {
        return *get_repl();
    }

    /// The sync file id is set when a client synchronizes with the server for the
    /// first time. It is used when generating GlobalKeys for tables without a primary
    /// key, where it is used as the "hi" part. This ensures global uniqueness of
    /// GlobalKeys.
    uint64_t get_sync_file_id() const noexcept;
    void set_sync_file_id(uint64_t id);

// llbt scope cut: table layer removed.

// llbt scope cut: table layer removed.

    //@}

    // Serialization

    /// Write this database to the specified output stream.
    ///
    /// \param out The destination output stream to write to.
    ///
    /// \param pad If true, the file is padded to ensure the footer is aligned
    /// to the end of a page
    void write(std::ostream& out, bool pad = false) const;

    /// Write this database to a new file. It is an error to specify a
    /// file that already exists. This is to protect against
    /// overwriting a database file that is currently open, which
    /// would cause undefined behaviour.
    ///
    /// \param path A filesystem path to the file you want to write to.
    ///
    /// \param encryption_key 32-byte key used to encrypt the database file,
    /// or nullptr to disable encryption.
    ///
    /// \param version If different from 0, the new file will be a full fledged
    /// barq file with free list and history info. The version of the commit
    /// will be set to the value given here.
    ///
    /// \param write_history Indicates if you want the Sync Client History to
    /// be written to the file (only relevant for synchronized files).
    /// \throw FileAccessError If the file could not be
    /// opened. If the reason corresponds to one of the exception
    /// types that are derived from FileAccessError, the
    /// derived exception type is thrown. In particular,
    /// util::File::Exists will be thrown if the file exists already.
    void write(const std::string& path, const char* encryption_key = nullptr, uint64_t version = 0,
               bool write_history = true) const;

    /// Write this database to a memory buffer.
    ///
    /// Ownership of the returned buffer is transferred to the
    /// caller. The memory will have been allocated using
    /// std::malloc().
    BinaryData write_to_mem() const;

// llbt scope cut: table layer removed.

// llbt scope cut: table layer removed.

    /// Return the size taken up by the current snapshot. This is in contrast to
    /// the number returned by DB::get_stats() which will return the
    /// size of the last snapshot done in that DB. If the snapshots are
    /// identical, the numbers will of course be equal.
    size_t get_used_space() const noexcept;

    /// check that an already attached barq file is valid for read only access.
    /// if not detach the file and throw a FileFormatUpgradeRequired.
    /// return the file format version.
    static int read_only_version_check(SlabAlloc& alloc, ref_type top_ref, const std::string& path);
    void verify() const;
#ifdef LLBT_DEBUG
    void print() const;
    void print_free() const;
    MemStats get_stats();
    void enable_mem_diagnostics(bool enable = true)
    {
        m_alloc.enable_debug(enable);
    }
#endif

protected:
    static constexpr size_t s_table_name_ndx = 0;
    static constexpr size_t s_table_refs_ndx = 1;
    static constexpr size_t s_file_size_ndx = 2;
    static constexpr size_t s_free_pos_ndx = 3;
    static constexpr size_t s_free_size_ndx = 4;
    static constexpr size_t s_free_version_ndx = 5;
    static constexpr size_t s_version_ndx = 6;
    static constexpr size_t s_hist_type_ndx = 7;
    static constexpr size_t s_hist_ref_ndx = 8;
    static constexpr size_t s_hist_version_ndx = 9;
    static constexpr size_t s_sync_file_id_ndx = 10;
    static constexpr size_t s_evacuation_point_ndx = 11;

    static constexpr size_t s_group_max_size = 12;

    virtual Replication* const* get_repl() const
    {
        return &g_dummy_replication;
    }
    static Replication* g_dummy_replication;

private:
    // nullptr, if we're sharing an allocator provided during initialization
    std::unique_ptr<SlabAlloc> m_local_alloc;
    // in-use allocator. If local, then equal to m_local_alloc.
    SlabAlloc& m_alloc;

    int m_file_format_version;
    /// `m_top` is the root node (or top array) of the Barq, and has the
    /// following layout:
    ///
    /// <pre>
    ///
    ///                                                     Introduced in file
    ///   Slot  Value                                       format version
    ///   ---------------------------------------------------------------------
    ///    1st   m_table_names
    ///    2nd   m_tables
    ///    3rd   Logical file size
    ///    4th   GroupWriter::m_free_positions (optional)
    ///    5th   GroupWriter::m_free_lengths   (optional)
    ///    6th   GroupWriter::m_free_versions  (optional)
    ///    7th   Transaction number / version  (optional)
    ///    8th   History type         (optional)             4
    ///    9th   History ref          (optional)             4
    ///   10th   History version      (optional)             7
    ///   11th   Sync File Id         (optional)            10
    ///   12th   Evacuation point     (optional)            22
    ///
    /// </pre>
    ///
    /// The 'History type' slot stores a value of type
    /// Replication::HistoryType. The 'History version' slot stores a history
    /// schema version as returned by Replication::get_history_schema_version().
    ///
    /// The first three entries are mandatory. In files created by
    /// Group::write(), none of the optional entries are present and the size of
    /// `m_top` is 3. In files updated by Group::commit(), the 4th and 5th entry
    /// are present, and the size of `m_top` is 5. In files updated by way of a
    /// transaction (Transaction::commit()), the 4th, 5th, 6th, and 7th entry
    /// are present, and the size of `m_top` is 7. In files that contain a
    /// changeset history, the 8th, 9th, and 10th entry are present. The 11th entry
    /// will be present if the file is syncked and the client has received a client
    /// file id from the server.
    ///
    /// When a group accessor is attached to a newly created file or an empty
    /// memory buffer where there is no top array yet, `m_top`, `m_tables`, and
    /// `m_table_names` will be left in the detached state until the initiation
    /// of the first write transaction. In particular, they will remain in the
    /// detached state during read transactions that precede the first write
    /// transaction.
    Array m_top;
    Array m_tables;
    ArrayStringShort m_table_names;
    uint64_t m_last_seen_mapping_version = 0;

// llbt scope cut: table layer removed.
    mutable std::mutex m_accessor_mutex;
    mutable int m_num_tables = 0;
    bool m_attached = false;
    bool m_is_writable = true;
    static std::optional<int> fake_target_file_format;


    Group(SlabAlloc* alloc) noexcept;
    void init_array_parents() noexcept;

    void open(ref_type top_ref, const std::string& file_path);

    // If the underlying memory mappings have been extended, this method is used
    // to update all the tables' allocator wrappers. The allocator wrappers are
    // configure to either allow or deny changes.
    void update_allocator_wrappers(bool writable);

    /// If `top_ref` is not zero, attach this group accessor to the specified
    /// underlying node structure. If `top_ref` is zero and \a
    /// create_group_when_missing is true, create a new node structure that
    /// represents an empty group, and attach this group accessor to it.
    void attach(ref_type top_ref, bool writable, bool create_group_when_missing, size_t file_size = -1,
                uint_fast64_t version = -1);

    /// Detach this group accessor from the underlying node structure. If this
    /// group accessors is already in the detached state, this function does
    /// nothing (idempotency).
    void detach() noexcept;

    /// \param writable Must be set to true when, and only when attaching for a
    /// write transaction.
    void attach_shared(ref_type new_top_ref, size_t new_file_size, bool writable, VersionID version);

    void create_empty_group();

    void reset_free_space_tracking();

    void remap_and_update_refs(ref_type new_top_ref, size_t new_file_size, bool writable);

    /// Recursively update refs stored in all cached array
    /// accessors. This includes cached array accessors in any
    /// currently attached table accessors. This ensures that the
    /// group instance itself, as well as any attached table accessor
    /// that exists across Transaction::commit() will remain valid. This
    /// function is not appropriate for use in conjunction with
    /// commits via shared group.
    void update_refs(ref_type top_ref) noexcept;

    // Overriding method in ArrayParent
    void update_child_ref(size_t, ref_type) override;

    // Overriding method in ArrayParent
    ref_type get_child_ref(size_t) const noexcept override;

    class TableWriter;
    class DefaultTableWriter;

    static void write(std::ostream&, int file_format_version, TableWriter&, bool no_top_array,
                      bool pad_for_encryption, uint_fast64_t version_number);

// llbt scope cut: table layer removed.

    void write(util::File& file, const char* encryption_key, uint_fast64_t version_number, TableWriter& writer) const;
    void write(std::ostream&, bool pad, uint_fast64_t version_numer, TableWriter& writer) const;

    /// Memory mappings must have been updated to reflect any growth in filesize before
    /// calling advance_transact()
    void advance_transact(ref_type new_top_ref, util::InputStream*, bool writable);
    void refresh_dirty_accessors();
    void flush_accessors_for_commit();

    /// \brief The version of the format of the node structure (in file or in
    /// memory) in use by Barq objects associated with this group.
    ///
    /// Every group contains a file format version field, which is returned
    /// by this function. The file format version field is set to the file format
    /// version specified by the attached file (or attached memory buffer) at the
    /// time of attachment and the value is used to determine if a file format
    /// upgrade is required.
    ///
    /// A value of zero means that the file format is not yet decided. This is
    /// only possible for empty Barqs where top-ref is zero. (When group is created
    /// with the unattached_tag). The version number will then be determined in the
    /// subsequent call to Group::open.
    ///
    /// In shared mode (when a Barq file is opened via a DB instance)
    /// it can happen that the file format is upgraded asyncronously (via
    /// another DB instance), and in that case the file format version
    /// field can get out of date, but only for a short while. It is always
    /// guaranteed to be, and remain up to date after the opening process completes
    /// (when DB::do_open() returns).
    ///
    /// An empty Barq file (one whose top-ref is zero) may specify a file
    /// format version of zero to indicate that the format is not yet
    /// decided. In that case the file format version must be changed to a proper
    /// before the opening process completes (Group::open() or DB::open()).
    ///
    /// File format versions:
    ///
    ///   1 Initial file format version
    ///
    ///   2 Various changes.
    ///
    ///   3 Supporting null on string columns broke the file format in following
    ///     way: Index appends an 'X' character to all strings except the null
    ///     string, to be able to distinguish between null and empty
    ///     string. Bumped to 3 because of null support of String columns and
    ///     because of new format of index.
    ///
    ///   4 Introduction of optional in-Barq history of changes (additional
    ///     entries in Group::m_top). Since this change is not forward
    ///     compatible, the file format version had to be bumped. This change is
    ///     implemented in a way that achieves backwards compatibility with
    ///     version 3 (and in turn with version 2).
    ///
    ///   5 Introduced the new Timestamp column type that replaces DateTime.
    ///     When opening an older database file, all DateTime columns will be
    ///     automatically upgraded Timestamp columns.
    ///
    ///   6 Introduced a new structure for the StringIndex. Moved the commit
    ///     logs into the Barq file. Changes to the transaction log format
    ///     including reshuffling instructions. This is the format used in
    ///     milestone 2.0.0.
    ///
    ///   7 Introduced "history schema version" as 10th entry in top array.
    ///
    ///   8 Subtables can now have search index.
    ///
    ///   9 Replication instruction values shuffled, instr_MoveRow added.
    ///
    ///  10 Cluster based table layout. Memory mapping changes which require
    ///     special treatment of large files of preceding versions.
    ///
    ///  11 Same as 10, but version 11 files will have search index added on
    ///     string primary key columns.
    ///
    ///  12 - 19 Room for new file formats in legacy code.
    ///
    ///  20 New data types: Decimal128 and ObjectId. Embedded tables. Search index
    ///     is removed from primary key columns.
    ///
    ///  21 New data types: UUID, Mixed, Set and Dictionary.
    ///
    ///  22 Object keys are no longer generated from primary key values. Search index
    ///     reintroduced.
    ///
    ///  23 Layout of Set and Dictionary changed.
    ///
    ///  24 Variable sized arrays for Decimal128.
    ///     Nested collections
    ///     Backlinks in BPlusTree
    ///     Sort order of Strings changed (affects sets and the string index)
    ///
    /// IMPORTANT: When introducing a new file format version, be sure to review
    /// the file validity checks in Group::open() and DB::do_open, the file
    /// format selection logic in
    /// Group::get_target_file_format_version_for_session(), and the file format
    /// upgrade logic in Group::upgrade_file_format(), AND the lists of accepted
    /// file formats and the version deletion list residing in "backup_restore.cpp"

    static constexpr int g_current_file_format_version = 25;

    int get_file_format_version() const noexcept;
    void set_file_format_version(int) noexcept;
    int get_committed_file_format_version() const noexcept;

    /// The specified history type must be a value of Replication::HistoryType.
    static int get_target_file_format_version_for_session(int current_file_format_version, int history_type) noexcept;


    static void get_version_and_history_info(const Array& top, _impl::History::version_type& version,
                                             int& history_type, int& history_schema_version) noexcept;
    static ref_type get_history_ref(const Array& top) noexcept;
    static size_t get_logical_file_size(const Array& top) noexcept;
    static size_t get_free_space_size(const Array& top) noexcept;
    static size_t get_history_size(const Array& top) noexcept;
    size_t get_logical_file_size() const noexcept
    {
        return get_logical_file_size(m_top);
    }
    void clear_history();
    void set_history_schema_version(int version);
    template <class Accessor>
    void set_history_parent(Accessor& history_root) noexcept;
    void prepare_top_for_history(int history_type, int history_schema_version, uint64_t file_ident);
    template <class Accessor>
    void prepare_history_parent(Accessor& history_root, int history_type, int history_schema_version,
                                uint64_t file_ident);
    static void validate_top_array(const Array& arr, const SlabAlloc& alloc,
                                   std::optional<size_t> read_lock_file_size = util::none,
                                   std::optional<uint_fast64_t> read_lock_version = util::none);

// llbt scope cut: table layer removed.
    void check_attached() const
    {
        if (!is_attached())
            throw StaleAccessor("Stale transaction");
    }

    friend class DB;
    friend class GroupCommitter;
    friend class GroupWriter;
    friend class SlabAlloc;
    friend class Transaction;
    friend class _impl::GroupFriend;
};

// llbt scope cut: table layer removed.

// Implementation

inline bool Group::is_attached() const noexcept
{
    return m_attached;
}


// llbt scope cut: table layer removed.

// llbt scope cut: table layer removed.


inline void Group::init_array_parents() noexcept
{
    m_table_names.set_parent(&m_top, 0);
    m_tables.set_parent(&m_top, 1);
}

inline void Group::update_child_ref(size_t child_ndx, ref_type new_ref)
{
    m_tables.set(child_ndx, new_ref);
}

inline ref_type Group::get_child_ref(size_t child_ndx) const noexcept
{
    return m_tables.get_as_ref(child_ndx);
}

// llbt scope cut: table layer removed.

inline ref_type Group::get_history_ref(const Array& top) noexcept
{
    bool has_history = (top.is_attached() && top.size() > s_hist_type_ndx);
    if (has_history) {
        // This function is only used is shared mode (from DB)
        LLBT_ASSERT(top.size() > s_hist_version_ndx);
        return top.get_as_ref(s_hist_ref_ndx);
    }
    return 0;
}

inline size_t Group::get_logical_file_size(const Array& top) noexcept
{
    if (top.is_attached() && top.size() > s_file_size_ndx) {
        return (size_t)top.get_as_ref_or_tagged(s_file_size_ndx).get_as_int();
    }
    return 0;
}


inline void Group::set_sync_file_id(uint64_t id)
{
    while (m_top.size() < s_sync_file_id_ndx + 1)
        m_top.add(0);
    m_top.set(s_sync_file_id_ndx, RefOrTagged::make_tagged(id));
}

inline void Group::set_history_schema_version(int version)
{
    while (m_top.size() < s_hist_version_ndx + 1)
        m_top.add(0);
    m_top.set(s_hist_version_ndx, RefOrTagged::make_tagged(unsigned(version))); // Throws
}

template <class Accessor>
inline void Group::set_history_parent(Accessor& history_root) noexcept
{
    history_root.set_parent(&m_top, 8);
}

template <class Accessor>
void Group::prepare_history_parent(Accessor& history_root, int history_type, int history_schema_version,
                                   uint64_t file_ident)
{
    prepare_top_for_history(history_type, history_schema_version, file_ident);
    set_history_parent(history_root);
}

class Group::TableWriter {
public:
    struct HistoryInfo {
        ref_type ref = 0;
        int type = 0;
        int version = 0;
        uint64_t sync_file_id = 0;
    };

    virtual ref_type write_names(_impl::OutputStream&) = 0;
    virtual ref_type write_tables(_impl::OutputStream&) = 0;
    virtual HistoryInfo write_history(_impl::OutputStream&) = 0;
    virtual ~TableWriter() noexcept {}

    void set_group(const Group* g)
    {
        m_group = g;
    }

protected:
    const Group* m_group = nullptr;
};

class Group::DefaultTableWriter : public Group::TableWriter {
public:
    DefaultTableWriter(bool should_write_history = true)
        : m_should_write_history(should_write_history)
    {
    }
    ref_type write_names(_impl::OutputStream& out) override;
    ref_type write_tables(_impl::OutputStream& out) override;
    HistoryInfo write_history(_impl::OutputStream& out) override;

private:
    bool m_should_write_history;
};

// llbt scope cut: table layer removed.

inline void Group::reset_free_space_tracking()
{
    // if used whith a shared allocator, free space should never be reset through
    // Group, but rather through the proper owner of the allocator, which is the DB object.
    LLBT_ASSERT(m_local_alloc);
    m_alloc.reset_free_space_tracking(); // Throws
}

// The purpose of this class is to give internal access to some, but
// not all of the non-public parts of the Group class.
class _impl::GroupFriend {
public:
    static Allocator& get_alloc(const Group& group) noexcept
    {
        return group.m_alloc;
    }

    static ref_type get_top_ref(const Group& group) noexcept
    {
        return group.m_top.get_ref();
    }

    static ref_type get_history_ref(Allocator& alloc, ref_type top_ref) noexcept
    {
        Array top(alloc);
        if (top_ref != 0)
            top.init_from_ref(top_ref);
        return Group::get_history_ref(top);
    }

    static ref_type get_history_ref(const Group& group) noexcept
    {
        return Group::get_history_ref(group.m_top);
    }

    static size_t get_logical_file_size(Allocator& alloc, ref_type top_ref) noexcept
    {
        Array top(alloc);
        if (top_ref)
            top.init_from_ref(top_ref);
        return Group::get_logical_file_size(top);
    }

    static int get_file_format_version(const Group& group) noexcept
    {
        return group.get_file_format_version();
    }

    static void get_version_and_history_info(const Allocator& alloc, ref_type top_ref,
                                             _impl::History::version_type& version, int& history_type,
                                             int& history_schema_version) noexcept
    {
        Array top{const_cast<Allocator&>(alloc)};
        if (top_ref != 0)
            top.init_from_ref(top_ref);
        Group::get_version_and_history_info(top, version, history_type, history_schema_version);
    }

    static void set_history_schema_version(Group& group, int version)
    {
        group.set_history_schema_version(version); // Throws
    }

    template <class Accessor>
    static void set_history_parent(Group& group, Accessor& history_root) noexcept
    {
        group.set_history_parent(history_root);
    }

    template <class Accessor>
    static void prepare_history_parent(Group& group, Accessor& history_root, int history_type,
                                       int history_schema_version, uint64_t file_ident = 0)
    {
        group.prepare_history_parent(history_root, history_type, history_schema_version, file_ident); // Throws
    }


    static int get_target_file_format_version_for_session(int current_file_format_version, int history_type) noexcept
    {
        return Group::get_target_file_format_version_for_session(current_file_format_version, history_type);
    }

    static void fake_target_file_format(const std::optional<int> format) noexcept;
};


// llbt scope cut: table layer removed.

} // namespace llbt

#endif // LLBT_GROUP_HPP
