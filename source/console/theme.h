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

#ifndef CONSOLE_THEME_H
#define CONSOLE_THEME_H

#include <QColor>

namespace console {

// Switches the whole application between the light look it has always had and a dark one.
//
// Qt 5 does not follow the dark mode of Windows by itself, so the dark look is made here: the
// Fusion style with a dark palette, and the title bars of the windows darkened through the window
// manager, which Qt leaves alone. Windows opened afterwards - dialogs, session windows - get the
// same title bar as they appear.
void applyTheme(bool dark);

bool isDarkTheme();

// For text that says something went wrong. One fixed red is either too dark to read on the dark
// background or too pale on the light one.
QColor errorColor();

} // namespace console

#endif // CONSOLE_THEME_H
