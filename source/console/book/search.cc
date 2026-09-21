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

#include "console/book/search.h"

#include <algorithm>
#include <vector>

namespace console {

namespace {

// How much a word is worth depending on where it was found. A word at the start of a word of the
// text is what somebody typing the beginning of a name produces, so it outranks the same letters
// found in the middle of one.
struct Weight
{
    int word_start;
    int inside;
};

const Weight kName = { 10, 6 };
const Weight kAddress = { 7, 5 };
const Weight kFolders = { 4, 3 };
const Weight kComment = { 3, 2 };

// The query read as a whole, found in the name as it was typed.
const int kWholeQueryInName = 15;

//--------------------------------------------------------------------------------------------------
bool isSpace(char16_t c)
{
    return c == u' ' || c == u'\t' || c == u'\r' || c == u'\n' || c == u' ';
}

//--------------------------------------------------------------------------------------------------
bool isWordChar(char16_t c)
{
    return (c >= u'0' && c <= u'9') ||
           (c >= u'a' && c <= u'z') ||
           (c >= u'A' && c <= u'Z') ||
           (c >= u'Ѐ' && c <= u'ӿ'); // Cyrillic.
}

//--------------------------------------------------------------------------------------------------
std::vector<std::u16string> splitWords(std::u16string_view text)
{
    std::vector<std::u16string> words;
    std::u16string current;

    for (char16_t c : text)
    {
        if (isSpace(c))
        {
            if (!current.empty())
                words.emplace_back(std::move(current));
            current.clear();
            continue;
        }

        current.push_back(c);
    }

    if (!current.empty())
        words.emplace_back(std::move(current));

    return words;
}

//--------------------------------------------------------------------------------------------------
// The weight of |word| in |text|, both folded: the start of a word where it begins one anywhere,
// the inside otherwise, and 0 when it is not there at all.
int weightIn(const std::u16string& text, const std::u16string& word, const Weight& weight)
{
    int best = 0;

    for (size_t pos = text.find(word); pos != std::u16string::npos; pos = text.find(word, pos + 1))
    {
        if (pos == 0 || !isWordChar(text[pos - 1]))
            return weight.word_start;

        best = weight.inside;
    }

    return best;
}

} // namespace

//--------------------------------------------------------------------------------------------------
std::u16string foldForSearch(std::u16string_view text)
{
    std::u16string result;
    result.reserve(text.size());

    for (char16_t c : text)
    {
        if (c >= u'A' && c <= u'Z')
            c = static_cast<char16_t>(c + (u'a' - u'A'));
        else if (c == u'Ё' || c == u'ё') // Ё, ё.
            c = u'е';
        else if (c >= u'А' && c <= u'Я') // А..Я.
            c = static_cast<char16_t>(c + 0x20);
        else if (c >= u'Ѐ' && c <= u'Џ') // Ѐ..Џ: Ukrainian, Belarusian, Serbian letters.
            c = static_cast<char16_t>(c + 0x50);

        result.push_back(c);
    }

    return result;
}

//--------------------------------------------------------------------------------------------------
int searchScore(std::u16string_view query, const SearchFields& fields)
{
    const std::vector<std::u16string> words = splitWords(foldForSearch(query));
    if (words.empty())
        return 0;

    const std::u16string name = foldForSearch(fields.name);
    const std::u16string address = foldForSearch(fields.address);
    const std::u16string comment = foldForSearch(fields.comment);
    const std::u16string folders = foldForSearch(fields.folders);

    int score = 0;

    for (const std::u16string& word : words)
    {
        // Each word counts once, for the best place it was found in.
        int best = weightIn(name, word, kName);
        best = std::max(best, weightIn(address, word, kAddress));
        best = std::max(best, weightIn(folders, word, kFolders));
        best = std::max(best, weightIn(comment, word, kComment));

        if (best == 0)
            return 0; // Every word has to be there.

        score += best;
    }

    if (words.size() > 1)
    {
        std::u16string whole = words.front();
        for (size_t i = 1; i < words.size(); ++i)
        {
            whole += u' ';
            whole += words[i];
        }

        if (name.find(whole) != std::u16string::npos)
            score += kWholeQueryInName;
    }

    return score;
}

} // namespace console
