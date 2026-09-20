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

#include "console/book/merge.h"

#include "proto/address_book.pb.h"

#include <algorithm>
#include <string>

namespace console {

namespace {

using proto::address_book::Computer;
using proto::address_book::ComputerGroup;

// Decides one field from the three versions of it and says whether it had to give up.
//
// The reflection that would let this be written once for every field is not available: the
// protocol is generated for the lite runtime, which leaves it out. So the fields are named one by
// one below. That is tedious, but it is also the only place where a new field would have to be
// remembered, and a new field forgotten here would silently stop being merged - so the tedium is
// where it belongs.
template <class T>
bool pick(const T& base, const T& local, const T& remote, T* out)
{
    if (local == remote)
    {
        *out = local; // Both sides agree, whatever they did to get there.
        return true;
    }

    if (local == base)
    {
        *out = remote; // Only the other side changed it.
        return true;
    }

    if (remote == base)
    {
        *out = local; // Only this side changed it.
        return true;
    }

    // Both changed it, to different things. The local value is kept so that the person keeps
    // seeing what they typed until they say what to do.
    *out = local;
    return false;
}

//--------------------------------------------------------------------------------------------------
void note(bool merged, const char* field, MergeResult* result)
{
    if (!merged)
        result->conflicts.emplace_back(field);
}

//--------------------------------------------------------------------------------------------------
// The desktop settings of a session, merged one flag at a time rather than as a lump: the two
// sides may well have changed different settings.
void mergeDesktopConfig(const proto::DesktopConfig& base,
                        const proto::DesktopConfig& local,
                        const proto::DesktopConfig& remote,
                        proto::DesktopConfig* out,
                        const char* field,
                        MergeResult* result)
{
    uint32_t flags = 0;
    note(pick(base.flags(), local.flags(), remote.flags(), &flags), field, result);
    out->set_flags(flags);

    int video_encoding = 0;
    note(pick(static_cast<int>(base.video_encoding()), static_cast<int>(local.video_encoding()),
              static_cast<int>(remote.video_encoding()), &video_encoding), field, result);
    out->set_video_encoding(static_cast<proto::VideoEncoding>(video_encoding));

    int audio_encoding = 0;
    note(pick(static_cast<int>(base.audio_encoding()), static_cast<int>(local.audio_encoding()),
              static_cast<int>(remote.audio_encoding()), &audio_encoding), field, result);
    out->set_audio_encoding(static_cast<proto::AudioEncoding>(audio_encoding));

    uint32_t compress_ratio = 0;
    note(pick(base.compress_ratio(), local.compress_ratio(), remote.compress_ratio(),
              &compress_ratio), field, result);
    out->set_compress_ratio(compress_ratio);

    // update_interval and scale_factor are deprecated and must keep their fixed values, so they
    // are carried over rather than merged: there is nothing here for two people to disagree about.
    out->set_update_interval(local.update_interval());
    out->set_scale_factor(local.scale_factor());
    out->mutable_pixel_format()->CopyFrom(local.pixel_format());
}

//--------------------------------------------------------------------------------------------------
void mergeInherit(const proto::address_book::InheritConfig& base,
                  const proto::address_book::InheritConfig& local,
                  const proto::address_book::InheritConfig& remote,
                  proto::address_book::InheritConfig* out,
                  MergeResult* result)
{
    bool credentials = false;
    note(pick(base.credentials(), local.credentials(), remote.credentials(), &credentials),
         "inherit.credentials", result);
    out->set_credentials(credentials);

    bool desktop_manage = false;
    note(pick(base.desktop_manage(), local.desktop_manage(), remote.desktop_manage(),
              &desktop_manage), "inherit.desktop_manage", result);
    out->set_desktop_manage(desktop_manage);

    bool desktop_view = false;
    note(pick(base.desktop_view(), local.desktop_view(), remote.desktop_view(), &desktop_view),
         "inherit.desktop_view", result);
    out->set_desktop_view(desktop_view);
}

//--------------------------------------------------------------------------------------------------
void mergeSessionConfig(const proto::address_book::SessionConfig& base,
                        const proto::address_book::SessionConfig& local,
                        const proto::address_book::SessionConfig& remote,
                        proto::address_book::SessionConfig* out,
                        MergeResult* result)
{
    mergeDesktopConfig(base.desktop_manage(), local.desktop_manage(), remote.desktop_manage(),
                       out->mutable_desktop_manage(), "session_config.desktop_manage", result);

    mergeDesktopConfig(base.desktop_view(), local.desktop_view(), remote.desktop_view(),
                       out->mutable_desktop_view(), "session_config.desktop_view", result);
}

} // namespace

//--------------------------------------------------------------------------------------------------
MergeResult mergeComputer(const Computer& base, const Computer& local, const Computer& remote,
                          Computer* out)
{
    MergeResult result;
    if (!out)
        return result;

    Computer merged;

    // The identity is not merged. It is the thing that says these three are versions of one
    // record, so taking it from anywhere but the local copy would make no sense.
    merged.set_guid(local.guid());

    std::string text;
    note(pick(base.name(), local.name(), remote.name(), &text), "name", &result);
    merged.set_name(text);

    note(pick(base.comment(), local.comment(), remote.comment(), &text), "comment", &result);
    merged.set_comment(text);

    note(pick(base.address(), local.address(), remote.address(), &text), "address", &result);
    merged.set_address(text);

    note(pick(base.username(), local.username(), remote.username(), &text), "username", &result);
    merged.set_username(text);

    note(pick(base.password(), local.password(), remote.password(), &text), "password", &result);
    merged.set_password(text);

    uint32_t port = 0;
    note(pick(base.port(), local.port(), remote.port(), &port), "port", &result);
    merged.set_port(port);

    int session_type = 0;
    note(pick(static_cast<int>(base.session_type()), static_cast<int>(local.session_type()),
              static_cast<int>(remote.session_type()), &session_type), "session_type", &result);
    merged.set_session_type(static_cast<proto::SessionType>(session_type));

    // When the record was made is a fact about the past, so the earliest of the three is the one
    // that is true. Nothing here can be in conflict.
    int64_t create_time = base.create_time();
    if (local.create_time() && (!create_time || local.create_time() < create_time))
        create_time = local.create_time();
    if (remote.create_time() && (!create_time || remote.create_time() < create_time))
        create_time = remote.create_time();
    merged.set_create_time(create_time);

    // And when it was last touched is the latest of them, for the same reason.
    merged.set_modify_time(std::max(local.modify_time(), remote.modify_time()));

    mergeInherit(base.inherit(), local.inherit(), remote.inherit(), merged.mutable_inherit(),
                 &result);

    mergeSessionConfig(base.session_config(), local.session_config(), remote.session_config(),
                       merged.mutable_session_config(), &result);

    *out = std::move(merged);
    return result;
}

//--------------------------------------------------------------------------------------------------
MergeResult mergeComputerGroup(const ComputerGroup& base, const ComputerGroup& local,
                               const ComputerGroup& remote, ComputerGroup* out)
{
    MergeResult result;
    if (!out)
        return result;

    ComputerGroup merged;

    merged.set_guid(local.guid());

    std::string text;
    note(pick(base.name(), local.name(), remote.name(), &text), "name", &result);
    merged.set_name(text);

    note(pick(base.comment(), local.comment(), remote.comment(), &text), "comment", &result);
    merged.set_comment(text);

    int64_t create_time = base.create_time();
    if (local.create_time() && (!create_time || local.create_time() < create_time))
        create_time = local.create_time();
    if (remote.create_time() && (!create_time || remote.create_time() < create_time))
        create_time = remote.create_time();
    merged.set_create_time(create_time);

    merged.set_modify_time(std::max(local.modify_time(), remote.modify_time()));

    // Whether the folder is open belongs to this window and is never sent, so the local value
    // simply stays.
    merged.set_expanded(local.expanded());

    const proto::address_book::ComputerGroupConfig& base_config = base.config();
    const proto::address_book::ComputerGroupConfig& local_config = local.config();
    const proto::address_book::ComputerGroupConfig& remote_config = remote.config();

    proto::address_book::ComputerGroupConfig* config = merged.mutable_config();

    note(pick(base_config.username(), local_config.username(), remote_config.username(), &text),
         "config.username", &result);
    config->set_username(text);

    note(pick(base_config.password(), local_config.password(), remote_config.password(), &text),
         "config.password", &result);
    config->set_password(text);

    mergeInherit(base_config.inherit(), local_config.inherit(), remote_config.inherit(),
                 config->mutable_inherit(), &result);

    mergeSessionConfig(base_config.session_config(), local_config.session_config(),
                       remote_config.session_config(), config->mutable_session_config(), &result);

    // The children of the group are records of their own and are not here to be merged.
    *out = std::move(merged);
    return result;
}

} // namespace console
