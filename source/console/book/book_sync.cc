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

#include "console/book/book_sync.h"

#include "base/guid.h"
#include "base/logging.h"
#include "console/book/local_changes.h"
#include "proto/address_book.pb.h"
#include "proto/router_book.pb.h"

#include <algorithm>

namespace console {

//--------------------------------------------------------------------------------------------------
BookSync::BookSync(std::string sync_key, Sender* sender, Observer* observer)
    : engine_(std::move(sync_key)),
      sender_(sender),
      observer_(observer)
{
    DCHECK(sender_);
    DCHECK(observer_);
}

//--------------------------------------------------------------------------------------------------
BookSync::~BookSync() = default;

//--------------------------------------------------------------------------------------------------
void BookSync::start(proto::address_book::Data* data)
{
    if (!data || data->sync().book_guid().empty())
        return;

    if (busy_)
    {
        // Answers to a second exchange would arrive about a book that moved underneath them.
        again_ = true;
        return;
    }

    busy_ = true;
    pushOrPull(data);
}

//--------------------------------------------------------------------------------------------------
void BookSync::pushOrPull(proto::address_book::Data* data)
{
    // What was edited here is found by comparing the book with what the router last gave, rather
    // than by having every edit announce itself. See local_changes.h for why.
    markLocalChanges(data);

    op_id_ = base::Guid::create().toStdString();

    proto::BookPushRequest request;
    if (engine_.buildPush(*data, op_id_, &request))
    {
        // Sending first and asking afterwards. The other way round, an edit made here would be
        // overwritten by the answer before it ever left the machine.
        sender_->sendPush(request);
        return;
    }

    requestPull(*data);
}

//--------------------------------------------------------------------------------------------------
void BookSync::requestPull(const proto::address_book::Data& data)
{
    // A fresh walk through the book starts at the beginning of it.
    pull_offset_ = 0;

    proto::BookPullRequest request;
    request.set_book_guid(data.sync().book_guid());
    request.set_since_revision(data.sync().last_pulled_revision());
    request.set_offset(pull_offset_);

    sender_->sendPull(request);
}

//--------------------------------------------------------------------------------------------------
void BookSync::noteConflicts(const std::vector<std::string>& guids)
{
    if (guids.empty())
        return;

    for (const std::string& guid : guids)
    {
        if (std::find(conflicts_.begin(), conflicts_.end(), guid) == conflicts_.end())
            conflicts_.emplace_back(guid);
    }

    observer_->onConflicts(guids);
}

//--------------------------------------------------------------------------------------------------
void BookSync::onPushResult(const proto::BookPushResult& result, proto::address_book::Data* data)
{
    if (!data)
        return;

    if (result.error_code() != proto::BOOK_ERROR_CODE_OK)
    {
        LOG(LS_ERROR) << "The router refused the batch: " << result.error_code();
        busy_ = false;
        return;
    }

    const SyncEngine::PushOutcome outcome = engine_.applyPushResult(result, data);

    if (outcome.accepted || outcome.merged || outcome.tree_rebuilt)
        observer_->onBookUpdated();

    noteConflicts(outcome.conflicts);

    // What merged is ready to go out again, and what was accepted may have been only part of what
    // is waiting, so the exchange carries on rather than stopping here.
    requestPull(*data);
}

//--------------------------------------------------------------------------------------------------
void BookSync::onPull(const proto::BookPull& page, proto::address_book::Data* data)
{
    if (!data)
        return;

    if (page.error_code() != proto::BOOK_ERROR_CODE_OK)
    {
        LOG(LS_ERROR) << "The router refused to send the book: " << page.error_code();
        busy_ = false;
        return;
    }

    const SyncEngine::PullOutcome outcome = engine_.applyPull(page, data);

    switch (outcome.status)
    {
        case SyncEngine::PullOutcome::Status::OK:
            break;

        default:
            // Nothing here is helped by asking again: the router was restored from a backup, or
            // the passphrase is wrong. Both need a person.
            busy_ = false;
            again_ = false;
            observer_->onSyncStopped(outcome.status);
            return;
    }

    // Told whenever the tree was replaced, and not merely when records changed. The two are the
    // same thing today; keeping the redraw tied to the replacement is what stops them drifting
    // apart and leaving the window pointing into memory that has been freed.
    if (outcome.applied || outcome.tree_rebuilt)
        observer_->onBookUpdated();

    noteConflicts(outcome.conflicts);

    if (page.has_more())
    {
        // More is waiting. The revision does not move until the last page, so asking by revision
        // would fetch this same page again; the next one is taken by moving along. The offset has
        // to be how far into the book we are, not how big the page just handled was - otherwise
        // every page after the first asks for the second one, and a large book never finishes
        // arriving.
        pull_offset_ += page.entry_size();

        proto::BookPullRequest request;
        request.set_book_guid(data->sync().book_guid());
        request.set_since_revision(data->sync().last_pulled_revision());
        request.set_offset(pull_offset_);

        sender_->sendPull(request);
        return;
    }

    // What arrived may have left something of this console's own still waiting - a merged record,
    // for one - so the exchange is run again rather than declared finished.
    proto::BookPushRequest pending;
    markLocalChanges(data);

    if (engine_.buildPush(*data, base::Guid::create().toStdString(), &pending))
    {
        op_id_ = pending.op_id();
        sender_->sendPush(pending);
        return;
    }

    busy_ = false;

    if (again_)
    {
        again_ = false;
        start(data);
        return;
    }

    observer_->onInSync(data->sync().last_pulled_revision());
}

//--------------------------------------------------------------------------------------------------
void BookSync::onBookChanged(const proto::BookChanged& message, proto::address_book::Data* data)
{
    if (!data || message.book_guid() != data->sync().book_guid())
        return;

    if (message.revision() <= data->sync().last_pulled_revision())
    {
        // This console already has it. The router tells everybody, including the one whose change
        // it was, and asking for what is already here would only make work.
        return;
    }

    start(data);
}

//--------------------------------------------------------------------------------------------------
bool BookSync::resolveConflict(const std::string& guid, bool keep_local,
                               proto::address_book::Data* data)
{
    if (!engine_.resolveConflict(guid, keep_local, data))
        return false;

    clearConflict(guid);

    observer_->onBookUpdated();
    start(data);
    return true;
}

//--------------------------------------------------------------------------------------------------
void BookSync::clearConflict(const std::string& guid)
{
    conflicts_.erase(std::remove(conflicts_.begin(), conflicts_.end(), guid), conflicts_.end());
}

} // namespace console
