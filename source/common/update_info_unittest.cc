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

#include "common/update_info.h"

#include "base/strings/unicode.h"

#include <gtest/gtest.h>

namespace common {

namespace {

const char kRepository[] = "teoritty/aspia-sync";

base::ByteArray bytes(std::string_view text)
{
    return base::ByteArray(text.begin(), text.end());
}

std::string release(std::string_view tag, std::string_view assets, std::string_view extra = "")
{
    return "{\"tag_name\":\"" + std::string(tag) + "\",\"draft\":false,\"prerelease\":false," +
           std::string(extra) + "\"body\":\"Fixes <things> & more\",\"assets\":[" +
           std::string(assets) + "]}";
}

std::string asset(std::string_view name, std::string_view url)
{
    return "{\"name\":\"" + std::string(name) + "\",\"browser_download_url\":\"" +
           std::string(url) + "\"}";
}

const char kHostUrl[] =
    "https://github.com/teoritty/aspia-sync/releases/download/v2.7.1/aspia-host-2.7.1-x64.msi";

} // namespace

TEST(UpdateInfoTest, GitHubRepository)
{
    EXPECT_EQ(gitHubRepository(u"https://github.com/teoritty/aspia-sync"), kRepository);
    EXPECT_EQ(gitHubRepository(u"https://github.com/Teoritty/Aspia-Sync/"), kRepository);
    EXPECT_EQ(gitHubRepository(u"https://github.com/teoritty/aspia-sync.git"), kRepository);

    EXPECT_EQ(gitHubRepository(u"https://update.aspia.net"), "");
    EXPECT_EQ(gitHubRepository(u"http://github.com/teoritty/aspia-sync"), "");
    EXPECT_EQ(gitHubRepository(u"https://github.com/teoritty"), "");
    EXPECT_EQ(gitHubRepository(u"https://github.com/teoritty/aspia-sync/releases"), "");
    EXPECT_EQ(gitHubRepository(u"https://github.com/teoritty/aspia?x=1"), "");
    EXPECT_EQ(gitHubRepository(u"https://github.com/../aspia"), "");
}

TEST(UpdateInfoTest, PicksTheInstallerOfThePackage)
{
    std::string json = release("v2.7.1",
        asset("aspia-console-2.7.1-x64.msi",
              "https://github.com/teoritty/aspia-sync/releases/download/v2.7.1/"
              "aspia-console-2.7.1-x64.msi") + "," +
        asset("aspia-host-2.7.1-x64.msi", kHostUrl));

    UpdateInfo info = UpdateInfo::fromGitHubRelease(bytes(json), kRepository, u"host");
    ASSERT_TRUE(info.isValid());
    EXPECT_EQ(info.version(), base::Version(2, 7, 1));
    EXPECT_EQ(info.url(), base::utf16FromUtf8(kHostUrl));
    EXPECT_EQ(info.description(), u"Fixes <things> & more");

    // What the rest of the program reads comes out the same.
    UpdateInfo again = UpdateInfo::fromXml(info.toXml());
    ASSERT_TRUE(again.isValid());
    EXPECT_EQ(again.version(), info.version());
    EXPECT_EQ(again.url(), info.url());
    EXPECT_EQ(again.description(), info.description());
}

TEST(UpdateInfoTest, NoInstallerOfThePackage)
{
    std::string json = release("v2.7.1", asset("aspia-host-2.7.1-x64.msi", kHostUrl));

    UpdateInfo info = UpdateInfo::fromGitHubRelease(bytes(json), kRepository, u"client");
    EXPECT_FALSE(info.isValid());

    // An invalid one tells the reader there are no updates rather than failing it.
    EXPECT_FALSE(UpdateInfo::fromXml(info.toXml()).isValid());
}

TEST(UpdateInfoTest, InstallerFromAnotherVersionIsNotTaken)
{
    std::string json = release("v2.7.2", asset("aspia-host-2.7.1-x64.msi", kHostUrl));
    EXPECT_FALSE(UpdateInfo::fromGitHubRelease(bytes(json), kRepository, u"host").isValid());
}

TEST(UpdateInfoTest, InstallerFromElsewhereIsRefused)
{
    for (const char* url : { "https://evil.example/aspia-host-2.7.1-x64.msi",
                             "https://github.com/someone/aspia-sync/releases/download/v2.7.1/"
                             "aspia-host-2.7.1-x64.msi",
                             "http://github.com/teoritty/aspia-sync/releases/download/v2.7.1/"
                             "aspia-host-2.7.1-x64.msi" })
    {
        std::string json = release("v2.7.1", asset("aspia-host-2.7.1-x64.msi", url));
        EXPECT_FALSE(UpdateInfo::fromGitHubRelease(bytes(json), kRepository, u"host").isValid())
            << url;
    }
}

TEST(UpdateInfoTest, UnreleasedIsNotAnUpdate)
{
    for (const char* flag : { "\"draft\":true,", "\"prerelease\":true," })
    {
        std::string json = "{" + std::string(flag) + release("v2.7.1",
            asset("aspia-host-2.7.1-x64.msi", kHostUrl)).substr(1);
        EXPECT_FALSE(UpdateInfo::fromGitHubRelease(bytes(json), kRepository, u"host").isValid())
            << flag;
    }
}

TEST(UpdateInfoTest, BadReleases)
{
    const std::string host = asset("aspia-host-2.7.1-x64.msi", kHostUrl);

    for (const std::string& json : { std::string(), std::string("not json"), std::string("[]"),
                                     std::string("{\"message\":\"Not Found\"}"),
                                     release("latest", host), release("v2.x.1", host) })
    {
        EXPECT_FALSE(UpdateInfo::fromGitHubRelease(bytes(json), kRepository, u"host").isValid())
            << json;
    }
}

TEST(UpdateInfoTest, LongNotesAreCut)
{
    // Two bytes a character, so a cut at an even length would split none and an odd one would.
    std::string body;
    for (int i = 0; i < 1500; ++i)
        body += "\xD0\xB6";

    std::string json = "{\"tag_name\":\"v2.7.1\",\"body\":\"x" + body + "\",\"assets\":[" +
                       asset("aspia-host-2.7.1-x64.msi", kHostUrl) + "]}";

    UpdateInfo info = UpdateInfo::fromGitHubRelease(bytes(json), kRepository, u"host");
    ASSERT_TRUE(info.isValid());
    EXPECT_LE(base::utf8FromUtf16(info.description()).size(), 2048u);
    EXPECT_TRUE(UpdateInfo::fromXml(info.toXml()).isValid());
}

TEST(UpdateInfoTest, XmlWithoutVersionIsNotAnUpdate)
{
    EXPECT_FALSE(UpdateInfo::fromXml(bytes("<update><url>https://x.example/a.msi</url></update>"))
        .isValid());
    EXPECT_FALSE(UpdateInfo::fromXml(bytes("<noupdate/>")).isValid());
}

} // namespace common
