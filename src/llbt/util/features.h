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

#ifndef LLBT_UTIL_FEATURES_H
#define LLBT_UTIL_FEATURES_H

#ifdef _MSC_VER
#pragma warning(disable : 4800) // Visual Studio int->bool performance warnings
#endif

#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#ifndef LLBT_NO_CONFIG
#include <llbt/util/config.h>
#endif

/* The maximum number of elements in a B+-tree node. Applies to inner nodes and
 * to leaves. The minimum allowable value is 2.
 */
#ifndef LLBT_MAX_BPNODE_SIZE
#define LLBT_MAX_BPNODE_SIZE 1000
#endif


#define LLBT_QUOTE_2(x) #x
#define LLBT_QUOTE(x) LLBT_QUOTE_2(x)

/* See these links for information about feature check macroes in GCC,
 * Clang, and MSVC:
 *
 * http://gcc.gnu.org/projects/cxx0x.html
 * http://clang.llvm.org/cxx_status.html
 * http://clang.llvm.org/docs/LanguageExtensions.html#checks-for-standard-language-features
 * http://msdn.microsoft.com/en-us/library/vstudio/hh567368.aspx
 * http://sourceforge.net/p/predef/wiki/Compilers
 */


/* Compiler is GCC and version is greater than or equal to the specified version */
#define LLBT_HAVE_AT_LEAST_GCC(maj, min) \
    (__GNUC__ > (maj) || __GNUC__ == (maj) && __GNUC_MINOR__ >= (min))

#if defined(__clang__)
#define LLBT_HAVE_CLANG_FEATURE(feature) __has_feature(feature)
#define LLBT_HAVE_CLANG_WARNING(warning) __has_warning(warning)
#else
#define LLBT_HAVE_CLANG_FEATURE(feature) 0
#define LLBT_HAVE_CLANG_WARNING(warning) 0
#endif

#ifdef __has_cpp_attribute
#define LLBT_HAS_CPP_ATTRIBUTE(attr) __has_cpp_attribute(attr)
#else
#define LLBT_HAS_CPP_ATTRIBUTE(attr) 0
#endif

#if LLBT_HAS_CPP_ATTRIBUTE(clang::fallthrough)
#define LLBT_FALLTHROUGH [[clang::fallthrough]]
#elif LLBT_HAS_CPP_ATTRIBUTE(gnu::fallthrough)
#define LLBT_FALLTHROUGH [[gnu::fallthrough]]
#elif LLBT_HAS_CPP_ATTRIBUTE(fallthrough)
#define LLBT_FALLTHROUGH [[fallthrough]]
#else
#define LLBT_FALLTHROUGH
#endif

// This should be renamed to LLBT_UNREACHABLE as soon as LLBT_UNREACHABLE is renamed to
// LLBT_ASSERT_NOT_REACHED which will better reflect its nature
#if defined(__GNUC__) || defined(__clang__)
#define LLBT_COMPILER_HINT_UNREACHABLE __builtin_unreachable
#else
#define LLBT_COMPILER_HINT_UNREACHABLE abort
#endif

#if defined(__GNUC__) // clang or GCC
#define LLBT_PRAGMA(v) _Pragma(LLBT_QUOTE_2(v))
#elif defined(_MSC_VER) // VS
#define LLBT_PRAGMA(v) __pragma(v)
#else
#define LLBT_PRAGMA(v)
#endif

#if defined(__clang__)
#define LLBT_DIAG(v) LLBT_PRAGMA(clang diagnostic v)
#elif defined(__GNUC__)
#define LLBT_DIAG(v) LLBT_PRAGMA(GCC diagnostic v)
#else
#define LLBT_DIAG(v)
#endif

#define LLBT_DIAG_PUSH() LLBT_DIAG(push)
#define LLBT_DIAG_POP() LLBT_DIAG(pop)

#ifdef _MSC_VER
#define LLBT_VS_WARNING_DISABLE #pragma warning (default: 4297)
#endif

