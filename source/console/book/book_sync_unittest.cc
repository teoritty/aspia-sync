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

#include "console/book/book_sync.h"

#include "console/book/entry_guid.h"
#include "console/book/flat_book.h"
#include "console/book/sync_key.h"
#include "proto/address_book.pb.h"
#include "proto/router_book.pb.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace console {

namespace {

using proto::address_book::Computer;
using proto::address_book::ComputerGroup;
using proto::address_book::Data;

const char kBookGuid[] = "11111111-1111-4111-8111-111111111111";
const char kEpoch[] = "22222222-2222-4222-8222-222222222222";

std::string testKey()
{
    return deriveSyncKey("department passphrase", std::string(kSyncSaltSize, 'z'));
}

// Stands in for the router. Remembers what was asked of it, so a test can look at what the
// exchange did rather than at what it said it would do.
class FakeSender : public BookSync::Sender
{
public:
    void sendPull(const proto::BookPullRequest& request) override
    {
        pulls.push_back(request);
    }

    void sendPush(const proto::BookPushRequest& request) override
    {
        pushes.push_back(request);
    }

    std::vector<proto::BookPullRequest> pulls;
    std::vector<proto::BookPushRequest> pushes;
};

class RecordingObserver : public BookSync::Observer
{
public:
    void onBookUpdated() override { ++updates; }

    void onConflicts(const std::vector<std::string>& guids) override
    {
        for (const std::string& guid : guids)
            conflicts.push_back(guid);
    }

    void onSyncStopped(SyncEngine::PullOutcome::Status reason) override
    {
        stopped = true;
        stop_reason = reason;
    }

    void onInSync(int64_t revision) override
    {
        in_sync = true;
        in_sync_revision = revision;
    }

    int updates = 0;
    std::vector<std::string> conflicts;
    bool stopped = false;
    SyncEngine::PullOutcome::Status stop_reason = SyncEngine::PullOutcome::Status::OK;
    bool in_sync = false;
    int64_t in_sync_revision = 0;
};

Data makeBook()
{
    Data data;

    ComputerGroup* root = data.mutable_root_group();
    root->set_name("book");

    Computer* computer = root->add_computer();
    computer->set_name("server");
    computer->set_address("12345");

    ensureEntryGuids(root);

    proto::address_book::SyncState* sync = data.mutable_sync();
    sync->set_book_guid(kBookGuid);
    sync->set_epoch(kEpoch);
    sync->set_last_pulled_revision(1);

    for (const FlatEntry& entry : flattenBook(*root))
    {
        proto::address_book::SyncEntryState* state = sync->add_entry();
        state->set_guid(entry.guid);
        state->set_revision(1);
        state->set_base_payload(entry.payload);
        state->set_base_parent_guid(entry.parent_guid);
    }

    return data;
}

proto::BookPull emptyPage(int64_t revision)
{
    proto::BookPull page;
    page.set_error_code(proto::BOOK_ERROR_CODE_OK);
    page.set_book_guid(kBookGuid);
    page.set_epoch(kEpoch);
    page.set_revision(revision);
    return page;
}

} // namespace

//--------------------------------------------------------------------------------------------------
TEST(book_sync_test, an_unchanged_book_only_asks)
{
    FakeSender sender;
    RecordingObserver observer;
    BookSync sync(testKey(), &sender, &observer);

    Data data = makeBook();
    sync.start(&data);

    EXPECT_TRUE(sender.pushes.empty());
    ASSERT_EQ(sender.pulls.size(), 1u);
    EXPECT_EQ(sender.pulls.front().since_revision(), 1);
}

//--------------------------------------------------------------------------------------------------
// Sending before asking. The other way round, an edit made here would be overwritten by the answer
// before it ever left the machine.
TEST(book_sync_test, what_is_waiting_goes_out_before_anything_is_asked)
{
    FakeSender sender;
    RecordingObserver observer;
    BookSync sync(testKey(), &sender, &observer);

    Data data = makeBook();
    data.mutable_root_group()->mutable_computer(0)->set_comment("changed here");

    sync.start(&data);

    ASSERT_EQ(sender.pushes.size(), 1u);
    EXPECT_TRUE(sender.pulls.empty());
    EXPECT_EQ(sender.pushes.front().change_size(), 1);
}

