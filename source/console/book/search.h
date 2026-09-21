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

#ifndef CONSOLE_BOOK_SEARCH_H
#define CONSOLE_BOOK_SEARCH_H

#include <string>
#include <string_view>

namespace console {

// What a computer can be found by.
struct SearchFields
{
    std::u16string name;
    std::u16string address;
    std::u16string comment;

    // The names of the folders it sits in, from the top down, in any form - they are searched as
    // one piece of text.
    std::u16string folders;
};

// How well a computer answers a search: 0 when it does not, and the higher the better otherwise.
//
// The query is taken as words, and every word has to be found somewhere - in the name, the address,
// the comment or the folders - in any order and anywhere inside a word. So "героев 2" finds
// "Героев Сталинграда касса №2", which is how people remember machines: by a couple of pieces of
// the name, not by the name.
//
// Of the computers that match, the ones where the words are in the name come first, words found at
// the start of a word count for more than words found inside one, and the query read as a whole
// counts for most of all.
//
// Case is ignored, and so is the difference between Ё and Е, which people type either way.
int searchScore(std::u16string_view query, const SearchFields& fields);

// The form text is compared in: lower case, Ё as Е. Latin and Cyrillic are folded; other scripts
// are left as they are, which still finds them when they are typed the way they are written.
std::u16string foldForSearch(std::u16string_view text);

} // namespace console

#endif // CONSOLE_BOOK_SEARCH_H
