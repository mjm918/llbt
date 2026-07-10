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

#include <llbt/util/serializer.hpp>
#include <llbt/unicode.hpp>
#include <llbt/object_id.hpp>
#include <llbt/uuid.hpp>
#include <llbt/query_value.hpp>

#include <llbt/binary_data.hpp>
#include <llbt/keys.hpp>
#include <llbt/null.hpp>
#include <llbt/string_data.hpp>
#include <llbt/timestamp.hpp>
#include <llbt/util/base64.hpp>

#include <cctype>
#include <cmath>
#include <iomanip>

namespace llbt {

/* Uses Fliegel & Van Flandern algorithm */
static constexpr long date_to_julian(int y, int m, int d)
{
    return (1461 * (y + 4800 + (m - 14) / 12)) / 4 + (367 * (m - 2 - 12 * ((m - 14) / 12))) / 12 -
           (3 * ((y + 4900 + (m - 14) / 12) / 100)) / 4 + d - 32075;
}

static void julian_to_date(int jd, int* y, int* m, int* d)
{
    uint64_t L = jd + 68569;
    uint64_t n = (4 * L) / 146097;
    uint64_t i, j;

    L = L - (146097 * n + 3) / 4;
    i = (4000 * (L + 1)) / 1461001;
    L = L - (1461 * i) / 4 + 31;
    j = (80 * L) / 2447;
    *d = static_cast<int>(L - (2447 * j) / 80);
    L = j / 11;
    *m = static_cast<int>(j + 2 - (12 * L));
    *y = static_cast<int>(100 * (n - 49) + i + L);
}

// Confirmed to work for all val < 16389
static void out_dec(char** buffer, unsigned val, int width)
{
    int w = width;
    char* p = *buffer;
    while (width > 0) {
        width--;
        unsigned div10 = (val * 6554) >> 16;
        p[width] = char(val - div10 * 10) + '0';
        val = div10;
    }
    *buffer += w;
}

static constexpr long epoc_julian_days = date_to_julian(1970, 1, 1); // 2440588
static constexpr int seconds_in_a_day = 24 * 60 * 60;

const char* Timestamp::to_string(std::array<char, 32>& buffer) const
{
    if (is_null()) {
        return "null";
    }
    int64_t seconds = get_seconds();
    int32_t nano = get_nanoseconds();
    if (nano < 0) {
        nano += Timestamp::nanoseconds_per_second;
        seconds--;
    }

    int64_t days = seconds / seconds_in_a_day;
    int julian_days = int(days + epoc_julian_days);

    int seconds_in_day = int(seconds - days * seconds_in_a_day);
    if (seconds_in_day < 0) {
        seconds_in_day += seconds_in_a_day;
        julian_days--;
    }

    int year;
    int month;
    int day;
    int hours = seconds_in_day / 3600;
    int remainingSeconds = seconds_in_day - hours * 3600;
    int minutes = remainingSeconds / 60;
    int secs = remainingSeconds - minutes * 60;

    julian_to_date(julian_days, &year, &month, &day);

    char* p = buffer.data();
    if (year < 0) {
        *p++ = '-';
        year = -year;
    }
    out_dec(&p, year, 4);
    *p++ = '-';
    out_dec(&p, month, 2);
    *p++ = '-';
    out_dec(&p, day, 2);
    *p++ = ' ';
    out_dec(&p, hours, 2);
    *p++ = ':';
    out_dec(&p, minutes, 2);
    *p++ = ':';
    out_dec(&p, secs, 2);
    *p = '\0';
    if (nano) {
        snprintf(p, 32 - (p - buffer.data()), ".%09d", nano);
    }
    return buffer.data();
}

namespace util {
namespace serializer {

template <>
std::string print_value<>(BinaryData data)
{
    if (data.is_null()) {
        return "NULL";
    }
    return util::format("binary(%1)", print_value<StringData>(StringData(data.data(), data.size())));
}

template <>
std::string print_value<>(bool b)
{
    if (b) {
        return "true";
    }
    return "false";
}

template <typename T>
inline std::string print_with_nan_check(T val)
{
    // we standardize NaN because some implementations (windows) will
    // print the different types of NaN such as "nan(ind)" to indicate "indefinite"
    if (std::isnan(val)) {
        // preserving the sign of nan is not strictly required but is good etiquette
        if (std::signbit(val)) {
            return "-nan";
        }
        return "nan";
    }
    std::stringstream ss;
    ss << std::setprecision(std::numeric_limits<T>::max_digits10) << val;
    return ss.str();
}

template <>
std::string print_value<>(float val)
{
    return print_with_nan_check(val);
}

template <>
std::string print_value<>(double val)
{
    return print_with_nan_check(val);
}

template <>
std::string print_value<>(llbt::null)
{
    return "NULL";
}

static bool contains_invalids(StringData data)
{
    // the custom whitelist is different from std::isprint because it doesn't include quotations
    const static std::string whitelist = " {|}~:;<=>?@!#$%&()*+,-./[]^_`";
    const char* ptr = data.data();
    const char* end = ptr + data.size();
    while (ptr < end) {
        using unsigned_char_t = unsigned char;
        char c = *ptr;
        size_t len = sequence_length(c);
        if (len == 1) {
            // std::isalnum takes an int, but is undefined for negative values so we must pass an unsigned char
            if (!std::isalnum(unsigned_char_t(c)) && whitelist.find(c) == std::string::npos) {
                return true;
            }
        }
        else {
            // multibyte utf8, check if subsequent bytes have the upper bit set
            for (size_t i = 1; i < len; i++) {
                ++ptr;
                if (ptr == end || (*ptr & 0x80) == 0) {
                    return true;
                }
            }
        }
        ++ptr;
    }
    return false;
}

template <>
std::string print_value<>(StringData data)
{
    if (data.is_null()) {
        return "NULL";
    }
    std::string out;
    const char* start = data.data();
    const size_t len = data.size();

    if (contains_invalids(data)) {
        std::string encode_buffer;
        encode_buffer.resize(util::base64_encoded_size(len));
        util::base64_encode(data, encode_buffer);
        out = "B64\"" + encode_buffer + "\"";
    }
    else {
        out.reserve(len + 2);
        out += '"';
        for (const char* i = start; i != start + len; ++i) {
            out += *i;
        }
        out += '"';
    }
    return out;
}

template <>
std::string print_value<>(llbt::Timestamp t)
{
    if (t.is_null()) {
        return "NULL";
    }
    std::stringstream ss;
    if (t.is_null()) {
        ss << "null";
    }
    else {
        ss << "T" << t.get_seconds() << ":" << t.get_nanoseconds();
    }
    return ss.str();
}

template <>
std::string print_value<>(llbt::ObjectId oid)
{
    return "oid(" + oid.to_string() + ")";
}

template <>
std::string print_value<>(llbt::ObjKey k)
{
    std::stringstream ss;
    if (!k) {
        ss << "NULL";
    }
    else {
        ss << "O" << k.value;
    }
    return ss.str();
}

std::string print_value(llbt::ObjLink link, Group*)
{
    // llbt scope cut: no table layer, so links print in raw key form.
    if (!link) {
        return "NULL";
    }
    std::stringstream ss;
    ss << "L" << link.get_table_key().value << ":" << link.get_obj_key().value;
    return ss.str();
}

template <>
std::string print_value<>(llbt::UUID uuid)
{
    return "uuid(" + uuid.to_string() + ")";
}

template <>
std::string print_value<>(llbt::TypeOfValue type)
{
    return '"' + type.to_string() + '"';
}

#if LLBT_ENABLE_GEOSPATIAL
template <>
std::string print_value<>(const llbt::Geospatial& geo)
{
    return geo.to_string();
}
#endif

// llbt scope cut: SerialisationState (query/table serialization helpers)
// removed with the table and query layers.

} // namespace serializer
} // namespace util
} // namespace llbt
