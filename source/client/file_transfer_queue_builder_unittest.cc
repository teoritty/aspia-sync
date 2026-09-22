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

#include "client/file_transfer_queue_builder.h"

#include "common/file_task.h"
#include "common/file_task_consumer.h"
#include "common/file_task_consumer_proxy.h"
#include "proto/file_transfer.pb.h"

#include <map>
#include <optional>

#include <gtest/gtest.h>

namespace client {

namespace {

struct Entry
{
    std::string name;
    bool is_directory;
};

// Plays the other side: answers a directory listing with what it was told the directory holds,
// the way a host would - including a host that lies about the names.
class FakeRemote final : public common::FileTaskConsumer
{
public:
    void setListing(const std::string& path, std::vector<Entry> entries)
    {
        listings_[path] = std::move(entries);
    }

    void doTask(std::shared_ptr<common::FileTask> task) final
    {
        std::unique_ptr<proto::FileReply> reply = std::make_unique<proto::FileReply>();

        auto it = listings_.find(task->request().file_list_request().path());
        if (it == listings_.end())
        {
            reply->set_error_code(proto::FILE_ERROR_PATH_NOT_FOUND);
        }
        else
        {
            reply->set_error_code(proto::FILE_ERROR_SUCCESS);
            for (const auto& entry : it->second)
            {
                proto::FileList::Item* item = reply->mutable_file_list()->add_item();
                item->set_name(entry.name);
                item->set_is_directory(entry.is_directory);
                item->set_size(entry.is_directory ? 0 : 10);
            }
        }

        task->setReply(std::move(reply));
    }

private:
    std::map<std::string, std::vector<Entry>> listings_;
};

struct Result
{
    std::optional<proto::FileError> error;
    FileTransfer::TaskList queue;
};

Result build(FakeRemote* remote, const std::vector<FileTransfer::Item>& items)
{
    auto proxy = std::make_shared<common::FileTaskConsumerProxy>(remote);
    FileTransferQueueBuilder builder(proxy, common::FileTask::Target::REMOTE);

    Result result;
    builder.start("C:/Remote", "D:/Local", items,
                  [&result](proto::FileError error_code) { result.error = error_code; });

    result.queue = builder.takeQueue();
    proxy->dettach();
    return result;
}

} // namespace

//--------------------------------------------------------------------------------------------------
TEST(file_transfer_queue_builder_test, walks_a_directory_into_the_target)
{
    FakeRemote remote;
    remote.setListing("C:/Remote/docs", { { "a.txt", false }, { "sub", true } });
    remote.setListing("C:/Remote/docs/sub", { { "b.txt", false } });

    Result result = build(&remote, { FileTransfer::Item("docs", 0, true) });

    ASSERT_TRUE(result.error.has_value());
    EXPECT_EQ(*result.error, proto::FILE_ERROR_SUCCESS);

    ASSERT_EQ(result.queue.size(), 4u);
    EXPECT_EQ(result.queue[0].targetPath(), "D:/Local/docs");
    EXPECT_EQ(result.queue[1].targetPath(), "D:/Local/docs/a.txt");
    EXPECT_EQ(result.queue[2].targetPath(), "D:/Local/docs/sub");
    EXPECT_EQ(result.queue[3].targetPath(), "D:/Local/docs/sub/b.txt");
}

//--------------------------------------------------------------------------------------------------
// A name from the other side that climbs out of the directory being copied must never become a
// path on this side.
TEST(file_transfer_queue_builder_test, refuses_a_listing_that_climbs_out_of_the_directory)
{
    const std::vector<std::string> kBadNames =
    {
        "..",
        ".",
        "..\\..\\Startup\\x.exe",
        "../../Startup/x.exe",
        "C:\\Windows\\x.exe",
        "",
    };

    for (const auto& bad_name : kBadNames)
    {
        FakeRemote remote;
        remote.setListing("C:/Remote/docs", { { "a.txt", false }, { bad_name, false } });

        Result result = build(&remote, { FileTransfer::Item("docs", 0, true) });

        ASSERT_TRUE(result.error.has_value()) << bad_name;
        EXPECT_EQ(*result.error, proto::FILE_ERROR_INVALID_PATH_NAME) << bad_name;
        EXPECT_TRUE(result.queue.empty()) << bad_name;
    }
}

//--------------------------------------------------------------------------------------------------
TEST(file_transfer_queue_builder_test, refuses_a_bad_name_among_the_chosen_items)
{
    FakeRemote remote;

    Result result = build(&remote, { FileTransfer::Item("..\\x.exe", 10, false) });

    ASSERT_TRUE(result.error.has_value());
    EXPECT_EQ(*result.error, proto::FILE_ERROR_INVALID_PATH_NAME);
    EXPECT_TRUE(result.queue.empty());
}

//--------------------------------------------------------------------------------------------------
// Names that are unusual but legal on Windows still go through.
TEST(file_transfer_queue_builder_test, keeps_names_that_are_merely_unusual)
{
    FakeRemote remote;
    remote.setListing("C:/Remote/docs",
                      { { "\xD0\x9E\xD1\x82\xD1\x87\xD1\x91\xD1\x82 2024.docx", false }, // UTF-8.
                        { ".gitignore", false },
                        { "a..b", false },
                        { "name with spaces", true } });
    remote.setListing("C:/Remote/docs/name with spaces", {});

    Result result = build(&remote, { FileTransfer::Item("docs", 0, true) });

    ASSERT_TRUE(result.error.has_value());
    EXPECT_EQ(*result.error, proto::FILE_ERROR_SUCCESS);
    EXPECT_EQ(result.queue.size(), 5u);
}

} // namespace client
