@echo off
echo Rarus Screen Capture - Console Mode Test
echo =====================================
echo This script will run the screen capture service in console mode for testing.
echo Any errors will be displayed in the console and logged to file.
echo.
echo WARNING: Make sure the service is stopped before running this test!
echo.
pause

REM Clean up old log file to start fresh
if exist "C:\temp\captures\service_log.txt" (
    echo Cleaning up old log file...
    del "C:\temp\captures\service_log.txt"
)

REM Create capture directory if it doesn't exist
if not exist "C:\temp\captures" (
    echo Creating capture directory...
    mkdir "C:\temp\captures"
)

echo.
echo Starting screen capture service in console mode...
echo Press any key to stop the test.
echo.

REM Run the service in console mode
"C:\rarus\RarusScreenCapture.exe"

echo.
echo Test completed.
echo Check C:\temp\captures for captured images.
echo Check C:\temp\captures\service_log.txt for detailed logs.
echo.
pause
