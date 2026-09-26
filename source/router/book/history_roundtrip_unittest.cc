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
#include "console/book/book_sync.h"
#include "console/book/entry_guid.h"
#include "console/book/sync_key.h"
#include "proto/address_book.pb.h"
#include "proto/router_book.pb.h"
#include "router/book/book_service.h"
#include "router/book/book_store.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <memory>
#include <queue>
#include <string>

namespace {

using console::BookSync;
using console::ChangeDescription;
using console::HistoryBatch;
using console::HistoryJournal;
using console::RollbackCheck;
using console::RollbackPlan;
using console::SyncEngine;
using proto::address_book::Computer;
using proto::address_book::ComputerGroup;
using proto::address_book::Data;

const char kBookGuid[] = "11111111-1111-4111-8111-111111111111";
const char kPassphrase[] = "department passphrase";

// Two consoles and a router, as in sync_roundtrip_unittest.cc: the real store and the real
// service, wired straight to the real exchange, with only the socket left out. What is added here
// is the journal and the way back - fetched, planned and sent exactly as the window does it.
class Wire : public BookSync::Sender, public BookSync::Observer
{
public:
    Wire(router::BookService* service, const std::string& key, const char* who,
         const char* address)
        : service_(service),
          key_(key),
          who_(who),
          address_(address),
          sync_(std::make_unique<BookSync>(key, this, this))
    {
        data_.mutable_root_group()->set_name(who);
        console::ensureEntryGuids(data_.mutable_root_group());

        data_.mutable_sync()->set_book_guid(kBookGuid);
    }

    // Runs an exchange until nothing more is asked for.
    void exchange()
    {
        sync_->start(&data_);
        drain();
    }

    // Everything the router has in its history, page by page, the way the window fetches it.
    // |tamper| gets each page before the console sees it - it stands for whoever holds the
    // router's database.
    void loadHistory(std::function<void(proto::BookHistory*)> tamper = nullptr)
    {
        int64_t before = 0;

        for (int guard = 0; guard < 100; ++guard)
        {
            proto::BookHistoryRequest request;
            request.set_book_guid(kBookGuid);
            request.set_before_revision(before);

            proto::BookHistory page;
            service_->handleHistoryRequest(request, &page);
            ASSERT_EQ(page.error_code(), proto::BOOK_ERROR_CODE_OK);

            if (tamper)
                tamper(&page);

            ASSERT_TRUE(journal_.addPage(page, kBookGuid, before, key_));

            if (!journal_.hasMore())
                return;

            before = journal_.nextBefore();
        }

        FAIL() << who_ << ": the history does not end";
    }

    // What the window does on "roll back": check, write into the book, send.
    RollbackCheck rollback(int64_t to_revision, const std::string& only_guid = std::string(),
                           std::function<void(proto::BookHistory*)> tamper = nullptr,
                           RollbackPlan* out_plan = nullptr)
    {
        loadHistory(tamper);

        RollbackPlan plan;
        const RollbackCheck check =
            console::prepareRollback(journal_, data_, to_revision, only_guid, &plan);

        if (out_plan)
            *out_plan = plan;

        if (check != RollbackCheck::OK)
            return check;

        console::applyRollback(plan, &data_);
        sync_->startRollback(&data_, to_revision);
        drain();
        return check;
    }

    const HistoryJournal& journal() const { return journal_; }

    std::string addGroup(const char* name)
    {
        ComputerGroup* group = data_.mutable_root_group()->add_computer_group();
        group->set_name(name);
        console::ensureEntryGuids(data_.mutable_root_group());
        return group->guid();
    }

    Computer* addComputer(const char* name, const char* address,
                          const std::string& group_guid = std::string())
    {
        ComputerGroup* target = group_guid.empty() ? data_.mutable_root_group()
                                                   : findGroup(group_guid);
        if (!target)
            return nullptr;

        Computer* computer = target->add_computer();
        computer->set_name(name);
        computer->set_address(address);
        console::ensureEntryGuids(data_.mutable_root_group());
        return computer;
    }

    ComputerGroup* findGroup(const std::string& guid)
    {
        ComputerGroup* root = data_.mutable_root_group();
        for (int i = 0; i < root->computer_group_size(); ++i)
        {
            if (root->computer_group(i).guid() == guid)
                return root->mutable_computer_group(i);
        }
        return nullptr;
    }

