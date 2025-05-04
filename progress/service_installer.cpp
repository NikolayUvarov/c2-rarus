#include <windows.h>
#include <tchar.h>
#include <stdio.h>
#include <strsafe.h>

// Define UNICODE if not already defined
#ifndef UNICODE
#define UNICODE
#endif

// Define _UNICODE if not already defined
#ifndef _UNICODE
#define _UNICODE
#endif

#define SERVICE_NAME L"RarusScreenCapture"
#define SERVICE_DISPLAY_NAME L"Rarus Screen Capture Service"
#define SERVICE_DESCRIPTION L"Captures screen images periodically for remote viewing"
#define SERVICE_START_TYPE SERVICE_AUTO_START
#define SERVICE_DEPENDENCIES L""
#define SERVICE_ACCOUNT NULL // NULL = LocalSystem
#define SERVICE_PASSWORD NULL

// Error logging function
void LogError(const wchar_t* message, DWORD error = GetLastError()) {
    HANDLE hEventLog = RegisterEventSourceW(NULL, SERVICE_NAME);
    if (hEventLog) {
        wchar_t errorMsg[256];
        swprintf_s(errorMsg, L"%s (Error code: %lu)", message, error);
        const wchar_t* strings[1] = { errorMsg };
        ReportEventW(hEventLog, EVENTLOG_ERROR_TYPE, 0, 0, NULL, 1, 0, strings, NULL);
        DeregisterEventSource(hEventLog);
    }
}

BOOL InstallService(LPCWSTR servicePath);
BOOL UninstallService();
BOOL StartInstService();
BOOL StopInstService();

int wmain(int argc, wchar_t* argv[]) {
    if (argc < 2) {
        wprintf(L"RarusServiceInstaller - Install, uninstall, start, or stop the Rarus Screen Capture service.\n\n");
        wprintf(L"Usage: %s [install|uninstall|start|stop] [service_path]\n", argv[0]);
        wprintf(L"  install   - Install the service\n");
        wprintf(L"  uninstall - Uninstall the service\n");
        wprintf(L"  start     - Start the service\n");
        wprintf(L"  stop      - Stop the service\n");
        wprintf(L"  service_path - Optional path to the service executable (for install only)\n");
        return 1;
    }

    if (wcscmp(argv[1], L"install") == 0) {
        wchar_t servicePath[MAX_PATH];
        
        if (argc >= 3) {
            StringCbCopyW(servicePath, sizeof(servicePath), argv[2]);
        } else {
            // Get the directory of the current executable
            GetModuleFileNameW(NULL, servicePath, MAX_PATH);
            
            // Remove the installer filename and add the service executable name
            wchar_t* lastBackslash = wcsrchr(servicePath, L'\\');
            if (lastBackslash != NULL) {
                *(lastBackslash + 1) = L'\0';
                StringCbCatW(servicePath, sizeof(servicePath), L"RarusScreenCapture.exe");
            }
        }
        
        // Check if the service executable exists
        DWORD fileAttributes = GetFileAttributesW(servicePath);
        if (fileAttributes == INVALID_FILE_ATTRIBUTES) {
            wprintf(L"Error: Service executable not found at '%s'\n", servicePath);
            wprintf(L"Please specify the correct path to RarusScreenCapture.exe\n");
            return 1;
        }
        
        if (InstallService(servicePath)) {
            wprintf(L"Service installed successfully.\n");
            return 0;
        } else {
            wprintf(L"Service installation failed.\n");
            return 1;
        }
    } else if (wcscmp(argv[1], L"uninstall") == 0) {
        if (UninstallService()) {
            wprintf(L"Service uninstalled successfully.\n");
            return 0;
        } else {
            wprintf(L"Service uninstallation failed.\n");
            return 1;
        }
    } else if (wcscmp(argv[1], L"start") == 0) {
        if (StartInstService()) {
            wprintf(L"Service started successfully.\n");
            return 0;
        } else {
            wprintf(L"Service start failed.\n");
            return 1;
        }
    } else if (wcscmp(argv[1], L"stop") == 0) {
        if (StopInstService()) {
            wprintf(L"Service stopped successfully.\n");
            return 0;
        } else {
            wprintf(L"Service stop failed.\n");
            return 1;
        }
    } else {
        wprintf(L"Unknown command: %s\n", argv[1]);
        return 1;
    }
    
    return 0;
}

