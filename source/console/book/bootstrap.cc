//
// Aspia Project
// Copyright (C) 2016-2024 Dmitry Chapyshev <dmitry@aspia.ru>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.
//

#include "console/book/bootstrap.h"

#include "proto/address_book.pb.h"

#include <map>
#include <set>

namespace console {

namespace {

using proto::address_book::Computer;
using proto::address_book::ComputerGroup;

struct Record
{
    std::string guid;
    std::string name;
    std::string address;
};

//--------------------------------------------------------------------------------------------------
// What a record is reached by. The port is part of it for a record entered by address, because two
// machines behind one address on different ports are two machines. An id issued by the router
// carries no port of its own, so it is left out of the key there.
std::string addressKey(const Computer& computer)
{
    const std::string& address = computer.address();
    if (address.empty())
        return std::string();

    bool host_id = true;
    for (char character : address)
    {
        if (character < '0' || character > '9')
        {
            host_id = false;
            break;
        }
    }

    if (host_id)
        return "id:" + address;

    return "addr:" + address + ":" + std::to_string(computer.port());
}

//--------------------------------------------------------------------------------------------------
void collect(const ComputerGroup& group, std::vector<Record>* out)
{
    for (int i = 0; i < group.computer_size(); ++i)
    {
        const Computer& computer = group.computer(i);

        Record record;
        record.guid = computer.guid();
        record.name = computer.name();
        record.address = addressKey(computer);

        out->emplace_back(std::move(record));
    }

    for (int i = 0; i < group.computer_group_size(); ++i)
        collect(group.computer_group(i), out);
}

//--------------------------------------------------------------------------------------------------
// Indexes the records by what they are reached by, and reports the addresses that appear more than
// once: those cannot be matched by address, because there is no telling which of the several is
// meant.
void index(const std::vector<Record>& records,
           std::map<std::string, const Record*>* by_address,
           std::set<std::string>* repeated)
{
    for (const Record& record : records)
    {
        if (record.address.empty())
            continue;

        if (!by_address->emplace(record.address, &record).second)
            repeated->insert(record.address);
    }
}

} // namespace

//--------------------------------------------------------------------------------------------------
BootstrapPlan planBootstrap(const ComputerGroup& local, const ComputerGroup& remote)
{
    BootstrapPlan plan;

    std::vector<Record> local_records;
    std::vector<Record> remote_records;

    collect(local, &local_records);
    collect(remote, &remote_records);

    std::map<std::string, const Record*> local_by_address;
    std::map<std::string, const Record*> remote_by_address;
    std::set<std::string> repeated;

    index(local_records, &local_by_address, &repeated);
    index(remote_records, &remote_by_address, &repeated);

    for (const Record& record : local_records)
    {
        if (record.guid.empty())
            continue;

        // The same guid on both sides means this console has already seen the shared book - there
        // is nothing to guess about, the records are the same by identity.
        if (record.address.empty())
        {
            plan.only_local.emplace_back(record.guid);
            continue;
        }

        if (repeated.count(record.address))
        {
            plan.ambiguous.emplace_back(record.guid);
            continue;
        }

        auto it = remote_by_address.find(record.address);
        if (it == remote_by_address.end())
        {
            plan.only_local.emplace_back(record.guid);
            continue;
        }

        BootstrapPair pair;
        pair.local_guid = record.guid;
        pair.remote_guid = it->second->guid;
        pair.local_name = record.name;
        pair.remote_name = it->second->name;
        pair.address = record.address;
        pair.names_differ = (record.name != it->second->name);

        plan.matched.emplace_back(std::move(pair));
    }

    return plan;
}

} // namespace console
