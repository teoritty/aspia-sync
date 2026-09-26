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

#ifndef CONSOLE_BOOK_BOOK_HISTORY_H
#define CONSOLE_BOOK_BOOK_HISTORY_H

#include "console/book/flat_book.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace proto {
class BookHistory;
} // namespace proto

namespace proto::address_book {
class Data;
} // namespace proto::address_book

namespace console {

// The history of the shared book, as the console reads it: who changed what and when, and what it
// takes to put the book back.
//
// The router keeps every accepted batch with each record as it was before and after, sealed. It
// cannot read any of it, so everything that needs the content - what a change was, and what
// putting it back means - is decided here. Like the engine, this touches neither the network nor
// the window.
//
// Putting the book back is not an operation of its own. It is a set of ordinary edits to the book,
// sent the ordinary way: they are checked against the revision like any other, so a colleague's
// edit made in the meantime becomes a conflict to be decided rather than something silently
// written over. And they land in the history themselves, so going back can be undone as well.

// One record at one point in the history.
struct HistoryRecord
{
    // False when there was no such record: the "before" of a batch that created it.
    bool exists = false;

    // A headstone: the record had been deleted.
    bool deleted = false;

    FlatEntry::Kind kind = FlatEntry::Kind::GROUP;
    std::string parent_guid;

    // The revision the record was at on the router.
    int64_t revision = 0;

    // Opened. Empty for a record that did not exist or had been deleted.
    std::string payload;

    // False when the payload was there but did not open. Such a record is shown for what little
    // is known about it and is never written back into the book: a damaged or substituted payload
    // must not become somebody's computer.
    bool readable = true;

    bool alive() const { return exists && !deleted; }
};

struct HistoryChange
{
    std::string guid;
    FlatEntry::Kind kind = FlatEntry::Kind::GROUP;

    HistoryRecord before;
    HistoryRecord after;
};

struct HistoryBatch
{
    int64_t revision = 0;
    int64_t server_time = 0;

    // As the router recorded them from the session, not as the console that made the batch said.
    std::string modified_by;
    std::string address;

    // Non-zero when the batch put the book back to that revision.
    int64_t rollback_to = 0;

    std::vector<HistoryChange> changes;
};

// Opens a page of the history. Nothing is dropped for failing to open; see HistoryRecord::readable.
std::vector<HistoryBatch> openHistory(const proto::BookHistory& page, std::string_view sync_key);

// What one change was, for a person to read.
struct ChangeDescription
{
    enum class Action
    {
        CREATED,
        CHANGED,
        MOVED,     // Only the group it is in changed.
        DELETED,
        RESTORED,  // Written over its own headstone.
        UNREADABLE
    };

    Action action = Action::CHANGED;

    // The name of the record, from whichever side has one.
    std::string name;

    // The fields that differ, by name: "name", "address", "password" and so on. Only which fields
    // changed, never what they changed to - the history is shown on a screen, and a password on a
    // screen is a password on the photo somebody takes of it.
    std::vector<std::string> fields;
};

ChangeDescription describeChange(const HistoryChange& change);

// What putting a record back means.
struct RollbackTarget
{
    std::string guid;
    FlatEntry::Kind kind = FlatEntry::Kind::GROUP;

    // Whether the record is to be in the book afterwards, and if so, as what.
    bool present = false;
    std::string parent_guid;
    std::string payload;

    // Where the record is on the router now: its revision, and whether it is a headstone. Putting
    // back a deleted record is written over its headstone, and the router takes that only from
    // somebody who names the headstone's revision.
    int64_t router_revision = 0;
    bool router_alive = false;
};

struct RollbackPlan
{
    std::vector<RollbackTarget> targets;

    // Records that would have to change but whose earlier version did not open. They are left as
    // they are.
    std::vector<std::string> unreadable;
};

// Works out what putting the book back to |to_revision| takes, from every batch after it. The
// batches may come in any order; the caller is responsible for having all of them - see
// BookHistory.oldest_revision.
//
// |only_guid| limits it to one record, which is how a single deleted computer is brought back
// without touching anything else.
RollbackPlan planRollback(const std::vector<HistoryBatch>& batches, int64_t to_revision,
                          const std::string& only_guid = std::string());

struct RollbackCounts
{
    size_t restored = 0; // Brought back after being deleted.
    size_t removed = 0;  // Made after the point and removed again.
    size_t changed = 0;  // Put back to what they were.

    size_t total() const { return restored + removed + changed; }
};

// The pages of the history this console has fetched, newest first.
class HistoryJournal
{
public:
    // Takes a page. A page asked for from the newest (before_revision 0) starts the journal over.
    // Returns false for a page that is not usable: an error, or another book.
    bool addPage(const proto::BookHistory& page, const std::string& book_guid,
                 int64_t asked_before, std::string_view sync_key);

    void clear();

    const std::vector<HistoryBatch>& batches() const { return batches_; }

    // Whether the router has older batches than those fetched, and what to ask for to get them.
    bool hasMore() const { return has_more_; }
    int64_t nextBefore() const;

    // The revision of the book when the first page was taken, and the oldest one it can be put
    // back to.
    int64_t revision() const { return revision_; }
    int64_t oldestRevision() const { return oldest_revision_; }

    int historyDays() const { return history_days_; }
    int historyMaxChanges() const { return history_max_changes_; }

    // Whether every batch after |revision| has been fetched.
    bool covers(int64_t revision) const;

    const HistoryBatch* findBatch(int64_t revision) const;

private:
    std::vector<HistoryBatch> batches_;
    bool has_more_ = false;
    int64_t revision_ = 0;
    int64_t oldest_revision_ = 0;
    int history_days_ = 0;
    int history_max_changes_ = 0;
};

// Whether the book can be put back to |to_revision| now, and what it takes.
enum class RollbackCheck
{
    OK = 0,

    // Something is waiting to be sent or decided, or the book is behind the router. The plan is
    // built on what the router has, and would be applied to a book that is not that.
    NOT_IN_STEP,

    // The point is older than the history reaches.
    TOO_OLD,

    // Not every batch after the point has been fetched yet.
    NOT_LOADED,

    // Everything is already as it was then.
    NOTHING_TO_DO
};

RollbackCheck prepareRollback(const HistoryJournal& journal,
                              const proto::address_book::Data& data,
                              int64_t to_revision, const std::string& only_guid,
                              RollbackPlan* plan);

// Writes the plan into the book, so that the next exchange sends it. The book must be in step with
// the router - nothing waiting to be sent, nothing waiting for a person - or the plan is built on
// a book that is not the one being changed; the caller checks.
//
// Returns what was done. Nothing is sent from here.
RollbackCounts applyRollback(const RollbackPlan& plan, proto::address_book::Data* data);

} // namespace console

#endif // CONSOLE_BOOK_BOOK_HISTORY_H
