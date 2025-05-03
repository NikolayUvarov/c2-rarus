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

// Service status and control
SERVICE_STATUS g_ServiceStatus = { 0 };
SERVICE_STATUS_HANDLE g_StatusHandle = NULL;
HANDLE g_ServiceStopEvent = INVALID_HANDLE_VALUE;

// Capture thread and control
std::thread g_CaptureThread;
std::atomic<bool> g_Running(false);
std::mutex g_CaptureMutex;
std::condition_variable g_CaptureCV;

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

// Messages
#define WM_CAPTURE_START (WM_USER + 1)
#define WM_CAPTURE_STOP (WM_USER + 2)

// Entry point
int wmain(int argc, wchar_t* argv[]) {
    // Initialize COM for the main thread
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        return 1;
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
            // Initialize screen capture
            if (!InitializeScreenCapture()) {
                CoUninitialize();
                return 1;
            }

            // Start capturing
            g_Running = true;
            g_CaptureThread = std::thread([]() {
                while (g_Running) {
                    CaptureScreens();
                    std::this_thread::sleep_for(std::chrono::milliseconds(CAPTURE_INTERVAL));
                }
            });

            printf("Press any key to exit...\n");
            getchar();

            // Cleanup
            g_Running = false;
            if (g_CaptureThread.joinable()) {
                g_CaptureThread.join();
            }
            CleanupScreenCapture();
        }
        else {
            // Error with service dispatcher
            wprintf(L"StartServiceCtrlDispatcher failed: %d\n", error);
        }
    }

    CoUninitialize();
    return 0;
}

// Main service function
void WINAPI ServiceMain(DWORD argc, LPWSTR* argv) {
    // Register service control handler
    g_StatusHandle = RegisterServiceCtrlHandlerW(SERVICE_NAME, ServiceCtrlHandler);
    if (g_StatusHandle == NULL) {
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
        return;
    }

    // Create stop event
    g_ServiceStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (g_ServiceStopEvent == NULL) {
        g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        g_ServiceStatus.dwWin32ExitCode = GetLastError();
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        return;
    }

    // Start service worker thread
    HANDLE hThread = CreateThread(NULL, 0, ServiceWorkerThread, NULL, 0, NULL);

    // Wait for service to stop
    WaitForSingleObject(hThread, INFINITE);

    CloseHandle(g_ServiceStopEvent);

    g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
    g_ServiceStatus.dwWin32ExitCode = 0;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
}

// Service control handler
void WINAPI ServiceCtrlHandler(DWORD dwControl) {
    switch (dwControl) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        if (g_ServiceStatus.dwCurrentState != SERVICE_RUNNING) {
            break;
        }

        g_ServiceStatus.dwControlsAccepted = 0;
        g_ServiceStatus.dwCurrentState = SERVICE_STOP_PENDING;
        g_ServiceStatus.dwWin32ExitCode = 0;
        g_ServiceStatus.dwCheckPoint = 4;
        
        if (SetServiceStatus(g_StatusHandle, &g_ServiceStatus) == FALSE) {
            // Log error
        }

        // Signal the service to stop
        SetEvent(g_ServiceStopEvent);
        break;

    default:
        break;
    }
}

// Hidden window class name
#define WINDOW_CLASS_NAME L"RarusScreenCaptureMessageWindow"

