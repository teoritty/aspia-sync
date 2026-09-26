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

#ifndef ROUTER_BOOK_BOOK_STORE_H
#define ROUTER_BOOK_BOOK_STORE_H

#include "base/macros_magic.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

struct sqlite3;

namespace router {

// The shared address books, as the router keeps them.
//
// The router is the source of truth for what the department's book contains, but not for what the
// records say: their content arrives sealed and is stored as it came. What the router owns is the
// order of changes, and that is what makes it the source of truth - every change is numbered, and
// a change built on a number that is no longer current is refused instead of quietly overwriting
// what somebody else wrote in the meantime.
//
// The store keeps its own connection to the router database. It does not touch the tables that
// were there before it.

struct Book
{
    std::string guid;
    std::string name;

    // The salt the consoles derive the shared key from, and a value sealed with that key so a
    // console can tell a wrong passphrase before it writes anything.
    std::string sync_salt;
    std::string key_verifier;

    // Changes when the database is restored from a backup. A console that sees an epoch other
    // than the one it worked with last knows its idea of the revision means nothing any more.
    std::string epoch;

    // Number of the last change. Grows by one per accepted batch.
    int64_t revision = 0;
};

struct BookEntry
{
    enum class Kind
    {
        GROUP    = 0,
        COMPUTER = 1
    };

    std::string guid;
    std::string parent_guid;
    Kind kind = Kind::GROUP;

    // The book revision this record was last changed at. A console sends back the one it saw; a
    // mismatch is a conflict.
    int64_t revision = 0;

    // The time of the router, which is what orders changes. The time of the client is kept only to
    // be shown to a person: a machine whose clock runs two days fast must not win every conflict.
    int64_t server_time = 0;
    int64_t client_time = 0;

    // Name of the computer the change came from. The department shares one router account, so the
    // account cannot say who did anything; the machine can.
    std::string modified_by;

    // A deleted record is kept as a headstone rather than removed. A console that was offline for
    // a month would otherwise bring back everything deleted while it was away.
    bool deleted = false;
    int64_t deleted_at = 0;

    // Sealed. The router cannot read it and never tries.
    std::string payload;
};

// One record as a console asks to have it. |base_revision| is the revision the console built the
// change on; 0 means it believes the record is new.
struct BookChange
{
    std::string guid;
    std::string parent_guid;
    BookEntry::Kind kind = BookEntry::Kind::GROUP;
    int64_t base_revision = 0;
    bool deleted = false;
    int64_t client_time = 0;
    std::string payload;
};

// What became of one requested change.
struct BookChangeResult
{
    enum class Status
    {
        OK = 0,

        // Somebody else changed the record after the console last saw it. The current record is
        // returned with this, so the console can merge and try again.
        CONFLICT,

        // The record cannot be placed: a guid that is not one, a parent that does not exist, or a
        // move that would put a group inside itself.
        REJECTED
    };

    std::string guid;
    Status status = Status::OK;
    std::string reason;

    // Filled for CONFLICT.
    BookEntry current;
};

// How much of the history of the books the router keeps. Set by the administrator of the router,
// who is the one who knows how much room the disk has.
struct BookHistoryPolicy
{
    // Batches older than this are dropped. Zero keeps no history at all: nothing is written, and
    // what was written before goes at the next sweep.
    int days = 0;

    // And at most this many changed records per book, whatever their age. Zero means no limit by
    // count. Whole batches go, never part of one: a batch half remembered could not be undone.
    int max_changes = 0;

    bool enabled() const { return days > 0; }
};

// Where a batch came from, as the session knows it rather than as the console says.
struct BookBatchNote
{
    // The address the session connected from. The computer name is given by the console itself
    // when it signs in, and the department shares one account; the address is the one thing about
    // the author the console does not get to choose.
    std::string address;

    // The revision the batch puts the book back to, or 0. A label for the history only; the store
    // checks nothing on its account beyond it being a revision that has been.
    int64_t rollback_to = 0;
};

// One record changed by a batch, as it was before the batch and as it was after it.
struct BookHistoryChange
{
    std::string guid;
    BookEntry::Kind kind = BookEntry::Kind::GROUP;

    // False when the batch created the record.
    bool has_before = false;
    BookEntry before;
    BookEntry after;
};

struct BookHistoryBatch
{
    int64_t revision = 0;
    int64_t server_time = 0;
    std::string modified_by;
    std::string address;
    int64_t rollback_to = 0;

