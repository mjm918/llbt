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
 **************************************************************************/

#include <new>
#include <algorithm>
#include <fstream>

#ifdef LLBT_DEBUG
#include <iostream>
#include <iomanip>
#endif

#include <llbt/util/file_mapper.hpp>
#include <llbt/util/memory_stream.hpp>
#include <llbt/util/thread.hpp>
#include <llbt/impl/destroy_guard.hpp>
#include <llbt/utilities.hpp>
#include <llbt/exceptions.hpp>
#include <llbt/group_writer.hpp>
#include <llbt/transaction.hpp>
#include <llbt/replication.hpp>

using namespace llbt;

// llbt: replication-less groups share this null sentinel (was Table::g_dummy_replication).
Replication* Group::g_dummy_replication = nullptr;
using namespace llbt::util;

namespace {

class Initialization {
public:
    Initialization()
    {
        llbt::cpuid_init();
    }
};

Initialization initialization;

} // anonymous namespace

Group::Group()
    : m_local_alloc(new SlabAlloc)
    , m_alloc(*m_local_alloc) // Throws
    , m_top(m_alloc)
    , m_tables(m_alloc)
    , m_table_names(m_alloc)
{
    init_array_parents();
    m_alloc.attach_empty(); // Throws
    m_file_format_version = get_target_file_format_version_for_session(0, Replication::hist_None);
    ref_type top_ref = 0; // Instantiate a new empty group
    bool create_group_when_missing = true;
    bool writable = create_group_when_missing;
    attach(top_ref, writable, create_group_when_missing); // Throws
}


Group::Group(const std::string& file_path, const char* encryption_key)
    : m_local_alloc(new SlabAlloc) // Throws
    , m_alloc(*m_local_alloc)
    , m_top(m_alloc)
    , m_tables(m_alloc)
    , m_table_names(m_alloc)
{
    init_array_parents();

    SlabAlloc::Config cfg;
    cfg.read_only = true;
    cfg.no_create = true;
    cfg.encryption_key = encryption_key;
    ref_type top_ref = m_alloc.attach_file(file_path, cfg); // Throws
    // Non-Transaction Groups always allow writing and simply don't allow
    // committing when opened in read-only mode
    m_alloc.set_read_only(false);

    open(top_ref, file_path);
}


Group::Group(BinaryData buffer, bool take_ownership)
    : m_local_alloc(new SlabAlloc) // Throws
    , m_alloc(*m_local_alloc)
    , m_top(m_alloc)
    , m_tables(m_alloc)
    , m_table_names(m_alloc)
{
    LLBT_ASSERT(buffer.data());

    init_array_parents();
    ref_type top_ref = m_alloc.attach_buffer(buffer.data(), buffer.size()); // Throws

    open(top_ref, {});

    if (take_ownership)
        m_alloc.own_buffer();
}

Group::Group(SlabAlloc* alloc) noexcept
    : m_alloc(*alloc)
    , // Throws
    m_top(m_alloc)
    , m_tables(m_alloc)
    , m_table_names(m_alloc)
{
    init_array_parents();
}

namespace {

class TableRecycler : public std::vector<Table*> {
public:
    ~TableRecycler()
    {
        LLBT_UNREACHABLE();
        // if ever enabled, remember to release Tables:
        // for (auto t : *this) {
        //    delete t;
        //}
    }
};

// We use the classic approach to construct a FIFO from two LIFO's,
// insertion is done into recycler_1, removal is done from recycler_2,
// and when recycler_2 is empty, recycler_1 is reversed into recycler_2.
// this i O(1) for each entry.
auto& g_table_recycler_1 = *new TableRecycler;
auto& g_table_recycler_2 = *new TableRecycler;
// number of tables held back before being recycled. We hold back recycling
// the latest to increase the probability of detecting race conditions
// without crashing.
const static int g_table_recycling_delay = 100;
auto& g_table_recycler_mutex = *new std::mutex;

} // namespace

// llbt scope cut: table key iteration removed.

// llbt scope cut: removed with the table layer.


// llbt scope cut: removed with the table layer.

// llbt scope cut: removed with the table layer.

// llbt scope cut: removed with the table layer.

// llbt scope cut: removed with the table layer.


int Group::get_file_format_version() const noexcept
{
    return m_file_format_version;
}


void Group::set_file_format_version(int file_format) noexcept
{
    m_file_format_version = file_format;
}


int Group::get_committed_file_format_version() const noexcept
{
    return m_alloc.get_committed_file_format_version();
}

std::optional<int> Group::fake_target_file_format;

void _impl::GroupFriend::fake_target_file_format(const std::optional<int> format) noexcept
{
    Group::fake_target_file_format = format;
}

int Group::get_target_file_format_version_for_session(int current_file_format_version,
                                                      int requested_history_type) noexcept
{
    if (Group::fake_target_file_format) {
        return *Group::fake_target_file_format;
    }
    // Note: This function is responsible for choosing the target file format
    // for a sessions. If it selects a file format that is different from
    // `current_file_format_version`, it will trigger a file format upgrade
    // process.

    // Note: `current_file_format_version` may be zero at this time, which means
    // that the file format it is not yet decided (only possible for empty
    // Barqs where top-ref is zero).

    // Please see Group::get_file_format_version() for information about the
    // individual file format versions.

    if (requested_history_type == Replication::hist_None) {
        if (current_file_format_version == 24) {
            // We are able to open these file formats in RO mode
            return current_file_format_version;
        }
    }

    return g_current_file_format_version;
}

