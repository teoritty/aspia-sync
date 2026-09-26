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

#include "router/book/book_store.h"

#include "base/logging.h"

#include <sqlite3.h>

#include <cctype>
#include <cstdint>
#include <ctime>
#include <limits>
#include <set>

namespace router {

namespace {

// Nothing here is built by pasting values into SQL. Every query is prepared with placeholders and
// the values are bound, which is how the rest of the router talks to its database and the only way
// that stays safe when a value arrives from the network.
class Statement
{
public:
    Statement(sqlite3* db, const char* sql)
        : db_(db)
    {
        const int error_code = sqlite3_prepare_v2(db, sql, -1, &statement_, nullptr);
        if (error_code != SQLITE_OK)
        {
            LOG(LS_ERROR) << "sqlite3_prepare_v2 failed: " << sqlite3_errstr(error_code)
                          << " (" << error_code << ")";
            statement_ = nullptr;
        }
    }

    ~Statement()
    {
        if (statement_)
            sqlite3_finalize(statement_);
    }

    bool isValid() const { return statement_ != nullptr; }
    sqlite3_stmt* get() const { return statement_; }

    bool bindText(int column, const std::string& value)
    {
        return sqlite3_bind_text(statement_, column, value.c_str(),
                                 static_cast<int>(value.size()),
                                 SQLITE_TRANSIENT) == SQLITE_OK;
    }

    bool bindBlob(int column, const std::string& value)
    {
        // An empty blob and a null are different things to sqlite, and binding a null pointer
        // would store the second when the first is meant.
        return sqlite3_bind_blob(statement_, column, value.empty() ? "" : value.data(),
                                 static_cast<int>(value.size()),
                                 SQLITE_TRANSIENT) == SQLITE_OK;
    }

    bool bindInt64(int column, int64_t value)
    {
        return sqlite3_bind_int64(statement_, column, value) == SQLITE_OK;
    }

    int step() { return sqlite3_step(statement_); }

    std::string columnText(int column) const
    {
        const unsigned char* text = sqlite3_column_text(statement_, column);
        if (!text)
            return std::string();
        return std::string(reinterpret_cast<const char*>(text),
                           static_cast<size_t>(sqlite3_column_bytes(statement_, column)));
    }

    std::string columnBlob(int column) const
    {
        const void* data = sqlite3_column_blob(statement_, column);
        const int size = sqlite3_column_bytes(statement_, column);
        if (!data || size <= 0)
            return std::string();
        return std::string(static_cast<const char*>(data), static_cast<size_t>(size));
    }

    int64_t columnInt64(int column) const { return sqlite3_column_int64(statement_, column); }

private:
    sqlite3* db_;
    sqlite3_stmt* statement_ = nullptr;

