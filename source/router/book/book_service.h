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

#ifndef ROUTER_BOOK_BOOK_SERVICE_H
#define ROUTER_BOOK_BOOK_SERVICE_H

#include "base/macros_magic.h"

#include <cstddef>
#include <string>

namespace proto {
class BookHistory;
class BookHistoryRequest;
class BookList;
class BookListRequest;
class BookPull;
class BookPullRequest;
class BookPushRequest;
class BookPushResult;
} // namespace proto

namespace router {

class BookStore;

// Answers the requests of a console about the shared address book.
//
// It is kept apart from the session that carries the messages so that it can be tested: what has
// to be right here is the checking of what arrives and the shape of the answers, and neither needs
// a socket to be exercised.
//
// Everything that arrives is treated as hostile. The session it comes from is authenticated, but
// an authenticated peer is not a well-behaved one, and the fields below end up in queries and in
// the links between records.
class BookService
{
public:
    // The store outlives the service; the session owns both.
    explicit BookService(BookStore* store);
    ~BookService() = default;

    // Largest page a single answer carries. A book of several hundred records with long comments
    // in them can pass the message size limit of the channel, and a message over that limit does
    // not fail - it ends the connection. So the page is capped here whatever the console asked
    // for, and |has_more| tells it to come back for the rest.
    static constexpr int64_t kMaxEntriesPerPage = 200;

    // And a second bound, because the number of records says nothing about their size: a hundred
    // records with a page of comment each are larger than a thousand bare ones.
    static constexpr size_t kMaxPayloadBytesPerPage = 1024 * 1024;

    // The same bounds for a page of the history. Each change there carries up to two payloads, the
    // record before and after, so the page is counted in batches, in changes and in bytes.
    static constexpr int64_t kMaxHistoryBatchesPerPage = 50;
    static constexpr int64_t kMaxHistoryChangesPerPage = 500;

    void handleListRequest(const proto::BookListRequest& request, proto::BookList* result);

    void handlePullRequest(const proto::BookPullRequest& request, proto::BookPull* result);

    // |modified_by| is the name of the computer the session came from. The department shares one
    // router account, so it is the only thing that can say where a change came from. |address| is
    // where the session connected from, and goes into the history beside it: the name is what the
    // console says about itself, the address is not.
    void handlePushRequest(const proto::BookPushRequest& request,
                           const std::string& modified_by,
                           proto::BookPushResult* result,
                           const std::string& address = std::string());

    void handleHistoryRequest(const proto::BookHistoryRequest& request,
                              proto::BookHistory* result);

private:
    BookStore* store_;

    DISALLOW_COPY_AND_ASSIGN(BookService);
};

} // namespace router

#endif // ROUTER_BOOK_BOOK_SERVICE_H
