/*
** llbt — Low Level Binary Tree
** Copyright (c) 2026 Mohammad Julfikar
** Dedicated to the public domain. See LICENSE and NOTICE.
*/
#ifndef LLBT_CORE_CRC32C_HPP
#define LLBT_CORE_CRC32C_HPP

#include <cstddef>
#include <cstdint>

namespace llbt::core_detail {

class Crc32c {
public:
    static uint32_t compute(const void* data, size_t size) noexcept;
};

} // namespace llbt::core_detail

#endif
