@echo off
rem Sets VSDIR to a Visual Studio (2022 or later, Build Tools or any edition) with the
rem x64 C++ tools and its CMake, and WINSDK to a Windows SDK with the x64 headers and
rem libraries. Returns 0 when both are there, 1 when there is no such Visual Studio
rem and 2 when there is one (VSDIR set) but no Windows SDK. install.bat and build.bat
rem call it.
rem vswhere first, with -all: without it, vswhere leaves out an instance that waits for
rem a Windows restart (common right after installing Build Tools). Then the default
rem folders, for a vswhere that is missing or does not list it.
set "VSDIR="
set "WINSDK="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -all -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 Microsoft.VisualStudio.Component.VC.CMake.Project -property installationPath`) do call :check "%%i"
if not defined VSDIR for /d %%v in ("%ProgramFiles%\Microsoft Visual Studio\*" "%ProgramFiles(x86)%\Microsoft Visual Studio\*") do for /d %%e in ("%%v\*") do call :check "%%e"
if not defined VSDIR exit /b 1
rem The Windows SDK is a separate component in the Visual Studio Installer: the MSVC
rem and CMake components do not bring it, and without it the build stops in CMake.
rem Its folder is in the registry (as vcvars64.bat reads it), the default one
rem otherwise.
set "KITS=%ProgramFiles(x86)%\Windows Kits\10"
for %%k in ("HKLM\SOFTWARE\Microsoft" "HKLM\SOFTWARE\Wow6432Node\Microsoft") do for /f "tokens=2*" %%a in ('reg query "%%~k\Windows Kits\Installed Roots" /v KitsRoot10 2^>nul') do if "%%a"=="REG_SZ" set "KITS=%%b"
if "%KITS:~-1%"=="\" set "KITS=%KITS:~0,-1%"
for /d %%s in ("%KITS%\Include\10.*") do call :check_sdk "%%~nxs"
if defined WINSDK exit /b 0
exit /b 2

:check
if exist "%~1\VC\Auxiliary\Build\vcvars64.bat" if exist "%~1\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" set "VSDIR=%~1"
exit /b 0

:check_sdk
if exist "%KITS%\Include\%~1\um\windows.h" if exist "%KITS%\Include\%~1\ucrt\stdio.h" if exist "%KITS%\Lib\%~1\um\x64\kernel32.lib" if exist "%KITS%\Lib\%~1\ucrt\x64\ucrt.lib" set "WINSDK=%~1"
exit /b 0
