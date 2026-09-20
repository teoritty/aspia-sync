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

#include "console/book/sync_engine.h"

#include "console/book/entry_guid.h"
#include "console/book/flat_book.h"
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

const char kBookGuid[] = "11111111-1111-4111-8111-111111111111";
const char kEpoch[] = "22222222-2222-4222-8222-222222222222";

std::string testKey()
{
    return deriveSyncKey("department passphrase", std::string(kSyncSaltSize, 'z'));
}

// A book with a root and one computer in it, already known to be in step with the router.
Data makeBook(int64_t revision = 1)
{
    Data data;

    ComputerGroup* root = data.mutable_root_group();
    root->set_name("book");

    Computer* computer = root->add_computer();
    computer->set_name("server");
    computer->set_address("12345");
    computer->set_password("secret");

    ensureEntryGuids(root);

    proto::address_book::SyncState* sync = data.mutable_sync();
    sync->set_book_guid(kBookGuid);
    sync->set_epoch(kEpoch);
    sync->set_last_pulled_revision(revision);

    for (const FlatEntry& entry : flattenBook(*root))
    {
        proto::address_book::SyncEntryState* state = sync->add_entry();
        state->set_guid(entry.guid);
        state->set_revision(revision);
        state->set_base_payload(entry.payload);
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

std::string computerGuid(const Data& data)
{
    return data.root_group().computer(0).guid();
}

// Records are looked up by guid rather than by position: rebuilding the tree orders them by guid,
// and the window sorts them by name anyway, so a position means nothing.
const Computer* findComputer(const Data& data, const std::string& guid)
{
    for (int i = 0; i < data.root_group().computer_size(); ++i)
    {
        if (data.root_group().computer(i).guid() == guid)
            return &data.root_group().computer(i);
    }
    return nullptr;
}

// One record as the router would send it.
void addEntry(proto::BookPull* page, const std::string& guid, const std::string& parent,
              proto::BookEntryKind kind, int64_t revision, const std::string& payload,
              const std::string& key)
{
    proto::BookEntryData* entry = page->add_entry();
    entry->set_guid(guid);
    entry->set_parent_guid(parent);
    entry->set_kind(kind);
    entry->set_revision(revision);

    std::string sealed;
    if (!payload.empty())
    {
        sealPayload(key, payload, &sealed);
        entry->set_payload(sealed);
    }
}

proto::BookPull makePage(int64_t revision)
{
    proto::BookPull page;
    page.set_error_code(proto::BOOK_ERROR_CODE_OK);
    page.set_book_guid(kBookGuid);
    page.set_epoch(kEpoch);
    page.set_revision(revision);
    return page;
}

bool listed(const std::vector<std::string>& list, const std::string& guid)
{
    return std::find(list.begin(), list.end(), guid) != list.end();
}

} // namespace

//--------------------------------------------------------------------------------------------------
TEST(sync_engine_test, a_new_record_from_the_router_is_taken)
{
    SyncEngine engine(testKey());
    Data data = makeBook();

    Computer arriving;
    arriving.set_name("workstation");
    arriving.set_address("54321");

    proto::BookPull page = makePage(2);
    addEntry(&page, "33333333-3333-4333-8333-333333333333", data.root_group().guid(),
             proto::BOOK_ENTRY_KIND_COMPUTER, 2, arriving.SerializeAsString(), testKey());

    const SyncEngine::PullOutcome outcome = engine.applyPull(page, &data);

    EXPECT_EQ(outcome.status, SyncEngine::PullOutcome::Status::OK);
    EXPECT_EQ(outcome.applied, 1u);
    EXPECT_TRUE(outcome.conflicts.empty());

    ASSERT_EQ(data.root_group().computer_size(), 2);
    EXPECT_EQ(data.sync().last_pulled_revision(), 2);
}

//--------------------------------------------------------------------------------------------------
TEST(sync_engine_test, an_edit_from_the_router_replaces_an_untouched_record)
{
    SyncEngine engine(testKey());
    Data data = makeBook();
    const std::string guid = computerGuid(data);

    Computer edited = data.root_group().computer(0);
    edited.set_comment("called about the printer");

    proto::BookPull page = makePage(2);
    addEntry(&page, guid, data.root_group().guid(), proto::BOOK_ENTRY_KIND_COMPUTER, 2,
             edited.SerializeAsString(), testKey());

    const SyncEngine::PullOutcome outcome = engine.applyPull(page, &data);

    EXPECT_EQ(outcome.status, SyncEngine::PullOutcome::Status::OK);
    EXPECT_EQ(outcome.applied, 1u);

    ASSERT_EQ(data.root_group().computer_size(), 1);
    EXPECT_EQ(data.root_group().computer(0).comment(), "called about the printer");

    const proto::address_book::SyncEntryState* state = stateOf(data, guid);
    ASSERT_TRUE(state);
    EXPECT_FALSE(state->dirty());
    EXPECT_EQ(state->revision(), 2);
}

//--------------------------------------------------------------------------------------------------
// Edits on both sides, to different fields. Both survive and nobody is asked.
TEST(sync_engine_test, edits_on_both_sides_are_merged)
{
    SyncEngine engine(testKey());
    Data data = makeBook();
    const std::string guid = computerGuid(data);

    // Changed here and not sent yet.
    data.mutable_root_group()->mutable_computer(0)->set_comment("mine");
    for (int i = 0; i < data.sync().entry_size(); ++i)
    {
        if (data.sync().entry(i).guid() == guid)
            data.mutable_sync()->mutable_entry(i)->set_dirty(true);
    }

    // And changed on the router, in another field.
    Computer remote = makeBook().root_group().computer(0);
    remote.set_guid(guid);
    remote.set_password("rotated on friday");

    proto::BookPull page = makePage(2);
    addEntry(&page, guid, data.root_group().guid(), proto::BOOK_ENTRY_KIND_COMPUTER, 2,
             remote.SerializeAsString(), testKey());

    const SyncEngine::PullOutcome outcome = engine.applyPull(page, &data);

    EXPECT_EQ(outcome.status, SyncEngine::PullOutcome::Status::OK);
    EXPECT_TRUE(outcome.conflicts.empty());

    const Computer& merged = data.root_group().computer(0);
    EXPECT_EQ(merged.comment(), "mine");
    EXPECT_EQ(merged.password(), "rotated on friday");

    // The merged version is not what the router has, so it is still waiting to be sent.
    const proto::address_book::SyncEntryState* state = stateOf(data, guid);
    ASSERT_TRUE(state);
    EXPECT_TRUE(state->dirty());
}

//--------------------------------------------------------------------------------------------------
// The same field on both sides. The local value stays and the record is reported - and, crucially,
// the rest of the page still goes in.
TEST(sync_engine_test, a_real_conflict_does_not_stop_the_rest)
{
    SyncEngine engine(testKey());
    Data data = makeBook();
    const std::string guid = computerGuid(data);

    data.mutable_root_group()->mutable_computer(0)->set_password("what I set");
    for (int i = 0; i < data.sync().entry_size(); ++i)
    {
        if (data.sync().entry(i).guid() == guid)
            data.mutable_sync()->mutable_entry(i)->set_dirty(true);
    }

    Computer remote = makeBook().root_group().computer(0);
    remote.set_guid(guid);
    remote.set_password("what they set");

    Computer arriving;
    arriving.set_name("another machine");

    proto::BookPull page = makePage(2);
    addEntry(&page, guid, data.root_group().guid(), proto::BOOK_ENTRY_KIND_COMPUTER, 2,
             remote.SerializeAsString(), testKey());
    addEntry(&page, "44444444-4444-4444-8444-444444444444", data.root_group().guid(),
             proto::BOOK_ENTRY_KIND_COMPUTER, 2, arriving.SerializeAsString(), testKey());

    const SyncEngine::PullOutcome outcome = engine.applyPull(page, &data);

    EXPECT_EQ(outcome.status, SyncEngine::PullOutcome::Status::OK);
    ASSERT_EQ(outcome.conflicts.size(), 1u);
    EXPECT_TRUE(listed(outcome.conflicts, guid));

    // What the person typed is what they keep seeing.
    const Computer* kept = findComputer(data, guid);
    ASSERT_TRUE(kept);
    EXPECT_EQ(kept->password(), "what I set");

    // And the unrelated record went in regardless.
    EXPECT_EQ(data.root_group().computer_size(), 2);
}

//--------------------------------------------------------------------------------------------------
TEST(sync_engine_test, a_headstone_removes_an_untouched_record)
{
    SyncEngine engine(testKey());
    Data data = makeBook();
    const std::string guid = computerGuid(data);

    proto::BookPull page = makePage(2);
    proto::BookEntryData* entry = page.add_entry();
    entry->set_guid(guid);
    entry->set_deleted(true);
    entry->set_revision(2);

    const SyncEngine::PullOutcome outcome = engine.applyPull(page, &data);

    EXPECT_EQ(outcome.status, SyncEngine::PullOutcome::Status::OK);
    EXPECT_EQ(outcome.applied, 1u);
    EXPECT_EQ(data.root_group().computer_size(), 0);
    EXPECT_FALSE(stateOf(data, guid));
}

//--------------------------------------------------------------------------------------------------
// Deleted there while being edited here. Which of the two was meant is not for the code to decide.
TEST(sync_engine_test, a_headstone_for_a_record_edited_here_asks_a_person)
{
    SyncEngine engine(testKey());
    Data data = makeBook();
    const std::string guid = computerGuid(data);

    data.mutable_root_group()->mutable_computer(0)->set_comment("still in use");
    for (int i = 0; i < data.sync().entry_size(); ++i)
    {
        if (data.sync().entry(i).guid() == guid)
            data.mutable_sync()->mutable_entry(i)->set_dirty(true);
    }

    proto::BookPull page = makePage(2);
    proto::BookEntryData* entry = page.add_entry();
    entry->set_guid(guid);
    entry->set_deleted(true);
    entry->set_revision(2);

    const SyncEngine::PullOutcome outcome = engine.applyPull(page, &data);

    EXPECT_TRUE(listed(outcome.conflicts, guid));
    EXPECT_EQ(data.root_group().computer_size(), 1);
}

//--------------------------------------------------------------------------------------------------
// The nastiest failure of all, because it is silent: the router is restored from a backup and its
// revision no longer refers to what this console remembers.
TEST(sync_engine_test, a_changed_epoch_stops_everything)
{
    SyncEngine engine(testKey());
    Data data = makeBook();

    proto::BookPull page = makePage(2);
    page.set_epoch("99999999-9999-4999-8999-999999999999");

    const SyncEngine::PullOutcome outcome = engine.applyPull(page, &data);

    EXPECT_EQ(outcome.status, SyncEngine::PullOutcome::Status::EPOCH_CHANGED);
    EXPECT_EQ(data.sync().last_pulled_revision(), 1);
}

//--------------------------------------------------------------------------------------------------
TEST(sync_engine_test, a_revision_that_went_back_stops_everything)
{
    SyncEngine engine(testKey());
    Data data = makeBook(10);

    const SyncEngine::PullOutcome outcome = engine.applyPull(makePage(5), &data);

    EXPECT_EQ(outcome.status, SyncEngine::PullOutcome::Status::REVISION_WENT_BACK);
    EXPECT_EQ(data.sync().last_pulled_revision(), 10);
}

//--------------------------------------------------------------------------------------------------
TEST(sync_engine_test, an_answer_about_another_book_is_refused)
{
    SyncEngine engine(testKey());
    Data data = makeBook();

    proto::BookPull page = makePage(2);
    page.set_book_guid("55555555-5555-4555-8555-555555555555");

    EXPECT_EQ(engine.applyPull(page, &data).status,
              SyncEngine::PullOutcome::Status::WRONG_BOOK);
}

//--------------------------------------------------------------------------------------------------
// Somebody typed the wrong passphrase. Nothing is written: a book half filled with unreadable
// records would be worse than one that did not synchronize at all.
TEST(sync_engine_test, a_wrong_key_writes_nothing)
{
    SyncEngine engine(deriveSyncKey("wrong passphrase", std::string(kSyncSaltSize, 'z')));
    Data data = makeBook();

    Computer arriving;
    arriving.set_name("workstation");

    proto::BookPull page = makePage(2);
    addEntry(&page, "33333333-3333-4333-8333-333333333333", data.root_group().guid(),
             proto::BOOK_ENTRY_KIND_COMPUTER, 2, arriving.SerializeAsString(), testKey());

    const SyncEngine::PullOutcome outcome = engine.applyPull(page, &data);

    EXPECT_EQ(outcome.status, SyncEngine::PullOutcome::Status::BAD_KEY);
    EXPECT_EQ(data.root_group().computer_size(), 1);
    EXPECT_EQ(data.sync().last_pulled_revision(), 1);
}

//--------------------------------------------------------------------------------------------------
// A console interrupted halfway must ask again from where it was rather than believe it has what
// it never received.
TEST(sync_engine_test, the_revision_moves_only_on_the_last_page)
{
    SyncEngine engine(testKey());
    Data data = makeBook();

    Computer arriving;
    arriving.set_name("workstation");

    proto::BookPull page = makePage(9);
    page.set_has_more(true);
    addEntry(&page, "33333333-3333-4333-8333-333333333333", data.root_group().guid(),
             proto::BOOK_ENTRY_KIND_COMPUTER, 9, arriving.SerializeAsString(), testKey());

    const SyncEngine::PullOutcome outcome = engine.applyPull(page, &data);

    EXPECT_EQ(outcome.status, SyncEngine::PullOutcome::Status::OK);
    EXPECT_EQ(outcome.applied, 1u);

    // The record is in, but the console has not yet earned the right to ask from revision 9.
    EXPECT_EQ(data.root_group().computer_size(), 2);
    EXPECT_EQ(data.sync().last_pulled_revision(), 1);
}

//--------------------------------------------------------------------------------------------------
TEST(sync_engine_test, nothing_to_send_when_nothing_changed)
{
    SyncEngine engine(testKey());
    const Data data = makeBook();

    proto::BookPushRequest request;
    EXPECT_FALSE(engine.buildPush(data, "op-1", &request));
    EXPECT_EQ(request.change_size(), 0);
}

//--------------------------------------------------------------------------------------------------
TEST(sync_engine_test, a_local_edit_is_collected_for_sending)
{
    SyncEngine engine(testKey());
    Data data = makeBook();
    const std::string guid = computerGuid(data);

    data.mutable_root_group()->mutable_computer(0)->set_comment("changed here");
    for (int i = 0; i < data.sync().entry_size(); ++i)
    {
        if (data.sync().entry(i).guid() == guid)
            data.mutable_sync()->mutable_entry(i)->set_dirty(true);
    }

    proto::BookPushRequest request;
    ASSERT_TRUE(engine.buildPush(data, "op-1", &request));

    ASSERT_EQ(request.change_size(), 1);
    EXPECT_EQ(request.change(0).guid(), guid);
    EXPECT_EQ(request.change(0).base_revision(), 1);
    EXPECT_EQ(request.book_guid(), kBookGuid);
    EXPECT_EQ(request.op_id(), "op-1");

    // What goes on the wire is sealed, and the router cannot read it.
    EXPECT_FALSE(request.change(0).payload().empty());
    EXPECT_EQ(request.change(0).payload().find("changed here"), std::string::npos);
}

//--------------------------------------------------------------------------------------------------
TEST(sync_engine_test, a_local_deletion_is_collected_for_sending)
{
    SyncEngine engine(testKey());
    Data data = makeBook();
    const std::string guid = computerGuid(data);

    for (int i = 0; i < data.sync().entry_size(); ++i)
    {
        if (data.sync().entry(i).guid() == guid)
            data.mutable_sync()->mutable_entry(i)->set_deleted(true);
    }
    data.mutable_root_group()->mutable_computer()->Clear();

    proto::BookPushRequest request;
    ASSERT_TRUE(engine.buildPush(data, "op-1", &request));

    ASSERT_EQ(request.change_size(), 1);
    EXPECT_EQ(request.change(0).guid(), guid);
    EXPECT_TRUE(request.change(0).deleted());
}

//--------------------------------------------------------------------------------------------------
TEST(sync_engine_test, an_accepted_change_stops_being_pending)
{
    SyncEngine engine(testKey());
    Data data = makeBook();
    const std::string guid = computerGuid(data);

    data.mutable_root_group()->mutable_computer(0)->set_comment("changed here");
    for (int i = 0; i < data.sync().entry_size(); ++i)
    {
        if (data.sync().entry(i).guid() == guid)
            data.mutable_sync()->mutable_entry(i)->set_dirty(true);
    }

    proto::BookPushResult result;
    result.set_error_code(proto::BOOK_ERROR_CODE_OK);
    result.set_revision(7);

    proto::BookChangeResult* entry = result.add_result();
    entry->set_guid(guid);
    entry->set_status(proto::BOOK_CHANGE_STATUS_OK);

    const SyncEngine::PushOutcome outcome = engine.applyPushResult(result, &data);

    EXPECT_EQ(outcome.accepted, 1u);

    const proto::address_book::SyncEntryState* state = stateOf(data, guid);
    ASSERT_TRUE(state);
    EXPECT_FALSE(state->dirty());
    EXPECT_EQ(state->revision(), 7);
}

//--------------------------------------------------------------------------------------------------
// Refused because somebody got there first, but the two edits touch different fields. It is merged
// straight away and is ready to go out again without troubling anyone.
TEST(sync_engine_test, a_refusal_that_merges_is_ready_to_send_again)
{
    SyncEngine engine(testKey());
    Data data = makeBook();
    const std::string guid = computerGuid(data);

    const Computer original = data.root_group().computer(0);

    data.mutable_root_group()->mutable_computer(0)->set_comment("mine");
    for (int i = 0; i < data.sync().entry_size(); ++i)
    {
        if (data.sync().entry(i).guid() == guid)
            data.mutable_sync()->mutable_entry(i)->set_dirty(true);
    }

    Computer theirs = original;
    theirs.set_password("theirs");

    proto::BookPushResult result;
    result.set_error_code(proto::BOOK_ERROR_CODE_OK);

    proto::BookChangeResult* entry = result.add_result();
    entry->set_guid(guid);
    entry->set_status(proto::BOOK_CHANGE_STATUS_CONFLICT);
    entry->mutable_current()->set_guid(guid);
    entry->mutable_current()->set_revision(5);

    std::string sealed;
    ASSERT_TRUE(sealPayload(testKey(), theirs.SerializeAsString(), &sealed));
    entry->mutable_current()->set_payload(sealed);

    const SyncEngine::PushOutcome outcome = engine.applyPushResult(result, &data);

    EXPECT_EQ(outcome.merged, 1u);
    EXPECT_TRUE(outcome.conflicts.empty());

    const Computer& merged = data.root_group().computer(0);
    EXPECT_EQ(merged.comment(), "mine");
    EXPECT_EQ(merged.password(), "theirs");

    // Built on what the router has now, so the next attempt is not stale.
    const proto::address_book::SyncEntryState* state = stateOf(data, guid);
    ASSERT_TRUE(state);
    EXPECT_TRUE(state->dirty());
    EXPECT_EQ(state->revision(), 5);
}

//--------------------------------------------------------------------------------------------------
TEST(sync_engine_test, a_refusal_that_does_not_merge_waits_for_a_person)
{
    SyncEngine engine(testKey());
    Data data = makeBook();
    const std::string guid = computerGuid(data);

    const Computer original = data.root_group().computer(0);

    data.mutable_root_group()->mutable_computer(0)->set_password("what I set");
    for (int i = 0; i < data.sync().entry_size(); ++i)
    {
        if (data.sync().entry(i).guid() == guid)
            data.mutable_sync()->mutable_entry(i)->set_dirty(true);
    }

    Computer theirs = original;
    theirs.set_password("what they set");

    proto::BookPushResult result;
    result.set_error_code(proto::BOOK_ERROR_CODE_OK);

    proto::BookChangeResult* entry = result.add_result();
    entry->set_guid(guid);
    entry->set_status(proto::BOOK_CHANGE_STATUS_CONFLICT);
    entry->mutable_current()->set_guid(guid);
    entry->mutable_current()->set_revision(5);

    std::string sealed;
    ASSERT_TRUE(sealPayload(testKey(), theirs.SerializeAsString(), &sealed));
    entry->mutable_current()->set_payload(sealed);

    const SyncEngine::PushOutcome outcome = engine.applyPushResult(result, &data);

    EXPECT_EQ(outcome.merged, 0u);
    EXPECT_TRUE(listed(outcome.conflicts, guid));
    EXPECT_EQ(data.root_group().computer(0).password(), "what I set");
}

//--------------------------------------------------------------------------------------------------
// A record the router will never take must stop being pending, or the console would offer it
// again forever.
TEST(sync_engine_test, a_rejected_change_stops_being_pending)
{
    SyncEngine engine(testKey());
    Data data = makeBook();
    const std::string guid = computerGuid(data);

    for (int i = 0; i < data.sync().entry_size(); ++i)
    {
        if (data.sync().entry(i).guid() == guid)
            data.mutable_sync()->mutable_entry(i)->set_dirty(true);
    }

    proto::BookPushResult result;
    result.set_error_code(proto::BOOK_ERROR_CODE_OK);

    proto::BookChangeResult* entry = result.add_result();
    entry->set_guid(guid);
    entry->set_status(proto::BOOK_CHANGE_STATUS_REJECTED);
    entry->set_reason("would_create_cycle");

    const SyncEngine::PushOutcome outcome = engine.applyPushResult(result, &data);

    EXPECT_EQ(outcome.rejected, 1u);
    EXPECT_TRUE(listed(outcome.conflicts, guid));

    const proto::address_book::SyncEntryState* state = stateOf(data, guid);
    ASSERT_TRUE(state);
    EXPECT_FALSE(state->dirty());
}

//--------------------------------------------------------------------------------------------------
TEST(sync_engine_test, an_error_from_the_router_changes_nothing)
{
    SyncEngine engine(testKey());
    Data data = makeBook();

    proto::BookPushResult result;
    result.set_error_code(proto::BOOK_ERROR_CODE_INTERNAL_ERROR);

    const SyncEngine::PushOutcome outcome = engine.applyPushResult(result, &data);

    EXPECT_EQ(outcome.accepted, 0u);
    EXPECT_EQ(outcome.rejected, 0u);
}

//--------------------------------------------------------------------------------------------------
TEST(sync_engine_test, tolerates_null)
{
    SyncEngine engine(testKey());

    EXPECT_EQ(engine.applyPull(makePage(1), nullptr).status,
              SyncEngine::PullOutcome::Status::WRONG_BOOK);

    const Data data = makeBook();
    EXPECT_FALSE(engine.buildPush(data, "op-1", nullptr));
    EXPECT_FALSE(engine.buildPush(data, std::string(), nullptr));
}

} // namespace console
