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

#include "console/book/book_history.h"

#include "console/book/local_changes.h"
#include "console/book/sync_key.h"
#include "proto/address_book.pb.h"
#include "proto/router_book.pb.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

namespace console {

namespace {

using proto::address_book::Computer;
using proto::address_book::ComputerGroup;
using proto::address_book::Data;

const char kBook[] = "11111111-1111-4111-8111-111111111111";
const char kServer[] = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
const char kLaptop[] = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
const char kGroup[] = "cccccccc-cccc-4ccc-8ccc-cccccccccccc";

std::string computer(const std::string& guid, const std::string& name,
                     const std::string& address, const std::string& password = std::string())
{
    Computer value;
    value.set_guid(guid);
    value.set_name(name);
    value.set_address(address);
    value.set_password(password);
    return value.SerializeAsString();
}

std::string group(const std::string& guid, const std::string& name)
{
    ComputerGroup value;
    value.set_guid(guid);
    value.set_name(name);
    return value.SerializeAsString();
}

class BookHistoryTest : public testing::Test
{
public:
    void SetUp() override
    {
        key_ = deriveSyncKey("passphrase", createSyncSalt());
        ASSERT_FALSE(key_.empty());
    }

protected:
    std::string seal(const std::string& payload) const
    {
        std::string sealed;
        EXPECT_TRUE(sealPayload(key_, payload, &sealed));
        return sealed;
    }

    // One record in one batch. An empty |before| means the batch created it; |deleted_after| that
    // the batch deleted it.
    proto::BookHistoryBatch* batch(proto::BookHistory* page, int64_t revision,
                                   const std::string& who = "PC-VANYA")
    {
        proto::BookHistoryBatch* batch = page->add_batch();
        batch->set_revision(revision);
        batch->set_server_time(1000 + revision);
        batch->set_modified_by(who);
        return batch;
    }

    void change(proto::BookHistoryBatch* batch, const std::string& guid,
                proto::BookEntryKind kind,
                const std::string& before, int64_t before_revision, bool before_deleted,
                const std::string& after, bool after_deleted,
                const std::string& before_parent = std::string(),
                const std::string& after_parent = std::string(),
                bool has_before = true)
    {
        proto::BookHistoryChange* item = batch->add_change();
        item->set_guid(guid);
        item->set_kind(kind);

        if (has_before)
        {
            proto::BookEntryData* data = item->mutable_before();
            data->set_guid(guid);
            data->set_kind(kind);
            data->set_revision(before_revision);
            data->set_deleted(before_deleted);
            data->set_parent_guid(before_parent);
            if (!before.empty())
                data->set_payload(seal(before));
        }

        proto::BookEntryData* data = item->mutable_after();
        data->set_guid(guid);
        data->set_kind(kind);
        data->set_revision(batch->revision());
        data->set_deleted(after_deleted);
        data->set_parent_guid(after_parent);
        if (!after.empty())
            data->set_payload(seal(after));
    }

    void created(proto::BookHistoryBatch* batch, const std::string& guid,
                 proto::BookEntryKind kind, const std::string& after,
                 const std::string& parent = std::string())
    {
        change(batch, guid, kind, std::string(), 0, false, after, false, std::string(), parent,
               false);
    }

    static proto::BookHistory page(int64_t revision, int64_t oldest, bool has_more = false)
    {
        proto::BookHistory page;
        page.set_error_code(proto::BOOK_ERROR_CODE_OK);
        page.set_book_guid(kBook);
        page.set_revision(revision);
        page.set_oldest_revision(oldest);
        page.set_has_more(has_more);
        page.set_history_days(30);
        return page;
    }

