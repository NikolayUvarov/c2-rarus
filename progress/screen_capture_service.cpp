
#include <windows.h>
#include <wincodec.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <memory>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <thread>
#include <fstream>

// Add COM support
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowscodecs.lib")

// Define UNICODE if not already defined
#ifndef UNICODE
#define UNICODE
#endif

// Define _UNICODE if not already defined
#ifndef _UNICODE
#define _UNICODE
#endif

// Uncomment this to force software rendering
// #define FORCE_SOFTWARE_RENDERER

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

// Service name and description
#define SERVICE_NAME L"RarusScreenCapture"
#define SERVICE_DISPLAY_NAME L"Rarus Screen Capture Service"
#define SERVICE_DESCRIPTION L"Captures screen images periodically for remote viewing"

// Named pipe base path
#define PIPE_BASE_PATH L"\\\\.\\pipe\\rarus-scr\\"

// Capture interval in milliseconds
#define CAPTURE_INTERVAL 1000

// Log file path
#define LOG_FILE_PATH L"C:/temp/captures/service_log.txt"

// Service status and control
SERVICE_STATUS g_ServiceStatus = { 0 };
SERVICE_STATUS_HANDLE g_StatusHandle = NULL;
HANDLE g_ServiceStopEvent = INVALID_HANDLE_VALUE;

// Capture thread and control
std::thread g_CaptureThread;
std::atomic<bool> g_Running(false);
std::mutex g_CaptureMutex;
std::condition_variable g_CaptureCV;

// Logging mutex
std::mutex g_LogMutex;

// Forward declarations
void WINAPI ServiceMain(DWORD argc, LPWSTR* argv);
void WINAPI ServiceCtrlHandler(DWORD);
DWORD WINAPI ServiceWorkerThread(LPVOID lpParam);
bool InitializeScreenCapture();
void CleanupScreenCapture();
bool CaptureScreens();
bool SaveImageToFile(const std::vector<BYTE>& imageData, const std::wstring& filename);
bool WriteImageToPipe(const std::vector<BYTE>& imageData, int monitorIndex);
std::wstring GetTimestampedFilename();
LRESULT CALLBACK WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
std::vector<BYTE> CompressToJpeg(ID3D11Texture2D* pTexture, UINT width, UINT height);

// Global DXGI and D3D resources
ComPtr<ID3D11Device> g_D3DDevice;
ComPtr<ID3D11DeviceContext> g_D3DContext;
std::vector<ComPtr<IDXGIOutputDuplication>> g_DuplicationInterfaces;
std::vector<DXGI_OUTPUT_DESC> g_MonitorInfo;
HWND g_MessageWindow = NULL;

// Detailed logging function that writes to a file
void LogToFile(const std::wstring& message) {
    std::lock_guard<std::mutex> lock(g_LogMutex);
    
    try {
        // Get current time for timestamp
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        std::tm tm;
        localtime_s(&tm, &time);
        
        // Format timestamp
        std::wstringstream timestamp;
        timestamp << std::setw(4) << std::setfill(L'0') << (tm.tm_year + 1900) << L"-";
        timestamp << std::setw(2) << std::setfill(L'0') << (tm.tm_mon + 1) << L"-";
        timestamp << std::setw(2) << std::setfill(L'0') << tm.tm_mday << L" ";
        timestamp << std::setw(2) << std::setfill(L'0') << tm.tm_hour << L":";
        timestamp << std::setw(2) << std::setfill(L'0') << tm.tm_min << L":";
        timestamp << std::setw(2) << std::setfill(L'0') << tm.tm_sec;
        
        // Get process ID and thread ID
        DWORD processId = GetCurrentProcessId();
        DWORD threadId = GetCurrentThreadId();
        
        // Create full log message
        std::wstringstream fullMessage;
        fullMessage << timestamp.str() << L" [PID:" << processId << L" TID:" << threadId << L"] " << message << L"\r\n";
        
        // Open the file for appending
        HANDLE hFile = CreateFileW(
            LOG_FILE_PATH,
            FILE_APPEND_DATA,
            FILE_SHARE_READ,
            NULL,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            NULL
        );
        
        if (hFile != INVALID_HANDLE_VALUE) {
            // Write the message
            std::wstring msgStr = fullMessage.str();
            DWORD bytesWritten;
            WriteFile(
                hFile,
                msgStr.c_str(),
                msgStr.size() * sizeof(wchar_t),
                &bytesWritten,
                NULL
            );
            
            // Close the file
            CloseHandle(hFile);
        }
    }
    catch (const std::exception& ex) {
        // Do nothing if logging fails - we can't log the logging failure
    }
}

// Error logging function
void LogError(const wchar_t* message, DWORD error = GetLastError()) {
    // Log to Event Log
    HANDLE hEventLog = RegisterEventSourceW(NULL, SERVICE_NAME);
    if (hEventLog) {
        wchar_t errorMsg[512];
        swprintf_s(errorMsg, L"%s (Error code: %lu)", message, error);
        const wchar_t* strings[1] = { errorMsg };
        ReportEventW(hEventLog, EVENTLOG_ERROR_TYPE, 0, 0, NULL, 1, 0, strings, NULL);
        DeregisterEventSource(hEventLog);
    }
    
    // Also log to file
    std::wstringstream errorStream;
    errorStream << L"ERROR: " << message << L" (Error code: " << error << L")";
    LogToFile(errorStream.str());
}

// Info logging function
void LogInfo(const wchar_t* message) {
    // Log to Event Log
    HANDLE hEventLog = RegisterEventSourceW(NULL, SERVICE_NAME);
    if (hEventLog) {
        const wchar_t* strings[1] = { message };
        ReportEventW(hEventLog, EVENTLOG_INFORMATION_TYPE, 0, 0, NULL, 1, 0, strings, NULL);
        DeregisterEventSource(hEventLog);
    }
    
    // Also log to file
    std::wstringstream infoStream;
    infoStream << L"INFO: " << message;
    LogToFile(infoStream.str());
}

// Messages
#define WM_CAPTURE_START (WM_USER + 1)
#define WM_CAPTURE_STOP (WM_USER + 2)

