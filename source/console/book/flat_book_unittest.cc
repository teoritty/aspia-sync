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

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <string>

namespace console {

namespace {

using proto::address_book::Computer;
using proto::address_book::ComputerGroup;

Computer* addComputer(ComputerGroup* parent, const char* name, const char* address = "")
{
    Computer* computer = parent->add_computer();
    computer->set_name(name);
    computer->set_address(address);
    return computer;
}

ComputerGroup* addGroup(ComputerGroup* parent, const char* name)
{
    ComputerGroup* group = parent->add_computer_group();
    group->set_name(name);
    return group;
}

// "office/basement/server" for every computer, so a tree can be compared with another one without
// depending on the order the records happen to sit in.
void collectPaths(const ComputerGroup& group, const std::string& prefix,
                  std::vector<std::string>* out)
{
    const std::string path = prefix.empty() ? group.name() : prefix + "/" + group.name();

    for (int i = 0; i < group.computer_size(); ++i)
        out->emplace_back(path + "/" + group.computer(i).name());

    for (int i = 0; i < group.computer_group_size(); ++i)
        collectPaths(group.computer_group(i), path, out);
}

std::vector<std::string> paths(const ComputerGroup& root)
{
    std::vector<std::string> result;
    collectPaths(root, std::string(), &result);
    std::sort(result.begin(), result.end());
    return result;
}

const FlatEntry* find(const std::vector<FlatEntry>& entries, const std::string& guid)
{
    for (const FlatEntry& entry : entries)
    {
        if (entry.guid == guid)
            return &entry;
    }
    return nullptr;
}

// A tree of three levels with computers on each of them.
ComputerGroup makeBook()
{
    ComputerGroup root;
    root.set_name("book");
    addComputer(&root, "at the root", "100");

    ComputerGroup* office = addGroup(&root, "office");
    addComputer(office, "reception", "200");

    ComputerGroup* basement = addGroup(office, "basement");
    addComputer(basement, "server", "300");
    addComputer(basement, "backup", "400");

    addGroup(&root, "empty");

    ensureEntryGuids(&root);
    return root;
}

} // namespace

//--------------------------------------------------------------------------------------------------
TEST(flat_book_test, flatten_yields_every_record)
{
    const ComputerGroup root = makeBook();

    const std::vector<FlatEntry> entries = flattenBook(root);

    // Root, 3 groups, 4 computers.
    EXPECT_EQ(entries.size(), 8u);

    size_t groups = 0;
    size_t computers = 0;
    for (const FlatEntry& entry : entries)
    {
        if (entry.kind == FlatEntry::Kind::GROUP)
            ++groups;
        else
            ++computers;
    }

    EXPECT_EQ(groups, 4u);
    EXPECT_EQ(computers, 4u);
}

//--------------------------------------------------------------------------------------------------
TEST(flat_book_test, only_the_root_has_no_parent)
{
    const ComputerGroup root = makeBook();

    const std::vector<FlatEntry> entries = flattenBook(root);

    size_t without_parent = 0;
    for (const FlatEntry& entry : entries)
    {
        if (entry.parent_guid.empty())
            ++without_parent;
    }

    EXPECT_EQ(without_parent, 1u);
    ASSERT_TRUE(find(entries, root.guid()));
    EXPECT_TRUE(find(entries, root.guid())->parent_guid.empty());
}

//--------------------------------------------------------------------------------------------------
// The shape of the tree has to survive the trip, to any depth.
TEST(flat_book_test, round_trip_keeps_the_tree)
{
    const ComputerGroup root = makeBook();

    ComputerGroup rebuilt;
    size_t skipped = 1;
    ASSERT_TRUE(rebuildBook(flattenBook(root), &rebuilt, &skipped));

    EXPECT_EQ(skipped, 0u);
    EXPECT_EQ(paths(rebuilt), paths(root));
    EXPECT_EQ(rebuilt.guid(), root.guid());
}

//--------------------------------------------------------------------------------------------------
TEST(flat_book_test, round_trip_keeps_the_fields_of_a_record)
{
    ComputerGroup root;
    root.set_name("book");

    Computer* computer = addComputer(&root, "server", "12345");
    computer->set_comment("the one in the basement");
    computer->set_port(4899);
    computer->set_username("admin");
    computer->set_password("secret");

    ensureEntryGuids(&root);

    ComputerGroup rebuilt;
    ASSERT_TRUE(rebuildBook(flattenBook(root), &rebuilt, nullptr));

    ASSERT_EQ(rebuilt.computer_size(), 1);
    const Computer& result = rebuilt.computer(0);

    EXPECT_EQ(result.name(), "server");
    EXPECT_EQ(result.address(), "12345");
    EXPECT_EQ(result.comment(), "the one in the basement");
    EXPECT_EQ(result.port(), 4899u);
    EXPECT_EQ(result.username(), "admin");
    EXPECT_EQ(result.password(), "secret");
    EXPECT_EQ(result.guid(), computer->guid());
}

//--------------------------------------------------------------------------------------------------
// Which folders a person has open is a state of that person's window. Carrying it would make the
// tree fold and unfold by itself whenever a colleague clicks something.
TEST(flat_book_test, expanded_is_not_carried)
{
    ComputerGroup root;
    root.set_name("book");
    root.set_expanded(true);

    ComputerGroup* office = addGroup(&root, "office");
    office->set_expanded(true);

    ensureEntryGuids(&root);

    ComputerGroup rebuilt;
    ASSERT_TRUE(rebuildBook(flattenBook(root), &rebuilt, nullptr));

    EXPECT_FALSE(rebuilt.expanded());
    ASSERT_EQ(rebuilt.computer_group_size(), 1);
    EXPECT_FALSE(rebuilt.computer_group(0).expanded());
}

//--------------------------------------------------------------------------------------------------
// The payload of a group must not carry what belongs to the records under it, or a group would
// bring a stale copy of its whole branch along every time it is sent.
TEST(flat_book_test, group_payload_holds_no_children)
{
    const ComputerGroup root = makeBook();

    for (const FlatEntry& entry : flattenBook(root))
    {
        if (entry.kind != FlatEntry::Kind::GROUP)
            continue;

        ComputerGroup group;
        ASSERT_TRUE(group.ParseFromString(entry.payload));
        EXPECT_EQ(group.computer_size(), 0) << "group: " << group.name();
        EXPECT_EQ(group.computer_group_size(), 0) << "group: " << group.name();
    }
}

//--------------------------------------------------------------------------------------------------
// Moving a group is a change of one field in one record. This is the whole reason for the flat
// form, so it is checked rather than assumed.
TEST(flat_book_test, moving_a_group_changes_one_record)
{
    const ComputerGroup root = makeBook();
    std::vector<FlatEntry> before = flattenBook(root);

    // "basement" moves from "office" to the root.
    std::string basement_guid;
    for (const FlatEntry& entry : before)
    {
        ComputerGroup group;
        if (entry.kind == FlatEntry::Kind::GROUP && group.ParseFromString(entry.payload) &&
            group.name() == "basement")
        {
            basement_guid = entry.guid;
        }
    }
    ASSERT_FALSE(basement_guid.empty());

    std::vector<FlatEntry> after = before;
    for (FlatEntry& entry : after)
    {
        if (entry.guid == basement_guid)
            entry.parent_guid = root.guid();
    }

    size_t differences = 0;
    for (size_t i = 0; i < before.size(); ++i)
    {
        if (before[i].parent_guid != after[i].parent_guid ||
            before[i].payload != after[i].payload)
        {
            ++differences;
        }
    }
    EXPECT_EQ(differences, 1u);

    ComputerGroup rebuilt;
    ASSERT_TRUE(rebuildBook(after, &rebuilt, nullptr));

    const std::vector<std::string> result = paths(rebuilt);
    EXPECT_NE(std::find(result.begin(), result.end(), "book/basement/server"), result.end());
    EXPECT_EQ(std::find(result.begin(), result.end(), "book/office/basement/server"),
              result.end());
}

//--------------------------------------------------------------------------------------------------
// The order the records arrive in is not something the sender has to get right.
TEST(flat_book_test, order_of_entries_does_not_matter)
{
    const ComputerGroup root = makeBook();

    std::vector<FlatEntry> entries = flattenBook(root);
    std::reverse(entries.begin(), entries.end());

    ComputerGroup rebuilt;
    size_t skipped = 1;
    ASSERT_TRUE(rebuildBook(entries, &rebuilt, &skipped));

    EXPECT_EQ(skipped, 0u);
    EXPECT_EQ(paths(rebuilt), paths(root));
}

//--------------------------------------------------------------------------------------------------
// A computer whose group did not arrive is kept at the root. Losing a machine because its folder
// is missing is not an acceptable outcome.
TEST(flat_book_test, computer_without_a_parent_goes_to_the_root)
{
    const ComputerGroup root = makeBook();

    std::vector<FlatEntry> entries = flattenBook(root);

    // Drop the "basement" group but keep the computers that named it.
    std::string basement_guid;
    for (const FlatEntry& entry : entries)
    {
        ComputerGroup group;
        if (entry.kind == FlatEntry::Kind::GROUP && group.ParseFromString(entry.payload) &&
            group.name() == "basement")
        {
            basement_guid = entry.guid;
        }
    }
    ASSERT_FALSE(basement_guid.empty());

    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&](const FlatEntry& entry) { return entry.guid == basement_guid; }),
                  entries.end());

    ComputerGroup rebuilt;
    size_t skipped = 0;
    ASSERT_TRUE(rebuildBook(entries, &rebuilt, &skipped));

    EXPECT_EQ(skipped, 2u); // "server" and "backup".

    const std::vector<std::string> result = paths(rebuilt);
    EXPECT_NE(std::find(result.begin(), result.end(), "book/server"), result.end());
    EXPECT_NE(std::find(result.begin(), result.end(), "book/backup"), result.end());
}