// Service worker thread
DWORD WINAPI ServiceWorkerThread(LPVOID lpParam) {
    // Initialize COM
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        g_ServiceStatus.dwWin32ExitCode = hr;
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        return 0;
    }

    // Create the capture directory if it doesn't exist
    fs::create_directories("C:/temp/captures");

    // Register window class for message window
    WNDCLASSEXW wx = {};
    wx.cbSize = sizeof(WNDCLASSEXW);
    wx.lpfnWndProc = WndProc;
    wx.hInstance = GetModuleHandle(NULL);
    wx.lpszClassName = WINDOW_CLASS_NAME;
    
    if (!RegisterClassExW(&wx)) {
        CoUninitialize();
        g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        g_ServiceStatus.dwWin32ExitCode = GetLastError();
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        return 0;
    }

    // Create message window
    g_MessageWindow = CreateWindowExW(0, WINDOW_CLASS_NAME, L"RarusScreenCaptureMessageWindow",
        0, 0, 0, 0, 0, HWND_MESSAGE, NULL, GetModuleHandle(NULL), NULL);

    if (g_MessageWindow == NULL) {
        CoUninitialize();
        g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        g_ServiceStatus.dwWin32ExitCode = GetLastError();
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        return 0;
    }

    // Initialize screen capture
    if (!InitializeScreenCapture()) {
        if (g_MessageWindow) {
            DestroyWindow(g_MessageWindow);
            g_MessageWindow = NULL;
        }
        UnregisterClass(WINDOW_CLASS_NAME, GetModuleHandle(NULL));
        CoUninitialize();
        g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        g_ServiceStatus.dwWin32ExitCode = 1;
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        return 0;
    }

    // Service is now running
    g_ServiceStatus.dwCurrentState = SERVICE_RUNNING;
    g_ServiceStatus.dwCheckPoint = 0;
    g_ServiceStatus.dwWaitHint = 0;
    
    if (SetServiceStatus(g_StatusHandle, &g_ServiceStatus) == FALSE) {
        CleanupScreenCapture();
        if (g_MessageWindow) {
            DestroyWindow(g_MessageWindow);
            g_MessageWindow = NULL;
        }
        UnregisterClass(WINDOW_CLASS_NAME, GetModuleHandle(NULL));
        CoUninitialize();
        return 0;
    }

    // Start capturing
    g_Running = true;
    g_CaptureThread = std::thread([]() {
        while (g_Running) {
            CaptureScreens();
            std::this_thread::sleep_for(std::chrono::milliseconds(CAPTURE_INTERVAL));
        }
    });

    // Message loop
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Wait for stop event
    WaitForSingleObject(g_ServiceStopEvent, INFINITE);

    // Stop capture thread
    g_Running = false;
    if (g_CaptureThread.joinable()) {
        g_CaptureThread.join();
    }

    // Clean up resources
    CleanupScreenCapture();
    
    if (g_MessageWindow) {
        DestroyWindow(g_MessageWindow);
        g_MessageWindow = NULL;
    }
    
    UnregisterClassW(WINDOW_CLASS_NAME, GetModuleHandle(NULL));
    CoUninitialize();

    return 0;
}

// Window procedure for the message window
LRESULT CALLBACK WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_CAPTURE_START:
        if (!g_Running) {
            g_Running = true;
            g_CaptureThread = std::thread([]() {
                while (g_Running) {
                    CaptureScreens();
                    std::this_thread::sleep_for(std::chrono::milliseconds(CAPTURE_INTERVAL));
                }
            });
        }
        return 0;

    case WM_CAPTURE_STOP:
        if (g_Running) {
            g_Running = false;
            if (g_CaptureThread.joinable()) {
                g_CaptureThread.join();
            }
        }
        return 0;

    default:
        return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
}

