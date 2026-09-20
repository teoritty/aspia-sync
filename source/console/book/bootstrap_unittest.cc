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

#include "console/book/entry_guid.h"
#include "proto/address_book.pb.h"

#include <gtest/gtest.h>

#include <algorithm>

namespace console {

namespace {

using proto::address_book::Computer;
using proto::address_book::ComputerGroup;

Computer* add(ComputerGroup* group, const char* name, const char* address, uint32_t port = 0)
{
    Computer* computer = group->add_computer();
    computer->set_name(name);
    computer->set_address(address);
    computer->set_port(port);
    return computer;
}

bool listed(const std::vector<std::string>& list, const std::string& guid)
{
    return std::find(list.begin(), list.end(), guid) != list.end();
}

} // namespace

//--------------------------------------------------------------------------------------------------
// The case the department is actually in: everybody has the same machines under ids issued by the
// router, but the guids were handed out separately on each console.
TEST(bootstrap_test, the_same_machine_is_recognized_by_its_id)
{
    ComputerGroup local;
    add(&local, "server", "12345");
    ensureEntryGuids(&local);

    ComputerGroup remote;
    add(&remote, "the server in the basement", "12345");
    ensureEntryGuids(&remote);

    const BootstrapPlan plan = planBootstrap(local, remote);

    ASSERT_EQ(plan.matched.size(), 1u);
    EXPECT_EQ(plan.matched.front().local_guid, local.computer(0).guid());
    EXPECT_EQ(plan.matched.front().remote_guid, remote.computer(0).guid());
    EXPECT_TRUE(plan.only_local.empty());
}

//--------------------------------------------------------------------------------------------------
// Keeping one name means losing the other, so it is pointed out. The rest can be confirmed
// without reading.
TEST(bootstrap_test, a_difference_in_names_is_pointed_out)
{
    ComputerGroup local;
    add(&local, "server", "12345");
    add(&local, "reception", "54321");
    ensureEntryGuids(&local);

    ComputerGroup remote;
    add(&remote, "the server in the basement", "12345");
    add(&remote, "reception", "54321");
    ensureEntryGuids(&remote);

    const BootstrapPlan plan = planBootstrap(local, remote);

    ASSERT_EQ(plan.matched.size(), 2u);

    size_t differing = 0;
    for (const BootstrapPair& pair : plan.matched)
    {
        if (pair.names_differ)
            ++differing;
    }

    EXPECT_EQ(differing, 1u);
}

//--------------------------------------------------------------------------------------------------
TEST(bootstrap_test, what_only_this_machine_has_is_contributed)
{
    ComputerGroup local;
    add(&local, "server", "12345");
    add(&local, "only mine", "99999");
    ensureEntryGuids(&local);

    ComputerGroup remote;
    add(&remote, "server", "12345");
    ensureEntryGuids(&remote);

    const BootstrapPlan plan = planBootstrap(local, remote);

    EXPECT_EQ(plan.matched.size(), 1u);
    ASSERT_EQ(plan.only_local.size(), 1u);
    EXPECT_EQ(plan.only_local.front(), local.computer(1).guid());
}

//--------------------------------------------------------------------------------------------------
// Records entered by address are matched the same way, and the port is part of what they are
// reached by: two machines behind one address on different ports are two machines.
TEST(bootstrap_test, records_entered_by_address_match_on_address_and_port)
{
    ComputerGroup local;
    add(&local, "first", "192.168.1.10", 4899);
    add(&local, "second", "192.168.1.10", 4900);
    ensureEntryGuids(&local);

    ComputerGroup remote;
    add(&remote, "the first one", "192.168.1.10", 4899);
    ensureEntryGuids(&remote);

    const BootstrapPlan plan = planBootstrap(local, remote);

    ASSERT_EQ(plan.matched.size(), 1u);
    EXPECT_EQ(plan.matched.front().local_guid, local.computer(0).guid());
    ASSERT_EQ(plan.only_local.size(), 1u);
    EXPECT_EQ(plan.only_local.front(), local.computer(1).guid());
}

//--------------------------------------------------------------------------------------------------
// The same id twice in one book is allowed - the same machine under two session types - and there
// is then no telling which of the two is meant. Guessing would pair the wrong ones, so neither is
// paired.
TEST(bootstrap_test, a_repeated_address_is_set_aside_rather_than_guessed)
{
    ComputerGroup local;
    add(&local, "manage", "12345");
    add(&local, "view", "12345");
    ensureEntryGuids(&local);

    ComputerGroup remote;
    add(&remote, "server", "12345");
    ensureEntryGuids(&remote);

    const BootstrapPlan plan = planBootstrap(local, remote);

    EXPECT_TRUE(plan.matched.empty());
    EXPECT_EQ(plan.ambiguous.size(), 2u);
    EXPECT_TRUE(listed(plan.ambiguous, local.computer(0).guid()));
    EXPECT_TRUE(listed(plan.ambiguous, local.computer(1).guid()));
}

//--------------------------------------------------------------------------------------------------
TEST(bootstrap_test, a_repeat_on_the_shared_side_is_set_aside_too)
{
    ComputerGroup local;
    add(&local, "server", "12345");
    ensureEntryGuids(&local);

    ComputerGroup remote;
    add(&remote, "manage", "12345");
    add(&remote, "view", "12345");
    ensureEntryGuids(&remote);

    const BootstrapPlan plan = planBootstrap(local, remote);

    EXPECT_TRUE(plan.matched.empty());
    ASSERT_EQ(plan.ambiguous.size(), 1u);
    EXPECT_EQ(plan.ambiguous.front(), local.computer(0).guid());
}

//--------------------------------------------------------------------------------------------------
// Matching walks the whole tree: the same machine may well sit in a differently named folder on
// each console, and where it sits says nothing about what it is.
TEST(bootstrap_test, matching_ignores_where_a_record_sits)
{
    ComputerGroup local;
    ComputerGroup* office = local.add_computer_group();
    office->set_name("office");
    ComputerGroup* basement = office->add_computer_group();
    basement->set_name("basement");
    add(basement, "server", "12345");
    ensureEntryGuids(&local);

    ComputerGroup remote;
    add(&remote, "server", "12345");
    ensureEntryGuids(&remote);

    const BootstrapPlan plan = planBootstrap(local, remote);

    EXPECT_EQ(plan.matched.size(), 1u);
}

//--------------------------------------------------------------------------------------------------
// A record with nothing to be reached by cannot be matched against anything, so it is contributed
// as it is rather than paired with whatever happens to be nearby.
TEST(bootstrap_test, a_record_without_an_address_is_contributed)
{
    ComputerGroup local;
    add(&local, "not filled in yet", "");
    ensureEntryGuids(&local);

    ComputerGroup remote;
    add(&remote, "also not filled in", "");
    ensureEntryGuids(&remote);

    const BootstrapPlan plan = planBootstrap(local, remote);

    EXPECT_TRUE(plan.matched.empty());
    EXPECT_EQ(plan.only_local.size(), 1u);
}

//--------------------------------------------------------------------------------------------------
TEST(bootstrap_test, an_empty_shared_book_takes_everything)
{
    ComputerGroup local;
    add(&local, "one", "1");
    add(&local, "two", "2");
    ensureEntryGuids(&local);

    const BootstrapPlan plan = planBootstrap(local, ComputerGroup());

    EXPECT_TRUE(plan.matched.empty());
    EXPECT_EQ(plan.only_local.size(), 2u);
}

//--------------------------------------------------------------------------------------------------
TEST(bootstrap_test, nothing_local_contributes_nothing)
{
    ComputerGroup remote;
    add(&remote, "one", "1");
    ensureEntryGuids(&remote);

    const BootstrapPlan plan = planBootstrap(ComputerGroup(), remote);

    EXPECT_TRUE(plan.matched.empty());
    EXPECT_TRUE(plan.only_local.empty());
}

//--------------------------------------------------------------------------------------------------
// An id and an address that happen to look alike are not the same thing, and must not be paired.
TEST(bootstrap_test, an_id_is_not_an_address)
{
    ComputerGroup local;
    add(&local, "by id", "12345");
    ensureEntryGuids(&local);

    ComputerGroup remote;
    add(&remote, "by address", "12345.example.local", 12345);
    ensureEntryGuids(&remote);

    const BootstrapPlan plan = planBootstrap(local, remote);

    EXPECT_TRUE(plan.matched.empty());
    EXPECT_EQ(plan.only_local.size(), 1u);
}

} // namespace console
