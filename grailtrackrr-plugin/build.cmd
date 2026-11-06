@echo off
rem Double-click or run from a terminal. Arguments are passed to build.ps1,
rem for example: build.cmd -GameDir "C:\Games\Diablo II Resurrected"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
set EXITCODE=%ERRORLEVEL%
if "%~1"=="" pause
exit /b %EXITCODE%
