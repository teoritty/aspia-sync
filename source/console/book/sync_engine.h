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

#ifndef CONSOLE_BOOK_SYNC_ENGINE_H
#define CONSOLE_BOOK_SYNC_ENGINE_H

#include "base/macros_magic.h"

#include <cstddef>
#include <string>
#include <vector>

namespace proto {
class BookPull;
class BookPushRequest;
class BookPushResult;
} // namespace proto

namespace proto::address_book {
class Data;
} // namespace proto::address_book

namespace console {

// The part of the console that decides what to do with what the router sent and what to send it
// next. It touches neither the network nor the window, so everything it does can be tried out in
// a test - which matters, because this is where an address book gets lost if anything is wrong.
//
// It works on the book itself. What it remembers between runs lives in the book file, so closing
// the console does not lose edits that have not been sent, and does not make the next start ask
// for the whole book again.
class SyncEngine
{
public:
    // The key the records are sealed with; see sync_key.h. The engine holds it for as long as the
    // book is open, exactly as the console already holds the key of the book file itself.
    explicit SyncEngine(std::string sync_key);
    ~SyncEngine();

    // At most this many records go in one batch. The router caps what it accepts as well; this is
    // the same bound on the near side, so a batch is never built only to be refused.
    static constexpr int kMaxChangesPerPush = 200;

    struct PullOutcome
    {
        enum class Status
        {
            OK = 0,

            // The answer is about a different book than the one this file follows.
            WRONG_BOOK,

            // The router was restored from a backup. Its revision no longer refers to what this
            // console remembers, and going on would quietly lose whatever happened in between.
            // The book has to be fetched again from nothing.
            EPOCH_CHANGED,

            // The same thing said by the numbers rather than by the epoch: the router is behind
            // where this console already was. Treated exactly as gravely.
            REVISION_WENT_BACK,

            // The passphrase is not the one the book was sealed with, so nothing can be read.
            // Nothing is written in this case - a book half filled with unreadable records would
            // be worse than one that did not synchronize at all.
            BAD_KEY
        };

        Status status = Status::OK;

        size_t applied = 0; // Records taken into the book.
        size_t skipped = 0; // Records that could not be used and were passed over.

        // The tree under root_group() was replaced, so anything holding a pointer into it - the
        // window holds one per computer in the list - is now holding a pointer to nothing. The
        // caller has to redraw before anybody can click on anything.
        bool tree_rebuilt = false;

        // Records where both sides changed the same field. They keep their local value and wait
        // for a person; everything else goes on without them, because one unanswered question
        // must not stop the book from synchronizing.
        std::vector<std::string> conflicts;
    };

    // Applies one page of what the router sent.
    //
    // |data->sync()| is updated along with the records, and the revision only moves on the last
    // page: a console interrupted halfway asks again from where it was rather than believing it
    // has what it never received.
    PullOutcome applyPull(const proto::BookPull& page, proto::address_book::Data* data);

    // Builds the next batch out of what has been changed here and not sent yet. Returns false when
    // there is nothing to send.
    bool buildPush(const proto::address_book::Data& data, const std::string& op_id,
                   proto::BookPushRequest* request);

    struct PushOutcome
    {
        size_t accepted = 0;
        size_t rejected = 0;

        // As above: what was merged went back into the tree, and the tree was rebuilt to do it.
        bool tree_rebuilt = false;

        // Refused because somebody else got there first. These are merged against what came back
        // with the refusal, and what merges cleanly is ready to be sent again at once.
        size_t merged = 0;

        // And what did not merge waits for a person.
        std::vector<std::string> conflicts;
    };

    // Settles a record that was changed here and by somebody else in the same field, and that has
    // been waiting for somebody to say which version is meant.
    //
    // |keep_local| keeps what is in this book and sends it; otherwise what the router has is taken
    // and the local version is given up. When the other side deleted the record, keeping it sends
    // it again as a new one and giving it up removes it here too.
    //
    // Returns false when the record is not one that is waiting.
    bool resolveConflict(const std::string& guid, bool keep_local,
                         proto::address_book::Data* data);

    // Takes the answer to a batch. What was accepted stops being pending; what was refused is
    // merged where it can be and set aside where it cannot.
    PushOutcome applyPushResult(const proto::BookPushResult& result,
                                proto::address_book::Data* data);

private:
    const std::string sync_key_;

    DISALLOW_COPY_AND_ASSIGN(SyncEngine);
};

} // namespace console

#endif // CONSOLE_BOOK_SYNC_ENGINE_H