    std::string key_;
};

const RollbackTarget* findTarget(const RollbackPlan& plan, const std::string& guid)
{
    for (const RollbackTarget& target : plan.targets)
    {
        if (target.guid == guid)
            return &target;
    }
    return nullptr;
}

} // namespace

//--------------------------------------------------------------------------------------------------
TEST_F(BookHistoryTest, a_page_is_opened_with_who_and_when)
{
    proto::BookHistory history = page(1, 0);
    created(batch(&history, 1, "PC-PETYA"), kServer, proto::BOOK_ENTRY_KIND_COMPUTER,
            computer(kServer, "server", "12345"));
    history.mutable_batch(0)->set_address("10.0.0.5");

    const std::vector<HistoryBatch> batches = openHistory(history, key_);
    ASSERT_EQ(batches.size(), 1u);
    EXPECT_EQ(batches[0].modified_by, "PC-PETYA");
    EXPECT_EQ(batches[0].address, "10.0.0.5");
    EXPECT_EQ(batches[0].server_time, 1001);

    ASSERT_EQ(batches[0].changes.size(), 1u);
    EXPECT_FALSE(batches[0].changes[0].before.exists);
    EXPECT_TRUE(batches[0].changes[0].after.alive());
    EXPECT_TRUE(batches[0].changes[0].after.readable);
}

//--------------------------------------------------------------------------------------------------
TEST_F(BookHistoryTest, what_a_change_was_is_said_by_its_fields)
{
    proto::BookHistory history = page(2, 0);
    change(batch(&history, 2), kServer, proto::BOOK_ENTRY_KIND_COMPUTER,
           computer(kServer, "server", "12345", "old secret"), 1, false,
           computer(kServer, "server", "54321", "new secret"), false);

    const std::vector<HistoryBatch> batches = openHistory(history, key_);
    const ChangeDescription description = describeChange(batches[0].changes[0]);

    EXPECT_EQ(description.action, ChangeDescription::Action::CHANGED);
    EXPECT_EQ(description.name, "server");
    EXPECT_EQ(description.fields, (std::vector<std::string>{ "address", "password" }));

    // Which fields, never what they became.
    for (const std::string& field : description.fields)
    {
        EXPECT_EQ(field.find("secret"), std::string::npos);
        EXPECT_EQ(field.find("54321"), std::string::npos);
    }
}

//--------------------------------------------------------------------------------------------------
TEST_F(BookHistoryTest, every_kind_of_change_is_told_apart)
{
    proto::BookHistory history = page(6, 0);

    created(batch(&history, 6), kServer, proto::BOOK_ENTRY_KIND_COMPUTER,
            computer(kServer, "server", "1"));

    change(batch(&history, 5), kServer, proto::BOOK_ENTRY_KIND_COMPUTER,
           computer(kServer, "server", "1"), 4, false, std::string(), true);

    change(batch(&history, 4), kServer, proto::BOOK_ENTRY_KIND_COMPUTER,
           std::string(), 3, true, computer(kServer, "server", "1"), false);

    change(batch(&history, 3), kServer, proto::BOOK_ENTRY_KIND_COMPUTER,
           computer(kServer, "server", "1"), 2, false, computer(kServer, "server", "1"), false,
           std::string(), kGroup);

    change(batch(&history, 2), kGroup, proto::BOOK_ENTRY_KIND_GROUP,
           group(kGroup, "office"), 1, false, group(kGroup, "accounting"), false,
           std::string(), kServer);

    const std::vector<HistoryBatch> batches = openHistory(history, key_);

    EXPECT_EQ(describeChange(batches[0].changes[0]).action, ChangeDescription::Action::CREATED);
    EXPECT_EQ(describeChange(batches[1].changes[0]).action, ChangeDescription::Action::DELETED);
    EXPECT_EQ(describeChange(batches[1].changes[0]).name, "server");
    EXPECT_EQ(describeChange(batches[2].changes[0]).action, ChangeDescription::Action::RESTORED);
    EXPECT_EQ(describeChange(batches[3].changes[0]).action, ChangeDescription::Action::MOVED);

    const ChangeDescription renamed = describeChange(batches[4].changes[0]);
    EXPECT_EQ(renamed.action, ChangeDescription::Action::CHANGED);
    EXPECT_EQ(renamed.name, "accounting");
    EXPECT_EQ(renamed.fields, (std::vector<std::string>{ "name", "group" }));
}

//--------------------------------------------------------------------------------------------------
// Sealed with another key, or damaged on the way: shown as such, and never put back.
TEST_F(BookHistoryTest, a_payload_that_does_not_open_is_shown_but_never_put_back)
{
    proto::BookHistory history = page(2, 0);
    change(batch(&history, 2), kServer, proto::BOOK_ENTRY_KIND_COMPUTER,
           computer(kServer, "server", "1"), 1, false, std::string(), true);

    // Somebody who holds the database swaps the sealed payload for something else.
    history.mutable_batch(0)->mutable_change(0)->mutable_before()->set_payload("not sealed");

    const std::vector<HistoryBatch> batches = openHistory(history, key_);
    ASSERT_EQ(batches.size(), 1u);

    EXPECT_FALSE(batches[0].changes[0].before.readable);
    EXPECT_EQ(describeChange(batches[0].changes[0]).action,
              ChangeDescription::Action::UNREADABLE);

    const RollbackPlan plan = planRollback(batches, 1);
    EXPECT_TRUE(plan.targets.empty());
    EXPECT_EQ(plan.unreadable, (std::vector<std::string>{ kServer }));
}

//--------------------------------------------------------------------------------------------------
TEST_F(BookHistoryTest, going_back_undoes_what_came_after)
{
    proto::BookHistory history = page(5, 0);

    // 5: the laptop, made after the point, is edited.
    change(batch(&history, 5), kLaptop, proto::BOOK_ENTRY_KIND_COMPUTER,
           computer(kLaptop, "laptop", "1"), 4, false, computer(kLaptop, "laptop", "2"), false);

    // 4: the server is deleted.
    change(batch(&history, 4), kServer, proto::BOOK_ENTRY_KIND_COMPUTER,
           computer(kServer, "server", "b"), 3, false, std::string(), true);

    // 3: the laptop is made.
    created(batch(&history, 3), kLaptop, proto::BOOK_ENTRY_KIND_COMPUTER,
            computer(kLaptop, "laptop", "1"));

    // 2: the server is edited.
    change(batch(&history, 2), kServer, proto::BOOK_ENTRY_KIND_COMPUTER,
           computer(kServer, "server", "a"), 1, false, computer(kServer, "server", "b"), false);

    const std::vector<HistoryBatch> batches = openHistory(history, key_);

    // Back to 1: the server as it was before its edit, and no laptop.
    const RollbackPlan plan = planRollback(batches, 1);
    ASSERT_EQ(plan.targets.size(), 2u);

    const RollbackTarget* server = findTarget(plan, kServer);
    ASSERT_TRUE(server);
    EXPECT_TRUE(server->present);
    EXPECT_EQ(server->payload, computer(kServer, "server", "a"));
    EXPECT_EQ(server->router_revision, 4); // The headstone.
    EXPECT_FALSE(server->router_alive);

    const RollbackTarget* laptop = findTarget(plan, kLaptop);
    ASSERT_TRUE(laptop);
    EXPECT_FALSE(laptop->present);
    EXPECT_EQ(laptop->router_revision, 5);
    EXPECT_TRUE(laptop->router_alive);

    // Back to 3: only the deletion and the laptop's edit are undone.
    const RollbackPlan later = planRollback(batches, 3);
    ASSERT_EQ(later.targets.size(), 2u);
    EXPECT_EQ(findTarget(later, kServer)->payload, computer(kServer, "server", "b"));
    EXPECT_EQ(findTarget(later, kLaptop)->payload, computer(kLaptop, "laptop", "1"));
    EXPECT_TRUE(findTarget(later, kLaptop)->present);

    // And one record alone.
    const RollbackPlan one = planRollback(batches, 3, kServer);
    ASSERT_EQ(one.targets.size(), 1u);
    EXPECT_EQ(one.targets[0].guid, kServer);
}

//--------------------------------------------------------------------------------------------------
TEST_F(BookHistoryTest, what_came_and_went_after_the_point_needs_nothing)
{
    proto::BookHistory history = page(3, 0);
    change(batch(&history, 3), kLaptop, proto::BOOK_ENTRY_KIND_COMPUTER,
           computer(kLaptop, "laptop", "1"), 2, false, std::string(), true);
    created(batch(&history, 2), kLaptop, proto::BOOK_ENTRY_KIND_COMPUTER,
            computer(kLaptop, "laptop", "1"));

    const std::vector<HistoryBatch> batches = openHistory(history, key_);
    EXPECT_TRUE(planRollback(batches, 1).targets.empty());
}

//--------------------------------------------------------------------------------------------------
TEST_F(BookHistoryTest, an_edit_changed_back_needs_nothing)
{
    proto::BookHistory history = page(3, 0);
    change(batch(&history, 3), kServer, proto::BOOK_ENTRY_KIND_COMPUTER,
           computer(kServer, "server", "2"), 2, false, computer(kServer, "server", "1"), false);
    change(batch(&history, 2), kServer, proto::BOOK_ENTRY_KIND_COMPUTER,
           computer(kServer, "server", "1"), 1, false, computer(kServer, "server", "2"), false);

    const std::vector<HistoryBatch> batches = openHistory(history, key_);

    // Sealed twice, the two "1"s differ as bytes; opened, they are the same.
    EXPECT_TRUE(planRollback(batches, 1).targets.empty());
}

//--------------------------------------------------------------------------------------------------
// The journal holds the pages in order and refuses one that would leave a hole.
TEST_F(BookHistoryTest, the_journal_takes_pages_in_order)
{
    HistoryJournal journal;

    proto::BookHistory first = page(4, 0, true);
    created(batch(&first, 4), kServer, proto::BOOK_ENTRY_KIND_COMPUTER,
            computer(kServer, "a", "1"));
    created(batch(&first, 3), kLaptop, proto::BOOK_ENTRY_KIND_COMPUTER,
            computer(kLaptop, "b", "1"));

    ASSERT_TRUE(journal.addPage(first, kBook, 0, key_));
    EXPECT_EQ(journal.revision(), 4);
    EXPECT_TRUE(journal.hasMore());
    EXPECT_EQ(journal.nextBefore(), 3);
    EXPECT_FALSE(journal.covers(1));
    EXPECT_TRUE(journal.covers(2));

    // An answer to something no longer asked.
    proto::BookHistory stale = page(4, 0, false);
    EXPECT_FALSE(journal.addPage(stale, kBook, 4, key_));

    // Another book.
    proto::BookHistory other = page(4, 0, false);
    other.set_book_guid("22222222-2222-4222-8222-222222222222");
    EXPECT_FALSE(journal.addPage(other, kBook, 3, key_));

    proto::BookHistory second = page(4, 0, false);
    created(batch(&second, 2), kGroup, proto::BOOK_ENTRY_KIND_GROUP, group(kGroup, "g"));
    ASSERT_TRUE(journal.addPage(second, kBook, 3, key_));

    EXPECT_EQ(journal.batches().size(), 3u);
    EXPECT_FALSE(journal.hasMore());
    EXPECT_TRUE(journal.covers(0));

    // Starting over from the newest drops what was there.
    proto::BookHistory again = page(5, 0, true);
    created(batch(&again, 5), kServer, proto::BOOK_ENTRY_KIND_COMPUTER,
            computer(kServer, "a", "2"));
    ASSERT_TRUE(journal.addPage(again, kBook, 0, key_));
    EXPECT_EQ(journal.batches().size(), 1u);
    EXPECT_EQ(journal.revision(), 5);
}

//--------------------------------------------------------------------------------------------------
TEST_F(BookHistoryTest, a_rollback_is_refused_when_the_book_is_not_in_step)
{
    Data data;
    data.mutable_sync()->set_book_guid(kBook);
    data.mutable_sync()->set_last_pulled_revision(2);

    Computer* server = data.mutable_root_group()->add_computer();
    server->set_guid(kServer);
    server->set_name("server");
    server->set_address("b");

    proto::address_book::SyncEntryState* state = data.mutable_sync()->add_entry();
    state->set_guid(kServer);
    state->set_revision(2);
    state->set_base_payload(server->SerializeAsString());

    proto::BookHistory history = page(2, 1);
    change(batch(&history, 2), kServer, proto::BOOK_ENTRY_KIND_COMPUTER,
           computer(kServer, "server", "a"), 1, false, computer(kServer, "server", "b"), false);

    HistoryJournal journal;
    ASSERT_TRUE(journal.addPage(history, kBook, 0, key_));

    RollbackPlan plan;
    EXPECT_EQ(prepareRollback(journal, data, 1, std::string(), &plan), RollbackCheck::OK);

    // Older than the history reaches.
    EXPECT_EQ(prepareRollback(journal, data, 0, std::string(), &plan), RollbackCheck::TOO_OLD);

    // Nothing after the newest.
    EXPECT_EQ(prepareRollback(journal, data, 2, std::string(), &plan),
              RollbackCheck::NOTHING_TO_DO);

    // An edit here that has not gone out yet.
    Data edited(data);
    edited.mutable_root_group()->mutable_computer(0)->set_comment("typing");
    EXPECT_EQ(prepareRollback(journal, edited, 1, std::string(), &plan),
              RollbackCheck::NOT_IN_STEP);

    // Behind the router.
    Data behind(data);
    behind.mutable_sync()->set_last_pulled_revision(1);
    EXPECT_EQ(prepareRollback(journal, behind, 1, std::string(), &plan),
              RollbackCheck::NOT_IN_STEP);

    // A question waiting for a person.
    Data waiting(data);
    waiting.mutable_sync()->mutable_entry(0)->set_conflict(true);
    EXPECT_EQ(prepareRollback(journal, waiting, 1, std::string(), &plan),
              RollbackCheck::NOT_IN_STEP);
}

//--------------------------------------------------------------------------------------------------
// Brought back over its headstone: in the book, marked to go out, and built on the headstone's
// revision so the router takes it.
TEST_F(BookHistoryTest, a_deleted_record_comes_back_ready_to_be_sent)
{
    Data data;
    data.mutable_sync()->set_book_guid(kBook);
    data.mutable_sync()->set_last_pulled_revision(3);

    ComputerGroup* office = data.mutable_root_group()->add_computer_group();
    office->set_guid(kGroup);
    office->set_name("office");

    proto::address_book::SyncEntryState* state = data.mutable_sync()->add_entry();
    state->set_guid(kGroup);
    state->set_revision(1);
    state->set_base_payload(group(kGroup, "office"));

    proto::BookHistory history = page(3, 0);
    change(batch(&history, 3), kServer, proto::BOOK_ENTRY_KIND_COMPUTER,
           computer(kServer, "server", "1", "secret"), 2, false, std::string(), true, kGroup,
           kGroup);
    created(batch(&history, 2), kServer, proto::BOOK_ENTRY_KIND_COMPUTER,
            computer(kServer, "server", "1", "secret"), kGroup);

    HistoryJournal journal;
    ASSERT_TRUE(journal.addPage(history, kBook, 0, key_));

    RollbackPlan plan;
    ASSERT_EQ(prepareRollback(journal, data, 2, kServer, &plan), RollbackCheck::OK);

    const RollbackCounts counts = applyRollback(plan, &data);
    EXPECT_EQ(counts.restored, 1u);
    EXPECT_EQ(counts.removed, 0u);
    EXPECT_EQ(counts.changed, 0u);

    // Back in its group, as it was.
    ASSERT_EQ(data.root_group().computer_group_size(), 1);
    const ComputerGroup& restored_in = data.root_group().computer_group(0);
    ASSERT_EQ(restored_in.computer_size(), 1);
    EXPECT_EQ(restored_in.computer(0).name(), "server");
    EXPECT_EQ(restored_in.computer(0).password(), "secret");

    markLocalChanges(&data);

    bool found = false;
    for (const auto& entry : data.sync().entry())
    {
        if (entry.guid() != kServer)
            continue;

        found = true;
        EXPECT_TRUE(entry.dirty());
        EXPECT_FALSE(entry.deleted());
        EXPECT_EQ(entry.revision(), 3); // The headstone's.
    }
    EXPECT_TRUE(found);
}

} // namespace console