// Install the service
BOOL InstallService(LPCWSTR servicePath) {
    SC_HANDLE schSCManager = NULL;
    SC_HANDLE schService = NULL;
    BOOL success = FALSE;
    SERVICE_DESCRIPTIONW serviceDescription = { const_cast<LPWSTR>(SERVICE_DESCRIPTION) };

    wprintf(L"Installing service with path: %s\n", servicePath);

    // Open the service control manager
    schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (schSCManager == NULL) {
        DWORD error = GetLastError();
        wprintf(L"OpenSCManager failed: %d\n", error);
        if (error == ERROR_ACCESS_DENIED) {
            wprintf(L"Access denied. Make sure you're running as Administrator.\n");
        }
        goto cleanup;
    }

    // Check if service already exists
    schService = OpenServiceW(schSCManager, SERVICE_NAME, SERVICE_QUERY_CONFIG);
    if (schService != NULL) {
        wprintf(L"Service already exists. Uninstall it first.\n");
        CloseServiceHandle(schService);
        schService = NULL;
        goto cleanup;
    }

    // Create the service
    schService = CreateServiceW(
        schSCManager,                // SCManager database
        SERVICE_NAME,                // Name of service
        SERVICE_DISPLAY_NAME,        // Display name
        SERVICE_ALL_ACCESS,          // Desired access
        SERVICE_WIN32_OWN_PROCESS,   // Service type
        SERVICE_START_TYPE,          // Start type
        SERVICE_ERROR_NORMAL,        // Error control type
        servicePath,                 // Service binary path
        NULL,                        // No load ordering group
        NULL,                        // No tag identifier
        SERVICE_DEPENDENCIES,        // Dependencies
        SERVICE_ACCOUNT,             // Service account (LocalSystem)
        SERVICE_PASSWORD             // Password
    );

    if (schService == NULL) {
        DWORD error = GetLastError();
        wprintf(L"CreateService failed: %d\n", error);
        goto cleanup;
    }

    // Set the service description
    if (!ChangeServiceConfig2(schService, SERVICE_CONFIG_DESCRIPTION, &serviceDescription)) {
        wprintf(L"ChangeServiceConfig2 failed: %d\n", GetLastError());
        // Continue anyway, this is not critical
    }

    wprintf(L"Service created successfully.\n");
    success = TRUE;

cleanup:
    if (schService) {
        CloseServiceHandle(schService);
    }
    if (schSCManager) {
        CloseServiceHandle(schSCManager);
    }
    return success;
}