void Group::get_version_and_history_info(const Array& top, _impl::History::version_type& version, int& history_type,
                                         int& history_schema_version) noexcept
{
    using version_type = _impl::History::version_type;
    version_type version_2 = 0;
    int history_type_2 = 0;
    int history_schema_version_2 = 0;
    if (top.is_attached()) {
        if (top.size() > s_version_ndx) {
            version_2 = version_type(top.get_as_ref_or_tagged(s_version_ndx).get_as_int());
        }
        if (top.size() > s_hist_type_ndx) {
            history_type_2 = int(top.get_as_ref_or_tagged(s_hist_type_ndx).get_as_int());
        }
        if (top.size() > s_hist_version_ndx) {
            history_schema_version_2 = int(top.get_as_ref_or_tagged(s_hist_version_ndx).get_as_int());
        }
    }
    // Version 0 is not a legal initial version, so it has to be set to 1
    // instead.
    if (version_2 == 0)
        version_2 = 1;
    version = version_2;
    history_type = history_type_2;
    history_schema_version = history_schema_version_2;
}

int Group::get_history_schema_version() noexcept
{
    bool history_schema_version = (m_top.is_attached() && m_top.size() > s_hist_version_ndx);
    if (history_schema_version) {
        return int(m_top.get_as_ref_or_tagged(s_hist_version_ndx).get_as_int());
    }
    return 0;
}

uint64_t Group::get_sync_file_id() const noexcept
{
    if (m_top.is_attached() && m_top.size() > s_sync_file_id_ndx) {
        return uint64_t(m_top.get_as_ref_or_tagged(s_sync_file_id_ndx).get_as_int());
    }
    auto repl = get_replication();
    if (repl && repl->get_history_type() == Replication::hist_SyncServer) {
        return 1;
    }
    return 0;
}

size_t Group::get_free_space_size(const Array& top) noexcept
{
    if (top.is_attached() && top.size() > s_free_size_ndx) {
        auto ref = top.get_as_ref(s_free_size_ndx);
        Array free_list_sizes(top.get_alloc());
        free_list_sizes.init_from_ref(ref);
        return size_t(free_list_sizes.get_sum());
    }
    return 0;
}

size_t Group::get_history_size(const Array& top) noexcept
{
    if (top.is_attached() && top.size() > s_hist_ref_ndx) {
        auto ref = top.get_as_ref(s_hist_ref_ndx);
        Array hist(top.get_alloc());
        hist.init_from_ref(ref);
        return hist.get_byte_size_deep();
    }
    return 0;
}

int Group::read_only_version_check(SlabAlloc& alloc, ref_type top_ref, const std::string& path)
{
    // Select file format if it is still undecided.
    auto file_format_version = alloc.get_committed_file_format_version();

    bool file_format_ok = false;
    // It is not possible to open prior file format versions without an upgrade.
    // Since a Barq file cannot be upgraded when opened in this mode
    // (we may be unable to write to the file), no earlier versions can be opened.
    // Please see Group::get_file_format_version() for information about the
    // individual file format versions.
    switch (file_format_version) {
        case 0:
            file_format_ok = (top_ref == 0);
            break;
        case g_current_file_format_version:
            file_format_ok = true;
            break;
    }
    if (LLBT_UNLIKELY(!file_format_ok))
        throw FileAccessError(ErrorCodes::FileFormatUpgradeRequired,
                              util::format("Barq file at path '%1' cannot be opened in read-only mode because it "
                                           "has a file format version (%2) which requires an upgrade",
                                           path, file_format_version),
                              path);
    return file_format_version;
}

void Group::open(ref_type top_ref, const std::string& file_path)
{
    SlabAlloc::DetachGuard dg(m_alloc);
    m_file_format_version = read_only_version_check(m_alloc, top_ref, file_path);

    Replication::HistoryType history_type = Replication::hist_None;
    int target_file_format_version = get_target_file_format_version_for_session(m_file_format_version, history_type);
    if (m_file_format_version == 0) {
        set_file_format_version(target_file_format_version);
    }
    else {
        // From a technical point of view, we could upgrade the Barq file
        // format in memory here, but since upgrading can be expensive, it is
        // currently disallowed.
        LLBT_ASSERT(target_file_format_version == m_file_format_version);
    }

    // Make all dynamically allocated memory (space beyond the attached file) as
    // available free-space.
    reset_free_space_tracking(); // Throws

    bool create_group_when_missing = true;
    bool writable = create_group_when_missing;
    attach(top_ref, writable, create_group_when_missing); // Throws
    dg.release();                                         // Do not detach after all
}

Group::~Group() noexcept
{
    // If this group accessor is detached at this point in time, it is either
    // because it is DB::m_group (m_is_shared), or it is a free-stading
    // group accessor that was never successfully opened.
    if (!m_top.is_attached())
        return;

    // Free-standing group accessor
    detach();

    // if a local allocator is set in m_local_alloc, then the destruction
    // of m_local_alloc will trigger destruction of the allocator, which will
    // verify that the allocator has been detached, so....
    if (m_local_alloc)
        m_local_alloc->detach();
}

void Group::remap_and_update_refs(ref_type new_top_ref, size_t new_file_size, bool writable)
{
    m_alloc.update_reader_view(new_file_size); // Throws
    update_allocator_wrappers(writable);

    // force update of all ref->ptr translations if the mapping has changed
    auto mapping_version = m_alloc.get_mapping_version();
    if (mapping_version != m_last_seen_mapping_version) {
        m_last_seen_mapping_version = mapping_version;
    }
    update_refs(new_top_ref);
}

