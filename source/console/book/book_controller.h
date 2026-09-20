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

#ifndef CONSOLE_BOOK_BOOK_CONTROLLER_H
#define CONSOLE_BOOK_BOOK_CONTROLLER_H

#include "base/macros_magic.h"
#include "base/scoped_task_runner.h"
#include "base/threading/thread.h"
#include "base/waitable_timer.h"
#include "base/net/tcp_channel.h"
#include "client/router_config.h"

#include <memory>

namespace base {
class ClientAuthenticator;
} // namespace base

namespace proto {
class BookChanged;
class BookList;
class BookPull;
class BookPullRequest;
class BookPushRequest;
class BookPushResult;
} // namespace proto

namespace console {

// Carries the address book messages between this console and the router.
//
// It differs from the controller that connects to a host in staying connected: the router tells a
// console when somebody else changed something, and a connection opened only when this console has
// a question of its own would never hear that.
//
// It knows nothing about books. It connects, keeps the connection up, sends what it is given and
// hands back what arrives. Deciding what any of it means is the engine's part, and keeping the two
// apart is what makes the engine testable.
// The network lives on a thread of its own with an asio loop, because that is what a TcpChannel
// needs - it takes its io context from the loop of the thread it is made on, and the loop of the
// window is a Qt one. Everything handed back to the delegate is posted to the thread the window
// runs on, so the address book is only ever touched from one thread.
class BookController final
    : public base::TcpChannel::Listener,
      public base::Thread::Delegate
{
public:
    // |ui_task_runner| is the task runner of the thread the delegate lives on.
    BookController(const client::RouterConfig& router_config,
                   std::shared_ptr<base::TaskRunner> ui_task_runner);
    ~BookController() final;

    class Delegate
    {
    public:
        virtual ~Delegate() = default;

        virtual void onBookConnected() = 0;

        // The connection is gone. The controller reconnects by itself, so this is to be shown to
        // the person, not acted on.
        virtual void onBookDisconnected() = 0;

        // Authentication was refused. Unlike a broken connection this does not right itself, so
        // the controller stops trying and waits to be told what to do.
        virtual void onBookAuthFailed() = 0;

        virtual void onBookList(const proto::BookList& message) = 0;
        virtual void onBookPull(const proto::BookPull& message) = 0;
        virtual void onBookPushResult(const proto::BookPushResult& message) = 0;

        // Somebody else changed something. It carries no content: what to do about it is to ask
        // for what is missing.
        virtual void onBookChanged(const proto::BookChanged& message) = 0;
    };

    void start(Delegate* delegate);
    void stop();

    bool isConnected() const;

    void requestBookList(int64_t request_id);
    void requestPull(const proto::BookPullRequest& request);
    void requestPush(const proto::BookPushRequest& request);

protected:
    // base::Thread::Delegate implementation.
    void onBeforeThreadRunning() final;
    void onAfterThreadRunning() final;

    // base::TcpChannel::Listener implementation.
    void onTcpConnected() final;
    void onTcpDisconnected(base::NetworkChannel::ErrorCode error_code) final;
    void onTcpMessageReceived(uint8_t channel_id, const base::ByteArray& buffer) final;
    void onTcpMessageWritten(uint8_t channel_id, base::ByteArray&& buffer, size_t pending) final;

private:
    void connectToRouter();
    void scheduleReconnect();
    void send(const google::protobuf::MessageLite& message);

    // Growing pause between attempts. Seven consoles all coming back the moment a restarted router
    // answers would arrive together and be refused together, and a fixed pause would only make
    // them do it again in step.
    static const int kMinReconnectSeconds = 5;
    static const int kMaxReconnectSeconds = 300;

    const client::RouterConfig router_config_;

    base::Thread io_thread_;
    std::shared_ptr<base::TaskRunner> io_task_runner_;
    base::ScopedTaskRunner ui_task_runner_;

    std::unique_ptr<base::TcpChannel> channel_;
    std::unique_ptr<base::ClientAuthenticator> authenticator_;
    std::unique_ptr<base::WaitableTimer> reconnect_timer_;

    Delegate* delegate_ = nullptr;
    bool connected_ = false;
    bool stopped_ = false;
    int reconnect_seconds_ = kMinReconnectSeconds;

    DISALLOW_COPY_AND_ASSIGN(BookController);
};

} // namespace console

#endif // CONSOLE_BOOK_BOOK_CONTROLLER_H
