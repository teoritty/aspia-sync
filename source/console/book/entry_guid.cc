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

#include "console/book/entry_guid.h"

#include "base/guid.h"
#include "proto/address_book.pb.h"

namespace console {

namespace {

//--------------------------------------------------------------------------------------------------
// Gives the record a guid if what it carries is not one. Returns whether it had to.
template <class Entry>
bool ensureGuid(Entry* entry)
{
    if (isValidEntryGuid(entry->guid()))
        return false;

    entry->set_guid(createEntryGuid());
    return true;
}

} // namespace

//--------------------------------------------------------------------------------------------------
std::string createEntryGuid()
{
    return base::Guid::create().toStdString();
}

//--------------------------------------------------------------------------------------------------
bool isValidEntryGuid(const std::string& guid)
{
    // A nil guid passes the format check but is what an uninitialized field looks like when
    // somebody writes one out, so it is not accepted as an identity.
    static const char kNullGuid[] = "00000000-0000-0000-0000-000000000000";

    if (guid == kNullGuid)
        return false;

    return base::Guid::isValidGuidString(guid);
}

//--------------------------------------------------------------------------------------------------
size_t ensureEntryGuids(proto::address_book::ComputerGroup* group)
{
    if (!group)
        return 0;

    size_t count = 0;

    if (ensureGuid(group))
        ++count;

    for (int i = 0; i < group->computer_size(); ++i)
    {
        if (ensureGuid(group->mutable_computer(i)))
            ++count;
    }

    for (int i = 0; i < group->computer_group_size(); ++i)
        count += ensureEntryGuids(group->mutable_computer_group(i));

    return count;
}

} // namespace console