// Initialize screen capture with DXGI Desktop Duplication
bool InitializeScreenCapture() {
    HRESULT hr;

    // Create D3D device
    D3D_FEATURE_LEVEL featureLevel;
    hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        g_D3DDevice.GetAddressOf(),
        &featureLevel,
        g_D3DContext.GetAddressOf()
    );

    if (FAILED(hr)) {
        return false;
    }

    // Get DXGI device
    ComPtr<IDXGIDevice> dxgiDevice;
    hr = g_D3DDevice.As(&dxgiDevice);
    if (FAILED(hr)) {
        return false;
    }

    // Get DXGI adapter
    ComPtr<IDXGIAdapter> dxgiAdapter;
    hr = dxgiDevice->GetAdapter(&dxgiAdapter);
    if (FAILED(hr)) {
        return false;
    }

    // Enumerate outputs (monitors)
    UINT i = 0;
    ComPtr<IDXGIOutput> currentOutput;
    
    while (dxgiAdapter->EnumOutputs(i, &currentOutput) != DXGI_ERROR_NOT_FOUND) {
        // Get output description
        DXGI_OUTPUT_DESC outputDesc;
        hr = currentOutput->GetDesc(&outputDesc);
        if (SUCCEEDED(hr)) {
            g_MonitorInfo.push_back(outputDesc);

            // Query for Output1
            ComPtr<IDXGIOutput1> output1;
            hr = currentOutput.As(&output1);
            if (SUCCEEDED(hr)) {
                // Create duplication interface
                ComPtr<IDXGIOutputDuplication> duplication;
                hr = output1->DuplicateOutput(g_D3DDevice.Get(), &duplication);
                if (SUCCEEDED(hr)) {
                    g_DuplicationInterfaces.push_back(duplication);
                }
            }
        }

        currentOutput = nullptr;
        i++;
    }

    // Make sure we found at least one monitor
    if (g_DuplicationInterfaces.empty()) {
        return false;
    }

    return true;
}

// Cleanup screen capture resources
void CleanupScreenCapture() {
    // Release duplication interfaces
    g_DuplicationInterfaces.clear();
    g_MonitorInfo.clear();

    // Release D3D resources
    g_D3DContext = nullptr;
    g_D3DDevice = nullptr;
}

// Capture all screens
bool CaptureScreens() {
    if (g_DuplicationInterfaces.empty()) {
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
            ComPtr<ID3D11Texture2D> desktopTexture;
            hr = desktopResource->QueryInterface(IID_PPV_ARGS(&desktopTexture));
            
            if (SUCCEEDED(hr)) {
                // Store texture and frame info
                monitorTextures.push_back(desktopTexture);
                frameInfos.push_back(frameInfo);
            }
            
            // Release the frame
            g_DuplicationInterfaces[i]->ReleaseFrame();
        }
    }

    // If we didn't capture any frames, return
    if (monitorTextures.empty()) {
        return false;
    }

    // Process each captured monitor
    for (size_t i = 0; i < monitorTextures.size(); i++) {
        // Get texture description
        D3D11_TEXTURE2D_DESC textureDesc;
        monitorTextures[i]->GetDesc(&textureDesc);

        // Compress the texture to JPEG
        std::vector<BYTE> imageData = CompressToJpeg(monitorTextures[i].Get(), textureDesc.Width, textureDesc.Height);
        
        if (!imageData.empty()) {
            // Write to named pipe for this monitor
            WriteImageToPipe(imageData, static_cast<int>(i + 1)); // +1 because 0 is reserved for all screens
        }
    }

    // Create a combined image from all monitors if we have multiple monitors
    if (monitorTextures.size() > 1) {
        // Calculate total width and height
        int totalWidth = 0;
        int maxHeight = 0;

        for (size_t i = 0; i < g_MonitorInfo.size(); i++) {
            RECT rect = g_MonitorInfo[i].DesktopCoordinates;
            totalWidth += (rect.right - rect.left);
            maxHeight = max(maxHeight, rect.bottom - rect.top);
        }

        // Create a texture for the combined image
        D3D11_TEXTURE2D_DESC combinedDesc = {};
        combinedDesc.Width = totalWidth;
        combinedDesc.Height = maxHeight;
        combinedDesc.MipLevels = 1;
        combinedDesc.ArraySize = 1;
        combinedDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        combinedDesc.SampleDesc.Count = 1;
        combinedDesc.Usage = D3D11_USAGE_DEFAULT;
        combinedDesc.BindFlags = D3D11_BIND_RENDER_TARGET;

        ComPtr<ID3D11Texture2D> combinedTexture;
        hr = g_D3DDevice->CreateTexture2D(&combinedDesc, nullptr, &combinedTexture);

        if (SUCCEEDED(hr)) {
            // TODO: Copy each monitor texture to the combined texture
            // This would require additional rendering steps with proper coordinates
            
            // For now, just use the first monitor
            if (!monitorTextures.empty()) {
                D3D11_TEXTURE2D_DESC firstDesc;
                monitorTextures[0]->GetDesc(&firstDesc);
                
                std::vector<BYTE> combinedImageData = CompressToJpeg(monitorTextures[0].Get(), firstDesc.Width, firstDesc.Height);
                
                if (!combinedImageData.empty()) {
                    // Write to named pipe for all screens
                    WriteImageToPipe(combinedImageData, 0);
                    
                    // Save to file with timestamp
                    std::wstring filename = GetTimestampedFilename();
                    SaveImageToFile(combinedImageData, filename);
                }
            }
        }
    }
    else if (monitorTextures.size() == 1) {
        // If we only have one monitor, use its image for both the named pipe and the file
        D3D11_TEXTURE2D_DESC desc;
        monitorTextures[0]->GetDesc(&desc);
        
        std::vector<BYTE> imageData = CompressToJpeg(monitorTextures[0].Get(), desc.Width, desc.Height);
        
        if (!imageData.empty()) {
            // Write to named pipe for all screens
            WriteImageToPipe(imageData, 0);
            
            // Save to file with timestamp
            std::wstring filename = GetTimestampedFilename();
            SaveImageToFile(imageData, filename);
        }
    }

    return true;
}

