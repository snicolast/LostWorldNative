@echo off
rem Sets VSDIR to a Visual Studio (2022 or later, Build Tools or any edition) with the
rem x64 C++ tools and its CMake, and returns 1 if there is none. install.bat and
rem build.bat call it.
rem vswhere first, with -all: without it, vswhere leaves out an instance that waits for
rem a Windows restart (common right after installing Build Tools). Then the default
rem folders, for a vswhere that is missing or does not list it.
set "VSDIR="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -all -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 Microsoft.VisualStudio.Component.VC.CMake.Project -property installationPath`) do call :check "%%i"
if defined VSDIR exit /b 0
for /d %%v in ("%ProgramFiles%\Microsoft Visual Studio\*" "%ProgramFiles(x86)%\Microsoft Visual Studio\*") do for /d %%e in ("%%v\*") do call :check "%%e"
if defined VSDIR exit /b 0
exit /b 1

:check
if exist "%~1\VC\Auxiliary\Build\vcvars64.bat" if exist "%~1\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" set "VSDIR=%~1"
exit /b 0
