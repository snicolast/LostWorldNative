@echo off
rem One-click install of Lost World Native. Installs what the build needs with
rem winget (Python 3 and the C++ tools of Build Tools for Visual Studio 2022, a few
rem GB), then runs build.bat. Running it again skips whatever is already installed.
rem Usage: double-click it with lostwsga.zip in this folder, drop lostwsga.zip on it,
rem or install.bat D:\roms\lostwsga.zip
setlocal
cd /d "%~dp0"
rem Started with a double click or a drop (cmd /c): keep the window open at the end
set "PAUSE_AT_END="
for %%a in (%cmdcmdline%) do if /i "%%~a"=="/c" set PAUSE_AT_END=1

echo Lost World Native - install
echo.

rem 1. The ROM, asked for first so nothing is installed for nothing
set "ZIP=%~1"
if "%ZIP%"=="" set "ZIP=%LW_ROM_ZIP%"
if "%ZIP%"=="" set "ZIP=%~dp0lostwsga.zip"
if exist data\crom.bin if exist data\sound_samples.bin goto rom_ok
:wait_rom
if exist "%ZIP%" goto rom_ok
echo The arcade ROM is not here ^("%ZIP%"^).
echo Copy your lostwsga.zip into this folder:
echo     %~dp0
echo then press a key. Or close this window.
pause >nul
set "ZIP=%~dp0lostwsga.zip"
goto wait_rom
:rom_ok

rem 2. Python 3.8 or later (the build scripts use only its standard library)
call :find_python && goto python_ok
call :need_winget || goto fail
echo Installing Python 3 for this user...
winget install -e --id Python.Python.3.12 --scope user --accept-package-agreements --accept-source-agreements --override "/passive InstallAllUsers=0 PrependPath=1 Include_test=0"
call :refresh_path
call :find_python && goto python_ok
rem Installed but not on PATH (an installer that ignored PrependPath): use it from where it went
for /d %%d in ("%LOCALAPPDATA%\Programs\Python\Python3*") do if exist "%%d\python.exe" set "PATH=%%d;%%d\Scripts;%PATH%"
call :find_python && goto python_ok
echo Could not install Python 3. Install it from https://www.python.org/downloads/
echo ^(tick "Add python.exe to PATH"^) and run install.bat again.
goto fail
:python_ok
echo Python: OK

rem 3. MSVC, CMake and a Windows SDK (the same check as build.bat)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VS_ADD=--add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.VC.CMake.Project --add Microsoft.VisualStudio.Component.Windows11SDK.22621"
call :find_vs && goto vs_ok
rem Visual Studio is there without the C++ tools: add them to it
set "VSDIR="
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
if "%VSDIR%"=="" goto vs_new
echo Adding the C++ tools to "%VSDIR%". Accept the Windows prompt; it takes a while.
"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\setup.exe" modify --installPath "%VSDIR%" %VS_ADD% --passive --norestart
goto vs_check
:vs_new
call :need_winget || goto fail
echo Installing the C++ tools of Build Tools for Visual Studio 2022 ^(a few GB^).
echo Accept the Windows prompt; it takes a while.
winget install -e --id Microsoft.VisualStudio.2022.BuildTools --accept-package-agreements --accept-source-agreements --override "--passive --wait --norestart %VS_ADD%"
:vs_check
call :find_vs && goto vs_ok
echo Could not install the C++ tools. Install Build Tools for Visual Studio 2022 from
echo https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022
echo with the "Desktop development with C++" workload, and run install.bat again.
goto fail
:vs_ok
echo C++ tools: OK
echo.

rem 4. Build (LW_NO_PAUSE: build.bat must not pause, this window does)
set LW_NO_PAUSE=1
rem build.bat only extracts the ROM when data\ is not complete
call build.bat "%ZIP%"
if errorlevel 1 goto fail
echo.
echo Installed. Play with LostWorld.exe; update later with update.bat.
if not defined PAUSE_AT_END exit /b 0
choice /c YN /m "Start the game now"
if not errorlevel 2 start "" "%~dp0LostWorld.exe"
exit /b 0

:fail
echo.
echo Install failed: see the messages above.
if defined PAUSE_AT_END pause
exit /b 1

:find_python
python -c "import sys; sys.exit(sys.version_info < (3, 8))" >nul 2>&1
exit /b %errorlevel%

:find_vs
set "VSDIR="
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 Microsoft.VisualStudio.Component.VC.CMake.Project -property installationPath`) do set "VSDIR=%%i"
if "%VSDIR%"=="" exit /b 1
exit /b 0

:need_winget
where winget >nul 2>&1 && exit /b 0
echo winget is missing. Install "App Installer" from the Microsoft Store:
echo     https://apps.microsoft.com/detail/9NBLGGH4NNS1
echo and run install.bat again, or install Python 3 and the Build Tools yourself ^(see README.md^).
exit /b 1

:refresh_path
rem winget changes PATH in the registry only: read it back into this window
for /f "usebackq delims=" %%p in (`powershell -NoProfile -Command "[Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [Environment]::GetEnvironmentVariable('Path','User')"`) do set "PATH=%%p"
exit /b 0