// Compress a texture to JPEG
std::vector<BYTE> CompressToJpeg(ID3D11Texture2D* pTexture, UINT width, UINT height) {
    std::vector<BYTE> imageData;
    HRESULT hr;

    // Create staging texture
    D3D11_TEXTURE2D_DESC stagingDesc;
    pTexture->GetDesc(&stagingDesc);
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> stagingTexture;
    hr = g_D3DDevice->CreateTexture2D(&stagingDesc, NULL, &stagingTexture);
    if (FAILED(hr)) {
        return imageData;
    }

    // Copy to staging texture
    g_D3DContext->CopyResource(stagingTexture.Get(), pTexture);

    // Map the staging texture
    D3D11_MAPPED_SUBRESOURCE mappedResource;
    hr = g_D3DContext->Map(stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mappedResource);
    if (FAILED(hr)) {
        return imageData;
    }

    // Create WIC factory
    ComPtr<IWICImagingFactory> wicFactory;
    hr = CoCreateInstance(
        CLSID_WICImagingFactory,
        NULL,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&wicFactory)
    );

    if (FAILED(hr)) {
        g_D3DContext->Unmap(stagingTexture.Get(), 0);
        return imageData;
    }

    // Create memory stream
    ComPtr<IStream> stream;
    hr = CreateStreamOnHGlobal(NULL, TRUE, &stream);
    if (FAILED(hr)) {
        g_D3DContext->Unmap(stagingTexture.Get(), 0);
        return imageData;
    }

    // Create JPEG encoder
    ComPtr<IWICBitmapEncoder> encoder;
    hr = wicFactory->CreateEncoder(GUID_ContainerFormatJpeg, NULL, &encoder);
    if (FAILED(hr)) {
        g_D3DContext->Unmap(stagingTexture.Get(), 0);
        return imageData;
    }

    // Initialize encoder
    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) {
        g_D3DContext->Unmap(stagingTexture.Get(), 0);
        return imageData;
    }

    // Create frame
    ComPtr<IWICBitmapFrameEncode> frameEncode;
    hr = encoder->CreateNewFrame(&frameEncode, NULL);
    if (FAILED(hr)) {
        g_D3DContext->Unmap(stagingTexture.Get(), 0);
        return imageData;
    }

    // Initialize frame
    hr = frameEncode->Initialize(NULL);
    if (FAILED(hr)) {
        g_D3DContext->Unmap(stagingTexture.Get(), 0);
        return imageData;
    }

    // Set frame size
    hr = frameEncode->SetSize(width, height);
    if (FAILED(hr)) {
        g_D3DContext->Unmap(stagingTexture.Get(), 0);
        return imageData;
    }

    // Set pixel format
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    hr = frameEncode->SetPixelFormat(&format);
    if (FAILED(hr)) {
        g_D3DContext->Unmap(stagingTexture.Get(), 0);
        return imageData;
    }

    // Write pixels
    hr = frameEncode->WritePixels(
        height,
        mappedResource.RowPitch,
        mappedResource.RowPitch * height,
        static_cast<BYTE*>(mappedResource.pData)
    );

    // Unmap the texture
    g_D3DContext->Unmap(stagingTexture.Get(), 0);

    if (FAILED(hr)) {
        return imageData;
    }

    // Commit frame
    hr = frameEncode->Commit();
    if (FAILED(hr)) {
        return imageData;
    }

    // Commit encoder
    hr = encoder->Commit();
    if (FAILED(hr)) {
        return imageData;
    }

    // Get data from stream
    STATSTG stat;
    hr = stream->Stat(&stat, STATFLAG_NONAME);
    if (FAILED(hr)) {
        return imageData;
    }

    // Resize vector
    imageData.resize(static_cast<size_t>(stat.cbSize.QuadPart));

    // Seek to beginning
    LARGE_INTEGER seekPos = { 0 };
    hr = stream->Seek(seekPos, STREAM_SEEK_SET, NULL);
    if (FAILED(hr)) {
        imageData.clear();
        return imageData;
    }

    // Read data
    ULONG bytesRead;
    hr = stream->Read(imageData.data(), static_cast<ULONG>(imageData.size()), &bytesRead);
    if (FAILED(hr) || bytesRead != imageData.size()) {
        imageData.clear();
        return imageData;
    }

    return imageData;
}

