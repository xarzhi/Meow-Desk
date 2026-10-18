@echo off
rem Configure MSVC environment and build meow-desk-core
setlocal

set "VS_ROOT=C:\Program Files\Microsoft Visual Studio\18\Community"
call "%VS_ROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

cd /d "%~dp0"

if not exist build mkdir build
cd build

cmake -G "Visual Studio 18 2026" -A x64 ..
if errorlevel 1 exit /b 1

cmake --build . --config Release
if errorlevel 1 exit /b 1

echo.
echo Build done. Output: build\Release\meow-desk.exe
endlocal
