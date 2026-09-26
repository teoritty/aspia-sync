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

list(APPEND SOURCE_ROUTER
    database.h
    database_factory.h
    database_factory_sqlite.cc
    database_factory_sqlite.h
    database_sqlite.cc
    database_sqlite.h
    main.cc
    server.cc
    server.h
    service.cc
    service.h
    service_constants.cc
    service_constants.h
    session.cc
    session.h
    session_admin.cc
    session_admin.h
    session_client.cc
    session_client.h
    session_host.cc
    session_host.h
    session_relay.cc
    session_relay.h
    settings.cc
    settings.h
    shared_key_pool.cc
    shared_key_pool.h
    user_list_db.cc
    user_list_db.h)

if (WIN32)
    list(APPEND SOURCE_ROUTER_WIN
        win/router.rc
        win/service_util.cc
        win/service_util.h)
endif()

source_group("" FILES ${SOURCE_ROUTER})

if (WIN32)
    source_group(win FILES ${SOURCE_ROUTER_WIN})
    set(ROUTER_PLATFORM_LIBS comsupp crypt32 dbghelp netapi32 version)
endif()

if (LINUX)
    set(ROUTER_PLATFORM_LIBS ICU::uc ICU::dt xdg_user_dirs)
endif()

# The shared address books, as the router keeps them. A library of its own: the router executable
# cannot be linked into a test, and keeping this apart from the router keeps merges with upstream
# mechanical. It depends on sqlite and base, not on the sessions or the server.
list(APPEND SOURCE_ROUTER_BOOK
    book/book_service.cc
    book/book_service.h
    book/book_store.cc
    book/book_store.h)

list(APPEND SOURCE_ROUTER_BOOK_TESTS
    book/book_store_unittest.cc)

# Drives the console side against the router side, so it needs the console's library - which is
# not there when only the router is built. The store tests still run everywhere.
if (NOT ASPIA_BUILD_ROUTER_ONLY)
    list(APPEND SOURCE_ROUTER_BOOK_TESTS
        book/history_roundtrip_unittest.cc
        book/sync_roundtrip_unittest.cc)
    set(ROUTER_TESTS_CONSOLE_LIBS aspia_console_book)
endif()

add_library(aspia_router_book STATIC ${SOURCE_ROUTER_BOOK})
target_link_libraries(aspia_router_book
    aspia_base
    aspia_proto
    ${Protobuf_LITE_LIBRARIES}
    unofficial::sqlite3::sqlite3)

# The roundtrip test drives the console side of the exchange against the router side, so the test
# executable - and nothing that ships - links both halves.
add_executable(aspia_router_tests ${SOURCE_ROUTER_BOOK_TESTS})
target_link_libraries(aspia_router_tests PRIVATE
    aspia_router_book
    ${ROUTER_TESTS_CONSOLE_LIBS}
    aspia_base
    aspia_proto
    GTest::gtest
    GTest::gtest_main
    OpenSSL::Crypto
    modp_b64
    ${Protobuf_LITE_LIBRARIES}
    unofficial::sqlite3::sqlite3
    ${ROUTER_PLATFORM_LIBS})
add_test(NAME aspia_router_tests COMMAND aspia_router_tests)

add_executable(aspia_router ${SOURCE_ROUTER} ${SOURCE_ROUTER_WIN})

if (WIN32)
    set_target_properties(aspia_router PROPERTIES LINK_FLAGS "/MANIFEST:NO")
endif()

target_link_libraries(aspia_router
    aspia_base
    aspia_proto
    aspia_router_book
    OpenSSL::Crypto
    modp_b64
    ${Protobuf_LITE_LIBRARIES}
    unofficial::sqlite3::sqlite3
    ${ROUTER_PLATFORM_LIBS})