void Group::validate_top_array(const Array& arr, const SlabAlloc& alloc, std::optional<size_t> read_lock_file_size,
                               std::optional<uint_fast64_t> read_lock_version)
{
    size_t top_size = arr.size();
    ref_type top_ref = arr.get_ref();

    switch (top_size) {
        // These are the valid sizes
        case 3:
        case 5:
        case 7:
        case 9:
        case 10:
        case 11:
        case 12: {
            ref_type table_names_ref = arr.get_as_ref_or_tagged(s_table_name_ndx).get_as_ref();
            ref_type tables_ref = arr.get_as_ref_or_tagged(s_table_refs_ndx).get_as_ref();
            auto logical_file_size = arr.get_as_ref_or_tagged(s_file_size_ndx).get_as_int();

            // Logical file size must never exceed actual file size.
            auto file_size = alloc.get_baseline();
            if (logical_file_size > file_size) {
                std::string err = util::format("Invalid logical file size: %1, actual file size: %2, read lock file "
                                               "size: %3, read lock version: %4",
                                               logical_file_size, file_size, read_lock_file_size, read_lock_version);
                throw InvalidDatabase(err, "");
            }
            // First two entries must be valid refs pointing inside the file
            auto invalid_ref = [logical_file_size](ref_type ref) {
                return ref == 0 || (ref & 7) || ref > logical_file_size;
            };
            if (invalid_ref(table_names_ref) || invalid_ref(tables_ref)) {
                std::string err = util::format(
                    "Invalid top array (top_ref, [0], [1]): %1, %2, %3, read lock size: %4, read lock version: %5",
                    top_ref, table_names_ref, tables_ref, read_lock_file_size, read_lock_version);
                throw InvalidDatabase(err, "");
            }
            break;
        }
        default: {
            auto logical_file_size = arr.get_as_ref_or_tagged(s_file_size_ndx).get_as_int();
            std::string err =
                util::format("Invalid top array size (ref: %1, array size: %2) file size: %3, read "
                             "lock size: %4, read lock version: %5",
                             top_ref, top_size, logical_file_size, read_lock_file_size, read_lock_version);
            throw InvalidDatabase(err, "");
            break;
        }
    }
}

void Group::attach(ref_type top_ref, bool writable, bool create_group_when_missing, size_t file_size,
                   uint_fast64_t version)
{
    LLBT_ASSERT(!m_top.is_attached());
    if (create_group_when_missing)
        LLBT_ASSERT(writable);

    // If this function throws, it must leave the group accesor in a the
    // unattached state.

    m_tables.detach();
    m_table_names.detach();
    m_is_writable = writable;

    if (top_ref != 0) {
        m_top.init_from_ref(top_ref);
        validate_top_array(m_top, m_alloc, file_size, version);
        m_table_names.init_from_parent();
        m_tables.init_from_parent();
    }
    else if (create_group_when_missing) {
        create_empty_group(); // Throws
    }
    m_attached = true;
    // llbt scope cut: no table accessors to maintain.
}


void Group::detach() noexcept
{
    // llbt scope cut: no table accessors to maintain.

    m_table_names.detach();
    m_tables.detach();
    m_top.detach();

    m_attached = false;
}

void Group::attach_shared(ref_type new_top_ref, size_t new_file_size, bool writable, VersionID version)
{
    LLBT_ASSERT_3(new_top_ref, <, new_file_size);
    LLBT_ASSERT(!is_attached());

    // update readers view of memory
    m_alloc.update_reader_view(new_file_size); // Throws
    update_allocator_wrappers(writable);

    // When `new_top_ref` is null, ask attach() to create a new node structure
    // for an empty group, but only during the initiation of write
    // transactions. When the transaction being initiated is a read transaction,
    // we instead have to leave array accessors m_top, m_tables, and
    // m_table_names in their detached state, as there are no underlying array
    // nodes to attached them to. In the case of write transactions, the nodes
    // have to be created, as they have to be ready for being modified.
    bool create_group_when_missing = writable;
    attach(new_top_ref, writable, create_group_when_missing, new_file_size, version.version); // Throws
}


// llbt scope cut: removed with the table layer.


void Group::create_empty_group()
{
    m_top.create(Array::type_HasRefs); // Throws
    _impl::DeepArrayDestroyGuard dg_top(&m_top);
    {
        m_table_names.create(); // Throws
        _impl::DestroyGuard<ArrayStringShort> dg(&m_table_names);
        m_top.add(m_table_names.get_ref()); // Throws
        dg.release();
    }
    {
        m_tables.create(Array::type_HasRefs); // Throws
        _impl::DestroyGuard<Array> dg(&m_tables);
        m_top.add(m_tables.get_ref()); // Throws
        dg.release();
    }
    size_t initial_logical_file_size = sizeof(SlabAlloc::Header);
    m_top.add(RefOrTagged::make_tagged(initial_logical_file_size)); // Throws
    dg_top.release();
}


// llbt scope cut: removed with the table layer.


// llbt scope cut: removed with the table layer.

// llbt scope cut: removed with the table layer.

// llbt scope cut: removed with the table layer.

// llbt scope cut: removed with the table layer.


// llbt scope cut: removed with the table layer.

// llbt scope cut: removed with the table layer.


// llbt scope cut: removed with the table layer.


// llbt scope cut: removed with the table layer.


// llbt scope cut: removed with the table layer.


// llbt scope cut: removed with the table layer.

// llbt scope cut: removed with the table layer.

// llbt scope cut: removed with the table layer.

// llbt scope cut: removed with the table layer.

