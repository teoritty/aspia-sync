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

#include "console/book/book_controller.h"

#include "base/logging.h"
#include "base/peer/client_authenticator.h"
#include "proto/router_book.pb.h"
#include "proto/router_common.pb.h"

#include <algorithm>
#include <chrono>

namespace console {

//--------------------------------------------------------------------------------------------------
BookController::BookController(const client::RouterConfig& router_config,
                               std::shared_ptr<base::TaskRunner> task_runner)
    : router_config_(router_config),
      task_runner_(std::move(task_runner)),
      reconnect_timer_(base::WaitableTimer::Type::SINGLE_SHOT, task_runner_)
{
    LOG(LS_INFO) << "Ctor";
}

//--------------------------------------------------------------------------------------------------
BookController::~BookController()
{
    LOG(LS_INFO) << "Dtor";
    stop();
}

//--------------------------------------------------------------------------------------------------
void BookController::start(Delegate* delegate)
{
    DCHECK(delegate);

    delegate_ = delegate;
    stopped_ = false;
    reconnect_seconds_ = kMinReconnectSeconds;

    connectToRouter();
}

//--------------------------------------------------------------------------------------------------
void BookController::stop()
{
    stopped_ = true;
    connected_ = false;
    delegate_ = nullptr;

    reconnect_timer_.stop();
    authenticator_.reset();
    channel_.reset();
}

//--------------------------------------------------------------------------------------------------
bool BookController::isConnected() const
{
    return connected_;
}

//--------------------------------------------------------------------------------------------------
void BookController::connectToRouter()
{
    if (stopped_)
        return;

    LOG(LS_INFO) << "Connecting to the router for the address book";

    channel_ = std::make_unique<base::TcpChannel>();
    channel_->setListener(this);
    channel_->connect(router_config_.address, router_config_.port);
}

//--------------------------------------------------------------------------------------------------
void BookController::scheduleReconnect()
{
    if (stopped_)
        return;

    LOG(LS_INFO) << "Reconnecting in " << reconnect_seconds_ << " seconds";

    reconnect_timer_.start(std::chrono::seconds(reconnect_seconds_), [this]()
    {
        connectToRouter();
    });

    // Seven consoles all coming back the moment a restarted router answers would arrive together
    // and be refused together; a fixed pause would only make them do it again in step.
    reconnect_seconds_ = std::min(reconnect_seconds_ * 2, kMaxReconnectSeconds);
}

//--------------------------------------------------------------------------------------------------
void BookController::onTcpConnected()
{
    LOG(LS_INFO) << "Connected to the router";

    channel_->setKeepAlive(true);
    channel_->setNoDelay(true);

    authenticator_ = std::make_unique<base::ClientAuthenticator>(task_runner_);

    authenticator_->setIdentify(proto::IDENTIFY_SRP);
    authenticator_->setUserName(router_config_.username);
    authenticator_->setPassword(router_config_.password);
    authenticator_->setSessionType(proto::ROUTER_SESSION_CLIENT);

    authenticator_->start(std::move(channel_),
                          [this](base::ClientAuthenticator::ErrorCode error_code)
    {
        if (error_code != base::ClientAuthenticator::ErrorCode::SUCCESS)
        {
            LOG(LS_ERROR) << "Authentication for the address book failed: "
                          << base::Authenticator::errorToString(error_code);

            // A refused password does not right itself, so there is no point in coming back every
            // few seconds to be refused again. The person is told and the controller waits.
            connected_ = false;

            if (delegate_)
                delegate_->onBookAuthFailed();
            return;
        }

        channel_ = authenticator_->takeChannel();
        channel_->setListener(this);

        // The book travels on a channel of its own, which is only possible where the protocol
        // supports them at all.
        channel_->setChannelIdSupport(true);

        connected_ = true;
        reconnect_seconds_ = kMinReconnectSeconds;

        channel_->resume();

        LOG(LS_INFO) << "Address book channel is open";

        if (delegate_)
            delegate_->onBookConnected();
    });
}

//--------------------------------------------------------------------------------------------------
void BookController::onTcpDisconnected(base::NetworkChannel::ErrorCode error_code)
{
    LOG(LS_INFO) << "Address book connection closed: " << base::NetworkChannel::errorToString(
        error_code);

    const bool was_connected = connected_;
    connected_ = false;

    if (delegate_ && was_connected)
        delegate_->onBookDisconnected();

    scheduleReconnect();
}

//--------------------------------------------------------------------------------------------------
void BookController::onTcpMessageReceived(uint8_t channel_id, const base::ByteArray& buffer)
{
    if (channel_id != proto::ROUTER_CHANNEL_ID_BOOK)
    {
        // Anything else belongs to a part of the protocol this controller has nothing to do with.
        return;
    }

    proto::RouterToBookClient message;
    if (!base::parse(buffer, &message))
    {
        LOG(LS_ERROR) << "Could not read an address book message from the router";
        return;
    }

    if (!delegate_)
        return;

    if (message.has_book_list())
        delegate_->onBookList(message.book_list());
    else if (message.has_book_pull())
        delegate_->onBookPull(message.book_pull());
    else if (message.has_book_push_result())
        delegate_->onBookPushResult(message.book_push_result());
    else if (message.has_book_changed())
        delegate_->onBookChanged(message.book_changed());
    else
        LOG(LS_ERROR) << "Unhandled address book message from the router";
}

//--------------------------------------------------------------------------------------------------
void BookController::onTcpMessageWritten(
    uint8_t /* channel_id */, base::ByteArray&& /* buffer */, size_t /* pending */)
{
    // Nothing
}

//--------------------------------------------------------------------------------------------------
void BookController::send(const google::protobuf::MessageLite& message)
{
    if (!connected_ || !channel_)
    {
        LOG(LS_ERROR) << "Attempt to send an address book message while not connected";
        return;
    }

    channel_->send(proto::ROUTER_CHANNEL_ID_BOOK, base::serialize(message));
}

//--------------------------------------------------------------------------------------------------
void BookController::requestBookList(int64_t request_id)
{
    proto::BookClientToRouter message;
    message.mutable_book_list_request()->set_request_id(request_id);
    send(message);
}

//--------------------------------------------------------------------------------------------------
void BookController::requestPull(const proto::BookPullRequest& request)
{
    proto::BookClientToRouter message;
    message.mutable_book_pull_request()->CopyFrom(request);
    send(message);
}

//--------------------------------------------------------------------------------------------------
void BookController::requestPush(const proto::BookPushRequest& request)
{
    proto::BookClientToRouter message;
    message.mutable_book_push_request()->CopyFrom(request);
    send(message);
}

} // namespace console
