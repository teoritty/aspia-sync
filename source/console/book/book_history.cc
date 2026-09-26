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

#include "console/book/book_history.h"

#include "console/book/local_changes.h"
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
HistoryRecord openRecord(const proto::BookEntryData& entry, std::string_view sync_key)
{
    HistoryRecord record;
    record.exists = true;
    record.deleted = entry.deleted();
    record.kind = kindFromProto(entry.kind());
    record.parent_guid = entry.parent_guid();
    record.revision = entry.revision();

    if (!entry.payload().empty())
        record.readable = openPayload(sync_key, entry.payload(), &record.payload);

    // A living record with nothing in it cannot be written back either. The router refuses to
    // store one, so it can only come of a history that was tampered with.
    if (!record.deleted && record.payload.empty())
        record.readable = false;

    if (!record.readable)
        record.payload.clear();

    return record;
}

//--------------------------------------------------------------------------------------------------
// Compares two messages by their serialized form. Both come out of the same generated code, so
// equal content serializes to equal bytes.
template <class T>
bool same(const T& a, const T& b)
{
    return a.SerializeAsString() == b.SerializeAsString();
}

//--------------------------------------------------------------------------------------------------
// The fields named one by one, for the same reason as in merge.cc: the lite runtime leaves out the
// reflection that would let it be written once. The times are left out on purpose - every edit
// moves them, and "modified: modify time" says nothing to anybody.
void computerFields(const Computer& a, const Computer& b, std::vector<std::string>* out)
{
    if (a.name() != b.name())
        out->emplace_back("name");
    if (a.address() != b.address())
        out->emplace_back("address");
    if (a.port() != b.port())
        out->emplace_back("port");
    if (a.comment() != b.comment())
        out->emplace_back("comment");
    if (a.username() != b.username())
        out->emplace_back("username");
    if (a.password() != b.password())
        out->emplace_back("password");
    if (a.session_type() != b.session_type())
        out->emplace_back("session_type");
    if (!same(a.inherit(), b.inherit()))
        out->emplace_back("inherit");
    if (!same(a.session_config(), b.session_config()))
        out->emplace_back("session_config");
}

//--------------------------------------------------------------------------------------------------
void groupFields(const ComputerGroup& a, const ComputerGroup& b, std::vector<std::string>* out)
{
    if (a.name() != b.name())
        out->emplace_back("name");
    if (a.comment() != b.comment())
        out->emplace_back("comment");
    if (a.config().username() != b.config().username())
        out->emplace_back("username");
    if (a.config().password() != b.config().password())
        out->emplace_back("password");
    if (!same(a.config().inherit(), b.config().inherit()))
        out->emplace_back("inherit");
    if (!same(a.config().session_config(), b.config().session_config()))
        out->emplace_back("session_config");
}

//--------------------------------------------------------------------------------------------------
std::string nameOf(FlatEntry::Kind kind, const std::string& payload)
{
    if (payload.empty())
        return std::string();

    if (kind == FlatEntry::Kind::COMPUTER)
    {
        Computer computer;
        if (computer.ParseFromString(payload))
            return computer.name();
        return std::string();
    }

    ComputerGroup group;
    if (group.ParseFromString(payload))
        return group.name();
    return std::string();
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

} // namespace

//--------------------------------------------------------------------------------------------------
std::vector<HistoryBatch> openHistory(const proto::BookHistory& page, std::string_view sync_key)
{
    std::vector<HistoryBatch> batches;
    batches.reserve(static_cast<size_t>(page.batch_size()));

    for (int i = 0; i < page.batch_size(); ++i)
    {
        const proto::BookHistoryBatch& source = page.batch(i);

        HistoryBatch batch;
        batch.revision = source.revision();
        batch.server_time = source.server_time();
        batch.modified_by = source.modified_by();
        batch.address = source.address();
        batch.rollback_to = source.rollback_to_revision();

        for (int j = 0; j < source.change_size(); ++j)
        {
            const proto::BookHistoryChange& item = source.change(j);

            HistoryChange change;
            change.guid = item.guid();
            change.kind = kindFromProto(item.kind());

            if (item.has_before())
                change.before = openRecord(item.before(), sync_key);

            if (item.has_after())
                change.after = openRecord(item.after(), sync_key);

            batch.changes.emplace_back(std::move(change));
        }

        batches.emplace_back(std::move(batch));
    }

    return batches;
}

