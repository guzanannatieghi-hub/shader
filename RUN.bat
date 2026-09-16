@echo off
cd /d "%~dp0"
if exist "build\Release\Ethereal.exe" (
    start "" "build\Release\Ethereal.exe"
) else (
    echo Ethereal.exe has not been built yet.
    echo Run BUILD.bat first.
    pause
)
