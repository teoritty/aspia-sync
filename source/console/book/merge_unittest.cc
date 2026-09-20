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

#include "console/book/merge.h"

#include "proto/address_book.pb.h"

#include <gtest/gtest.h>

#include <algorithm>

namespace console {

namespace {

using proto::address_book::Computer;
using proto::address_book::ComputerGroup;

Computer makeComputer()
{
    Computer computer;
    computer.set_guid("11111111-1111-4111-8111-111111111111");
    computer.set_name("server");
    computer.set_comment("the one in the basement");
    computer.set_address("12345");
    computer.set_port(4899);
    computer.set_username("admin");
    computer.set_password("secret");
    return computer;
}

bool hasConflict(const MergeResult& result, const char* field)
{
    return std::find(result.conflicts.begin(), result.conflicts.end(), field) !=
           result.conflicts.end();
}

} // namespace

//--------------------------------------------------------------------------------------------------
TEST(merge_test, nothing_changed_anywhere)
{
    const Computer base = makeComputer();

    Computer out;
    const MergeResult result = mergeComputer(base, base, base, &out);

    EXPECT_TRUE(result.ok());
    EXPECT_EQ(out.name(), base.name());
    EXPECT_EQ(out.password(), base.password());
}

//--------------------------------------------------------------------------------------------------
TEST(merge_test, only_this_side_changed)
{
    const Computer base = makeComputer();

    Computer local = base;
    local.set_comment("moved to the first floor");

    Computer out;
    const MergeResult result = mergeComputer(base, local, base, &out);

    EXPECT_TRUE(result.ok());
    EXPECT_EQ(out.comment(), "moved to the first floor");
}

//--------------------------------------------------------------------------------------------------
TEST(merge_test, only_the_other_side_changed)
{
    const Computer base = makeComputer();

    Computer remote = base;
    remote.set_password("a new one");

    Computer out;
    const MergeResult result = mergeComputer(base, base, remote, &out);

    EXPECT_TRUE(result.ok());
    EXPECT_EQ(out.password(), "a new one");
}

//--------------------------------------------------------------------------------------------------
// The case the whole exercise is for: one person edits a comment while another changes a password,
// both survive, and neither of them is asked anything.
TEST(merge_test, different_fields_both_survive)
{
    const Computer base = makeComputer();

    Computer local = base;
    local.set_comment("called about the printer");

    Computer remote = base;
    remote.set_password("rotated on friday");

    Computer out;
    const MergeResult result = mergeComputer(base, local, remote, &out);

    EXPECT_TRUE(result.ok());
    EXPECT_EQ(out.comment(), "called about the printer");
    EXPECT_EQ(out.password(), "rotated on friday");
}

//--------------------------------------------------------------------------------------------------
// And the case that has to be put to a person, because guessing would throw away somebody's work.
TEST(merge_test, the_same_field_changed_differently_is_a_conflict)
{
    const Computer base = makeComputer();

    Computer local = base;
    local.set_password("what I set");

    Computer remote = base;
    remote.set_password("what they set");

    Computer out;
    const MergeResult result = mergeComputer(base, local, remote, &out);

    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(hasConflict(result, "password"));

    // Until the person decides, what they typed is what they keep seeing.
    EXPECT_EQ(out.password(), "what I set");
}

//--------------------------------------------------------------------------------------------------
TEST(merge_test, the_same_field_changed_to_the_same_value_is_not_a_conflict)
{
    const Computer base = makeComputer();

    Computer local = base;
    local.set_address("54321");

    Computer remote = base;
    remote.set_address("54321");

    Computer out;
    const MergeResult result = mergeComputer(base, local, remote, &out);

    EXPECT_TRUE(result.ok());
    EXPECT_EQ(out.address(), "54321");
}

//--------------------------------------------------------------------------------------------------
TEST(merge_test, several_conflicting_fields_are_all_reported)
{
    const Computer base = makeComputer();

    Computer local = base;
    local.set_name("mine");
    local.set_password("mine");

    Computer remote = base;
    remote.set_name("theirs");
    remote.set_password("theirs");

    Computer out;
    const MergeResult result = mergeComputer(base, local, remote, &out);

    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(hasConflict(result, "name"));
    EXPECT_TRUE(hasConflict(result, "password"));
}

//--------------------------------------------------------------------------------------------------
// The identity is what says these are three versions of one record, so it is never merged.
TEST(merge_test, the_identity_is_kept)
{
    const Computer base = makeComputer();

    Computer remote = base;
    remote.set_guid("22222222-2222-4222-8222-222222222222");

    Computer out;
    mergeComputer(base, base, remote, &out);

    EXPECT_EQ(out.guid(), base.guid());
}

//--------------------------------------------------------------------------------------------------
// When the record was made is a fact about the past: the earliest of the three is the true one,
// and there is nothing here for two people to disagree about.
TEST(merge_test, the_earliest_creation_time_wins)
{
    Computer base = makeComputer();
    base.set_create_time(500);

    Computer local = base;
    local.set_create_time(400);

    Computer remote = base;
    remote.set_create_time(600);

    Computer out;
    const MergeResult result = mergeComputer(base, local, remote, &out);

    EXPECT_TRUE(result.ok());
    EXPECT_EQ(out.create_time(), 400);
}

//--------------------------------------------------------------------------------------------------
TEST(merge_test, the_latest_modification_time_wins)
{
    Computer base = makeComputer();
    base.set_modify_time(500);

    Computer local = base;
    local.set_modify_time(700);

    Computer remote = base;
    remote.set_modify_time(600);

    Computer out;
    mergeComputer(base, local, remote, &out);

    EXPECT_EQ(out.modify_time(), 700);
}

//--------------------------------------------------------------------------------------------------
// Session settings are merged flag by flag rather than as a lump: two people may well have
// changed different settings of the same computer.
TEST(merge_test, session_settings_are_merged_one_at_a_time)
{
    Computer base = makeComputer();
    base.mutable_session_config()->mutable_desktop_manage()->set_flags(1);
    base.mutable_session_config()->mutable_desktop_manage()->set_compress_ratio(6);

    Computer local = base;
    local.mutable_session_config()->mutable_desktop_manage()->set_flags(3);

    Computer remote = base;
    remote.mutable_session_config()->mutable_desktop_manage()->set_compress_ratio(8);

    Computer out;
    const MergeResult result = mergeComputer(base, local, remote, &out);

    EXPECT_TRUE(result.ok());
    EXPECT_EQ(out.session_config().desktop_manage().flags(), 3u);
    EXPECT_EQ(out.session_config().desktop_manage().compress_ratio(), 8u);
}

//--------------------------------------------------------------------------------------------------
TEST(merge_test, inherit_flags_are_merged_one_at_a_time)
{
    Computer base = makeComputer();
    base.mutable_inherit()->set_credentials(false);
    base.mutable_inherit()->set_desktop_manage(false);

    Computer local = base;
    local.mutable_inherit()->set_credentials(true);

    Computer remote = base;
    remote.mutable_inherit()->set_desktop_manage(true);

    Computer out;
    const MergeResult result = mergeComputer(base, local, remote, &out);

    EXPECT_TRUE(result.ok());
    EXPECT_TRUE(out.inherit().credentials());
    EXPECT_TRUE(out.inherit().desktop_manage());
}

//--------------------------------------------------------------------------------------------------
TEST(merge_test, groups_merge_their_own_fields)
{
    ComputerGroup base;
    base.set_guid("33333333-3333-4333-8333-333333333333");
    base.set_name("office");
    base.set_comment("the one downstairs");

    ComputerGroup local = base;
    local.set_name("main office");

    ComputerGroup remote = base;
    remote.set_comment("moved in march");

    ComputerGroup out;
    const MergeResult result = mergeComputerGroup(base, local, remote, &out);

    EXPECT_TRUE(result.ok());
    EXPECT_EQ(out.name(), "main office");
    EXPECT_EQ(out.comment(), "moved in march");
}

//--------------------------------------------------------------------------------------------------
// Credentials set on a group are merged like everything else: they are what the computers inside
// inherit, so losing them would take the whole branch offline.
TEST(merge_test, group_credentials_are_merged)
{
    ComputerGroup base;
    base.set_guid("33333333-3333-4333-8333-333333333333");
    base.mutable_config()->set_username("admin");
    base.mutable_config()->set_password("old");

    ComputerGroup local = base;
    local.mutable_config()->set_username("operator");

    ComputerGroup remote = base;
    remote.mutable_config()->set_password("new");

    ComputerGroup out;
    const MergeResult result = mergeComputerGroup(base, local, remote, &out);

    EXPECT_TRUE(result.ok());
    EXPECT_EQ(out.config().username(), "operator");
    EXPECT_EQ(out.config().password(), "new");
}

//--------------------------------------------------------------------------------------------------
// Whether a folder is open belongs to this window and never leaves it, so the remote value is not
// even looked at.
TEST(merge_test, expanded_stays_local)
{
    ComputerGroup base;
    base.set_guid("33333333-3333-4333-8333-333333333333");
    base.set_expanded(false);

    ComputerGroup local = base;
    local.set_expanded(true);

    ComputerGroup remote = base;
    remote.set_expanded(false);

    ComputerGroup out;
    const MergeResult result = mergeComputerGroup(base, local, remote, &out);

    EXPECT_TRUE(result.ok());
    EXPECT_TRUE(out.expanded());
}

//--------------------------------------------------------------------------------------------------
// The children of a group are records of their own. A merge that carried them would put a stale
// copy of the whole branch back into the book.
TEST(merge_test, group_children_are_not_carried)
{
    ComputerGroup base;
    base.set_guid("33333333-3333-4333-8333-333333333333");

    ComputerGroup local = base;
    local.add_computer()->set_name("inside");
    local.add_computer_group()->set_name("nested");

    ComputerGroup out;
    mergeComputerGroup(base, local, base, &out);

    EXPECT_EQ(out.computer_size(), 0);
    EXPECT_EQ(out.computer_group_size(), 0);
}

//--------------------------------------------------------------------------------------------------
TEST(merge_test, tolerates_null_output)
{
    const Computer base = makeComputer();

    EXPECT_TRUE(mergeComputer(base, base, base, nullptr).ok());

    ComputerGroup group;
    EXPECT_TRUE(mergeComputerGroup(group, group, group, nullptr).ok());
}

} // namespace console
