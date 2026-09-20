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

#include "router/book/book_store.h"

#include "base/guid.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <limits>
#include <set>
#include <string>

namespace router {

namespace {

const char kBookGuid[] = "11111111-1111-4111-8111-111111111111";

std::string guid()
{
    return base::Guid::create().toStdString();
}

// A database file of its own per test, removed afterwards. The store is checked against sqlite,
// not against a stand-in for it: what is being tested is whether transactions, conflicts and the
// unique keys behave, and a fake would only agree with itself.
class BookStoreTest : public testing::Test
{
public:
    void SetUp() override
    {
        static int counter = 0;
        path_ = std::filesystem::temp_directory_path() /
            ("aspia_book_store_test_" + std::to_string(++counter) + "_" +
             std::to_string(reinterpret_cast<uintptr_t>(this)) + ".db3");

        std::error_code error_code;
        std::filesystem::remove(path_, error_code);

        store_ = BookStore::open(path_);
        ASSERT_TRUE(store_);

        Book book;
        book.guid = kBookGuid;
        book.name = "department";
        book.sync_salt = std::string(32, 's');
        book.key_verifier = "verifier";
        book.epoch = guid();
        ASSERT_TRUE(store_->createBook(book));
    }

    void TearDown() override
    {
        store_.reset();

        std::error_code error_code;
        std::filesystem::remove(path_, error_code);
    }

protected:
    // Adds a record and returns the revision the book ended up at.
    int64_t put(const std::string& entry_guid, const std::string& parent,
                BookEntry::Kind kind, int64_t base_revision, const std::string& payload,
                BookChangeResult::Status expected = BookChangeResult::Status::OK)
    {
        BookChange change;
        change.guid = entry_guid;
        change.parent_guid = parent;
        change.kind = kind;
        change.base_revision = base_revision;
        change.payload = payload;

        std::vector<BookChangeResult> results;
        int64_t revision = 0;

        EXPECT_TRUE(store_->applyChanges(kBookGuid, guid(), "PC-TEST", { change },
                                         &results, &revision));
        EXPECT_EQ(results.size(), 1u);
        if (!results.empty())
            EXPECT_EQ(results.front().status, expected) << "reason: " << results.front().reason;

        return revision;
    }

