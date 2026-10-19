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
where python >nul 2>&1 && set "PY=python"
if not defined PY (
  where py >nul 2>&1 && set "PY=py -3"
)
if not defined PY (
  echo [ERROR] Python 3 not found. Install it or add python.exe to PATH.
  pause
  exit /b 1
)

%PY% "tools\icon.py" %*
if errorlevel 1 (
  echo.
  echo [FAILED] see the output above.
  pause
  exit /b 1
)
