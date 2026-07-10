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

#ifndef LLBT_UTIL_ASSERT_HPP
#define LLBT_UTIL_ASSERT_HPP

#include <llbt/util/features.h>
#include <llbt/util/terminate.hpp>

#if LLBT_ENABLE_ASSERTIONS || defined(LLBT_DEBUG)
#define LLBT_ASSERTIONS_ENABLED 1
#else
#define LLBT_ASSERTIONS_ENABLED 0
#endif

#define LLBT_ASSERT_RELEASE(condition)                                                                              \
    (LLBT_LIKELY(condition) ? static_cast<void>(0)                                                                  \
                             : llbt::util::terminate("Assertion failed: " #condition, __FILE__, __LINE__))

#if LLBT_ASSERTIONS_ENABLED
#define LLBT_ASSERT(condition) LLBT_ASSERT_RELEASE(condition)
#else
#define LLBT_ASSERT(condition) static_cast<void>(sizeof bool(condition))
#endif

#ifdef LLBT_DEBUG
#define LLBT_ASSERT_DEBUG(condition) LLBT_ASSERT_RELEASE(condition)
#else
#define LLBT_ASSERT_DEBUG(condition) static_cast<void>(sizeof bool(condition))
#endif

#define LLBT_STRINGIFY(X) #X

#define LLBT_ASSERT_RELEASE_EX(condition, ...)                                                                      \
    (LLBT_LIKELY(condition) ? static_cast<void>(0)                                                                  \
                             : llbt::util::terminate_with_info("Assertion failed: " #condition, __LINE__, __FILE__, \
                                                                LLBT_STRINGIFY((__VA_ARGS__)), __VA_ARGS__))

#ifdef LLBT_DEBUG
#define LLBT_ASSERT_DEBUG_EX LLBT_ASSERT_RELEASE_EX
#else
#define LLBT_ASSERT_DEBUG_EX(condition, ...) static_cast<void>(sizeof bool(condition))
#endif

// Becase the assert is used in noexcept methods, it's a bad idea to allocate
// buffer space for the message so therefore we must pass it to terminate which
// will 'cerr' it for us without needing any buffer
#if LLBT_ENABLE_ASSERTIONS || defined(LLBT_DEBUG)

#define LLBT_ASSERT_EX LLBT_ASSERT_RELEASE_EX

#define LLBT_ASSERT_3(left, cmp, right)                                                                             \
    (LLBT_LIKELY((left)cmp(right)) ? static_cast<void>(0)                                                           \
                                    : llbt::util::terminate("Assertion failed: "                                    \
                                                             "" #left " " #cmp " " #right,                           \
                                                             __FILE__, __LINE__, left, right))

#define LLBT_ASSERT_7(left1, cmp1, right1, logical, left2, cmp2, right2)                                            \
    (LLBT_LIKELY(((left1)cmp1(right1))logical((left2)cmp2(right2)))                                                 \
         ? static_cast<void>(0)                                                                                      \
         : llbt::util::terminate("Assertion failed: "                                                               \
                                  "" #left1 " " #cmp1 " " #right1 " " #logical " "                                   \
                                  "" #left2 " " #cmp2 " " #right2,                                                   \
                                  __FILE__, __LINE__, left1, right1, left2, right2))

#define LLBT_ASSERT_11(left1, cmp1, right1, logical1, left2, cmp2, right2, logical2, left3, cmp3, right3)           \
    (LLBT_LIKELY(((left1)cmp1(right1))logical1((left2)cmp2(right2)) logical2((left3)cmp3(right3)))                  \
         ? static_cast<void>(0)                                                                                      \
         : llbt::util::terminate("Assertion failed: "                                                               \
                                  "" #left1 " " #cmp1 " " #right1 " " #logical1 " "                                  \
                                  "" #left2 " " #cmp2 " " #right2 " " #logical2 " "                                  \
                                  "" #left3 " " #cmp3 " " #right3,                                                   \
                                  __FILE__, __LINE__, left1, right1, left2, right2, left3, right3))
#else
#define LLBT_ASSERT_EX(condition, ...) static_cast<void>(sizeof bool(condition))
#define LLBT_ASSERT_3(left, cmp, right) static_cast<void>(sizeof bool((left)cmp(right)))
#define LLBT_ASSERT_7(left1, cmp1, right1, logical, left2, cmp2, right2)                                            \
    static_cast<void>(sizeof bool(((left1)cmp1(right1))logical((left2)cmp2(right2))))
#define LLBT_ASSERT_11(left1, cmp1, right1, logical1, left2, cmp2, right2, logical2, left3, cmp3, right3)           \
    static_cast<void>(sizeof bool(((left1)cmp1(right1))logical1((left2)cmp2(right2)) logical2((left3)cmp3(right3))))
#endif

#define LLBT_UNREACHABLE() llbt::util::terminate("Unreachable code", __FILE__, __LINE__)
#ifdef LLBT_COVER
#define LLBT_COVER_NEVER(x) false
#define LLBT_COVER_ALWAYS(x) true
#else
#define LLBT_COVER_NEVER(x) (x)
#define LLBT_COVER_ALWAYS(x) (x)
#endif

#endif // LLBT_UTIL_ASSERT_HPP
