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
where python >nul 2>&1 && set "PY=python"
if not defined PY (
  where py >nul 2>&1 && set "PY=py -3"
)
if not defined PY (
  echo [ERROR] Python 3 not found. Install it or add python.exe to PATH.
  pause
  exit /b 1
)

%PY% "tools\package.py" %*
if errorlevel 1 (
  echo.
  echo [FAILED] see the output above.
  pause
  exit /b 1
)