    bool removeGroup(const std::string& guid)
    {
        ComputerGroup* root = data_.mutable_root_group();
        for (int i = 0; i < root->computer_group_size(); ++i)
        {
            if (root->computer_group(i).guid() != guid)
                continue;

            root->mutable_computer_group()->DeleteSubrange(i, 1);
            return true;
        }
        return false;
    }

    // Anywhere in the book.
    Computer* findComputer(const std::string& name)
    {
        return findIn(data_.mutable_root_group(), name);
    }

    bool removeComputer(const std::string& name)
    {
        return removeIn(data_.mutable_root_group(), name);
    }

    // Every computer in the book, as "name@address", sorted: a picture of the book that does not
    // depend on the order a rebuild put things in.
    std::vector<std::string> picture() const
    {
        std::vector<std::string> out;
        collect(data_.root_group(), std::string(), &out);
        std::sort(out.begin(), out.end());
        return out;
    }

    const std::vector<std::string>& conflicts() const { return sync_->conflicts(); }
    Data& data() { return data_; }

    // BookSync::Sender.
    void sendPull(const proto::BookPullRequest& request) override { pulls_.push(request); }
    void sendPush(const proto::BookPushRequest& request) override { pushes_.push(request); }

    // BookSync::Observer.
    void onBookUpdated() override {}
    void onConflicts(const std::vector<std::string>&) override {}
    void onSyncStopped(SyncEngine::PullOutcome::Status) override {}
    void onInSync(int64_t) override {}

    // The last pull answer, for what the router says about its history.
    const proto::BookPull& lastPull() const { return last_pull_; }

private:
    void drain()
    {
        int guard = 0;
        while (!pulls_.empty() || !pushes_.empty())
        {
            ASSERT_LT(++guard, 100) << who_ << ": the exchange does not settle";

            if (!pushes_.empty())
            {
                proto::BookPushRequest request = pushes_.front();
                pushes_.pop();

                proto::BookPushResult result;
                service_->handlePushRequest(request, who_, &result, address_);
                sync_->onPushResult(result, &data_);
                continue;
            }

            proto::BookPullRequest request = pulls_.front();
            pulls_.pop();

            proto::BookPull page;
            service_->handlePullRequest(request, &page);
            last_pull_ = page;
            sync_->onPull(page, &data_);
        }
    }

    static Computer* findIn(ComputerGroup* group, const std::string& name)
    {
        for (int i = 0; i < group->computer_size(); ++i)
        {
            if (group->computer(i).name() == name)
                return group->mutable_computer(i);
        }

        for (int i = 0; i < group->computer_group_size(); ++i)
        {
            if (Computer* found = findIn(group->mutable_computer_group(i), name))
                return found;
        }

        return nullptr;
    }

    static bool removeIn(ComputerGroup* group, const std::string& name)
    {
        for (int i = 0; i < group->computer_size(); ++i)
        {
            if (group->computer(i).name() != name)
                continue;

            group->mutable_computer()->DeleteSubrange(i, 1);
            return true;
        }

        for (int i = 0; i < group->computer_group_size(); ++i)
        {
            if (removeIn(group->mutable_computer_group(i), name))
                return true;
        }

        return false;
    }

    static void collect(const ComputerGroup& group, const std::string& path,
                        std::vector<std::string>* out)
    {
        for (const Computer& computer : group.computer())
        {
            out->emplace_back(path + computer.name() + "@" + computer.address() + "#" +
                              computer.comment() + "#" + computer.password());
        }

        for (const ComputerGroup& child : group.computer_group())
            collect(child, path + child.name() + "/", out);
    }

    router::BookService* service_;
    const std::string key_;
    const char* who_;
    const char* address_;

    Data data_;
    std::unique_ptr<BookSync> sync_;
    HistoryJournal journal_;

    std::queue<proto::BookPullRequest> pulls_;
    std::queue<proto::BookPushRequest> pushes_;