    std::vector<BookHistoryChange> changes;
};

class BookStore
{
public:
    ~BookStore();

    // Opens the store, creating its tables when they are not there yet. Returns nullptr when the
    // database cannot be opened.
    static std::unique_ptr<BookStore> open(const std::filesystem::path& file_path);

    // Books.
    bool createBook(const Book& book);
    bool bookList(std::vector<Book>* out) const;
    bool findBook(const std::string& guid, Book* out) const;
    bool removeBook(const std::string& guid);

    // Replaces the epoch with a new one and is what an administrator runs after restoring the
    // database from a backup: it tells every console that what it remembers about the revision no
    // longer refers to anything.
    bool resetEpoch(const std::string& book_guid, const std::string& epoch);

    // Records changed after |since_revision|, headstones included - a console has to learn about a
    // deletion as much as about an edit. Ordered by revision, so a page taken from the middle is
    // stable. |count| of 0 means all of them.
    bool entriesSince(const std::string& book_guid, int64_t since_revision,
                      int64_t offset, int64_t count, std::vector<BookEntry>* out) const;

    bool findEntry(const std::string& book_guid, const std::string& guid, BookEntry* out) const;
    bool entryCount(const std::string& book_guid, int64_t* out) const;

    // Applies a batch. Either every change that can be applied is applied and the revision moves
    // once, or nothing is written at all - a batch half in place would leave the book in a state
    // no console asked for.
    //
    // |op_id| makes a repeat harmless. A console whose connection dropped before the answer
    // arrived does not know whether the batch was applied; sending it again returns what the first
    // attempt returned, without applying anything a second time.
    //
    // Returns false only when the database itself refused. Changes the store declined are reported
    // through |results|, which always holds one entry per requested change, in order.
    //
    // While the history is kept (see setHistoryPolicy), the batch is written to it in the same
    // transaction: every record as it was before and as it is after. A batch in the book and not
    // in its history would be a change nobody could trace or undo.
    bool applyChanges(const std::string& book_guid,
                      const std::string& op_id,
                      const std::string& modified_by,
                      const std::vector<BookChange>& changes,
                      std::vector<BookChangeResult>* results,
                      int64_t* new_revision,
                      const BookBatchNote& note = BookBatchNote());

    // History.
    void setHistoryPolicy(const BookHistoryPolicy& policy) { history_policy_ = policy; }
    const BookHistoryPolicy& historyPolicy() const { return history_policy_; }

    // Batches older than |before_revision|, newest first; 0 starts from the newest. Stops after
    // |max_batches| batches, or once |max_changes| changed records have been taken - but always
    // takes at least one whole batch, or a batch larger than the budget would never be shown.
    // |has_more| says whether anything older is left.
    bool historyBefore(const std::string& book_guid, int64_t before_revision,
                       int64_t max_batches, int64_t max_changes,
                       std::vector<BookHistoryBatch>* out, bool* has_more) const;

    // The oldest revision the book can be put back to: every batch after it is in the history.
    // Equal to the current revision when there is nothing to go back through - no history at all,
    // or a gap in it left by a time it was switched off.
    bool oldestHistoryRevision(const std::string& book_guid, int64_t* out) const;

    // Drops batches older than |before| from the history, and then the oldest batches beyond
    // |max_changes| changed records (0: no limit). Returns the number of changes removed.
    int64_t pruneHistory(const std::string& book_guid, int64_t before, int64_t max_changes);

    // Drops headstones older than |before|. The interval has to be longer than the longest a
    // console may stay offline, or one coming back would resurrect what was deleted while it was
    // away. Returns the number of rows removed.
    int64_t pruneTombstones(const std::string& book_guid, int64_t before);

    // Drops the record of applied batches older than |before|. Only needed to keep the table from
    // growing; a repeat of a batch this old would be a console that has been gone far longer than
    // any retry would wait.
    int64_t pruneAppliedOps(int64_t before);

private:
    explicit BookStore(sqlite3* db);

    bool createTables();
    bool bookIdByGuid(const std::string& guid, int64_t* book_id) const;
    int64_t pruneHistoryThrough(int64_t book_id, int64_t revision);
    int64_t pruneHistoryByCount(int64_t book_id, int64_t max_changes);

    sqlite3* db_;
    BookHistoryPolicy history_policy_;

    DISALLOW_COPY_AND_ASSIGN(BookStore);
};

} // namespace router

#endif // ROUTER_BOOK_BOOK_STORE_H
