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

#include "console/theme.h"

#include "base/logging.h"
#include "build/build_config.h"

#include <QApplication>
#include <QEvent>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>
#include <QWidget>

#if defined(OS_WIN)
#include <Windows.h>
#include <dwmapi.h>
#endif // defined(OS_WIN)

namespace console {

namespace {

bool g_dark = false;

// The style the application started with, so that switching back to light gives back exactly what
// people are used to rather than Fusion with light colours.
QString g_light_style;

//--------------------------------------------------------------------------------------------------
QPalette darkPalette()
{
    const QColor window(0x35, 0x35, 0x35);
    const QColor base(0x2a, 0x2a, 0x2a);
    const QColor text(0xe8, 0xe8, 0xe8);
    const QColor disabled(0x80, 0x80, 0x80);
    const QColor highlight(0x2a, 0x82, 0xda);

    QPalette palette;

    palette.setColor(QPalette::Window, window);
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Base, base);
    palette.setColor(QPalette::AlternateBase, window);
    palette.setColor(QPalette::ToolTipBase, window);
    palette.setColor(QPalette::ToolTipText, text);
    palette.setColor(QPalette::PlaceholderText, disabled);
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::Button, window);
    palette.setColor(QPalette::ButtonText, text);
    palette.setColor(QPalette::BrightText, Qt::red);
    palette.setColor(QPalette::Link, QColor(0x5a, 0xa9, 0xff));
    palette.setColor(QPalette::Highlight, highlight);
    palette.setColor(QPalette::HighlightedText, Qt::white);

    palette.setColor(QPalette::Light, QColor(0x50, 0x50, 0x50));
    palette.setColor(QPalette::Midlight, QColor(0x45, 0x45, 0x45));
    palette.setColor(QPalette::Mid, QColor(0x30, 0x30, 0x30));
    palette.setColor(QPalette::Dark, QColor(0x20, 0x20, 0x20));
    palette.setColor(QPalette::Shadow, QColor(0x14, 0x14, 0x14));

    palette.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
    palette.setColor(QPalette::Disabled, QPalette::Text, disabled);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
    palette.setColor(QPalette::Disabled, QPalette::Highlight, QColor(0x50, 0x50, 0x50));
    palette.setColor(QPalette::Disabled, QPalette::HighlightedText, disabled);

    return palette;
}

//--------------------------------------------------------------------------------------------------
// The title bar belongs to the window manager, not to Qt, so the palette does not reach it.
void setDarkTitleBar(QWidget* window, bool dark)
{
#if defined(OS_WIN)
    if (!window || !window->isWindow() || !window->testAttribute(Qt::WA_WState_Created))
        return;

    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    const BOOL value = dark ? TRUE : FALSE;

    // 20 is DWMWA_USE_IMMERSIVE_DARK_MODE from Windows 10 20H1 on; the builds before it (1809 and
    // 1903) knew the same thing under 19. Neither is in the SDK headers of every toolchain.
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &value, sizeof(value))))
        DwmSetWindowAttribute(hwnd, 19, &value, sizeof(value));

    // A window already on the screen keeps the title bar it was drawn with until something makes
    // the frame be drawn again.
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
#else
    Q_UNUSED(window);
    Q_UNUSED(dark);
#endif // defined(OS_WIN)
}

//--------------------------------------------------------------------------------------------------
// Gives every window that appears the title bar of the current theme. Without it the main window
// would be dark and every dialog opened from it would flash a white title bar.
class TitleBarFilter final : public QObject
{
public:
    explicit TitleBarFilter(QObject* parent)
        : QObject(parent)
    {
        // Nothing
    }

protected:
    bool eventFilter(QObject* object, QEvent* event) final
    {
        if (event->type() == QEvent::Show && object->isWidgetType())
        {
            QWidget* widget = static_cast<QWidget*>(object);
            if (widget->isWindow())
                setDarkTitleBar(widget, g_dark);
        }

        return QObject::eventFilter(object, event);
    }
};

} // namespace

//--------------------------------------------------------------------------------------------------
void applyTheme(bool dark)
{
    if (g_light_style.isEmpty())
    {
        g_light_style = QApplication::style()->objectName();
        qApp->installEventFilter(new TitleBarFilter(qApp));
    }

    g_dark = dark;

    LOG(LS_INFO) << "Theme: " << (dark ? "dark" : "light");

    if (dark)
    {
        QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
        QApplication::setPalette(darkPalette());
    }
    else
    {
        QApplication::setStyle(QStyleFactory::create(g_light_style));
        QApplication::setPalette(QApplication::style()->standardPalette());
    }

    // Windows already on the screen do not get a Show event, so they are done here.
    for (QWidget* widget : QApplication::topLevelWidgets())
        setDarkTitleBar(widget, dark);
}

//--------------------------------------------------------------------------------------------------
bool isDarkTheme()
{
    return g_dark;
}

//--------------------------------------------------------------------------------------------------
QColor errorColor()
{
    return g_dark ? QColor(0xff, 0x6b, 0x6b) : QColor(0xb0, 0x00, 0x20);
}

} // namespace console
