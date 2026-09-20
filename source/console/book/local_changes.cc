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

#include "console/book/local_changes.h"

#include "console/book/flat_book.h"
#include "proto/address_book.pb.h"

#include <map>
#include <set>

namespace console {

namespace {

using proto::address_book::Data;
using proto::address_book::SyncEntryState;

} // namespace

//--------------------------------------------------------------------------------------------------
LocalChanges markLocalChanges(Data* data)
{
    LocalChanges changes;

    if (!data || data->sync().book_guid().empty())
        return changes;

    const std::vector<FlatEntry> entries = flattenBook(data->root_group());

    std::map<std::string, const FlatEntry*> in_book;
    for (const FlatEntry& entry : entries)
        in_book.emplace(entry.guid, &entry);

    proto::address_book::SyncState* sync = data->mutable_sync();

    std::set<std::string> known;

    for (int i = 0; i < sync->entry_size(); ++i)
    {
        SyncEntryState* state = sync->mutable_entry(i);
        known.insert(state->guid());

        auto it = in_book.find(state->guid());
        if (it == in_book.end())
        {
            // The record is gone from the book. A base for it means the router still has it, so
            // the deletion has to be sent; without one there was never anything to delete.
            if (!state->base_payload().empty() && !state->deleted())
            {
                state->set_deleted(true);
                state->set_dirty(false);
                ++changes.deleted;
            }
            else if (state->deleted())
            {
                ++changes.deleted;
            }
            continue;
        }

        if (state->deleted())
        {
            // It is back - restored from the recently deleted, or re-created with the same
            // identity. Either way it is no longer a deletion to send.
            state->set_deleted(false);
        }

        const bool content_differs = (state->base_payload() != it->second->payload);
        const bool moved = (state->base_parent_guid() != it->second->parent_guid);

        if (content_differs || moved)
        {
            state->set_dirty(true);
            ++changes.changed;
        }
        else if (state->dirty())
        {
            // Changed and then changed back to exactly what the router has. There is nothing to
            // send about it any more, and sending it would only make the router refuse somebody
            // else's work for no reason.
            state->set_dirty(false);
        }
    }

    for (const FlatEntry& entry : entries)
    {
        if (known.count(entry.guid))
            continue;

        // Never seen by the router. Its base stays empty, which is also what tells the router the
        // record is new rather than an edit of something it already has.
        SyncEntryState* state = sync->add_entry();
        state->set_guid(entry.guid);
        state->set_revision(0);
        state->set_dirty(true);

        ++changes.added;
    }

    return changes;
}

} // namespace console