    proto::BookPull last_pull_;
};

class HistoryRoundtripTest : public testing::Test
{
public:
    void SetUp() override
    {
        static int counter = 0;
        path_ = std::filesystem::temp_directory_path() /
            ("aspia_history_roundtrip_" + std::to_string(++counter) + "_" +
             std::to_string(reinterpret_cast<uintptr_t>(this)) + ".db3");

        std::error_code ignored;
        std::filesystem::remove(path_, ignored);

        store_ = router::BookStore::open(path_);
        ASSERT_TRUE(store_);

        router::BookHistoryPolicy policy;
        policy.days = 30;
        policy.max_changes = 10000;
        store_->setHistoryPolicy(policy);

        salt_ = console::createSyncSalt();
        key_ = console::deriveSyncKey(kPassphrase, salt_);
        ASSERT_FALSE(key_.empty());

        router::Book book;
        book.guid = kBookGuid;
        book.name = "department";
        book.sync_salt = salt_;
        book.key_verifier = console::createKeyVerifier(key_);
        book.epoch = "22222222-2222-4222-8222-222222222222";
        ASSERT_TRUE(store_->createBook(book));

        service_ = std::make_unique<router::BookService>(store_.get());

        vanya_ = std::make_unique<Wire>(service_.get(), key_, "PC-VANYA", "10.0.0.11");
        petya_ = std::make_unique<Wire>(service_.get(), key_, "PC-PETYA", "10.0.0.22");
    }

    void TearDown() override
    {
        vanya_.reset();
        petya_.reset();
        service_.reset();
        store_.reset();

        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

protected:
    int64_t revision() const
    {
        router::Book book;
        EXPECT_TRUE(store_->findBook(kBookGuid, &book));
        return book.revision;
    }

    // Vanya makes a group of three; both have it.
    std::string makeOffice()
    {
        const std::string office = vanya_->addGroup("office");
        vanya_->addComputer("server", "1", office);
        vanya_->addComputer("printer", "2", office);
        vanya_->addComputer("reception", "3", office);
        vanya_->exchange();
        petya_->exchange();
        return office;
    }

    std::filesystem::path path_;
    std::unique_ptr<router::BookStore> store_;
    std::unique_ptr<router::BookService> service_;
    std::string salt_;
    std::string key_;

    std::unique_ptr<Wire> vanya_;
    std::unique_ptr<Wire> petya_;
};

} // namespace

//--------------------------------------------------------------------------------------------------
// What the department asked for: somebody deleted something, and the journal says who and what.
TEST_F(HistoryRoundtripTest, a_deletion_shows_up_in_the_journal_with_its_author)
{
    const std::string office = makeOffice();
    ASSERT_EQ(petya_->picture().size(), 3u);

    ASSERT_TRUE(petya_->removeGroup(office));
    petya_->exchange();
    vanya_->exchange();
    EXPECT_TRUE(vanya_->picture().empty());

    vanya_->loadHistory();
    const std::vector<HistoryBatch>& batches = vanya_->journal().batches();
    ASSERT_FALSE(batches.empty());

    // Newest first: Petya's deletion of the group and everything in it.
    const HistoryBatch& deletion = batches.front();
    EXPECT_EQ(deletion.modified_by, "PC-PETYA");
    EXPECT_EQ(deletion.address, "10.0.0.22");
    EXPECT_GT(deletion.server_time, 0);
    ASSERT_EQ(deletion.changes.size(), 4u);

    std::vector<std::string> names;
    for (const console::HistoryChange& change : deletion.changes)
    {
        const ChangeDescription description = console::describeChange(change);
        EXPECT_EQ(description.action, ChangeDescription::Action::DELETED);
        names.push_back(description.name);
    }
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names, (std::vector<std::string>{ "office", "printer", "reception", "server" }));

    // And before it, Vanya's batch that made them.
    ASSERT_EQ(batches.size(), 2u);
    EXPECT_EQ(batches[1].modified_by, "PC-VANYA");
    EXPECT_EQ(console::describeChange(batches[1].changes[0]).action,
              ChangeDescription::Action::CREATED);
}