ref_type Group::DefaultTableWriter::write_names(_impl::OutputStream& out)
{
    bool deep = true;                                                 // Deep
    bool only_if_modified = false;                                    // Always
    return m_group->m_table_names.write(out, deep, only_if_modified); // Throws
}
ref_type Group::DefaultTableWriter::write_tables(_impl::OutputStream& out)
{
    bool deep = true;                                            // Deep
    bool only_if_modified = false;                               // Always
    return m_group->m_tables.write(out, deep, only_if_modified); // Throws
}

auto Group::DefaultTableWriter::write_history(_impl::OutputStream& out) -> HistoryInfo
{
    bool deep = true;              // Deep
    bool only_if_modified = false; // Always
    ref_type history_ref = _impl::GroupFriend::get_history_ref(*m_group);
    HistoryInfo info;
    if (history_ref) {
        _impl::History::version_type version;
        int history_type, history_schema_version;
        _impl::GroupFriend::get_version_and_history_info(_impl::GroupFriend::get_alloc(*m_group),
                                                         m_group->m_top.get_ref(), version, history_type,
                                                         history_schema_version);
        LLBT_ASSERT(history_type != Replication::hist_None);
        if (!m_should_write_history || history_type == Replication::hist_None) {
            return info; // Only sync history should be preserved when writing to a new file
        }
        info.type = history_type;
        info.version = history_schema_version;
        Array history{const_cast<Allocator&>(_impl::GroupFriend::get_alloc(*m_group))};
        history.init_from_ref(history_ref);
        info.ref = history.write(out, deep, only_if_modified); // Throws
    }
    info.sync_file_id = m_group->get_sync_file_id();
    return info;
}

void Group::write(std::ostream& out, bool pad) const
{
    DefaultTableWriter table_writer;
    write(out, pad, 0, table_writer);
}

void Group::write(std::ostream& out, bool pad_for_encryption, uint_fast64_t version_number, TableWriter& writer) const
{
    LLBT_ASSERT(is_attached());
    writer.set_group(this);
    bool no_top_array = !m_top.is_attached();
    write(out, m_file_format_version, writer, no_top_array, pad_for_encryption, version_number); // Throws
}

void Group::write(File& file, const char* encryption_key, uint_fast64_t version_number, TableWriter& writer) const
{
    LLBT_ASSERT(file.get_size() == 0);

    file.set_encryption_key(encryption_key);

    // The aim is that the buffer size should be at least 1/256 of needed size but less than 64 Mb
    constexpr size_t upper_bound = 64 * 1024 * 1024;
    size_t min_space = std::min(get_used_space() >> 8, upper_bound);
    size_t buffer_size = page_size();
    while (buffer_size < min_space) {
        buffer_size <<= 1;
    }
    File::Streambuf streambuf(&file, buffer_size);

    std::ostream out(&streambuf);
    out.exceptions(std::ios_base::failbit | std::ios_base::badbit);
    write(out, encryption_key != 0, version_number, writer);
    int sync_status = streambuf.pubsync();
    LLBT_ASSERT(sync_status == 0);
}

void Group::write(const std::string& path, const char* encryption_key, uint64_t version_number,
                  bool write_history) const
{
    File file;
    int flags = 0;
    file.open(path, File::access_ReadWrite, File::create_Must, flags);
    DefaultTableWriter table_writer(write_history);
    write(file, encryption_key, version_number, table_writer);
}


BinaryData Group::write_to_mem() const
{
    LLBT_ASSERT(is_attached());

    // Get max possible size of buffer
    size_t max_size = m_alloc.get_total_size();

    auto buffer = std::unique_ptr<char[]>(new (std::nothrow) char[max_size]);
    if (!buffer)
        throw Exception(ErrorCodes::OutOfMemory, "Could not allocate memory while dumping to memory");
    MemoryOutputStream out; // Throws
    out.set_buffer(buffer.get(), buffer.get() + max_size);
    write(out); // Throws
    size_t buffer_size = out.size();
    return BinaryData(buffer.release(), buffer_size);
}


