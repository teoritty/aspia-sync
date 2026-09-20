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
#include "console/book/sync_key.h"
#include "proto/address_book.pb.h"
#include "proto/router_book.pb.h"
#include "router/book/book_service.h"
#include "router/book/book_store.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <queue>
#include <string>

namespace {

using console::BookSync;
using console::SyncEngine;
using proto::address_book::Computer;
using proto::address_book::ComputerGroup;
using proto::address_book::Data;

const char kBookGuid[] = "11111111-1111-4111-8111-111111111111";
const char kPassphrase[] = "department passphrase";

// Two consoles and a router, wired to each other directly.
//
// The socket is left out on purpose: it is proven by hand against a running router - the
// connection, the handshake, the channel, asking for the list of books and creating one all go
// over a real network. What a test can do that hand-work cannot is run the whole exchange over
// and over, in every order, and say the same thing every time. So everything but the wire is
// here, and it is the real store and the real service, not stand-ins for them.
class Wire : public BookSync::Sender, public BookSync::Observer
{
public:
    Wire(router::BookService* service, const std::string& key, const char* who)
        : service_(service),
          who_(who),
          sync_(std::make_unique<BookSync>(key, this, this))
    {
        data_.mutable_root_group()->set_name(who);
        console::ensureEntryGuids(data_.mutable_root_group());

        data_.mutable_sync()->set_book_guid(kBookGuid);
    }

    // Runs the exchange until nothing more is asked for. The requests are queued rather than
    // served as they are made, so an answer never arrives inside the call that asked for it.
    void exchange()
    {
        sync_->start(&data_);

        int guard = 0;
        while (!pulls_.empty() || !pushes_.empty())
        {
            ASSERT_LT(++guard, 100) << who_ << ": the exchange does not settle";

            if (!pushes_.empty())
            {
                proto::BookPushRequest request = pushes_.front();
                pushes_.pop();

                proto::BookPushResult result;
                service_->handlePushRequest(request, who_, &result);
                sync_->onPushResult(result, &data_);
                continue;
            }

            proto::BookPullRequest request = pulls_.front();
            pulls_.pop();

            proto::BookPull page;
            service_->handlePullRequest(request, &page);
            sync_->onPull(page, &data_);
        }
    }

    Computer* addComputer(const char* name, const char* address)
    {
        Computer* computer = data_.mutable_root_group()->add_computer();
        computer->set_name(name);
        computer->set_address(address);
        console::ensureEntryGuids(data_.mutable_root_group());
        return computer;
    }

    Computer* findComputer(const std::string& name)
    {
        ComputerGroup* root = data_.mutable_root_group();
        for (int i = 0; i < root->computer_size(); ++i)
        {
            if (root->computer(i).name() == name)
                return root->mutable_computer(i);
        }
        return nullptr;
    }

    bool removeComputer(const std::string& name)
    {
        ComputerGroup* root = data_.mutable_root_group();
        for (int i = 0; i < root->computer_size(); ++i)
        {
            if (root->computer(i).name() != name)
                continue;

            root->mutable_computer()->DeleteSubrange(i, 1);
            return true;
        }
        return false;
    }

    bool resolve(const std::string& guid, bool keep_local)
    {
        return sync_->resolveConflict(guid, keep_local, &data_);
    }

    int computerCount() const { return data_.root_group().computer_size(); }
    const std::vector<std::string>& conflicts() const { return sync_->conflicts(); }
    Data& data() { return data_; }

    // BookSync::Sender.
    void sendPull(const proto::BookPullRequest& request) override { pulls_.push(request); }
    void sendPush(const proto::BookPushRequest& request) override { pushes_.push(request); }

    // BookSync::Observer.
    void onBookUpdated() override { ++updates_; }
    void onConflicts(const std::vector<std::string>&) override {}
    void onSyncStopped(SyncEngine::PullOutcome::Status reason) override { stop_reason_ = reason; }
    void onInSync(int64_t) override {}

    int updates() const { return updates_; }
    SyncEngine::PullOutcome::Status stopReason() const { return stop_reason_; }

private:
    router::BookService* service_;
    const char* who_;

    Data data_;
    std::unique_ptr<BookSync> sync_;

    std::queue<proto::BookPullRequest> pulls_;
    std::queue<proto::BookPushRequest> pushes_;