//--------------------------------------------------------------------------------------------------
// And the way back: the whole book as it was, on both consoles, and the rollback in the journal.
TEST_F(HistoryRoundtripTest, rolling_back_brings_the_deleted_back_everywhere)
{
    const std::string office = makeOffice();
    const std::vector<std::string> before = vanya_->picture();
    const int64_t good = revision();

    ASSERT_TRUE(petya_->removeGroup(office));
    petya_->exchange();
    vanya_->exchange();
    ASSERT_TRUE(vanya_->picture().empty());

    ASSERT_EQ(vanya_->rollback(good), RollbackCheck::OK);
    EXPECT_EQ(vanya_->picture(), before);
    EXPECT_TRUE(vanya_->conflicts().empty());

    petya_->exchange();
    EXPECT_EQ(petya_->picture(), before);

    // The rollback went out as ordinary edits, and says what it was.
    vanya_->loadHistory();
    const HistoryBatch& newest = vanya_->journal().batches().front();
    EXPECT_EQ(newest.modified_by, "PC-VANYA");
    EXPECT_EQ(newest.rollback_to, good);
    EXPECT_EQ(newest.changes.size(), 4u);
    for (const console::HistoryChange& change : newest.changes)
    {
        EXPECT_EQ(console::describeChange(change).action, ChangeDescription::Action::RESTORED);
    }
}

//--------------------------------------------------------------------------------------------------
// The rollback is in the history like anything else, so it can be undone the same way.
TEST_F(HistoryRoundtripTest, a_rollback_can_itself_be_rolled_back)
{
    const std::string office = makeOffice();
    const int64_t good = revision();

    ASSERT_TRUE(petya_->removeGroup(office));
    petya_->exchange();
    vanya_->exchange();
    const int64_t emptied = revision();

    ASSERT_EQ(vanya_->rollback(good), RollbackCheck::OK);
    petya_->exchange();
    ASSERT_EQ(petya_->picture().size(), 3u);

    ASSERT_EQ(petya_->rollback(emptied), RollbackCheck::OK);
    vanya_->exchange();

    EXPECT_TRUE(petya_->picture().empty());
    EXPECT_TRUE(vanya_->picture().empty());
}

//--------------------------------------------------------------------------------------------------
// One record brought back, and nothing else touched.
TEST_F(HistoryRoundtripTest, one_record_is_put_back_on_its_own)
{
    makeOffice();

    ASSERT_TRUE(petya_->removeComputer("server"));
    ASSERT_TRUE(petya_->removeComputer("printer"));
    petya_->exchange();

    // Meanwhile a new one is added, which the undo must leave alone.
    petya_->addComputer("laptop", "9");
    petya_->exchange();
    vanya_->exchange();

    vanya_->loadHistory();
    const std::vector<HistoryBatch>& batches = vanya_->journal().batches();
    ASSERT_GE(batches.size(), 2u);

    // The batch that deleted the two, and the server's change in it.
    const HistoryBatch& deletion = batches[1];
    std::string server_guid;
    for (const console::HistoryChange& change : deletion.changes)
    {
        if (console::describeChange(change).name == "server")
            server_guid = change.guid;
    }
    ASSERT_FALSE(server_guid.empty());

    ASSERT_EQ(vanya_->rollback(deletion.revision - 1, server_guid), RollbackCheck::OK);
    petya_->exchange();

    const std::vector<std::string> expected = {
        "laptop@9##", "office/reception@3##", "office/server@1##" };
    EXPECT_EQ(vanya_->picture(), expected);
    EXPECT_EQ(petya_->picture(), expected);
}

//--------------------------------------------------------------------------------------------------
// Petya is typing in a field the rollback does not touch. Both survive, without a question.
TEST_F(HistoryRoundtripTest, an_unsent_edit_elsewhere_survives_a_rollback)
{
    vanya_->addComputer("server", "1");
    vanya_->exchange();
    petya_->exchange();
    const int64_t good = revision();

    vanya_->findComputer("server")->set_address("2");
    vanya_->exchange();
    petya_->exchange();

    // Not sent yet.
    petya_->findComputer("server")->set_comment("typing");

    ASSERT_EQ(vanya_->rollback(good), RollbackCheck::OK);
    petya_->exchange();
    vanya_->exchange();

    EXPECT_TRUE(petya_->conflicts().empty());
    EXPECT_EQ(petya_->picture(), (std::vector<std::string>{ "server@1#typing#" }));
    EXPECT_EQ(vanya_->picture(), (std::vector<std::string>{ "server@1#typing#" }));
}