#if LLBT_HAVE_CLANG_WARNING("-Wtautological-compare") || LLBT_HAVE_AT_LEAST_GCC(6, 0)
#define LLBT_DIAG_IGNORE_TAUTOLOGICAL_COMPARE() LLBT_DIAG(ignored "-Wtautological-compare")
#else
#define LLBT_DIAG_IGNORE_TAUTOLOGICAL_COMPARE()
#endif

#ifdef _MSC_VER
#  define LLBT_DIAG_IGNORE_UNSIGNED_MINUS() LLBT_PRAGMA(warning(disable:4146))
#else
#define LLBT_DIAG_IGNORE_UNSIGNED_MINUS()
#endif

/* The way to specify that a function never returns. */
#if LLBT_HAVE_AT_LEAST_GCC(4, 8) || LLBT_HAVE_CLANG_FEATURE(cxx_attributes)
#define LLBT_NORETURN [[noreturn]]
#elif __GNUC__
#define LLBT_NORETURN __attribute__((noreturn))
#elif defined(_MSC_VER)
#define LLBT_NORETURN __declspec(noreturn)
#else
#define LLBT_NORETURN
#endif


/* The way to specify that a variable or type is intended to possibly
 * not be used. Use it to suppress a warning from the compiler. */
#if __GNUC__
#define LLBT_UNUSED __attribute__((unused))
#else
#define LLBT_UNUSED
#endif

/* The way to specify that a function is deprecated
 * not be used. Use it to suppress a warning from the compiler. */
#if __GNUC__
#define LLBT_DEPRECATED(x) [[deprecated(x)]]
#else
#define LLBT_DEPRECATED(x) __declspec(deprecated(x))
#endif


#if __GNUC__ || defined __INTEL_COMPILER
#define LLBT_UNLIKELY(expr) __builtin_expect(!!(expr), 0)
#define LLBT_LIKELY(expr) __builtin_expect(!!(expr), 1)
#else
#define LLBT_UNLIKELY(expr) (expr)
#define LLBT_LIKELY(expr) (expr)
#endif


#if defined(__GNUC__) || defined(__HP_aCC)
#define LLBT_FORCEINLINE inline __attribute__((always_inline))
#elif defined(_MSC_VER)
#define LLBT_FORCEINLINE __forceinline
#else
#define LLBT_FORCEINLINE inline
#endif


#if LLBT_HAS_CPP_ATTRIBUTE(gnu::cold)
#define LLBT_COLD [[gnu::cold]]
#else
#define LLBT_COLD
#endif


#if LLBT_HAS_CPP_ATTRIBUTE(gnu::noinline)
#define LLBT_NOINLINE [[gnu::noinline]]
#elif defined(__GNUC__) || defined(__HP_aCC)
#define LLBT_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define LLBT_NOINLINE __declspec(noinline)
#else
#define LLBT_NOINLINE
#endif


#if LLBT_HAS_CPP_ATTRIBUTE(nodiscard)
#define LLBT_NODISCARD [[nodiscard]]
#else
#if defined(__GNUC__) || defined(__HP_aCC)
#define LLBT_NODISCARD __attribute__((warn_unused_result))
#elif defined(_MSC_VER)
#define LLBT_NODISCARD _Check_return_
#else
#define LLBT_NODISCARD
#endif
#endif

/* Thread specific data (only for POD types) */
#if defined __clang__
#define LLBT_THREAD_LOCAL __thread
#else
#define LLBT_THREAD_LOCAL thread_local
#endif


#if defined ANDROID || defined __ANDROID_API__
#define LLBT_ANDROID 1
#define LLBT_LINUX 0
#elif defined(__linux__)
#define LLBT_ANDROID 0
#define LLBT_LINUX 1
#else
#define LLBT_ANDROID 0
#define LLBT_LINUX 0
#endif

#if defined _WIN32
#include <winapifamily.h>
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP | WINAPI_PARTITION_SYSTEM)
#define LLBT_WINDOWS 1
#define LLBT_UWP 0
#elif WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_APP)
#define LLBT_WINDOWS 0
#define LLBT_UWP 1
#endif
#else
#define LLBT_WINDOWS 0
#define LLBT_UWP 0
#endif

