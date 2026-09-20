@echo off
rem
rem Aspia Project
rem Copyright (C) 2016-2024 Dmitry Chapyshev <dmitry@aspia.ru>
rem
rem Shared build environment. Called by configure.cmd, build.cmd and verify.cmd;
rem not meant to be run on its own.
rem
rem Note on style: this file uses goto instead of multi-line "if (...)" blocks.
rem The paths it works with contain "(x86)", and a variable holding parentheses
rem breaks such a block when cmd substitutes it while parsing.
rem

rem Repository root: two levels up from this file.
for %%I in ("%~dp0..\..") do set "REPO=%%~fI"

rem Visual Studio is located through vswhere instead of a hardcoded path: the
rem edition and the version differ between machines.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :no_vswhere

set "VSPATH="
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%I"
if not defined VSPATH goto :no_vs
if not exist "%VSPATH%\VC\Tools\MSVC" goto :no_toolset

rem The ATL and MFC components are required: the wtl port of vcpkg.json pulls in
rem atl and atlmfc, and those need atlbase.h and afxres.h from the toolset. They
rem are not part of the base C++ workload, so the check is done here rather than
rem half an hour into installing dependencies.
rem The toolset version is part of the path, so the directories are walked: dir
rem only expands a wildcard in the file name, not in the middle of a path.
set "HAVE_ATLMFC="
for /d %%I in ("%VSPATH%\VC\Tools\MSVC\*") do if exist "%%I\atlmfc\include\afxres.h" set "HAVE_ATLMFC=1"
if not defined HAVE_ATLMFC goto :no_atlmfc

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 goto :no_vcvars

where cmake >nul 2>&1
if errorlevel 1 goto :no_cmake

set "VCPKG_ROOT=%REPO%\vcpkg4aspia"
if not exist "%VCPKG_ROOT%\.git" goto :no_vcpkg

set "VCPKG_TRIPLET=x64-windows-static"
set "VCPKG_DEFAULT_HOST_TRIPLET=x64-windows-static"
set "VCPKG_INSTALLED_DIR=%REPO%\builds\ninja-multi-vcpkg-ci\vcpkg_installed"
set "VCPKG_BINARY_SOURCES=default,readwrite"
set "VCPKG_DOWNLOADS=%REPO%\builds\vcpkg_downloads"
set "BUILD_DIR=%REPO%\builds\ninja-multi-vcpkg-ci"

rem A MinGW installation on PATH (msys2, Strawberry Perl, Git for Windows) makes
rem CMake find packages there before it looks at vcpkg. zstd found that way also
rem drags C:\msys64\mingw64\include into the compilation, where its MinGW system
rem headers clash with the ones of MSVC and produce hundreds of errors inside
rem vcruntime.h. Telling CMake to ignore those prefixes is more reliable than
rem editing PATH, which differs from machine to machine.
set "IGNORE_PREFIXES=-DCMAKE_IGNORE_PREFIX_PATH=C:/msys64/mingw64;C:/msys64;C:/Strawberry/c"

exit /b 0

:no_vswhere
echo ERROR: vswhere.exe not found.
echo Install Visual Studio 2022 Build Tools with the C++ workload.
exit /b 1

:no_vs
echo ERROR: no Visual Studio installation with the C++ toolset was found.
exit /b 1

:no_toolset
echo ERROR: no MSVC toolset under "%VSPATH%".
exit /b 1

:no_atlmfc
echo ERROR: the ATL/MFC headers are missing from the C++ toolset.
echo Add the VC.ATL and VC.ATLMFC components, see tools/build/README.md.
exit /b 1

:no_vcvars
echo ERROR: vcvars64.bat failed.
exit /b 1

:no_cmake
echo ERROR: cmake is not on PATH. CMake 3.21 or newer is required.
exit /b 1

:no_vcpkg
echo ERROR: the vcpkg4aspia submodule is not checked out.
echo Run: git submodule update --init
exit /b 1
