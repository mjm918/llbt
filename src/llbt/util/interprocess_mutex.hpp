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

#ifndef LLBT_UTIL_INTERPROCESS_MUTEX
#define LLBT_UTIL_INTERPROCESS_MUTEX

#include <llbt/util/features.h>
#include <llbt/util/thread.hpp>
#include <llbt/util/file.hpp>
#include <llbt/utilities.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#if LLBT_PLATFORM_APPLE
#include <dispatch/dispatch.h>
#endif

// Enable this only on platforms where it might be needed
#if LLBT_PLATFORM_APPLE || LLBT_ANDROID
#define LLBT_ROBUST_MUTEX_EMULATION 1
#else
#define LLBT_ROBUST_MUTEX_EMULATION 0
#endif

namespace llbt::util {

// fwd decl to support friend decl below
class InterprocessCondVar;

// A wrapper for a semaphore to expose a mutex interface. Unlike a real mutex,
// this can be locked and unlocked from different threads. Currently only
// implemented on Apple platforms
class SemaphoreMutex {
public:
    SemaphoreMutex() noexcept;
    ~SemaphoreMutex() noexcept;

    SemaphoreMutex(const SemaphoreMutex&) = delete;
    SemaphoreMutex& operator=(const SemaphoreMutex&) = delete;

    void lock() noexcept;
    void unlock() noexcept;
    bool try_lock() noexcept;

private:
#if LLBT_PLATFORM_APPLE
    dispatch_semaphore_t m_semaphore;
#endif
};


/// Emulation of a Robust Mutex.
/// A Robust Mutex is an interprocess mutex which will automatically
/// release any locks held by a process when it crashes. Contrary to
/// Posix robust mutexes, this robust mutex is not capable of informing
/// participants that they have been granted a lock after a crash of
/// the process holding it (though it could be added if needed).

class InterprocessMutex {
public:
    InterprocessMutex() = default;
    ~InterprocessMutex() noexcept;

    // Disable copying. Copying a locked Mutex will create a scenario
    // where the same file descriptor will be locked once but unlocked twice.
    InterprocessMutex(const InterprocessMutex&) = delete;
    InterprocessMutex& operator=(const InterprocessMutex&) = delete;

#if LLBT_ROBUST_MUTEX_EMULATION || defined(_WIN32)
    struct SharedPart {};
#else
    using SharedPart = RobustMutex;
#endif

    /// You need to bind the emulation to a SharedPart in shared/mmapped memory.
    /// The SharedPart is assumed to have been initialized (possibly by another process)
    /// elsewhere.
    ///
    /// With `in_process_only` the caller promises that no other process will
    /// use this mutex (e.g. an in-memory store, or a file the application
    /// opens from a single process). Where the robust-mutex emulation would
    /// pay a file-lock syscall per lock/unlock, the mutex then runs on a
    /// process-wide semaphore instead — shared between all InterprocessMutex
    /// instances bound to the same underlying file, so multiple DB objects
    /// in one process still exclude each other.
    void set_shared_part(SharedPart& shared_part, const std::string& path, const std::string& mutex_name,
                         bool in_process_only = false);

    /// Destroy shared object. Potentially release system resources. Caller must
    /// ensure that the shared_part is not in use at the point of call.
    void release_shared_part();

    /// Lock the mutex. If the mutex is already locked, wait for it to be unlocked.
    void lock();

    /// Non-blocking attempt to lock the mutex. Returns true if the lock is obtained.
    /// If the lock can not be obtained return false immediately.
    bool try_lock();

    /// Unlock the mutex
    void unlock();

