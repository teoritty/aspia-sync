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

#include "router/session_admin.h"

#include "base/guid.h"
#include "base/logging.h"
#include "base/peer/user.h"
#include "router/book/book_store.h"
#include "router/database.h"
#include "router/database_sqlite.h"
#include "router/server.h"
#include "router/session_relay.h"

namespace router {

namespace {

// Bounds on what an administrator may send. None of these are reached by the console; they are
// here so that something which is not the console cannot make the router store nonsense.
constexpr size_t kMaxBookNameLength = 64;
constexpr size_t kSyncSaltSize = 32;
constexpr size_t kMaxVerifierLength = 1024;

} // namespace

//--------------------------------------------------------------------------------------------------
SessionAdmin::SessionAdmin()
    : Session(proto::ROUTER_SESSION_ADMIN)
{
    LOG(LS_INFO) << "Ctor";
}

//--------------------------------------------------------------------------------------------------
SessionAdmin::~SessionAdmin()
{
    LOG(LS_INFO) << "Dtor";
}

//--------------------------------------------------------------------------------------------------
void SessionAdmin::onSessionReady()
{
    // Nothing
}

//--------------------------------------------------------------------------------------------------
void SessionAdmin::onSessionMessageReceived(uint8_t /* channel_id */, const base::ByteArray& buffer)
{
    std::unique_ptr<proto::AdminToRouter> message = std::make_unique<proto::AdminToRouter>();

    if (!base::parse(buffer, message.get()))
    {
        LOG(LS_ERROR) << "Could not read message from manager";
        return;
    }

    if (message->has_session_list_request())
    {
        doSessionListRequest(message->session_list_request());
    }
    else if (message->has_session_request())
    {
        doSessionRequest(message->session_request());
    }
    else if (message->has_user_list_request())
    {
        doUserListRequest();
    }
    else if (message->has_user_request())
    {
        doUserRequest(message->user_request());
    }
    else if (message->has_peer_connection_request())
    {
        doPeerConnectionRequest(message->peer_connection_request());
    }
    else if (message->has_book_create_request())
    {
        doBookCreateRequest(message->book_create_request());
    }
    else
    {
        LOG(LS_ERROR) << "Unhandled message from manager";
    }
}

//--------------------------------------------------------------------------------------------------
void SessionAdmin::onSessionMessageWritten(uint8_t /* channel_id */, size_t /* pending */)
{
    // Nothing
}

//--------------------------------------------------------------------------------------------------
void SessionAdmin::doBookCreateRequest(const proto::BookCreateRequest& request)
{
    proto::RouterToAdmin message;
    proto::BookCreateResult* result = message.mutable_book_create_result();

    // The salt and the verifier are made by the console. The router checks that they are there and
    // of a sane size, and nothing else: it cannot read what they protect and has no business
    // trying.
    if (request.name().empty() || request.name().size() > kMaxBookNameLength ||
        request.sync_salt().size() != kSyncSaltSize ||
        request.key_verifier().empty() ||
        request.key_verifier().size() > kMaxVerifierLength)
    {
        LOG(LS_ERROR) << "Invalid book creation request";
        result->set_error_code(proto::BookCreateResult::INVALID_DATA);
        sendMessage(proto::ROUTER_CHANNEL_ID_SESSION, message);
        return;
    }

    std::unique_ptr<BookStore> store = BookStore::open(DatabaseSqlite::filePath());
    if (!store)
    {
        LOG(LS_ERROR) << "Unable to open the address book store";
        result->set_error_code(proto::BookCreateResult::INTERNAL_ERROR);
        sendMessage(proto::ROUTER_CHANNEL_ID_SESSION, message);
        return;
    }

    std::vector<Book> existing;
    if (store->bookList(&existing))
    {
        for (const Book& book : existing)
        {
            if (book.name != request.name())
                continue;

            // Two books under one name would be told apart by nobody, and the consoles pick one
            // from a list of names.
            LOG(LS_ERROR) << "A book with this name already exists";
            result->set_error_code(proto::BookCreateResult::ALREADY_EXISTS);
            sendMessage(proto::ROUTER_CHANNEL_ID_SESSION, message);
            return;
        }
    }

    Book book;
    book.guid = base::Guid::create().toStdString();
    book.name = request.name();
    book.sync_salt = request.sync_salt();
    book.key_verifier = request.key_verifier();

    // The epoch says which run of this database the revisions belong to. It is replaced when the
    // database is restored from a backup, which is how a console finds out that what it remembers
    // about the revision no longer refers to anything.
    book.epoch = base::Guid::create().toStdString();

    if (!store->createBook(book))
    {
        LOG(LS_ERROR) << "Unable to create the book";
        result->set_error_code(proto::BookCreateResult::INTERNAL_ERROR);
        sendMessage(proto::ROUTER_CHANNEL_ID_SESSION, message);
        return;
    }

    LOG(LS_INFO) << "Shared address book created: " << book.name;

    result->set_error_code(proto::BookCreateResult::SUCCESS);
    result->set_guid(book.guid);

    sendMessage(proto::ROUTER_CHANNEL_ID_SESSION, message);
}

//--------------------------------------------------------------------------------------------------
void SessionAdmin::doUserListRequest()
{
    std::unique_ptr<Database> database = openDatabase();
    if (!database)
    {
        LOG(LS_ERROR) << "Failed to connect to database";
        return;
    }

    std::unique_ptr<proto::RouterToAdmin> message = std::make_unique<proto::RouterToAdmin>();
    proto::UserList* list = message->mutable_user_list();

    std::vector<base::User> users = database->userList();
    for (const auto& user : users)
        list->add_user()->CopyFrom(user.serialize());

    sendMessage(proto::ROUTER_CHANNEL_ID_SESSION, *message);
}

//--------------------------------------------------------------------------------------------------
void SessionAdmin::doUserRequest(const proto::UserRequest& request)
{
    std::unique_ptr<proto::RouterToAdmin> message = std::make_unique<proto::RouterToAdmin>();
    proto::UserResult* result = message->mutable_user_result();
    result->set_type(request.type());

    switch (request.type())
    {
        case proto::USER_REQUEST_ADD:
            result->set_error_code(addUser(request.user()));
            break;

        case proto::USER_REQUEST_MODIFY:
            result->set_error_code(modifyUser(request.user()));
            break;

        case proto::USER_REQUEST_DELETE:
            result->set_error_code(deleteUser(request.user()));
            break;

        default:
            LOG(LS_ERROR) << "Unknown request type: " << request.type();
            return;
    }

    sendMessage(proto::ROUTER_CHANNEL_ID_SESSION, *message);
}

//--------------------------------------------------------------------------------------------------
void SessionAdmin::doSessionListRequest(const proto::SessionListRequest& /* request */)
{
    std::unique_ptr<proto::RouterToAdmin> message = std::make_unique<proto::RouterToAdmin>();

    message->set_allocated_session_list(server().sessionList().release());
    if (!message->has_session_list())
        message->mutable_session_list()->set_error_code(proto::SessionList::UNKNOWN_ERROR);

    sendMessage(proto::ROUTER_CHANNEL_ID_SESSION, *message);
}

//--------------------------------------------------------------------------------------------------
void SessionAdmin::doSessionRequest(const proto::SessionRequest& request)
{
    std::unique_ptr<proto::RouterToAdmin> message = std::make_unique<proto::RouterToAdmin>();
    proto::SessionResult* session_result = message->mutable_session_result();
    session_result->set_type(request.type());

    if (request.type() == proto::SESSION_REQUEST_DISCONNECT)
    {
        Session::SessionId session_id = request.session_id();

        if (!server().stopSession(session_id))
        {
            LOG(LS_ERROR) << "Session not found: " << session_id;
            session_result->set_error_code(proto::SessionResult::INVALID_SESSION_ID);
        }
        else
        {
            LOG(LS_INFO) << "Session '" << session_id << "' disconnected by " << userName();
            session_result->set_error_code(proto::SessionResult::SUCCESS);
        }
    }
    else
    {
        LOG(LS_ERROR) << "Unknown session request: " << request.type();
        session_result->set_error_code(proto::SessionResult::INVALID_REQUEST);
    }

    sendMessage(proto::ROUTER_CHANNEL_ID_SESSION, *message);
}

//--------------------------------------------------------------------------------------------------
void SessionAdmin::doPeerConnectionRequest(const proto::PeerConnectionRequest& request)
{
    SessionRelay* relay_session =
        dynamic_cast<SessionRelay*>(server().sessionById(request.relay_session_id()));
    if (!relay_session)
    {
        LOG(LS_ERROR) << "Relay with id " << request.relay_session_id() << " not found";
        return;
    }

    relay_session->disconnectPeerSession(request);
}

//--------------------------------------------------------------------------------------------------
proto::UserResult::ErrorCode SessionAdmin::addUser(const proto::User& user)
{
    LOG(LS_INFO) << "User add request: " << user.name();

    base::User new_user = base::User::parseFrom(user);
    if (!new_user.isValid())
    {
        LOG(LS_ERROR) << "Failed to create user";
        return proto::UserResult::INTERNAL_ERROR;
    }

    if (!base::User::isValidUserName(new_user.name))
    {
        LOG(LS_ERROR) << "Invalid user name: " << new_user.name;
        return proto::UserResult::INVALID_DATA;
    }

    std::unique_ptr<Database> database = openDatabase();
    if (!database)
    {
        LOG(LS_ERROR) << "Failed to connect to database";
        return proto::UserResult::INTERNAL_ERROR;
    }

    if (!database->addUser(new_user))
        return proto::UserResult::INTERNAL_ERROR;

    return proto::UserResult::SUCCESS;
}

//--------------------------------------------------------------------------------------------------
proto::UserResult::ErrorCode SessionAdmin::modifyUser(const proto::User& user)
{
    LOG(LS_INFO) << "User modify request: " << user.name();

    if (user.entry_id() <= 0)
    {
        LOG(LS_ERROR) << "Invalid user ID: " << user.entry_id();
        return proto::UserResult::INVALID_DATA;
    }

    base::User new_user = base::User::parseFrom(user);
    if (!new_user.isValid())
    {
        LOG(LS_ERROR) << "Failed to create user";
        return proto::UserResult::INTERNAL_ERROR;
    }

    if (!base::User::isValidUserName(new_user.name))
    {
        LOG(LS_ERROR) << "Invalid user name: " << new_user.name;
        return proto::UserResult::INVALID_DATA;
    }

    std::unique_ptr<Database> database = openDatabase();
    if (!database)
    {
        LOG(LS_ERROR) << "Failed to connect to database";
        return proto::UserResult::INTERNAL_ERROR;
    }

    if (!database->modifyUser(new_user))
    {
        LOG(LS_ERROR) << "modifyUser failed";
        return proto::UserResult::INTERNAL_ERROR;
    }

    return proto::UserResult::SUCCESS;
}

//--------------------------------------------------------------------------------------------------
proto::UserResult::ErrorCode SessionAdmin::deleteUser(const proto::User& user)
{
    std::unique_ptr<Database> database = openDatabase();
    if (!database)
    {
        LOG(LS_ERROR) << "Failed to connect to database";
        return proto::UserResult::INTERNAL_ERROR;
    }

    int64_t entry_id = user.entry_id();

    LOG(LS_INFO) << "User remove request: " << entry_id;

    if (!database->removeUser(entry_id))
    {
        LOG(LS_ERROR) << "removeUser failed";
        return proto::UserResult::INTERNAL_ERROR;
    }

    return proto::UserResult::SUCCESS;
}

} // namespace router