    std::unique_ptr<BookStore> store_;
    std::filesystem::path path_;
};

} // namespace

//--------------------------------------------------------------------------------------------------
TEST_F(BookStoreTest, book_is_stored_and_read_back)
{
    Book book;
    ASSERT_TRUE(store_->findBook(kBookGuid, &book));

    EXPECT_EQ(book.name, "department");
    EXPECT_EQ(book.sync_salt, std::string(32, 's'));
    EXPECT_EQ(book.key_verifier, "verifier");
    EXPECT_EQ(book.revision, 0);
    EXPECT_FALSE(book.epoch.empty());
}

//--------------------------------------------------------------------------------------------------
TEST_F(BookStoreTest, a_book_guid_is_taken_once)
{
    Book book;
    book.guid = kBookGuid;
    book.sync_salt = std::string(32, 'x');
    book.key_verifier = "verifier";
    book.epoch = guid();

    EXPECT_FALSE(store_->createBook(book));
}

//--------------------------------------------------------------------------------------------------
TEST_F(BookStoreTest, refuses_a_book_without_a_salt_or_a_verifier)
{
    Book book;
    book.guid = guid();
    book.epoch = guid();
    book.key_verifier = "verifier";
    EXPECT_FALSE(store_->createBook(book));

    book.sync_salt = std::string(32, 'x');
    book.key_verifier.clear();
    EXPECT_FALSE(store_->createBook(book));
}

//--------------------------------------------------------------------------------------------------
TEST_F(BookStoreTest, first_change_moves_the_revision_to_one)
{
    EXPECT_EQ(put(guid(), std::string(), BookEntry::Kind::GROUP, 0, "root"), 1);

    Book book;
    ASSERT_TRUE(store_->findBook(kBookGuid, &book));
    EXPECT_EQ(book.revision, 1);
}

//--------------------------------------------------------------------------------------------------
TEST_F(BookStoreTest, a_record_is_read_back_as_it_was_written)
{
    const std::string root = guid();
    const std::string computer = guid();

    put(root, std::string(), BookEntry::Kind::GROUP, 0, "root payload");
    put(computer, root, BookEntry::Kind::COMPUTER, 0, "sealed bytes");

    BookEntry entry;
    ASSERT_TRUE(store_->findEntry(kBookGuid, computer, &entry));

    EXPECT_EQ(entry.guid, computer);
    EXPECT_EQ(entry.parent_guid, root);
    EXPECT_EQ(entry.kind, BookEntry::Kind::COMPUTER);
    EXPECT_EQ(entry.payload, "sealed bytes");
    EXPECT_EQ(entry.modified_by, "PC-TEST");
    EXPECT_FALSE(entry.deleted);
    EXPECT_GT(entry.server_time, 0);
}

//--------------------------------------------------------------------------------------------------
// The whole point of the revision: a change built on a version that is no longer current is
// refused rather than quietly overwriting what somebody else wrote in between.
TEST_F(BookStoreTest, a_stale_change_is_refused)
{
    const std::string entry_guid = guid();

    const int64_t first = put(entry_guid, std::string(), BookEntry::Kind::GROUP, 0, "one");
    put(entry_guid, std::string(), BookEntry::Kind::GROUP, first, "two");

    // Somebody who still thinks the record is at the first revision.
    BookChange change;
    change.guid = entry_guid;
    change.base_revision = first;
    change.payload = "three";

    std::vector<BookChangeResult> results;
    int64_t revision = 0;
    ASSERT_TRUE(store_->applyChanges(kBookGuid, guid(), "PC-OTHER", { change },
                                     &results, &revision));

    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results.front().status, BookChangeResult::Status::CONFLICT);

    // The current record comes back with the refusal, so the console can merge and try again.
    EXPECT_EQ(results.front().current.payload, "two");

    BookEntry entry;
    ASSERT_TRUE(store_->findEntry(kBookGuid, entry_guid, &entry));
    EXPECT_EQ(entry.payload, "two");
}

//--------------------------------------------------------------------------------------------------
// Seven people editing seven different records at the same time all get through: the check is per
// record, not per book.
TEST_F(BookStoreTest, changes_to_different_records_do_not_conflict)
{
    const std::string root = guid();
    put(root, std::string(), BookEntry::Kind::GROUP, 0, "root");

    for (int i = 0; i < 7; ++i)
        put(guid(), root, BookEntry::Kind::COMPUTER, 0, "payload");

    int64_t count = 0;
    ASSERT_TRUE(store_->entryCount(kBookGuid, &count));
    EXPECT_EQ(count, 8);
}

//--------------------------------------------------------------------------------------------------
TEST_F(BookStoreTest, a_new_record_claiming_a_revision_is_refused)
{
    BookChange change;
    change.guid = guid();
    change.base_revision = 5; // Nothing is there, so nothing can be at revision 5.
    change.payload = "payload";

    std::vector<BookChangeResult> results;
    int64_t revision = 0;
    ASSERT_TRUE(store_->applyChanges(kBookGuid, guid(), "PC-TEST", { change },
                                     &results, &revision));

    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results.front().status, BookChangeResult::Status::CONFLICT);
}