//--------------------------------------------------------------------------------------------------
// And in the same field: put to Petya, never silently written over.
TEST_F(HistoryRoundtripTest, an_unsent_edit_in_the_same_field_becomes_a_question)
{
    vanya_->addComputer("server", "1");
    vanya_->exchange();
    petya_->exchange();
    const int64_t good = revision();

    vanya_->findComputer("server")->set_address("2");
    vanya_->exchange();
    petya_->exchange();

    petya_->findComputer("server")->set_address("3");

    ASSERT_EQ(vanya_->rollback(good), RollbackCheck::OK);
    petya_->exchange();

    EXPECT_EQ(petya_->conflicts().size(), 1u);
    EXPECT_EQ(petya_->findComputer("server")->address(), "3");
    EXPECT_EQ(vanya_->findComputer("server")->address(), "1");
}

//--------------------------------------------------------------------------------------------------
// A console with something of its own still waiting is refused, rather than planning on a book
// that is not what the router has.
TEST_F(HistoryRoundtripTest, a_book_out_of_step_is_not_rolled_back)
{
    vanya_->addComputer("server", "1");
    vanya_->exchange();
    const int64_t good = revision();

    vanya_->addComputer("printer", "2");
    vanya_->exchange();

    // Unsent.
    vanya_->findComputer("server")->set_comment("typing");
    EXPECT_EQ(vanya_->rollback(good), RollbackCheck::NOT_IN_STEP);

    // Behind the router.
    petya_->exchange();
    vanya_->exchange();
    petya_->addComputer("laptop", "3");
    petya_->exchange();
    EXPECT_EQ(vanya_->rollback(good), RollbackCheck::NOT_IN_STEP);

    // Once in step, it goes - and takes back everything after the point, the comment included.
    vanya_->exchange();
    EXPECT_EQ(vanya_->rollback(good), RollbackCheck::OK);
    petya_->exchange();
    EXPECT_EQ(vanya_->picture(), (std::vector<std::string>{ "server@1##" }));
    EXPECT_EQ(petya_->picture(), (std::vector<std::string>{ "server@1##" }));
}

//--------------------------------------------------------------------------------------------------
// A small limit: the history keeps the newest changes, says how far back it reaches, and a point
// older than that is refused.
TEST_F(HistoryRoundtripTest, a_point_older_than_the_history_is_refused)
{
    router::BookHistoryPolicy policy;
    policy.days = 30;
    policy.max_changes = 3;
    store_->setHistoryPolicy(policy);

    vanya_->addComputer("one", "1");
    vanya_->exchange();
    const int64_t first = revision();

    for (int i = 2; i <= 6; ++i)
    {
        vanya_->addComputer(("n" + std::to_string(i)).c_str(), std::to_string(i).c_str());
        vanya_->exchange();
    }

    vanya_->loadHistory();
    EXPECT_EQ(vanya_->journal().batches().size(), 3u);
    EXPECT_EQ(vanya_->journal().oldestRevision(), revision() - 3);

    EXPECT_EQ(vanya_->rollback(first), RollbackCheck::TOO_OLD);
    EXPECT_EQ(vanya_->rollback(revision() - 3), RollbackCheck::OK);
    EXPECT_EQ(vanya_->picture().size(), 3u);
}

//--------------------------------------------------------------------------------------------------
// A history that goes back further than one page is fetched whole before anything is planned.
TEST_F(HistoryRoundtripTest, a_long_history_is_fetched_page_by_page)
{
    vanya_->addComputer("server", "0");
    vanya_->exchange();
    const int64_t good = revision();

    const int kEdits = static_cast<int>(router::BookService::kMaxHistoryBatchesPerPage) + 7;
    for (int i = 1; i <= kEdits; ++i)
    {
        vanya_->findComputer("server")->set_address(std::to_string(i));
        vanya_->exchange();
    }

    ASSERT_EQ(vanya_->rollback(good), RollbackCheck::OK);
    EXPECT_EQ(vanya_->journal().batches().size(), static_cast<size_t>(kEdits + 1));
    EXPECT_EQ(vanya_->findComputer("server")->address(), "0");
}

