#ifndef LLBT_UTIL_PLATFORM_INFO_HPP
#define LLBT_UTIL_PLATFORM_INFO_HPP

#include <string>


namespace llbt {
namespace util {

/// Get a description of the current system platform.
///
/// Returns a space-separated concatenation of `osname`, `sysname`, `release`,
/// `version`, and `machine` as returned by get_platform_info(PlatformInfo&).
std::string get_platform_info();


struct PlatformInfo {
    std::string osname;  ///< Equivalent to `uname -o` (Linux).
    std::string sysname; ///< Equivalent to `uname -s`.
    std::string release; ///< Equivalent to `uname -r`.
    std::string version; ///< Equivalent to `uname -v`.
    std::string machine; ///< Equivalent to `uname -m`.
};

/// Get a description of the current system platform.
void get_platform_info(PlatformInfo&);


// Implementation

inline std::string get_platform_info()
{
    PlatformInfo info;
    get_platform_info(info); // Throws
    return (info.osname + " " + info.sysname + " " + info.release + " " + info.version + " " +
            info.machine); // Throws
}

inline std::string get_library_platform()
{
#if LLBT_ANDROID
    return "Android";
#elif LLBT_WINDOWS
    return "Windows";
#elif LLBT_UWP
    return "UWP";
#elif LLBT_MACCATALYST // test Catalyst first because it's a subset of iOS
    return "Mac Catalyst";
#elif LLBT_IOS
    return "iOS";
#elif LLBT_TVOS
    return "tvOS";
#elif LLBT_WATCHOS
    return "watchOS";
#elif LLBT_PLATFORM_APPLE
    return "macOS";
#elif LLBT_LINUX
    return "Linux";
#endif

    return "unknown";
}

inline std::string get_library_cpu_arch()
{
#if LLBT_ARCHITECTURE_ARM32
    return "arm";
#elif LLBT_ARCHITECTURE_ARM64
    return "arm64";
#elif LLBT_ARCHITECTURE_X86_32
    return "x86";
#elif LLBT_ARCHITECTURE_X86_64
    return "x86_64";
#endif

    return "unknown";
}

} // namespace util
} // namespace llbt

#endif // LLBT_UTIL_PLATFORM_INFO_HPP
