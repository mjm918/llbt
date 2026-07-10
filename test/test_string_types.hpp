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

#ifndef LLBT_TEST_STRING_TYPES_HPP
#define LLBT_TEST_STRING_TYPES_HPP

#include <llbt/alloc.hpp>
#include <llbt/bplustree.hpp>
#include <llbt/array_string.hpp>
#include <llbt/array_key.hpp>

struct string_array {
    using ColumnTestType = llbt::ArrayString;
    string_array(bool = false)
    {
        m_col = new ColumnTestType(llbt::Allocator::get_default());
        m_col->create();
        m_ref = m_col->get_ref();
    }
    virtual ~string_array()
    {
        m_col->destroy();
        delete m_col;
        m_col = nullptr;
    }
    ColumnTestType& get_column()
    {
        return *m_col;
    }
    llbt::ref_type m_ref;
    ColumnTestType* m_col;
    static bool is_nullable()
    {
        return true;
    }
    static bool is_enumerated()
    {
        return false;
    }
};

struct string_column {
    using ColumnTestType = llbt::BPlusTree<llbt::StringData>;
    string_column(bool = false)
    {
        m_col = new ColumnTestType(llbt::Allocator::get_default());
        m_col->create();
        m_ref = m_col->get_ref();
    }
    virtual ~string_column()
    {
        m_col->destroy();
        delete m_col;
        m_col = nullptr;
    }
    ColumnTestType& get_column()
    {
        return *m_col;
    }
    llbt::ref_type m_ref;
    ColumnTestType* m_col;
    static bool is_nullable()
    {
        return false;
    }
    static bool is_enumerated()
    {
        return false;
    }
};

#endif // LLBT_TEST_STRING_TYPES_HPP
