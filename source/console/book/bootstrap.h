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

#ifndef CONSOLE_BOOK_BOOTSTRAP_H
#define CONSOLE_BOOK_BOOTSTRAP_H

#include <string>
#include <vector>

namespace proto::address_book {
class ComputerGroup;
} // namespace proto::address_book

namespace console {

// Joining a book that already exists.
//
// Seven people have been keeping their own copies, and the same machine is in all of them under
// seven different identities - the guids were handed out separately on each console, so nothing
// says those records are the same computer. Matched wrongly, the department ends up with the same
// machine seven times over; matched not at all, the same.
//
// What does say it is the address. In this department every computer is reached by an id the
// router issued, so two records pointing at the same id are the same machine however differently
// they were named. Records entered by address and port are matched the same way, which is as good
// as it gets for those.
//
// This is a guess, not a fact - the same id can legitimately appear twice in one book under
// different session types - so nothing here is applied. The plan is shown, a person confirms it,
// and only then is anything written. It happens once, and it is the one moment when a mistake
// spoils the book for everybody at the same time.

struct BootstrapPair
{
    // The record on this machine, and the one already in the shared book that it appears to be.
    std::string local_guid;
    std::string remote_guid;

    // What each of them is called, so a person can see what they are agreeing to.
    std::string local_name;
    std::string remote_name;

    // Where they agree to be found. The same on both sides by construction.
    std::string address;

    // The names differ, so keeping one means losing the other. Worth pointing out; the rest can
    // be confirmed without reading.
    bool names_differ = false;
};

struct BootstrapPlan
{
    // Records that are the same machine on both sides. Taking the shared version keeps everybody
    // on one name; taking the local one renames it for the whole department.
    std::vector<BootstrapPair> matched;

    // On this machine and nowhere else: these are what this person contributes.
    std::vector<std::string> only_local;

    // Records whose address appears more than once on one of the sides. They are set aside rather
    // than guessed at, because there is no telling which of the several is meant.
    std::vector<std::string> ambiguous;
};

// Works out what joining would do. Neither book is modified.
//
// |local| is the book on this machine, |remote| the one rebuilt from what the router sent.
BootstrapPlan planBootstrap(const proto::address_book::ComputerGroup& local,
                            const proto::address_book::ComputerGroup& remote);

} // namespace console

#endif // CONSOLE_BOOK_BOOTSTRAP_H