//--------------------------------------------------------------------------------------------------
ChangeDescription describeChange(const HistoryChange& change)
{
    ChangeDescription description;

    const HistoryRecord& before = change.before;
    const HistoryRecord& after = change.after;

    description.name = nameOf(change.kind, after.payload);
    if (description.name.empty())
        description.name = nameOf(change.kind, before.payload);

    if ((before.alive() && !before.readable) || (after.alive() && !after.readable))
    {
        description.action = ChangeDescription::Action::UNREADABLE;
        return description;
    }

    if (!after.alive())
    {
        description.action = ChangeDescription::Action::DELETED;
        return description;
    }

    if (!before.exists)
    {
        description.action = ChangeDescription::Action::CREATED;
        return description;
    }

    if (before.deleted)
    {
        description.action = ChangeDescription::Action::RESTORED;
        return description;
    }

    if (change.kind == FlatEntry::Kind::COMPUTER)
    {
        Computer a;
        Computer b;
        a.ParseFromString(before.payload);
        b.ParseFromString(after.payload);
        computerFields(a, b, &description.fields);
    }
    else
    {
        ComputerGroup a;
        ComputerGroup b;
        a.ParseFromString(before.payload);
        b.ParseFromString(after.payload);
        groupFields(a, b, &description.fields);
    }

    const bool moved = (before.parent_guid != after.parent_guid);

    if (moved && description.fields.empty())
    {
        description.action = ChangeDescription::Action::MOVED;
        return description;
    }

    if (moved)
        description.fields.emplace_back("group");

    description.action = ChangeDescription::Action::CHANGED;
    return description;
}

//--------------------------------------------------------------------------------------------------
RollbackPlan planRollback(const std::vector<HistoryBatch>& batches, int64_t to_revision,
                          const std::string& only_guid)
{
    // For every record touched after the point: the first change after it, whose "before" is what
    // the record was at the point, and the last one, whose "after" is what the router holds now.
    struct Span
    {
        int64_t first_revision = 0;
        int64_t last_revision = 0;
        const HistoryChange* first = nullptr;
        const HistoryChange* last = nullptr;
    };

    std::map<std::string, Span> spans;

    for (const HistoryBatch& batch : batches)
    {
        if (batch.revision <= to_revision)
            continue;

        for (const HistoryChange& change : batch.changes)
        {
            if (!only_guid.empty() && change.guid != only_guid)
                continue;

            Span& span = spans[change.guid];

            if (!span.first || batch.revision < span.first_revision)
            {
                span.first = &change;
                span.first_revision = batch.revision;
            }

            if (!span.last || batch.revision > span.last_revision)
            {
                span.last = &change;
                span.last_revision = batch.revision;
            }
        }
    }

    RollbackPlan plan;

    for (const auto& [guid, span] : spans)
    {
        const HistoryRecord& then = span.first->before;
        const HistoryRecord& now = span.last->after;

        RollbackTarget target;
        target.guid = guid;
        target.kind = then.exists ? then.kind : span.first->kind;
        target.present = then.alive();
        target.router_revision = now.revision;
        target.router_alive = now.alive();

        if (target.present)
        {
            if (!then.readable)
            {
                plan.unreadable.emplace_back(guid);
                continue;
            }

            target.parent_guid = then.parent_guid;
            target.payload = then.payload;

            // Already what it was. Comparing the opened content, not the sealed bytes: sealing the
            // same thing twice never gives the same bytes.
            if (now.alive() && now.readable && now.payload == then.payload &&
                now.parent_guid == then.parent_guid && now.kind == target.kind)
            {
                continue;
            }
        }
        else if (!now.alive())
        {
            continue; // Not there then, not there now.
        }

        plan.targets.emplace_back(std::move(target));
    }

    return plan;
}

//--------------------------------------------------------------------------------------------------
bool HistoryJournal::addPage(const proto::BookHistory& page, const std::string& book_guid,
                             int64_t asked_before, std::string_view sync_key)
{
    if (page.error_code() != proto::BOOK_ERROR_CODE_OK || page.book_guid() != book_guid)
        return false;

    if (asked_before == 0)
    {
        clear();
        revision_ = page.revision();
    }
    else if (asked_before != nextBefore())
    {
        // An answer to a question that is no longer the current one: the journal was started over
        // while it was on its way. Taking it would leave a hole between the pages.
        return false;
    }

    oldest_revision_ = page.oldest_revision();
    history_days_ = page.history_days();
    history_max_changes_ = page.history_max_changes();
    has_more_ = page.has_more();

    std::vector<HistoryBatch> opened = openHistory(page, sync_key);
    for (HistoryBatch& batch : opened)
    {
        // Pages come newest first and each starts below the last. Anything else is not appended:
        // the order is what rolling back relies on to find the first change after a point.
        if (!batches_.empty() && batch.revision >= batches_.back().revision)
            continue;

        batches_.emplace_back(std::move(batch));
    }

    return true;
}

