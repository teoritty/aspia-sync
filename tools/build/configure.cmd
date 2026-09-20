@echo off
rem
rem Aspia Project
rem Copyright (C) 2016-2024 Dmitry Chapyshev <dmitry@aspia.ru>
rem
rem Configures the build and installs the dependencies through vcpkg. The first
rem run builds Qt5 from source and takes hours; later runs reuse the binary cache.
rem
setlocal
call "%~dp0env.cmd" || exit /b 1
cd /d "%REPO%"
cmake --preset ninja-multi-vcpkg-ci %IGNORE_PREFIXES%
exit /b %ERRORLEVEL%
