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

#ifndef LLBT_TEST_UTIL_TEST_ONLY_HPP
#define LLBT_TEST_UTIL_TEST_ONLY_HPP

#include "unit_test.hpp"

#define ONLY(name)                                                                                                   \
    llbt::test_util::SetTestOnly llbt_set_test_only__##name(#name);                                                \
    TEST(name)

#define NONCONCURRENT_ONLY(name)                                                                                     \
    llbt::test_util::SetTestOnly llbt_set_test_only__##name(#name);                                                \
    NONCONCURRENT_TEST(name)

#define ONLY_TYPES(name, ...)                                                                                        \
    llbt::test_util::SetTestOnly llbt_set_test_only__##name(#name "*");                                            \
    TEST_TYPES(name, __VA_ARGS__)

namespace llbt {
namespace test_util {

struct SetTestOnly {
    SetTestOnly(const char* test_name);
};

const char* get_test_only();

} // namespace test_util
} // namespace llbt

#endif // LLBT_TEST_UTIL_TEST_ONLY_HPP
