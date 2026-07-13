/*
** llbt — Low Level Binary Tree
** Copyright (c) 2026 Mohammad Julfikar
** Dedicated to the public domain. See LICENSE and NOTICE.
*/
#include <llbt/core/commit_recovery.hpp>

#include <llbt/alloc_slab.hpp>
#include <llbt/array.hpp>
#include <llbt/core/commit_journal.hpp>
#include <llbt/core/commit_journal_format.hpp>
#include <llbt/core/crc32c.hpp>
#include <llbt/exceptions.hpp>
#include <llbt/group.hpp>
#include <llbt/node_header.hpp>

#include <algorithm>
#include <cstring>

namespace llbt::core_detail {

using namespace journal_format;

CommitRecovery::CommitRecovery(SlabAlloc& alloc, const std::vector<ref_type>& chunks) noexcept
    : m_alloc(alloc)
    , m_chunks(chunks)
{
}

void CommitRecovery::read_bytes(size_t offset, void* output, size_t size) const
{
    char* out = static_cast<char*>(output);
    while (size) {
        size_t chunk_index = offset / CommitJournal::chunk_size;
        size_t in_chunk = offset % CommitJournal::chunk_size;
        size_t count = std::min(size, CommitJournal::chunk_size - in_chunk);
        const char* source = m_alloc.translate(m_chunks.at(chunk_index)) + NodeHeader::header_size + in_chunk;
        std::memcpy(out, source, count);
        out += count;
        offset += count;
        size -= count;
    }
}

CommitRecovery::Result CommitRecovery::scan(ref_type checkpoint_top) const
{
    Result result;
    result.top_ref = checkpoint_top;
    result.logical_size = _impl::GroupFriend::get_logical_file_size(m_alloc, checkpoint_top);
    int history_type = 0;
    int history_schema = 0;
    _impl::GroupFriend::get_version_and_history_info(m_alloc, checkpoint_top, result.version,
                                                     history_type, history_schema);
    const uint64_t checkpoint_version = result.version;

    size_t offset = 0;
    size_t previous = size_t(-1);
    uint64_t sequence = 0;
    size_t relevant_commits = 0;
    while (offset + fixed_header_size + footer_size <= CommitJournal::capacity) {
        char fixed[fixed_header_size];
        read_bytes(offset, fixed, sizeof(fixed));
        if (get_u64(fixed) != header_magic)
            break;
        if (get_u32(fixed + 8) != record_version || get_u32(fixed + 12) != fixed_header_size)
            throw InvalidDatabase("bad commit journal record header", "");
        uint64_t next_sequence = get_u64(fixed + 16);
        uint64_t version = get_u64(fixed + 24);
        uint64_t previous_field = get_u64(fixed + 32);
        uint64_t new_top = get_u64(fixed + 40);
        uint64_t logical_size = get_u64(fixed + 48);
        uint32_t arrays = get_u32(fixed + 56);
        uint32_t record_size = get_u32(fixed + 60);
        size_t minimum = fixed_header_size + size_t(arrays) * entry_size + footer_size;
        if (record_size < minimum || record_size % 8 || offset + record_size > CommitJournal::capacity)
            break;
        std::vector<char> record(record_size);
        read_bytes(offset, record.data(), record.size());
        size_t footer = record.size() - footer_size;
        if (get_u64(record.data() + footer) != footer_magic ||
            get_u64(record.data() + footer + 8) != next_sequence ||
            get_u32(record.data() + footer + 16) != record_size)
            break;
        uint32_t stored_crc = get_u32(record.data() + footer + 20);
        if (stored_crc != Crc32c::compute(record.data(), footer + 20))
            break;
        if (next_sequence != sequence + 1 ||
            previous_field != (previous == size_t(-1) ? UINT64_MAX : previous))
            throw InvalidDatabase("bad commit journal record chain", "");

        bool arrays_valid = true;
        size_t cursor = fixed_header_size;
        for (uint32_t i = 0; i < arrays && version > checkpoint_version; ++i) {
            ref_type ref = ref_type(get_u64(record.data() + cursor));
            uint32_t size = get_u32(record.data() + cursor + 8);
            uint32_t checksum = get_u32(record.data() + cursor + 12);
            if (size < NodeHeader::header_size || uint64_t(ref) + size > uint64_t(m_alloc.get_file_size())) {
                arrays_valid = false;
                break;
            }
            const char* node = m_alloc.translate(ref);
            uint32_t stored;
            std::memcpy(&stored, node, sizeof(stored));
            if (stored != checksum || Crc32c::compute(node + 4, size - 4) != checksum) {
                arrays_valid = false;
                break;
            }
            cursor += entry_size;
            result.cow_bytes += size;
        }
        if (!arrays_valid) {
            char next_magic[sizeof(uint64_t)]{};
            if (offset + record_size + sizeof(next_magic) <= CommitJournal::capacity) {
                read_bytes(offset + record_size, next_magic, sizeof(next_magic));
                if (get_u64(next_magic) == header_magic)
                    throw InvalidDatabase("corruption inside commit journal chain", "");
            }
            break;
        }
        if (version > result.version) {
            result.top_ref = ref_type(new_top);
            result.version = version;
            result.logical_size = size_t(logical_size);
        }
        if (version > checkpoint_version)
            ++relevant_commits;
        previous = offset;
        sequence = next_sequence;
        offset += record_size;
    }
    result.tail = offset;
    result.previous_offset = previous;
    result.sequence = sequence;
    result.commits = relevant_commits;
    return result;
}

} // namespace llbt::core_detail