//--------------------------------------------------------------------------------------------------
// Two people, both offline: one moves A into B, the other moves B into A. Neither did anything
// wrong, and together they detach a branch from the root. It must not hang the rebuild and must
// not lose what is in those groups.
TEST(flat_book_test, cycle_does_not_lose_anything)
{
    ComputerGroup root;
    root.set_name("book");
    ComputerGroup* a = addGroup(&root, "a");
    ComputerGroup* b = addGroup(&root, "b");
    addComputer(a, "inside a");
    addComputer(b, "inside b");
    ensureEntryGuids(&root);

    std::vector<FlatEntry> entries = flattenBook(root);

    const std::string a_guid = a->guid();
    const std::string b_guid = b->guid();

    for (FlatEntry& entry : entries)
    {
        if (entry.guid == a_guid)
            entry.parent_guid = b_guid;
        else if (entry.guid == b_guid)
            entry.parent_guid = a_guid;
    }

    ComputerGroup rebuilt;
    size_t skipped = 0;
    ASSERT_TRUE(rebuildBook(entries, &rebuilt, &skipped));

    EXPECT_EQ(skipped, 2u); // Both groups were placed at the root instead of where they claimed.

    const std::vector<std::string> result = paths(rebuilt);
    EXPECT_EQ(result.size(), 2u);
    EXPECT_NE(std::find(result.begin(), result.end(), "book/a/inside a"), result.end());
    EXPECT_NE(std::find(result.begin(), result.end(), "book/b/inside b"), result.end());
}

