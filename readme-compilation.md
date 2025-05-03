# Windows Screen Capture Service - Compilation Guide

This guide explains how to compile the Windows Screen Capture Service solution, which consists of the service executable and an installer utility.

## Prerequisites

To build the solution, you need:

1. **Windows 10/11 64-bit**
2. **Visual Studio 2019 or later** with the following components:
   - Desktop development with C++
   - Windows 10/11 SDK
3. **CMake 3.10 or newer**

## Project Structure

The solution consists of the following files:

- `screen_capture_service.cpp` - The main service implementation
- `service_installer.cpp` - A utility to install, uninstall, start, and stop the service
- `CMakeLists.txt` - The CMake build script

## Building the Solution

### Using Visual Studio Developer Command Prompt

1. Open a **Developer Command Prompt for Visual Studio**
2. Navigate to the directory containing the source files
3. Create a build directory and navigate to it:

   ```cmd
   mkdir build
   cd build
   ```

4. Generate the Visual Studio solution files with CMake:

   ```cmd
   cmake ..
   ```

5. Build the solution:

   ```cmd
   cmake --build . --config Release
   ```

6. The compiled executables will be placed in the `build\bin` directory:
   - `RarusScreenCapture.exe` - The service executable
   - `RarusServiceInstaller.exe` - The installer utility

### Using Visual Studio IDE

1. Launch **Visual Studio**
2. Select **Open a local folder** and navigate to the directory containing the source files
3. Once the folder is loaded, right-click on `CMakeLists.txt` in Solution Explorer and select **Configure CMake**
4. After configuration completes, select **Build > Build All** from the menu
5. The compiled executables will be placed in the output directory specified in the CMake configuration

## Common Compilation Errors and Solutions

### Unicode/ANSI String Mismatch

If you encounter errors related to string type mismatches (like converting from `LPWSTR` to `LPSTR`), ensure that:
- The Unicode character set is defined: `#define UNICODE` and `#define _UNICODE`
- You're using the Unicode versions of Windows API functions (those ending with 'W' instead of 'A')

### Missing Libraries

If you encounter errors about missing libraries:
- Ensure you have the Windows 10/11 SDK installed
- Verify that the necessary libraries are linked in CMakeLists.txt (d3d11.lib, dxgi.lib, windowscodecs.lib)

### Permission Issues

When creating the capture directory:
- Run Visual Studio or the command prompt as Administrator if you encounter permission issues
- Alternatively, modify the code to use a different directory for capturing images

## Installation and Usage

After successful compilation:

1. Open a Command Prompt as Administrator
2. Navigate to the build\bin directory
3. Install the service:

   ```cmd
   RarusServiceInstaller.exe install
   ```

4. Start the service:

   ```cmd
   RarusServiceInstaller.exe start
   ```

The service will now run in the background, capturing screenshots every second and saving them to `C:\temp\captures\` with timestamped filenames.