    /// Attempt to check if the mutex is valid (only relevant if not emulating)
    bool is_valid() noexcept;

#if LLBT_ROBUST_MUTEX_EMULATION
    constexpr static bool is_robust_on_this_platform = true; // we're faking it!
#else
    constexpr static bool is_robust_on_this_platform = RobustMutex::is_robust_on_this_platform;
#endif

#if LLBT_PLATFORM_APPLE
    // On Apple platforms we support locking and unlocking InterprocessMutex on
    // different threads, while on other platforms the locking thread owns the
    // mutex. The non-thread-confined version should be implementable on more
    // platforms if desired.
    constexpr static bool is_thread_confined = false;
#else
    constexpr static bool is_thread_confined = true;
#endif

private:
#if LLBT_ROBUST_MUTEX_EMULATION
    File m_file;
#if LLBT_PLATFORM_APPLE
    using LocalMutex = SemaphoreMutex;
#else
    using LocalMutex = Mutex;
#endif
    LocalMutex m_local_mutex;
    // Every instance for the same lock file shares this process-wide mutex.
    // Cross-process mode takes the file lock as well; single-process mode can
    // skip it without splitting same-process users into two lock domains.
    std::shared_ptr<LocalMutex> m_shared_in_process;
    bool m_use_file_lock = true;
    static std::shared_ptr<LocalMutex> shared_in_process_mutex(const File::UniqueID& uid);

#else
    SharedPart* m_shared_part = nullptr;

#ifdef _WIN32
    HANDLE m_handle = 0;
#endif

#endif
    friend class InterprocessCondVar;
};

inline InterprocessMutex::~InterprocessMutex() noexcept
{
#ifdef _WIN32
    if (m_handle) {
        bool b = CloseHandle(m_handle);
        LLBT_ASSERT_RELEASE(b);
    }
#endif
}

#if LLBT_ROBUST_MUTEX_EMULATION
inline std::shared_ptr<InterprocessMutex::LocalMutex>
InterprocessMutex::shared_in_process_mutex(const File::UniqueID& uid)
{
    // One process-wide mutex per underlying lock file, so every
    // InterprocessMutex bound to the same file shares it.
    static std::mutex s_registry_mutex;
    static std::map<File::UniqueID, std::weak_ptr<LocalMutex>> s_registry;
    std::lock_guard<std::mutex> lg(s_registry_mutex);
    auto& slot = s_registry[uid];
    if (auto existing = slot.lock())
        return existing;
    auto fresh = std::make_shared<LocalMutex>();
    slot = fresh;
    return fresh;
}
#endif

inline void InterprocessMutex::set_shared_part(SharedPart& shared_part, const std::string& path,
                                               const std::string& mutex_name, bool in_process_only)
{
#if LLBT_ROBUST_MUTEX_EMULATION
    static_cast<void>(shared_part);

    std::string filename;
    if (path.size() == 0) {
        filename = make_temp_file(mutex_name.c_str());
    }
    else {
        filename = util::format("%1.%2.mx", path, mutex_name);
    }

    // Always open file for write and retreive the uid in case other process
    // deletes the file. Avoid using just mode_Write (which implies truncate).
    // On fat32/exfat uid could be reused by OS in a situation when
    // multiple processes open and truncate the same lock file concurrently.
    m_file.close();
    m_file.open(filename, File::mode_Append);
    // exFAT does not allocate a unique id for the file until it's non-empty
    m_file.resize(1);
    m_shared_in_process = shared_in_process_mutex(File::get_unique_id(m_file.get_descriptor(), filename));
    m_use_file_lock = !in_process_only;

#elif defined(_WIN32)
    static_cast<void>(in_process_only);
    if (m_handle) {
        bool b = CloseHandle(m_handle);
        LLBT_ASSERT_RELEASE(b);
    }
    // replace backslashes because they're significant in object namespace names
    std::string path_escaped = path;
    std::replace(path_escaped.begin(), path_escaped.end(), '\\', '/');
    std::string name = "Local\\llbt_named_intermutex_" + path_escaped + mutex_name;

    std::wstring wname(name.begin(), name.end());
    m_handle = CreateMutexW(0, false, wname.c_str());
    if (!m_handle) {
        throw std::system_error(GetLastError(), std::system_category(), "Error opening mutex");
    }
#else
    m_shared_part = &shared_part;
    static_cast<void>(path);
    static_cast<void>(mutex_name);
#endif
}

inline void InterprocessMutex::release_shared_part()
{
#if LLBT_ROBUST_MUTEX_EMULATION
    m_shared_in_process.reset();
    m_use_file_lock = true;
    if (m_file.is_attached()) {
        m_file.close();
        File::try_remove(m_file.get_path());
    }
#else
    m_shared_part = nullptr;
#endif
}

inline void InterprocessMutex::lock()
{
#if LLBT_ROBUST_MUTEX_EMULATION
    std::unique_lock mutex_lock(m_local_mutex);
    m_shared_in_process->lock();
    if (m_use_file_lock) {
        try {
            m_file.lock();
        }
        catch (...) {
            m_shared_in_process->unlock();
            throw;
        }
    }
    mutex_lock.release();
#else

#ifdef _WIN32
    DWORD d = WaitForSingleObject(m_handle, INFINITE);
    LLBT_ASSERT_RELEASE(d != WAIT_FAILED);
#else
    LLBT_ASSERT(m_shared_part);
    m_shared_part->lock([]() {});
#endif
#endif
}

inline bool InterprocessMutex::try_lock()
{
#if LLBT_ROBUST_MUTEX_EMULATION
    std::unique_lock mutex_lock(m_local_mutex, std::try_to_lock_t());
    if (!mutex_lock.owns_lock()) {
        return false;
    }
    if (!m_shared_in_process->try_lock()) {
        return false;
    }
    if (m_use_file_lock) {
        try {
            if (!m_file.try_lock()) {
                m_shared_in_process->unlock();
                return false;
            }
        }
        catch (...) {
            m_shared_in_process->unlock();
            throw;
        }
    }
    mutex_lock.release();
    return true;
#elif defined(_WIN32)
    DWORD ret = WaitForSingleObject(m_handle, 0);
    LLBT_ASSERT_RELEASE(ret != WAIT_FAILED);

    if (ret == WAIT_OBJECT_0) {
        return true;
    }
    else {
        return false;
    }
#else
    LLBT_ASSERT(m_shared_part);
    return m_shared_part->try_lock([]() {});
#endif
}


inline void InterprocessMutex::unlock()
{
#if LLBT_ROBUST_MUTEX_EMULATION
    if (m_use_file_lock)
        m_file.unlock();
    m_shared_in_process->unlock();
    m_local_mutex.unlock();
#else
#ifdef _WIN32
    bool b = ReleaseMutex(m_handle);
    LLBT_ASSERT_RELEASE(b);
#else
    LLBT_ASSERT(m_shared_part);
    m_shared_part->unlock();
#endif
#endif
}


inline bool InterprocessMutex::is_valid() noexcept
{
#if LLBT_ROBUST_MUTEX_EMULATION
    return true;
#elif defined(_WIN32)
    // There is no safe way of testing if the m_handle mutex handle is valid on Windows, without having bad side
    // effects for the cases where it is indeed invalid. If m_handle contains an arbitrary value, it might by
    // coincidence be equal to a real live handle of another kind. This excludes a try_lock implementation and many
    // other ideas.
    return true;
#else
    LLBT_ASSERT(m_shared_part);
    return m_shared_part->is_valid();
#endif
}


} // namespace llbt::util

#endif // #ifndef LLBT_UTIL_INTERPROCESS_MUTEX
