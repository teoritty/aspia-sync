@echo off
rem
rem Aspia Project
rem Copyright (C) 2016-2024 Dmitry Chapyshev <dmitry@aspia.ru>
rem
rem Builds the configured tree. Pass a configuration as the first argument
rem (Release by default).
rem
setlocal
call "%~dp0env.cmd" || exit /b 1

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"

cd /d "%REPO%"
cmake --build "%BUILD_DIR%" --config %CONFIG%
exit /b %ERRORLEVEL%