//--------------------------------------------------------------------------------------------------
TEST(flat_book_test, repeated_guid_is_taken_once)
{
    const ComputerGroup root = makeBook();

    std::vector<FlatEntry> entries = flattenBook(root);
    entries.push_back(entries.back());

    ComputerGroup rebuilt;
    size_t skipped = 0;
    ASSERT_TRUE(rebuildBook(entries, &rebuilt, &skipped));

    EXPECT_EQ(skipped, 1u);
    EXPECT_EQ(paths(rebuilt), paths(root));
}

//--------------------------------------------------------------------------------------------------
// One damaged record must not cost the whole book.
TEST(flat_book_test, damaged_payload_is_skipped)
{
    const ComputerGroup root = makeBook();

    std::vector<FlatEntry> entries = flattenBook(root);
    for (FlatEntry& entry : entries)
    {
        if (entry.kind == FlatEntry::Kind::COMPUTER)
        {
            entry.payload = std::string("\xff\xff\xff\xff", 4);
            break;
        }
    }

    ComputerGroup rebuilt;
    size_t skipped = 0;
    ASSERT_TRUE(rebuildBook(entries, &rebuilt, &skipped));

    EXPECT_EQ(skipped, 1u);
    EXPECT_EQ(paths(rebuilt).size(), paths(root).size() - 1);
}

//--------------------------------------------------------------------------------------------------
TEST(flat_book_test, refuses_a_list_without_a_root)
{
    const ComputerGroup root = makeBook();

    std::vector<FlatEntry> entries = flattenBook(root);
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [](const FlatEntry& entry) { return entry.parent_guid.empty(); }),
                  entries.end());

    ComputerGroup rebuilt;
    rebuilt.set_name("untouched");

    EXPECT_FALSE(rebuildBook(entries, &rebuilt, nullptr));
    EXPECT_EQ(rebuilt.name(), "untouched");
}

//--------------------------------------------------------------------------------------------------
TEST(flat_book_test, refuses_an_empty_list)
{
    ComputerGroup rebuilt;
    EXPECT_FALSE(rebuildBook(std::vector<FlatEntry>(), &rebuilt, nullptr));
}

//--------------------------------------------------------------------------------------------------
TEST(flat_book_test, tolerates_null)
{
    const ComputerGroup root = makeBook();
    EXPECT_FALSE(rebuildBook(flattenBook(root), nullptr, nullptr));
}

//--------------------------------------------------------------------------------------------------
// A record with no identity cannot be put back where it belongs, so it is not sent at all.
TEST(flat_book_test, records_without_a_guid_are_not_flattened)
{
    ComputerGroup root;
    root.set_name("book");
    ensureEntryGuids(&root);

    addComputer(&root, "no identity yet");

    const std::vector<FlatEntry> entries = flattenBook(root);
    EXPECT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries.front().guid, root.guid());
}

} // namespace console
