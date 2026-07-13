/*
** llbt — Low Level Binary Tree
** Copyright (c) 2026 Mohammad Julfikar
** Dedicated to the public domain. See LICENSE and NOTICE.
*/
#include <llbt/core/commit_journal.hpp>

#include <llbt/alloc_slab.hpp>
#include <llbt/array.hpp>
#include <llbt/array_blob.hpp>
#include <llbt/core/crc32c.hpp>
#include <llbt/core/commit_journal_format.hpp>
#include <llbt/core/commit_recovery.hpp>
#include <llbt/exceptions.hpp>
#include <llbt/group.hpp>
#include <llbt/group_writer.hpp>
#include <llbt/impl/simulated_failure.hpp>
#include <llbt/node_header.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace llbt::core_detail {
using namespace journal_format;

CommitJournal::CommitJournal(SlabAlloc& alloc, std::vector<ref_type> chunks)
    : m_alloc(alloc)
    , m_chunks(std::move(chunks))
{
}

size_t CommitJournal::required_size(size_t array_count) noexcept
{
    return aligned(fixed_header_size + array_count * entry_size + footer_size);
}

bool CommitJournal::ensure(Array& registry_top)
{
    while (registry_top.size() <= registry_journal_slot)
        registry_top.add(0);
    if (registry_top.get_as_ref(registry_journal_slot))
        return false;

    Allocator& alloc = registry_top.get_alloc();
    Array descriptor(alloc);
    descriptor.create(Array::type_HasRefs);
    std::vector<char> zeros(chunk_size, 0);
    for (size_t i = 0; i < chunk_count; ++i) {
        ArrayBlob chunk(alloc);
        chunk.create();
        chunk.add(zeros.data(), zeros.size());
        descriptor.add(chunk.get_ref());
    }
    registry_top.set_as_ref(registry_journal_slot, descriptor.get_ref());
    return true;
}

std::unique_ptr<CommitJournal> CommitJournal::open(SlabAlloc& alloc, ref_type group_top_ref,
                                                   Recovery* recovery)
{
    auto journal = open_at_checkpoint(alloc, group_top_ref);
    if (!journal)
        return nullptr;
    Recovery found = CommitRecovery(alloc, journal->m_chunks).scan(group_top_ref);
    journal->m_tail = found.tail;
    journal->m_previous_offset = found.previous_offset;
    journal->m_sequence = found.sequence;
    journal->m_commits_since_checkpoint = found.commits;
    journal->m_cow_bytes_since_checkpoint = found.cow_bytes;
    if (recovery)
        *recovery = found;
    return journal;
}

std::unique_ptr<CommitJournal> CommitJournal::open_at_checkpoint(SlabAlloc& alloc, ref_type group_top_ref)
{
    if (!group_top_ref)
        return nullptr;
    Array group_top(alloc);
    group_top.init_from_ref(group_top_ref);
    ref_type registry_ref = _impl::GroupFriend::get_history_ref(alloc, group_top_ref);
    if (!registry_ref)
        return nullptr;
    Array registry(alloc);
    registry.init_from_ref(registry_ref);
    if (registry.size() <= registry_journal_slot)
        return nullptr;
    ref_type descriptor_ref = registry.get_as_ref(registry_journal_slot);
    if (!descriptor_ref)
        return nullptr;
    Array descriptor(alloc);
    descriptor.init_from_ref(descriptor_ref);
    if (descriptor.size() != chunk_count)
        throw InvalidDatabase("bad commit journal descriptor", "");
    std::vector<ref_type> chunks;
    chunks.reserve(chunk_count);
    for (size_t i = 0; i < chunk_count; ++i) {
        ref_type ref = descriptor.get_as_ref(i);
        if (!ref)
            throw InvalidDatabase("bad commit journal chunk", "");
        chunks.push_back(ref);
    }
    auto journal = std::unique_ptr<CommitJournal>(new CommitJournal(alloc, std::move(chunks)));
    journal->m_checkpoint_top_ref = group_top_ref;
    journal->m_checkpoint_version = [&] {
        int history_type = 0;
        int history_schema = 0;
        uint64_t version = 0;
        _impl::GroupFriend::get_version_and_history_info(alloc, group_top_ref, version, history_type,
                                                         history_schema);
        return version;
    }();
    journal->m_checkpoint_logical_size = _impl::GroupFriend::get_logical_file_size(alloc, group_top_ref);
    return journal;
}

