/*
** llbt — Low Level Binary Tree
** Copyright (c) 2026 Mohammad Julfikar
** Dedicated to the public domain. See LICENSE and NOTICE.
*/
#ifndef LLBT_CORE_COMMIT_RECOVERY_HPP
#define LLBT_CORE_COMMIT_RECOVERY_HPP

#include <llbt/alloc.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace llbt {
class SlabAlloc;

namespace core_detail {

class CommitRecovery {
public:
    struct Result {
        ref_type top_ref = 0;
        uint64_t version = 0;
        size_t logical_size = 0;
        size_t tail = 0;
        size_t previous_offset = size_t(-1);
        uint64_t sequence = 0;
        size_t commits = 0;
        size_t cow_bytes = 0;
    };

    CommitRecovery(SlabAlloc& alloc, const std::vector<ref_type>& chunks) noexcept;
    Result scan(ref_type checkpoint_top) const;

private:
    void read_bytes(size_t offset, void* output, size_t size) const;

    SlabAlloc& m_alloc;
    const std::vector<ref_type>& m_chunks;
};

} // namespace core_detail
} // namespace llbt

#endif
