# Windows Screen Capture Service

This project provides a robust Windows service that captures screen content at regular intervals and makes it available through named pipes. It's designed to work even on login screens, UAC prompts, and in user sessions.

## Features

- Captures screen images every second
- Works on Windows login screens and in user sessions
- Captures UAC question windows
- Handles multiple monitors
- Saves screen captures to timestamped files
- Makes captures available through named pipes
- Supports service control through Windows Service Manager
- Compatible with Windows 10/11

## Implementation Details

The solution uses the DXGI Desktop Duplication API, which is the modern Microsoft-recommended approach for screen capture on Windows. This API provides direct access to the desktop compositor, making it possible to capture protected screens like the login screen and UAC prompts.

The service uses the following technologies:
- C++ for performance and direct access to Windows APIs
- DXGI for screen capture
- D3D11 for hardware-accelerated processing
- Windows Imaging Component (WIC) for JPEG compression
- Windows Service API for system service integration
- Named pipes for inter-process communication

## Building the Project

### Prerequisites

- Windows 10 or 11
- Visual Studio 2019 or newer with C++ development tools
- CMake 3.10 or newer

### Build Steps

1. Clone the repository
2. Open a Developer Command Prompt for Visual Studio
3. Navigate to the project directory
4. Create a build directory and navigate to it:
   ```
   mkdir build
   cd build
   ```
5. Generate the build files with CMake:
   ```
   cmake ..
   ```
6. Build the project:
   ```
   cmake --build . --config Release
   ```
7. The executables will be placed in the `build\bin` directory

## Installation and Usage

### Installing the Service

```
RarusServiceInstaller.exe install
```

### Starting the Service

```
RarusServiceInstaller.exe start
```

### Stopping the Service

```
RarusServiceInstaller.exe stop
```

### Uninstalling the Service

```
RarusServiceInstaller.exe uninstall
```

## Using the Captured Images

### File Output

The service saves screen captures to the following location:
```
C:/temp/captures/YYYY-MM-DD-hh-mm-ss.jpg
```

### Named Pipes

The captured images are available through named pipes with the following naming convention:
- `\\.\pipe\rarus-scr\0` - All screens in one image
- `\\.\pipe\rarus-scr\1` - Only first screen
- `\\.\pipe\rarus-scr\2` - Only second screen (if available)
- And so on for additional monitors

## Troubleshooting

- **Service fails to start**: Ensure you have administrator privileges and that the Windows Desktop Manager service is running.
- **Blank or black screenshots**: This may happen if there are GPU driver issues or if hardware acceleration is disabled.
- **Cannot capture UAC screens**: Make sure the service is running with SYSTEM privileges (default when installed with the installer).
- **Named pipes not accessible**: Check that the client application has sufficient permissions to access the named pipes.

## License

This project is provided as-is, without any warranty or support. Use at your own risk.