// Some documentation of the defines provided by Apple:
// http://developer.apple.com/library/mac/documentation/Porting/Conceptual/PortingUnix/compiling/compiling.html#//apple_ref/doc/uid/TP40002850-SW13
#if defined __APPLE__ && defined __MACH__
#define LLBT_PLATFORM_APPLE 1
/* Apple OSX and iOS (Darwin). */
#include <Availability.h>
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE == 1 && TARGET_OS_IOS == 1
/* Device (iPhone or iPad) or simulator. */
#define LLBT_IOS 1
#define LLBT_APPLE_DEVICE !TARGET_OS_SIMULATOR
#define LLBT_MACCATALYST TARGET_OS_MACCATALYST
#else
#define LLBT_IOS 0
#define LLBT_MACCATALYST 0
#endif
#if TARGET_OS_WATCH == 1
/* Device (Apple Watch) or simulator. */
#define LLBT_WATCHOS 1
#define LLBT_APPLE_DEVICE !TARGET_OS_SIMULATOR
#else
#define LLBT_WATCHOS 0
#endif
#if TARGET_OS_TV
/* Device (Apple TV) or simulator. */
#define LLBT_TVOS 1
#define LLBT_APPLE_DEVICE !TARGET_OS_SIMULATOR
#else
#define LLBT_TVOS 0
#endif
#else
#define LLBT_PLATFORM_APPLE 0
#define LLBT_MACCATALYST 0
#define LLBT_IOS 0
#define LLBT_WATCHOS 0
#define LLBT_TVOS 0
#endif
#ifndef LLBT_APPLE_DEVICE
#define LLBT_APPLE_DEVICE 0
#endif

#if LLBT_ANDROID || LLBT_IOS || LLBT_WATCHOS || LLBT_TVOS || LLBT_UWP
#define LLBT_MOBILE 1
#else
#define LLBT_MOBILE 0
#endif


#if defined(LLBT_DEBUG) && !defined(LLBT_COOKIE_CHECK)
#define LLBT_COOKIE_CHECK
#endif

// We're in i686 mode
#if defined(__i386) || defined(__i386__) || defined(__i686__) || defined(_M_I86) || defined(_M_IX86)
#define LLBT_ARCHITECTURE_X86_32 1
#else
#define LLBT_ARCHITECTURE_X86_32 0
#endif

// We're in amd64 mode
#if defined(__amd64) || defined(__amd64__) || defined(__x86_64) || defined(__x86_64__) || defined(_M_X64) || \
    defined(_M_AMD64)
#define LLBT_ARCHITECTURE_X86_64 1
#else
#define LLBT_ARCHITECTURE_X86_64 0
#endif

#if defined(__arm__) || defined(_M_ARM)
#define LLBT_ARCHITECTURE_ARM32 1
#else
#define LLBT_ARCHITECTURE_ARM32 0
#endif

#if defined(__aarch64__) || defined(_M_ARM64) || defined(_M_ARM64EC)
#define LLBT_ARCHITECTURE_ARM64 1
#else
#define LLBT_ARCHITECTURE_ARM64 0
#endif

// Address Sanitizer
#if defined(__has_feature) // Clang
#  if __has_feature(address_sanitizer)
#    define LLBT_SANITIZE_ADDRESS 1
#  else
#    define LLBT_SANITIZE_ADDRESS 0
#  endif
#elif defined(__SANITIZE_ADDRESS__) && __SANITIZE_ADDRESS__ // GCC
#  define LLBT_SANITIZE_ADDRESS 1
#else
#  define LLBT_SANITIZE_ADDRESS 0
#endif

// Thread Sanitizer
#if defined(__has_feature) // Clang
#  if __has_feature(thread_sanitizer)
#    define LLBT_SANITIZE_THREAD 1
#  else
#    define LLBT_SANITIZE_THREAD 0
#  endif
#elif defined(__SANITIZE_THREAD__) && __SANITIZE_THREAD__ // GCC
#  define LLBT_SANITIZE_THREAD 1
#else
#  define LLBT_SANITIZE_THREAD 0
#endif

#endif /* LLBT_UTIL_FEATURES_H */
