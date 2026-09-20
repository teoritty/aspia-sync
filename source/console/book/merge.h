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

#ifndef CONSOLE_BOOK_MERGE_H
#define CONSOLE_BOOK_MERGE_H

#include <string>
#include <vector>

namespace proto::address_book {
class Computer;
class ComputerGroup;
} // namespace proto::address_book

namespace console {

// Bringing together two versions of one record.
//
// The router refuses a change built on a version that is no longer current and hands back what it
// has. It cannot do anything cleverer than that, because the content is sealed with a key it does
// not hold - only a console can look inside. So the merging happens here.
//
// It takes three versions: the record as the router had it when this console last saw it, the
// record as it is here now, and the record as the router has it today. With those three, a field
// that only one side touched can be taken from that side without asking anybody. One person edits
// a comment while another changes a password, and both edits survive without either of them
// noticing there was a conflict at all.
//
// Only a field both sides changed, to different values, has to be put to a person. That is rare
// by construction, and it is the one case where guessing would silently throw away somebody's
// work.

// Names of the fields that could not be merged. Empty means the merge is complete and can be sent
// without asking anyone.
struct MergeResult
{
    bool ok() const { return conflicts.empty(); }

    std::vector<std::string> conflicts;
};

// Merges one computer. |out| receives the result; on a conflicting field it keeps the local value,
// so that what the person sees is what they typed until they decide otherwise.
MergeResult mergeComputer(const proto::address_book::Computer& base,
                          const proto::address_book::Computer& local,
                          const proto::address_book::Computer& remote,
                          proto::address_book::Computer* out);

// The same for a group. Only the fields of the group itself are merged: its children are records
// of their own, and its expanded flag never leaves this machine.
MergeResult mergeComputerGroup(const proto::address_book::ComputerGroup& base,
                               const proto::address_book::ComputerGroup& local,
                               const proto::address_book::ComputerGroup& remote,
                               proto::address_book::ComputerGroup* out);

} // namespace console

#endif // CONSOLE_BOOK_MERGE_H
