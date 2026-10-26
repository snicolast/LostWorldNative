@echo off
rem One-click install of Lost World Native: installs what build.bat needs when it
rem is missing (Python 3 and Build Tools for Visual Studio 2022, with winget), then
rem builds the game from your own lostwsga.zip.
rem Usage: double-click it with lostwsga.zip next to it, or drop lostwsga.zip on it.
setlocal
cd /d "%~dp0"
echo Lost World Native setup
echo.

set "ZIP=%~1"
if "%ZIP%"=="" if exist data\crom.bin goto rom_ok
if "%ZIP%"=="" set "ZIP=%LW_ROM_ZIP%"
if "%ZIP%"=="" set "ZIP=%~dp0lostwsga.zip"
if exist "%ZIP%" goto rom_ok
echo lostwsga.zip not found next to setup.bat.
echo Copy your lostwsga.zip ^(Japan, Rev. A^) into this folder and run setup.bat again,
echo or drop the zip on setup.bat. The ROM is not included with this project.
goto fail
:rom_ok

rem Python 3: the "python" of the Microsoft Store alias does not count
python -c "import sys; sys.exit(sys.version_info[0] != 3)" >nul 2>&1 && goto python_ok
set "PYDIR=%LOCALAPPDATA%\Programs\Python\Python312"
if exist "%PYDIR%\python.exe" goto python_path
call :need_winget || goto fail
echo Installing Python 3.12 (winget)...
winget install -e --id Python.Python.3.12 --scope user --accept-package-agreements --accept-source-agreements || goto python_fail
if not exist "%PYDIR%\python.exe" goto python_fail
:python_path
set "PATH=%PYDIR%;%PATH%"
:python_ok

rem Visual Studio 2022 with the C++ tools and CMake, the same check as build.bat
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
call :find_vs
if not "%VSDIR%"=="" goto vs_ok
call :need_winget || goto fail
echo Installing Build Tools for Visual Studio 2022 with the C++ tools (winget).
echo It is a large download (about 3 GB^) and Windows will ask for permission.
winget install -e --id Microsoft.VisualStudio.2022.BuildTools --accept-package-agreements --accept-source-agreements --override "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.VC.CMake.Project --add Microsoft.VisualStudio.Component.Windows11SDK.22621"
call :find_vs
if "%VSDIR%"=="" goto vs_fail
:vs_ok

echo.
echo Building (a few minutes the first time)...
call build.bat "%ZIP%" || goto fail
echo.
echo Installed. Play with LostWorld.exe in this folder.
pause
exit /b 0

:find_vs
set "VSDIR="
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 Microsoft.VisualStudio.Component.VC.CMake.Project -property installationPath`) do set "VSDIR=%%i"
exit /b 0

:need_winget
where winget >nul 2>&1 && exit /b 0
echo winget is not available on this PC ^(it comes with "App Installer" from the Microsoft Store^).
echo Install by hand instead: Python 3 from https://www.python.org/downloads/ and
echo Build Tools for Visual Studio 2022 with "Desktop development with C++" from
echo https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022
exit /b 1

:python_fail
echo Could not install Python 3. Install it from https://www.python.org/downloads/ and run setup.bat again.
goto fail

:vs_fail
echo Build Tools for Visual Studio 2022 with the C++ tools and CMake were not found after the install.
echo Open the Visual Studio Installer, add "Desktop development with C++" and run setup.bat again.
goto fail

:fail
echo.
echo Setup did not finish.
pause
exit /b 1