void Group::write(std::ostream& out, int file_format_version, TableWriter& table_writer, bool no_top_array,
                  bool pad_for_encryption, uint_fast64_t version_number)
{
    _impl::OutputStream out_2(out);

    // Write the file header
    SlabAlloc::Header streaming_header;
    if (no_top_array) {
        file_format_version = 0;
    }
    else if (file_format_version == 0) {
        // Use current file format version
        file_format_version = get_target_file_format_version_for_session(0, Replication::hist_None);
    }
    SlabAlloc::init_streaming_header(&streaming_header, file_format_version);
    out_2.write(reinterpret_cast<const char*>(&streaming_header), sizeof streaming_header);

    ref_type top_ref = 0;
    size_t final_file_size = sizeof streaming_header;
    if (no_top_array) {
        // Accept version number 1 as that number is (unfortunately) also used
        // to denote the empty initial state of a Barq file.
        LLBT_ASSERT(version_number == 0 || version_number == 1);
    }
    else {
        // Because we need to include the total logical file size in the
        // top-array, we have to start by writing everything except the
        // top-array, and then finally compute and write a correct version of
        // the top-array. The free-space information of the group will only be
        // included if a non-zero version number is given as parameter,
        // indicating that versioning info is to be saved. This is used from
        // DB to compact the database by writing only the live data
        // into a separate file.
        ref_type names_ref = table_writer.write_names(out_2);   // Throws
        ref_type tables_ref = table_writer.write_tables(out_2); // Throws
        SlabAlloc new_alloc;
        new_alloc.attach_empty(); // Throws
        Array top(new_alloc);
        top.create(Array::type_HasRefs); // Throws
        _impl::ShallowArrayDestroyGuard dg_top(&top);
        int_fast64_t value_1 = from_ref(names_ref);
        int_fast64_t value_2 = from_ref(tables_ref);
        top.add(value_1); // Throws
        top.add(value_2); // Throws
        top.add(0);       // Throws

        int top_size = 3;
        if (version_number) {
            TableWriter::HistoryInfo history_info = table_writer.write_history(out_2); // Throws

            Array free_list(new_alloc);
            Array size_list(new_alloc);
            Array version_list(new_alloc);
            free_list.create(Array::type_Normal); // Throws
            _impl::DeepArrayDestroyGuard dg_1(&free_list);
            size_list.create(Array::type_Normal); // Throws
            _impl::DeepArrayDestroyGuard dg_2(&size_list);
            version_list.create(Array::type_Normal); // Throws
            _impl::DeepArrayDestroyGuard dg_3(&version_list);
            bool deep = true;              // Deep
            bool only_if_modified = false; // Always
            ref_type free_list_ref = free_list.write(out_2, deep, only_if_modified);
            ref_type size_list_ref = size_list.write(out_2, deep, only_if_modified);
            ref_type version_list_ref = version_list.write(out_2, deep, only_if_modified);
            top.add(RefOrTagged::make_ref(free_list_ref));     // Throws
            top.add(RefOrTagged::make_ref(size_list_ref));     // Throws
            top.add(RefOrTagged::make_ref(version_list_ref));  // Throws
            top.add(RefOrTagged::make_tagged(version_number)); // Throws
            top_size = 7;

            if (history_info.type != Replication::hist_None) {
                top.add(RefOrTagged::make_tagged(history_info.type));
                top.add(RefOrTagged::make_ref(history_info.ref));
                top.add(RefOrTagged::make_tagged(history_info.version));
                top.add(RefOrTagged::make_tagged(history_info.sync_file_id));
                top_size = s_group_max_size;
                // ^ this is too large, since the evacuation point entry is not there:
                // (but the code below is self correcting)
            }
        }
        top_ref = out_2.get_ref_of_next_array();

        // Produce a preliminary version of the top array whose
        // representation is guaranteed to be able to hold the final file
        // size
        size_t max_top_byte_size = Array::get_max_byte_size(top_size);
        size_t max_final_file_size = size_t(top_ref) + max_top_byte_size;
        top.ensure_minimum_width(RefOrTagged::make_tagged(max_final_file_size)); // Throws

        // Finalize the top array by adding the projected final file size
        // to it
        size_t top_byte_size = top.get_byte_size();
        final_file_size = size_t(top_ref) + top_byte_size;
        top.set(2, RefOrTagged::make_tagged(final_file_size)); // Throws

        // Write the top array
        bool deep = false;                        // Shallow
        bool only_if_modified = false;            // Always
        top.write(out_2, deep, only_if_modified); // Throws
        LLBT_ASSERT_3(size_t(out_2.get_ref_of_next_array()), ==, final_file_size);

        dg_top.reset(nullptr); // Destroy now
    }

    // encryption will pad the file to a multiple of the page, so ensure the
    // footer is aligned to the end of a page
    if (pad_for_encryption) {
#if LLBT_ENABLE_ENCRYPTION
        size_t unrounded_size = final_file_size + sizeof(SlabAlloc::StreamingFooter);
        size_t rounded_size = round_up_to_page_size(unrounded_size);
        if (rounded_size != unrounded_size) {
            std::unique_ptr<char[]> buffer(new char[rounded_size - unrounded_size]());
            out_2.write(buffer.get(), rounded_size - unrounded_size);
        }
#endif
    }

    // Write streaming footer
    SlabAlloc::StreamingFooter footer;
    footer.m_top_ref = top_ref;
    footer.m_magic_cookie = SlabAlloc::footer_magic_cookie;
    out_2.write(reinterpret_cast<const char*>(&footer), sizeof footer);
}


void Group::update_refs(ref_type top_ref) noexcept
{
    // After Group::commit() we will always have free space tracking
    // info.
    LLBT_ASSERT_3(m_top.size(), >=, 5);

    m_top.init_from_ref(top_ref);

    // Now we can update it's child arrays
    m_table_names.init_from_parent();
    m_tables.init_from_parent();

    // llbt scope cut: no table accessors to maintain.
}

// llbt scope cut: removed with the table layer.
size_t Group::get_used_space() const noexcept
{
    if (!m_top.is_attached())
        return 0;

    size_t used_space = (size_t(m_top.get(2)) >> 1);

    if (m_top.size() > 4) {
        Array free_lengths(const_cast<SlabAlloc&>(m_alloc));
        free_lengths.init_from_ref(ref_type(m_top.get(4)));
        used_space -= size_t(free_lengths.get_sum());
    }

    return used_space;
}


namespace {
class TransactAdvancer : public _impl::NullInstructionObserver {
public:
    TransactAdvancer(Group&, bool& schema_changed)
        : m_schema_changed(schema_changed)
    {
    }

    bool insert_group_level_table(TableKey) noexcept
    {
        m_schema_changed = true;
        return true;
    }

    bool erase_class(TableKey) noexcept
    {
        m_schema_changed = true;
        return true;
    }

    bool rename_class(TableKey) noexcept
    {
        m_schema_changed = true;
        return true;
    }

    bool insert_column(ColKey)
    {
        m_schema_changed = true;
        return true;
    }

    bool erase_column(ColKey)
    {
        m_schema_changed = true;
        return true;
    }