// Entry point
int wmain(int argc, wchar_t* argv[]) {
    // Initialize COM for the main thread
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        wprintf(L"COM initialization failed: 0x%08X\n", hr);
        return 1;
    }

    // Create base directory for captures if it doesn't exist
    try {
        fs::create_directories("C:/temp/captures");
    }
    catch (const std::exception& ex) {
        wprintf(L"Failed to create capture directory: %hs\n", ex.what());
        // Continue anyway, will check directory again later
    }

    // Start logging
    LogToFile(L"Application starting");
    LogToFile(L"Command-line arguments:");
    for (int i = 0; i < argc; i++) {
        std::wstringstream argStream;
        argStream << L"  Arg[" << i << L"]: " << argv[i];
        LogToFile(argStream.str());
    }

    // Register the service
    SERVICE_TABLE_ENTRY ServiceTable[] = {
        { const_cast<LPWSTR>(SERVICE_NAME), (LPSERVICE_MAIN_FUNCTION)ServiceMain },
        { NULL, NULL }
    };

    if (StartServiceCtrlDispatcher(ServiceTable) == FALSE) {
        // If running as console app for debugging or service is not installed
        DWORD error = GetLastError();
        if (error == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            // Running as console application, not as service
            LogToFile(L"Running in console mode (not as service)");
            wprintf(L"Running in console mode for debugging...\n");

            // Initialize screen capture with additional error checking
            wprintf(L"Initializing screen capture...\n");
            LogToFile(L"Initializing screen capture in console mode");
            if (!InitializeScreenCapture()) {
                LogToFile(L"Screen capture initialization failed in console mode");
                wprintf(L"Screen capture initialization failed.\n");
                CoUninitialize();
                return 1;
            }

            // Start capturing
            wprintf(L"Starting capture thread...\n");
            LogToFile(L"Starting capture thread in console mode");
            g_Running = true;
            g_CaptureThread = std::thread([]() {
                LogToFile(L"Capture thread started in console mode");
                while (g_Running) {
                    LogToFile(L"Attempting to capture screens in console mode");
                    bool result = CaptureScreens();
                    if (!result) {
                        LogToFile(L"Screen capture failed in console mode, will retry after delay");
                        // If capture fails, wait a bit and try to re-initialize
                        std::this_thread::sleep_for(std::chrono::seconds(5));
                        CleanupScreenCapture();
                        if (InitializeScreenCapture()) {
                            // Successfully reinitialized
                            LogToFile(L"Screen capture reinitialized in console mode");
                            wprintf(L"Screen capture reinitialized.\n");
                        }
                    }
                    else {
                        LogToFile(L"Screen capture successful in console mode");
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(CAPTURE_INTERVAL));
                }
                LogToFile(L"Capture thread stopping in console mode");
            });

            wprintf(L"Press any key to exit...\n");
            getchar();

            // Cleanup
            wprintf(L"Stopping capture thread...\n");
            LogToFile(L"Stopping capture thread in console mode");
            g_Running = false;
            if (g_CaptureThread.joinable()) {
                g_CaptureThread.join();
            }
            wprintf(L"Cleaning up resources...\n");
            LogToFile(L"Cleaning up resources in console mode");
            CleanupScreenCapture();
            wprintf(L"Done.\n");
            LogToFile(L"Console application exiting");
        }
        else {
            // Error with service dispatcher
            LogToFile(L"StartServiceCtrlDispatcher failed: " + std::to_wstring(error));
            wprintf(L"StartServiceCtrlDispatcher failed: %d\n", error);
        }
    }
    else {
        LogToFile(L"Service dispatcher completed");
    }

    CoUninitialize();
    LogToFile(L"Application exiting");
    return 0;
}

// Main service function
void WINAPI ServiceMain(DWORD argc, LPWSTR* argv) {
    LogToFile(L"ServiceMain started");
    
    // Register service control handler
    g_StatusHandle = RegisterServiceCtrlHandlerW(SERVICE_NAME, ServiceCtrlHandler);
    if (g_StatusHandle == NULL) {
        LogError(L"RegisterServiceCtrlHandler failed");
        LogToFile(L"ServiceMain exiting due to RegisterServiceCtrlHandler failure");
        return;
    }

    // Initialize service status
    g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ServiceStatus.dwCurrentState = SERVICE_START_PENDING;
    g_ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    g_ServiceStatus.dwWin32ExitCode = 0;
    g_ServiceStatus.dwServiceSpecificExitCode = 0;
    g_ServiceStatus.dwCheckPoint = 0;
    g_ServiceStatus.dwWaitHint = 3000;

    // Update the service status
    if (SetServiceStatus(g_StatusHandle, &g_ServiceStatus) == FALSE) {
        LogError(L"SetServiceStatus failed");
        LogToFile(L"ServiceMain exiting due to SetServiceStatus failure");
        return;
    }

    // Create stop event
    g_ServiceStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (g_ServiceStopEvent == NULL) {
        g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        g_ServiceStatus.dwWin32ExitCode = GetLastError();
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        LogError(L"CreateEvent failed");
        LogToFile(L"ServiceMain exiting due to CreateEvent failure");
        return;
    }

    // Start service worker thread
    LogToFile(L"Starting ServiceWorkerThread");
    HANDLE hThread = CreateThread(NULL, 0, ServiceWorkerThread, NULL, 0, NULL);
    if (hThread == NULL) {
        g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        g_ServiceStatus.dwWin32ExitCode = GetLastError();
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        LogError(L"CreateThread failed");
        LogToFile(L"ServiceMain exiting due to CreateThread failure");
        CloseHandle(g_ServiceStopEvent);
        return;
    }

    // Wait for service to stop
    LogToFile(L"Waiting for service worker thread to complete");
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);
    CloseHandle(g_ServiceStopEvent);

    g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
    g_ServiceStatus.dwWin32ExitCode = 0;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
    LogToFile(L"ServiceMain exiting normally");
}

// Service control handler
void WINAPI ServiceCtrlHandler(DWORD dwControl) {
    std::wstring controlMsg = L"ServiceCtrlHandler received control code: " + std::to_wstring(dwControl);
    LogToFile(controlMsg);
    
    switch (dwControl) {
    case SERVICE_CONTROL_STOP:
        LogToFile(L"Received SERVICE_CONTROL_STOP");
    case SERVICE_CONTROL_SHUTDOWN:
        LogToFile(L"Received SERVICE_CONTROL_SHUTDOWN");
        if (g_ServiceStatus.dwCurrentState != SERVICE_RUNNING) {
            LogToFile(L"Service is not running, ignoring stop/shutdown request");
            break;
        }

        g_ServiceStatus.dwControlsAccepted = 0;
        g_ServiceStatus.dwCurrentState = SERVICE_STOP_PENDING;
        g_ServiceStatus.dwWin32ExitCode = 0;
        g_ServiceStatus.dwCheckPoint = 4;
        
        if (SetServiceStatus(g_StatusHandle, &g_ServiceStatus) == FALSE) {
            LogError(L"SetServiceStatus failed during stop");
            LogToFile(L"Failed to update service status during stop/shutdown");
        }

        // Signal the service to stop
        LogToFile(L"Signaling service stop event");
        SetEvent(g_ServiceStopEvent);
        break;

    default:
        LogToFile(L"Received unhandled control code, ignoring");
        break;
    }
}

