/*
** llbt — Low Level Binary Tree
** Copyright (c) 2026 Mohammad Julfikar
**
** Dedicated to the public domain under the same terms as the llbt-authored
** core API files. See LICENSE and NOTICE.
*/
#ifndef LLBT_COMMIT_METRICS_HPP
#define LLBT_COMMIT_METRICS_HPP

#include <cstdint>

namespace llbt::core {

class CommitMetrics {
public:
    void reset() noexcept;

    uint64_t arrays_written = 0;
    uint64_t cow_bytes_written = 0;
    uint64_t binary_payload_bytes = 0;
    uint64_t mapping_windows = 0;
    uint64_t mapping_windows_touched = 0;
    uint64_t file_size_before = 0;
    uint64_t file_size_after = 0;
    int64_t physical_file_growth = 0;
    uint64_t tree_write_us = 0;
    uint64_t array_alloc_us = 0;
    uint64_t array_io_us = 0;
    uint64_t free_list_us = 0;
    uint64_t write_group_us = 0;
    uint64_t flush_us = 0;
    uint64_t first_sync_us = 0;
    uint64_t header_update_us = 0;
    uint64_t second_sync_us = 0;
    uint64_t data_sync_us = 0;
    uint64_t header_publish_us = 0;
    uint64_t total_us = 0;
};

} // namespace llbt::core

#endif