    bool rename_column(ColKey) noexcept
    {
        m_schema_changed = true;
        return true; // No-op
    }

private:
    bool& m_schema_changed;
};
} // anonymous namespace


void Group::update_allocator_wrappers(bool writable)
{
    m_is_writable = writable;
    // llbt scope cut: no table accessors to maintain.
}

void Group::flush_accessors_for_commit()
{
    // llbt scope cut: no table accessors to maintain.
}

void Group::refresh_dirty_accessors()
{
    // llbt scope cut: no table accessors to refresh.
}


void Group::advance_transact(ref_type new_top_ref, util::InputStream* in, bool writable)
{
    LLBT_ASSERT(is_attached());
    // Exception safety: If this function throws, the group accessor and all of
    // its subordinate accessors are left in a state that may not be fully
    // consistent. Only minimal consistency is guaranteed (see
    // AccessorConsistencyLevels). In this case, the application is required to
    // either destroy the Group object, forcing all subordinate accessors to
    // become detached, or take some other equivalent action that involves a
    // call to Group::detach(), such as terminating the transaction in progress.
    // such actions will also lead to the detachment of all subordinate
    // accessors. Until then it is an error, and unsafe if the application
    // attempts to access the group one of its subordinate accessors.
    //
    // The purpose of this function is to refresh all attached accessors after
    // the underlying node structure has undergone arbitrary change, such as
    // when a read transaction has been advanced to a later snapshot of the
    // database.
    //
    // Initially, when this function is invoked, we cannot assume any
    // correspondence between the accessor state and the underlying node
    // structure. We can assume that the hierarchy is in a state of minimal
    // consistency, and that it can be brought to a state of structural
    // correspondence using information in the transaction logs. When structural
    // correspondence is achieved, we can reliably refresh the accessor hierarchy
    // (Table::refresh_accessor_tree()) to bring it back to a fully consistent
    // state. See AccessorConsistencyLevels.
    //
    // Much of the information in the transaction logs is not used in this
    // process, because the changes have already been applied to the underlying
    // node structure. All we need to do here is to bring the accessors back
    // into a state where they correctly reflect the underlying structure (or
    // detach them if the underlying object has been removed.)
    //
    // This is no longer needed in Core, but we need to compute "schema_changed",
    // for the benefit of ObjectStore.
    static_cast<void>(in); // llbt scope cut: no schema-change observers.

    m_top.detach();                                           // Soft detach
    bool create_group_when_missing = false;                   // See Group::attach_shared().
    attach(new_top_ref, writable, create_group_when_missing); // Throws
    refresh_dirty_accessors();                                // Throws

}

void Group::prepare_top_for_history(int history_type, int history_schema_version, uint64_t file_ident)
{
    LLBT_ASSERT(m_file_format_version >= 7);
    while (m_top.size() < s_hist_type_ndx) {
        m_top.add(0); // Throws
    }

    if (m_top.size() > s_hist_version_ndx) {
        int stored_history_type = int(m_top.get_as_ref_or_tagged(s_hist_type_ndx).get_as_int());
        int stored_history_schema_version = int(m_top.get_as_ref_or_tagged(s_hist_version_ndx).get_as_int());
        if (stored_history_type != Replication::hist_None) {
            LLBT_ASSERT(stored_history_type == history_type);
            LLBT_ASSERT(stored_history_schema_version == history_schema_version);
        }
        m_top.set(s_hist_type_ndx, RefOrTagged::make_tagged(history_type));              // Throws
        m_top.set(s_hist_version_ndx, RefOrTagged::make_tagged(history_schema_version)); // Throws
    }
    else {
        // No history yet
        LLBT_ASSERT(m_top.size() == s_hist_type_ndx);
        ref_type history_ref = 0;                                    // No history yet
        m_top.add(RefOrTagged::make_tagged(history_type));           // Throws
        m_top.add(RefOrTagged::make_ref(history_ref));               // Throws
        m_top.add(RefOrTagged::make_tagged(history_schema_version)); // Throws
    }

    if (m_top.size() > s_sync_file_id_ndx) {
        m_top.set(s_sync_file_id_ndx, RefOrTagged::make_tagged(file_ident));
    }
    else {
        m_top.add(RefOrTagged::make_tagged(file_ident)); // Throws
    }
}

void Group::clear_history()
{
    bool has_history = (m_top.is_attached() && m_top.size() > s_hist_type_ndx);
    if (has_history) {
        auto hist_ref = m_top.get_as_ref(s_hist_ref_ndx);
        Array::destroy_deep(hist_ref, m_top.get_alloc());
        m_top.set(s_hist_type_ndx, RefOrTagged::make_tagged(Replication::hist_None)); // Throws
        m_top.set(s_hist_version_ndx, RefOrTagged::make_tagged(0));                   // Throws
        m_top.set(s_hist_ref_ndx, 0);                                                 // Throws
    }
}

#ifdef LLBT_DEBUG // LCOV_EXCL_START ignore debug functions