// Hidden window class name
#define WINDOW_CLASS_NAME L"RarusScreenCaptureMessageWindow"

// Service worker thread
DWORD WINAPI ServiceWorkerThread(LPVOID lpParam) {
    LogToFile(L"ServiceWorkerThread started");
    
    // Initialize COM
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        LogError(L"COM initialization failed in service thread", hr);
        LogToFile(L"ServiceWorkerThread exiting due to COM initialization failure");
        g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        g_ServiceStatus.dwWin32ExitCode = hr;
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        return 0;
    }

    LogToFile(L"COM initialized successfully");

    // Create the capture directory if it doesn't exist
    try {
        LogToFile(L"Creating capture directory");
        fs::create_directories("C:/temp/captures");
        LogToFile(L"Capture directory created or already exists");
    }
    catch (const std::exception& ex) {
        LogToFile(L"Failed to create capture directory: " + std::wstring(L"Exception caught"));
        LogError(L"Failed to create capture directory");
        // Continue anyway, will attempt to create directory during saves
    }

    // Register window class for message window
    LogToFile(L"Registering window class");
    WNDCLASSEXW wx = {};
    wx.cbSize = sizeof(WNDCLASSEXW);
    wx.lpfnWndProc = WndProc;
    wx.hInstance = GetModuleHandle(NULL);
    wx.lpszClassName = WINDOW_CLASS_NAME;
    
    if (!RegisterClassExW(&wx)) {
        DWORD error = GetLastError();
        if (error != ERROR_CLASS_ALREADY_EXISTS) {
            LogError(L"RegisterClassEx failed", error);
            LogToFile(L"Window class registration failed");
            CoUninitialize();
            g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
            g_ServiceStatus.dwWin32ExitCode = error;
            SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
            return 0;
        }
        else {
            LogToFile(L"Window class already exists, continuing");
        }
    }
    else {
        LogToFile(L"Window class registered successfully");
    }

    // Create message window
    LogToFile(L"Creating message window");
    g_MessageWindow = CreateWindowExW(0, WINDOW_CLASS_NAME, L"RarusScreenCaptureMessageWindow",
        0, 0, 0, 0, 0, HWND_MESSAGE, NULL, GetModuleHandle(NULL), NULL);

    if (g_MessageWindow == NULL) {
        DWORD error = GetLastError();
        LogError(L"CreateWindowEx failed", error);
        LogToFile(L"Message window creation failed");
        UnregisterClassW(WINDOW_CLASS_NAME, GetModuleHandle(NULL));
        CoUninitialize();
        g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        g_ServiceStatus.dwWin32ExitCode = error;
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        return 0;
    }

    LogToFile(L"Message window created successfully");

    // Initialize screen capture
    LogToFile(L"Initializing screen capture");
    if (!InitializeScreenCapture()) {
        LogError(L"Screen capture initialization failed");
        LogToFile(L"Screen capture initialization failed");
        if (g_MessageWindow) {
            LogToFile(L"Destroying message window");
            DestroyWindow(g_MessageWindow);
            g_MessageWindow = NULL;
        }
        LogToFile(L"Unregistering window class");
        UnregisterClassW(WINDOW_CLASS_NAME, GetModuleHandle(NULL));
        LogToFile(L"Uninitializing COM");
        CoUninitialize();
        g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        g_ServiceStatus.dwWin32ExitCode = 1;
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        return 0;
    }

    LogToFile(L"Screen capture initialized successfully");

    // Service is now running
    LogToFile(L"Updating service status to RUNNING");
    g_ServiceStatus.dwCurrentState = SERVICE_RUNNING;
    g_ServiceStatus.dwCheckPoint = 0;
    g_ServiceStatus.dwWaitHint = 0;
    
    if (SetServiceStatus(g_StatusHandle, &g_ServiceStatus) == FALSE) {
        LogError(L"SetServiceStatus failed");
        LogToFile(L"Failed to update service status to RUNNING");
        CleanupScreenCapture();
        if (g_MessageWindow) {
            DestroyWindow(g_MessageWindow);
            g_MessageWindow = NULL;
        }
        UnregisterClassW(WINDOW_CLASS_NAME, GetModuleHandle(NULL));
        CoUninitialize();
        return 0;
    }

    LogToFile(L"Service status updated to RUNNING successfully");

    // Start capturing
    LogToFile(L"Starting capture thread");
    g_Running = true;
    g_CaptureThread = std::thread([]() {
        LogToFile(L"Capture thread started");
        try {
            while (g_Running) {
                LogToFile(L"Attempting to capture screens");
                bool result = CaptureScreens();
                if (!result) {
                    LogToFile(L"Screen capture failed, will retry after delay");
                    // If capture fails, wait a bit and try to re-initialize
                    std::this_thread::sleep_for(std::chrono::seconds(5));
                    CleanupScreenCapture();
                    if (InitializeScreenCapture()) {
                        // Successfully reinitialized
                        LogToFile(L"Screen capture reinitialized successfully");
                    }
                    else {
                        LogToFile(L"Screen capture reinitialization failed");
                    }
                }
                else {
                    LogToFile(L"Screen capture successful");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(CAPTURE_INTERVAL));
            }
        }
        catch (const std::exception& ex) {
            // Log the exception but don't crash
            LogToFile(L"Exception in capture thread: exception caught");
            LogError(L"Exception in capture thread");
        }
        LogToFile(L"Capture thread stopping");
    });

    // Message loop
    LogToFile(L"Entering message loop");
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    LogToFile(L"Message loop exited");

    // Wait for stop event
    LogToFile(L"Waiting for stop event");
    WaitForSingleObject(g_ServiceStopEvent, INFINITE);
    LogToFile(L"Stop event signaled");

    // Stop capture thread
    LogToFile(L"Stopping capture thread");
    g_Running = false;
    if (g_CaptureThread.joinable()) {
        LogToFile(L"Waiting for capture thread to join");
        g_CaptureThread.join();
        LogToFile(L"Capture thread joined successfully");
    }
    else {
        LogToFile(L"Capture thread was not joinable");
    }

    // Clean up resources
    LogToFile(L"Cleaning up screen capture resources");
    CleanupScreenCapture();
    
    if (g_MessageWindow) {
        LogToFile(L"Destroying message window");
        DestroyWindow(g_MessageWindow);
        g_MessageWindow = NULL;
    }
    
    LogToFile(L"Unregistering window class");
    UnregisterClassW(WINDOW_CLASS_NAME, GetModuleHandle(NULL));
    
    LogToFile(L"Uninitializing COM");
    CoUninitialize();

    LogToFile(L"ServiceWorkerThread exiting normally");
    return 0;
}

