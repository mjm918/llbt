cmake_minimum_required(VERSION 3.20)
project(llbt VERSION 3.0.0 LANGUAGES C CXX)

if(NOT DEFINED CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(LLBT_ENABLE_ENCRYPTION "Enable encrypted page mapping (AES)" OFF)
option(LLBT_ENABLE_ASSERTIONS "Enable LLBT_ASSERT in release builds" OFF)
option(LLBT_ENABLE_ALLOC_SET_ZERO "Zero-fill new allocations" OFF)
option(LLBT_ENABLE_MEMDEBUG "Memory debug checks" OFF)
option(LLBT_BUILD_TESTS "Build llbt tests" ON)
option(LLBT_BUILD_EXAMPLES "Build llbt examples" OFF)
option(LLBT_BUILD_BENCH "Build llbt benchmarks" OFF)
option(LLBT_INSTALL "Generate install rules" ${PROJECT_IS_TOP_LEVEL})
set(LLBT_MAX_BPNODE_SIZE 1000 CACHE STRING "Maximum B+tree node size")

# ---- feature detection (mirrors upstream config.h.in) ----
include(CheckSymbolExists)
include(CheckCXXSourceCompiles)
find_package(Backtrace)
if(Backtrace_FOUND)
    set(LLBT_HAVE_BACKTRACE 1)
else()
    set(Backtrace_HEADER "execinfo.h")
    set(LLBT_HAVE_BACKTRACE 0)
endif()
check_symbol_exists(posix_fallocate "fcntl.h" LLBT_HAVE_POSIX_FALLOCATE)
check_symbol_exists(readdir64 "dirent.h" LLBT_HAVE_READDIR64)
check_cxx_source_compiles("
    #include <pthread.h>
    int main() { char n[16]; pthread_getname_np(pthread_self(), n, 16); }
" LLBT_HAVE_PTHREAD_GETNAME)
check_cxx_source_compiles("
    #include <pthread.h>
    int main() {
    #ifdef __APPLE__
        pthread_setname_np(\"n\");
    #else
        pthread_setname_np(pthread_self(), \"n\");
    #endif
    }
" LLBT_HAVE_PTHREAD_SETNAME)

set(LLBT_HAVE_OPENSSL 0)
set(LLBT_HAVE_SECURE_TRANSPORT 0)
if(LLBT_ENABLE_ENCRYPTION AND NOT APPLE)
    find_package(OpenSSL REQUIRED)
    set(LLBT_HAVE_OPENSSL 1)
endif()
set(LLBT_USE_SYSTEM_OPENSSL_PATHS ${LLBT_HAVE_OPENSSL})
set(LLBT_INCLUDE_CERTS 0)
set(LLBT_ENABLE_GEOSPATIAL 0)
set(LLBT_VALGRIND 0)
set(LLBT_ASAN 0)
set(LLBT_TSAN 0)
set(VERSION ${PROJECT_VERSION})

# 0/1 normalization for cmakedefine01
foreach(var LLBT_HAVE_POSIX_FALLOCATE LLBT_HAVE_READDIR64
            LLBT_HAVE_PTHREAD_GETNAME LLBT_HAVE_PTHREAD_SETNAME)
    if(${var})
        set(${var} 1)
    else()
        set(${var} 0)
    endif()
endforeach()
foreach(opt ENCRYPTION ASSERTIONS ALLOC_SET_ZERO MEMDEBUG)
    if(LLBT_ENABLE_${opt})
        set(LLBT_ENABLE_${opt} 1)
    else()
        set(LLBT_ENABLE_${opt} 0)
    endif()
endforeach()

# ---- generated headers (templates arrive with the phase-1 import) ----
set(LLBT_GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/llbt/util/config.h.in)
    configure_file(src/llbt/util/config.h.in ${LLBT_GENERATED_DIR}/llbt/util/config.h)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/llbt/version_numbers.hpp.in)
    set(CONFIG_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
    set(CONFIG_VERSION_MINOR ${PROJECT_VERSION_MINOR})
    set(CONFIG_VERSION_PATCH ${PROJECT_VERSION_PATCH})
    set(CONFIG_VERSION_TWEAK "")
    set(CONFIG_VERSION ${PROJECT_VERSION})
    configure_file(src/llbt/version_numbers.hpp.in ${LLBT_GENERATED_DIR}/llbt/version_numbers.hpp)
endif()

# ---- vendored third-party (see THIRD-PARTY-NOTICES) ----
include(CheckCXXCompilerFlag)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/external/IntelRDFPMathLib20U2/CMakeLists.txt)
    add_subdirectory(src/external/IntelRDFPMathLib20U2)
    set(LLBT_BID_OBJECTS $<TARGET_OBJECTS:Bid>)
endif()

# ---- library ----
file(GLOB_RECURSE LLBT_SOURCES CONFIGURE_DEPENDS src/llbt/*.cpp)
if(APPLE)
    file(GLOB_RECURSE LLBT_OBJC_SOURCES CONFIGURE_DEPENDS src/llbt/*.mm)
    list(APPEND LLBT_SOURCES ${LLBT_OBJC_SOURCES})
endif()
if(LLBT_SOURCES)
    add_library(llbt-core STATIC ${LLBT_SOURCES} ${LLBT_BID_OBJECTS})
    target_compile_features(llbt-core PUBLIC cxx_std_17)
    target_include_directories(llbt-core PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${LLBT_GENERATED_DIR}>
        $<INSTALL_INTERFACE:include>)
    if(LLBT_HAVE_OPENSSL)
        target_link_libraries(llbt-core PUBLIC OpenSSL::Crypto)
    endif()
    if(Backtrace_FOUND AND Backtrace_LIBRARIES)
        target_link_libraries(llbt-core PUBLIC ${Backtrace_LIBRARIES})
    endif()
    find_package(Threads REQUIRED)
    target_link_libraries(llbt-core PUBLIC Threads::Threads)
    if(WIN32)
        target_compile_definitions(llbt-core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    if(APPLE)
        target_link_libraries(llbt-core PUBLIC "-framework Foundation" "-framework CoreFoundation")
    endif()
    add_library(llbt::core ALIAS llbt-core)
endif()

# ---- tests ----
if(LLBT_BUILD_TESTS AND PROJECT_IS_TOP_LEVEL AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test/CMakeLists.txt)
    enable_testing()
    add_subdirectory(test)
endif()

# ---- examples ----
if(LLBT_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ---- benchmarks ----
if(LLBT_BUILD_BENCH)
    add_subdirectory(bench)
endif()

# ---- install / export ----
if(LLBT_INSTALL AND TARGET llbt-core)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    install(TARGETS llbt-core EXPORT llbtTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(DIRECTORY include/llbt DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(DIRECTORY src/llbt DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h")
    install(FILES src/llbt.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(DIRECTORY src/external/json
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/external)
    install(DIRECTORY ${LLBT_GENERATED_DIR}/llbt DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(FILES LICENSE NOTICE THIRD-PARTY-NOTICES
        DESTINATION ${CMAKE_INSTALL_DATADIR}/llbt)

    install(EXPORT llbtTargets NAMESPACE llbt:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/llbt)
    write_basic_package_version_file(${CMAKE_BINARY_DIR}/llbtConfigVersion.cmake
        COMPATIBILITY SameMajorVersion)
    file(WRITE ${CMAKE_BINARY_DIR}/llbtConfig.cmake
        "include(CMakeFindDependencyMacro)\nfind_dependency(Threads)\ninclude(\"\${CMAKE_CURRENT_LIST_DIR}/llbtTargets.cmake\")\n")
    install(FILES ${CMAKE_BINARY_DIR}/llbtConfig.cmake ${CMAKE_BINARY_DIR}/llbtConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/llbt)
endif()