class MemUsageVerifier : public Array::MemUsageHandler {
public:
    MemUsageVerifier(ref_type ref_begin, ref_type immutable_ref_end, ref_type mutable_ref_end, ref_type baseline)
        : m_ref_begin(ref_begin)
        , m_immutable_ref_end(immutable_ref_end)
        , m_mutable_ref_end(mutable_ref_end)
        , m_baseline(baseline)
    {
    }
    void add_immutable(ref_type ref, size_t size)
    {
        LLBT_ASSERT_3(ref % 8, ==, 0);  // 8-byte alignment
        LLBT_ASSERT_3(size % 8, ==, 0); // 8-byte alignment
        LLBT_ASSERT_3(size, >, 0);
        LLBT_ASSERT_3(ref, >=, m_ref_begin);
        LLBT_ASSERT_3(size, <=, m_immutable_ref_end - ref);
        Chunk chunk;
        chunk.ref = ref;
        chunk.size = size;
        m_chunks.push_back(chunk);
    }
    void add_mutable(ref_type ref, size_t size)
    {
        LLBT_ASSERT_3(ref % 8, ==, 0);  // 8-byte alignment
        LLBT_ASSERT_3(size % 8, ==, 0); // 8-byte alignment
        LLBT_ASSERT_3(size, >, 0);
        LLBT_ASSERT_3(ref, >=, m_immutable_ref_end);
        LLBT_ASSERT_3(size, <=, m_mutable_ref_end - ref);
        Chunk chunk;
        chunk.ref = ref;
        chunk.size = size;
        m_chunks.push_back(chunk);
    }
    void add(ref_type ref, size_t size)
    {
        LLBT_ASSERT_3(ref % 8, ==, 0);  // 8-byte alignment
        LLBT_ASSERT_3(size % 8, ==, 0); // 8-byte alignment
        LLBT_ASSERT_3(size, >, 0);
        LLBT_ASSERT_3(ref, >=, m_ref_begin);
        LLBT_ASSERT(size <= (ref < m_baseline ? m_immutable_ref_end : m_mutable_ref_end) - ref);
        Chunk chunk;
        chunk.ref = ref;
        chunk.size = size;
        m_chunks.push_back(chunk);
    }
    void add(const MemUsageVerifier& verifier)
    {
        m_chunks.insert(m_chunks.end(), verifier.m_chunks.begin(), verifier.m_chunks.end());
    }
    void handle(ref_type ref, size_t allocated, size_t) override
    {
        add(ref, allocated);
    }
    void canonicalize()
    {
        // Sort the chunks in order of increasing ref, then merge adjacent
        // chunks while checking that there is no overlap
        typedef std::vector<Chunk>::iterator iter;
        iter i_1 = m_chunks.begin(), end = m_chunks.end();
        iter i_2 = i_1;
        sort(i_1, end);
        if (i_1 != end) {
            while (++i_2 != end) {
                ref_type prev_ref_end = i_1->ref + i_1->size;
                LLBT_ASSERT_3(prev_ref_end, <=, i_2->ref);
                if (i_2->ref == prev_ref_end) { // in-file
                    i_1->size += i_2->size;     // Merge
                }
                else {
                    *++i_1 = *i_2;
                }
            }
            m_chunks.erase(i_1 + 1, end);
        }
    }
    void clear()
    {
        m_chunks.clear();
    }
    void check_total_coverage()
    {
        LLBT_ASSERT_3(m_chunks.size(), ==, 1);
        LLBT_ASSERT_3(m_chunks.front().ref, ==, m_ref_begin);
        LLBT_ASSERT_3(m_chunks.front().size, ==, m_mutable_ref_end - m_ref_begin);
    }

private:
    struct Chunk {
        ref_type ref;
        size_t size;
        bool operator<(const Chunk& c) const
        {
            return ref < c.ref;
        }
    };
    std::vector<Chunk> m_chunks;
    ref_type m_ref_begin, m_immutable_ref_end, m_mutable_ref_end, m_baseline;
};

#endif