// Save image data to a file
bool SaveImageToFile(const std::vector<BYTE>& imageData, const std::wstring& filename) {
    if (imageData.empty()) {
        return false;
    }

    // Create and open the file
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
        return false;
    }

    // Write the data
    DWORD bytesWritten;
    BOOL result = WriteFile(
        hFile,
        imageData.data(),
        static_cast<DWORD>(imageData.size()),
        &bytesWritten,
        NULL
    );

    // Close the file
    CloseHandle(hFile);

    return result && (bytesWritten == imageData.size());
}

// Write image data to a named pipe
bool WriteImageToPipe(const std::vector<BYTE>& imageData, int monitorIndex) {
    if (imageData.empty()) {
        return false;
    }

    // Create the pipe name
    std::wstring pipeName = PIPE_BASE_PATH + std::to_wstring(monitorIndex);

    // Open or create the named pipe
    HANDLE hPipe = CreateNamedPipeW(
        pipeName.c_str(),
        PIPE_ACCESS_OUTBOUND,
        PIPE_TYPE_BYTE | PIPE_WAIT,
        1,
        0,
        0,
        NMPWAIT_USE_DEFAULT_WAIT,
        NULL
    );

    if (hPipe == INVALID_HANDLE_VALUE) {
        return false;
    }

    // Connect to the pipe
    if (ConnectNamedPipe(hPipe, NULL) || GetLastError() == ERROR_PIPE_CONNECTED) {
        // Write the data
        DWORD bytesWritten;
        BOOL result = WriteFile(
            hPipe,
            imageData.data(),
            static_cast<DWORD>(imageData.size()),
            &bytesWritten,
            NULL
        );

        // Disconnect and close the pipe
        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);

        return result && (bytesWritten == imageData.size());
    }
    else {
        // Failed to connect
        CloseHandle(hPipe);
        return false;
    }
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

