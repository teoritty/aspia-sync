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

#ifndef ROUTER_SETTINGS_H
#define ROUTER_SETTINGS_H

#include "base/settings/json_settings.h"

namespace router {

class Settings
{
public:
    Settings();
    ~Settings();

    static std::filesystem::path filePath();

    void reset();
    void flush();

    void setListenInterface(const std::u16string& interface);
    std::u16string listenInterface() const;

    void setPort(uint16_t port);
    uint16_t port() const;

    void setPrivateKey(const base::ByteArray& private_key);
    base::ByteArray privateKey() const;

    using WhiteList = std::vector<std::u16string>;

    void setClientWhiteList(const WhiteList& list);
    WhiteList clientWhiteList() const;

    void setHostWhiteList(const WhiteList& list);
    WhiteList hostWhiteList() const;

    void setAdminWhiteList(const WhiteList& list);
    WhiteList adminWhiteList() const;

    void setRelayWhiteList(const WhiteList& list);
    WhiteList relayWhiteList() const;

    void setSeedKey(const base::ByteArray& seed_key);
    base::ByteArray seedKey() const;

    // How long the history of the shared address books is kept, in days, and how many changed
    // records per book it may hold at most. Zero days keeps no history; zero changes sets no limit
    // by count. See BookHistoryPolicy.
    static constexpr int kDefaultBookHistoryDays = 30;
    static constexpr int kDefaultBookHistoryMaxChanges = 10000;

    void setBookHistoryDays(int days);
    int bookHistoryDays() const;

    void setBookHistoryMaxChanges(int max_changes);
    int bookHistoryMaxChanges() const;

private:
    void setWhiteList(std::string_view key, const WhiteList& value);
    WhiteList whiteList(std::string_view key) const;

    base::JsonSettings impl_;
};

} // namespace router

#endif // ROUTER_SETTINGS_H
