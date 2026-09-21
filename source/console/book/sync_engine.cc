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

#include "console/book/sync_engine.h"

#include "base/logging.h"
#include "console/book/flat_book.h"
#include "console/book/merge.h"
#include "console/book/sync_key.h"
#include "proto/address_book.pb.h"
#include "proto/router_book.pb.h"

#include <map>

namespace console {

namespace {

using proto::address_book::Computer;
using proto::address_book::ComputerGroup;
using proto::address_book::Data;
using proto::address_book::SyncEntryState;

//--------------------------------------------------------------------------------------------------
FlatEntry::Kind kindFromProto(proto::BookEntryKind kind)
{
    return (kind == proto::BOOK_ENTRY_KIND_COMPUTER) ? FlatEntry::Kind::COMPUTER
                                                     : FlatEntry::Kind::GROUP;
}

//--------------------------------------------------------------------------------------------------
proto::BookEntryKind kindToProto(FlatEntry::Kind kind)
{
    return (kind == FlatEntry::Kind::COMPUTER) ? proto::BOOK_ENTRY_KIND_COMPUTER
                                               : proto::BOOK_ENTRY_KIND_GROUP;
}

//--------------------------------------------------------------------------------------------------
SyncEntryState* findState(Data* data, const std::string& guid)
{
    proto::address_book::SyncState* sync = data->mutable_sync();

    for (int i = 0; i < sync->entry_size(); ++i)
    {
        if (sync->entry(i).guid() == guid)
            return sync->mutable_entry(i);
    }

    return nullptr;
}

//--------------------------------------------------------------------------------------------------
const SyncEntryState* findState(const Data& data, const std::string& guid)
{
    for (int i = 0; i < data.sync().entry_size(); ++i)
    {
        if (data.sync().entry(i).guid() == guid)
            return &data.sync().entry(i);
    }

    return nullptr;
}

//--------------------------------------------------------------------------------------------------
SyncEntryState* stateFor(Data* data, const std::string& guid)
{
    SyncEntryState* state = findState(data, guid);
    if (state)
        return state;

    state = data->mutable_sync()->add_entry();
    state->set_guid(guid);
    return state;
}

//--------------------------------------------------------------------------------------------------
// Sets a record aside for a person to decide about.
//
// It stays pending, so that whatever they choose still goes out, but it stops being sent until
// then: the router refuses it, the refusal fails to merge, and the two of them would go round that
// loop for as long as the console stayed open.
void holdForPerson(Data* data, const std::string& guid)
{
    stateFor(data, guid)->set_conflict(true);
}

//--------------------------------------------------------------------------------------------------
void removeState(Data* data, const std::string& guid)
{
    proto::address_book::SyncState* sync = data->mutable_sync();

    for (int i = 0; i < sync->entry_size(); ++i)
    {
        if (sync->entry(i).guid() != guid)
            continue;

        sync->mutable_entry()->DeleteSubrange(i, 1);
        return;
    }
}

//--------------------------------------------------------------------------------------------------
// Merges one record against what the router has. Returns false when a person has to decide.
bool mergeAgainst(FlatEntry::Kind kind, const std::string& base_payload,
                  const std::string& local_payload, const std::string& remote_payload,
                  std::string* out)
{
    if (kind == FlatEntry::Kind::COMPUTER)
    {
        Computer base;
        Computer local;
        Computer remote;

        // A base that does not parse is treated as nothing having been there: both sides then look
        // changed and the person is asked, which is the safe way to be wrong.
        base.ParseFromString(base_payload);

        if (!local.ParseFromString(local_payload) || !remote.ParseFromString(remote_payload))
            return false;

        Computer merged;
        const MergeResult result = mergeComputer(base, local, remote, &merged);
        *out = merged.SerializeAsString();
        return result.ok();
    }

    ComputerGroup base;
    ComputerGroup local;
    ComputerGroup remote;

    base.ParseFromString(base_payload);

    if (!local.ParseFromString(local_payload) || !remote.ParseFromString(remote_payload))
        return false;

    ComputerGroup merged;
    const MergeResult result = mergeComputerGroup(base, local, remote, &merged);
    *out = merged.SerializeAsString();
    return result.ok();
}

} // namespace

//--------------------------------------------------------------------------------------------------
SyncEngine::SyncEngine(std::string sync_key)
    : sync_key_(std::move(sync_key))
{
    // Nothing
}

//--------------------------------------------------------------------------------------------------
SyncEngine::~SyncEngine() = default;

//--------------------------------------------------------------------------------------------------
SyncEngine::PullOutcome SyncEngine::applyPull(const proto::BookPull& page, Data* data)
{
    PullOutcome outcome;

    if (!data)
    {
        outcome.status = PullOutcome::Status::WRONG_BOOK;
        return outcome;
    }

    const proto::address_book::SyncState& sync = data->sync();

    if (sync.book_guid() != page.book_guid())
    {
        outcome.status = PullOutcome::Status::WRONG_BOOK;
        return outcome;
    }

    // Both of these say the same thing: what this console remembers about the revision no longer
    // refers to anything on the router. Going on would leave the two quietly out of step, and
    // nobody would find out until something was missing.
    if (!sync.epoch().empty() && sync.epoch() != page.epoch())
    {
        outcome.status = PullOutcome::Status::EPOCH_CHANGED;
        return outcome;
    }

    if (page.revision() < sync.last_pulled_revision())
    {
        outcome.status = PullOutcome::Status::REVISION_WENT_BACK;
        return outcome;
    }

    // Everything is opened before anything is written. A page half applied because the passphrase
    // turned out to be wrong would leave the book in a state nobody asked for.
    std::map<std::string, std::string> opened;

    for (int i = 0; i < page.entry_size(); ++i)
    {
        const proto::BookEntryData& entry = page.entry(i);
        if (entry.deleted())
            continue;

        std::string payload;
        if (!openPayload(sync_key_, entry.payload(), &payload))
        {
            // One record that does not open is a damaged record and is passed over. All of them
            // failing means the key is wrong, and that is not something to work around.
            ++outcome.skipped;
            continue;
        }

        opened.emplace(entry.guid(), std::move(payload));
    }

    if (opened.empty() && outcome.skipped > 0 && outcome.skipped == static_cast<size_t>(
            page.entry_size()))
    {
        outcome.status = PullOutcome::Status::BAD_KEY;
        outcome.skipped = 0;
        return outcome;
    }

    // The tree is taken apart, changed record by record and put back together. Doing it this way
    // means the placing of records - orphans, cycles, ordering - is handled by the one piece that
    // already knows how, instead of a second attempt at it here.
    std::vector<FlatEntry> entries = flattenBook(data->root_group());

    std::map<std::string, size_t> index;
    for (size_t i = 0; i < entries.size(); ++i)
        index.emplace(entries[i].guid, i);

    for (int i = 0; i < page.entry_size(); ++i)
    {
        const proto::BookEntryData& remote = page.entry(i);

        const SyncEntryState* state = findState(*data, remote.guid());
        const bool dirty_here = state && state->dirty();

        auto it = index.find(remote.guid());
        const bool present_here = (it != index.end());

        if (remote.deleted())
        {
            if (!present_here)
            {
                // Already gone, or never arrived. Nothing to do but forget it.
                removeState(data, remote.guid());
                ++outcome.applied;
                continue;
            }

            if (dirty_here)
            {
                // Somebody deleted it while this console was editing it. Which of the two is meant
                // is not for this code to decide.
                //
                // The base payload is emptied to say that the router holds nothing readable for
                // this record now, which is what tells the two answers apart later: keeping it
                // puts it back, giving it up removes it here as well.
                //
                // The revision stays what the router said. It is the revision of the headstone,
                // and putting the record back means writing over that headstone - which the
                // router only accepts from somebody who knew it was there.
                SyncEntryState* held = stateFor(data, remote.guid());
                held->set_revision(remote.revision());
                held->clear_base_payload();
                held->clear_base_parent_guid();
                held->set_dirty(true);
                held->set_conflict(true);

                outcome.conflicts.emplace_back(remote.guid());
                continue;
            }

            entries[it->second].guid.clear(); // Marked for removal below.
            removeState(data, remote.guid());
            ++outcome.applied;
            continue;
        }

        auto payload = opened.find(remote.guid());
        if (payload == opened.end())
            continue; // Could not be opened; already counted as skipped.

        if (!present_here)
        {
            FlatEntry entry;
            entry.guid = remote.guid();
            entry.parent_guid = remote.parent_guid();
            entry.kind = kindFromProto(remote.kind());
            entry.payload = payload->second;

            index.emplace(entry.guid, entries.size());
            entries.emplace_back(std::move(entry));

            SyncEntryState* new_state = stateFor(data, remote.guid());
            new_state->set_revision(remote.revision());
            new_state->set_base_payload(payload->second);
            new_state->set_base_parent_guid(remote.parent_guid());
            new_state->set_dirty(false);

            ++outcome.applied;
            continue;
        }

        FlatEntry& local = entries[it->second];

        if (!dirty_here)
        {
            // Nothing was changed here, so what the router has is simply taken.
            local.parent_guid = remote.parent_guid();
            local.kind = kindFromProto(remote.kind());
            local.payload = payload->second;

            SyncEntryState* existing = stateFor(data, remote.guid());
            existing->set_revision(remote.revision());
            existing->set_base_payload(payload->second);
            existing->set_base_parent_guid(remote.parent_guid());
            existing->set_dirty(false);

            ++outcome.applied;
            continue;
        }

        std::string merged;
        const bool clean = mergeAgainst(local.kind, state->base_payload(), local.payload,
                                        payload->second, &merged);

        if (!clean)
        {
            // The local value stays, so the person keeps seeing what they typed. It also stays
            // pending, so once they decide it goes out.
            //
            // The base moves to what the router has all the same. What the person decides has to
            // be built on that: keeping the old base would have the router refuse their answer as
            // stale, and the question would come back a second time for no reason.
            SyncEntryState* held = stateFor(data, remote.guid());
            held->set_revision(remote.revision());
            held->set_base_payload(payload->second);
            held->set_base_parent_guid(remote.parent_guid());
            held->set_dirty(true);
            held->set_conflict(true);

            outcome.conflicts.emplace_back(remote.guid());
            continue;
        }

        local.payload = merged;

        // Where the record sits is not merged field by field: a record is in one group or another,
        // and if this console did not move it the router's answer is the one that is true.
        if (state->base_payload().empty() || local.parent_guid == remote.parent_guid())
            local.parent_guid = remote.parent_guid();

        SyncEntryState* existing = stateFor(data, remote.guid());
        existing->set_revision(remote.revision());
        existing->set_base_payload(payload->second);
        existing->set_base_parent_guid(remote.parent_guid());

        // Still pending: the merged version is not what the router has yet.
        existing->set_dirty(true);

        ++outcome.applied;
    }

    // The tree is put back together only when this page actually changed something.
    //
    // Rebuilding replaces the whole of the root group, and the window holds pointers straight into
    // it - one per computer in the list. Doing it for a page that changed nothing would leave
    // every one of those pointers hanging while the window had no reason to be told to redraw, and
    // the next thing the person clicked would be read out of freed memory. So the rebuild happens
    // only when there is something to rebuild for, and it says so in |tree_rebuilt|, which is what
    // makes the caller redraw.
    if (outcome.applied != 0)
    {
        std::vector<FlatEntry> kept;
        kept.reserve(entries.size());
        for (FlatEntry& entry : entries)
        {
            if (!entry.guid.empty())
                kept.emplace_back(std::move(entry));
        }

        size_t skipped_by_rebuild = 0;
        if (!rebuildBook(kept, data->mutable_root_group(), &skipped_by_rebuild))
        {
            LOG(LS_ERROR) << "Unable to rebuild the address book from the received records";
            outcome.status = PullOutcome::Status::WRONG_BOOK;
            return outcome;
        }

        outcome.skipped += skipped_by_rebuild;
        outcome.tree_rebuilt = true;
    }

    data->mutable_sync()->set_epoch(page.epoch());

    // The revision moves only on the last page. A console interrupted halfway asks again from
    // where it was rather than believing it has what it never received.
    if (!page.has_more())
        data->mutable_sync()->set_last_pulled_revision(page.revision());

    return outcome;
}

//--------------------------------------------------------------------------------------------------
bool SyncEngine::buildPush(const Data& data, const std::string& op_id,
                           proto::BookPushRequest* request)
{
    if (!request || op_id.empty() || data.sync().book_guid().empty())
        return false;

    request->set_book_guid(data.sync().book_guid());
    request->set_op_id(op_id);

    const std::vector<FlatEntry> entries = flattenBook(data.root_group());

    std::map<std::string, const FlatEntry*> by_guid;
    for (const FlatEntry& entry : entries)
        by_guid.emplace(entry.guid, &entry);

    for (int i = 0; i < data.sync().entry_size(); ++i)
    {
        if (request->change_size() >= kMaxChangesPerPush)
            break;

        const SyncEntryState& state = data.sync().entry(i);
        if (!state.dirty() && !state.deleted())
            continue;

        if (state.conflict())
            continue; // Waiting for a person. See holdForPerson.

        proto::BookChangeData* change = request->add_change();
        change->set_guid(state.guid());
        change->set_base_revision(state.revision());

        if (state.deleted())
        {
            change->set_deleted(true);
            continue;
        }

        auto it = by_guid.find(state.guid());
        if (it == by_guid.end())
        {
            // Marked as changed but no longer in the book, and not marked as deleted either. The
            // state is wrong rather than the book, so the record is not sent.
            request->mutable_change()->RemoveLast();
            continue;
        }

        std::string sealed;
        if (!sealPayload(sync_key_, it->second->payload, &sealed))
        {
            LOG(LS_ERROR) << "Unable to seal a record for sending";
            request->mutable_change()->RemoveLast();
            continue;
        }

        change->set_parent_guid(it->second->parent_guid);
        change->set_kind(kindToProto(it->second->kind));
        change->set_payload(sealed);
    }

    return request->change_size() > 0;
}

//--------------------------------------------------------------------------------------------------
SyncEngine::PushOutcome SyncEngine::applyPushResult(const proto::BookPushResult& result, Data* data)
{
    PushOutcome outcome;

    if (!data || result.error_code() != proto::BOOK_ERROR_CODE_OK)
        return outcome;

    std::vector<FlatEntry> entries = flattenBook(data->root_group());

    std::map<std::string, size_t> index;
    for (size_t i = 0; i < entries.size(); ++i)
        index.emplace(entries[i].guid, i);

    bool tree_changed = false;

    for (int i = 0; i < result.result_size(); ++i)
    {
        const proto::BookChangeResult& entry = result.result(i);

        switch (entry.status())
        {
            case proto::BOOK_CHANGE_STATUS_OK:
            {
                SyncEntryState* state = findState(data, entry.guid());
                if (!state)
                    break;

                if (state->deleted())
                {
                    // The deletion is on the router now; nothing here has to remember it.
                    removeState(data, entry.guid());
                    ++outcome.accepted;
                    break;
                }

                auto it = index.find(entry.guid());
                if (it != index.end())
                {
                    state->set_base_payload(entries[it->second].payload);
                    state->set_base_parent_guid(entries[it->second].parent_guid);
                }

                state->set_revision(result.revision());
                state->set_dirty(false);
                ++outcome.accepted;
            }
            break;

            case proto::BOOK_CHANGE_STATUS_CONFLICT:
            {
                SyncEntryState* state = findState(data, entry.guid());
                auto it = index.find(entry.guid());

                if (!state || it == index.end() || !entry.has_current())
                {
                    outcome.conflicts.emplace_back(entry.guid());
                    holdForPerson(data, entry.guid());
                    break;
                }

                std::string remote;
                if (!openPayload(sync_key_, entry.current().payload(), &remote))
                {
                    outcome.conflicts.emplace_back(entry.guid());
                    holdForPerson(data, entry.guid());
                    break;
                }

                std::string merged;
                if (!mergeAgainst(entries[it->second].kind, state->base_payload(),
                                  entries[it->second].payload, remote, &merged))
                {
                    // Both sides changed the same field. It waits for a person, and stays pending
                    // so that it goes out once they decide - built on what the router has now, so
                    // that their answer is not refused as stale.
                    state->set_revision(entry.current().revision());
                    state->set_base_payload(remote);
                    state->set_base_parent_guid(entry.current().parent_guid());
                    state->set_dirty(true);
                    state->set_conflict(true);

                    outcome.conflicts.emplace_back(entry.guid());
                    break;
                }

                entries[it->second].payload = merged;
                tree_changed = true;

                // The merge is built on what the router has now, so the next attempt is no longer
                // stale and goes through without another round.
                state->set_revision(entry.current().revision());
                state->set_base_payload(remote);
                state->set_base_parent_guid(entry.current().parent_guid());
                state->set_dirty(true);

                ++outcome.merged;
            }
            break;

            default:
                // Refused outright. Keeping it pending would make the console send it again
                // forever, so it stops being pending and is reported.
                if (SyncEntryState* state = findState(data, entry.guid()))
                    state->set_dirty(false);

                outcome.conflicts.emplace_back(entry.guid());
                ++outcome.rejected;
                break;
        }
    }

    if (tree_changed)
    {
        rebuildBook(entries, data->mutable_root_group(), nullptr);
        outcome.tree_rebuilt = true;
    }

    return outcome;
}

//--------------------------------------------------------------------------------------------------
bool SyncEngine::resolveConflict(const std::string& guid, bool keep_local, Data* data)
{
    if (!data)
        return false;

    SyncEntryState* state = findState(data, guid);
    if (!state || !state->conflict())
        return false;

    state->set_conflict(false);

    if (keep_local)
    {
        // What is in the book stays and goes out on the next exchange. The base under it is what
        // the router has, so it is not refused as stale this time.
        state->set_dirty(true);
        return true;
    }

    // Theirs. What this console had is given up and the record goes back to the base, which is the
    // version the question was asked about.
    std::vector<FlatEntry> entries = flattenBook(data->root_group());

    for (auto it = entries.begin(); it != entries.end(); ++it)
    {
        if (it->guid != guid)
            continue;

        if (state->base_payload().empty())
        {
            // The router has nothing for it: somebody deleted it. Giving up the local version
            // means letting the deletion through.
            entries.erase(it);
            rebuildBook(entries, data->mutable_root_group(), nullptr);
            removeState(data, guid);
            return true;
        }

        it->payload = state->base_payload();
        it->parent_guid = state->base_parent_guid();
        rebuildBook(entries, data->mutable_root_group(), nullptr);
        break;
    }

    state->set_dirty(false);
    return true;
}

} // namespace console