//--------------------------------------------------------------------------------------------------
TEST(book_sync_test, an_accepted_batch_is_followed_by_asking)
{
    FakeSender sender;
    RecordingObserver observer;
    BookSync sync(testKey(), &sender, &observer);

    Data data = makeBook();
    data.mutable_root_group()->mutable_computer(0)->set_comment("changed here");
    sync.start(&data);

    ASSERT_EQ(sender.pushes.size(), 1u);

    proto::BookPushResult result;
    result.set_error_code(proto::BOOK_ERROR_CODE_OK);
    result.set_revision(2);

    proto::BookChangeResult* entry = result.add_result();
    entry->set_guid(sender.pushes.front().change(0).guid());
    entry->set_status(proto::BOOK_CHANGE_STATUS_OK);

    sync.onPushResult(result, &data);

    ASSERT_EQ(sender.pulls.size(), 1u);
}

//--------------------------------------------------------------------------------------------------
TEST(book_sync_test, an_exchange_with_nothing_left_reports_being_in_step)
{
    FakeSender sender;
    RecordingObserver observer;
    BookSync sync(testKey(), &sender, &observer);

    Data data = makeBook();
    sync.start(&data);
    ASSERT_EQ(sender.pulls.size(), 1u);

    sync.onPull(emptyPage(1), &data);

    EXPECT_TRUE(observer.in_sync);
    EXPECT_EQ(observer.in_sync_revision, 1);
    EXPECT_FALSE(sync.isBusy());
}

//--------------------------------------------------------------------------------------------------
// A book of several hundred records arrives in pieces, and the next piece is taken by moving along
// the page rather than by the revision - which has not moved yet on purpose.
TEST(book_sync_test, more_pages_are_asked_for_in_turn)
{
    FakeSender sender;
    RecordingObserver observer;
    BookSync sync(testKey(), &sender, &observer);

    Data data = makeBook();
    sync.start(&data);

    Computer arriving;
    arriving.set_name("workstation");

    proto::BookPull page = emptyPage(9);
    page.set_has_more(true);

    proto::BookEntryData* entry = page.add_entry();
    entry->set_guid("33333333-3333-4333-8333-333333333333");
    entry->set_parent_guid(data.root_group().guid());
    entry->set_kind(proto::BOOK_ENTRY_KIND_COMPUTER);
    entry->set_revision(9);

    std::string sealed;
    ASSERT_TRUE(sealPayload(testKey(), arriving.SerializeAsString(), &sealed));
    entry->set_payload(sealed);

    sync.onPull(page, &data);

    ASSERT_EQ(sender.pulls.size(), 2u);
    EXPECT_EQ(sender.pulls.back().offset(), 1);
    EXPECT_FALSE(observer.in_sync);
    EXPECT_TRUE(sync.isBusy());
}

//--------------------------------------------------------------------------------------------------
// A record that merged cleanly is ready to go out again, and the exchange has to carry it rather
// than stop and wait for the next reason to run.
TEST(book_sync_test, what_merged_goes_out_without_waiting)
{
    FakeSender sender;
    RecordingObserver observer;
    BookSync sync(testKey(), &sender, &observer);

    Data data = makeBook();
    const std::string guid = data.root_group().computer(0).guid();

    data.mutable_root_group()->mutable_computer(0)->set_comment("mine");
    sync.start(&data);
    ASSERT_EQ(sender.pushes.size(), 1u);

    Computer theirs = makeBook().root_group().computer(0);
    theirs.set_guid(guid);
    theirs.set_password("theirs");

    proto::BookPull page = emptyPage(5);
    proto::BookEntryData* entry = page.add_entry();
    entry->set_guid(guid);
    entry->set_parent_guid(data.root_group().guid());
    entry->set_kind(proto::BOOK_ENTRY_KIND_COMPUTER);
    entry->set_revision(5);

    std::string sealed;
    ASSERT_TRUE(sealPayload(testKey(), theirs.SerializeAsString(), &sealed));
    entry->set_payload(sealed);

    sync.onPull(page, &data);

    ASSERT_EQ(sender.pushes.size(), 2u);
    EXPECT_TRUE(sync.isBusy());
}

//--------------------------------------------------------------------------------------------------
// The one failure that must not be retried: the router was restored from a backup.
TEST(book_sync_test, a_changed_epoch_stops_the_exchange)
{
    FakeSender sender;
    RecordingObserver observer;
    BookSync sync(testKey(), &sender, &observer);

    Data data = makeBook();
    sync.start(&data);

    proto::BookPull page = emptyPage(2);
    page.set_epoch("99999999-9999-4999-8999-999999999999");

    sync.onPull(page, &data);

    EXPECT_TRUE(observer.stopped);
    EXPECT_EQ(observer.stop_reason, SyncEngine::PullOutcome::Status::EPOCH_CHANGED);
    EXPECT_FALSE(sync.isBusy());

    // And it does not go asking again on its own.
    EXPECT_EQ(sender.pulls.size(), 1u);
}

