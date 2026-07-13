/*
** llbt — Low Level Binary Tree
** Copyright (c) 2026 Mohammad Julfikar
** Dedicated to the public domain. See LICENSE and NOTICE.
*/
#include <llbt/core/crc32c.hpp>

#include <array>
#include <cstring>

#if defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#endif
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
#include <nmmintrin.h>
#endif

namespace llbt::core_detail {
namespace {

constexpr std::array<uint32_t, 256> make_table()
{
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < table.size(); ++i) {
        uint32_t crc = i;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0x82f63b78u & (0u - (crc & 1u)));
        table[i] = crc;
    }
    return table;
}

constexpr auto g_table = make_table();

#if defined(__APPLE__) && defined(__aarch64__) && !defined(__ARM_FEATURE_CRC32)
// Apple Silicon implements the ARMv8 CRC instructions, but Apple Clang does
// not define __ARM_FEATURE_CRC32 for the default arm64 target. Keep the
// instructions in this small function so the rest of the library still uses
// the portable target settings.
uint32_t compute_apple_arm64(const uint8_t* data, size_t size) noexcept
{
    uint32_t crc = 0xffffffffu;
    while (size >= sizeof(uint64_t)) {
        uint64_t word;
        std::memcpy(&word, data, sizeof(word));
        __asm__("crc32cx %w[crc], %w[crc], %x[word]" : [crc] "+r"(crc) : [word] "r"(word));
        data += sizeof(word);
        size -= sizeof(word);
    }
    while (size--) {
        uint32_t byte = *data++;
        __asm__("crc32cb %w[crc], %w[crc], %w[byte]" : [crc] "+r"(crc) : [byte] "r"(byte));
    }
    return ~crc;
}
#endif

#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
__attribute__((target("sse4.2"))) uint32_t compute_sse42(const uint8_t* data, size_t size) noexcept
{
    uint64_t crc = 0xffffffffu;
#if defined(__x86_64__)
    while (size >= sizeof(uint64_t)) {
        uint64_t word;
        std::memcpy(&word, data, sizeof(word));
        crc = _mm_crc32_u64(crc, word);
        data += sizeof(word);
        size -= sizeof(word);
    }
#endif
    uint32_t tail = uint32_t(crc);
    while (size--)
        tail = _mm_crc32_u8(tail, *data++);
    return ~tail;
}
#endif

} // namespace

uint32_t Crc32c::compute(const void* bytes, size_t size) noexcept
{
    auto* data = static_cast<const uint8_t*>(bytes);
    uint32_t crc = 0xffffffffu;
#if defined(__ARM_FEATURE_CRC32) && defined(__aarch64__)
    while (size >= sizeof(uint64_t)) {
        uint64_t word;
        std::memcpy(&word, data, sizeof(word));
        crc = __crc32cd(crc, word);
        data += sizeof(word);
        size -= sizeof(word);
    }
    while (size--)
        crc = __crc32cb(crc, *data++);
#elif defined(__APPLE__) && defined(__aarch64__)
    return compute_apple_arm64(data, size);
#elif (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
    if (__builtin_cpu_supports("sse4.2"))
        return compute_sse42(data, size);
    while (size--)
        crc = g_table[(crc ^ *data++) & 0xff] ^ (crc >> 8);
#else
    while (size--)
        crc = g_table[(crc ^ *data++) & 0xff] ^ (crc >> 8);
#endif
    return ~crc;
}

} // namespace llbt::core_detail