    DISALLOW_COPY_AND_ASSIGN(Statement);
};

//--------------------------------------------------------------------------------------------------
bool exec(sqlite3* db, const char* sql)
{
    char* error_string = nullptr;
    const int error_code = sqlite3_exec(db, sql, nullptr, nullptr, &error_string);
    if (error_code != SQLITE_OK)
    {
        LOG(LS_ERROR) << "sqlite3_exec failed: " << (error_string ? error_string : "");
        sqlite3_free(error_string);
        return false;
    }
    return true;
}

//--------------------------------------------------------------------------------------------------
int64_t currentTime()
{
    return static_cast<int64_t>(std::time(nullptr));
}

//--------------------------------------------------------------------------------------------------
// A guid arrives from the network, so its shape is checked before it becomes part of a query or a
// parent link. 8-4-4-4-12 hexadecimal digits.
bool isValidGuid(const std::string& guid)
{
    static const size_t kLength = 36;
    static const size_t kDashes[] = { 8, 13, 18, 23 };

    if (guid.size() != kLength)
        return false;

    for (size_t i = 0; i < kLength; ++i)
    {
        const bool is_dash_position =
            (i == kDashes[0] || i == kDashes[1] || i == kDashes[2] || i == kDashes[3]);

        if (is_dash_position)
        {
            if (guid[i] != '-')
                return false;
        }
        else if (!std::isxdigit(static_cast<unsigned char>(guid[i])))
        {
            return false;
        }
    }

    return true;
}

//--------------------------------------------------------------------------------------------------
BookEntry::Kind kindFromInt(int64_t value)
{
    return value == 1 ? BookEntry::Kind::COMPUTER : BookEntry::Kind::GROUP;
}

} // namespace

//--------------------------------------------------------------------------------------------------
BookStore::BookStore(sqlite3* db)
    : db_(db)
{
    DCHECK(db_);
}

//--------------------------------------------------------------------------------------------------
BookStore::~BookStore()
{
    if (db_)
        sqlite3_close(db_);
}

//--------------------------------------------------------------------------------------------------
// static
std::unique_ptr<BookStore> BookStore::open(const std::filesystem::path& file_path)
{
    sqlite3* db = nullptr;

    const int error_code = sqlite3_open(file_path.string().c_str(), &db);
    if (error_code != SQLITE_OK)
    {
        LOG(LS_ERROR) << "sqlite3_open failed: " << sqlite3_errstr(error_code);
        if (db)
            sqlite3_close(db);
        return nullptr;
    }

    // Foreign keys are off by default in sqlite, and the entries of a removed book have to go with
    // it rather than linger without one.
    exec(db, "PRAGMA foreign_keys = ON");

    std::unique_ptr<BookStore> store(new BookStore(db));
    if (!store->createTables())
        return nullptr;

    return store;
}

//--------------------------------------------------------------------------------------------------
bool BookStore::createTables()
{
    static const char kSql[] =
        "BEGIN TRANSACTION;"
        "CREATE TABLE IF NOT EXISTS \"books\" ("
            "\"book_id\" INTEGER PRIMARY KEY AUTOINCREMENT,"
            "\"guid\" TEXT NOT NULL UNIQUE,"
            "\"name\" TEXT NOT NULL DEFAULT '',"
            "\"sync_salt\" BLOB NOT NULL,"
            "\"key_verifier\" BLOB NOT NULL,"
            "\"epoch\" TEXT NOT NULL,"
            "\"revision\" INTEGER NOT NULL DEFAULT 0);"
        "CREATE TABLE IF NOT EXISTS \"book_entries\" ("
            "\"book_id\" INTEGER NOT NULL REFERENCES \"books\"(\"book_id\") ON DELETE CASCADE,"
            "\"guid\" TEXT NOT NULL,"
            "\"parent_guid\" TEXT NOT NULL DEFAULT '',"
            "\"kind\" INTEGER NOT NULL DEFAULT 0,"
            "\"revision\" INTEGER NOT NULL DEFAULT 0,"
            "\"server_time\" INTEGER NOT NULL DEFAULT 0,"
            "\"client_time\" INTEGER NOT NULL DEFAULT 0,"
            "\"modified_by\" TEXT NOT NULL DEFAULT '',"
            "\"deleted\" INTEGER NOT NULL DEFAULT 0,"
            "\"deleted_at\" INTEGER NOT NULL DEFAULT 0,"
            "\"payload\" BLOB NOT NULL,"
            "PRIMARY KEY(\"book_id\",\"guid\"));"
        // What every console asks for: everything after the revision it has. Without this the
        // router would read the whole book on each request.
        "CREATE INDEX IF NOT EXISTS \"book_entries_revision\" "
            "ON \"book_entries\"(\"book_id\",\"revision\");"
        "CREATE TABLE IF NOT EXISTS \"applied_ops\" ("
            "\"book_id\" INTEGER NOT NULL REFERENCES \"books\"(\"book_id\") ON DELETE CASCADE,"
            "\"op_id\" TEXT NOT NULL,"
            "\"applied_at\" INTEGER NOT NULL DEFAULT 0,"
            "\"revision\" INTEGER NOT NULL DEFAULT 0,"
            "PRIMARY KEY(\"book_id\",\"op_id\"));"
        // The history: one row per accepted batch, and one per record it changed, holding the
        // record as it was before and as it was after. The payloads are sealed exactly as in
        // book_entries; nothing here is readable that the book itself would not give away.
        "CREATE TABLE IF NOT EXISTS \"book_batches\" ("
            "\"book_id\" INTEGER NOT NULL REFERENCES \"books\"(\"book_id\") ON DELETE CASCADE,"
            "\"revision\" INTEGER NOT NULL,"
            "\"server_time\" INTEGER NOT NULL DEFAULT 0,"
            "\"modified_by\" TEXT NOT NULL DEFAULT '',"
            "\"address\" TEXT NOT NULL DEFAULT '',"
            "\"rollback_to\" INTEGER NOT NULL DEFAULT 0,"
            "PRIMARY KEY(\"book_id\",\"revision\"));"
        "CREATE TABLE IF NOT EXISTS \"book_history\" ("
            "\"book_id\" INTEGER NOT NULL REFERENCES \"books\"(\"book_id\") ON DELETE CASCADE,"
            "\"revision\" INTEGER NOT NULL,"
            "\"guid\" TEXT NOT NULL,"
            "\"kind\" INTEGER NOT NULL DEFAULT 0,"
            "\"before_exists\" INTEGER NOT NULL DEFAULT 0,"
            "\"before_kind\" INTEGER NOT NULL DEFAULT 0,"
            "\"before_parent_guid\" TEXT NOT NULL DEFAULT '',"
            "\"before_revision\" INTEGER NOT NULL DEFAULT 0,"
            "\"before_server_time\" INTEGER NOT NULL DEFAULT 0,"
            "\"before_modified_by\" TEXT NOT NULL DEFAULT '',"
            "\"before_deleted\" INTEGER NOT NULL DEFAULT 0,"
            "\"before_payload\" BLOB NOT NULL,"
            "\"after_parent_guid\" TEXT NOT NULL DEFAULT '',"
            "\"after_deleted\" INTEGER NOT NULL DEFAULT 0,"
            "\"after_payload\" BLOB NOT NULL,"
            "PRIMARY KEY(\"book_id\",\"revision\",\"guid\"));"
        "COMMIT;";

    return exec(db_, kSql);
}

//--------------------------------------------------------------------------------------------------
bool BookStore::bookIdByGuid(const std::string& guid, int64_t* book_id) const
{
    Statement statement(db_, "SELECT book_id FROM books WHERE guid = ?");
    if (!statement.isValid() || !statement.bindText(1, guid))
        return false;

    if (statement.step() != SQLITE_ROW)
        return false;

    *book_id = statement.columnInt64(0);
    return true;
}

//--------------------------------------------------------------------------------------------------
bool BookStore::createBook(const Book& book)
{
    if (!isValidGuid(book.guid) || book.sync_salt.empty() || book.key_verifier.empty() ||
        book.epoch.empty())
    {
        LOG(LS_ERROR) << "Invalid book record";
        return false;
    }

    Statement statement(db_,
        "INSERT INTO books (guid, name, sync_salt, key_verifier, epoch, revision) "
        "VALUES (?, ?, ?, ?, ?, 0)");
    if (!statement.isValid())
        return false;

    if (!statement.bindText(1, book.guid) || !statement.bindText(2, book.name) ||
        !statement.bindBlob(3, book.sync_salt) || !statement.bindBlob(4, book.key_verifier) ||
        !statement.bindText(5, book.epoch))
    {
        return false;
    }

    return statement.step() == SQLITE_DONE;
}

//--------------------------------------------------------------------------------------------------
bool BookStore::bookList(std::vector<Book>* out) const
{
    if (!out)
        return false;

    out->clear();

    Statement statement(db_,
        "SELECT guid, name, sync_salt, key_verifier, epoch, revision FROM books ORDER BY book_id");
    if (!statement.isValid())
        return false;

    for (;;)
    {
        const int result = statement.step();
        if (result == SQLITE_DONE)
            break;
        if (result != SQLITE_ROW)
            return false;

        Book book;
        book.guid = statement.columnText(0);
        book.name = statement.columnText(1);
        book.sync_salt = statement.columnBlob(2);
        book.key_verifier = statement.columnBlob(3);
        book.epoch = statement.columnText(4);
        book.revision = statement.columnInt64(5);

        out->emplace_back(std::move(book));
    }

    return true;
}

//--------------------------------------------------------------------------------------------------
bool BookStore::findBook(const std::string& guid, Book* out) const
{
    if (!out)
        return false;

    Statement statement(db_,
        "SELECT guid, name, sync_salt, key_verifier, epoch, revision FROM books WHERE guid = ?");
    if (!statement.isValid() || !statement.bindText(1, guid))
        return false;

    if (statement.step() != SQLITE_ROW)
        return false;

    out->guid = statement.columnText(0);
    out->name = statement.columnText(1);
    out->sync_salt = statement.columnBlob(2);
    out->key_verifier = statement.columnBlob(3);
    out->epoch = statement.columnText(4);
    out->revision = statement.columnInt64(5);

    return true;
}

//--------------------------------------------------------------------------------------------------
bool BookStore::removeBook(const std::string& guid)
{
    Statement statement(db_, "DELETE FROM books WHERE guid = ?");
    if (!statement.isValid() || !statement.bindText(1, guid))
        return false;

    return statement.step() == SQLITE_DONE && sqlite3_changes(db_) > 0;
}

//--------------------------------------------------------------------------------------------------
bool BookStore::resetEpoch(const std::string& book_guid, const std::string& epoch)
{
    if (epoch.empty())
        return false;

    Statement statement(db_, "UPDATE books SET epoch = ? WHERE guid = ?");
    if (!statement.isValid() || !statement.bindText(1, epoch) ||
        !statement.bindText(2, book_guid))
    {
        return false;
    }

    return statement.step() == SQLITE_DONE && sqlite3_changes(db_) > 0;
}

//--------------------------------------------------------------------------------------------------
bool BookStore::entriesSince(const std::string& book_guid, int64_t since_revision,
                             int64_t offset, int64_t count, std::vector<BookEntry>* out) const
{
    if (!out || offset < 0 || count < 0)
        return false;

    out->clear();

    int64_t book_id = 0;
    if (!bookIdByGuid(book_guid, &book_id))
        return false;

    Statement statement(db_,
        "SELECT guid, parent_guid, kind, revision, server_time, client_time, modified_by, "
        "deleted, deleted_at, payload FROM book_entries "
        "WHERE book_id = ? AND revision > ? ORDER BY revision, guid LIMIT ? OFFSET ?");
    if (!statement.isValid())
        return false;

    // A count of zero means "everything"; sqlite takes a negative limit for that.
    const int64_t limit = (count == 0) ? -1 : count;

    if (!statement.bindInt64(1, book_id) || !statement.bindInt64(2, since_revision) ||
        !statement.bindInt64(3, limit) || !statement.bindInt64(4, offset))
    {
        return false;
    }

    for (;;)
    {
        const int result = statement.step();
        if (result == SQLITE_DONE)
            break;
        if (result != SQLITE_ROW)
            return false;

        BookEntry entry;
        entry.guid = statement.columnText(0);
        entry.parent_guid = statement.columnText(1);
        entry.kind = kindFromInt(statement.columnInt64(2));
        entry.revision = statement.columnInt64(3);
        entry.server_time = statement.columnInt64(4);
        entry.client_time = statement.columnInt64(5);
        entry.modified_by = statement.columnText(6);
        entry.deleted = statement.columnInt64(7) != 0;
        entry.deleted_at = statement.columnInt64(8);
        entry.payload = statement.columnBlob(9);

        out->emplace_back(std::move(entry));
    }

    return true;
}

//--------------------------------------------------------------------------------------------------
bool BookStore::findEntry(const std::string& book_guid, const std::string& guid,
                          BookEntry* out) const
{
    if (!out)
        return false;

    int64_t book_id = 0;
    if (!bookIdByGuid(book_guid, &book_id))
        return false;

    Statement statement(db_,
        "SELECT guid, parent_guid, kind, revision, server_time, client_time, modified_by, "
        "deleted, deleted_at, payload FROM book_entries WHERE book_id = ? AND guid = ?");
    if (!statement.isValid() || !statement.bindInt64(1, book_id) || !statement.bindText(2, guid))
        return false;

    if (statement.step() != SQLITE_ROW)
        return false;

    out->guid = statement.columnText(0);
    out->parent_guid = statement.columnText(1);
    out->kind = kindFromInt(statement.columnInt64(2));
    out->revision = statement.columnInt64(3);
    out->server_time = statement.columnInt64(4);
    out->client_time = statement.columnInt64(5);
    out->modified_by = statement.columnText(6);
    out->deleted = statement.columnInt64(7) != 0;
    out->deleted_at = statement.columnInt64(8);
    out->payload = statement.columnBlob(9);

    return true;
}

//--------------------------------------------------------------------------------------------------
bool BookStore::entryCount(const std::string& book_guid, int64_t* out) const
{
    if (!out)
        return false;

    int64_t book_id = 0;
    if (!bookIdByGuid(book_guid, &book_id))
        return false;

    Statement statement(db_, "SELECT COUNT(*) FROM book_entries WHERE book_id = ?");
    if (!statement.isValid() || !statement.bindInt64(1, book_id))
        return false;

    if (statement.step() != SQLITE_ROW)
        return false;

    *out = statement.columnInt64(0);
    return true;
}

//--------------------------------------------------------------------------------------------------
namespace {

// Walks parent links upward from |from_guid| and answers whether |moved_guid| is on the way. The
// walk reads the rows of the running transaction, so changes applied earlier in the same batch are
// taken into account.
//
// Two people offline can produce a cycle without either doing anything wrong: one moves A into B
// while the other moves B into A. Applied as asked, the two groups would hang off each other and
// detach from the root, taking every computer in them out of the book.
bool wouldCreateCycle(sqlite3* db, int64_t book_id, const std::string& moved_guid,
                      const std::string& from_guid)
{
    std::set<std::string> seen;
    std::string current = from_guid;

    while (!current.empty())
    {
        if (current == moved_guid)
            return true;

        if (!seen.insert(current).second)
            return true; // Already a loop, whatever this change does to it.

        Statement statement(db, "SELECT parent_guid FROM book_entries "
                                "WHERE book_id = ? AND guid = ? AND deleted = 0");
        if (!statement.isValid() || !statement.bindInt64(1, book_id) ||
            !statement.bindText(2, current))
        {
            return false;
        }

        if (statement.step() != SQLITE_ROW)
            return false; // The chain ends at a record that is not there: no loop.

        current = statement.columnText(0);
    }

    return false;
}

//--------------------------------------------------------------------------------------------------
BookChangeResult rejected(const std::string& guid, const char* reason)
{
    BookChangeResult result;
    result.guid = guid;
    result.status = BookChangeResult::Status::REJECTED;
    result.reason = reason;
    return result;
}

} // namespace

//--------------------------------------------------------------------------------------------------
bool BookStore::applyChanges(const std::string& book_guid,
                             const std::string& op_id,
                             const std::string& modified_by,
                             const std::vector<BookChange>& changes,
                             std::vector<BookChangeResult>* results,
                             int64_t* new_revision,
                             const BookBatchNote& note)
{
    if (!results || !new_revision || op_id.empty())
        return false;

    results->clear();
    *new_revision = 0;

    Book book;
    if (!findBook(book_guid, &book))
    {
        LOG(LS_ERROR) << "Unknown book";
        return false;
    }

    int64_t book_id = 0;
    if (!bookIdByGuid(book_guid, &book_id))
        return false;

    // A console whose connection dropped before the answer arrived does not know whether the batch
    // was applied. Sending it again must not apply it a second time.
    {
        Statement statement(db_, "SELECT revision FROM applied_ops WHERE book_id = ? AND op_id = ?");
        if (!statement.isValid() || !statement.bindInt64(1, book_id) ||
            !statement.bindText(2, op_id))
        {
            return false;
        }

        if (statement.step() == SQLITE_ROW)
        {
            const int64_t applied_revision = statement.columnInt64(0);

            for (const BookChange& change : changes)
            {
                BookChangeResult result;
                result.guid = change.guid;

                BookEntry current;
                if (findEntry(book_guid, change.guid, &current) &&
                    current.revision == applied_revision)
                {
                    result.status = BookChangeResult::Status::OK;
                }
                else
                {
                    // The record moved on after this batch was applied. The console has to look at
                    // what is there now, exactly as it would after a plain conflict.
                    result.status = BookChangeResult::Status::CONFLICT;
                    result.current = current;
                }

                results->emplace_back(std::move(result));
            }

            *new_revision = applied_revision;
            return true;
        }
    }

    const int64_t revision = book.revision + 1;
    const int64_t now = currentTime();
    const bool keep_history = history_policy_.enabled();

    if (!exec(db_, "BEGIN IMMEDIATE TRANSACTION"))
        return false;

    size_t applied = 0;

    for (const BookChange& change : changes)
    {
        if (!isValidGuid(change.guid))
        {
            results->emplace_back(rejected(change.guid, "invalid_guid"));
            continue;
        }

        if (!change.parent_guid.empty() && !isValidGuid(change.parent_guid))
        {
            results->emplace_back(rejected(change.guid, "invalid_parent"));
            continue;
        }

        if (change.parent_guid == change.guid)
        {
            results->emplace_back(rejected(change.guid, "parent_is_self"));
            continue;
        }

        BookEntry current;
        const bool exists = findEntry(book_guid, change.guid, &current);

        const int64_t expected = exists ? current.revision : 0;
        if (change.base_revision != expected)
        {
            BookChangeResult result;
            result.guid = change.guid;
            result.status = BookChangeResult::Status::CONFLICT;
            result.current = current;
            results->emplace_back(std::move(result));
            continue;
        }

        if (change.kind == BookEntry::Kind::GROUP && !change.deleted &&
            !change.parent_guid.empty() &&
            wouldCreateCycle(db_, book_id, change.guid, change.parent_guid))
        {
            results->emplace_back(rejected(change.guid, "would_create_cycle"));
            continue;
        }

        // A headstone keeps no payload. The content is of no use to the book once the record is
        // gone, and keeping a sealed password around for the months a headstone lives would be
        // careless. The history does keep it, for as long as the administrator set and no longer:
        // there it is what an accidental deletion is undone from.
        const std::string payload = change.deleted ? std::string() : change.payload;

        // A deletion comes with the guid alone: the console does not say what it deletes. The
        // record stays what it was, or its headstone and its history would call a computer a
        // group, and the history would open its payload as the wrong kind of record.
        const BookEntry::Kind kind = (change.deleted && exists) ? current.kind : change.kind;

        if (!change.deleted && payload.empty())
        {
            results->emplace_back(rejected(change.guid, "empty_payload"));
            continue;
        }

        Statement statement(db_,
            "INSERT INTO book_entries (book_id, guid, parent_guid, kind, revision, server_time, "
            "client_time, modified_by, deleted, deleted_at, payload) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(book_id, guid) DO UPDATE SET "
            "parent_guid = excluded.parent_guid, kind = excluded.kind, "
            "revision = excluded.revision, server_time = excluded.server_time, "
            "client_time = excluded.client_time, modified_by = excluded.modified_by, "
            "deleted = excluded.deleted, deleted_at = excluded.deleted_at, "
            "payload = excluded.payload");

        if (!statement.isValid() ||
            !statement.bindInt64(1, book_id) ||
            !statement.bindText(2, change.guid) ||
            !statement.bindText(3, change.parent_guid) ||
            !statement.bindInt64(4, kind == BookEntry::Kind::COMPUTER ? 1 : 0) ||
            !statement.bindInt64(5, revision) ||
            !statement.bindInt64(6, now) ||
            !statement.bindInt64(7, change.client_time) ||
            !statement.bindText(8, modified_by) ||
            !statement.bindInt64(9, change.deleted ? 1 : 0) ||
            !statement.bindInt64(10, change.deleted ? now : 0) ||
            !statement.bindBlob(11, payload))
        {
            exec(db_, "ROLLBACK");
            return false;
        }

        if (statement.step() != SQLITE_DONE)
        {
            exec(db_, "ROLLBACK");
            return false;
        }

        if (keep_history)
        {
            // What the record was is exactly what the upsert above wrote over, and it is only
            // here, in |current|, for the length of this call. A record deleted by this batch
            // keeps its payload in the "before" half, which is what makes putting it back
            // possible.
            Statement history(db_,
                "INSERT OR REPLACE INTO book_history (book_id, revision, guid, kind, "
                "before_exists, before_kind, before_parent_guid, before_revision, "
                "before_server_time, before_modified_by, before_deleted, before_payload, "
                "after_parent_guid, after_deleted, after_payload) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");

            if (!history.isValid() ||
                !history.bindInt64(1, book_id) ||
                !history.bindInt64(2, revision) ||
                !history.bindText(3, change.guid) ||
                !history.bindInt64(4, kind == BookEntry::Kind::COMPUTER ? 1 : 0) ||
                !history.bindInt64(5, exists ? 1 : 0) ||
                !history.bindInt64(6, current.kind == BookEntry::Kind::COMPUTER ? 1 : 0) ||
                !history.bindText(7, current.parent_guid) ||
                !history.bindInt64(8, current.revision) ||
                !history.bindInt64(9, current.server_time) ||
                !history.bindText(10, current.modified_by) ||
                !history.bindInt64(11, current.deleted ? 1 : 0) ||
                !history.bindBlob(12, current.payload) ||
                !history.bindText(13, change.parent_guid) ||
                !history.bindInt64(14, change.deleted ? 1 : 0) ||
                !history.bindBlob(15, payload) ||
                history.step() != SQLITE_DONE)
            {
                exec(db_, "ROLLBACK");
                return false;
            }
        }

        BookChangeResult result;
        result.guid = change.guid;
        result.status = BookChangeResult::Status::OK;
        results->emplace_back(std::move(result));

        ++applied;
    }

    if (applied)
    {
        Statement bump(db_, "UPDATE books SET revision = ? WHERE book_id = ?");
        if (!bump.isValid() || !bump.bindInt64(1, revision) || !bump.bindInt64(2, book_id) ||
            bump.step() != SQLITE_DONE)
        {
            exec(db_, "ROLLBACK");
            return false;
        }

        Statement remember(db_,
            "INSERT INTO applied_ops (book_id, op_id, applied_at, revision) VALUES (?, ?, ?, ?)");
        if (!remember.isValid() || !remember.bindInt64(1, book_id) || !remember.bindText(2, op_id) ||
            !remember.bindInt64(3, now) || !remember.bindInt64(4, revision) ||
            remember.step() != SQLITE_DONE)
        {
            exec(db_, "ROLLBACK");
            return false;
        }

        if (keep_history)
        {
            // The label comes from the console and is taken only if it names a revision that has
            // been. Anything else would have the history say the book went back to a point it
            // never was at.
            const int64_t rollback_to =
                (note.rollback_to > 0 && note.rollback_to < revision) ? note.rollback_to : 0;

            Statement batch(db_,
                "INSERT OR REPLACE INTO book_batches "
                "(book_id, revision, server_time, modified_by, address, rollback_to) "
                "VALUES (?, ?, ?, ?, ?, ?)");
            if (!batch.isValid() || !batch.bindInt64(1, book_id) ||
                !batch.bindInt64(2, revision) || !batch.bindInt64(3, now) ||
                !batch.bindText(4, modified_by) || !batch.bindText(5, note.address) ||
                !batch.bindInt64(6, rollback_to) || batch.step() != SQLITE_DONE)
            {
                exec(db_, "ROLLBACK");
                return false;
            }
        }
    }

    if (!exec(db_, "COMMIT"))
    {
        exec(db_, "ROLLBACK");
        return false;
    }

    // Kept within its bound after every batch rather than once a day: the bound is there because
    // the disk of the router is small, and a busy day must not be what fills it.
    if (applied && keep_history && history_policy_.max_changes > 0)
        pruneHistoryByCount(book_id, history_policy_.max_changes);

    *new_revision = applied ? revision : book.revision;
    return true;
}

//--------------------------------------------------------------------------------------------------
int64_t BookStore::pruneTombstones(const std::string& book_guid, int64_t before)
{
    int64_t book_id = 0;
    if (!bookIdByGuid(book_guid, &book_id))
        return 0;

    Statement statement(db_,
        "DELETE FROM book_entries WHERE book_id = ? AND deleted = 1 AND deleted_at < ?");
    if (!statement.isValid() || !statement.bindInt64(1, book_id) ||
        !statement.bindInt64(2, before))
    {
        return 0;
    }

    if (statement.step() != SQLITE_DONE)
        return 0;

    return sqlite3_changes(db_);
}

//--------------------------------------------------------------------------------------------------
bool BookStore::historyBefore(const std::string& book_guid, int64_t before_revision,
                              int64_t max_batches, int64_t max_changes,
                              std::vector<BookHistoryBatch>* out, bool* has_more) const
{
    if (!out || !has_more || before_revision < 0 || max_batches <= 0)
        return false;

    out->clear();
    *has_more = false;

    int64_t book_id = 0;
    if (!bookIdByGuid(book_guid, &book_id))
        return false;

    // Zero means from the newest. No revision reaches the largest value, so it stands for "no
    // bound" without a second form of the query.
    const int64_t bound =
        (before_revision == 0) ? std::numeric_limits<int64_t>::max() : before_revision;

    Statement batches(db_,
        "SELECT revision, server_time, modified_by, address, rollback_to FROM book_batches "
        "WHERE book_id = ? AND revision < ? ORDER BY revision DESC LIMIT ?");
    if (!batches.isValid() || !batches.bindInt64(1, book_id) || !batches.bindInt64(2, bound) ||
        !batches.bindInt64(3, max_batches + 1))
    {
        return false;
    }

    for (;;)
    {
        const int result = batches.step();
        if (result == SQLITE_DONE)
            break;
        if (result != SQLITE_ROW)
            return false;

        // One more than asked for was selected, to tell "this is the last page" from "there is
        // exactly one batch left" without a second query.
        if (static_cast<int64_t>(out->size()) >= max_batches)
        {
            *has_more = true;
            break;
        }

        BookHistoryBatch batch;
        batch.revision = batches.columnInt64(0);
        batch.server_time = batches.columnInt64(1);
        batch.modified_by = batches.columnText(2);
        batch.address = batches.columnText(3);
        batch.rollback_to = batches.columnInt64(4);
        out->emplace_back(std::move(batch));
    }

    int64_t taken = 0;

    for (size_t i = 0; i < out->size(); ++i)
    {
        BookHistoryBatch& batch = (*out)[i];

        Statement changes(db_,
            "SELECT guid, kind, before_exists, before_kind, before_parent_guid, before_revision, "
            "before_server_time, before_modified_by, before_deleted, before_payload, "
            "after_parent_guid, after_deleted, after_payload FROM book_history "
            "WHERE book_id = ? AND revision = ? ORDER BY guid");
        if (!changes.isValid() || !changes.bindInt64(1, book_id) ||
            !changes.bindInt64(2, batch.revision))
        {
            return false;
        }

        for (;;)
        {
            const int result = changes.step();
            if (result == SQLITE_DONE)
                break;
            if (result != SQLITE_ROW)
                return false;

            BookHistoryChange change;
            change.guid = changes.columnText(0);
            change.kind = kindFromInt(changes.columnInt64(1));
            change.has_before = changes.columnInt64(2) != 0;

            if (change.has_before)
            {
                change.before.guid = change.guid;
                change.before.kind = kindFromInt(changes.columnInt64(3));
                change.before.parent_guid = changes.columnText(4);
                change.before.revision = changes.columnInt64(5);
                change.before.server_time = changes.columnInt64(6);
                change.before.modified_by = changes.columnText(7);
                change.before.deleted = changes.columnInt64(8) != 0;
                change.before.payload = changes.columnBlob(9);
            }

            change.after.guid = change.guid;
            change.after.kind = change.kind;
            change.after.parent_guid = changes.columnText(10);
            change.after.revision = batch.revision;
            change.after.server_time = batch.server_time;
            change.after.modified_by = batch.modified_by;
            change.after.deleted = changes.columnInt64(11) != 0;
            change.after.deleted_at = change.after.deleted ? batch.server_time : 0;
            change.after.payload = changes.columnBlob(12);

            batch.changes.emplace_back(std::move(change));
        }

        taken += static_cast<int64_t>(batch.changes.size());

        // The answer has a size limit of its own; see BookService. The page ends on a whole batch.
        if (max_changes > 0 && taken >= max_changes && i + 1 < out->size())
        {
            out->resize(i + 1);
            *has_more = true;
            break;
        }
    }

    return true;
}

//--------------------------------------------------------------------------------------------------
bool BookStore::oldestHistoryRevision(const std::string& book_guid, int64_t* out) const
{
    if (!out)
        return false;

    Book book;
    if (!findBook(book_guid, &book))
        return false;

    int64_t book_id = 0;
    if (!bookIdByGuid(book_guid, &book_id))
        return false;

    // Walked down from the newest for as long as the revisions follow each other. Every accepted
    // batch moves the revision by exactly one, so a missing number is a batch that is not in the
    // history - one made while it was switched off - and nothing before it can be undone
    // correctly: the record as that batch left it is simply not known.
    Statement statement(db_,
        "SELECT revision FROM book_batches WHERE book_id = ? ORDER BY revision DESC");
    if (!statement.isValid() || !statement.bindInt64(1, book_id))
        return false;

    int64_t expected = book.revision;

    for (;;)
    {
        const int result = statement.step();
        if (result == SQLITE_DONE)
            break;
        if (result != SQLITE_ROW)
            return false;

        if (statement.columnInt64(0) != expected)
            break;

        --expected;
    }

    *out = expected;
    return true;
}

//--------------------------------------------------------------------------------------------------
int64_t BookStore::pruneHistoryThrough(int64_t book_id, int64_t revision)
{
    Statement changes(db_, "DELETE FROM book_history WHERE book_id = ? AND revision <= ?");
    if (!changes.isValid() || !changes.bindInt64(1, book_id) || !changes.bindInt64(2, revision) ||
        changes.step() != SQLITE_DONE)
    {
        return 0;
    }

    const int64_t removed = sqlite3_changes(db_);

    Statement batches(db_, "DELETE FROM book_batches WHERE book_id = ? AND revision <= ?");
    if (!batches.isValid() || !batches.bindInt64(1, book_id) || !batches.bindInt64(2, revision) ||
        batches.step() != SQLITE_DONE)
    {
        LOG(LS_ERROR) << "Unable to remove old batches from the history";
    }

    return removed;
}

//--------------------------------------------------------------------------------------------------
int64_t BookStore::pruneHistoryByCount(int64_t book_id, int64_t max_changes)
{
    if (max_changes <= 0)
        return 0;

    // The revision of the first change beyond the limit, counted from the newest. Everything up to
    // and including its batch goes: cutting a batch in two would leave half of it impossible to
    // undo.
    int64_t through = -1;
    {
        Statement statement(db_,
            "SELECT revision FROM book_history WHERE book_id = ? "
            "ORDER BY revision DESC LIMIT 1 OFFSET ?");
        if (!statement.isValid() || !statement.bindInt64(1, book_id) ||
            !statement.bindInt64(2, max_changes))
        {
            return 0;
        }

        if (statement.step() != SQLITE_ROW)
            return 0; // Within the limit.

        through = statement.columnInt64(0);
    }

    return pruneHistoryThrough(book_id, through);
}

//--------------------------------------------------------------------------------------------------
int64_t BookStore::pruneHistory(const std::string& book_guid, int64_t before, int64_t max_changes)
{
    int64_t book_id = 0;
    if (!bookIdByGuid(book_guid, &book_id))
        return 0;

    int64_t removed = 0;

    // By age first: the newest batch older than |before|, and everything up to it.
    int64_t through = -1;
    {
        Statement statement(db_,
            "SELECT MAX(revision) FROM book_batches WHERE book_id = ? AND server_time < ?");
        if (statement.isValid() && statement.bindInt64(1, book_id) &&
            statement.bindInt64(2, before) && statement.step() == SQLITE_ROW &&
            sqlite3_column_type(statement.get(), 0) != SQLITE_NULL)
        {
            through = statement.columnInt64(0);
        }
    }

    if (through >= 0)
        removed += pruneHistoryThrough(book_id, through);

    removed += pruneHistoryByCount(book_id, max_changes);
    return removed;
}

//--------------------------------------------------------------------------------------------------
int64_t BookStore::pruneAppliedOps(int64_t before)
{
    Statement statement(db_, "DELETE FROM applied_ops WHERE applied_at < ?");
    if (!statement.isValid() || !statement.bindInt64(1, before))
        return 0;

    if (statement.step() != SQLITE_DONE)
        return 0;

    return sqlite3_changes(db_);
}

} // namespace router
