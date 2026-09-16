@echo off
setlocal
cd /d "%~dp0"

where cmake >nul 2>nul
if errorlevel 1 (
    echo CMake was not found.
    echo.
    echo Install Visual Studio 2022 Community with:
    echo   - Desktop development with C++
    echo   - Windows 10 or 11 SDK
    echo   - C++ CMake tools for Windows
    echo.
    pause
    exit /b 1
)

if not exist build mkdir build

cmake -S . -B build -A x64
if errorlevel 1 goto :fail

cmake --build build --config Release
if errorlevel 1 goto :fail

echo.
echo ============================================
echo Build complete.
echo EXE: build\Release\Ethereal.exe
echo ============================================
echo.
pause
exit /b 0

:fail
echo.
echo Build failed. See the messages above.
pause
exit /b 1
