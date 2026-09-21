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

include(CheckIPOSupported)
include(GNUInstallDirs)

# Qt configuration.
set(CMAKE_INCLUDE_CURRENT_DIR ON)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
list(APPEND CMAKE_AUTORCC_OPTIONS -compress 9 -threshold 5)

if (NOT ASPIA_BUILD_ROUTER_ONLY AND NOT Qt5LinguistTools_FOUND)
    message(WARNING "Qt5 linguist tools not found. Internationalization support will be disabled.")
    add_definitions(-DI18L_DISABLED)
endif()

if (WIN32)
    # Target version.
    add_definitions(-DNTDDI_VERSION=0x06010000
                    -D_WIN32_WINNT=0x0601
                    -D_WIN32_WINDOWS=_WIN32_WINNT
                    -DWINVER=_WIN32_WINNT
                    -D_WIN32_IE=0x0800
                    -DPSAPI_VERSION=2)

    # Other definitions.
    add_definitions(-D_UNICODE
                    -DUNICODE
                    -DWIN32_LEAN_AND_MEAN
                    -DNOMINMAX)
endif()

# For Asio.
add_definitions(-DASIO_STANDALONE
                -DASIO_NO_DEPRECATED
                -D_SILENCE_CXX17_ALLOCATOR_VOID_DEPRECATION_WARNING)

if (${CMAKE_SYSTEM_PROCESSOR} MATCHES "AMD64" OR ${CMAKE_SYSTEM_PROCESSOR} MATCHES "x86")
    # For RapidJSON.
    add_definitions(-DRAPIDJSON_SSE2)
endif()

# For Qt.
add_definitions(-DQT_NO_CAST_TO_ASCII
                -DQT_NO_CAST_FROM_BYTEARRAY
                -DQT_USE_QSTRINGBUILDER)

set(QT_COMMON_LIBS
    Qt5::Core
    Qt5::CorePrivate
    Qt5::QGenericEnginePlugin
    Qt5::Gui
    Qt5::GuiPrivate
    Qt5::QICOPlugin
    Qt5::Network
    Qt5::NetworkPrivate
    Qt5::PrintSupport
    Qt5::PrintSupportPrivate
    Qt5::Widgets
    Qt5::WidgetsPrivate
    Qt5::Xml
    Qt5::XmlPrivate)

if (WIN32)
    set(QT_PLATFORM_LIBS
        Qt5::WinMain
        Qt5::WinExtras
        Qt5::WinExtrasPrivate
        Qt5::QWindowsIntegrationPlugin
        Qt5::QWindowsVistaStylePlugin
        Qt5::QWindowsPrinterSupportPlugin)
endif()

set(THIRD_PARTY_LIBS
    CURL::libcurl
    fmt::fmt-header-only
    protobuf::libprotobuf-lite
    zstd::libzstd_static
    OpenSSL::Crypto
    OpenSSL::SSL
    modp_b64
    x11region)

# What only the parts that deal with sound and pictures use. A router-only build has none of them.
if (NOT ASPIA_BUILD_ROUTER_ONLY)
    list(APPEND THIRD_PARTY_LIBS
        Opus::opus
        libwebm
        unofficial::libvpx::libvpx
        yuv)
endif()

include_directories(${PROJECT_SOURCE_DIR}/source ${PROJECT_BINARY_DIR}/source ${WTL_INCLUDE_DIRS})

# C++ compliller flags.
set(CMAKE_CXX_STANDARD 20)