// Window procedure for the message window
LRESULT CALLBACK WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    std::wstringstream msgStream;
    msgStream << L"WndProc received message: " << uMsg;
    LogToFile(msgStream.str());
    
    switch (uMsg) {
    case WM_CAPTURE_START:
        LogToFile(L"Received WM_CAPTURE_START message");
        if (!g_Running) {
            LogToFile(L"Starting capture thread from WM_CAPTURE_START");
            g_Running = true;
            g_CaptureThread = std::thread([]() {
                LogToFile(L"Capture thread started from WM_CAPTURE_START");
                while (g_Running) {
                    CaptureScreens();
                    std::this_thread::sleep_for(std::chrono::milliseconds(CAPTURE_INTERVAL));
                }
                LogToFile(L"Capture thread stopping from WM_CAPTURE_START");
            });
        }
        else {
            LogToFile(L"Capture thread already running, ignoring WM_CAPTURE_START");
        }
        return 0;

    case WM_CAPTURE_STOP:
        LogToFile(L"Received WM_CAPTURE_STOP message");
        if (g_Running) {
            LogToFile(L"Stopping capture thread from WM_CAPTURE_STOP");
            g_Running = false;
            if (g_CaptureThread.joinable()) {
                LogToFile(L"Waiting for capture thread to join from WM_CAPTURE_STOP");
                g_CaptureThread.join();
                LogToFile(L"Capture thread joined successfully from WM_CAPTURE_STOP");
            }
            else {
                LogToFile(L"Capture thread was not joinable from WM_CAPTURE_STOP");
            }
        }
        else {
            LogToFile(L"Capture thread not running, ignoring WM_CAPTURE_STOP");
        }
        return 0;

    default:
        LogToFile(L"Unhandled window message, passing to DefWindowProc");
        return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
}

// Initialize screen capture with DXGI Desktop Duplication
bool InitializeScreenCapture() {
    LogToFile(L"InitializeScreenCapture called");
    HRESULT hr;

    // Create D3D device with additional flags for debugging
    D3D_FEATURE_LEVEL featureLevel;
    UINT createDeviceFlags = 0;
    
    // In debug builds, enable debugging
#ifdef _DEBUG
    createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
    LogToFile(L"Debug build: enabling D3D11_CREATE_DEVICE_DEBUG");
#endif

    // Try with hardware acceleration first, unless forced to use software
#ifndef FORCE_SOFTWARE_RENDERER
    LogToFile(L"Attempting to create D3D11 device with hardware acceleration");
    hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        createDeviceFlags,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        g_D3DDevice.GetAddressOf(),
        &featureLevel,
        g_D3DContext.GetAddressOf()
    );
#else
    hr = E_FAIL; // Force software renderer
    LogToFile(L"Software rendering forced");
#endif

    // If hardware acceleration fails, try with WARP (software rendering)
    if (FAILED(hr)) {
        LogError(L"Hardware D3D11 device creation failed, trying WARP", hr);
        LogToFile(L"Hardware D3D11 device creation failed with HRESULT " + std::to_wstring(hr) + L", trying WARP");
        
        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            createDeviceFlags,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            g_D3DDevice.GetAddressOf(),
            &featureLevel,
            g_D3DContext.GetAddressOf()
        );
        
        if (FAILED(hr)) {
            LogError(L"WARP D3D11 device creation failed", hr);
            LogToFile(L"WARP D3D11 device creation failed with HRESULT " + std::to_wstring(hr));
            return false;
        }
        LogToFile(L"WARP D3D11 device created successfully");
    }
    else {
        LogToFile(L"Hardware D3D11 device created successfully");
    }

    // Output feature level
    std::wstringstream flStream;
    flStream << L"D3D11 device created with feature level: ";
    switch(featureLevel) {
        case D3D_FEATURE_LEVEL_11_1: flStream << L"11.1"; break;
        case D3D_FEATURE_LEVEL_11_0: flStream << L"11.0"; break;
        case D3D_FEATURE_LEVEL_10_1: flStream << L"10.1"; break;
        case D3D_FEATURE_LEVEL_10_0: flStream << L"10.0"; break;
        case D3D_FEATURE_LEVEL_9_3: flStream << L"9.3"; break;
        case D3D_FEATURE_LEVEL_9_2: flStream << L"9.2"; break;
        case D3D_FEATURE_LEVEL_9_1: flStream << L"9.1"; break;
        default: flStream << L"Unknown (" << featureLevel << L")";
    }
    LogToFile(flStream.str());

    // Get DXGI device
    LogToFile(L"Getting DXGI device");
    ComPtr<IDXGIDevice> dxgiDevice;
    hr = g_D3DDevice.As(&dxgiDevice);
    if (FAILED(hr)) {
        LogError(L"Failed to get DXGI device", hr);
        LogToFile(L"Failed to get DXGI device with HRESULT " + std::to_wstring(hr));
        return false;
    }
    LogToFile(L"DXGI device obtained successfully");

    // Get DXGI adapter
    LogToFile(L"Getting DXGI adapter");
    ComPtr<IDXGIAdapter> dxgiAdapter;
    hr = dxgiDevice->GetAdapter(&dxgiAdapter);
    if (FAILED(hr)) {
        LogError(L"Failed to get DXGI adapter", hr);
        LogToFile(L"Failed to get DXGI adapter with HRESULT " + std::to_wstring(hr));
        return false;
    }
    LogToFile(L"DXGI adapter obtained successfully");

    // Get adapter description
    DXGI_ADAPTER_DESC adapterDesc;
    if (SUCCEEDED(dxgiAdapter->GetDesc(&adapterDesc))) {
        LogToFile(L"Adapter: " + std::wstring(adapterDesc.Description));
        std::wstringstream memStream;
        memStream << L"Adapter Dedicated Video Memory: " << (adapterDesc.DedicatedVideoMemory / (1024 * 1024)) << L" MB";
        LogToFile(memStream.str());
    }

    // Enumerate outputs (monitors)
    LogToFile(L"Enumerating monitors");
    UINT i = 0;
    ComPtr<IDXGIOutput> currentOutput;
    

    while (dxgiAdapter->EnumOutputs(i, &currentOutput) != DXGI_ERROR_NOT_FOUND) {
        // Get output description
        DXGI_OUTPUT_DESC outputDesc;
        hr = currentOutput->GetDesc(&outputDesc);
        if (SUCCEEDED(hr)) {
            // Log monitor information
            std::wstringstream monitorStream;
            monitorStream << L"Monitor " << i << L": " << (outputDesc.AttachedToDesktop ? L"Attached" : L"Detached");
            monitorStream << L", Device: " << (outputDesc.DeviceName ? outputDesc.DeviceName : L"Unknown");
            LogToFile(monitorStream.str());
            
            std::wstringstream rectStream;
            rectStream << L"Monitor " << i << L" coordinates: ("
                << outputDesc.DesktopCoordinates.left << L"," 
                << outputDesc.DesktopCoordinates.top << L") - ("
                << outputDesc.DesktopCoordinates.right << L"," 
                << outputDesc.DesktopCoordinates.bottom << L")";
            LogToFile(rectStream.str());
            
            g_MonitorInfo.push_back(outputDesc);

            // Query for Output1
            ComPtr<IDXGIOutput1> output1;
            hr = currentOutput.As(&output1);
            if (SUCCEEDED(hr)) {
                // Create duplication interface
                ComPtr<IDXGIOutputDuplication> duplication;
                hr = output1->DuplicateOutput(g_D3DDevice.Get(), &duplication);
                if (SUCCEEDED(hr)) {
                    LogToFile(L"Successfully created duplication interface for monitor " + std::to_wstring(i));
                    g_DuplicationInterfaces.push_back(duplication);
                }
                else {
                    // Log that we failed to duplicate this output but continue
                    LogError(L"Failed to duplicate output", hr);
                    LogToFile(L"Failed to create duplication interface for monitor " + std::to_wstring(i) + 
                              L" with HRESULT " + std::to_wstring(hr));
                }
            }
            else {
                LogError(L"Failed to get IDXGIOutput1 interface", hr);
                LogToFile(L"Failed to get IDXGIOutput1 interface for monitor " + std::to_wstring(i) + 
                          L" with HRESULT " + std::to_wstring(hr));
            }
        }
        else {
            LogError(L"Failed to get output description", hr);
            LogToFile(L"Failed to get output description for monitor " + std::to_wstring(i) + 
                      L" with HRESULT " + std::to_wstring(hr));
        }

        currentOutput = nullptr;
        i++;
    }

    // Make sure we found at least one monitor
    if (g_DuplicationInterfaces.empty()) {
        LogError(L"No monitors found or duplication interfaces created");
        LogToFile(L"Failed to create any duplication interfaces");
        return false;
    }

    LogToFile(L"Successfully created " + std::to_wstring(g_DuplicationInterfaces.size()) + L" duplication interfaces");
    return true;
}

