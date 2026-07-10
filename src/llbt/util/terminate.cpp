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

#include <llbt/util/terminate.hpp>

#include <iostream>
#include <sstream>
#include <llbt/util/features.h>
#include <llbt/util/thread.hpp>
#include <llbt/util/backtrace.hpp>
#include <llbt/version.hpp>

#if LLBT_PLATFORM_APPLE
#include <os/log.h>

#include <dlfcn.h>
#include <execinfo.h>
#include <CoreFoundation/CoreFoundation.h>
#endif

#if LLBT_ANDROID
#include <android/log.h>
#endif

// extern "C" and noinline so that a readable message shows up in the stack trace
// of the crash
// prototype here to silence warning
// The macro indirection here puts the core version number in the actual stack trace

// LCOV_EXCL_START
#define LLBT_DEFINE_TERMINATE_VERSIONED_(x)                                                                         \
    extern "C" LLBT_NORETURN LLBT_NOINLINE void please_report_this_issue_in_github_barq_barq_core_v##x();        \
                                                                                                                     \
    extern "C" LLBT_NORETURN LLBT_NOINLINE void please_report_this_issue_in_github_barq_barq_core_v##x()         \
    {                                                                                                                \
        std::abort();                                                                                                \
    }

#define LLBT_DEFINE_TERMINATE_VERSIONED(x) LLBT_DEFINE_TERMINATE_VERSIONED_(x)
#define LLBT_EVALUATE_(x) _##x
#define LLBT_EVALUATE(x) LLBT_EVALUATE_(x)
#define LLBT_MACRO_CONCAT(A, B) LLBT_MACRO_CONCAT_(A, B)
#define LLBT_MACRO_CONCAT_(A, B) A##B
#define LLBT_VERSION_SUFFIX_CONCAT                                                                                  \
    LLBT_MACRO_CONCAT(LLBT_EVALUATE(LLBT_VERSION_MAJOR),                                                          \
                       LLBT_MACRO_CONCAT(LLBT_EVALUATE(LLBT_VERSION_MINOR), LLBT_EVALUATE(LLBT_VERSION_PATCH)))

LLBT_DEFINE_TERMINATE_VERSIONED(LLBT_VERSION_SUFFIX_CONCAT)

#define LLBT_TERMINATE_VERSIONED_(x) please_report_this_issue_in_github_barq_barq_core_v##x()
#define LLBT_TERMINATE_VERSIONED(x) LLBT_TERMINATE_VERSIONED_(x)
#define LLBT_TERMINATE_AUTO_VERSIONED() LLBT_TERMINATE_VERSIONED(LLBT_VERSION_SUFFIX_CONCAT)

// LCOV_EXCL_STOP

namespace {

#if LLBT_PLATFORM_APPLE
void nslog(const char* message) noexcept
{
    // Standard error goes nowhere for applications managed by launchd,
    // so log to ASL/unified logging system logs as well.
    fputs(message, stderr);
    // The unified logging system considers dynamic strings to be private in
    // order to protect users. This means we must specify "%{public}s" to get
    // the message here. See `man os_log` for more details.
    os_log_error(OS_LOG_DEFAULT, "%{public}s", message);
    // Log the message to Crashlytics if it's loaded into the process
    void* addr = dlsym(RTLD_DEFAULT, "CLSLog");
    if (addr) {
        CFStringRef str =
            CFStringCreateWithCStringNoCopy(kCFAllocatorDefault, message, kCFStringEncodingUTF8, kCFAllocatorNull);
        auto fn = reinterpret_cast<void (*)(CFStringRef, ...)>(reinterpret_cast<size_t>(addr));
        fn(CFSTR("%@"), str);
        CFRelease(str);
    }
}

void (*termination_notification_callback)(const char*) noexcept = nslog;

#elif LLBT_ANDROID

void android_log(const char* message) noexcept
{
    __android_log_write(ANDROID_LOG_ERROR, "BARQ", message);
}

void (*termination_notification_callback)(const char*) noexcept = android_log;

#else

void (*termination_notification_callback)(const char*) noexcept = nullptr;

#endif

} // unnamed namespace

namespace llbt {
namespace util {

// LCOV_EXCL_START
LLBT_NORETURN static void terminate_internal(std::stringstream& ss) noexcept
{
    util::Backtrace::capture().print(ss);

    ss << "\n!!! IMPORTANT: Please report this at https://github.com/barq/barq-core/issues/new/choose";

    if (termination_notification_callback) {
        termination_notification_callback(ss.str().c_str());
    }
    else {
        std::cerr << ss.rdbuf();
        std::string thread_name;
        if (Thread::get_name(thread_name))
            std::cerr << "\nThread name: " << thread_name;
    }

    LLBT_TERMINATE_AUTO_VERSIONED();
}

LLBT_NORETURN void terminate(const char* message, const char* file, long line,
                              std::initializer_list<Printable>&& values) noexcept
{
    terminate_with_info(message, file, line, nullptr, std::move(values));
}

LLBT_NORETURN void terminate_with_info(const char* message, const char* file, long line,
                                        const char* interesting_names,
                                        std::initializer_list<Printable>&& values) noexcept
{
    std::stringstream ss;
    ss << file << ':' << line << ": " LLBT_VER_CHUNK " " << message;
    if (interesting_names)
        ss << " with " << interesting_names << " = ";
    Printable::print_all(ss, values, bool(interesting_names));
    ss << '\n';
    terminate_internal(ss);
}
// LCOV_EXCL_STOP

} // namespace util
} // namespace llbt
