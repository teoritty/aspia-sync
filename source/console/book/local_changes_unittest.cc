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

#include "console/book/entry_guid.h"
#include "console/book/flat_book.h"
#include "proto/address_book.pb.h"

#include <gtest/gtest.h>

#include <string>

namespace console {

namespace {

using proto::address_book::Computer;
using proto::address_book::ComputerGroup;
using proto::address_book::Data;

const char kBookGuid[] = "11111111-1111-4111-8111-111111111111";

// A book that is in step with the router: every record has a base, nothing is pending.
Data makeBook()
{
    Data data;

    ComputerGroup* root = data.mutable_root_group();
    root->set_name("book");

    ComputerGroup* office = root->add_computer_group();
    office->set_name("office");

    Computer* computer = office->add_computer();
    computer->set_name("server");
    computer->set_password("secret");

    ensureEntryGuids(root);

    proto::address_book::SyncState* sync = data.mutable_sync();
    sync->set_book_guid(kBookGuid);
    sync->set_last_pulled_revision(1);

    for (const FlatEntry& entry : flattenBook(*root))
    {
        proto::address_book::SyncEntryState* state = sync->add_entry();
        state->set_guid(entry.guid);
        state->set_revision(1);
        state->set_base_payload(entry.payload);
        state->set_base_parent_guid(entry.parent_guid);
        state->set_dirty(false);
    }

    return data;
}

const proto::address_book::SyncEntryState* stateOf(const Data& data, const std::string& guid)
{
    for (int i = 0; i < data.sync().entry_size(); ++i)
    {
        if (data.sync().entry(i).guid() == guid)
            return &data.sync().entry(i);
    }
    return nullptr;
}

ComputerGroup* office(Data* data)
{
    return data->mutable_root_group()->mutable_computer_group(0);
}

} // namespace

//--------------------------------------------------------------------------------------------------
TEST(local_changes_test, an_untouched_book_has_nothing_to_send)
{
    Data data = makeBook();

    const LocalChanges changes = markLocalChanges(&data);

    EXPECT_EQ(changes.total(), 0u);
}

//--------------------------------------------------------------------------------------------------
TEST(local_changes_test, an_edited_record_is_found)
{
    Data data = makeBook();
    const std::string guid = office(&data)->computer(0).guid();

    office(&data)->mutable_computer(0)->set_comment("called about the printer");

    const LocalChanges changes = markLocalChanges(&data);

    EXPECT_EQ(changes.changed, 1u);
    EXPECT_EQ(changes.added, 0u);
    EXPECT_EQ(changes.deleted, 0u);

    const proto::address_book::SyncEntryState* state = stateOf(data, guid);
    ASSERT_TRUE(state);
    EXPECT_TRUE(state->dirty());
}

//--------------------------------------------------------------------------------------------------
// The reason this is done by comparison rather than by hooking the places that edit: dragging a
// computer into another group changes nothing about the record itself.
TEST(local_changes_test, a_moved_record_is_found)
{
    Data data = makeBook();

    Computer moved = office(&data)->computer(0);
    office(&data)->mutable_computer()->Clear();
    data.mutable_root_group()->add_computer()->CopyFrom(moved);

    const LocalChanges changes = markLocalChanges(&data);

    EXPECT_EQ(changes.changed, 1u);
    EXPECT_EQ(changes.deleted, 0u);

    const proto::address_book::SyncEntryState* state = stateOf(data, moved.guid());
    ASSERT_TRUE(state);
    EXPECT_TRUE(state->dirty());
}

//--------------------------------------------------------------------------------------------------
TEST(local_changes_test, a_new_record_is_found)
{
    Data data = makeBook();

    Computer* added = office(&data)->add_computer();
    added->set_name("workstation");
    ensureEntryGuids(data.mutable_root_group());

    const LocalChanges changes = markLocalChanges(&data);

    EXPECT_EQ(changes.added, 1u);

    const proto::address_book::SyncEntryState* state = stateOf(data, added->guid());
    ASSERT_TRUE(state);
    EXPECT_TRUE(state->dirty());

    // No base at all is what tells the router this is a record it has never seen.
    EXPECT_TRUE(state->base_payload().empty());
    EXPECT_EQ(state->revision(), 0);
}

//--------------------------------------------------------------------------------------------------
TEST(local_changes_test, a_deleted_record_is_found)
{
    Data data = makeBook();
    const std::string guid = office(&data)->computer(0).guid();

    office(&data)->mutable_computer()->Clear();

    const LocalChanges changes = markLocalChanges(&data);

    EXPECT_EQ(changes.deleted, 1u);

    const proto::address_book::SyncEntryState* state = stateOf(data, guid);
    ASSERT_TRUE(state);
    EXPECT_TRUE(state->deleted());
}

//--------------------------------------------------------------------------------------------------
// Deleting a group takes everything under it, and every one of those has to be sent as gone: a
// console that was away would otherwise learn about the folder and keep the computers.
TEST(local_changes_test, deleting_a_group_marks_everything_in_it)
{
    Data data = makeBook();

    const std::string group_guid = office(&data)->guid();
    const std::string computer_guid = office(&data)->computer(0).guid();

    data.mutable_root_group()->mutable_computer_group()->Clear();

    const LocalChanges changes = markLocalChanges(&data);

    EXPECT_EQ(changes.deleted, 2u);
    EXPECT_TRUE(stateOf(data, group_guid)->deleted());
    EXPECT_TRUE(stateOf(data, computer_guid)->deleted());
}

//--------------------------------------------------------------------------------------------------
// Typed something, thought better of it, put it back. There is nothing to send, and sending it
// would make the router refuse somebody else's work for no reason at all.
TEST(local_changes_test, an_edit_undone_stops_being_pending)
{
    Data data = makeBook();
    const std::string guid = office(&data)->computer(0).guid();
    const std::string original = office(&data)->computer(0).comment();

    office(&data)->mutable_computer(0)->set_comment("a mistake");
    ASSERT_EQ(markLocalChanges(&data).changed, 1u);
    ASSERT_TRUE(stateOf(data, guid)->dirty());

    office(&data)->mutable_computer(0)->set_comment(original);

    const LocalChanges changes = markLocalChanges(&data);

    EXPECT_EQ(changes.total(), 0u);
    EXPECT_FALSE(stateOf(data, guid)->dirty());
}

//--------------------------------------------------------------------------------------------------
// Restored from the recently deleted before the deletion ever went out.
TEST(local_changes_test, a_restored_record_is_no_longer_a_deletion)
{
    Data data = makeBook();

    const Computer saved = office(&data)->computer(0);
    office(&data)->mutable_computer()->Clear();
    ASSERT_EQ(markLocalChanges(&data).deleted, 1u);

    office(&data)->add_computer()->CopyFrom(saved);

    const LocalChanges changes = markLocalChanges(&data);

    EXPECT_EQ(changes.deleted, 0u);
    EXPECT_FALSE(stateOf(data, saved.guid())->deleted());
}

//--------------------------------------------------------------------------------------------------
// A record that was created here and removed again before it ever reached the router is simply
// gone: there is nothing on the other side to delete.
TEST(local_changes_test, a_record_created_and_removed_before_sending_is_not_a_deletion)
{
    Data data = makeBook();

    Computer* added = office(&data)->add_computer();
    added->set_name("a mistake");
    ensureEntryGuids(data.mutable_root_group());
    const std::string guid = added->guid();

    ASSERT_EQ(markLocalChanges(&data).added, 1u);

    office(&data)->mutable_computer()->DeleteSubrange(1, 1);

    const LocalChanges changes = markLocalChanges(&data);

    EXPECT_EQ(changes.deleted, 0u);

    const proto::address_book::SyncEntryState* state = stateOf(data, guid);
    ASSERT_TRUE(state);
    EXPECT_FALSE(state->deleted());
}

//--------------------------------------------------------------------------------------------------
TEST(local_changes_test, a_book_that_is_not_synchronized_is_left_alone)
{
    Data data = makeBook();
    data.mutable_sync()->clear_book_guid();

    office(&data)->mutable_computer(0)->set_comment("changed");

    EXPECT_EQ(markLocalChanges(&data).total(), 0u);
}

//--------------------------------------------------------------------------------------------------
TEST(local_changes_test, tolerates_null)
{
    EXPECT_EQ(markLocalChanges(nullptr).total(), 0u);
}

} // namespace console
