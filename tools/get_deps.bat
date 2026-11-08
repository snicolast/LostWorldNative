@echo off
rem Downloads the libraries CMake builds with (SDL3 and miniz, listed in
rem third_party\deps.txt) into third_party\downloads, with retries, and checks
rem their SHA256. CMake then uses those copies instead of downloading them itself.
rem build.bat calls this. If a download keeps failing (proxy, antivirus, flaky
rem connection), save the file with a browser into third_party\downloads\ with the
rem same name and run build.bat again.
setlocal
cd /d "%~dp0.."
set "DL=third_party\downloads"
if not exist "%DL%" mkdir "%DL%"
set "FAILED="
for /f "eol=# tokens=1-4" %%a in (third_party\deps.txt) do call :get %%a %%b %%c %%d
if defined FAILED goto fail
exit /b 0

rem :get name file url sha256
:get
set "F=%DL%\%~2"
if exist "%F%" (
    call :check "%F%" %4 && exit /b 0
    echo %~2 in %DL% is incomplete or damaged: downloading it again
    del "%F%"
)
echo Downloading %~1 ^(%~2^)
curl -fL --retry 5 --retry-delay 3 --connect-timeout 30 --progress-bar -o "%F%.part" "%~3"
if errorlevel 1 (
    del "%F%.part" 2>nul
    echo Could not download %~3
    set FAILED=1
    exit /b 0
)
call :check "%F%.part" %4 || (
    del "%F%.part" 2>nul
    echo %~2 was downloaded but its SHA256 is not the expected one ^(a proxy or
    echo antivirus page instead of the file?^)
    set FAILED=1
    exit /b 0
)
move /y "%F%.part" "%F%" >nul
exit /b 0

rem :check file sha256
:check
certutil -hashfile %1 SHA256 2>nul | findstr /i /c:"%~2" >nul
exit /b %errorlevel%

:fail
echo.
echo Some libraries could not be downloaded ^(see above^). Download them with your
echo browser, put them in %DL%\ keeping the file name, and run build.bat again:
for /f "eol=# tokens=1-4" %%a in (third_party\deps.txt) do if not exist "%DL%\%%b" echo   %%c
exit /b 1