//--------------------------------------------------------------------------------------------------
// Whoever holds the router's database swaps one sealed payload in the history. That record is not
// written back; the rest is.
TEST_F(HistoryRoundtripTest, a_tampered_history_record_is_not_put_back)
{
    const std::string office = makeOffice();
    const int64_t good = revision();

    std::string server_guid;
    for (const Computer& computer : vanya_->findGroup(office)->computer())
    {
        if (computer.name() == "server")
            server_guid = computer.guid();
    }
    ASSERT_FALSE(server_guid.empty());

    ASSERT_TRUE(vanya_->removeGroup(office));
    vanya_->exchange();

    auto tamper = [&](proto::BookHistory* page)
    {
        for (proto::BookHistoryBatch& batch : *page->mutable_batch())
        {
            for (proto::BookHistoryChange& change : *batch.mutable_change())
            {
                if (change.guid() == server_guid && change.has_before())
                    change.mutable_before()->set_payload("something else entirely");
            }
        }
    };

    RollbackPlan plan;
    ASSERT_EQ(vanya_->rollback(good, std::string(), tamper, &plan), RollbackCheck::OK);
    EXPECT_EQ(plan.unreadable, (std::vector<std::string>{ server_guid }));

    petya_->exchange();

    const std::vector<std::string> expected = { "office/printer@2##", "office/reception@3##" };
    EXPECT_EQ(vanya_->picture(), expected);
    EXPECT_EQ(petya_->picture(), expected);
}

//--------------------------------------------------------------------------------------------------
// The history holds passwords as the book does: sealed. The router cannot read them there either.
TEST_F(HistoryRoundtripTest, what_the_history_holds_cannot_be_read_without_the_key)
{
    Computer* server = vanya_->addComputer("server", "12345");
    server->set_password("hunter2");
    vanya_->exchange();

    ASSERT_TRUE(vanya_->removeComputer("server"));
    vanya_->exchange();

    std::vector<router::BookHistoryBatch> batches;
    bool has_more = false;
    ASSERT_TRUE(store_->historyBefore(kBookGuid, 0, 10, 0, &batches, &has_more));
    ASSERT_EQ(batches.size(), 2u);

    for (const router::BookHistoryBatch& batch : batches)
    {
        for (const router::BookHistoryChange& change : batch.changes)
        {
            EXPECT_EQ(change.before.payload.find("hunter2"), std::string::npos);
            EXPECT_EQ(change.after.payload.find("hunter2"), std::string::npos);
            EXPECT_EQ(change.before.payload.find("server"), std::string::npos);
        }
    }

    // And the deleted one is still there to be put back, sealed.
    EXPECT_FALSE(batches[0].changes[0].before.payload.empty());
}

//--------------------------------------------------------------------------------------------------
// Switched off on the router: nothing is written, and every pull says so, so the console does not
// offer what it cannot do.
TEST_F(HistoryRoundtripTest, a_router_without_a_history_says_so)
{
    store_->setHistoryPolicy(router::BookHistoryPolicy());

    vanya_->addComputer("server", "1");
    vanya_->exchange();

    EXPECT_EQ(vanya_->lastPull().history_days(), 0);

    proto::BookHistoryRequest request;
    request.set_book_guid(kBookGuid);

    proto::BookHistory page;
    service_->handleHistoryRequest(request, &page);
    ASSERT_EQ(page.error_code(), proto::BOOK_ERROR_CODE_OK);
    EXPECT_EQ(page.batch_size(), 0);
    EXPECT_EQ(page.history_days(), 0);
    EXPECT_EQ(page.oldest_revision(), page.revision());
}

//--------------------------------------------------------------------------------------------------
TEST_F(HistoryRoundtripTest, every_pull_says_how_long_the_history_is_kept)
{
    vanya_->addComputer("server", "1");
    vanya_->exchange();

    EXPECT_EQ(vanya_->lastPull().history_days(), 30);
    EXPECT_EQ(vanya_->lastPull().history_max_changes(), 10000);
}

