@echo off
rem Builds LinSCP for Windows with CMake.
rem
rem Usage:  windows\build.cmd [build-directory]
rem
rem Uses Visual Studio 2022 when present, otherwise MinGW Makefiles.
rem Requirements: CMake 3.16+, a C++17 compiler, and (at run time)
rem curl.exe, which ships with Windows 10 1803 and later.
setlocal
set "BUILD_DIR=%~1"
if "%BUILD_DIR%"=="" set "BUILD_DIR=build-windows"

where cmake >nul 2>nul
if errorlevel 1 (
    echo CMake was not found on PATH. Install CMake 3.16 or newer.
    exit /b 1
)

cmake -S . -B "%BUILD_DIR%" -A x64 2>nul
if errorlevel 1 (
    echo Visual Studio generator not available, trying MinGW Makefiles...
    cmake -S . -B "%BUILD_DIR%" -G "MinGW Makefiles"
    if errorlevel 1 exit /b 1
)

cmake --build "%BUILD_DIR%" --config Release
if errorlevel 1 exit /b 1

ctest --test-dir "%BUILD_DIR%" -C Release --output-on-failure
if errorlevel 1 exit /b 1

echo.
echo Built: %BUILD_DIR%\Release\linuxscp.exe (MSVC) or %BUILD_DIR%\linuxscp.exe (MinGW)
echo Try:   linuxscp.exe -version
endlocal
