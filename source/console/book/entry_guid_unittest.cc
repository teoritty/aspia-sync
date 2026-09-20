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

#include "proto/address_book.pb.h"

#include <gtest/gtest.h>

#include <set>
#include <string>

namespace console {

namespace {

using proto::address_book::Computer;
using proto::address_book::ComputerGroup;

Computer* addComputer(ComputerGroup* parent, const char* name)
{
    Computer* computer = parent->add_computer();
    computer->set_name(name);
    return computer;
}

ComputerGroup* addGroup(ComputerGroup* parent, const char* name)
{
    ComputerGroup* group = parent->add_computer_group();
    group->set_name(name);
    return group;
}

// Collects every guid in the tree, the root included.
void collectGuids(const ComputerGroup& group, std::vector<std::string>* out)
{
    out->emplace_back(group.guid());

    for (int i = 0; i < group.computer_size(); ++i)
        out->emplace_back(group.computer(i).guid());

    for (int i = 0; i < group.computer_group_size(); ++i)
        collectGuids(group.computer_group(i), out);
}

} // namespace

//--------------------------------------------------------------------------------------------------
TEST(entry_guid_test, created_guid_is_valid)
{
    EXPECT_TRUE(isValidEntryGuid(createEntryGuid()));
}

//--------------------------------------------------------------------------------------------------
TEST(entry_guid_test, created_guids_differ)
{
    std::set<std::string> guids;
    for (int i = 0; i < 100; ++i)
        guids.insert(createEntryGuid());

    EXPECT_EQ(guids.size(), 100u);
}

//--------------------------------------------------------------------------------------------------
TEST(entry_guid_test, rejects_malformed_guids)
{
    EXPECT_FALSE(isValidEntryGuid(std::string()));
    EXPECT_FALSE(isValidEntryGuid("not-a-guid"));
    EXPECT_FALSE(isValidEntryGuid("00000000-0000-0000-0000-000000000000"));
    EXPECT_FALSE(isValidEntryGuid("7f3d9a1e2b4c4d5e8f90a1b2c3d4e5f6"));
}

//--------------------------------------------------------------------------------------------------
// A book written by an older version carries no guids at all. Opening it has to give one to every
// record, the root group included.
TEST(entry_guid_test, fills_whole_tree)
{
    ComputerGroup root;
    addComputer(&root, "at the root");

    ComputerGroup* office = addGroup(&root, "office");
    addComputer(office, "reception");

    ComputerGroup* basement = addGroup(office, "basement");
    addComputer(basement, "server");
    addComputer(basement, "backup");

    // Root, 2 groups, 4 computers.
    EXPECT_EQ(ensureEntryGuids(&root), 7u);

    std::vector<std::string> guids;
    collectGuids(root, &guids);

    ASSERT_EQ(guids.size(), 7u);
    for (const std::string& guid : guids)
        EXPECT_TRUE(isValidEntryGuid(guid)) << "guid: " << guid;

    EXPECT_EQ(std::set<std::string>(guids.begin(), guids.end()).size(), 7u);
}

//--------------------------------------------------------------------------------------------------
// The one property that must never break: a book opened twice keeps the guids of the first open.
// A guid that changed between two opens would make every record look new to everybody else.
TEST(entry_guid_test, keeps_existing_guids)
{
    ComputerGroup root;
    ComputerGroup* office = addGroup(&root, "office");
    addComputer(office, "reception");

    ASSERT_EQ(ensureEntryGuids(&root), 3u);

    std::vector<std::string> before;
    collectGuids(root, &before);

    EXPECT_EQ(ensureEntryGuids(&root), 0u);

    std::vector<std::string> after;
    collectGuids(root, &after);

    EXPECT_EQ(before, after);
}

//--------------------------------------------------------------------------------------------------
// A book half-way through: some records were written by a version that knew about guids, some
// were not. Only the ones without get a new one.
TEST(entry_guid_test, fills_only_what_is_missing)
{
    ComputerGroup root;
    root.set_guid(createEntryGuid());

    Computer* known = addComputer(&root, "known");
    known->set_guid(createEntryGuid());

    addComputer(&root, "unknown");

    const std::string root_guid = root.guid();
    const std::string known_guid = known->guid();

    EXPECT_EQ(ensureEntryGuids(&root), 1u);

    EXPECT_EQ(root.guid(), root_guid);
    EXPECT_EQ(root.computer(0).guid(), known_guid);
    EXPECT_TRUE(isValidEntryGuid(root.computer(1).guid()));
}

//--------------------------------------------------------------------------------------------------
// A guid that is not one is replaced rather than kept: matching other records against a value
// nothing produced would pair up entries that have nothing to do with each other.
TEST(entry_guid_test, replaces_malformed_guid)
{
    ComputerGroup root;
    Computer* computer = addComputer(&root, "damaged");
    computer->set_guid("certainly not a guid");

    EXPECT_EQ(ensureEntryGuids(&root), 2u);
    EXPECT_TRUE(isValidEntryGuid(root.computer(0).guid()));
}

//--------------------------------------------------------------------------------------------------
TEST(entry_guid_test, handles_empty_book)
{
    ComputerGroup root;

    EXPECT_EQ(ensureEntryGuids(&root), 1u);
    EXPECT_TRUE(isValidEntryGuid(root.guid()));
}

//--------------------------------------------------------------------------------------------------
TEST(entry_guid_test, tolerates_null)
{
    EXPECT_EQ(ensureEntryGuids(nullptr), 0u);
}

} // namespace console