void Group::verify() const
{
#ifdef LLBT_DEBUG
    LLBT_ASSERT(is_attached());

    m_alloc.verify();

    if (!m_top.is_attached()) {
        return;
    }

    // Verify tables
    {
        auto keys = get_table_keys();
        for (auto key : keys) {
            ConstTableRef table = get_table(key);
            LLBT_ASSERT_3(table->get_key().value, ==, key.value);
            table->verify();
        }
    }

    // Verify history if present
    if (Replication* repl = *get_repl()) {
        if (auto hist = repl->_create_history_read()) {
            hist->set_group(const_cast<Group*>(this), false);
            _impl::History::version_type version = 0;
            int history_type = 0;
            int history_schema_version = 0;
            get_version_and_history_info(m_top, version, history_type, history_schema_version);
            LLBT_ASSERT(history_type != Replication::hist_None || history_schema_version == 0);
            ref_type hist_ref = get_history_ref(m_top);
            hist->update_from_ref_and_version(hist_ref, version);
            hist->verify();
        }
    }

    if (auto tr = dynamic_cast<const Transaction*>(this)) {
        // This is a transaction
        if (tr->get_transact_stage() == DB::TransactStage::transact_Reading) {
            // Verifying the memory cannot be done from a read transaction
            // There might be a write transaction running that has freed some
            // memory that is seen as being in use in this transaction
            return;
        }
    }
    size_t logical_file_size = to_size_t(m_top.get_as_ref_or_tagged(2).get_as_int());
    size_t ref_begin = sizeof(SlabAlloc::Header);
    ref_type real_immutable_ref_end = logical_file_size;
    ref_type real_mutable_ref_end = m_alloc.get_total_size();
    ref_type real_baseline = m_alloc.get_baseline();
    // Fake that any empty area between the file and slab is part of the file (immutable):
    ref_type immutable_ref_end = m_alloc.align_size_to_section_boundary(real_immutable_ref_end);
    ref_type mutable_ref_end = m_alloc.align_size_to_section_boundary(real_mutable_ref_end);
    ref_type baseline = m_alloc.align_size_to_section_boundary(real_baseline);

    // Check the consistency of the allocation of used memory
    MemUsageVerifier mem_usage_1(ref_begin, immutable_ref_end, mutable_ref_end, baseline);
    m_top.report_memory_usage(mem_usage_1);
    mem_usage_1.canonicalize();

    // Check concistency of the allocation of the immutable memory that was
    // marked as free before the file was opened.
    MemUsageVerifier mem_usage_2(ref_begin, immutable_ref_end, mutable_ref_end, baseline);
    {
        LLBT_ASSERT_EX(m_top.size() == 3 || m_top.size() == 5 || m_top.size() == 7 || m_top.size() >= 10,
                        m_top.size());
        Allocator& alloc = m_top.get_alloc();
        Array pos(alloc), len(alloc), ver(alloc);
        pos.set_parent(const_cast<Array*>(&m_top), s_free_pos_ndx);
        len.set_parent(const_cast<Array*>(&m_top), s_free_size_ndx);
        ver.set_parent(const_cast<Array*>(&m_top), s_free_version_ndx);
        if (m_top.size() > s_free_pos_ndx) {
            if (ref_type ref = m_top.get_as_ref(s_free_pos_ndx))
                pos.init_from_ref(ref);
        }
        if (m_top.size() > s_free_size_ndx) {
            if (ref_type ref = m_top.get_as_ref(s_free_size_ndx))
                len.init_from_ref(ref);
        }
        if (m_top.size() > s_free_version_ndx) {
            if (ref_type ref = m_top.get_as_ref(s_free_version_ndx))
                ver.init_from_ref(ref);
        }
        LLBT_ASSERT(pos.is_attached() == len.is_attached());
        LLBT_ASSERT(pos.is_attached() || !ver.is_attached()); // pos.is_attached() <== ver.is_attached()
        if (pos.is_attached()) {
            size_t n = pos.size();
            LLBT_ASSERT_3(n, ==, len.size());
            if (ver.is_attached())
                LLBT_ASSERT_3(n, ==, ver.size());
            for (size_t i = 0; i != n; ++i) {
                ref_type ref = to_ref(pos.get(i));
                size_t size_of_i = to_size_t(len.get(i));
                mem_usage_2.add_immutable(ref, size_of_i);
            }
            mem_usage_2.canonicalize();
            mem_usage_1.add(mem_usage_2);
            mem_usage_1.canonicalize();
            mem_usage_2.clear();
        }
    }

    // Check the concistency of the allocation of the immutable memory that has
    // been marked as free after the file was opened
    for (const auto& free_block : m_alloc.m_free_read_only) {
        mem_usage_2.add_immutable(free_block.first, free_block.second);
    }
    mem_usage_2.canonicalize();
    mem_usage_1.add(mem_usage_2);
    mem_usage_1.canonicalize();
    mem_usage_2.clear();

    // Check the consistency of the allocation of the mutable memory that has
    // been marked as free
    m_alloc.for_all_free_entries([&](ref_type ref, size_t sz) {
        mem_usage_2.add_mutable(ref, sz);
    });
    mem_usage_2.canonicalize();
    mem_usage_1.add(mem_usage_2);
    mem_usage_1.canonicalize();
    mem_usage_2.clear();

    // There may be a hole between the end of file and the beginning of the slab area.
    // We need to take that into account here.
    LLBT_ASSERT_3(real_immutable_ref_end, <=, real_baseline);
    auto slab_start = immutable_ref_end;
    if (real_immutable_ref_end < slab_start) {
        ref_type ref = real_immutable_ref_end;
        size_t corrected_size = slab_start - real_immutable_ref_end;
        mem_usage_1.add_immutable(ref, corrected_size);
        mem_usage_1.canonicalize();
    }

    // At this point we have accounted for all memory managed by the slab
    // allocator
    mem_usage_1.check_total_coverage();
#endif
}

// llbt scope cut: removed with the table layer.

#ifdef LLBT_DEBUG

MemStats Group::get_stats()
{
    MemStats mem_stats;
    m_top.stats(mem_stats);

    return mem_stats;
}

void Group::print() const
{
    m_alloc.print();
}


void Group::print_free() const
{
    Allocator& alloc = m_top.get_alloc();
    Array pos(alloc), len(alloc), ver(alloc);
    pos.set_parent(const_cast<Array*>(&m_top), s_free_pos_ndx);
    len.set_parent(const_cast<Array*>(&m_top), s_free_size_ndx);
    ver.set_parent(const_cast<Array*>(&m_top), s_free_version_ndx);
    if (m_top.size() > s_free_pos_ndx) {
        if (ref_type ref = m_top.get_as_ref(s_free_pos_ndx))
            pos.init_from_ref(ref);
    }
    if (m_top.size() > s_free_size_ndx) {
        if (ref_type ref = m_top.get_as_ref(s_free_size_ndx))
            len.init_from_ref(ref);
    }
    if (m_top.size() > s_free_version_ndx) {
        if (ref_type ref = m_top.get_as_ref(s_free_version_ndx))
            ver.init_from_ref(ref);
    }

    if (!pos.is_attached()) {
        std::cout << "none\n";
        return;
    }
    bool has_versions = ver.is_attached();

    size_t n = pos.size();
    for (size_t i = 0; i != n; ++i) {
        size_t offset = to_size_t(pos.get(i));
        size_t size_of_i = to_size_t(len.get(i));
        std::cout << i << ": " << offset << " " << size_of_i;

        if (has_versions) {
            size_t version = to_size_t(ver.get(i));
            std::cout << " " << version;
        }
        std::cout << "\n";
    }
    std::cout << "\n";
}
#endif

// LCOV_EXCL_STOP ignore debug functions
