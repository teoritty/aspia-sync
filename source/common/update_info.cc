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

#include "base/logging.h"
#include "base/strings/unicode.h"

#include "third_party/rapidxml/rapidxml.hpp"

#include <rapidjson/document.h>

namespace common {

namespace {

const size_t kMaxDescriptionLength = 2048;

//--------------------------------------------------------------------------------------------------
std::string toLowerAscii(std::string text)
{
    for (char& c : text)
    {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

//--------------------------------------------------------------------------------------------------
std::string escapeXml(std::string_view text)
{
    std::string result;
    result.reserve(text.size());

    for (char c : text)
    {
        switch (c)
        {
            case '&': result += "&amp;"; break;
            case '<': result += "&lt;"; break;
            case '>': result += "&gt;"; break;
            case '"': result += "&quot;"; break;
            case '\'': result += "&apos;"; break;
            default: result += c; break;
        }
    }

    return result;
}

} // namespace

//--------------------------------------------------------------------------------------------------
// static
UpdateInfo UpdateInfo::fromXml(const base::ByteArray& buffer)
{
    if (buffer.empty())
    {
        LOG(LS_INFO) << "Empty XML buffer";
        return UpdateInfo();
    }

    // rapidxml reads up to a terminating zero, and what came from the network has none. It also
    // writes into what it parses, so it gets a copy of its own.
    std::string text(buffer.begin(), buffer.end());

    rapidxml::xml_document<> xml;

    try
    {
        xml.parse<rapidxml::parse_default>(text.data());
    }
    catch (const rapidxml::parse_error& error)
    {
        LOG(LS_ERROR) << "Invalid XML for update info: " << error.what() << " at "
                      << error.where<char>();
        return UpdateInfo();
    }

    rapidxml::xml_node<>* root_node = xml.first_node("update");
    if (!root_node)
    {
        LOG(LS_ERROR) << "Node 'update' not found. No available updates";
        return UpdateInfo();
    }

    UpdateInfo update_info;

    for (const rapidxml::xml_node<>* child_node = root_node->first_node();
         child_node != nullptr;
         child_node = child_node->next_sibling())
    {
        if (child_node->type() != rapidxml::node_element)
            continue;

        std::string_view name(child_node->name(), child_node->name_size());

        const rapidxml::xml_node<>* node = child_node->first_node();
        if (node && node->type() == rapidxml::node_data)
        {
            if (name == "version")
            {
                update_info.version_ = base::Version(base::utf16FromUtf8(
                    std::string_view(node->value(), node->value_size())));
                if (!update_info.version_.isValid())
                {
                    LOG(LS_ERROR) << "Invalid version: " << node->value();
                    return UpdateInfo();
                }
            }
            else if (name == "description")
            {
                if (node->value_size() > kMaxDescriptionLength)
                {
                    LOG(LS_ERROR) << "Invalid description length: " << node->value_size()
                                  << " (max: " << kMaxDescriptionLength << ")";
                    return UpdateInfo();
                }

                update_info.description_ = base::utf16FromUtf8(
                    std::string_view(node->value(), node->value_size()));
            }
            else if (name == "url")
            {
                static const size_t kMinUrlLength = 10;
                static const size_t kMaxUrlLength = 256;

                if (node->value_size() < kMinUrlLength || node->value_size() > kMaxUrlLength)
                {
                    LOG(LS_ERROR) << "Invalid URL length: " << node->value_size()
                                  << " (min: " << kMinUrlLength << " max: " << kMaxUrlLength << ")";
                    return UpdateInfo();
                }

                update_info.url_ = base::utf16FromUtf8(
                    std::string_view(node->value(), node->value_size()));
            }
        }
    }

    // Without a version there is nothing to compare with the one running.
    if (!update_info.version_.isValid())
    {
        LOG(LS_ERROR) << "No version in update info";
        return UpdateInfo();
    }

    update_info.valid_ = true;
    return update_info;
}

//--------------------------------------------------------------------------------------------------
// static
UpdateInfo UpdateInfo::fromGitHubRelease(const base::ByteArray& buffer,
                                         std::string_view repository,
                                         std::u16string_view package_name)
{
    if (buffer.empty() || repository.empty() || package_name.empty())
        return UpdateInfo();

    rapidjson::Document release;
    release.Parse(reinterpret_cast<const char*>(buffer.data()), buffer.size());
    if (release.HasParseError() || !release.IsObject())
    {
        LOG(LS_ERROR) << "Invalid JSON for a release";
        return UpdateInfo();
    }

    // "releases/latest" gives neither of these, but a release that is not out yet is never an
    // update, whatever gave it.
    for (const char* flag : { "draft", "prerelease" })
    {
        auto it = release.FindMember(flag);
        if (it != release.MemberEnd() && it->value.IsBool() && it->value.GetBool())
        {
            LOG(LS_INFO) << "Release is a " << flag;
            return UpdateInfo();
        }
    }

    auto tag = release.FindMember("tag_name");
    if (tag == release.MemberEnd() || !tag->value.IsString())
    {
        LOG(LS_ERROR) << "No tag in the release";
        return UpdateInfo();
    }

    // Tags are "vX.Y.Z", see .github/workflows/release.yml.
    std::string_view version_string(tag->value.GetString(), tag->value.GetStringLength());
    if (version_string.starts_with('v'))
        version_string.remove_prefix(1);

    UpdateInfo update_info;
    update_info.version_ = base::Version(base::utf16FromUtf8(version_string));
    if (!update_info.version_.isValid())
    {
        LOG(LS_ERROR) << "Invalid version in the tag: " << tag->value.GetString();
        return UpdateInfo();
    }

    // Only Windows x64 has installers, and they are named by the release workflow.
    const std::string asset_name = "aspia-" + base::utf8FromUtf16(package_name) + '-' +
        base::utf8FromUtf16(update_info.version_.toString(3)) + "-x64.msi";

    // The host installs what it downloads on its own. Whatever the release says, the installer
    // has to come from the releases of this very repository.
    const std::string url_prefix =
        "https://github.com/" + std::string(repository) + "/releases/download/";

    auto assets = release.FindMember("assets");
    if (assets == release.MemberEnd() || !assets->value.IsArray())
    {
        LOG(LS_ERROR) << "No files in the release";
        return UpdateInfo();
    }

    std::string url;
    for (const auto& asset : assets->value.GetArray())
    {
        if (!asset.IsObject())
            continue;

        auto name = asset.FindMember("name");
        auto download_url = asset.FindMember("browser_download_url");
        if (name == asset.MemberEnd() || !name->value.IsString() ||
            download_url == asset.MemberEnd() || !download_url->value.IsString())
        {
            continue;
        }

        if (std::string_view(name->value.GetString(), name->value.GetStringLength()) != asset_name)
            continue;

        std::string candidate(download_url->value.GetString(), download_url->value.GetStringLength());
        if (!toLowerAscii(candidate).starts_with(url_prefix))
        {
            LOG(LS_ERROR) << "Installer is not in the releases of " << repository << ": "
                          << candidate;
            return UpdateInfo();
        }

        url = std::move(candidate);
        break;
    }

    if (url.empty())
    {
        LOG(LS_INFO) << "No " << asset_name << " in the release";
        return UpdateInfo();
    }

    update_info.url_ = base::utf16FromUtf8(url);

    auto body = release.FindMember("body");
    if (body != release.MemberEnd() && body->value.IsString())
    {
        std::string description(body->value.GetString(), body->value.GetStringLength());

        // fromXml() takes no more than this, and the notes of a release are often longer. They are
        // cut rather than the update lost, and not in the middle of a character.
        if (description.size() > kMaxDescriptionLength)
        {
            size_t length = kMaxDescriptionLength;
            while (length > 0 && (static_cast<uint8_t>(description[length]) & 0xC0) == 0x80)
                --length;
            description.resize(length);
        }

        update_info.description_ = base::utf16FromUtf8(description);
    }

    update_info.valid_ = true;
    return update_info;
}

//--------------------------------------------------------------------------------------------------
base::ByteArray UpdateInfo::toXml() const
{
    std::string xml;

    if (!valid_)
    {
        xml = "<noupdate/>";
    }
    else
    {
        xml = "<update><version>" + escapeXml(base::utf8FromUtf16(version_.toString(3))) +
              "</version><description>" + escapeXml(base::utf8FromUtf16(description_)) +
              "</description><url>" + escapeXml(base::utf8FromUtf16(url_)) + "</url></update>";
    }

    return base::ByteArray(xml.begin(), xml.end());
}

//--------------------------------------------------------------------------------------------------
std::string gitHubRepository(std::u16string_view update_server)
{
    static const std::string_view kPrefix = "https://github.com/";

    std::string server = toLowerAscii(base::utf8FromUtf16(update_server));
    if (!server.starts_with(kPrefix))
        return std::string();

    std::string_view repository(server);
    repository.remove_prefix(kPrefix.size());

    while (repository.ends_with('/'))
        repository.remove_suffix(1);
    if (repository.ends_with(".git"))
        repository.remove_suffix(4);

    // It goes into the address of the API as it is, so nothing but a plain "owner/name".
    const size_t slash = repository.find('/');
    if (slash == std::string_view::npos || slash == 0 || slash == repository.size() - 1 ||
        repository.find('/', slash + 1) != std::string_view::npos)
    {
        return std::string();
    }

    for (char c : repository)
    {
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                             c == '-' || c == '_' || c == '.' || c == '/';
        if (!allowed)
            return std::string();
    }

    if (repository.find("..") != std::string_view::npos)
        return std::string();

    return std::string(repository);
}

} // namespace common
