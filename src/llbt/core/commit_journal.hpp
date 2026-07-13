/*
** llbt — Low Level Binary Tree
** Copyright (c) 2026 Mohammad Julfikar
** Dedicated to the public domain. See LICENSE and NOTICE.
*/
#ifndef LLBT_CORE_COMMIT_JOURNAL_HPP
#define LLBT_CORE_COMMIT_JOURNAL_HPP

#include <llbt/alloc.hpp>
#include <llbt/core/commit_recovery.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace llbt {
class Allocator;
class Array;
class SlabAlloc;
class WriteWindowMgr;

namespace core_detail {

struct WrittenArray {
    ref_type ref = 0;
    uint32_t size = 0;
    uint32_t checksum = 0;
};

class CommitJournal {
public:
    using Recovery = CommitRecovery::Result;

    struct PendingAppend {
        size_t next_offset = 0;
        size_t record_offset = 0;
        uint64_t sequence = 0;
        uint64_t version = 0;
        size_t cow_bytes = 0;
    };

    static constexpr size_t chunk_size = 4u * 1024u * 1024u;
    static constexpr size_t chunk_count = 16;
    static constexpr size_t capacity = chunk_size * chunk_count;

    static bool ensure(Array& registry_top);
    static size_t required_size(size_t array_count) noexcept;
    static std::unique_ptr<CommitJournal> open(SlabAlloc& alloc, ref_type group_top_ref,
                                               Recovery* recovery = nullptr);
    static std::unique_ptr<CommitJournal> open_at_checkpoint(SlabAlloc& alloc, ref_type group_top_ref);

    PendingAppend prepare_append(WriteWindowMgr& windows, uint64_t version, ref_type new_top_ref,
                                 size_t logical_size, const std::vector<WrittenArray>& arrays);
    void commit_append(const PendingAppend&) noexcept;
    bool needs_checkpoint(size_t next_record_size = 0, size_t next_cow_bytes = 0) const noexcept;
    void reset_after_checkpoint(WriteWindowMgr& windows, ref_type top_ref, uint64_t version,
                                size_t logical_size) noexcept;
    void clear_after_compaction(WriteWindowMgr& windows, ref_type top_ref, uint64_t version,
                                size_t logical_size);

    ref_type checkpoint_top_ref() const noexcept { return m_checkpoint_top_ref; }
    uint64_t checkpoint_version() const noexcept { return m_checkpoint_version; }
    size_t checkpoint_logical_size() const noexcept { return m_checkpoint_logical_size; }

private:
    CommitJournal(SlabAlloc& alloc, std::vector<ref_type> chunks);

    void write_bytes(WriteWindowMgr& windows, size_t offset, const void* data, size_t size);

    SlabAlloc& m_alloc;
    std::vector<ref_type> m_chunks;
    size_t m_tail = 0;
    size_t m_commits_since_checkpoint = 0;
    size_t m_cow_bytes_since_checkpoint = 0;
    uint64_t m_sequence = 0;
    size_t m_previous_offset = size_t(-1);
    ref_type m_checkpoint_top_ref = 0;
    uint64_t m_checkpoint_version = 0;
    size_t m_checkpoint_logical_size = 0;
};

} // namespace core_detail
} // namespace llbt

#endif