// Uninstall the service
BOOL UninstallService() {
    SC_HANDLE schSCManager = NULL;
    SC_HANDLE schService = NULL;
    BOOL success = FALSE;
    SERVICE_STATUS serviceStatus;

    // Open the service control manager
    schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (schSCManager == NULL) {
        DWORD error = GetLastError();
        wprintf(L"OpenSCManager failed: %d\n", error);
        if (error == ERROR_ACCESS_DENIED) {
            wprintf(L"Access denied. Make sure you're running as Administrator.\n");
        }
        goto cleanup;
    }

    // Open the service
    schService = OpenServiceW(
        schSCManager,
        SERVICE_NAME,
        SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE
    );

    if (schService == NULL) {
        DWORD error = GetLastError();
        wprintf(L"OpenService failed: %d\n", error);
        if (error == ERROR_SERVICE_DOES_NOT_EXIST) {
            wprintf(L"Service does not exist.\n");
        }
        goto cleanup;
    }

    // Stop the service if it's running
    if (QueryServiceStatus(schService, &serviceStatus)) {
        if (serviceStatus.dwCurrentState != SERVICE_STOPPED) {
            // Stop the service
            if (ControlService(schService, SERVICE_CONTROL_STOP, &serviceStatus)) {
                wprintf(L"Stopping service...\n");

                // Wait for the service to stop
                Sleep(1000);
                while (QueryServiceStatus(schService, &serviceStatus)) {
                    if (serviceStatus.dwCurrentState == SERVICE_STOPPED) {
                        break;
                    }
                    wprintf(L".");
                    Sleep(1000);
                }
                wprintf(L"\n");

                if (serviceStatus.dwCurrentState != SERVICE_STOPPED) {
                    wprintf(L"Failed to stop service.\n");
                    // Continue anyway, try to delete it
                }
            }
            else {
                wprintf(L"ControlService failed: %d\n", GetLastError());
                // Continue anyway, try to delete it
            }
        }
    }

    // Delete the service
    if (!DeleteService(schService)) {
        DWORD error = GetLastError();
        wprintf(L"DeleteService failed: %d\n", error);
        goto cleanup;
    }

    wprintf(L"Service deleted successfully.\n");
    success = TRUE;

cleanup:
    if (schService) {
        CloseServiceHandle(schService);
    }
    if (schSCManager) {
        CloseServiceHandle(schSCManager);
    }
    return success;
}

// Start the service
BOOL StartInstService() {
    SC_HANDLE schSCManager = NULL;
    SC_HANDLE schService = NULL;
    BOOL success = FALSE;
    SERVICE_STATUS_PROCESS serviceStatus;
    DWORD bytesNeeded;

    // Open the service control manager
    schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (schSCManager == NULL) {
        DWORD error = GetLastError();
        wprintf(L"OpenSCManager failed: %d\n", error);
        if (error == ERROR_ACCESS_DENIED) {
            wprintf(L"Access denied. Make sure you're running as Administrator.\n");
        }
        goto cleanup;
    }

    // Open the service
    schService = OpenServiceW(
        schSCManager,
        SERVICE_NAME,
        SERVICE_START | SERVICE_QUERY_STATUS
    );

    if (schService == NULL) {
        DWORD error = GetLastError();
        wprintf(L"OpenService failed: %d\n", error);
        if (error == ERROR_SERVICE_DOES_NOT_EXIST) {
            wprintf(L"Service does not exist. Install it first.\n");
        }
        goto cleanup;
    }

    // Check if the service is already running
    if (!QueryServiceStatusEx(
        schService,
        SC_STATUS_PROCESS_INFO,
        (LPBYTE)&serviceStatus,
        sizeof(SERVICE_STATUS_PROCESS),
        &bytesNeeded)) {
        wprintf(L"QueryServiceStatusEx failed: %d\n", GetLastError());
        goto cleanup;
    }

    if (serviceStatus.dwCurrentState == SERVICE_RUNNING) {
        wprintf(L"Service is already running.\n");
        success = TRUE;
        goto cleanup;
    }

    // Start the service
    if (!StartService(schService, 0, NULL)) {
        DWORD error = GetLastError();
        wprintf(L"StartService failed: %d\n", error);
        goto cleanup;
    }

    // Wait for the service to start
    wprintf(L"Starting service...\n");
    Sleep(1000);

    // Query the service status
    if (!QueryServiceStatusEx(
        schService,
        SC_STATUS_PROCESS_INFO,
        (LPBYTE)&serviceStatus,
        sizeof(SERVICE_STATUS_PROCESS),
        &bytesNeeded)) {
        wprintf(L"QueryServiceStatusEx failed: %d\n", GetLastError());
        goto cleanup;
    }

    // Wait until the service is running or failed
    while (serviceStatus.dwCurrentState == SERVICE_START_PENDING) {
        wprintf(L".");
        Sleep(1000);

        if (!QueryServiceStatusEx(
            schService,
            SC_STATUS_PROCESS_INFO,
            (LPBYTE)&serviceStatus,
            sizeof(SERVICE_STATUS_PROCESS),
            &bytesNeeded)) {
            wprintf(L"QueryServiceStatusEx failed: %d\n", GetLastError());
            goto cleanup;
        }
    }
    wprintf(L"\n");

    if (serviceStatus.dwCurrentState != SERVICE_RUNNING) {
        wprintf(L"Service failed to start. Status: %d\n", serviceStatus.dwCurrentState);
        goto cleanup;
    }

    wprintf(L"Service started successfully.\n");
    success = TRUE;

cleanup:
    if (schService) {
        CloseServiceHandle(schService);
    }
    if (schSCManager) {
        CloseServiceHandle(schSCManager);
    }
    return success;
}

