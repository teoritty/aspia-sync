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

#ifndef CONSOLE_BOOK_ENTRY_GUID_H
#define CONSOLE_BOOK_ENTRY_GUID_H

#include <cstddef>
#include <string>

namespace proto::address_book {
class Computer;
class ComputerGroup;
} // namespace proto::address_book

namespace console {

// The identity of a record in the address book.
//
// A name says nothing: two computers may be called the same, and a rename would turn a record into
// a different one. A position in the tree says nothing either, because moving a record between
// groups is an ordinary thing to do. Only a guid assigned once and kept for the life of the record
// answers "this is the same entry", which is what merging two books and synchronizing them need.
//
// Books written before this field existed carry no guid at all, so opening one has to give every
// record its own. That is done once, on load, and the book is saved with the guids in it.

// Generates a guid for a record. Version 4, as base::Guid::create() makes them.
std::string createEntryGuid();

// Answers whether the string is a guid this code would have produced. A record whose guid is
// malformed is treated as having none: a value that cannot be believed is worse than an absent
// one, because the records around it would be matched against it.
bool isValidEntryGuid(const std::string& guid);

// Walks the group and everything under it and gives a guid to every record that has none or whose
// guid is malformed. Returns the number of records it had to touch, so the caller knows whether
// the book has to be written back.
//
// Records that already carry a valid guid are left exactly as they are: this runs on every open,
// and a guid that changed between two opens would defeat the whole point of having one.
size_t ensureEntryGuids(proto::address_book::ComputerGroup* group);

} // namespace console

#endif // CONSOLE_BOOK_ENTRY_GUID_H