//--------------------------------------------------------------------------------------------------
// What arrives is treated as hostile, the history request as much as any other.
TEST_F(HistoryRoundtripTest, a_malformed_history_request_is_refused)
{
    proto::BookHistory page;

    proto::BookHistoryRequest negative;
    negative.set_book_guid(kBookGuid);
    negative.set_before_revision(-1);
    service_->handleHistoryRequest(negative, &page);
    EXPECT_EQ(page.error_code(), proto::BOOK_ERROR_CODE_INVALID_REQUEST);

    proto::BookHistoryRequest negative_count;
    negative_count.set_book_guid(kBookGuid);
    negative_count.set_count(-5);
    page.Clear();
    service_->handleHistoryRequest(negative_count, &page);
    EXPECT_EQ(page.error_code(), proto::BOOK_ERROR_CODE_INVALID_REQUEST);

    proto::BookHistoryRequest long_guid;
    long_guid.set_book_guid(std::string(1000, 'a'));
    page.Clear();
    service_->handleHistoryRequest(long_guid, &page);
    EXPECT_EQ(page.error_code(), proto::BOOK_ERROR_CODE_INVALID_REQUEST);

    proto::BookHistoryRequest unknown;
    unknown.set_book_guid("33333333-3333-4333-8333-333333333333");
    page.Clear();
    service_->handleHistoryRequest(unknown, &page);
    EXPECT_EQ(page.error_code(), proto::BOOK_ERROR_CODE_NOT_FOUND);
}

//--------------------------------------------------------------------------------------------------
// Asking for a million batches gets a page of the usual size.
TEST_F(HistoryRoundtripTest, a_page_of_the_history_is_capped_whatever_is_asked)
{
    const int kBatches = static_cast<int>(router::BookService::kMaxHistoryBatchesPerPage) + 5;
    for (int i = 0; i < kBatches; ++i)
    {
        vanya_->addComputer(("n" + std::to_string(i)).c_str(), "1");
        vanya_->exchange();
    }

    proto::BookHistoryRequest request;
    request.set_book_guid(kBookGuid);
    request.set_count(1000000);

    proto::BookHistory page;
    service_->handleHistoryRequest(request, &page);
    ASSERT_EQ(page.error_code(), proto::BOOK_ERROR_CODE_OK);
    EXPECT_EQ(page.batch_size(), router::BookService::kMaxHistoryBatchesPerPage);
    EXPECT_TRUE(page.has_more());
}

//--------------------------------------------------------------------------------------------------
// The label is only a label. A console cannot make the history claim the book went back to a
// point it never was at, and a negative one is refused outright.
TEST_F(HistoryRoundtripTest, a_forged_rollback_label_is_not_believed)
{
    vanya_->addComputer("server", "1");
    vanya_->exchange();

    proto::BookPushRequest push;
    push.set_book_guid(kBookGuid);
    push.set_op_id("44444444-4444-4444-8444-444444444444");
    push.set_rollback_to_revision(-3);

    proto::BookPushResult result;
    service_->handlePushRequest(push, "PC-EVIL", &result, "10.6.6.6");
    EXPECT_EQ(result.error_code(), proto::BOOK_ERROR_CODE_INVALID_REQUEST);

    vanya_->findComputer("server")->set_address("2");
    vanya_->exchange();

    // A far-future label on an ordinary batch is dropped.
    std::vector<router::BookChange> changes(1);
    changes[0].guid = vanya_->findComputer("server")->guid();
    changes[0].kind = router::BookEntry::Kind::COMPUTER;
    changes[0].base_revision = revision();
    changes[0].payload = "sealed elsewhere";

    std::vector<router::BookChangeResult> results;
    int64_t new_revision = 0;
    ASSERT_TRUE(store_->applyChanges(kBookGuid, "55555555-5555-4555-8555-555555555555",
                                     "PC-EVIL", changes, &results, &new_revision,
                                     router::BookBatchNote{ "10.6.6.6", 1000 }));

    std::vector<router::BookHistoryBatch> batches;
    bool has_more = false;
    ASSERT_TRUE(store_->historyBefore(kBookGuid, 0, 1, 0, &batches, &has_more));
    ASSERT_EQ(batches.size(), 1u);
    EXPECT_EQ(batches[0].rollback_to, 0);
    EXPECT_EQ(batches[0].modified_by, "PC-EVIL");
    EXPECT_EQ(batches[0].address, "10.6.6.6");
}