message(STATUS "CMAKE_SYSTEM_PROCESSOR: ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "CMAKE_HOST_SYSTEM_PROCESSOR: ${CMAKE_HOST_SYSTEM_PROCESSOR}")
message(STATUS "CMAKE_RUNTIME_OUTPUT_DIRECTORY: ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
message(STATUS "CMAKE_LIBRARY_OUTPUT_DIRECTORY: ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")
message(STATUS "CMAKE_BINARY_DIR: ${CMAKE_BINARY_DIR}")
message(STATUS "CMAKE_SOURCE_DIR: ${CMAKE_SOURCE_DIR}")
message(STATUS "CMAKE_COMMAND: ${CMAKE_COMMAND}")
message(STATUS "CMAKE_CPACK_COMMAND: ${CMAKE_CPACK_COMMAND}")
message(STATUS "CMAKE_MAKE_PROGRAM: ${CMAKE_MAKE_PROGRAM}")
message(STATUS "CMAKE_GENERATOR: ${CMAKE_GENERATOR}")

if (CMAKE_BUILD_TYPE EQUAL "DEBUG")
    message(STATUS "DEBUG build detected")
else()
    message(STATUS "RELEASE build detected")
endif()

if (MSVC)
    # C++ compliller flags.
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /W3 /MP")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /W3 /MP")

    # C compiller flags.
    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /W3 /MP")
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} /W3 /MP")

    if (${CMAKE_CL_64} EQUAL 0)
        message(STATUS "x86 compiller detected")
        set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /arch:SSE2")
        set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /arch:SSE2")
    else()
        message(STATUS "x64 compiller detected")
    endif()

    # Linker flags.
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /DEBUG /OPT:REF /IGNORE:4099")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /DEBUG /OPT:REF /IGNORE:4099")

    # Static runtime library.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if (LINUX)
    # C++ compliller flags.
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -fPIC")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fPIC")

    # C compiller flags.
    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -fPIC")
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -fPIC")
endif()

check_ipo_supported(RESULT IPO_SUPPORTED OUTPUT error)

if (IPO_SUPPORTED)
    message(STATUS "IPO/LTO supported")

    # Enable link-time code generatation.
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
else()
    message(STATUS "IPO/LTO not supported")
endif()

