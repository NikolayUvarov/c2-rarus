@echo off
setlocal enabledelayedexpansion

echo RarusScreenCapture Build Script
echo ==============================

REM Check for administrator privileges
net session >nul 2>&1
if %errorLevel% neq 0 (
    echo WARNING: This script is not running with administrator privileges.
    echo          Some operations may fail if Admin rights are required.
    echo.
    timeout /t 3 >nul
)

REM Create build directory if it doesn't exist
if not exist build (
    echo Creating build directory...
    mkdir build
)

REM Navigate to build directory
cd build

REM Run CMake to configure the project
echo Running CMake configuration...
cmake -A x64 ..

if %errorLevel% neq 0 (
    echo.
    echo ERROR: CMake configuration failed with error code %errorLevel%
    exit /b %errorLevel%
)

REM Build the project
echo.
echo Building solution in Release mode...
cmake --build . --config Release

if %errorLevel% neq 0 (
    echo.
    echo ERROR: Build failed with error code %errorLevel%
    exit /b %errorLevel%
)

echo.
echo Build completed successfully!

REM Create installation folder
set INSTALL_DIR=C:\rarus
if not exist "%INSTALL_DIR%" (
    echo.
    echo Creating installation directory: %INSTALL_DIR%
    mkdir "%INSTALL_DIR%"
)

REM Copy files to installation folder
echo.
echo Copying files to installation directory...
copy /Y "bin\RarusScreenCapture.exe" "%INSTALL_DIR%\"
copy /Y "bin\RarusServiceInstaller.exe" "%INSTALL_DIR%\"

echo.
echo Creating capture directory...
if not exist "C:\temp\captures" mkdir "C:\temp\captures"

echo.
echo ==============================
echo Installation complete!
echo Files installed to: %INSTALL_DIR%
echo.
echo To install the service, run:
echo %INSTALL_DIR%\RarusServiceInstaller.exe install
echo.
echo To start the service, run:
echo %INSTALL_DIR%\RarusServiceInstaller.exe start
echo.
echo To stop the service, run:
echo %INSTALL_DIR%\RarusServiceInstaller.exe stop
echo.
echo To uninstall the service, run:
echo %INSTALL_DIR%\RarusServiceInstaller.exe uninstall
echo ==============================

cd ..