    int updates_ = 0;
    SyncEngine::PullOutcome::Status stop_reason_ = SyncEngine::PullOutcome::Status::OK;
};

class SyncRoundtripTest : public testing::Test
{
public:
    void SetUp() override
    {
        static int counter = 0;
        path_ = std::filesystem::temp_directory_path() /
            ("aspia_roundtrip_" + std::to_string(++counter) + "_" +
             std::to_string(reinterpret_cast<uintptr_t>(this)) + ".db3");

        std::error_code ignored;
        std::filesystem::remove(path_, ignored);

        store_ = router::BookStore::open(path_);
        ASSERT_TRUE(store_);

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

        vanya_ = std::make_unique<Wire>(service_.get(), key_, "PC-VANYA");
        petya_ = std::make_unique<Wire>(service_.get(), key_, "PC-PETYA");
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
// The thing the department asked for: one adds a computer, the other has it.
TEST_F(SyncRoundtripTest, a_computer_added_by_one_reaches_the_other)
{
    vanya_->addComputer("server", "12345");
    vanya_->exchange();

    EXPECT_EQ(petya_->computerCount(), 0);

    petya_->exchange();

    ASSERT_EQ(petya_->computerCount(), 1);
    EXPECT_EQ(petya_->data().root_group().computer(0).name(), "server");
    EXPECT_EQ(petya_->data().root_group().computer(0).address(), "12345");
}

//--------------------------------------------------------------------------------------------------
TEST_F(SyncRoundtripTest, what_the_router_holds_cannot_be_read_without_the_key)
{
    Computer* computer = vanya_->addComputer("server", "12345");
    computer->set_password("hunter2");
    vanya_->exchange();

    std::vector<router::BookEntry> entries;
    ASSERT_TRUE(store_->entriesSince(kBookGuid, 0, 0, 0, &entries));
    ASSERT_FALSE(entries.empty());

    for (const router::BookEntry& entry : entries)
    {
        EXPECT_EQ(entry.payload.find("hunter2"), std::string::npos);
        EXPECT_EQ(entry.payload.find("server"), std::string::npos);
    }
}

//--------------------------------------------------------------------------------------------------
TEST_F(SyncRoundtripTest, an_edit_by_one_reaches_the_other)
{
    vanya_->addComputer("server", "12345");
    vanya_->exchange();
    petya_->exchange();

    vanya_->findComputer("server")->set_comment("called about the printer");
    vanya_->exchange();
    petya_->exchange();

    ASSERT_EQ(petya_->computerCount(), 1);
    EXPECT_EQ(petya_->data().root_group().computer(0).comment(), "called about the printer");
}

//--------------------------------------------------------------------------------------------------
// Both edit the same computer, in different fields, without either of them being asked anything.
TEST_F(SyncRoundtripTest, edits_to_different_fields_both_survive)
{
    vanya_->addComputer("server", "12345");
    vanya_->exchange();
    petya_->exchange();

    vanya_->findComputer("server")->set_comment("from vanya");
    petya_->findComputer("server")->set_password("from petya");

    vanya_->exchange();
    petya_->exchange();
    vanya_->exchange();

    EXPECT_TRUE(vanya_->conflicts().empty());
    EXPECT_TRUE(petya_->conflicts().empty());

    EXPECT_EQ(vanya_->findComputer("server")->comment(), "from vanya");
    EXPECT_EQ(vanya_->findComputer("server")->password(), "from petya");

    EXPECT_EQ(petya_->findComputer("server")->comment(), "from vanya");
    EXPECT_EQ(petya_->findComputer("server")->password(), "from petya");
}

//--------------------------------------------------------------------------------------------------
// And the case that has to be put to a person: the same field, changed to different things.
TEST_F(SyncRoundtripTest, the_same_field_changed_by_both_is_reported)
{
    vanya_->addComputer("server", "12345");
    vanya_->exchange();
    petya_->exchange();

    vanya_->findComputer("server")->set_password("what vanya set");
    petya_->findComputer("server")->set_password("what petya set");

    vanya_->exchange();
    petya_->exchange();

    EXPECT_FALSE(petya_->conflicts().empty());

    // Until Petya decides, what he sees is his own value.
    EXPECT_EQ(petya_->findComputer("server")->password(), "what petya set");

    // And Vanya's went through, because he got there first.
    EXPECT_EQ(vanya_->findComputer("server")->password(), "what vanya set");
}

//--------------------------------------------------------------------------------------------------
TEST_F(SyncRoundtripTest, a_deletion_reaches_the_other)
{
    vanya_->addComputer("server", "12345");
    vanya_->addComputer("reception", "54321");
    vanya_->exchange();
    petya_->exchange();
    ASSERT_EQ(petya_->computerCount(), 2);

    ASSERT_TRUE(vanya_->removeComputer("server"));
    vanya_->exchange();
    petya_->exchange();

    ASSERT_EQ(petya_->computerCount(), 1);
    EXPECT_EQ(petya_->data().root_group().computer(0).name(), "reception");
}

//--------------------------------------------------------------------------------------------------
// Petya was away while Vanya worked. Everything that happened meanwhile arrives at once.
TEST_F(SyncRoundtripTest, everything_missed_while_away_arrives_at_once)
{
    vanya_->addComputer("one", "1");
    vanya_->exchange();

    vanya_->addComputer("two", "2");
    vanya_->exchange();

    vanya_->addComputer("three", "3");
    vanya_->exchange();

    petya_->exchange();

    EXPECT_EQ(petya_->computerCount(), 3);
}

//--------------------------------------------------------------------------------------------------
// Edited on both sides while one of them was away. Neither loses their work.
TEST_F(SyncRoundtripTest, an_edit_made_offline_is_not_lost)
{
    vanya_->addComputer("server", "12345");
    vanya_->exchange();
    petya_->exchange();

    // Petya, with no connection, adds one of his own.
    petya_->addComputer("laptop", "999");

    // Vanya carries on meanwhile.
    vanya_->addComputer("printer", "777");
    vanya_->exchange();

    // Petya comes back.
    petya_->exchange();

    EXPECT_EQ(petya_->computerCount(), 3);

    vanya_->exchange();
    EXPECT_EQ(vanya_->computerCount(), 3);
}

//--------------------------------------------------------------------------------------------------
// Nothing changed anywhere: an exchange must not invent work for itself.
TEST_F(SyncRoundtripTest, an_exchange_with_nothing_to_do_changes_nothing)
{
    vanya_->addComputer("server", "12345");
    vanya_->exchange();
    petya_->exchange();

    int64_t before = 0;
    router::Book book;
    ASSERT_TRUE(store_->findBook(kBookGuid, &book));
    before = book.revision;

    vanya_->exchange();
    petya_->exchange();

    ASSERT_TRUE(store_->findBook(kBookGuid, &book));
    EXPECT_EQ(book.revision, before);
}

//--------------------------------------------------------------------------------------------------
// The passphrase is wrong. Nothing is taken into the book rather than a book half full of records
// nobody can read.
TEST_F(SyncRoundtripTest, a_console_with_the_wrong_passphrase_takes_nothing)
{
    vanya_->addComputer("server", "12345");
    vanya_->exchange();

    Wire stranger(service_.get(), console::deriveSyncKey("wrong passphrase", salt_), "PC-STRANGER");
    stranger.exchange();

    EXPECT_EQ(stranger.computerCount(), 0);
    EXPECT_EQ(stranger.stopReason(), SyncEngine::PullOutcome::Status::BAD_KEY);
}

//--------------------------------------------------------------------------------------------------
// A book of several hundred records goes over in pieces, and every piece has to arrive.
TEST_F(SyncRoundtripTest, a_large_book_arrives_whole)
{
    const int kCount = router::BookService::kMaxEntriesPerPage * 2 + 17;

    for (int i = 0; i < kCount; ++i)
        vanya_->addComputer(("machine " + std::to_string(i)).c_str(), std::to_string(i).c_str());

    vanya_->exchange();
    petya_->exchange();

    EXPECT_EQ(petya_->computerCount(), kCount);
}

//--------------------------------------------------------------------------------------------------
// A conflict must not put the console in a loop: it would send the record, be refused, fail to
// merge the refusal and send it again, for as long as it stayed open.
TEST_F(SyncRoundtripTest, a_conflict_stops_being_sent_until_it_is_answered)
{
    vanya_->addComputer("server", "12345");
    vanya_->exchange();
    petya_->exchange();

    vanya_->findComputer("server")->set_password("what vanya set");
    petya_->findComputer("server")->set_password("what petya set");

    vanya_->exchange();
    petya_->exchange();

    ASSERT_EQ(petya_->conflicts().size(), 1u);

    router::Book book;
    ASSERT_TRUE(store_->findBook(kBookGuid, &book));
    const int64_t settled = book.revision;

    // Petya carries on working. The unanswered question does not follow him around.
    petya_->addComputer("printer", "777");
    petya_->exchange();
    vanya_->exchange();

    EXPECT_EQ(vanya_->computerCount(), 2);
    EXPECT_EQ(petya_->conflicts().size(), 1u);

    ASSERT_TRUE(store_->findBook(kBookGuid, &book));
    EXPECT_GT(book.revision, settled); // The printer went through.
}

//--------------------------------------------------------------------------------------------------
TEST_F(SyncRoundtripTest, keeping_your_own_version_sends_it)
{
    vanya_->addComputer("server", "12345");
    vanya_->exchange();
    petya_->exchange();

    vanya_->findComputer("server")->set_password("what vanya set");
    petya_->findComputer("server")->set_password("what petya set");

    vanya_->exchange();
    petya_->exchange();

    ASSERT_EQ(petya_->conflicts().size(), 1u);
    const std::string guid = petya_->conflicts().front();

    ASSERT_TRUE(petya_->resolve(guid, true));
    petya_->exchange();

    EXPECT_TRUE(petya_->conflicts().empty());
    EXPECT_EQ(petya_->findComputer("server")->password(), "what petya set");

    vanya_->exchange();
    EXPECT_EQ(vanya_->findComputer("server")->password(), "what petya set");
}

//--------------------------------------------------------------------------------------------------
TEST_F(SyncRoundtripTest, taking_their_version_gives_up_your_own)
{
    vanya_->addComputer("server", "12345");
    vanya_->exchange();
    petya_->exchange();

    vanya_->findComputer("server")->set_password("what vanya set");
    petya_->findComputer("server")->set_password("what petya set");

    vanya_->exchange();
    petya_->exchange();

    ASSERT_EQ(petya_->conflicts().size(), 1u);
    const std::string guid = petya_->conflicts().front();

    ASSERT_TRUE(petya_->resolve(guid, false));
    petya_->exchange();

    EXPECT_TRUE(petya_->conflicts().empty());
    EXPECT_EQ(petya_->findComputer("server")->password(), "what vanya set");

    // And nothing of Petya's went out, so Vanya still has what he had.
    vanya_->exchange();
    EXPECT_EQ(vanya_->findComputer("server")->password(), "what vanya set");
}

//--------------------------------------------------------------------------------------------------
// One deletes a computer while the other is editing it. Neither outcome is obviously right, so it
// is put to the person - and both answers have to work.
TEST_F(SyncRoundtripTest, deleted_there_and_edited_here_is_put_to_the_person)
{
    vanya_->addComputer("server", "12345");
    vanya_->exchange();
    petya_->exchange();

    ASSERT_TRUE(vanya_->removeComputer("server"));
    petya_->findComputer("server")->set_comment("still in use");

    vanya_->exchange();
    petya_->exchange();

    ASSERT_EQ(petya_->conflicts().size(), 1u);
    const std::string guid = petya_->conflicts().front();

    // Petya says it is still in use.
    ASSERT_TRUE(petya_->resolve(guid, true));
    petya_->exchange();

    EXPECT_TRUE(petya_->conflicts().empty());
    ASSERT_EQ(petya_->computerCount(), 1);

    vanya_->exchange();
    ASSERT_EQ(vanya_->computerCount(), 1);
    EXPECT_EQ(vanya_->findComputer("server")->comment(), "still in use");
}

//--------------------------------------------------------------------------------------------------
TEST_F(SyncRoundtripTest, letting_their_deletion_through_removes_it_here_too)
{
    vanya_->addComputer("server", "12345");
    vanya_->addComputer("reception", "54321");
    vanya_->exchange();
    petya_->exchange();

    ASSERT_TRUE(vanya_->removeComputer("server"));
    petya_->findComputer("server")->set_comment("still in use");

    vanya_->exchange();
    petya_->exchange();

    ASSERT_EQ(petya_->conflicts().size(), 1u);

    ASSERT_TRUE(petya_->resolve(petya_->conflicts().front(), false));
    petya_->exchange();

    EXPECT_TRUE(petya_->conflicts().empty());
    ASSERT_EQ(petya_->computerCount(), 1);
    EXPECT_EQ(petya_->data().root_group().computer(0).name(), "reception");

    vanya_->exchange();
    EXPECT_EQ(vanya_->computerCount(), 1);
}
