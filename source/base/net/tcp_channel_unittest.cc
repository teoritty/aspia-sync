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

#include "base/net/tcp_channel.h"

#include "base/message_loop/message_loop.h"
#include "base/net/tcp_server.h"
#include "base/task_runner.h"

#include <optional>

#include <gtest/gtest.h>

namespace base {

namespace {

// A port on the loopback interface only; nothing outside the machine can reach it.
const uint16_t kPort = 38561;

struct Outcome
{
    std::optional<size_t> received_size;
    bool disconnected = false;
};

// Takes the one connection the test makes and reports what came of it: a message, or the channel
// giving up on it.
class Receiver final
    : public TcpServer::Delegate,
      public TcpChannel::Listener
{
public:
    Receiver(bool authenticated, Outcome* outcome)
        : authenticated_(authenticated),
          outcome_(outcome)
    {
        // Nothing
    }

    void onNewConnection(std::unique_ptr<TcpChannel> channel) final
    {
        channel_ = std::move(channel);
        if (authenticated_)
            channel_->setAuthenticated();
        channel_->setListener(this);
        channel_->resume();
    }

    void onTcpConnected() final {}

    void onTcpDisconnected(NetworkChannel::ErrorCode /* error_code */) final
    {
        outcome_->disconnected = true;
        MessageLoop::current()->taskRunner()->postQuit();
    }

    void onTcpMessageReceived(uint8_t /* channel_id */, const ByteArray& buffer) final
    {
        outcome_->received_size = buffer.size();
        MessageLoop::current()->taskRunner()->postQuit();
    }

    void onTcpMessageWritten(uint8_t /* channel_id */, ByteArray&& /* buffer */,
                             size_t /* pending */) final {}

private:
    const bool authenticated_;
    Outcome* outcome_;
    std::unique_ptr<TcpChannel> channel_;
};

// Connects and sends one message of the given size as soon as the connection is up.
class Sender final : public TcpChannel::Listener
{
public:
    Sender(bool authenticated, size_t size)
        : authenticated_(authenticated),
          size_(size)
    {
        // Nothing
    }

    void start()
    {
        channel_ = std::make_unique<TcpChannel>();
        channel_->setListener(this);
        channel_->connect(u"127.0.0.1", kPort);
    }

    void onTcpConnected() final
    {
        if (authenticated_)
            channel_->setAuthenticated();
        channel_->resume();
        channel_->send(0, ByteArray(size_, 'x'));
    }

    void onTcpDisconnected(NetworkChannel::ErrorCode /* error_code */) final {}
    void onTcpMessageReceived(uint8_t /* channel_id */, const ByteArray& /* buffer */) final {}
    void onTcpMessageWritten(uint8_t /* channel_id */, ByteArray&& /* buffer */,
                             size_t /* pending */) final {}

private:
    const bool authenticated_;
    const size_t size_;
    std::unique_ptr<TcpChannel> channel_;
};

Outcome exchange(bool sender_authenticated, bool receiver_authenticated, size_t size)
{
    MessageLoop message_loop(MessageLoop::Type::ASIO);

    Outcome outcome;
    Receiver receiver(receiver_authenticated, &outcome);

    TcpServer server;
    server.start(u"127.0.0.1", kPort, &receiver);

    Sender sender(sender_authenticated, size);
    sender.start();

    // Whatever happens, the test does not hang.
    message_loop.taskRunner()->postDelayedTask(
        [runner = message_loop.taskRunner()]() { runner->postQuit(); },
        std::chrono::seconds(10));

    message_loop.run();
    server.stop();
    return outcome;
}

} // namespace

//--------------------------------------------------------------------------------------------------
TEST(tcp_channel_test, a_small_message_passes_before_authentication)
{
    Outcome outcome = exchange(false, false, 1024);

    ASSERT_TRUE(outcome.received_size.has_value());
    EXPECT_EQ(*outcome.received_size, 1024u);
}

//--------------------------------------------------------------------------------------------------
// A message just under the limit: authentication messages have to keep fitting.
TEST(tcp_channel_test, a_message_at_the_limit_passes_before_authentication)
{
    Outcome outcome = exchange(false, false, NetworkChannel::kMaxAuthMessageSize - 64);

    ASSERT_TRUE(outcome.received_size.has_value());
    EXPECT_EQ(*outcome.received_size, NetworkChannel::kMaxAuthMessageSize - 64);
}

//--------------------------------------------------------------------------------------------------
// The side that has not authenticated the peer drops a large message instead of reading it.
TEST(tcp_channel_test, a_large_message_is_refused_before_authentication)
{
    Outcome outcome = exchange(true, false, 64 * 1024);

    EXPECT_FALSE(outcome.received_size.has_value());
    EXPECT_TRUE(outcome.disconnected);
}

//--------------------------------------------------------------------------------------------------
// And does not send one either.
TEST(tcp_channel_test, a_large_message_is_not_sent_before_authentication)
{
    Outcome outcome = exchange(false, true, 64 * 1024);

    EXPECT_FALSE(outcome.received_size.has_value());
}

//--------------------------------------------------------------------------------------------------
// After authentication the limit is the usual one: a megabyte, as a page of a shared book can be.
TEST(tcp_channel_test, a_large_message_passes_after_authentication)
{
    Outcome outcome = exchange(true, true, 1024 * 1024);

    ASSERT_TRUE(outcome.received_size.has_value());
    EXPECT_EQ(*outcome.received_size, 1024u * 1024u);
}

} // namespace base
