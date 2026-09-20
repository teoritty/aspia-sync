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

#ifndef CONSOLE_BOOK_FLAT_BOOK_H
#define CONSOLE_BOOK_FLAT_BOOK_H

#include <string>
#include <vector>

namespace proto::address_book {
class ComputerGroup;
} // namespace proto::address_book

namespace console {

// The address book as a list of records instead of a tree.
//
// In the file the tree is the nesting itself: a computer is inside the group message that holds
// it, and there is no link back to the parent. Nothing can be sent on its own that way - to send
// one computer from a group five levels down, the whole branch from the root would have to go
// with it, and that is the opposite of what synchronization needs.
//
// Flattened, every record stands by itself and names its parent. The shape of the tree is
// preserved exactly, to any depth, and moving a group with a hundred computers in it becomes a
// change of one field in one record instead of a hundred and one records on the wire.

struct FlatEntry
{
    enum class Kind
    {
        GROUP    = 0,
        COMPUTER = 1
    };

    std::string guid;

    // The group holding this record. Empty means the top level of the book: the root group is not
    // a record, because each person has a root of their own.
    std::string parent_guid;

    Kind kind = Kind::GROUP;

    // The record itself, serialized. For a group it carries neither its children nor its expanded
    // flag: the children are records of their own, and whether a folder is open is a state of one
    // person's window, not of the book everybody shares.
    std::string payload;
};

// Turns the tree into a list. Every record must already carry a guid (see ensureEntryGuids); one
// that does not is skipped along with everything under it, because a record nothing can name
// cannot be placed back afterwards.
//
// The root group itself is not in the list. It is the book rather than something in it, and each
// person's copy has a root of its own, made when they first opened their file.
std::vector<FlatEntry> flattenBook(const proto::address_book::ComputerGroup& root);

// Builds the tree back under |root|, which keeps everything of its own - its guid, its name, its
// expanded flag - and has only its children replaced. The result of rebuildBook(flattenBook(x), &y)
// is x's contents under y's own root, minus the expanded flags.
//
// The input comes from the network and is not to be trusted:
//
//   - a record whose parent is missing is attached to the root rather than dropped. Losing a
//     computer because the folder it was in did not arrive is not acceptable;
//   - the same happens to a record whose parents form a cycle, which two people can produce
//     without either of them doing anything wrong: one moves A into B while the other moves B
//     into A;
//   - a repeated guid is taken once, the first time it appears;
//   - a payload that does not parse is skipped, and so is a record of an unknown kind. One
//     damaged record must not cost the whole book.
//
// Returns false only when |root| is null; an empty list gives an empty book, which is what a
// book everybody has emptied should look like. |skipped| receives the number of records that were not placed for any of the reasons above, so
// the caller can tell the person that the book did not arrive whole.
bool rebuildBook(const std::vector<FlatEntry>& entries,
                 proto::address_book::ComputerGroup* root,
                 size_t* skipped = nullptr);

} // namespace console

#endif // CONSOLE_BOOK_FLAT_BOOK_H
