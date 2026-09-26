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

cmake_minimum_required(VERSION 3.21.0)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

project(aspia)

set(ASPIA_VERSION_MAJOR 2)
set(ASPIA_VERSION_MINOR 7)
set(ASPIA_VERSION_PATCH 1)

# On CI installed dir could be outside of vcpkg root
if ("$ENV{VCPKG_INSTALLED_DIR}" STREQUAL "")
    set(VCPKG_INSTALLED_DIR $ENV{VCPKG_ROOT}/installed)
    message(STATUS "VCPKG_INSTALLED_DIR -> redefine: ${VCPKG_INSTALLED_DIR}")
else()
    set(VCPKG_INSTALLED_DIR $ENV{VCPKG_INSTALLED_DIR})
    message(STATUS "VCPKG_INSTALLED_DIR -> external: ${VCPKG_INSTALLED_DIR}")
endif()

add_definitions(-DASPIA_VERSION_MAJOR=${ASPIA_VERSION_MAJOR}
                -DASPIA_VERSION_MINOR=${ASPIA_VERSION_MINOR}
                -DASPIA_VERSION_PATCH=${ASPIA_VERSION_PATCH})

# The router alone, with nothing that needs a screen: no Qt, no codecs, no console, host or client.
# It is how the router is built for Linux, where it runs as a server and nothing else of the project
# has any use. Off by default, so the Windows build is what it always was.
option(ASPIA_BUILD_ROUTER_ONLY "Build only the router and what it depends on" OFF)

if (ASPIA_BUILD_ROUTER_ONLY)
    message(STATUS "Building the router only")
endif()

if (UNIX AND NOT APPLE)
    message(STATUS "Linux detected")
    set(LINUX TRUE)
endif()

if (WIN32)
    message(STATUS "Windows detected")
endif()

set(CMAKE_SKIP_BUILD_RPATH ON)

find_package(asio CONFIG REQUIRED)
find_package(CURL CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(protobuf CONFIG REQUIRED)
find_package(RapidJSON CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(zstd CONFIG REQUIRED)

if (NOT ASPIA_BUILD_ROUTER_ONLY)
    find_package(Qt5 REQUIRED Core Gui Network PrintSupport Widgets Xml)
    find_package(Qt5LinguistTools)
    find_package(libyuv CONFIG REQUIRED)
    find_package(Opus CONFIG REQUIRED)
    find_package(unofficial-libvpx CONFIG REQUIRED)
endif()

if (WIN32)
    if (NOT ASPIA_BUILD_ROUTER_ONLY)
        find_package(Qt5WinExtras REQUIRED)
    endif()
    find_path(WTL_INCLUDE_DIRS "atlapp.h")
endif()

if (UNIX)
    find_package(ICU REQUIRED uc dt)
endif()

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

enable_testing()

add_subdirectory(source)
