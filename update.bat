@echo off
rem Updates Lost World Native to the latest version on GitHub and rebuilds it
rem (tools\update.ps1). No git needed. It keeps the ROM, save\, your own textures
rem and every other file that is not part of the repository.
rem Usage: update.bat [-Check] [-Force] [-NoBuild]
setlocal EnableDelayedExpansion
rem Started with a double click (cmd /c): keep the window open at the end
set "PAUSE_AT_END="
for %%a in (%cmdcmdline%) do if /i "%%~a"=="/c" set PAUSE_AT_END=1
rem The rest is one block, read before it runs: the update may replace this file
(
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\update.ps1" %*
    set "RC=!errorlevel!"
    if defined PAUSE_AT_END pause
    exit /b !RC!
)
