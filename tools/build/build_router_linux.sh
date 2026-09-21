#!/bin/sh
#
# Aspia Project
# Copyright (C) 2016-2024 Dmitry Chapyshev <dmitry@aspia.ru>
#
# Builds the router for Linux, runs its tests and packs it into a .deb.
#
# Run it on the oldest system the router has to run on - Ubuntu 24.04. A program built there runs on
# anything newer; one built on something newer does not run on 24.04.
#
# Needs (apt): build-essential cmake ninja-build git curl zip unzip tar pkg-config nasm
#              autoconf autoconf-archive automake libtool python3 bison flex
#
set -eu

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="$REPO/builds/linux-router"

export VCPKG_ROOT="$REPO/vcpkg4aspia"
export VCPKG_INSTALLED_DIR="$BUILD_DIR/vcpkg_installed"

if [ ! -e "$VCPKG_ROOT/.git" ]; then
    echo "ERROR: the vcpkg4aspia submodule is not checked out. Run: git submodule update --init" >&2
    exit 1
fi

if [ ! -x "$VCPKG_ROOT/vcpkg" ]; then
    "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics
fi

cd "$REPO"

# The first run builds the dependencies from source and takes a while; later ones reuse them.
cmake --preset linux-router
cmake --build "$BUILD_DIR" --config Release
ctest --preset linux-router --output-on-failure

cd "$BUILD_DIR"
cpack -C Release

echo
echo "Package:"
ls -1 "$BUILD_DIR"/*.deb
