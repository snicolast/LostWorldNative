@echo off
rem Downloads the NVIDIA DLSS SDK files this project uses (headers, import library,
rem runtime DLL and license) from NVIDIA's official GitHub repository into
rem third_party\ngx. They are not kept in this repository: the SDK license does not
rem allow redistributing the SDK itself. build.bat calls this when they are missing.
setlocal
cd /d "%~dp0.."
set TAG=v310.9.16
set URL=https://raw.githubusercontent.com/NVIDIA/DLSS/%TAG%
set DST=third_party\ngx
echo Downloading the NVIDIA DLSS SDK %TAG% into %DST% (license: %DST%\LICENSE.txt)
for %%d in (include lib bin) do if not exist %DST%\%%d mkdir %DST%\%%d
for %%h in (nvsdk_ngx.h nvsdk_ngx_defs.h nvsdk_ngx_defs_vk.h nvsdk_ngx_helpers.h nvsdk_ngx_helpers_vk.h nvsdk_ngx_loader.h nvsdk_ngx_params.h nvsdk_ngx_vk.h) do (
    call :get include\%%h %URL%/include/%%h || goto :fail
)
call :get lib\nvsdk_ngx_d.lib %URL%/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib || goto :fail
echo nvngx_dlss.dll is about 60 MB:
call :get bin\nvngx_dlss.dll %URL%/lib/Windows_x86_64/rel/nvngx_dlss.dll progress || goto :fail
call :get LICENSE.txt %URL%/LICENSE.txt || goto :fail
exit /b 0

rem :get <file under %DST%> <url> [progress]: downloads to a .part file first, so an
rem interrupted download never leaves a truncated file behind; "progress" shows a bar
:get
set "CURL_OUT=-sS"
if "%~3"=="progress" set "CURL_OUT=--progress-bar"
curl -fL --retry 3 --retry-delay 3 --connect-timeout 30 %CURL_OUT% -o "%DST%\%~1.part" "%~2" || (del /q "%DST%\%~1.part" 2>nul & exit /b 1)
move /y "%DST%\%~1.part" "%DST%\%~1" >nul || exit /b 1
exit /b 0

:fail
echo Could not download the DLSS SDK from %URL%
exit /b 1