// Cleanup screen capture resources
void CleanupScreenCapture() {
    LogToFile(L"CleanupScreenCapture called");
    
    // Release duplication interfaces
    LogToFile(L"Releasing duplication interfaces");
    g_DuplicationInterfaces.clear();
    
    // Release monitor info
    LogToFile(L"Clearing monitor info");
    g_MonitorInfo.clear();

    // Release D3D resources
    LogToFile(L"Releasing D3D context");
    if (g_D3DContext) g_D3DContext.Reset();
    
    LogToFile(L"Releasing D3D device");
    if (g_D3DDevice) g_D3DDevice.Reset();
    
    LogToFile(L"CleanupScreenCapture completed");
}

// Capture all screens
bool CaptureScreens() {
    // We need detailed logging here, but shouldn't log every frame to avoid filling the log file
    // So use a static counter to log only occasionally
    static int captureCount = 0;
    bool detailedLogging = (captureCount % 100 == 0); // Log details every 100 captures
    captureCount++;
    
    if (detailedLogging) {
        LogToFile(L"CaptureScreens called (capture #" + std::to_wstring(captureCount) + L")");
    }
    
    if (g_DuplicationInterfaces.empty()) {
        LogToFile(L"No duplication interfaces available");
        return false;
    }

    HRESULT hr;
    std::vector<ComPtr<ID3D11Texture2D>> monitorTextures;
    std::vector<DXGI_OUTDUPL_FRAME_INFO> frameInfos;

    // Acquire frames from each monitor
    for (size_t i = 0; i < g_DuplicationInterfaces.size(); i++) {
        ComPtr<IDXGIResource> desktopResource;
        DXGI_OUTDUPL_FRAME_INFO frameInfo;

        // Try to acquire the next frame
        hr = g_DuplicationInterfaces[i]->AcquireNextFrame(100, &frameInfo, &desktopResource);
        
        if (SUCCEEDED(hr)) {
            if (detailedLogging) {
                LogToFile(L"Successfully acquired frame from monitor " + std::to_wstring(i));
            }
            
            ComPtr<ID3D11Texture2D> desktopTexture;
            hr = desktopResource->QueryInterface(IID_PPV_ARGS(&desktopTexture));
            
            if (SUCCEEDED(hr)) {
                // Store texture and frame info
                monitorTextures.push_back(desktopTexture);
                frameInfos.push_back(frameInfo);
            }
            else {
                LogError(L"Failed to query texture interface", hr);
                LogToFile(L"Failed to query texture interface with HRESULT " + std::to_wstring(hr));
            }
            
            // Release the frame
            g_DuplicationInterfaces[i]->ReleaseFrame();
        }
        else if (hr != DXGI_ERROR_WAIT_TIMEOUT) {
            // Log only if it's not a timeout error
            LogError(L"Failed to acquire next frame", hr);
            LogToFile(L"Failed to acquire frame from monitor " + std::to_wstring(i) + 
                      L" with HRESULT " + std::to_wstring(hr));
        }
        else if (detailedLogging) {
            LogToFile(L"Timeout waiting for frame from monitor " + std::to_wstring(i));
        }
    }

    // If we didn't capture any frames, return
    if (monitorTextures.empty()) {
        if (detailedLogging) {
            LogToFile(L"No frames captured from any monitor");
        }
        return false;
    }

    // Process each captured monitor
    for (size_t i = 0; i < monitorTextures.size(); i++) {
        try {
            // Get texture description
            D3D11_TEXTURE2D_DESC textureDesc;
            monitorTextures[i]->GetDesc(&textureDesc);

            if (detailedLogging) {
                std::wstringstream texStream;
                texStream << L"Monitor " << i << L" texture: " << textureDesc.Width << L"x" << textureDesc.Height 
                         << L", Format: " << textureDesc.Format;
                LogToFile(texStream.str());
            }

            // Compress the texture to JPEG
            std::vector<BYTE> imageData = CompressToJpeg(monitorTextures[i].Get(), textureDesc.Width, textureDesc.Height);
            
            if (!imageData.empty()) {
                if (detailedLogging) {
                    LogToFile(L"Successfully compressed monitor " + std::to_wstring(i) + 
                              L" image to JPEG (" + std::to_wstring(imageData.size()) + L" bytes)");
                }
                
                // Write to named pipe for this monitor
                WriteImageToPipe(imageData, static_cast<int>(i + 1)); // +1 because 0 is reserved for all screens
            }
            else if (detailedLogging) {
                LogToFile(L"Failed to compress monitor " + std::to_wstring(i) + L" image to JPEG");
            }
        }
        catch (const std::exception& ex) {
            // Log but continue with other monitors
            LogError(L"Exception while processing monitor");
            LogToFile(L"Exception while processing monitor " + std::to_wstring(i) + L": exception caught");
        }
    }

    // Create a combined image from all monitors if we have multiple monitors
    if (monitorTextures.size() > 1) {
        // For simplicity and stability, just use the first monitor for now
        if (!monitorTextures.empty()) {
            try {
                LogToFile(L"Creating combined image (using first monitor)");
                D3D11_TEXTURE2D_DESC firstDesc;
                monitorTextures[0]->GetDesc(&firstDesc);
                
                std::vector<BYTE> combinedImageData = CompressToJpeg(monitorTextures[0].Get(), firstDesc.Width, firstDesc.Height);
                
                if (!combinedImageData.empty()) {
                    LogToFile(L"Successfully compressed combined image to JPEG (" + 
                              std::to_wstring(combinedImageData.size()) + L" bytes)");
                    
                    // Write to named pipe for all screens
                    WriteImageToPipe(combinedImageData, 0);
                    
                    // Save to file with timestamp
                    std::wstring filename = GetTimestampedFilename();
                    SaveImageToFile(combinedImageData, filename);
                }
                else {
                    LogToFile(L"Failed to compress combined image to JPEG");
                }
            }
            catch (const std::exception& ex) {
                LogError(L"Exception while creating combined image");
                LogToFile(L"Exception while creating combined image: exception caught");
            }
        }
    }
    else if (monitorTextures.size() == 1) {
        // If we only have one monitor, use its image for both the named pipe and the file
        try {
            LogToFile(L"Creating single monitor image");
            D3D11_TEXTURE2D_DESC desc;
            monitorTextures[0]->GetDesc(&desc);
            
            std::vector<BYTE> imageData = CompressToJpeg(monitorTextures[0].Get(), desc.Width, desc.Height);
            
            if (!imageData.empty()) {
                LogToFile(L"Successfully compressed single monitor image to JPEG (" + 
                          std::to_wstring(imageData.size()) + L" bytes)");
                
                // Write to named pipe for all screens
                WriteImageToPipe(imageData, 0);
                
                // Save to file with timestamp
                std::wstring filename = GetTimestampedFilename();
                SaveImageToFile(imageData, filename);
            }
            else {
                LogToFile(L"Failed to compress single monitor image to JPEG");
            }
        }
        catch (const std::exception& ex) {
            LogError(L"Exception while processing single monitor");
            LogToFile(L"Exception while processing single monitor: exception caught");
        }
    }

    if (detailedLogging) {
        LogToFile(L"CaptureScreens completed successfully");
    }
    return true;
}