// Stop the service
BOOL StopInstService() {
    SC_HANDLE schSCManager = NULL;
    SC_HANDLE schService = NULL;
    BOOL success = FALSE;
    SERVICE_STATUS_PROCESS serviceStatus;
    DWORD bytesNeeded;

    // Open the service control manager
    schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (schSCManager == NULL) {
        DWORD error = GetLastError();
        wprintf(L"OpenSCManager failed: %d\n", error);
        if (error == ERROR_ACCESS_DENIED) {
            wprintf(L"Access denied. Make sure you're running as Administrator.\n");
        }
        goto cleanup;
    }

    // Open the service
    schService = OpenServiceW(
        schSCManager,
        SERVICE_NAME,
        SERVICE_STOP | SERVICE_QUERY_STATUS
    );

    if (schService == NULL) {
        DWORD error = GetLastError();
        wprintf(L"OpenService failed: %d\n", error);
        if (error == ERROR_SERVICE_DOES_NOT_EXIST) {
            wprintf(L"Service does not exist.\n");
        }
        goto cleanup;
    }

    // Check if the service is already stopped
    if (!QueryServiceStatusEx(
        schService,
        SC_STATUS_PROCESS_INFO,
        (LPBYTE)&serviceStatus,
        sizeof(SERVICE_STATUS_PROCESS),
        &bytesNeeded)) {
        wprintf(L"QueryServiceStatusEx failed: %d\n", GetLastError());
        goto cleanup;
    }

    if (serviceStatus.dwCurrentState == SERVICE_STOPPED) {
        wprintf(L"Service is already stopped.\n");
        success = TRUE;
        goto cleanup;
    }

    // Stop the service
    if (!ControlService(schService, SERVICE_CONTROL_STOP, (LPSERVICE_STATUS)&serviceStatus)) {
        wprintf(L"ControlService failed: %d\n", GetLastError());
        goto cleanup;
    }

    // Wait for the service to stop
    wprintf(L"Stopping service...\n");
    Sleep(1000);

    // Query the service status
    if (!QueryServiceStatusEx(
        schService,
        SC_STATUS_PROCESS_INFO,
        (LPBYTE)&serviceStatus,
        sizeof(SERVICE_STATUS_PROCESS),
        &bytesNeeded)) {
        wprintf(L"QueryServiceStatusEx failed: %d\n", GetLastError());
        goto cleanup;
    }

    // Wait until the service is stopped or failed
    while (serviceStatus.dwCurrentState == SERVICE_STOP_PENDING) {
        wprintf(L".");
        Sleep(1000);

        if (!QueryServiceStatusEx(
            schService,
            SC_STATUS_PROCESS_INFO,
            (LPBYTE)&serviceStatus,
            sizeof(SERVICE_STATUS_PROCESS),
            &bytesNeeded)) {
            wprintf(L"QueryServiceStatusEx failed: %d\n", GetLastError());
            goto cleanup;
        }
    }
    wprintf(L"\n");

    if (serviceStatus.dwCurrentState != SERVICE_STOPPED) {
        wprintf(L"Service failed to stop. Status: %d\n", serviceStatus.dwCurrentState);
        goto cleanup;
    }

    wprintf(L"Service stopped successfully.\n");
    success = TRUE;

cleanup:
    if (schService) {
        CloseServiceHandle(schService);
    }
    if (schSCManager) {
        CloseServiceHandle(schSCManager);
    }
    return success;
}