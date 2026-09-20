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

#ifndef ROUTER_SESSION_CLIENT_H
#define ROUTER_SESSION_CLIENT_H

#include "proto/router_peer.pb.h"
#include "router/session.h"

#include <memory>

namespace proto {
class BookClientToRouter;
} // namespace proto

namespace router {

class BookService;
class BookStore;
class ServerProxy;
class SharedKeyPool;

class SessionClient final : public Session
{
public:
    SessionClient();
    ~SessionClient() final;

    // Tells this console that the shared book moved on. Called by the server for every client
    // session but the one that made the change.
    void onBookChanged(const std::string& book_guid, int64_t revision);

protected:
    // Session implementation.
    void onSessionReady() final;
    void onSessionMessageReceived(uint8_t channel_id, const base::ByteArray& buffer) final;
    void onSessionMessageWritten(uint8_t channel_id, size_t pending) final;

    // This is the one session type the address book is served to.
    void onBookMessageReceived(const base::ByteArray& buffer) final;

private:
    void readConnectionRequest(const proto::ConnectionRequest& request);
    void readCheckHostStatus(const proto::CheckHostStatus& check_host_status);
    void readBookMessage(const proto::BookClientToRouter& message);

    // Opened on the first book request rather than at the start of the session: most sessions
    // never ask about the book, and opening a database for them would be work done for nothing.
    bool ensureBookService();

    std::unique_ptr<BookStore> book_store_;
    std::unique_ptr<BookService> book_service_;

    DISALLOW_COPY_AND_ASSIGN(SessionClient);
};

} // namespace router

#endif // ROUTER_SESSION_CLIENT_H
