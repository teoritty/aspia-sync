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

#include "router/server.h"

#include "base/logging.h"
#include "base/stl_util.h"
#include "base/task_runner.h"
#include "base/crypto/key_pair.h"
#include "base/crypto/random.h"
#include "base/files/base_paths.h"
#include "base/files/file_util.h"
#include "base/net/tcp_channel.h"
#include "router/book/book_store.h"
#include "router/database_factory_sqlite.h"
#include "router/database_sqlite.h"
#include "router/session_admin.h"
#include "router/session_client.h"
#include "router/session_host.h"
#include "router/session_relay.h"
#include "router/settings.h"
#include "router/user_list_db.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <limits>

namespace router {

namespace {

//--------------------------------------------------------------------------------------------------
const char* sessionTypeToString(proto::RouterSession session_type)
{
    switch (session_type)
    {
        case proto::ROUTER_SESSION_CLIENT:
            return "ROUTER_SESSION_CLIENT";

        case proto::ROUTER_SESSION_HOST:
            return "ROUTER_SESSION_HOST";

        case proto::ROUTER_SESSION_ADMIN:
            return "ROUTER_SESSION_ADMIN";

        case proto::ROUTER_SESSION_RELAY:
            return "ROUTER_SESSION_RELAY";

        default:
            return "ROUTER_SESSION_UNKNOWN";
    }
}

} // namespace

//--------------------------------------------------------------------------------------------------
Server::Server(std::shared_ptr<base::TaskRunner> task_runner)
    : task_runner_(std::move(task_runner)),
      database_factory_(base::make_local_shared<DatabaseFactorySqlite>())
{
    LOG(LS_INFO) << "Ctor";
    DCHECK(task_runner_);
}

//--------------------------------------------------------------------------------------------------
Server::~Server()
{
    LOG(LS_INFO) << "Dtor";
}

//--------------------------------------------------------------------------------------------------
bool Server::start()
{
    if (server_)
    {
        LOG(LS_ERROR) << "Server already started";
        return false;
    }

    std::unique_ptr<Database> database = database_factory_->openDatabase();
    if (!database)
    {
        LOG(LS_ERROR) << "Failed to open the database";
        return false;
    }

    Settings settings;

    base::ByteArray private_key = settings.privateKey();
    if (private_key.empty())
    {
        LOG(LS_INFO) << "The private key is not specified in the configuration file";
        return false;
    }

    std::u16string listen_interface = settings.listenInterface();
    if (!base::TcpServer::isValidListenInterface(listen_interface))
    {
        LOG(LS_ERROR) << "Invalid listen interface address";
        return false;
    }

    uint16_t port = settings.port();
    if (!port)
    {
        LOG(LS_ERROR) << "Invalid port specified in configuration file";
        return false;
    }

    client_white_list_ = settings.clientWhiteList();
    if (client_white_list_.empty())
    {
        LOG(LS_INFO) << "Empty client white list. Connections from all clients will be allowed";
    }
    else
    {
        LOG(LS_INFO) << "Client white list is not empty. Allowed clients:";

        for (size_t i = 0; i < client_white_list_.size(); ++i)
            LOG(LS_INFO) << "#" << (i + 1) << ": " << client_white_list_[i];
    }

    host_white_list_ = settings.hostWhiteList();
    if (host_white_list_.empty())
    {
        LOG(LS_INFO) << "Empty host white list. Connections from all hosts will be allowed";
    }
    else
    {
        LOG(LS_INFO) << "Host white list is not empty. Allowed hosts:";

        for (size_t i = 0; i < host_white_list_.size(); ++i)
            LOG(LS_INFO) << "#" << (i + 1) << ": " << host_white_list_[i];
    }

    admin_white_list_ = settings.adminWhiteList();
    if (admin_white_list_.empty())
    {
        LOG(LS_INFO) << "Empty admin white list. Connections from all admins will be allowed";
    }
    else
    {
        LOG(LS_INFO) << "Admin white list is not empty. Allowed admins:";

        for (size_t i = 0; i < admin_white_list_.size(); ++i)
            LOG(LS_INFO) << "#" << (i + 1) << ": " << admin_white_list_[i];
    }

    relay_white_list_ = settings.relayWhiteList();
    if (relay_white_list_.empty())
    {
        LOG(LS_INFO) << "Empty relay white list. Connections from all relays will be allowed";
    }
    else
    {
        LOG(LS_INFO) << "Relay white list is not empty. Allowed relays:";

        for (size_t i = 0; i < relay_white_list_.size(); ++i)
            LOG(LS_INFO) << "#" << (i + 1) << ": " << relay_white_list_[i];
    }

    book_history_policy_.days = settings.bookHistoryDays();
    book_history_policy_.max_changes = settings.bookHistoryMaxChanges();

    if (book_history_policy_.enabled())
    {
        LOG(LS_INFO) << "Address book history is kept for " << book_history_policy_.days
                     << " days, at most " << book_history_policy_.max_changes << " changes";
    }
    else
    {
        LOG(LS_INFO) << "Address book history is not kept";
    }

    base::ByteArray seed_key = settings.seedKey();
    if (seed_key.empty())
    {
        LOG(LS_INFO) << "Empty seed key. New key generated";
        seed_key = base::Random::byteArray(64);
        settings.setSeedKey(seed_key);
    }

    std::unique_ptr<base::UserListBase> user_list = UserListDb::open(*database_factory_);
    user_list->setSeedKey(seed_key);

    authenticator_manager_ =
        std::make_unique<base::ServerAuthenticatorManager>(task_runner_, this);
    authenticator_manager_->setPrivateKey(private_key);
    authenticator_manager_->setUserList(std::move(user_list));
    authenticator_manager_->setAnonymousAccess(
        base::ServerAuthenticator::AnonymousAccess::ENABLE,
        proto::ROUTER_SESSION_HOST | proto::ROUTER_SESSION_RELAY);

    relay_key_pool_ = std::make_unique<SharedKeyPool>(this);

    server_ = std::make_unique<base::TcpServer>();
    server_->start(listen_interface, port, this);

    // And the sweep of what the shared books no longer need, which then repeats daily.
    pruneBooks();

    LOG(LS_INFO) << "Server started";
    return true;
}

//--------------------------------------------------------------------------------------------------
void Server::pruneBooks()
{
    std::unique_ptr<BookStore> store = BookStore::open(DatabaseSqlite::filePath());
    if (store)
    {
        const int64_t now = static_cast<int64_t>(std::time(nullptr));
        const int64_t day = 24 * 60 * 60;

        // A record deleted within the history must still have its headstone: putting it back is
        // written over the headstone, and the router takes that only from somebody who names it.
        // So headstones live at least as long as the history does, and a day more.
        int64_t tombstone_retention = kTombstoneRetentionSeconds;
        if (book_history_policy_.enabled())
        {
            tombstone_retention = std::max(tombstone_retention,
                (static_cast<int64_t>(book_history_policy_.days) + 1) * day);
        }

        // With the history switched off, what it holds goes too. Somebody who turned it off to
        // make room would not expect the room to stay taken.
        const int64_t history_before = book_history_policy_.enabled()
            ? now - static_cast<int64_t>(book_history_policy_.days) * day
            : std::numeric_limits<int64_t>::max();

        std::vector<Book> books;
        if (store->bookList(&books))
        {
            int64_t removed = 0;
            int64_t history_removed = 0;

            for (const Book& book : books)
            {
                removed += store->pruneTombstones(book.guid, now - tombstone_retention);
                history_removed += store->pruneHistory(
                    book.guid, history_before, book_history_policy_.max_changes);
            }

            if (removed != 0)
                LOG(LS_INFO) << "Headstones removed: " << removed;

            if (history_removed != 0)
                LOG(LS_INFO) << "Changes removed from the history: " << history_removed;
        }
        else
        {
            LOG(LS_ERROR) << "Unable to list the shared books";
        }

        store->pruneAppliedOps(now - kAppliedOpRetentionSeconds);
    }
    else
    {
        // Not fatal. Nothing is lost by a sweep that did not happen; the next one is a day away.
        LOG(LS_ERROR) << "Unable to open the address book store to clean it up";
    }

    task_runner_->postDelayedTask(std::bind(&Server::pruneBooks, this), std::chrono::hours(24));
}

//--------------------------------------------------------------------------------------------------
void Server::onBookChanged(const std::string& book_guid, int64_t revision,
                           Session::SessionId origin_session_id)
{
    for (const auto& session : sessions_)
    {
        if (session->sessionType() != proto::ROUTER_SESSION_CLIENT)
            continue;

        // The console that made the change already has the answer to its own request; telling it
        // again would only make it ask for what it just sent.
        if (session->sessionId() == origin_session_id)
            continue;

        static_cast<SessionClient*>(session.get())->onBookChanged(book_guid, revision);
    }
}

//--------------------------------------------------------------------------------------------------
std::unique_ptr<proto::SessionList> Server::sessionList() const
{
    std::unique_ptr<proto::SessionList> result = std::make_unique<proto::SessionList>();

    for (const auto& session : sessions_)
    {
        proto::Session* item = result->add_session();

        item->set_session_id(session->sessionId());
        item->set_session_type(session->sessionType());
        item->set_timepoint(static_cast<uint64_t>(session->startTime()));
        item->set_ip_address(session->address());
        item->mutable_version()->CopyFrom(session->version().toProto());
        item->set_os_name(session->osName());
        item->set_computer_name(session->computerName());
        item->set_architecture(session->architecture());

        switch (session->sessionType())
        {
            case proto::ROUTER_SESSION_HOST:
            {
                proto::HostSessionData session_data;

                for (const auto& host_id : static_cast<SessionHost*>(session.get())->hostIdList())
                    session_data.add_host_id(host_id);

                item->set_session_data(session_data.SerializeAsString());
            }
            break;

            case proto::ROUTER_SESSION_RELAY:
            {
                proto::RelaySessionData session_data;
                session_data.set_pool_size(relay_key_pool_->countForRelay(session->sessionId()));

                const std::optional<proto::RelayStat>& in_relay_stat =
                    static_cast<SessionRelay*>(session.get())->relayStat();
                if (in_relay_stat.has_value())
                {
                    proto::RelaySessionData::RelayStat* out_relay_stat =
                        session_data.mutable_relay_stat();

                    out_relay_stat->set_uptime(in_relay_stat->uptime());
                    out_relay_stat->mutable_peer_connection()->CopyFrom(
                        in_relay_stat->peer_connection());
                }

                item->set_session_data(session_data.SerializeAsString());
            }
            break;

            default:
                break;
        }
    }

    result->set_error_code(proto::SessionList::SUCCESS);
    return result;
}

//--------------------------------------------------------------------------------------------------
bool Server::stopSession(Session::SessionId session_id)
{
    for (auto it = sessions_.begin(); it != sessions_.end(); ++it)
    {
        if (it->get()->sessionId() == session_id)
        {
            sessions_.erase(it);
            return true;
        }
    }

    return false;
}

//--------------------------------------------------------------------------------------------------
void Server::onHostSessionWithId(SessionHost* session)
{
    if (!session)
    {
        LOG(LS_ERROR) << "Invalid session pointer";
        return;
    }

    for (auto it = sessions_.begin(); it != sessions_.end();)
    {
        Session* other_session_ptr = it->get();

        if (!other_session_ptr || other_session_ptr->sessionType() != proto::ROUTER_SESSION_HOST)
        {
            ++it;
            continue;
        }

        SessionHost* other_session = reinterpret_cast<SessionHost*>(other_session_ptr);
        if (other_session == session)
        {
            ++it;
            continue;
        }

        // The session is erased once, after the walk over its IDs: erasing it inside that walk
        // left the walk reading the session just destroyed, and the outer loop stepping past the
        // iterator that erase() had returned.
        bool is_found = false;

        for (const auto& host_id : session->hostIdList())
        {
            if (other_session->hasHostId(host_id))
            {
                LOG(LS_INFO) << "Detected previous connection with ID " << host_id;

                is_found = true;
                break;
            }
        }

        if (is_found)
            it = sessions_.erase(it);
        else
            ++it;
    }
}

//--------------------------------------------------------------------------------------------------
SessionHost* Server::hostSessionById(base::HostId host_id)
{
    for (auto it = sessions_.begin(); it != sessions_.end(); ++it)
    {
        Session* entry = it->get();

        if (entry->sessionType() == proto::ROUTER_SESSION_HOST &&
            static_cast<SessionHost*>(entry)->hasHostId(host_id))
        {
            return static_cast<SessionHost*>(entry);
        }
    }

    return nullptr;
}

//--------------------------------------------------------------------------------------------------
Session* Server::sessionById(Session::SessionId session_id)
{
    for (auto& session : sessions_)
    {
        if (session->sessionId() == session_id)
            return session.get();
    }

    return nullptr;
}

//--------------------------------------------------------------------------------------------------
void Server::onNewConnection(std::unique_ptr<base::TcpChannel> channel)
{
    LOG(LS_INFO) << "New connection: " << channel->peerAddress();

    channel->setKeepAlive(true);
    channel->setNoDelay(true);

    if (authenticator_manager_)
        authenticator_manager_->addNewChannel(std::move(channel));
    else
        LOG(LS_ERROR) << "Authenticator not available";
}

//--------------------------------------------------------------------------------------------------
void Server::onPoolKeyUsed(Session::SessionId session_id, uint32_t key_id)
{
    for (const auto& session : sessions_)
    {
        SessionRelay* relay_session = static_cast<SessionRelay*>(session.get());
        if (relay_session->sessionId() == session_id)
            relay_session->sendKeyUsed(key_id);
    }
}

//--------------------------------------------------------------------------------------------------
void Server::onNewSession(base::ServerAuthenticatorManager::SessionInfo&& session_info)
{
    std::u16string address = session_info.channel->peerAddress();
    proto::RouterSession session_type =
        static_cast<proto::RouterSession>(session_info.session_type);

    LOG(LS_INFO) << "New session: " << sessionTypeToString(session_type) << " (" << address << ")";

    if (session_info.version >= base::Version::kVersion_2_6_0)
    {
        LOG(LS_INFO) << "Using channel id support";
        session_info.channel->setChannelIdSupport(true);
    }

    std::unique_ptr<Session> session;

    switch (session_info.session_type)
    {
        case proto::ROUTER_SESSION_CLIENT:
        {
            if (!client_white_list_.empty() && !base::contains(client_white_list_, address))
                break;

            session = std::make_unique<SessionClient>();
        }
        break;

        case proto::ROUTER_SESSION_HOST:
        {
            if (!host_white_list_.empty() && !base::contains(host_white_list_, address))
                break;

            session = std::make_unique<SessionHost>();
        }
        break;

        case proto::ROUTER_SESSION_ADMIN:
        {
            if (!admin_white_list_.empty() && !base::contains(admin_white_list_, address))
                break;

            session = std::make_unique<SessionAdmin>();
        }
        break;

        case proto::ROUTER_SESSION_RELAY:
        {
            if (!relay_white_list_.empty() && !base::contains(relay_white_list_, address))
                break;

            session = std::make_unique<SessionRelay>();
        }
        break;

        default:
        {
            LOG(LS_ERROR) << "Unsupported session type: "
                          << static_cast<int>(session_info.session_type);
        }
        break;
    }

    if (!session)
    {
        LOG(LS_ERROR) << "Connection rejected for '" << address << "'";
        return;
    }

    session->setChannel(std::move(session_info.channel));
    session->setDatabaseFactory(database_factory_);
    session->setServer(this);
    session->setRelayKeyPool(relay_key_pool_->share());
    session->setVersion(session_info.version);
    session->setOsName(session_info.os_name);
    session->setComputerName(session_info.computer_name);
    session->setArchitecture(session_info.architecture);
    session->setUserName(session_info.user_name);

    sessions_.emplace_back(std::move(session));
    sessions_.back()->start(this);
}

//--------------------------------------------------------------------------------------------------
void Server::onSessionFinished(Session::SessionId session_id, proto::RouterSession /* session_type */)
{
    for (auto it = sessions_.begin(); it != sessions_.end(); ++it)
    {
        if (it->get()->sessionId() == session_id)
        {
            // Session will be destroyed after completion of the current call.
            task_runner_->deleteSoon(std::move(*it));

            // Delete a session from the list.
            sessions_.erase(it);
            break;
        }
    }
}

} // namespace router
