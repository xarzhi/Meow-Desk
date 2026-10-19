@echo off
rem Build meow-desk. Same as ./build.sh -- drives cmake+ninja directly, so no
rem vcvars64 (and no reg.exe) is needed, and the output lands in build\ like the
rem rest of the scripts expect.
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

%PY% "tools\build.py" %*
if errorlevel 1 (
  echo.
  echo [FAILED] see the output above.
  pause
  exit /b 1
)
