@echo off
rem Set the program icon. Same logic as ./set-icon.sh -- no bash needed,
rem so this works in cmd, PowerShell, and by double-clicking.
rem
rem   set-icon.bat logo.png    replace the icon with this image
rem   set-icon.bat             regenerate from the current logo.png
rem   set-icon.bat --help      full help
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

%PYC% "tools\icon.py" %*
if errorlevel 1 (
  echo.
  echo [FAILED] see the output above.
  pause
  exit /b 1
)
