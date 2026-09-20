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

#include "base/files/file_util.h"

#include "base/macros_magic.h"
#include "build/build_config.h"

#include <gtest/gtest.h>

#include <string>

#if defined(OS_WIN)
#include "base/win/scoped_object.h"
#include <Windows.h>
#endif // defined(OS_WIN)

namespace base {

namespace {

// A directory of its own for each test, removed with everything in it afterwards. The tests write
// real files: what is being checked is the behaviour of the file system, and a mock of it would
// only check that the mock agrees with itself.
class ScopedTestDir
{
public:
    ScopedTestDir()
    {
        static int counter = 0;
        path_ = std::filesystem::temp_directory_path() /
            ("aspia_file_util_test_" + std::to_string(++counter) + "_" +
             std::to_string(reinterpret_cast<uintptr_t>(this)));

        std::error_code error_code;
        std::filesystem::remove_all(path_, error_code);
        std::filesystem::create_directories(path_, error_code);
    }

    ~ScopedTestDir()
    {
        std::error_code error_code;
        std::filesystem::permissions(path_, std::filesystem::perms::all,
                                     std::filesystem::perm_options::add, error_code);
        std::filesystem::remove_all(path_, error_code);
    }

    std::filesystem::path file(std::string_view name) const { return path_ / name; }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;

    DISALLOW_COPY_AND_ASSIGN(ScopedTestDir);
};

std::string readBack(const std::filesystem::path& path)
{
    std::string content;
    if (!readFile(path, &content))
        return std::string();
    return content;
}

int fileCount(const std::filesystem::path& dir)
{
    int count = 0;
    std::error_code error_code;
    for (auto it = std::filesystem::directory_iterator(dir, error_code);
         it != std::filesystem::directory_iterator(); ++it)
    {
        ++count;
    }
    return count;
}

} // namespace

//--------------------------------------------------------------------------------------------------
TEST(file_util_test, atomic_write_creates_file)
{
    ScopedTestDir dir;
    const std::filesystem::path path = dir.file("book.aab");

    ASSERT_TRUE(writeFileAtomically(path, std::string_view("content")));

    EXPECT_TRUE(std::filesystem::exists(path));
    EXPECT_EQ(readBack(path), "content");
}

//--------------------------------------------------------------------------------------------------
TEST(file_util_test, atomic_write_replaces_content)
{
    ScopedTestDir dir;
    const std::filesystem::path path = dir.file("book.aab");

    ASSERT_TRUE(writeFileAtomically(path, std::string_view("old content, longer")));
    ASSERT_TRUE(writeFileAtomically(path, std::string_view("new")));

    EXPECT_EQ(readBack(path), "new");
}

//--------------------------------------------------------------------------------------------------
TEST(file_util_test, atomic_write_accepts_empty_content)
{
    ScopedTestDir dir;
    const std::filesystem::path path = dir.file("book.aab");

    // Not a default-constructed view: that one carries no pointer at all, which is the separate
    // case checked by atomic_write_refuses_null_data.
    ASSERT_TRUE(writeFileAtomically(path, std::string_view("")));

    EXPECT_TRUE(std::filesystem::exists(path));
    EXPECT_EQ(std::filesystem::file_size(path), 0u);
}

//--------------------------------------------------------------------------------------------------
// The temporary the write goes through must not survive it. A leftover would accumulate next to
// the address book and, worse, would be taken for a book by anyone looking at the directory.
TEST(file_util_test, atomic_write_leaves_no_temporary)
{
    ScopedTestDir dir;

    ASSERT_TRUE(writeFileAtomically(dir.file("book.aab"), std::string_view("content")));

    EXPECT_EQ(fileCount(dir.path()), 1);
}

//--------------------------------------------------------------------------------------------------
// The point of the whole exercise: a write that fails must leave what was there before. The
// previous implementation opened the target for writing and truncated it before it had anything
// to put in its place, so an interrupted save destroyed the address book.
TEST(file_util_test, atomic_write_keeps_original_when_replace_fails)
{
    ScopedTestDir dir;
    const std::filesystem::path path = dir.file("book.aab");

    ASSERT_TRUE(writeFileAtomically(path, std::string_view("original")));

    // Holding the target open without sharing it is what makes the rename fail: the content has
    // been written and flushed by then, so the failure lands exactly on the step this test is
    // about. Marking the file read-only does not do it - the rename replaces it anyway.
    win::ScopedHandle lock(CreateFileW(path.c_str(),
                                       GENERIC_READ,
                                       0, // No sharing.
                                       nullptr,
                                       OPEN_EXISTING,
                                       FILE_ATTRIBUTE_NORMAL,
                                       nullptr));
    ASSERT_TRUE(lock.isValid());

    EXPECT_FALSE(writeFileAtomically(path, std::string_view("replacement")));

    // And it must not leave its temporary behind on the way out either.
    EXPECT_EQ(fileCount(dir.path()), 1);

    lock.reset();
    EXPECT_EQ(readBack(path), "original");
}

//--------------------------------------------------------------------------------------------------
TEST(file_util_test, atomic_write_refuses_missing_directory)
{
    ScopedTestDir dir;

    EXPECT_FALSE(writeFileAtomically(dir.path() / "absent" / "book.aab",
                                     std::string_view("content")));
}

//--------------------------------------------------------------------------------------------------
TEST(file_util_test, atomic_write_refuses_null_data)
{
    ScopedTestDir dir;
    const std::filesystem::path path = dir.file("book.aab");

    EXPECT_FALSE(writeFileAtomically(path, nullptr, 16));
    EXPECT_FALSE(std::filesystem::exists(path));
}

} // namespace base
