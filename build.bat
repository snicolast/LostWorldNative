@echo off
rem Builds Lost World Native: extracts the ROM, recompiles it and builds with MSVC.
rem Usage: build.bat [path to lostwsga.zip]   (or set LW_ROM_ZIP, or put it next to
rem this file; only needed the first time)
setlocal
cd /d "%~dp0"
rem Started with a double click (cmd /c): keep the window open at the end so the
rem message can be read
set "PAUSE_AT_END="
for %%a in (%cmdcmdline%) do if /i "%%~a"=="/c" set PAUSE_AT_END=1
set ZIP=%~1
if "%ZIP%"=="" set ZIP=%LW_ROM_ZIP%
if "%ZIP%"=="" set ZIP=%~dp0lostwsga.zip

if exist data\crom.bin goto rom_ok
if not exist "%ZIP%" goto no_rom
echo [1/5] Extracting the ROM
python tools\romtool.py "%ZIP%" data || goto fail
:rom_ok
echo [2/5] Recompiling the PowerPC code
python tools\recomp.py data\crom.bin build\gen || goto fail
rem NVIDIA DLSS SDK (not in the repository): fetched from NVIDIA's GitHub the first
rem time. Needed to build on any GPU; DLAA itself only runs on NVIDIA RTX
if not exist third_party\ngx\bin\nvngx_dlss.dll (
    echo [3/5] Downloading the NVIDIA DLSS SDK ^(needed to build on any GPU; DLAA is optional^)
    call tools\get_dlss.bat || goto fail
)

rem Visual Studio 2022 (Build Tools or any edition) with the C++ tools and its
rem CMake, found with vswhere; the Build Tools default path if vswhere is missing
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSDIR="
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 Microsoft.VisualStudio.Component.VC.CMake.Project -property installationPath`) do set "VSDIR=%%i"
if "%VSDIR%"=="" set "VSDIR=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools"
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" goto no_vs
echo [4/5] Setting up Visual Studio ^("%VSDIR%"^)
rem Its output is hidden unless it fails (a missing Windows SDK, for example)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >"%TEMP%\lw_vcvars.log" 2>&1 || (
    type "%TEMP%\lw_vcvars.log"
    echo Visual Studio setup failed. Check that the "Windows 11 SDK" component is installed.
    goto fail
)
set "CMAKE=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "CTEST=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
if not exist "%CMAKE%" goto no_vs
echo [5/5] Building
"%CMAKE%" -S . -B build\cmake -G Ninja -DCMAKE_BUILD_TYPE=Release || goto fail
"%CMAKE%" --build build\cmake --target LostWorld || goto fail
copy /y build\cmake\LostWorld.exe LostWorld.exe >nul
rem NVIDIA DLSS runtime (official signed DLL) for --aa dlaa
if exist third_party\ngx\bin\nvngx_dlss.dll copy /y third_party\ngx\bin\nvngx_dlss.dll nvngx_dlss.dll >nul
rem The tests after the game: one that does not build or pass (a new compiler's
rem bug, say) is reported but does not take LostWorld.exe away
set "TESTS_OK=1"
"%CMAKE%" --build build\cmake || set "TESTS_OK="
if defined TESTS_OK "%CTEST%" --test-dir build\cmake --output-on-failure || set "TESTS_OK="
echo.
echo Done: LostWorld.exe
if not defined TESTS_OK echo Warning: the tests did not build or did not pass ^(see above^). The game is built and can be played.
if defined PAUSE_AT_END pause
exit /b 0

:fail
echo.
echo Build failed: see the message above.
if defined PAUSE_AT_END pause
exit /b 1

:no_rom
echo ROM not found ^("%ZIP%"^): pass the path to lostwsga.zip as an argument ^(build.bat D:\roms\lostwsga.zip^),
echo in the LW_ROM_ZIP environment variable, or put it next to build.bat. Only needed the first time: it is extracted into data\.
goto fail

:no_vs
echo Visual Studio 2022 with the x64 C++ tools and CMake not found ^(looked in "%VSDIR%"^).
echo Install Build Tools for Visual Studio 2022 with the "Desktop development with C++" workload.
goto fail
