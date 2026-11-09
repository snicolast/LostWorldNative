@echo off
rem Sets BUILD_PY to the command of a Python 3.10 or later (py -3, python, or a quoted
rem full path) and returns 0, or returns 1 when there is none. The build scripts need
rem 3.10 ("int | None" annotations; 3.8 and 3.9 stop with "unsupported operand
rem type(s) for |", Star Wars Trilogy issue #51). install.bat and build.bat call it, and build.bat runs
rem every script with %BUILD_PY%.
rem Every candidate is run and checked: "python" in PATH can be an older Python or the
rem Microsoft Store alias (which only points to the Store). In order: the py launcher's
rem newest Python 3, python in PATH, then the python.org installs (per user, which
rem install.bat makes, and for all users), whether or not they are in PATH.
set "BUILD_PY="
call :try py -3
if not defined BUILD_PY call :try python
if not defined BUILD_PY for /d %%d in ("%LOCALAPPDATA%\Programs\Python\Python3*" "%ProgramFiles%\Python3*") do if not defined BUILD_PY if exist "%%~d\python.exe" call :try "%%~d\python.exe"
if not defined BUILD_PY exit /b 1
rem Which one, for the log
%BUILD_PY% -c "import sys; print('Python ' + sys.version.split()[0] + ': ' + sys.executable)"
exit /b 0

:try
%* -c "import sys; sys.exit(sys.version_info < (3, 10))" >nul 2>&1 && set "BUILD_PY=%*"
exit /b 0
