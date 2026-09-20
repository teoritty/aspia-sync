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

#ifndef CONSOLE_BOOK_LOCAL_CHANGES_H
#define CONSOLE_BOOK_LOCAL_CHANGES_H

#include <cstddef>

namespace proto::address_book {
class Data;
} // namespace proto::address_book

namespace console {

// Finds what has been changed in this copy of the book and has not reached the router yet.
//
// It could have been done the other way: have every place that edits the book say so. The console
// has a dozen of those - the dialogs, dragging a computer between groups, deleting a group, an
// import - and one of them forgotten would mean an edit that quietly never leaves the machine.
// That is exactly the failure this whole feature exists to prevent, so it is not a risk worth
// taking for the convenience.
//
// Instead the book is compared with what the router last gave: a record that differs from its
// stored base was changed here, one with no base at all is new, and a base whose record is gone
// was deleted. Nothing has to be remembered at the moment of the edit, so nothing can be
// forgotten, and it costs one pass over a few hundred records.

struct LocalChanges
{
    size_t added = 0;
    size_t changed = 0;
    size_t deleted = 0;

    size_t total() const { return added + changed + deleted; }
};

// Compares the book with its sync state and marks what has to be sent. Returns what it found, so
// the console can tell the person how much is waiting.
//
// Records already marked and not sent yet stay marked. A record that has been changed back to
// exactly what the router has stops being marked: there is nothing to send about it any more.
//
// Does nothing to a book that is not synchronized.
LocalChanges markLocalChanges(proto::address_book::Data* data);

} // namespace console

#endif // CONSOLE_BOOK_LOCAL_CHANGES_H