# Compiller flags.
message(STATUS "CXX compiller version: ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "CXX flags (release): ${CMAKE_CXX_FLAGS_RELEASE}")
message(STATUS "CXX flags (debug): ${CMAKE_CXX_FLAGS_DEBUG}")
message(STATUS "C compiller version: ${CMAKE_C_COMPILER_VERSION}")
message(STATUS "C flags (release): ${CMAKE_C_FLAGS_RELEASE}")
message(STATUS "C flags (debug): ${CMAKE_C_FLAGS_DEBUG}")

# Linker flags.
message(STATUS "Linker flags (shared release): ${CMAKE_SHARED_LINKER_FLAGS_RELEASE}")
message(STATUS "Linker flags (shared debug): ${CMAKE_SHARED_LINKER_FLAGS_DEBUG}")
message(STATUS "Linker flags (static release): ${CMAKE_STATIC_LINKER_FLAGS_RELEASE}")
message(STATUS "Linker flags (static debug): ${CMAKE_STATIC_LINKER_FLAGS_DEBUG}")
message(STATUS "Linker flags (exe release): ${CMAKE_EXE_LINKER_FLAGS_RELEASE}")
message(STATUS "Linker flags (exe debug): ${CMAKE_EXE_LINKER_FLAGS_DEBUG}")

# Get git branch info.
execute_process(COMMAND git rev-parse --abbrev-ref HEAD
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                OUTPUT_VARIABLE GIT_CURRENT_BRANCH
                OUTPUT_STRIP_TRAILING_WHITESPACE)

# Get git commit hash.
execute_process(COMMAND git rev-parse HEAD
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                OUTPUT_VARIABLE GIT_COMMIT_HASH
                OUTPUT_STRIP_TRAILING_WHITESPACE)

# Get git commit count.
execute_process(COMMAND git rev-list --count HEAD
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                OUTPUT_VARIABLE GIT_COMMIT_COUNT
                OUTPUT_STRIP_TRAILING_WHITESPACE)

if (NOT GIT_COMMIT_COUNT GREATER 0)
    message(STATUS "Failed to determine the number of commits")
    set(GIT_COMMIT_COUNT 0)
endif()

# Add git branch and commit hash to definitions.
add_definitions(-DGIT_CURRENT_BRANCH=\"${GIT_CURRENT_BRANCH}\"
                -DGIT_COMMIT_HASH=\"${GIT_COMMIT_HASH}\"
                -DGIT_COMMIT_COUNT=${GIT_COMMIT_COUNT})

message(STATUS "Git branch: ${GIT_CURRENT_BRANCH}")
message(STATUS "Git commit: ${GIT_COMMIT_HASH}")
message(STATUS "Git commit count: ${GIT_COMMIT_COUNT}")

add_subdirectory(base)
add_subdirectory(proto)
add_subdirectory(router)
add_subdirectory(third_party)

if (NOT ASPIA_BUILD_ROUTER_ONLY)
    add_subdirectory(client)
    add_subdirectory(common)
    add_subdirectory(console)
    add_subdirectory(qt_base)
    add_subdirectory(relay)
    add_subdirectory(tools)

    if (WIN32)
        add_subdirectory(host)
    endif()
endif()

###################################################################################################
# Installers
###################################################################################################

set(CPACK_PACKAGE_NAME "aspia")
set(CPACK_PACKAGE_VENDOR "Dmitry Chapyshev")
set(CPACK_PACKAGE_CONTACT "Dmitry Chapyshev <dmitry@aspia.ru>")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://aspia.org/")
set(CPACK_PACKAGE_VERSION "${ASPIA_VERSION_MAJOR}.${ASPIA_VERSION_MINOR}.${ASPIA_VERSION_PATCH}")
set(CPACK_PACKAGE_VERSION_MAJOR "${ASPIA_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${ASPIA_VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH "${ASPIA_VERSION_PATCH}")

#--------------------------------------------------------------------------------------------------
# Host
#--------------------------------------------------------------------------------------------------

#--------------------------------------------------------------------------------------------------
# Console
#--------------------------------------------------------------------------------------------------

if (NOT ASPIA_BUILD_ROUTER_ONLY)
    install(TARGETS aspia_console
        BUNDLE DESTINATION . COMPONENT console
        RUNTIME DESTINATION bin COMPONENT console)
    set(CPACK_COMPONENT_CONSOLE_NAME "aspia-console")
    set(CPACK_COMPONENT_CONSOLE_DISPLAY_NAME "Aspia Console")
    set(CPACK_COMPONENT_CONSOLE_DESCRIPTION "Console for managing hosts and routers.")
endif()

#--------------------------------------------------------------------------------------------------
# Client
#--------------------------------------------------------------------------------------------------

if (NOT ASPIA_BUILD_ROUTER_ONLY)
    install(TARGETS aspia_client
        BUNDLE DESTINATION . COMPONENT client
        RUNTIME DESTINATION bin COMPONENT client)
    set(CPACK_COMPONENT_CLIENT_NAME "aspia-client")
    set(CPACK_COMPONENT_CLIENT_DISPLAY_NAME "Aspia Client")
    set(CPACK_COMPONENT_CLIENT_DESCRIPTION "Client for managing hosts.")
endif()

#--------------------------------------------------------------------------------------------------
# Router
#--------------------------------------------------------------------------------------------------

if (LINUX)
    install(TARGETS aspia_router
        BUNDLE DESTINATION . COMPONENT router
        RUNTIME DESTINATION bin COMPONENT router)
    set(CPACK_COMPONENT_ROUTER_NAME "aspia-router")
    set(CPACK_COMPONENT_ROUTER_DISPLAY_NAME "Aspia Router")
    set(CPACK_COMPONENT_ROUTER_DESCRIPTION "Provides a connection routing service.")

    # Service file.
    install(FILES router/linux/aspia-router.service DESTINATION lib/systemd/system COMPONENT router)

    set(CPACK_DEBIAN_ROUTER_FILE_NAME
        "${CPACK_COMPONENT_ROUTER_NAME}-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_PROCESSOR}.deb")

    set(CPACK_GENERATOR "DEB")
    set(CPACK_DEB_COMPONENT_INSTALL ON)
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_PACKAGE_SECTION "net")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
    set(CPACK_COMPONENTS_ALL router)
endif()

#--------------------------------------------------------------------------------------------------
# Relay
#--------------------------------------------------------------------------------------------------

include(CPack)
