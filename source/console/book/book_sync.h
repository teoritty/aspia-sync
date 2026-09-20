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

#ifndef CONSOLE_BOOK_BOOK_SYNC_H
#define CONSOLE_BOOK_BOOK_SYNC_H

#include "base/macros_magic.h"
#include "console/book/sync_engine.h"

#include <memory>
#include <string>
#include <vector>

namespace proto {
class BookChanged;
class BookList;
class BookPull;
class BookPullRequest;
class BookPushRequest;
class BookPushResult;
} // namespace proto

namespace proto::address_book {
class Data;
} // namespace proto::address_book

namespace console {

// Runs the exchange: asks for what is missing, sends what is waiting, and does it again whenever
// the router says something moved.
//
// What actually goes over the wire is behind an interface, so the whole of this can be exercised
// without a router: a test hands it answers and looks at what it asks for next. The part that
// cannot be tested - sockets and handshakes - stays in the controller and holds no decisions.
class BookSync
{
public:
    // Where requests go. The console wires this to the controller; a test wires it to itself.
    class Sender
    {
    public:
        virtual ~Sender() = default;

        virtual void sendPull(const proto::BookPullRequest& request) = 0;
        virtual void sendPush(const proto::BookPushRequest& request) = 0;
    };

    // What the person is told. None of it is fatal on its own; it is what appears in the status
    // line and in the list of things waiting to be decided.
    class Observer
    {
    public:
        virtual ~Observer() = default;

        // The book changed and has to be written to disk and redrawn.
        virtual void onBookUpdated() = 0;

        // Records where both sides changed the same field. They keep their local value until a
        // person says otherwise.
        virtual void onConflicts(const std::vector<std::string>& guids) = 0;

        // The exchange cannot go on: the router was restored from a backup, or the passphrase is
        // wrong. Both need a person, and neither is helped by trying again.
        virtual void onSyncStopped(SyncEngine::PullOutcome::Status reason) = 0;

        // Nothing is waiting to be sent and nothing new has arrived.
        virtual void onInSync(int64_t revision) = 0;
    };

    BookSync(std::string sync_key, Sender* sender, Observer* observer);
    ~BookSync();

    // Starts an exchange: what is waiting goes out, then what is missing is asked for. Called when
    // the connection comes up, when the book is edited, and when the router says something moved.
    void start(proto::address_book::Data* data);

    // Answers from the router.
    void onPull(const proto::BookPull& page, proto::address_book::Data* data);
    void onPushResult(const proto::BookPushResult& result, proto::address_book::Data* data);
    void onBookChanged(const proto::BookChanged& message, proto::address_book::Data* data);

    // Whether an exchange is in flight. A second one is not started on top of it: the answers
    // would be about a book that has moved underneath them.
    bool isBusy() const { return busy_; }

    // Records waiting for a person, kept so the console can show them after a restart as well.
    const std::vector<std::string>& conflicts() const { return conflicts_; }
    void clearConflict(const std::string& guid);

private:
    void pushOrPull(proto::address_book::Data* data);
    void requestPull(const proto::address_book::Data& data);
    void noteConflicts(const std::vector<std::string>& guids);

    SyncEngine engine_;
    Sender* sender_;
    Observer* observer_;

    bool busy_ = false;

    // Set while an exchange is running and something else asks for one. Starting a second
    // exchange on top of the first would have its answers arrive about a book that moved
    // underneath them, so the request is remembered and run afterwards.
    bool again_ = false;

    std::string op_id_;
    std::vector<std::string> conflicts_;

    DISALLOW_COPY_AND_ASSIGN(BookSync);
};

} // namespace console

#endif // CONSOLE_BOOK_BOOK_SYNC_H