//--------------------------------------------------------------------------------------------------
// A console whose connection dropped before the answer arrived does not know whether its batch
// was applied. Sending it again must not apply it twice.
TEST_F(BookStoreTest, repeating_a_batch_applies_it_once)
{
    const std::string entry_guid = guid();
    const std::string op = guid();

    BookChange change;
    change.guid = entry_guid;
    change.base_revision = 0;
    change.payload = "payload";

    std::vector<BookChangeResult> first;
    int64_t first_revision = 0;
    ASSERT_TRUE(store_->applyChanges(kBookGuid, op, "PC-TEST", { change },
                                     &first, &first_revision));
    ASSERT_EQ(first.size(), 1u);
    ASSERT_EQ(first.front().status, BookChangeResult::Status::OK);

    std::vector<BookChangeResult> second;
    int64_t second_revision = 0;
    ASSERT_TRUE(store_->applyChanges(kBookGuid, op, "PC-TEST", { change },
                                     &second, &second_revision));

    EXPECT_EQ(second_revision, first_revision);
    ASSERT_EQ(second.size(), 1u);
    EXPECT_EQ(second.front().status, BookChangeResult::Status::OK);

    Book book;
    ASSERT_TRUE(store_->findBook(kBookGuid, &book));
    EXPECT_EQ(book.revision, first_revision);

    int64_t count = 0;
    ASSERT_TRUE(store_->entryCount(kBookGuid, &count));
    EXPECT_EQ(count, 1);
}

//--------------------------------------------------------------------------------------------------
// Two people, both offline: one moves A into B, the other moves B into A. Applied as asked, the
// two would hang off each other and take every computer in them out of the book.
TEST_F(BookStoreTest, a_move_that_would_make_a_cycle_is_refused)
{
    const std::string root = guid();
    const std::string a = guid();
    const std::string b = guid();

    put(root, std::string(), BookEntry::Kind::GROUP, 0, "root");
    put(a, root, BookEntry::Kind::GROUP, 0, "a");
    put(b, root, BookEntry::Kind::GROUP, 0, "b");

    BookEntry entry_a;
    BookEntry entry_b;
    ASSERT_TRUE(store_->findEntry(kBookGuid, a, &entry_a));
    ASSERT_TRUE(store_->findEntry(kBookGuid, b, &entry_b));

    // A goes into B: fine.
    put(a, b, BookEntry::Kind::GROUP, entry_a.revision, "a");

    // B into A would close the loop.
    put(b, a, BookEntry::Kind::GROUP, entry_b.revision, "b",
        BookChangeResult::Status::REJECTED);

    BookEntry after;
    ASSERT_TRUE(store_->findEntry(kBookGuid, b, &after));
    EXPECT_EQ(after.parent_guid, root);
}

//--------------------------------------------------------------------------------------------------
TEST_F(BookStoreTest, a_record_cannot_be_its_own_parent)
{
    const std::string entry_guid = guid();
    put(entry_guid, entry_guid, BookEntry::Kind::GROUP, 0, "payload",
        BookChangeResult::Status::REJECTED);
}

//--------------------------------------------------------------------------------------------------
TEST_F(BookStoreTest, refuses_a_guid_that_is_not_one)
{
    put("not-a-guid", std::string(), BookEntry::Kind::GROUP, 0, "payload",
        BookChangeResult::Status::REJECTED);
    put(guid(), "also-not-a-guid", BookEntry::Kind::GROUP, 0, "payload",
        BookChangeResult::Status::REJECTED);
}

//--------------------------------------------------------------------------------------------------
TEST_F(BookStoreTest, refuses_a_record_without_a_payload)
{
    put(guid(), std::string(), BookEntry::Kind::GROUP, 0, std::string(),
        BookChangeResult::Status::REJECTED);
}

//--------------------------------------------------------------------------------------------------
// A console that was offline has to learn about a deletion as much as about an edit, so what is
// deleted stays as a headstone and comes back with the changes.
TEST_F(BookStoreTest, deletion_leaves_a_headstone)
{
    const std::string entry_guid = guid();
    const int64_t revision = put(entry_guid, std::string(), BookEntry::Kind::GROUP, 0, "payload");

    BookChange change;
    change.guid = entry_guid;
    change.base_revision = revision;
    change.deleted = true;

    std::vector<BookChangeResult> results;
    int64_t new_revision = 0;
    ASSERT_TRUE(store_->applyChanges(kBookGuid, guid(), "PC-TEST", { change },
                                     &results, &new_revision));
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results.front().status, BookChangeResult::Status::OK);

    BookEntry entry;
    ASSERT_TRUE(store_->findEntry(kBookGuid, entry_guid, &entry));
    EXPECT_TRUE(entry.deleted);
    EXPECT_GT(entry.deleted_at, 0);

    // The content of a deleted record is of no use, and keeping a sealed password around for the
    // months a headstone lives would be careless.
    EXPECT_TRUE(entry.payload.empty());
}