// Compress a texture to JPEG
std::vector<BYTE> CompressToJpeg(ID3D11Texture2D* pTexture, UINT width, UINT height) {
    std::vector<BYTE> imageData;
    HRESULT hr;

    LogToFile(L"CompressToJpeg called for " + std::to_wstring(width) + L"x" + std::to_wstring(height) + L" texture");

    // Null check
    if (!pTexture) {
        LogError(L"Null texture passed to CompressToJpeg");
        LogToFile(L"Null texture passed to CompressToJpeg");
        return imageData;
    }

    // Create staging texture
    D3D11_TEXTURE2D_DESC stagingDesc;
    pTexture->GetDesc(&stagingDesc);
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    LogToFile(L"Creating staging texture");
    ComPtr<ID3D11Texture2D> stagingTexture;
    hr = g_D3DDevice->CreateTexture2D(&stagingDesc, NULL, &stagingTexture);
    if (FAILED(hr)) {
        LogError(L"Failed to create staging texture", hr);
        LogToFile(L"Failed to create staging texture with HRESULT " + std::to_wstring(hr));
        return imageData;
    }

    // Copy to staging texture
    LogToFile(L"Copying resource to staging texture");
    try {
        g_D3DContext->CopyResource(stagingTexture.Get(), pTexture);
    }
    catch (...) {
        LogError(L"Exception during CopyResource");
        LogToFile(L"Exception caught during CopyResource call");
        return imageData;
    }

    // Map the staging texture
    LogToFile(L"Mapping staging texture");
    D3D11_MAPPED_SUBRESOURCE mappedResource;
    hr = g_D3DContext->Map(stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mappedResource);
    if (FAILED(hr)) {
        LogError(L"Failed to map staging texture", hr);
        LogToFile(L"Failed to map staging texture with HRESULT " + std::to_wstring(hr));
        return imageData;
    }

    // Create WIC factory
    LogToFile(L"Creating WIC factory");
    ComPtr<IWICImagingFactory> wicFactory;
    hr = CoCreateInstance(
        CLSID_WICImagingFactory,
        NULL,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&wicFactory)
    );

    if (FAILED(hr)) {
        g_D3DContext->Unmap(stagingTexture.Get(), 0);
        LogError(L"Failed to create WIC factory", hr);
        LogToFile(L"Failed to create WIC factory with HRESULT " + std::to_wstring(hr));
        return imageData;
    }

    // Create memory stream
    LogToFile(L"Creating memory stream");
    ComPtr<IStream> stream;
    hr = CreateStreamOnHGlobal(NULL, TRUE, &stream);
    if (FAILED(hr)) {
        g_D3DContext->Unmap(stagingTexture.Get(), 0);
        LogError(L"Failed to create memory stream", hr);
        LogToFile(L"Failed to create memory stream with HRESULT " + std::to_wstring(hr));
        return imageData;
    }

    // Create JPEG encoder
    LogToFile(L"Creating JPEG encoder");
    ComPtr<IWICBitmapEncoder> encoder;
    hr = wicFactory->CreateEncoder(GUID_ContainerFormatJpeg, NULL, &encoder);
    if (FAILED(hr)) {
        g_D3DContext->Unmap(stagingTexture.Get(), 0);
        LogError(L"Failed to create JPEG encoder", hr);
        LogToFile(L"Failed to create JPEG encoder with HRESULT " + std::to_wstring(hr));
        return imageData;
    }

    // Initialize encoder
    LogToFile(L"Initializing encoder");
    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) {
        g_D3DContext->Unmap(stagingTexture.Get(), 0);
        LogError(L"Failed to initialize encoder", hr);
        LogToFile(L"Failed to initialize encoder with HRESULT " + std::to_wstring(hr));
        return imageData;
    }

    // Create frame
    LogToFile(L"Creating frame");
    ComPtr<IWICBitmapFrameEncode> frameEncode;
    hr = encoder->CreateNewFrame(&frameEncode, NULL);
    if (FAILED(hr)) {
        g_D3DContext->Unmap(stagingTexture.Get(), 0);
        LogError(L"Failed to create frame", hr);
        LogToFile(L"Failed to create frame with HRESULT " + std::to_wstring(hr));
        return imageData;
    }

    // Initialize frame
    LogToFile(L"Initializing frame");
    hr = frameEncode->Initialize(NULL);
    if (FAILED(hr)) {
        g_D3DContext->Unmap(stagingTexture.Get(), 0);
        LogError(L"Failed to initialize frame", hr);
        LogToFile(L"Failed to initialize frame with HRESULT " + std::to_wstring(hr));
        return imageData;
    }

    // Set frame size
    LogToFile(L"Setting frame size to " + std::to_wstring(width) + L"x" + std::to_wstring(height));
    hr = frameEncode->SetSize(width, height);
    if (FAILED(hr)) {
        g_D3DContext->Unmap(stagingTexture.Get(), 0);
        LogError(L"Failed to set frame size", hr);
        LogToFile(L"Failed to set frame size with HRESULT " + std::to_wstring(hr));
        return imageData;
    }

    // Set pixel format
    LogToFile(L"Setting pixel format to 32bppBGRA");
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    hr = frameEncode->SetPixelFormat(&format);
    if (FAILED(hr)) {
        g_D3DContext->Unmap(stagingTexture.Get(), 0);
        LogError(L"Failed to set pixel format", hr);
        LogToFile(L"Failed to set pixel format with HRESULT " + std::to_wstring(hr));
        return imageData;
    }

    // Verify that the pixel data size is valid
    UINT rowPitch = mappedResource.RowPitch;
    UINT totalSize = rowPitch * height;
    if (totalSize == 0 || rowPitch == 0) {
        g_D3DContext->Unmap(stagingTexture.Get(), 0);
        LogError(L"Invalid pixel data size");
        LogToFile(L"Invalid pixel data size: rowPitch=" + std::to_wstring(rowPitch) + L", totalSize=" + std::to_wstring(totalSize));
        return imageData;
    }

    // Log pixel data details
    LogToFile(L"Pixel data: rowPitch=" + std::to_wstring(rowPitch) + L", totalSize=" + std::to_wstring(totalSize));
    LogToFile(L"pData=" + std::to_wstring((UINT64)mappedResource.pData));

    // Write pixels
    LogToFile(L"Writing pixels to frame");
    hr = frameEncode->WritePixels(
        height,
        mappedResource.RowPitch,
        mappedResource.RowPitch * height,
        static_cast<BYTE*>(mappedResource.pData)
    );

    // Unmap the texture
    LogToFile(L"Unmapping staging texture");
    g_D3DContext->Unmap(stagingTexture.Get(), 0);

    if (FAILED(hr)) {
        LogError(L"Failed to write pixels", hr);
        LogToFile(L"Failed to write pixels with HRESULT " + std::to_wstring(hr));
        return imageData;
    }

    // Commit frame
    LogToFile(L"Committing frame");
    hr = frameEncode->Commit();
    if (FAILED(hr)) {
        LogError(L"Failed to commit frame", hr);
        LogToFile(L"Failed to commit frame with HRESULT " + std::to_wstring(hr));
        return imageData;
    }

    // Commit encoder
    LogToFile(L"Committing encoder");
    hr = encoder->Commit();
    if (FAILED(hr)) {
        LogError(L"Failed to commit encoder", hr);
        LogToFile(L"Failed to commit encoder with HRESULT " + std::to_wstring(hr));
        return imageData;
    }

    // Get data from stream
    LogToFile(L"Getting stream statistics");
    STATSTG stat;
    hr = stream->Stat(&stat, STATFLAG_NONAME);
    if (FAILED(hr)) {
        LogError(L"Failed to get stream stats", hr);
        LogToFile(L"Failed to get stream stats with HRESULT " + std::to_wstring(hr));
        return imageData;
    }

    // Resize vector
    LogToFile(L"Resizing image data buffer to " + std::to_wstring(stat.cbSize.QuadPart) + L" bytes");
    try {
        imageData.resize(static_cast<size_t>(stat.cbSize.QuadPart));
    }
    catch (const std::exception& ex) {
        LogError(L"Failed to resize image data buffer");
        LogToFile(L"Failed to resize image data buffer: exception caught");
        return imageData;
    }

    // Seek to beginning
    LogToFile(L"Seeking to start of stream");
    LARGE_INTEGER seekPos = { 0 };
    hr = stream->Seek(seekPos, STREAM_SEEK_SET, NULL);
    if (FAILED(hr)) {
        LogError(L"Failed to seek stream", hr);
        LogToFile(L"Failed to seek stream with HRESULT " + std::to_wstring(hr));
        imageData.clear();
        return imageData;
    }

    // Read data
    if (imageData.empty()) {
        LogToFile(L"Image data buffer is empty, cannot read from stream");
        return imageData;
    }

    LogToFile(L"Reading data from stream");
    ULONG bytesRead;
    hr = stream->Read(imageData.data(), static_cast<ULONG>(imageData.size()), &bytesRead);
    if (FAILED(hr) || bytesRead != imageData.size()) {
        LogError(L"Failed to read stream data", hr);
        LogToFile(L"Failed to read stream data with HRESULT " + std::to_wstring(hr) + 
                 L", bytesRead=" + std::to_wstring(bytesRead) + 
                 L", expected=" + std::to_wstring(imageData.size()));
        imageData.clear();
        return imageData;
    }

    LogToFile(L"CompressToJpeg completed successfully, returned " + std::to_wstring(imageData.size()) + L" bytes");
    return imageData;
}