//--------------------------------------------------------------------------------------------------
// Everybody is told when something moves, including the console whose change it was. Asking for
// what is already here would only make work.
TEST(book_sync_test, a_notification_about_what_is_already_here_is_ignored)
{
    FakeSender sender;
    RecordingObserver observer;
    BookSync sync(testKey(), &sender, &observer);

    Data data = makeBook();

    proto::BookChanged message;
    message.set_book_guid(kBookGuid);
    message.set_revision(1);

    sync.onBookChanged(message, &data);

    EXPECT_TRUE(sender.pulls.empty());
    EXPECT_TRUE(sender.pushes.empty());
}

//--------------------------------------------------------------------------------------------------
TEST(book_sync_test, a_notification_about_something_newer_starts_an_exchange)
{
    FakeSender sender;
    RecordingObserver observer;
    BookSync sync(testKey(), &sender, &observer);

    Data data = makeBook();

    proto::BookChanged message;
    message.set_book_guid(kBookGuid);
    message.set_revision(2);

    sync.onBookChanged(message, &data);

    EXPECT_EQ(sender.pulls.size(), 1u);
}

//--------------------------------------------------------------------------------------------------
TEST(book_sync_test, a_notification_about_another_book_is_ignored)
{
    FakeSender sender;
    RecordingObserver observer;
    BookSync sync(testKey(), &sender, &observer);

    Data data = makeBook();

    proto::BookChanged message;
    message.set_book_guid("55555555-5555-4555-8555-555555555555");
    message.set_revision(99);

    sync.onBookChanged(message, &data);

    EXPECT_TRUE(sender.pulls.empty());
}

//--------------------------------------------------------------------------------------------------
// Answers to a second exchange would arrive about a book that moved underneath them, so a request
// made while one is running is remembered and run after it.
TEST(book_sync_test, a_second_exchange_waits_for_the_first)
{
    FakeSender sender;
    RecordingObserver observer;
    BookSync sync(testKey(), &sender, &observer);

    Data data = makeBook();
    sync.start(&data);
    ASSERT_EQ(sender.pulls.size(), 1u);

    sync.start(&data); // Something was edited while the first was in flight.
    EXPECT_EQ(sender.pulls.size(), 1u);

    sync.onPull(emptyPage(1), &data);

    EXPECT_EQ(sender.pulls.size(), 2u);
}

//--------------------------------------------------------------------------------------------------
TEST(book_sync_test, a_book_that_is_not_synchronized_does_nothing)
{
    FakeSender sender;
    RecordingObserver observer;
    BookSync sync(testKey(), &sender, &observer);

    Data data = makeBook();
    data.mutable_sync()->clear_book_guid();

    sync.start(&data);

    EXPECT_TRUE(sender.pulls.empty());
    EXPECT_TRUE(sender.pushes.empty());
}

//--------------------------------------------------------------------------------------------------
TEST(book_sync_test, an_error_from_the_router_ends_the_exchange_quietly)
{
    FakeSender sender;
    RecordingObserver observer;
    BookSync sync(testKey(), &sender, &observer);

    Data data = makeBook();
    sync.start(&data);

    proto::BookPull page = emptyPage(2);
    page.set_error_code(proto::BOOK_ERROR_CODE_INTERNAL_ERROR);

    sync.onPull(page, &data);

    EXPECT_FALSE(sync.isBusy());
    EXPECT_FALSE(observer.stopped); // Not the kind of failure a person has to be pulled into.
    EXPECT_FALSE(observer.in_sync);
}

//--------------------------------------------------------------------------------------------------
TEST(book_sync_test, conflicts_are_remembered_until_they_are_dealt_with)
{
    FakeSender sender;
    RecordingObserver observer;
    BookSync sync(testKey(), &sender, &observer);

    Data data = makeBook();
    const std::string guid = data.root_group().computer(0).guid();

    data.mutable_root_group()->mutable_computer(0)->set_password("what I set");
    sync.start(&data);

    Computer theirs = makeBook().root_group().computer(0);
    theirs.set_guid(guid);
    theirs.set_password("what they set");

    proto::BookPull page = emptyPage(5);
    proto::BookEntryData* entry = page.add_entry();
    entry->set_guid(guid);
    entry->set_parent_guid(data.root_group().guid());
    entry->set_kind(proto::BOOK_ENTRY_KIND_COMPUTER);
    entry->set_revision(5);

    std::string sealed;
    ASSERT_TRUE(sealPayload(testKey(), theirs.SerializeAsString(), &sealed));
    entry->set_payload(sealed);

    sync.onPull(page, &data);

    ASSERT_EQ(sync.conflicts().size(), 1u);
    EXPECT_EQ(sync.conflicts().front(), guid);

    sync.clearConflict(guid);
    EXPECT_TRUE(sync.conflicts().empty());
}

} // namespace console