void CommitJournal::write_bytes(WriteWindowMgr& windows, size_t offset, const void* input, size_t size)
{
    const char* data = static_cast<const char*>(input);
    while (size) {
        size_t chunk_index = offset / chunk_size;
        size_t in_chunk = offset % chunk_size;
        size_t count = std::min(size, chunk_size - in_chunk);
        ref_type target = m_chunks.at(chunk_index) + NodeHeader::header_size + in_chunk;
        windows.write_bytes(target, data, count);
        data += count;
        offset += count;
        size -= count;
    }
}

CommitJournal::PendingAppend CommitJournal::prepare_append(WriteWindowMgr& windows, uint64_t version,
                                                           ref_type new_top_ref, size_t logical_size,
                                                           const std::vector<WrittenArray>& arrays)
{
    size_t record_size = required_size(arrays.size());
    if (record_size > capacity || m_tail + record_size > capacity)
        throw std::length_error("commit journal is full; checkpoint required");
    std::vector<char> record(record_size, 0);
    put_u64(record.data(), header_magic);
    put_u32(record.data() + 8, record_version);
    put_u32(record.data() + 12, uint32_t(fixed_header_size));
    put_u64(record.data() + 16, m_sequence + 1);
    put_u64(record.data() + 24, version);
    put_u64(record.data() + 32, m_previous_offset == size_t(-1) ? UINT64_MAX : m_previous_offset);
    put_u64(record.data() + 40, new_top_ref);
    put_u64(record.data() + 48, logical_size);
    put_u32(record.data() + 56, uint32_t(arrays.size()));
    put_u32(record.data() + 60, uint32_t(record_size));
    size_t cursor = fixed_header_size;
    for (const WrittenArray& array : arrays) {
        put_u64(record.data() + cursor, array.ref);
        put_u32(record.data() + cursor + 8, array.size);
        put_u32(record.data() + cursor + 12, array.checksum);
        cursor += entry_size;
    }
    size_t footer = record_size - footer_size;
    put_u64(record.data() + footer, footer_magic);
    put_u64(record.data() + footer + 8, m_sequence + 1);
    put_u32(record.data() + footer + 16, uint32_t(record_size));
    put_u32(record.data() + footer + 20, Crc32c::compute(record.data(), footer + 20));
    if (_impl::SimulatedFailure::check_trigger(_impl::SimulatedFailure::commit_journal__partial_record)) {
        write_bytes(windows, m_tail, record.data(), record.size() / 2);
        throw _impl::SimulatedFailure(_impl::SimulatedFailure::commit_journal__partial_record);
    }
    write_bytes(windows, m_tail, record.data(), record.size());
    size_t cow_bytes = 0;
    for (const WrittenArray& array : arrays)
        cow_bytes += array.size;
    return {m_tail + record_size, m_tail, m_sequence + 1, version, cow_bytes};
}

void CommitJournal::commit_append(const PendingAppend& pending) noexcept
{
    m_tail = pending.next_offset;
    m_previous_offset = pending.record_offset;
    m_sequence = pending.sequence;
    ++m_commits_since_checkpoint;
    m_cow_bytes_since_checkpoint += pending.cow_bytes;
}

bool CommitJournal::needs_checkpoint(size_t next_record_size, size_t next_cow_bytes) const noexcept
{
    constexpr size_t max_retained_cow_bytes = 64u * 1024u * 1024u;
    return m_commits_since_checkpoint >= 4096 || m_tail + next_record_size > capacity * 3 / 4 ||
           m_cow_bytes_since_checkpoint + next_cow_bytes > max_retained_cow_bytes;
}

void CommitJournal::reset_after_checkpoint(WriteWindowMgr& windows, ref_type top_ref, uint64_t version,
                                           size_t logical_size) noexcept
{
    try {
        clear_after_compaction(windows, top_ref, version, logical_size);
    }
    catch (...) {
        return;
    }
}

void CommitJournal::clear_after_compaction(WriteWindowMgr& windows, ref_type top_ref, uint64_t version,
                                           size_t logical_size)
{
    const uint64_t zero = 0;
    write_bytes(windows, 0, &zero, sizeof(zero));
    m_tail = 0;
    m_previous_offset = size_t(-1);
    m_sequence = 0;
    m_commits_since_checkpoint = 0;
    m_cow_bytes_since_checkpoint = 0;
    m_checkpoint_top_ref = top_ref;
    m_checkpoint_version = version;
    m_checkpoint_logical_size = logical_size;
}

} // namespace llbt::core_detail
