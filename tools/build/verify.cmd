@echo off
rem
rem Aspia Project
rem Copyright (C) 2016-2024 Dmitry Chapyshev <dmitry@aspia.ru>
rem
rem Configure, build and run the tests. This is what a change has to pass before
rem it is committed.
rem
setlocal
call "%~dp0env.cmd" || exit /b 1

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"

cd /d "%REPO%"

echo ============================== CONFIGURE ==============================
cmake --preset ninja-multi-vcpkg-ci %IGNORE_PREFIXES% || exit /b 1

echo ============================== BUILD ==================================
cmake --build "%BUILD_DIR%" --config %CONFIG% || exit /b 1

echo ============================== TESTS ==================================
"%BUILD_DIR%\%CONFIG%\aspia_base_tests.exe"
exit /b %ERRORLEVEL%
