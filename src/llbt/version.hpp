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

#ifndef LLBT_VERSION_HPP
#define LLBT_VERSION_HPP

#include <string>

#ifndef LLBT_VERSION_MAJOR
#include <llbt/version_numbers.hpp>
#endif

#define LLBT_PRODUCT_NAME "barq-core"
#define LLBT_VER_CHUNK "[" LLBT_PRODUCT_NAME "-" LLBT_VERSION_STRING "]"

namespace llbt {

enum Feature {
    feature_Debug,
    feature_Replication,
};

class StringData;

class Version {
public:
    static int get_major()
    {
        return LLBT_VERSION_MAJOR;
    }
    static int get_minor()
    {
        return LLBT_VERSION_MINOR;
    }
    static int get_patch()
    {
        return LLBT_VERSION_PATCH;
    }
    static StringData get_extra();
    static std::string get_version();
    static bool is_at_least(int major, int minor, int patch, StringData extra);
    static bool is_at_least(int major, int minor, int patch);
    static bool has_feature(Feature feature);
};


} // namespace llbt

#endif // LLBT_VERSION_HPP
