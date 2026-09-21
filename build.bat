@echo off
rem Builds bin\DopplersTTFtoC.exe - a single static exe, no runtime DLLs needed.
setlocal
cd /d "%~dp0"
if "%TOOLCHAIN%"=="" set "TOOLCHAIN=C:\workenv\w64devkit\bin"
set "PATH=%TOOLCHAIN%;%PATH%"

if not exist build mkdir build
if not exist bin mkdir bin

if not exist res\app.ico (
    g++ -O2 -std=c++20 tools\make_icon.cpp -o build\make_icon.exe || goto :fail
    build\make_icon.exe res\app.ico || goto :fail
)

windres -O coff -I res res\app.rc -o build\app_res.o || goto :fail

g++ -std=c++20 -O2 -Wall -Wextra -Wno-missing-field-initializers -municode -mwindows -static -s ^
    src\main.cpp src\font_engine.cpp src\codegen.cpp build\app_res.o ^
    -o bin\DopplersTTFtoC.exe ^
    -lcomctl32 -lcomdlg32 -lshell32 -lole32 -luuid -lgdi32 -luser32 || goto :fail

echo.
echo Built bin\DopplersTTFtoC.exe
exit /b 0

:fail
echo.
echo BUILD FAILED
exit /b 1
