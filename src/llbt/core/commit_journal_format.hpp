/*
** llbt — Low Level Binary Tree
** Copyright (c) 2026 Mohammad Julfikar
** Dedicated to the public domain. See LICENSE and NOTICE.
*/
#ifndef LLBT_CORE_COMMIT_JOURNAL_FORMAT_HPP
#define LLBT_CORE_COMMIT_JOURNAL_FORMAT_HPP

#include <cstddef>
#include <cstdint>

namespace llbt::core_detail::journal_format {

constexpr uint64_t header_magic = 0x35324a4354424c4cULL;
constexpr uint64_t footer_magic = 0x524554464a43544cULL;
constexpr uint32_t record_version = 1;
constexpr size_t fixed_header_size = 64;
constexpr size_t entry_size = 16;
constexpr size_t footer_size = 24;
constexpr size_t registry_journal_slot = 3;

inline size_t aligned(size_t value) noexcept
{
    return (value + 7) & ~size_t(7);
}

inline void put_u32(char* out, uint32_t value) noexcept
{
    for (size_t i = 0; i < 4; ++i)
        out[i] = char(value >> (i * 8));
}

inline void put_u64(char* out, uint64_t value) noexcept
{
    for (size_t i = 0; i < 8; ++i)
        out[i] = char(value >> (i * 8));
}

inline uint32_t get_u32(const char* in) noexcept
{
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i)
        value |= uint32_t(uint8_t(in[i])) << (i * 8);
    return value;
}

inline uint64_t get_u64(const char* in) noexcept
{
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i)
        value |= uint64_t(uint8_t(in[i])) << (i * 8);
    return value;
}

} // namespace llbt::core_detail::journal_format

#endif
