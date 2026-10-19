@echo off
rem Build meow-desk. Same as ./build.sh -- drives cmake+ninja directly, so no
rem vcvars64 (and no reg.exe) is needed, and the output lands in build\ like the
rem rest of the scripts expect.
setlocal
cd /d "%~dp0"

set "PY="
rem Python is only the driver here. See tools\find-python.bat for why a plain
rem "where python" is not enough on this machine (Store redirector stub).
call "%~dp0tools\find-python.bat"
if not defined PYC (
  echo [ERROR] Python 3 not found.
  echo         Install Python 3 from https://www.python.org/downloads/
  echo         and tick "Add python.exe to PATH" during setup.
  pause
  exit /b 1
)

%PYC% "tools\build.py" %*
if errorlevel 1 (
  echo.
  echo [FAILED] see the output above.
  pause
  exit /b 1
)
