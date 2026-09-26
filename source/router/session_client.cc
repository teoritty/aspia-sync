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

#include "router/session_client.h"

#include "proto/router_book.pb.h"
#include "router/book/book_service.h"
#include "router/book/book_store.h"
#include "router/database_sqlite.h"

#include "base/logging.h"
#include "base/crypto/random.h"
#include "base/strings/unicode.h"
#include "proto/relay_peer.pb.h"
#include "router/server.h"
#include "router/session_host.h"
#include "router/session_relay.h"

namespace router {

//--------------------------------------------------------------------------------------------------
SessionClient::SessionClient()
    : Session(proto::ROUTER_SESSION_CLIENT)
{
    LOG(LS_INFO) << "Ctor";
}

//--------------------------------------------------------------------------------------------------
SessionClient::~SessionClient()
{
    LOG(LS_INFO) << "Dtor";
}

//--------------------------------------------------------------------------------------------------
void SessionClient::onSessionReady()
{
    // Nothing
}

//--------------------------------------------------------------------------------------------------
void SessionClient::onSessionMessageReceived(uint8_t /* channel_id */, const base::ByteArray& buffer)
{
    std::unique_ptr<proto::PeerToRouter> message = std::make_unique<proto::PeerToRouter>();
    if (!base::parse(buffer, message.get()))
    {
        LOG(LS_ERROR) << "Could not read message from client";
        return;
    }

    if (message->has_connection_request())
    {
        readConnectionRequest(message->connection_request());
    }
    else if (message->has_check_host_status())
    {
        readCheckHostStatus(message->check_host_status());
    }
    else
    {
        LOG(LS_ERROR) << "Unhandled message from client";
    }
}

//--------------------------------------------------------------------------------------------------
void SessionClient::onSessionMessageWritten(uint8_t /* channel_id */, size_t /* pending */)
{
    // Nothing
}

//--------------------------------------------------------------------------------------------------
bool SessionClient::ensureBookService()
{
    if (book_service_)
        return true;

    book_store_ = BookStore::open(DatabaseSqlite::filePath());
    if (!book_store_)
    {
        LOG(LS_ERROR) << "Unable to open the address book store";
        return false;
    }

    // The history is kept as the administrator of the router set it. Every session opens a store
    // of its own, so each is told.
    book_store_->setHistoryPolicy(server().bookHistoryPolicy());

    book_service_ = std::make_unique<BookService>(book_store_.get());
    return true;
}

//--------------------------------------------------------------------------------------------------
void SessionClient::onBookMessageReceived(const base::ByteArray& buffer)
{
    proto::BookClientToRouter message;
    if (!base::parse(buffer, &message))
    {
        LOG(LS_ERROR) << "Could not read address book message from client";
        return;
    }

    readBookMessage(message);
}

//--------------------------------------------------------------------------------------------------
void SessionClient::readBookMessage(const proto::BookClientToRouter& message)
{
    if (!ensureBookService())
        return;

    proto::RouterToBookClient reply;

    if (message.has_book_list_request())
    {
        book_service_->handleListRequest(message.book_list_request(), reply.mutable_book_list());
    }
    else if (message.has_book_pull_request())
    {
        book_service_->handlePullRequest(message.book_pull_request(), reply.mutable_book_pull());
    }
    else if (message.has_book_push_request())
    {
        // The department shares one router account, so the account cannot say who made a change.
        // The computer the session came from can, and that is what is recorded against the record.
        book_service_->handlePushRequest(message.book_push_request(), computerName(),
                                         reply.mutable_book_push_result(), address());

        const proto::BookPushResult& result = reply.book_push_result();
        if (result.error_code() == proto::BOOK_ERROR_CODE_OK)
        {
            // Everyone else with the book open is told that something moved, and asks for what it
            // does not have. The message carries no content, so one of them is enough however much
            // changed.
            server().onBookChanged(result.book_guid(), result.revision(), sessionId());
        }
    }
    else if (message.has_book_history_request())
    {
        book_service_->handleHistoryRequest(message.book_history_request(),
                                            reply.mutable_book_history());
    }
    else
    {
        LOG(LS_ERROR) << "Unhandled address book message from client";
        return;
    }

    sendMessage(proto::ROUTER_CHANNEL_ID_BOOK, reply);
}

//--------------------------------------------------------------------------------------------------
void SessionClient::onBookChanged(const std::string& book_guid, int64_t revision)
{
    proto::RouterToBookClient message;

    proto::BookChanged* changed = message.mutable_book_changed();
    changed->set_book_guid(book_guid);
    changed->set_revision(revision);

    sendMessage(proto::ROUTER_CHANNEL_ID_BOOK, message);
}

//--------------------------------------------------------------------------------------------------
void SessionClient::readConnectionRequest(const proto::ConnectionRequest& request)
{
    LOG(LS_INFO) << "New connection request (host_id: " << request.host_id() << ")";

    std::unique_ptr<proto::RouterToPeer> message = std::make_unique<proto::RouterToPeer>();
    proto::ConnectionOffer* offer = message->mutable_connection_offer();

    SessionHost* host = server().hostSessionById(request.host_id());
    if (!host)
    {
        LOG(LS_ERROR) << "Host with id " << request.host_id() << " NOT found!";
        offer->set_error_code(proto::ConnectionOffer::PEER_NOT_FOUND);
    }
    else
    {
        LOG(LS_INFO) << "Host with id " << request.host_id() << " found";

        std::optional<SharedKeyPool::Credentials> credentials = relayKeyPool().takeCredentials();
        if (!credentials.has_value())
        {
            LOG(LS_ERROR) << "Empty key pool";
            offer->set_error_code(proto::ConnectionOffer::KEY_POOL_EMPTY);
        }
        else
        {
            SessionRelay* relay = static_cast<SessionRelay*>(
                server().sessionById(credentials->session_id));
            if (!relay)
            {
                LOG(LS_ERROR) << "No relay with session id " << credentials->session_id;
                offer->set_error_code(proto::ConnectionOffer::KEY_POOL_EMPTY);
            }
            else
            {
                const std::optional<SessionRelay::PeerData>& peer_data = relay->peerData();
                if (!peer_data.has_value())
                {
                    LOG(LS_ERROR) << "No peer data for relay with session id "
                                  << credentials->session_id;
                    offer->set_error_code(proto::ConnectionOffer::KEY_POOL_EMPTY);
                }
                else
                {
                    offer->set_error_code(proto::ConnectionOffer::SUCCESS);

                    proto::HostOfferData* offer_data = offer->mutable_host_data();
                    offer_data->set_host_id(request.host_id());

                    proto::RelayCredentials* offer_credentials = offer->mutable_relay();

                    offer_credentials->set_host(relay->peerData()->first);
                    offer_credentials->set_port(relay->peerData()->second);
                    offer_credentials->mutable_key()->Swap(&credentials->key);

                    proto::PeerToRelay::Secret secret;
                    secret.set_random_data(base::Random::string(16));
                    secret.set_client_address(address());
                    secret.set_client_user_name(userName());
                    secret.set_host_address(host->address());
                    secret.set_host_id(request.host_id());

                    offer_credentials->set_secret(secret.SerializeAsString());

                    LOG(LS_INFO) << "Sending connection offer to host";
                    offer->set_peer_role(proto::ConnectionOffer::HOST);
                    host->sendConnectionOffer(*offer);
                }
            }
        }
    }

    LOG(LS_INFO) << "Sending connection offer to client";
    offer->clear_host_data(); // Host data is only needed by the host.
    offer->set_peer_role(proto::ConnectionOffer::CLIENT);
    sendMessage(proto::ROUTER_CHANNEL_ID_SESSION, *message);
}

//--------------------------------------------------------------------------------------------------
void SessionClient::readCheckHostStatus(const proto::CheckHostStatus& check_host_status)
{
    std::unique_ptr<proto::RouterToPeer> message = std::make_unique<proto::RouterToPeer>();
    proto::HostStatus* host_status = message->mutable_host_status();

    if (server().hostSessionById(check_host_status.host_id()))
        host_status->set_status(proto::HostStatus::STATUS_ONLINE);
    else
        host_status->set_status(proto::HostStatus::STATUS_OFFLINE);

    LOG(LS_INFO) << "Sending host status for host ID " << check_host_status.host_id()
                 << ": " << host_status->status();
    sendMessage(proto::ROUTER_CHANNEL_ID_SESSION, *message);
}

} // namespace router
