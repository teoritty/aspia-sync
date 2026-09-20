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

#include "router/book/book_service.h"

#include "base/logging.h"
#include "proto/router_book.pb.h"
#include "router/book/book_store.h"

#include <vector>

namespace router {

namespace {

// Bounds on what arrives. None of these are reached by anything the console sends; they are here
// so that something which is not the console cannot make the router allocate without limit.
constexpr size_t kMaxGuidLength = 64;
constexpr size_t kMaxOpIdLength = 64;
constexpr size_t kMaxPayloadLength = 256 * 1024;
constexpr int kMaxChangesPerRequest = 500;

//--------------------------------------------------------------------------------------------------
BookEntry::Kind kindFromProto(proto::BookEntryKind kind)
{
    return (kind == proto::BOOK_ENTRY_KIND_COMPUTER) ? BookEntry::Kind::COMPUTER
                                                     : BookEntry::Kind::GROUP;
}

//--------------------------------------------------------------------------------------------------
proto::BookEntryKind kindToProto(BookEntry::Kind kind)
{
    return (kind == BookEntry::Kind::COMPUTER) ? proto::BOOK_ENTRY_KIND_COMPUTER
                                               : proto::BOOK_ENTRY_KIND_GROUP;
}

//--------------------------------------------------------------------------------------------------
void entryToProto(const BookEntry& entry, proto::BookEntryData* out)
{
    out->set_guid(entry.guid);
    out->set_parent_guid(entry.parent_guid);
    out->set_kind(kindToProto(entry.kind));
    out->set_revision(entry.revision);
    out->set_server_time(entry.server_time);
    out->set_client_time(entry.client_time);
    out->set_modified_by(entry.modified_by);
    out->set_deleted(entry.deleted);
    out->set_deleted_at(entry.deleted_at);
    out->set_payload(entry.payload);
}

} // namespace

//--------------------------------------------------------------------------------------------------
BookService::BookService(BookStore* store)
    : store_(store)
{
    DCHECK(store_);
}

//--------------------------------------------------------------------------------------------------
void BookService::handleListRequest(const proto::BookListRequest& request, proto::BookList* result)
{
    DCHECK(result);

    result->set_request_id(request.request_id());

    std::vector<Book> books;
    if (!store_->bookList(&books))
    {
        LOG(LS_ERROR) << "Unable to read the list of books";
        result->set_error_code(proto::BOOK_ERROR_CODE_INTERNAL_ERROR);
        return;
    }

    for (const Book& book : books)
    {
        proto::BookInfo* info = result->add_book();
        info->set_guid(book.guid);
        info->set_name(book.name);
        info->set_sync_salt(book.sync_salt);
        info->set_key_verifier(book.key_verifier);
        info->set_epoch(book.epoch);
        info->set_revision(book.revision);
    }

    result->set_error_code(proto::BOOK_ERROR_CODE_OK);
}

//--------------------------------------------------------------------------------------------------
void BookService::handlePullRequest(const proto::BookPullRequest& request, proto::BookPull* result)
{
    DCHECK(result);

    result->set_request_id(request.request_id());
    result->set_book_guid(request.book_guid());

    if (request.book_guid().size() > kMaxGuidLength || request.since_revision() < 0 ||
        request.offset() < 0 || request.count() < 0)
    {
        result->set_error_code(proto::BOOK_ERROR_CODE_INVALID_REQUEST);
        return;
    }

    Book book;
    if (!store_->findBook(request.book_guid(), &book))
    {
        result->set_error_code(proto::BOOK_ERROR_CODE_NOT_FOUND);
        return;
    }

    // The revision and the epoch are taken before the records, so a console that stores them can
    // never end up believing it has more than it was given.
    result->set_revision(book.revision);
    result->set_epoch(book.epoch);

    int64_t count = request.count();
    if (count <= 0 || count > kMaxEntriesPerPage)
        count = kMaxEntriesPerPage;

    // One more than asked for, to tell "this is the last page" from "there is exactly one page
    // left" without a second query.
    std::vector<BookEntry> entries;
    if (!store_->entriesSince(request.book_guid(), request.since_revision(), request.offset(),
                              count + 1, &entries))
    {
        result->set_error_code(proto::BOOK_ERROR_CODE_INTERNAL_ERROR);
        return;
    }

    bool has_more = false;
    if (static_cast<int64_t>(entries.size()) > count)
    {
        entries.resize(static_cast<size_t>(count));
        has_more = true;
    }

    size_t bytes = 0;
    for (const BookEntry& entry : entries)
    {
        // The number of records says nothing about their size, so the page also stops when it has
        // carried enough bytes. At least one record always goes, or a record larger than the
        // budget would stall the exchange forever.
        if (bytes >= kMaxPayloadBytesPerPage && result->entry_size() > 0)
        {
            has_more = true;
            break;
        }

        entryToProto(entry, result->add_entry());
        bytes += entry.payload.size();
    }

    result->set_has_more(has_more);
    result->set_error_code(proto::BOOK_ERROR_CODE_OK);
}

//--------------------------------------------------------------------------------------------------
void BookService::handlePushRequest(const proto::BookPushRequest& request,
                                    const std::string& modified_by,
                                    proto::BookPushResult* result)
{
    DCHECK(result);

    result->set_request_id(request.request_id());
    result->set_book_guid(request.book_guid());
    result->set_op_id(request.op_id());

    if (request.book_guid().size() > kMaxGuidLength || request.op_id().empty() ||
        request.op_id().size() > kMaxOpIdLength ||
        request.change_size() > kMaxChangesPerRequest)
    {
        result->set_error_code(proto::BOOK_ERROR_CODE_INVALID_REQUEST);
        return;
    }

    std::vector<BookChange> changes;
    changes.reserve(static_cast<size_t>(request.change_size()));

    for (int i = 0; i < request.change_size(); ++i)
    {
        const proto::BookChangeData& source = request.change(i);

        if (source.guid().size() > kMaxGuidLength ||
            source.parent_guid().size() > kMaxGuidLength ||
            source.payload().size() > kMaxPayloadLength ||
            source.base_revision() < 0)
        {
            result->set_error_code(proto::BOOK_ERROR_CODE_INVALID_REQUEST);
            return;
        }

        BookChange change;
        change.guid = source.guid();
        change.parent_guid = source.parent_guid();
        change.kind = kindFromProto(source.kind());
        change.base_revision = source.base_revision();
        change.deleted = source.deleted();
        change.client_time = source.client_time();
        change.payload = source.payload();

        changes.emplace_back(std::move(change));
    }

    std::vector<BookChangeResult> results;
    int64_t revision = 0;

    if (!store_->applyChanges(request.book_guid(), request.op_id(), modified_by, changes,
                              &results, &revision))
    {
        Book book;
        result->set_error_code(store_->findBook(request.book_guid(), &book)
                                   ? proto::BOOK_ERROR_CODE_INTERNAL_ERROR
                                   : proto::BOOK_ERROR_CODE_NOT_FOUND);
        return;
    }

    for (const BookChangeResult& source : results)
    {
        proto::BookChangeResult* entry = result->add_result();
        entry->set_guid(source.guid);
        entry->set_reason(source.reason);

        switch (source.status)
        {
            case BookChangeResult::Status::OK:
                entry->set_status(proto::BOOK_CHANGE_STATUS_OK);
                break;

            case BookChangeResult::Status::CONFLICT:
                entry->set_status(proto::BOOK_CHANGE_STATUS_CONFLICT);
                entryToProto(source.current, entry->mutable_current());
                break;

            default:
                entry->set_status(proto::BOOK_CHANGE_STATUS_REJECTED);
                break;
        }
    }

    result->set_revision(revision);
    result->set_error_code(proto::BOOK_ERROR_CODE_OK);
}

} // namespace router
