#
# Aspia Project
# Copyright (C) 2016-2024 Dmitry Chapyshev <dmitry@aspia.ru>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <https://www.gnu.org/licenses/>.
#

include(translations)

list(APPEND SOURCE_CONSOLE
    address_book_dialog.cc
    address_book_dialog.h
    address_book_dialog.ui
    address_book_tab.cc
    address_book_tab.h
    address_book_tab.ui
    application.cc
    application.h
    computer_dialog.cc
    computer_dialog.h
    computer_dialog.ui
    computer_dialog_desktop.cc
    computer_dialog_desktop.h
    computer_dialog_desktop.ui
    computer_dialog_general.cc
    computer_dialog_general.h
    computer_dialog_general.ui
    computer_dialog_parent.cc
    computer_dialog_parent.h
    computer_dialog_parent.ui
    computer_dialog_tab.h
    computer_drag.h
    computer_factory.cc
    computer_factory.h
    computer_group_dialog.cc
    computer_group_dialog.h
    computer_group_dialog.ui
    computer_group_dialog_desktop.cc
    computer_group_dialog_desktop.h
    computer_group_dialog_desktop.ui
    computer_group_dialog_general.cc
    computer_group_dialog_general.h
    computer_group_dialog_general.ui
    computer_group_dialog_parent.cc
    computer_group_dialog_parent.h
    computer_group_dialog_parent.ui
    computer_group_dialog_tab.h
    computer_group_drag.h
    computer_group_item.cc
    computer_group_item.h
    computer_group_mime_data.h
    computer_group_tree.cc
    computer_group_tree.h
    computer_item.cc
    computer_item.h
    computer_mime_data.h
    computer_tree.cc
    computer_tree.h
    console.rc
    main.cc
    fast_connect_dialog.cc
    fast_connect_dialog.h
    fast_connect_dialog.ui
    import_export_util.cc
    import_export_util.h
    main_window.cc
    main_window.h
    main_window.ui
    mru.cc
    mru.h
    mru_action.cc
    mru_action.h
    open_address_book_dialog.cc
    open_address_book_dialog.h
    open_address_book_dialog.ui
    settings.cc
    settings.h
    statusbar.cc
    statusbar.h
    sync_dialog.cc
    sync_dialog.h
    sync_wizard.cc
    sync_wizard.h
    update_settings_dialog.cc
    update_settings_dialog.h
    update_settings_dialog.ui)

list(APPEND SOURCE_CONSOLE_RESOURCES resources/console.qrc)

source_group("" FILES ${SOURCE_CONSOLE})
source_group(moc FILES ${SOURCE_CONSOLE_MOC})
source_group(resources FILES ${SOURCE_CONSOLE_RESOURCES})

if (WIN32)
    set(CONSOLE_PLATFORM_LIBS
        avrt
        crypt32
        dbghelp
        dwmapi
        imm32
        iphlpapi
        netapi32
        shlwapi
        userenv
        uxtheme
        version
        winmm
        wtsapi32)
endif()

# The model of the address book - identity of its records, and later on everything the
# synchronization needs. It is a library of its own for two reasons: the console executable cannot
# be linked into a test, and keeping this code apart from the console keeps merges with upstream
# mechanical. It depends on the protocol and on base, never on Qt or on the console.
list(APPEND SOURCE_CONSOLE_BOOK
    book/bootstrap.cc
    book/bootstrap.h
    book/book_controller.cc
    book/book_controller.h
    book/book_sync.cc
    book/book_sync.h
    book/entry_guid.cc
    book/entry_guid.h
    book/flat_book.cc
    book/flat_book.h
    book/local_changes.cc
    book/local_changes.h
    book/merge.cc
    book/merge.h
    book/sync_engine.cc
    book/sync_engine.h
    book/sync_key.cc
    book/sync_key.h)

list(APPEND SOURCE_CONSOLE_BOOK_TESTS
    book/book_sync_unittest.cc
    book/bootstrap_unittest.cc
    book/entry_guid_unittest.cc
    book/flat_book_unittest.cc
    book/local_changes_unittest.cc
    book/merge_unittest.cc
    book/sync_engine_unittest.cc
    book/sync_key_unittest.cc)

add_library(aspia_console_book STATIC ${SOURCE_CONSOLE_BOOK})
target_link_libraries(aspia_console_book
    aspia_base
    aspia_proto
    ${THIRD_PARTY_LIBS})

add_executable(aspia_console_tests ${SOURCE_CONSOLE_BOOK_TESTS})
target_link_libraries(aspia_console_tests PRIVATE
    aspia_console_book
    aspia_base
    aspia_proto
    GTest::gtest
    GTest::gtest_main
    ${THIRD_PARTY_LIBS})
add_test(NAME aspia_console_tests COMMAND aspia_console_tests)

add_executable(aspia_console MACOSX_BUNDLE
    ${aspia_console_ICON}
    ${SOURCE_CONSOLE}
    ${SOURCE_CONSOLE_RESOURCES})

if (WIN32)
    set_target_properties(aspia_console PROPERTIES WIN32_EXECUTABLE TRUE)
    set_target_properties(aspia_console PROPERTIES LINK_FLAGS "/MANIFEST:NO")
endif()

target_link_libraries(aspia_console
    aspia_base
    aspia_client_core
    aspia_console_book
    aspia_common
    aspia_proto
    aspia_qt_base
    ${CONSOLE_PLATFORM_LIBS}
    ${QT_COMMON_LIBS}
    ${QT_PLATFORM_LIBS}
    ${THIRD_PARTY_LIBS})
set_property(TARGET aspia_console PROPERTY AUTOMOC ON)
set_property(TARGET aspia_console PROPERTY AUTOUIC ON)
set_property(TARGET aspia_console PROPERTY AUTORCC ON)
qt5_import_plugins(aspia_console
    INCLUDE ""
    EXCLUDE ""
    INCLUDE_BY_TYPE ""
    EXCLUDE_BY_TYPE imageformats)

if(Qt5LinguistTools_FOUND)
    # Get the list of translation files.
    file(GLOB CONSOLE_TS_FILES translations/*.ts)
    add_translations(aspia_console QRC_FILE "translations/console_translations.qrc" TS_FILES ${CONSOLE_TS_FILES})

    # Get the list of files containing messages.
    file(GLOB_RECURSE CONSOLE_TR_SOURCES
        ${PROJECT_SOURCE_DIR}/source/console/*.cc
        ${PROJECT_SOURCE_DIR}/source/console/*.cpp
        ${PROJECT_SOURCE_DIR}/source/console/*.h
        ${PROJECT_SOURCE_DIR}/source/console/*.ui)

    create_translation(TS_CONSOLE_UPDATE ${CONSOLE_TR_SOURCES} ${CONSOLE_TS_FILES})
    add_custom_target(aspia_console_ts_update DEPENDS ${TS_CONSOLE_UPDATE})
endif()
