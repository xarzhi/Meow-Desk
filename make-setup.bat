@echo off
rem Package a distributable installer. Same logic as ./make-setup.sh -- no bash
rem needed, so this works in cmd, PowerShell, and by double-clicking.
rem
rem   make-setup.bat              build -> dist\package\ -> zip
rem   make-setup.bat --no-build   package the exe that is already in build\
rem   make-setup.bat --help       full help
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

%PYC% "tools\package.py" %*
if errorlevel 1 (
  echo.
  echo [FAILED] see the output above.
  pause
  exit /b 1
)
