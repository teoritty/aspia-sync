# Building

The console, the client and the host are built for Windows x64 only. The router is also built for Linux, see below.

## What to install

| What | How |
|---|---|
| Visual Studio 2022 Build Tools, C++ workload | `winget install Microsoft.VisualStudio.2022.BuildTools` |
| The **ATL** component | `setup.exe modify --installPath "<path>" --add Microsoft.VisualStudio.Component.VC.ATL --quiet` |
| The **MFC** component | `setup.exe modify --installPath "<path>" --add Microsoft.VisualStudio.Component.VC.ATLMFC --quiet` |
| CMake 3.21+ | `winget install Kitware.CMake` |
| Ninja | `winget install Ninja-build.Ninja` |

`setup.exe` is in `%ProgramFiles(x86)%\Microsoft Visual Studio\Installer`; the commands need administrator rights.

ATL and MFC are not needed for their own sake: the `wtl` package from `vcpkg.json` pulls in the `atl` and `atlmfc` ports, and those need `atlbase.h` and `afxres.h` from the toolset. The base C++ workload does not include these components, and without them the build stops while installing dependencies. `env.cmd` checks for them and says so right away rather than half an hour in.

## How to build

```
git submodule update --init

tools\build\configure.cmd
tools\build\build.cmd
```

Or in one command, with the tests:

```
tools\build\verify.cmd
```

**The first configuration takes hours**: vcpkg builds Qt5 from source for a static triplet. Later ones take what is ready from the binary cache and finish in minutes. `builds/` will take about 20 GB.

## Why not the stock presets

`CMakePresets.json` has `ninja-multi-vcpkg-local-*` presets, but they do not work locally: their `VCPKG_ROOT` points to a `vcpkg` directory that the repository does not have (the submodule is called `vcpkg4aspia`), and the Windows preset has the path to Ninja from another developer's machine hardcoded. The scripts use the `ninja-multi-vcpkg-ci` preset, which takes everything from environment variables.

## If the build fails strangely

**Hundreds of errors inside `vcruntime.h`, mentions of `C:\msys64`.** CMake found a dependency (usually zstd) in a MinGW installation on `PATH` and dragged MinGW headers along with it, which clash with the MSVC ones. `env.cmd` tells CMake to ignore such prefixes through `CMAKE_IGNORE_PREFIX_PATH`; if your MinGW lives somewhere else, add it there.

**`Unable to locate 'atlbase.h'` or `'afxres.h'`.** The ATL or MFC component is missing, see the table above.

**`Permission denied (publickey)` when initializing the submodule.** The URL in `.gitmodules` is https, so no key is needed; the error means an old URL is left in `.git/config`. Remove it: `git config --unset submodule.vcpkg4aspia.url`.

## Editing the build scripts

`.cmd` files must have CRLF line endings, otherwise `cmd` parses them every other line and prints nonsense such as `|| was unexpected at this time`. `.gitattributes` sets this with the rule `*.cmd text eol=crlf`; when editing with another editor, make sure it does not rewrite them to LF.

## The router for Linux

Only the router is built: without Qt, codecs, audio or the desktop (the `ASPIA_BUILD_ROUTER_ONLY` option, the `linux-router` preset). The result is a `.deb` package with the program and a systemd service.

Build on **Ubuntu 24.04**, the oldest system the router has to run on. What is built there runs on newer systems too; what is built on a newer one will not start on 24.04. WSL with Ubuntu 24.04 will do.

```
sudo apt install build-essential cmake ninja-build git curl zip unzip tar pkg-config nasm \
    autoconf autoconf-archive automake libtool python3 bison flex

git submodule update --init
tools/build/build_router_linux.sh
```

The script builds, runs the tests and puts the package into `builds/linux-router/aspia-router-<version>-x86_64.deb`. The first build takes about half an hour (vcpkg builds OpenSSL, protobuf, ICU and the rest); later ones take minutes.

Under WSL, build in the Linux file system (`~/...`), not on the Windows drive (`/mnt/c/...`): the build is several times slower there.

### Installing on a server

```
sudo apt install ./aspia-router-<version>-x86_64.deb
sudo aspia_router --create-config
sudo systemctl enable --now aspia-router
```

`--create-config` creates the user `admin` with the password `admin`, the keys and the database. Change the password right away from the console: "Tools → Router Manage".

| What | Where |
|---|---|
| Settings and the private key | `/etc/aspia/router.json` (root only) |
| Public key | `/etc/aspia/router.pub` |
| Database: users, hosts, shared books | `/var/lib/aspia/router.db3` (root only) |
| Logs | the systemd journal: `journalctl -u aspia-router` |

The default port is TCP 8060; if the server has a firewall: `sudo ufw allow 8060/tcp`.