//--------------------------------------------------------------------------------------------------
TEST_F(BookStoreTest, headstones_come_back_with_the_changes)
{
    const std::string entry_guid = guid();
    const int64_t revision = put(entry_guid, std::string(), BookEntry::Kind::GROUP, 0, "payload");

    BookChange change;
    change.guid = entry_guid;
    change.base_revision = revision;
    change.deleted = true;

    std::vector<BookChangeResult> results;
    int64_t new_revision = 0;
    ASSERT_TRUE(store_->applyChanges(kBookGuid, guid(), "PC-TEST", { change },
                                     &results, &new_revision));

    std::vector<BookEntry> entries;
    ASSERT_TRUE(store_->entriesSince(kBookGuid, revision, 0, 0, &entries));

    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries.front().guid, entry_guid);
    EXPECT_TRUE(entries.front().deleted);
}

//--------------------------------------------------------------------------------------------------
// What a console asks for after being away: everything that changed since the revision it has.
TEST_F(BookStoreTest, entries_since_returns_only_what_changed)
{
    const std::string first = guid();
    const std::string second = guid();

    put(first, std::string(), BookEntry::Kind::GROUP, 0, "one");
    const int64_t mark = put(second, first, BookEntry::Kind::COMPUTER, 0, "two");

    std::vector<BookEntry> entries;
    ASSERT_TRUE(store_->entriesSince(kBookGuid, mark, 0, 0, &entries));
    EXPECT_TRUE(entries.empty());

    const std::string third = guid();
    put(third, first, BookEntry::Kind::COMPUTER, 0, "three");

    ASSERT_TRUE(store_->entriesSince(kBookGuid, mark, 0, 0, &entries));
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries.front().guid, third);
}

//--------------------------------------------------------------------------------------------------
TEST_F(BookStoreTest, entries_since_zero_returns_the_whole_book)
{
    const std::string root = guid();
    put(root, std::string(), BookEntry::Kind::GROUP, 0, "root");
    put(guid(), root, BookEntry::Kind::COMPUTER, 0, "one");
    put(guid(), root, BookEntry::Kind::COMPUTER, 0, "two");

    std::vector<BookEntry> entries;
    ASSERT_TRUE(store_->entriesSince(kBookGuid, 0, 0, 0, &entries));
    EXPECT_EQ(entries.size(), 3u);
}

//--------------------------------------------------------------------------------------------------
// A book of several hundred records goes over the wire in pieces, so the pages have to be stable
// and not overlap.
TEST_F(BookStoreTest, entries_can_be_taken_in_pages)
{
    const std::string root = guid();
    put(root, std::string(), BookEntry::Kind::GROUP, 0, "root");
    for (int i = 0; i < 9; ++i)
        put(guid(), root, BookEntry::Kind::COMPUTER, 0, "payload");

    std::set<std::string> seen;
    for (int offset = 0; offset < 10; offset += 4)
    {
        std::vector<BookEntry> page;
        ASSERT_TRUE(store_->entriesSince(kBookGuid, 0, offset, 4, &page));

        for (const BookEntry& entry : page)
            EXPECT_TRUE(seen.insert(entry.guid).second) << "seen twice: " << entry.guid;
    }

    EXPECT_EQ(seen.size(), 10u);
}

