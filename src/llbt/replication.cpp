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

#include <llbt/replication.hpp>

#include <llbt/path.hpp>
#include <iostream>

using namespace llbt;
using namespace llbt::util;
using LogLevel = util::Logger::Level;

const char* Replication::history_type_name(int type)
{
    switch (type) {
        case hist_None:
            return "None";
        case hist_OutOfBarq:
            return "Local out of Barq";
        case hist_InBarq:
            return "Local in-Barq";
        case hist_SyncClient:
            return "SyncClient";
        case hist_SyncServer:
            return "SyncServer";
        default:
            return "Unknown";
    }
}

void Replication::initialize(DB&)
{
    // Nothing needs to be done here
}

void Replication::do_initiate_transact(Group&, version_type, bool)
{
    char* data = m_stream.get_data();
    size_t size = m_stream.get_size();
    m_encoder.set_buffer(data, data + size);
}

Replication::version_type Replication::prepare_commit(version_type orig_version)
{
    char* data = m_stream.get_data();
    size_t size = m_encoder.write_position() - data;
    version_type new_version = prepare_changeset(data, size, orig_version); // Throws
    return new_version;
}

// llbt scope cut: table-layer instrumentation removed.
