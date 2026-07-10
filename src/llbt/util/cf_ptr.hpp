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

#ifndef LLBT_UTIL_CF_PTR_HPP
#define LLBT_UTIL_CF_PTR_HPP

#include <llbt/util/assert.hpp>

#if LLBT_PLATFORM_APPLE

#include <CoreFoundation/CoreFoundation.h>

namespace llbt {
namespace util {

template <class Ref>
class CFPtr {
public:
    explicit CFPtr(Ref ref = nullptr) noexcept
        : m_ref(ref)
    {
    }

    CFPtr(CFPtr&& rg) noexcept
        : m_ref(rg.m_ref)
    {
        rg.m_ref = nullptr;
    }

    ~CFPtr() noexcept
    {
        if (m_ref)
            CFRelease(m_ref);
    }

    CFPtr& operator=(CFPtr&& rg) noexcept
    {
        LLBT_ASSERT(!m_ref || m_ref != rg.m_ref);
        if (m_ref)
            CFRelease(m_ref);
        m_ref = rg.m_ref;
        rg.m_ref = nullptr;
        return *this;
    }

    explicit operator bool() const noexcept
    {
        return bool(m_ref);
    }

    Ref get() const noexcept
    {
        return m_ref;
    }

    Ref release() noexcept
    {
        Ref ref = m_ref;
        m_ref = nullptr;
        return ref;
    }

    void reset(Ref ref = nullptr) noexcept
    {
        LLBT_ASSERT(!m_ref || m_ref != ref);
        if (m_ref)
            CFRelease(m_ref);
        m_ref = ref;
    }

private:
    Ref m_ref;
};

template <class Ref>
CFPtr<Ref> adoptCF(Ref ptr)
{
    return CFPtr<Ref>(ptr);
}

template <class Ref>
CFPtr<Ref> retainCF(Ref ptr)
{
    CFRetain(ptr);
    return CFPtr<Ref>(ptr);
}
} // namespace util
} // namespace llbt


#endif // LLBT_PLATFORM_APPLE

#endif // LLBT_UTIL_CF_PTR_HPP
