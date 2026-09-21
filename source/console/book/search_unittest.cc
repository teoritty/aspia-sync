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

#include <gtest/gtest.h>

namespace console {

namespace {

SearchFields named(const char16_t* name)
{
    SearchFields fields;
    fields.name = name;
    return fields;
}

} // namespace

//--------------------------------------------------------------------------------------------------
// The case that started it: two pieces of a name, one of them a number glued to a sign.
TEST(search_test, finds_a_name_by_pieces_of_it)
{
    EXPECT_GT(searchScore(u"героев 2", named(u"Героев Сталинграда касса №2")), 0);
}

//--------------------------------------------------------------------------------------------------
TEST(search_test, the_order_of_the_words_does_not_matter)
{
    EXPECT_GT(searchScore(u"2 героев", named(u"Героев Сталинграда касса №2")), 0);
    EXPECT_GT(searchScore(u"касса сталинграда", named(u"Героев Сталинграда касса №2")), 0);
}

//--------------------------------------------------------------------------------------------------
TEST(search_test, every_word_has_to_be_found)
{
    EXPECT_EQ(searchScore(u"героев 3", named(u"Героев Сталинграда касса №2")), 0);
    EXPECT_EQ(searchScore(u"героев склад", named(u"Героев Сталинграда касса №2")), 0);
}

//--------------------------------------------------------------------------------------------------
TEST(search_test, case_and_yo_are_ignored)
{
    EXPECT_GT(searchScore(u"ГЕРОЕВ", named(u"героев")), 0);
    EXPECT_GT(searchScore(u"ёлка", named(u"Елка")), 0);
    EXPECT_GT(searchScore(u"елка", named(u"Ёлка")), 0);
    EXPECT_GT(searchScore(u"server", named(u"SERVER-01")), 0);
}

//--------------------------------------------------------------------------------------------------
TEST(search_test, spaces_around_and_between_the_words_do_not_count)
{
    EXPECT_GT(searchScore(u"  героев    2 ", named(u"Героев Сталинграда касса №2")), 0);
}

//--------------------------------------------------------------------------------------------------
TEST(search_test, a_query_of_nothing_finds_nothing)
{
    EXPECT_EQ(searchScore(u"", named(u"Героев")), 0);
    EXPECT_EQ(searchScore(u"   ", named(u"Героев")), 0);
}

//--------------------------------------------------------------------------------------------------
// The words may come from different places: the name, the ID, the folders, the comment.
TEST(search_test, words_are_looked_for_in_every_field)
{
    SearchFields fields;
    fields.name = u"Касса 2";
    fields.address = u"123456789";
    fields.comment = u"у окна";
    fields.folders = u"Магазины / Героев Сталинграда";

    EXPECT_GT(searchScore(u"123456", fields), 0);
    EXPECT_GT(searchScore(u"героев касса", fields), 0);
    EXPECT_GT(searchScore(u"окна", fields), 0);
    EXPECT_GT(searchScore(u"магазины 2", fields), 0);
    EXPECT_EQ(searchScore(u"склад", fields), 0);
}

//--------------------------------------------------------------------------------------------------
TEST(search_test, the_name_counts_for_more_than_the_comment)
{
    SearchFields in_name = named(u"Касса");

    SearchFields in_comment = named(u"Компьютер у входа");
    in_comment.comment = u"стоит рядом с кассой";

    EXPECT_GT(searchScore(u"касс", in_name), searchScore(u"касс", in_comment));
}

//--------------------------------------------------------------------------------------------------
TEST(search_test, the_name_counts_for_more_than_the_folders)
{
    SearchFields in_name = named(u"Бухгалтерия 1");

    SearchFields in_folder = named(u"PC-07");
    in_folder.folders = u"Бухгалтерия";

    EXPECT_GT(searchScore(u"бухгалтерия", in_name), searchScore(u"бухгалтерия", in_folder));
}

//--------------------------------------------------------------------------------------------------
TEST(search_test, the_start_of_a_word_counts_for_more_than_its_middle)
{
    EXPECT_GT(searchScore(u"касс", named(u"Касса 1")), searchScore(u"касс", named(u"Прокассир")));
}

//--------------------------------------------------------------------------------------------------
TEST(search_test, the_query_as_a_whole_counts_for_most)
{
    const int together = searchScore(u"касса 2", named(u"Касса 2 у окна"));
    const int apart = searchScore(u"касса 2", named(u"Касса у окна 2"));

    EXPECT_GT(apart, 0);
    EXPECT_GT(together, apart);
}

//--------------------------------------------------------------------------------------------------
TEST(search_test, folding)
{
    EXPECT_EQ(foldForSearch(u"АБВ ЁЁ Ab-12"), u"абв ее ab-12");
    EXPECT_EQ(foldForSearch(u"ІЇЄ"), u"іїє");
}

} // namespace console