// Save image data to a file
bool SaveImageToFile(const std::vector<BYTE>& imageData, const std::wstring& filename) {
    LogToFile(L"SaveImageToFile called for file: " + filename);
    
    if (imageData.empty()) {
        LogError(L"Empty image data in SaveImageToFile");
        LogToFile(L"Empty image data in SaveImageToFile");
        return false;
    }

    // Create directory if it doesn't exist
    try {
        std::filesystem::path filePath(filename);
        std::filesystem::create_directories(filePath.parent_path());
        LogToFile(L"Directory created/verified: " + filePath.parent_path().wstring());
    }
    catch (const std::exception& ex) {
        LogError(L"Failed to create directory for file");
        LogToFile(L"Failed to create directory for file: exception caught");
    }

    // Create and open the file
    LogToFile(L"Creating file: " + filename);
    HANDLE hFile = CreateFileW(
        filename.c_str(),
        GENERIC_WRITE,
        0,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (hFile == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        LogError(L"Failed to create file", error);
        LogToFile(L"Failed to create file with error code " + std::to_wstring(error));
        return false;
    }

    // Write the data
    LogToFile(L"Writing " + std::to_wstring(imageData.size()) + L" bytes to file");
    DWORD bytesWritten;
    BOOL result = WriteFile(
        hFile,
        imageData.data(),
        static_cast<DWORD>(imageData.size()),
        &bytesWritten,
        NULL
    );

    // Close the file
    LogToFile(L"Closing file handle");
    CloseHandle(hFile);

    if (!result) {
        DWORD error = GetLastError();
        LogError(L"Failed to write to file", error);
        LogToFile(L"Failed to write to file with error code " + std::to_wstring(error));
        return false;
    }
    else if (bytesWritten != imageData.size()) {
        LogError(L"Incomplete write to file");
        LogToFile(L"Incomplete write to file: wrote " + std::to_wstring(bytesWritten) + 
                 L" bytes, expected " + std::to_wstring(imageData.size()));
        return false;
    }

    LogToFile(L"File saved successfully");
    return true;
}

// Write image data to a named pipe
bool WriteImageToPipe(const std::vector<BYTE>& imageData, int monitorIndex) {
    // Don't log detailed info for named pipes to avoid filling the log file
    if (imageData.empty()) {
        return false;
    }

    // Create the pipe name
    std::wstring pipeName = PIPE_BASE_PATH + std::to_wstring(monitorIndex);
    LogToFile(L"WriteImageToPipe called for pipe: " + pipeName);

    // Try to open an existing pipe first
    HANDLE hPipe = CreateFileW(
        pipeName.c_str(),
        GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL
    );

    // If pipe doesn't exist or can't be opened, create a new one
    if (hPipe == INVALID_HANDLE_VALUE) {
        LogToFile(L"Existing pipe not found, creating new pipe");
        
        // Create a new named pipe
        hPipe = CreateNamedPipeW(
            pipeName.c_str(),
            PIPE_ACCESS_OUTBOUND,
            PIPE_TYPE_BYTE | PIPE_WAIT,
            1,
            65536,  // Output buffer size
            0,
            NMPWAIT_USE_DEFAULT_WAIT,
            NULL
        );

        if (hPipe == INVALID_HANDLE_VALUE) {
            DWORD error = GetLastError();
            LogError(L"Failed to create named pipe", error);
            LogToFile(L"Failed to create named pipe with error code " + std::to_wstring(error));
            return false;
        }

        // Try to connect to the pipe, with a timeout
        BOOL pipeConnected = FALSE;
        
        // Try ConnectNamedPipe, but don't wait too long
        OVERLAPPED overlapped = {0};
        overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
        
        if (overlapped.hEvent == NULL) {
            DWORD error = GetLastError();
            LogError(L"Failed to create pipe connection event", error);
            CloseHandle(hPipe);
            return false;
        }
        
        // Start connection
        if (ConnectNamedPipe(hPipe, &overlapped)) {
            // Immediate connection
            pipeConnected = TRUE;
            LogToFile(L"Pipe connected immediately");
        } 
        else {
            DWORD error = GetLastError();
            
            if (error == ERROR_PIPE_CONNECTED) {
                // Client already connected
                pipeConnected = TRUE;
                LogToFile(L"Client already connected to pipe");
            }
            else if (error == ERROR_IO_PENDING) {
                // Wait for connection with timeout
                LogToFile(L"Waiting for client to connect to pipe");
                DWORD waitResult = WaitForSingleObject(overlapped.hEvent, 100); // 100ms timeout
                
                if (waitResult == WAIT_OBJECT_0) {
                    // Connection established
                    pipeConnected = TRUE;
                    LogToFile(L"Client connected to pipe");
                }
                else {
                    // Connection timeout
                    LogToFile(L"Timeout waiting for client to connect to pipe");
                    CancelIo(hPipe);
                    CloseHandle(overlapped.hEvent);
                    CloseHandle(hPipe);
                    return false;
                }
            }
            else {
                // Failed to start connection
                LogError(L"Failed to connect to named pipe", error);
                CloseHandle(overlapped.hEvent);
                CloseHandle(hPipe);
                return false;
            }
        }
        
        CloseHandle(overlapped.hEvent);
        
        if (!pipeConnected) {
            LogToFile(L"Failed to connect to pipe");
            CloseHandle(hPipe);
            return false;
        }
    }
    else {
        LogToFile(L"Connected to existing pipe");
    }

    // Write the data
    LogToFile(L"Writing " + std::to_wstring(imageData.size()) + L" bytes to pipe");
    DWORD bytesWritten;
    BOOL result = WriteFile(
        hPipe,
        imageData.data(),
        static_cast<DWORD>(imageData.size()),
        &bytesWritten,
        NULL
    );

    // Flush and close the pipe
    LogToFile(L"Flushing and closing pipe");
    FlushFileBuffers(hPipe);
    DisconnectNamedPipe(hPipe);
    CloseHandle(hPipe);

    if (!result) {
        DWORD error = GetLastError();
        LogError(L"Failed to write to pipe", error);
        LogToFile(L"Failed to write to pipe with error code " + std::to_wstring(error));
        return false;
    }
    else if (bytesWritten != imageData.size()) {
        LogError(L"Incomplete write to pipe");
        LogToFile(L"Incomplete write to pipe: wrote " + std::to_wstring(bytesWritten) + 
                 L" bytes, expected " + std::to_wstring(imageData.size()));
        return false;
    }

    LogToFile(L"Data written to pipe successfully");
    return true;
}

// Get a filename with timestamp
std::wstring GetTimestampedFilename() {
    // Get current time
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    localtime_s(&tm, &time);
    
// Format the timestamp
    std::wstringstream ss;
    ss << L"C:/temp/captures/";
    ss << std::setw(4) << std::setfill(L'0') << (tm.tm_year + 1900) << L"-";
    ss << std::setw(2) << std::setfill(L'0') << (tm.tm_mon + 1) << L"-";
    ss << std::setw(2) << std::setfill(L'0') << tm.tm_mday << L"-";
    ss << std::setw(2) << std::setfill(L'0') << tm.tm_hour << L"-";
    ss << std::setw(2) << std::setfill(L'0') << tm.tm_min << L"-";
    ss << std::setw(2) << std::setfill(L'0') << tm.tm_sec << L".jpg";

    return ss.str();
}







        