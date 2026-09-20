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

#include "console/book/flat_book.h"

#include "console/book/entry_guid.h"
#include "proto/address_book.pb.h"

#include <map>
#include <vector>

namespace console {

namespace {

using proto::address_book::Computer;
using proto::address_book::ComputerGroup;

//--------------------------------------------------------------------------------------------------
// The group without what belongs to the records under it and without the state of one person's
// window.
std::string groupPayload(const ComputerGroup& group)
{
    ComputerGroup stripped(group);

    stripped.clear_computer();
    stripped.clear_computer_group();
    stripped.clear_expanded();

    return stripped.SerializeAsString();
}

//--------------------------------------------------------------------------------------------------
void flattenGroup(const ComputerGroup& group, const std::string& parent_guid,
                  std::vector<FlatEntry>* out)
{
    if (!isValidEntryGuid(group.guid()))
        return;

    FlatEntry entry;
    entry.guid = group.guid();
    entry.parent_guid = parent_guid;
    entry.kind = FlatEntry::Kind::GROUP;
    entry.payload = groupPayload(group);
    out->emplace_back(std::move(entry));

    for (int i = 0; i < group.computer_size(); ++i)
    {
        const Computer& computer = group.computer(i);
        if (!isValidEntryGuid(computer.guid()))
            continue;

        FlatEntry child;
        child.guid = computer.guid();
        child.parent_guid = group.guid();
        child.kind = FlatEntry::Kind::COMPUTER;
        child.payload = computer.SerializeAsString();
        out->emplace_back(std::move(child));
    }

    for (int i = 0; i < group.computer_group_size(); ++i)
        flattenGroup(group.computer_group(i), group.guid(), out);
}

} // namespace

//--------------------------------------------------------------------------------------------------
std::vector<FlatEntry> flattenBook(const ComputerGroup& root)
{
    std::vector<FlatEntry> entries;
    flattenGroup(root, std::string(), &entries);
    return entries;
}

//--------------------------------------------------------------------------------------------------
bool rebuildBook(const std::vector<FlatEntry>& entries, ComputerGroup* root, size_t* skipped)
{
    if (skipped)
        *skipped = 0;

    if (!root)
        return false;

    size_t skipped_count = 0;

    // A repeated guid is taken once. Two records under one name would otherwise both be placed and
    // the book would grow a copy of something with every exchange.
    std::map<std::string, const FlatEntry*> by_guid;
    const FlatEntry* root_entry = nullptr;

    for (const FlatEntry& entry : entries)
    {
        if (!isValidEntryGuid(entry.guid))
        {
            ++skipped_count;
            continue;
        }

        if (!by_guid.emplace(entry.guid, &entry).second)
        {
            ++skipped_count;
            continue;
        }

        if (entry.parent_guid.empty() && entry.kind == FlatEntry::Kind::GROUP && !root_entry)
            root_entry = &entry;
    }

    if (!root_entry)
    {
        if (skipped)
            *skipped = skipped_count;
        return false;
    }

    ComputerGroup rebuilt;
    if (!rebuilt.ParseFromString(root_entry->payload))
    {
        if (skipped)
            *skipped = skipped_count;
        return false;
    }
    rebuilt.set_guid(root_entry->guid);

    // Where each group ended up, so the records naming it can be put inside. Protobuf keeps the
    // address of an element of a repeated field stable when the field grows, so these stay valid.
    std::map<std::string, ComputerGroup*> groups;
    groups.emplace(root_entry->guid, &rebuilt);

    std::vector<const FlatEntry*> pending;
    pending.reserve(by_guid.size());

    for (const auto& [guid, entry] : by_guid)
    {
        if (entry == root_entry)
            continue;
        pending.push_back(entry);
    }

    // Placing groups first, by repeated passes: every pass puts in the ones whose parent is
    // already there. What is left after a pass that placed nothing hangs off a missing parent or
    // off a cycle, and goes to the root.
    bool progress = true;
    while (progress)
    {
        progress = false;

        for (auto it = pending.begin(); it != pending.end();)
        {
            const FlatEntry* entry = *it;
            if (entry->kind != FlatEntry::Kind::GROUP)
            {
                ++it;
                continue;
            }

            auto parent = groups.find(entry->parent_guid);
            if (parent == groups.end())
            {
                ++it;
                continue;
            }

            ComputerGroup* group = parent->second->add_computer_group();
            if (!group->ParseFromString(entry->payload))
            {
                parent->second->mutable_computer_group()->RemoveLast();
                ++skipped_count;
            }
            else
            {
                group->set_guid(entry->guid);
                groups.emplace(entry->guid, group);
            }

            it = pending.erase(it);
            progress = true;
        }
    }

    for (const FlatEntry* entry : pending)
    {
        if (entry->kind != FlatEntry::Kind::GROUP)
            continue;

        // Its parent is gone or its chain of parents loops. Keeping it at the root loses where it
        // used to be, which is a great deal better than losing the computers inside it.
        ComputerGroup* group = rebuilt.add_computer_group();
        if (!group->ParseFromString(entry->payload))
        {
            rebuilt.mutable_computer_group()->RemoveLast();
            ++skipped_count;
            continue;
        }

        group->set_guid(entry->guid);
        groups.emplace(entry->guid, group);
        ++skipped_count;
    }

    // Now the computers, into the groups that are all in place by this point.
    for (const auto& [guid, entry] : by_guid)
    {
        if (entry->kind != FlatEntry::Kind::COMPUTER)
            continue;

        auto parent = groups.find(entry->parent_guid);
        const bool orphan = (parent == groups.end());

        ComputerGroup* target = orphan ? &rebuilt : parent->second;

        Computer* computer = target->add_computer();
        if (!computer->ParseFromString(entry->payload))
        {
            target->mutable_computer()->RemoveLast();
            ++skipped_count;
            continue;
        }

        computer->set_guid(entry->guid);

        if (orphan)
            ++skipped_count;
    }

    *root = std::move(rebuilt);

    if (skipped)
        *skipped = skipped_count;

    return true;
}

} // namespace console
