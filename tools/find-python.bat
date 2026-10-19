@echo off
rem ---------------------------------------------------------------------------
rem Locate a REAL Python 3 and put it in PYC (already quoted, ready to prefix a
rem command: %PYC% tools\package.py).
rem
rem Why this file exists: "where python" is not enough on this machine. The only
rem python on PATH can be %LOCALAPPDATA%\Microsoft\WindowsApps\python.exe, which
rem is the Microsoft Store redirector (AppInstallerPythonRedirector.exe) -- it
rem opens the Store and runs nothing. Double-clicking a .bat then looks like
rem "nothing happened". So every candidate is actually executed once and only
rem accepted if it really reports Python 3.
rem
rem Search order:
rem   1. python / python3 on PATH (store stubs rejected)
rem   2. official installer default: %LOCALAPPDATA%\Programs\Python\Python3*
rem   3. C:\Python3xx, D:\Python3xx, Program Files
rem   4. the py launcher (py -3)
rem   5. the interpreter that ships with WorkBuddy (present on this machine)
rem ---------------------------------------------------------------------------
setlocal EnableDelayedExpansion
set "PYC="

call :try python
call :try python3

for /d %%D in ("%LOCALAPPDATA%\Programs\Python\Python3*") do call :try "%%~fD\python.exe"
for %%D in ("C:\Python313" "C:\Python312" "C:\Python311" "C:\Python310" "D:\Python313" "D:\Python312" "D:\Python311" "D:\Python310" "C:\Program Files\Python313" "C:\Program Files\Python312") do call :try "%%~D\python.exe"
for /d %%D in ("%USERPROFILE%\.workbuddy\binaries\python\versions\*") do call :try "%%~fD\python.exe"

if not defined PYC (
    where py >nul 2>&1 && set "PYC=py -3"
)

endlocal & set "PYC=%PYC%"
goto :eof

rem -- %1 = candidate (bare name or full path, quotes ok). Accepted only if it
rem    runs and reports major version 3.
:try
if defined PYC goto :eof
set "CAND=%~1"
if "%CAND%"=="" goto :eof
echo %CAND% | find /i "WindowsApps" >nul
if not errorlevel 1 goto :eof
"%CAND%" -c "import sys;raise SystemExit(0 if sys.version_info[0]==3 else 1)" >nul 2>&1
if errorlevel 1 goto :eof
set "PYC="%CAND%""
goto :eof