//--------------------------------------------------------------------------------------------------
void HistoryJournal::clear()
{
    batches_.clear();
    has_more_ = false;
    revision_ = 0;
    oldest_revision_ = 0;
    history_days_ = 0;
    history_max_changes_ = 0;
}

//--------------------------------------------------------------------------------------------------
int64_t HistoryJournal::nextBefore() const
{
    return batches_.empty() ? 0 : batches_.back().revision;
}

//--------------------------------------------------------------------------------------------------
bool HistoryJournal::covers(int64_t revision) const
{
    if (!has_more_)
        return true;

    return !batches_.empty() && batches_.back().revision <= revision + 1;
}

//--------------------------------------------------------------------------------------------------
const HistoryBatch* HistoryJournal::findBatch(int64_t revision) const
{
    for (const HistoryBatch& batch : batches_)
    {
        if (batch.revision == revision)
            return &batch;
    }

    return nullptr;
}

//--------------------------------------------------------------------------------------------------
RollbackCheck prepareRollback(const HistoryJournal& journal, const Data& data,
                              int64_t to_revision, const std::string& only_guid,
                              RollbackPlan* plan)
{
    if (!plan)
        return RollbackCheck::NOTHING_TO_DO;

    *plan = RollbackPlan();

    // The plan says what the router has now and what it had then. Applied to a book that is not
    // exactly what the router has now - an edit not sent, a question not answered, a batch not
    // fetched - it would undo or redo the wrong things.
    if (data.sync().book_guid().empty() ||
        data.sync().last_pulled_revision() != journal.revision())
    {
        return RollbackCheck::NOT_IN_STEP;
    }

    // Looked for on a copy: finding edits marks them, and whether to go ahead is not decided yet.
    Data copy(data);
    markLocalChanges(&copy);

    for (int i = 0; i < copy.sync().entry_size(); ++i)
    {
        const SyncEntryState& state = copy.sync().entry(i);
        if (state.dirty() || state.deleted() || state.conflict())
            return RollbackCheck::NOT_IN_STEP;
    }

    if (to_revision < journal.oldestRevision())
        return RollbackCheck::TOO_OLD;

    if (!journal.covers(to_revision))
        return RollbackCheck::NOT_LOADED;

    *plan = planRollback(journal.batches(), to_revision, only_guid);

    if (plan->targets.empty())
        return RollbackCheck::NOTHING_TO_DO;

    return RollbackCheck::OK;
}

//--------------------------------------------------------------------------------------------------
RollbackCounts applyRollback(const RollbackPlan& plan, Data* data)
{
    RollbackCounts counts;

    if (!data || plan.targets.empty())
        return counts;

    std::vector<FlatEntry> entries = flattenBook(data->root_group());

    std::map<std::string, size_t> index;
    for (size_t i = 0; i < entries.size(); ++i)
        index.emplace(entries[i].guid, i);

    for (const RollbackTarget& target : plan.targets)
    {
        auto it = index.find(target.guid);

        if (!target.present)
        {
            if (it == index.end())
                continue;

            // Removed from the tree; the deletion is found and sent like any other (see
            // local_changes.h), because the sync state still holds what the router has.
            entries[it->second].guid.clear();
            ++counts.removed;
            continue;
        }

        if (it != index.end())
        {
            FlatEntry& entry = entries[it->second];
            entry.kind = target.kind;
            entry.parent_guid = target.parent_guid;
            entry.payload = target.payload;
            ++counts.changed;
            continue;
        }

        FlatEntry entry;
        entry.guid = target.guid;
        entry.kind = target.kind;
        entry.parent_guid = target.parent_guid;
        entry.payload = target.payload;

        index.emplace(entry.guid, entries.size());
        entries.emplace_back(std::move(entry));

        // Not in the book, so the router holds it as a headstone (or holds nothing, if it never
        // left this console). Written over the headstone, the router wants its revision; with no
        // base the record reads as changed and goes out on the next exchange.
        SyncEntryState* state = findState(data, target.guid);
        if (!state)
        {
            state = data->mutable_sync()->add_entry();
            state->set_guid(target.guid);
        }

        state->set_revision(target.router_revision);
        state->clear_base_payload();
        state->clear_base_parent_guid();
        state->set_deleted(false);
        state->set_conflict(false);
        state->set_dirty(true);

        ++counts.restored;
    }

    std::vector<FlatEntry> kept;
    kept.reserve(entries.size());
    for (FlatEntry& entry : entries)
    {
        if (!entry.guid.empty())
            kept.emplace_back(std::move(entry));
    }

    rebuildBook(kept, data->mutable_root_group(), nullptr);
    return counts;
}

} // namespace console