//--------------------------------------------------------------------------------------------------
// The interval has to be longer than the longest a console may stay offline, so the pruning is
// driven by a time the caller decides on rather than by the store.
TEST_F(BookStoreTest, old_headstones_are_pruned)
{
    const std::string entry_guid = guid();
    const int64_t revision = put(entry_guid, std::string(), BookEntry::Kind::GROUP, 0, "payload");

    BookChange change;
    change.guid = entry_guid;
    change.base_revision = revision;
    change.deleted = true;

    std::vector<BookChangeResult> results;
    int64_t new_revision = 0;
    ASSERT_TRUE(store_->applyChanges(kBookGuid, guid(), "PC-TEST", { change },
                                     &results, &new_revision));

    // Nothing is old enough yet.
    EXPECT_EQ(store_->pruneTombstones(kBookGuid, 1), 0);

    BookEntry entry;
    EXPECT_TRUE(store_->findEntry(kBookGuid, entry_guid, &entry));

    // Everything is, now.
    EXPECT_EQ(store_->pruneTombstones(kBookGuid, std::numeric_limits<int64_t>::max()), 1);
    EXPECT_FALSE(store_->findEntry(kBookGuid, entry_guid, &entry));
}

//--------------------------------------------------------------------------------------------------
TEST_F(BookStoreTest, pruning_leaves_the_records_that_are_not_deleted)
{
    const std::string entry_guid = guid();
    put(entry_guid, std::string(), BookEntry::Kind::GROUP, 0, "payload");

    EXPECT_EQ(store_->pruneTombstones(kBookGuid, std::numeric_limits<int64_t>::max()), 0);

    BookEntry entry;
    EXPECT_TRUE(store_->findEntry(kBookGuid, entry_guid, &entry));
}

//--------------------------------------------------------------------------------------------------
// After a restore from a backup the revision means nothing any more, and the epoch is what tells
// the consoles so.
TEST_F(BookStoreTest, epoch_can_be_replaced)
{
    Book before;
    ASSERT_TRUE(store_->findBook(kBookGuid, &before));

    const std::string epoch = guid();
    ASSERT_TRUE(store_->resetEpoch(kBookGuid, epoch));

    Book after;
    ASSERT_TRUE(store_->findBook(kBookGuid, &after));

    EXPECT_NE(after.epoch, before.epoch);
    EXPECT_EQ(after.epoch, epoch);
    EXPECT_EQ(after.revision, before.revision);
}

//--------------------------------------------------------------------------------------------------
TEST_F(BookStoreTest, removing_a_book_takes_its_records_with_it)
{
    const std::string root = guid();
    put(root, std::string(), BookEntry::Kind::GROUP, 0, "root");

    ASSERT_TRUE(store_->removeBook(kBookGuid));

    Book book;
    EXPECT_FALSE(store_->findBook(kBookGuid, &book));

    BookEntry entry;
    EXPECT_FALSE(store_->findEntry(kBookGuid, root, &entry));
}

//--------------------------------------------------------------------------------------------------
TEST_F(BookStoreTest, refuses_to_work_on_a_book_that_is_not_there)
{
    std::vector<BookChangeResult> results;
    int64_t revision = 0;

    BookChange change;
    change.guid = guid();
    change.payload = "payload";

    EXPECT_FALSE(store_->applyChanges("22222222-2222-4222-8222-222222222222", guid(), "PC-TEST",
                                      { change }, &results, &revision));

    std::vector<BookEntry> entries;
    EXPECT_FALSE(store_->entriesSince("22222222-2222-4222-8222-222222222222", 0, 0, 0, &entries));
}

//--------------------------------------------------------------------------------------------------
TEST_F(BookStoreTest, refuses_a_batch_without_an_op_id)
{
    std::vector<BookChangeResult> results;
    int64_t revision = 0;

    EXPECT_FALSE(store_->applyChanges(kBookGuid, std::string(), "PC-TEST", {},
                                      &results, &revision));
}

//--------------------------------------------------------------------------------------------------
// The records survive the store being closed and opened again: this is a database, not a cache.
TEST_F(BookStoreTest, records_survive_a_reopen)
{
    const std::string entry_guid = guid();
    put(entry_guid, std::string(), BookEntry::Kind::GROUP, 0, "payload");

    store_.reset();
    store_ = BookStore::open(path_);
    ASSERT_TRUE(store_);

    BookEntry entry;
    ASSERT_TRUE(store_->findEntry(kBookGuid, entry_guid, &entry));
    EXPECT_EQ(entry.payload, "payload");
}

} // namespace router
