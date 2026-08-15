#include "browser_overlay.h"

#include "common/utils.h"
#include "render/render.h"
#include "third_party/stb_image.h"

#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>
#include <wrl.h>
#include <array>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <objidl.h>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

using Microsoft::WRL::ComPtr;

extern HMODULE g_hModule;

namespace {

constexpr wchar_t kBrowserOverlayHostClassName[] = L"ToolscreenBrowserOverlayHostWindow";
constexpr int kBrowserOverlayOffscreenPos = -32000;
constexpr size_t kBrowserOverlayMaxFrameBytes = 100ull * 1024ull * 1024ull;
constexpr size_t kBrowserOverlayUploadPboCount = 3;
constexpr size_t kBrowserOverlayEnvironmentCount = 4;

struct BrowserOverlayEnvironmentState {
    ComPtr<ICoreWebView2Environment> environment;
    std::atomic<bool> requested{ false };
    std::atomic<bool> ready{ false };
    std::atomic<bool> failed{ false };
};

struct BrowserOverlayRenderData {
    unsigned char* pixelData = nullptr;
    int width = 0;
    int height = 0;

    BrowserOverlayRenderData() = default;
    ~BrowserOverlayRenderData() {
        if (pixelData) {
            delete[] pixelData;
            pixelData = nullptr;
        }
    }

    BrowserOverlayRenderData(const BrowserOverlayRenderData&) = delete;
    BrowserOverlayRenderData& operator=(const BrowserOverlayRenderData&) = delete;
};

struct BrowserOverlayCacheEntry {
    std::string name;
    std::string url;
    std::string customCss;
    int browserWidth = 1280;
    int browserHeight = 720;
    int fps = 15;
    bool transparentBackground = false;
    bool muteAudio = true;
    bool hardwareAcceleration = true;
    bool allowSystemMediaKeys = true;
    bool reloadOnUpdate = false;
    int reloadInterval = 0;
    bool markedForRemoval = false;

    HWND hostWindow = nullptr;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    EventRegistrationToken contentLoadingToken{};
    EventRegistrationToken navigationCompletedToken{};
    EventRegistrationToken processFailedToken{};
    bool hasSeenContentLoading = false;
    bool captureInFlight = false;
    bool controllerCreationPending = false;
    bool controllerCreationFailed = false;
    bool pendingReload = false;
    uint64_t latestQueuedDecodeSequence = 0;
    std::chrono::steady_clock::time_point lastCaptureTime{};
    std::chrono::steady_clock::time_point lastReloadTime{};

    std::unique_ptr<BrowserOverlayRenderData> writeBuffer = std::make_unique<BrowserOverlayRenderData>();
    std::unique_ptr<BrowserOverlayRenderData> readyBuffer = std::make_unique<BrowserOverlayRenderData>();
    std::unique_ptr<BrowserOverlayRenderData> backBuffer = std::make_unique<BrowserOverlayRenderData>();
    std::atomic<bool> hasNewFrame{ false };
    std::mutex swapMutex;
    std::mutex renderStateMutex;

    GLuint glTextureId = 0;
    int glTextureWidth = 0;
    int glTextureHeight = 0;
    std::array<GLuint, kBrowserOverlayUploadPboCount> uploadPbos{};
    size_t nextUploadPboIndex = 0;
    BrowserOverlayRenderData* lastUploadedRenderData = nullptr;
    BrowserOverlayRenderData* lastPublishedRenderData = nullptr;
    std::shared_ptr<const std::vector<unsigned char>> publishedPixels;
    uint64_t publishedGeneration = 0;
    bool filterInitialized = false;
    bool lastPixelatedScaling = false;
};

struct BrowserOverlayPendingGlCleanup {
    GLuint textureId = 0;
    std::array<GLuint, kBrowserOverlayUploadPboCount> uploadPbos{};
};

struct BrowserOverlayEncodedFrame {
    std::string overlayId;
    uint64_t sequence = 0;
    std::vector<stbi_uc> encodedBytes;
};

struct PixelStoreStateGuard {
    GLint unpackAlignment = 4;
    GLint unpackRowLength = 0;
    GLint unpackSkipRows = 0;
    GLint unpackSkipPixels = 0;

    PixelStoreStateGuard() {
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpackAlignment);
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &unpackRowLength);
        glGetIntegerv(GL_UNPACK_SKIP_ROWS, &unpackSkipRows);
        glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &unpackSkipPixels);
    }

    ~PixelStoreStateGuard() {
        glPixelStorei(GL_UNPACK_ALIGNMENT, unpackAlignment);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, unpackRowLength);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, unpackSkipRows);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, unpackSkipPixels);
    }
};

std::map<std::string, std::shared_ptr<BrowserOverlayCacheEntry>> g_browserOverlayCache;
std::mutex g_browserOverlayCacheMutex;

std::vector<BrowserOverlayPendingGlCleanup> g_browserOverlayPendingGlCleanup;
std::mutex g_browserOverlayPendingGlCleanupMutex;

std::atomic<bool> g_stopBrowserOverlayThread{ false };
std::thread g_browserOverlayThread;
std::thread g_browserOverlayDecodeThread;

std::map<std::string, BrowserOverlayEncodedFrame> g_browserOverlayPendingDecodeFrames;
std::mutex g_browserOverlayDecodeMutex;
std::condition_variable g_browserOverlayDecodeCv;

std::array<BrowserOverlayEnvironmentState, kBrowserOverlayEnvironmentCount> g_browserOverlayEnvironments;
std::atomic<bool> g_browserOverlayHostClassRegistered{ false };

void LogBrowserOverlayMessage(const std::string& message);

LRESULT CALLBACK BrowserOverlayHostWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

bool EnsureBrowserOverlayHostClass() {
    if (g_browserOverlayHostClassRegistered.load(std::memory_order_acquire)) {
        return true;
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = BrowserOverlayHostWndProc;
    wc.hInstance = g_hModule ? g_hModule : GetModuleHandleW(nullptr);
    wc.lpszClassName = kBrowserOverlayHostClassName;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);

    if (!RegisterClassExW(&wc)) {
        const DWORD lastError = GetLastError();
        if (lastError != ERROR_CLASS_ALREADY_EXISTS) {
            LogBrowserOverlayMessage("[BrowserOverlay] Failed to register host window class: " + std::to_string(lastError));
            return false;
        }
    }

    g_browserOverlayHostClassRegistered.store(true, std::memory_order_release);
    return true;
}

std::wstring GetBrowserOverlayUserDataFolder() {
    std::filesystem::path basePath;
    if (!g_toolscreenPath.empty()) {
        basePath = std::filesystem::path(g_toolscreenPath);
    } else {
        basePath = std::filesystem::temp_directory_path() / L"Toolscreen";
    }

    const std::filesystem::path userDataPath = basePath / L"browser-overlay-webview2";
    std::error_code ec;
    std::filesystem::create_directories(userDataPath, ec);
    return userDataPath.wstring();
}

size_t GetBrowserOverlayEnvironmentIndex(bool allowSystemMediaKeys, bool hardwareAcceleration) {
    size_t index = allowSystemMediaKeys ? 1u : 0u;
    if (hardwareAcceleration) {
        index += 2u;
    }
    return index;
}

std::wstring GetBrowserOverlayEnvironmentModeName(bool allowSystemMediaKeys, bool hardwareAcceleration) {
    return std::wstring(allowSystemMediaKeys ? L"media-keys-enabled" : L"media-keys-disabled") +
           (hardwareAcceleration ? L"_gpu-enabled" : L"_gpu-disabled");
}

std::string GetBrowserOverlayEnvironmentModeLabel(bool allowSystemMediaKeys, bool hardwareAcceleration) {
    return std::string(allowSystemMediaKeys ? "system media keys enabled" : "system media keys disabled") + ", " +
           (hardwareAcceleration ? "hardware acceleration enabled" : "hardware acceleration disabled");
}

void LogBrowserOverlayMessage(const std::string& message) {
    LogCategory("browser_overlay", message);
}

std::wstring GetBrowserOverlayUserDataFolder(bool allowSystemMediaKeys, bool hardwareAcceleration) {
    const std::filesystem::path userDataPath =
        std::filesystem::path(GetBrowserOverlayUserDataFolder()) / GetBrowserOverlayEnvironmentModeName(allowSystemMediaKeys, hardwareAcceleration);
    std::error_code ec;
    std::filesystem::create_directories(userDataPath, ec);
    return userDataPath.wstring();
}

ComPtr<ICoreWebView2EnvironmentOptions> CreateBrowserOverlayEnvironmentOptions(bool allowSystemMediaKeys, bool hardwareAcceleration) {
    std::wstring additionalArgs;
    if (!allowSystemMediaKeys) {
        additionalArgs += L"--disable-features=HardwareMediaKeyHandling";
    }
    if (!hardwareAcceleration) {
        if (!additionalArgs.empty()) {
            additionalArgs += L" ";
        }
        additionalArgs += L"--disable-gpu";
    }
    if (additionalArgs.empty()) {
        return nullptr;
    }

    auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    if (!options) {
        return nullptr;
    }

    options->put_AdditionalBrowserArguments(additionalArgs.c_str());
    return options;
}

void PumpBrowserOverlayMessages() {
    MSG msg = {};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

std::string FormatHexResult(HRESULT value) {
    std::ostringstream stream;
    stream << "0x" << std::hex << static_cast<unsigned long>(value);
    return stream.str();
}

void AppendJavaScriptHexEscape(std::wstring& output, wchar_t value) {
    static constexpr wchar_t kHexDigits[] = L"0123456789ABCDEF";
    output += L"\\u";
    output.push_back(kHexDigits[(value >> 12) & 0xF]);
    output.push_back(kHexDigits[(value >> 8) & 0xF]);
    output.push_back(kHexDigits[(value >> 4) & 0xF]);
    output.push_back(kHexDigits[value & 0xF]);
}

std::wstring EscapeForJavaScriptStringLiteral(const std::wstring& input) {
    std::wstring escaped;
    escaped.reserve(input.size() + 16);
    escaped.push_back(L'\'');

    for (wchar_t ch : input) {
        switch (ch) {
        case L'\\':
            escaped += L"\\\\";
            break;
        case L'\'':
            escaped += L"\\'";
            break;
        case L'\r':
            escaped += L"\\r";
            break;
        case L'\n':
            escaped += L"\\n";
            break;
        case L'\t':
            escaped += L"\\t";
            break;
        default:
            if (ch < 0x20 || ch == 0x2028 || ch == 0x2029) {
                AppendJavaScriptHexEscape(escaped, ch);
            } else {
                escaped.push_back(ch);
            }
            break;
        }
    }

    escaped.push_back(L'\'');
    return escaped;
}

std::wstring BuildBrowserOverlayInjectedCss(const BrowserOverlayCacheEntry& entry) {
    std::wstring css;

    if (entry.transparentBackground) {
        css += L"html, body { background: transparent !important; }\n";
    }

    if (!entry.customCss.empty()) {
        css += Utf8ToWide(entry.customCss);
    }

    return css;
}

void ApplyBrowserOverlayInjectedStyles(BrowserOverlayCacheEntry& entry) {
    if (!entry.webview) {
        return;
    }

    const std::wstring cssLiteral = EscapeForJavaScriptStringLiteral(BuildBrowserOverlayInjectedCss(entry));
    const std::wstring script =
        LR"JS((() => {
    try {
        const styleId = '__toolscreen_browser_overlay_style';
        const cssText = )JS" +
        cssLiteral +
        LR"JS(;
        let style = document.getElementById(styleId);
        if (!cssText) {
            if (style) {
                style.remove();
            }
            return;
        }

        const root = document.head || document.documentElement || document.body;
        if (!root) {
            return;
        }

        if (!style) {
            style = document.createElement('style');
            style.id = styleId;
            root.appendChild(style);
        }

        style.textContent = cssText;
    } catch (e) {
    }
})();)JS";

    entry.webview->ExecuteScript(
        script.c_str(),
        Microsoft::WRL::Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [](HRESULT, PCWSTR) -> HRESULT { return S_OK; })
            .Get());
}

bool EnsureBrowserOverlayEnvironment(bool allowSystemMediaKeys, bool hardwareAcceleration) {
    BrowserOverlayEnvironmentState& environmentState =
        g_browserOverlayEnvironments[GetBrowserOverlayEnvironmentIndex(allowSystemMediaKeys, hardwareAcceleration)];

    if (environmentState.ready.load(std::memory_order_acquire)) {
        return true;
    }
    if (environmentState.failed.load(std::memory_order_acquire)) {
        return false;
    }
    if (environmentState.requested.exchange(true, std::memory_order_acq_rel)) {
        return environmentState.ready.load(std::memory_order_acquire);
    }

    HANDLE completionEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!completionEvent) {
        LogBrowserOverlayMessage("[BrowserOverlay] Failed to create WebView2 environment wait event.");
        environmentState.failed.store(true, std::memory_order_release);
        return false;
    }

    HRESULT createResult = E_FAIL;
    ComPtr<ICoreWebView2Environment> createdEnvironment;
    const std::wstring userDataFolder = GetBrowserOverlayUserDataFolder(allowSystemMediaKeys, hardwareAcceleration);
    ComPtr<ICoreWebView2EnvironmentOptions> environmentOptions =
        CreateBrowserOverlayEnvironmentOptions(allowSystemMediaKeys, hardwareAcceleration);
    if ((!allowSystemMediaKeys || !hardwareAcceleration) && !environmentOptions) {
        LogBrowserOverlayMessage("[BrowserOverlay] Failed to create WebView2 environment options for " +
                                 GetBrowserOverlayEnvironmentModeLabel(allowSystemMediaKeys, hardwareAcceleration));
        CloseHandle(completionEvent);
        environmentState.failed.store(true, std::memory_order_release);
        return false;
    }

    const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr,
        userDataFolder.c_str(),
        environmentOptions.Get(),
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [&createResult, &createdEnvironment, completionEvent](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
                createResult = result;
                createdEnvironment = environment;
                SetEvent(completionEvent);
                return S_OK;
            })
            .Get());

    if (FAILED(hr)) {
        LogBrowserOverlayMessage("[BrowserOverlay] CreateCoreWebView2EnvironmentWithOptions failed: " + FormatHexResult(hr));
        CloseHandle(completionEvent);
        environmentState.failed.store(true, std::memory_order_release);
        return false;
    }

    while (!g_stopBrowserOverlayThread.load(std::memory_order_acquire)) {
        PumpBrowserOverlayMessages();
        const DWORD waitResult = WaitForSingleObject(completionEvent, 10);
        if (waitResult == WAIT_OBJECT_0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    CloseHandle(completionEvent);

    if (g_stopBrowserOverlayThread.load(std::memory_order_acquire)) {
        return false;
    }

    if (FAILED(createResult) || !createdEnvironment) {
        LogBrowserOverlayMessage("[BrowserOverlay] WebView2 environment creation failed: " + FormatHexResult(createResult));
        environmentState.failed.store(true, std::memory_order_release);
        return false;
    }

    environmentState.environment = createdEnvironment;
    environmentState.ready.store(true, std::memory_order_release);
    LogBrowserOverlayMessage("[BrowserOverlay] WebView2 environment initialized (" +
                             GetBrowserOverlayEnvironmentModeLabel(allowSystemMediaKeys, hardwareAcceleration) + ")");
    return true;
}

ICoreWebView2Environment* GetBrowserOverlayEnvironment(bool allowSystemMediaKeys, bool hardwareAcceleration) {
    if (!EnsureBrowserOverlayEnvironment(allowSystemMediaKeys, hardwareAcceleration)) {
        return nullptr;
    }

    return g_browserOverlayEnvironments[GetBrowserOverlayEnvironmentIndex(allowSystemMediaKeys, hardwareAcceleration)].environment.Get();
}

void UpdateBrowserOverlayBuffer(BrowserOverlayCacheEntry& entry, const unsigned char* pixels, int width, int height) {
    if (!pixels || width <= 0 || height <= 0) {
        return;
    }

    if (entry.writeBuffer->width != width || entry.writeBuffer->height != height) {
        if (entry.writeBuffer->pixelData) {
            delete[] entry.writeBuffer->pixelData;
            entry.writeBuffer->pixelData = nullptr;
        }
        entry.writeBuffer->width = width;
        entry.writeBuffer->height = height;
        const size_t bufferSize = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
        if (bufferSize > 0 && bufferSize < kBrowserOverlayMaxFrameBytes) {
            entry.writeBuffer->pixelData = new unsigned char[bufferSize];
        }
    }

    if (!entry.writeBuffer->pixelData) {
        return;
    }

    const size_t copySize = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    memcpy(entry.writeBuffer->pixelData, pixels, copySize);

    {
        std::lock_guard<std::mutex> lock(entry.swapMutex);
        entry.writeBuffer.swap(entry.readyBuffer);
    }

    entry.hasNewFrame.store(true, std::memory_order_release);
}

void EnqueueBrowserOverlayDecode(BrowserOverlayEncodedFrame&& frame) {
    {
        std::lock_guard<std::mutex> lock(g_browserOverlayDecodeMutex);
        auto& slot = g_browserOverlayPendingDecodeFrames[frame.overlayId];
        slot = std::move(frame);
    }
    g_browserOverlayDecodeCv.notify_one();
}

bool CaptureBrowserOverlayHostWindow(BrowserOverlayCacheEntry& entry) {
    if (!entry.hostWindow || !IsWindow(entry.hostWindow) || !IsWindowVisible(entry.hostWindow)) {
        return false;
    }

    RECT clientRect{};
    if (!GetClientRect(entry.hostWindow, &clientRect)) {
        return false;
    }

    const int captureWidth = clientRect.right - clientRect.left;
    const int captureHeight = clientRect.bottom - clientRect.top;
    if (captureWidth <= 0 || captureHeight <= 0) {
        return false;
    }

    if (entry.writeBuffer->width != captureWidth || entry.writeBuffer->height != captureHeight) {
        if (entry.writeBuffer->pixelData) {
            delete[] entry.writeBuffer->pixelData;
            entry.writeBuffer->pixelData = nullptr;
        }

        entry.writeBuffer->width = captureWidth;
        entry.writeBuffer->height = captureHeight;

        const size_t bufferSize = static_cast<size_t>(captureWidth) * static_cast<size_t>(captureHeight) * 4;
        if (bufferSize > 0 && bufferSize < kBrowserOverlayMaxFrameBytes) {
            entry.writeBuffer->pixelData = new unsigned char[bufferSize];
        }
    }

    if (!entry.writeBuffer->pixelData) {
        return false;
    }

    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem = hdcScreen ? CreateCompatibleDC(hdcScreen) : nullptr;
    HDC hdcWindow = GetDC(entry.hostWindow);
    if (!hdcScreen || !hdcMem || !hdcWindow) {
        if (hdcWindow) {
            ReleaseDC(entry.hostWindow, hdcWindow);
        }
        if (hdcMem) {
            DeleteDC(hdcMem);
        }
        if (hdcScreen) {
            ReleaseDC(nullptr, hdcScreen);
        }
        return false;
    }

    HBITMAP hBitmap = CreateCompatibleBitmap(hdcScreen, captureWidth, captureHeight);
    if (!hBitmap) {
        ReleaseDC(entry.hostWindow, hdcWindow);
        DeleteDC(hdcMem);
        ReleaseDC(nullptr, hdcScreen);
        return false;
    }

    HBITMAP oldBitmap = static_cast<HBITMAP>(SelectObject(hdcMem, hBitmap));

    RECT clearRect = { 0, 0, captureWidth, captureHeight };
    FillRect(hdcMem, &clearRect, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

    BOOL captureOk = PrintWindow(entry.hostWindow, hdcMem, PW_RENDERFULLCONTENT);
    if (!captureOk) {
        captureOk = BitBlt(hdcMem, 0, 0, captureWidth, captureHeight, hdcWindow, 0, 0, SRCCOPY | CAPTUREBLT);
    }

    bool success = false;
    if (captureOk) {
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = captureWidth;
        bmi.bmiHeader.biHeight = -captureHeight;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        const int scanlines = GetDIBits(hdcScreen, hBitmap, 0, captureHeight, entry.writeBuffer->pixelData, &bmi, DIB_RGB_COLORS);
        if (scanlines == captureHeight) {
            const size_t totalPixels = static_cast<size_t>(captureWidth) * static_cast<size_t>(captureHeight);
            for (size_t pixelIndex = 0; pixelIndex < totalPixels; ++pixelIndex) {
                unsigned char* pixel = &entry.writeBuffer->pixelData[pixelIndex * 4];
                std::swap(pixel[0], pixel[2]);
                pixel[3] = 255;
            }

            {
                std::lock_guard<std::mutex> lock(entry.swapMutex);
                entry.writeBuffer.swap(entry.readyBuffer);
            }

            entry.hasNewFrame.store(true, std::memory_order_release);
            success = true;
        }
    }

    SelectObject(hdcMem, oldBitmap);
    DeleteObject(hBitmap);
    ReleaseDC(entry.hostWindow, hdcWindow);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);
    return success;
}

void QueueBrowserOverlayGlCleanup(BrowserOverlayCacheEntry& entry) {
    std::lock_guard<std::mutex> renderLock(entry.renderStateMutex);

    BrowserOverlayPendingGlCleanup cleanup{};
    cleanup.textureId = entry.glTextureId;
    cleanup.uploadPbos = entry.uploadPbos;

    entry.glTextureId = 0;
    entry.glTextureWidth = 0;
    entry.glTextureHeight = 0;
    entry.uploadPbos.fill(0);
    entry.nextUploadPboIndex = 0;
    entry.lastUploadedRenderData = nullptr;
    entry.filterInitialized = false;
    entry.lastPixelatedScaling = false;

    bool hasResources = cleanup.textureId != 0;
    if (!hasResources) {
        for (GLuint pboId : cleanup.uploadPbos) {
            if (pboId != 0) {
                hasResources = true;
                break;
            }
        }
    }

    if (!hasResources) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_browserOverlayPendingGlCleanupMutex);
    g_browserOverlayPendingGlCleanup.push_back(cleanup);
}

void FlushBrowserOverlayGlCleanup() {
    if (!wglGetCurrentContext()) {
        return;
    }

    std::vector<BrowserOverlayPendingGlCleanup> pending;
    {
        std::lock_guard<std::mutex> lock(g_browserOverlayPendingGlCleanupMutex);
        if (g_browserOverlayPendingGlCleanup.empty()) {
            return;
        }
        pending.swap(g_browserOverlayPendingGlCleanup);
    }

    for (const BrowserOverlayPendingGlCleanup& cleanup : pending) {
        if (cleanup.textureId != 0) {
            glDeleteTextures(1, &cleanup.textureId);
        }

        GLuint pboIds[kBrowserOverlayUploadPboCount] = {};
        GLsizei pboCount = 0;
        for (GLuint pboId : cleanup.uploadPbos) {
            if (pboId != 0) {
                pboIds[pboCount++] = pboId;
            }
        }
        if (pboCount > 0) {
            glDeleteBuffers(pboCount, pboIds);
        }
    }
}

void UploadBrowserOverlayTexture(BrowserOverlayCacheEntry& entry, const BrowserOverlayRenderData& renderData) {
    PixelStoreStateGuard pixelStoreGuard;
    if (entry.glTextureId == 0) {
        glGenTextures(1, &entry.glTextureId);
        BindTextureDirect(GL_TEXTURE_2D, entry.glTextureId);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        entry.filterInitialized = false;
    }

    BindTextureDirect(GL_TEXTURE_2D, entry.glTextureId);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    const size_t bufferSize = static_cast<size_t>(renderData.width) * static_cast<size_t>(renderData.height) * 4;
    bool uploadedWithPbo = false;
    if (bufferSize > 0 && bufferSize < kBrowserOverlayMaxFrameBytes) {
        const size_t pboIndex = entry.nextUploadPboIndex % kBrowserOverlayUploadPboCount;
        entry.nextUploadPboIndex = (pboIndex + 1) % kBrowserOverlayUploadPboCount;

        GLuint& pboId = entry.uploadPbos[pboIndex];
        if (pboId == 0) {
            glGenBuffers(1, &pboId);
        }

        if (pboId != 0) {
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pboId);
            glBufferData(GL_PIXEL_UNPACK_BUFFER, static_cast<GLsizeiptr>(bufferSize), nullptr, GL_STREAM_DRAW);

            GLbitfield mapFlags = GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT;
#ifdef GL_MAP_UNSYNCHRONIZED_BIT
            mapFlags |= GL_MAP_UNSYNCHRONIZED_BIT;
#endif
            void* mapped = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, static_cast<GLsizeiptr>(bufferSize), mapFlags);
            if (mapped) {
                memcpy(mapped, renderData.pixelData, bufferSize);
                const GLboolean unmapped = glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
                if (unmapped == GL_TRUE) {
                    if (entry.glTextureWidth != renderData.width || entry.glTextureHeight != renderData.height) {
                        entry.glTextureWidth = renderData.width;
                        entry.glTextureHeight = renderData.height;
                        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, renderData.width, renderData.height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                                     nullptr);
                    } else {
                        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, renderData.width, renderData.height, GL_RGBA, GL_UNSIGNED_BYTE,
                                        nullptr);
                    }
                    uploadedWithPbo = true;
                }
            }

            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        }
    }

    if (!uploadedWithPbo) {
        if (entry.glTextureWidth != renderData.width || entry.glTextureHeight != renderData.height) {
            entry.glTextureWidth = renderData.width;
            entry.glTextureHeight = renderData.height;
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, renderData.width, renderData.height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                         renderData.pixelData);
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, renderData.width, renderData.height, GL_RGBA, GL_UNSIGNED_BYTE,
                            renderData.pixelData);
        }
    }

    entry.lastUploadedRenderData = const_cast<BrowserOverlayRenderData*>(&renderData);
}

void NavigateBrowserOverlay(BrowserOverlayCacheEntry& entry) {
    if (!entry.webview) {
        return;
    }

    entry.hasSeenContentLoading = false;
    entry.pendingReload = false;
    entry.webview->Navigate(Utf8ToWide(entry.url).c_str());
    entry.lastReloadTime = std::chrono::steady_clock::now();
}

void UpdateBrowserOverlayBounds(BrowserOverlayCacheEntry& entry) {
    const int width = (std::max)(1, entry.browserWidth);
    const int height = (std::max)(1, entry.browserHeight);

    if (entry.hostWindow) {
        SetWindowPos(entry.hostWindow, HWND_BOTTOM, kBrowserOverlayOffscreenPos, kBrowserOverlayOffscreenPos, width, height,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER | SWP_SHOWWINDOW);
        ShowWindow(entry.hostWindow, SW_SHOWNOACTIVATE);
    }

    if (entry.controller) {
        RECT bounds = { 0, 0, width, height };
        entry.controller->put_Bounds(bounds);
        entry.controller->put_IsVisible(TRUE);
    }
}

void ApplyBrowserOverlayControllerSettings(BrowserOverlayCacheEntry& entry) {
    if (entry.controller) {
        ComPtr<ICoreWebView2Controller2> controller2;
        if (SUCCEEDED(entry.controller.As(&controller2)) && controller2) {
            COREWEBVIEW2_COLOR backgroundColor = entry.transparentBackground ? COREWEBVIEW2_COLOR{ 0, 0, 0, 0 }
                                                                          : COREWEBVIEW2_COLOR{ 255, 255, 255, 255 };
            controller2->put_DefaultBackgroundColor(backgroundColor);
        }
    }

    if (entry.webview) {
        ComPtr<ICoreWebView2_8> webview8;
        if (SUCCEEDED(entry.webview.As(&webview8)) && webview8) {
            webview8->put_IsMuted(entry.muteAudio ? TRUE : FALSE);
        }
    }
}

HWND CreateBrowserOverlayHostWindow(const std::wstring& title, int width, int height) {
    if (!EnsureBrowserOverlayHostClass()) {
        return nullptr;
    }

    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kBrowserOverlayHostClassName,
        title.c_str(),
        WS_POPUP | WS_VISIBLE,
        kBrowserOverlayOffscreenPos,
        kBrowserOverlayOffscreenPos,
        (std::max)(1, width),
        (std::max)(1, height),
        nullptr,
        nullptr,
        g_hModule ? g_hModule : GetModuleHandleW(nullptr),
        nullptr);

    if (hwnd) {
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        UpdateWindow(hwnd);
    }

    return hwnd;
}

void DestroyBrowserOverlayEntry(BrowserOverlayCacheEntry& entry) {
    QueueBrowserOverlayGlCleanup(entry);
    if (entry.controller) {
        entry.controller->Close();
        entry.controller.Reset();
    }
    entry.webview.Reset();
    if (entry.hostWindow) {
        DestroyWindow(entry.hostWindow);
        entry.hostWindow = nullptr;
    }
}

void RequestBrowserOverlayReload(BrowserOverlayCacheEntry& entry) {
    if (!entry.webview) {
        return;
    }

    entry.pendingReload = false;
    entry.hasSeenContentLoading = false;
    entry.webview->Reload();
    entry.lastReloadTime = std::chrono::steady_clock::now();
}

void ConfigureBrowserOverlayController(const std::string& overlayId, BrowserOverlayCacheEntry& entry) {
    if (!entry.controller) {
        return;
    }

    UpdateBrowserOverlayBounds(entry);

    entry.controller->get_CoreWebView2(entry.webview.ReleaseAndGetAddressOf());
    if (!entry.webview) {
        return;
    }

    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(entry.webview->get_Settings(&settings)) && settings) {
        settings->put_AreDevToolsEnabled(FALSE);
        settings->put_AreDefaultContextMenusEnabled(FALSE);

        ComPtr<ICoreWebView2Settings3> settings3;
        if (SUCCEEDED(settings.As(&settings3)) && settings3) {
            settings3->put_AreBrowserAcceleratorKeysEnabled(entry.allowSystemMediaKeys ? TRUE : FALSE);
        }
    }

    ApplyBrowserOverlayControllerSettings(entry);

    entry.webview->add_ContentLoading(
        Microsoft::WRL::Callback<ICoreWebView2ContentLoadingEventHandler>(
            [overlayId](ICoreWebView2*, ICoreWebView2ContentLoadingEventArgs*) -> HRESULT {
                std::lock_guard<std::mutex> lock(g_browserOverlayCacheMutex);
                auto it = g_browserOverlayCache.find(overlayId);
                if (it != g_browserOverlayCache.end() && it->second) {
                    it->second->hasSeenContentLoading = true;
                    ApplyBrowserOverlayInjectedStyles(*it->second);
                }
                return S_OK;
            })
            .Get(),
        &entry.contentLoadingToken);

    entry.webview->add_NavigationCompleted(
        Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [overlayId](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                BOOL success = FALSE;
                COREWEBVIEW2_WEB_ERROR_STATUS errorStatus = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                if (args) {
                    args->get_IsSuccess(&success);
                    args->get_WebErrorStatus(&errorStatus);
                }

                std::lock_guard<std::mutex> lock(g_browserOverlayCacheMutex);
                auto it = g_browserOverlayCache.find(overlayId);
                if (it == g_browserOverlayCache.end() || !it->second) {
                    return S_OK;
                }

                it->second->lastReloadTime = std::chrono::steady_clock::now();
                if (!success) {
                    LogBrowserOverlayMessage("[BrowserOverlay] Navigation failed for '" + overlayId + "' with status " +
                                             std::to_string(static_cast<int>(errorStatus)));
                } else {
                    ApplyBrowserOverlayInjectedStyles(*it->second);
                }
                return S_OK;
            })
            .Get(),
        &entry.navigationCompletedToken);

    entry.webview->add_ProcessFailed(
        Microsoft::WRL::Callback<ICoreWebView2ProcessFailedEventHandler>(
            [overlayId](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
                COREWEBVIEW2_PROCESS_FAILED_KIND kind = COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED;
                if (args) {
                    args->get_ProcessFailedKind(&kind);
                }

                std::lock_guard<std::mutex> lock(g_browserOverlayCacheMutex);
                auto it = g_browserOverlayCache.find(overlayId);
                if (it != g_browserOverlayCache.end() && it->second) {
                    it->second->pendingReload = true;
                    it->second->captureInFlight = false;
                }

                LogBrowserOverlayMessage("[BrowserOverlay] Process failure for '" + overlayId + "', kind=" +
                                         std::to_string(static_cast<int>(kind)));
                return S_OK;
            })
            .Get(),
        &entry.processFailedToken);

    NavigateBrowserOverlay(entry);
}

void BeginBrowserOverlayControllerCreation(const std::string& overlayId, BrowserOverlayCacheEntry& entry) {
    ICoreWebView2Environment* environment = GetBrowserOverlayEnvironment(entry.allowSystemMediaKeys, entry.hardwareAcceleration);
    if (!environment || entry.controller || entry.controllerCreationPending || entry.controllerCreationFailed) {
        return;
    }

    const bool allowSystemMediaKeys = entry.allowSystemMediaKeys;
    const bool hardwareAcceleration = entry.hardwareAcceleration;

    if (!entry.hostWindow) {
        entry.hostWindow = CreateBrowserOverlayHostWindow(Utf8ToWide("Toolscreen Browser Overlay - " + overlayId), entry.browserWidth,
                                                          entry.browserHeight);
        if (!entry.hostWindow) {
            entry.controllerCreationFailed = true;
            LogBrowserOverlayMessage("[BrowserOverlay] Failed to create host window for '" + overlayId + "'");
            return;
        }
    }

    entry.controllerCreationPending = true;

    environment->CreateCoreWebView2Controller(
        entry.hostWindow,
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [overlayId, allowSystemMediaKeys, hardwareAcceleration](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                std::lock_guard<std::mutex> lock(g_browserOverlayCacheMutex);
                auto it = g_browserOverlayCache.find(overlayId);
                if (it == g_browserOverlayCache.end() || !it->second) {
                    if (controller) {
                        controller->Close();
                    }
                    return S_OK;
                }

                BrowserOverlayCacheEntry& entry = *it->second;
                entry.controllerCreationPending = false;
                if (FAILED(result) || !controller) {
                    entry.controllerCreationFailed = true;
                    LogBrowserOverlayMessage("[BrowserOverlay] Failed to create controller for '" + overlayId + "': " +
                                             FormatHexResult(result));
                    return S_OK;
                }

                if (entry.allowSystemMediaKeys != allowSystemMediaKeys || entry.hardwareAcceleration != hardwareAcceleration) {
                    controller->Close();
                    entry.controllerCreationFailed = false;
                    BeginBrowserOverlayControllerCreation(overlayId, entry);
                    return S_OK;
                }

                entry.controller = controller;
                entry.controllerCreationFailed = false;
                ConfigureBrowserOverlayController(overlayId, entry);
                return S_OK;
            })
            .Get());
}

void SyncBrowserOverlayEntry(const BrowserOverlayConfig& config) {
    std::shared_ptr<BrowserOverlayCacheEntry> entryPtr;
    bool urlChanged = false;
    bool cssChanged = false;
    bool sizeChanged = false;
    bool transparencyChanged = false;
    bool mediaKeyHandlingChanged = false;
    bool hardwareAccelerationChanged = false;
    const bool transparentModeEnabled = config.transparentBackground;
    const bool allowSystemMediaKeys = config.allowSystemMediaKeys;
    const bool hardwareAcceleration = config.hardwareAcceleration;

    {
        std::lock_guard<std::mutex> lock(g_browserOverlayCacheMutex);
        auto& slot = g_browserOverlayCache[config.name];
        if (!slot) {
            slot = std::make_shared<BrowserOverlayCacheEntry>();
            slot->name = config.name;
            slot->url = config.url;
            slot->customCss = config.customCss;
            slot->browserWidth = (std::max)(1, config.browserWidth);
            slot->browserHeight = (std::max)(1, config.browserHeight);
            slot->fps = (std::max)(1, config.fps);
            slot->transparentBackground = transparentModeEnabled;
            slot->muteAudio = config.muteAudio;
            slot->hardwareAcceleration = hardwareAcceleration;
            slot->allowSystemMediaKeys = allowSystemMediaKeys;
            slot->reloadOnUpdate = config.reloadOnUpdate;
            slot->reloadInterval = (std::max)(0, config.reloadInterval);
        } else {
            urlChanged = slot->url != config.url;
            cssChanged = slot->customCss != config.customCss;
            sizeChanged = slot->browserWidth != config.browserWidth || slot->browserHeight != config.browserHeight;
            transparencyChanged = slot->transparentBackground != transparentModeEnabled;
            mediaKeyHandlingChanged = slot->allowSystemMediaKeys != allowSystemMediaKeys;
            hardwareAccelerationChanged = slot->hardwareAcceleration != hardwareAcceleration;
            slot->url = config.url;
            slot->customCss = config.customCss;
            slot->browserWidth = (std::max)(1, config.browserWidth);
            slot->browserHeight = (std::max)(1, config.browserHeight);
            slot->fps = (std::max)(1, config.fps);
            slot->transparentBackground = transparentModeEnabled;
            slot->muteAudio = config.muteAudio;
            slot->hardwareAcceleration = hardwareAcceleration;
            slot->allowSystemMediaKeys = allowSystemMediaKeys;
            slot->reloadOnUpdate = config.reloadOnUpdate;
            slot->reloadInterval = (std::max)(0, config.reloadInterval);
            slot->markedForRemoval = false;
        }
        entryPtr = slot;
    }

    if (!entryPtr) {
        return;
    }

    UpdateBrowserOverlayBounds(*entryPtr);

    if ((mediaKeyHandlingChanged || hardwareAccelerationChanged) && !entryPtr->controllerCreationPending) {
        DestroyBrowserOverlayEntry(*entryPtr);
        entryPtr->controllerCreationPending = false;
        entryPtr->controllerCreationFailed = false;
        entryPtr->captureInFlight = false;
        entryPtr->hasSeenContentLoading = false;
        entryPtr->pendingReload = false;
        entryPtr->lastCaptureTime = std::chrono::steady_clock::time_point{};
        entryPtr->lastReloadTime = std::chrono::steady_clock::time_point{};
    }

    if (!entryPtr->controller) {
        entryPtr->controllerCreationFailed = false;
        BeginBrowserOverlayControllerCreation(config.name, *entryPtr);
        return;
    }

    if (transparencyChanged || cssChanged) {
        ApplyBrowserOverlayInjectedStyles(*entryPtr);
    }

    ApplyBrowserOverlayControllerSettings(*entryPtr);

    if (urlChanged) {
        NavigateBrowserOverlay(*entryPtr);
        return;
    }

    if (sizeChanged && entryPtr->reloadOnUpdate) {
        entryPtr->pendingReload = true;
    }
}


bool StageBrowserOverlayTestFrameInternal(const BrowserOverlayConfig& config, const std::vector<unsigned char>& rgbaPixels, int width,
                                         int height) {
    if (width <= 0 || height <= 0) {
        return false;
    }

    const size_t expectedByteCount = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
    if (rgbaPixels.size() != expectedByteCount) {
        return false;
    }

    std::shared_ptr<BrowserOverlayCacheEntry> entry;
    {
        std::lock_guard<std::mutex> lock(g_browserOverlayCacheMutex);
        auto& slot = g_browserOverlayCache[config.name];
        if (!slot) {
            slot = std::make_shared<BrowserOverlayCacheEntry>();
        }

        entry = slot;
        entry->name = config.name;
        entry->url = config.url;
        entry->customCss = config.customCss;
        entry->browserWidth = (std::max)(1, config.browserWidth);
        entry->browserHeight = (std::max)(1, config.browserHeight);
        entry->fps = (std::max)(1, config.fps);
        entry->transparentBackground = config.transparentBackground;
        entry->muteAudio = config.muteAudio;
        entry->allowSystemMediaKeys = config.allowSystemMediaKeys;
        entry->reloadOnUpdate = config.reloadOnUpdate;
        entry->reloadInterval = (std::max)(0, config.reloadInterval);
        entry->markedForRemoval = false;
    }

    std::lock_guard<std::mutex> swapLock(entry->swapMutex);
    entry->readyBuffer = std::make_unique<BrowserOverlayRenderData>();
    entry->readyBuffer->pixelData = new unsigned char[expectedByteCount];
    std::copy(rgbaPixels.begin(), rgbaPixels.end(), entry->readyBuffer->pixelData);
    entry->readyBuffer->width = width;
    entry->readyBuffer->height = height;
    entry->lastUploadedRenderData = nullptr;
    entry->hasNewFrame.store(true, std::memory_order_release);
    return true;
}
void ReconcileBrowserOverlays() {
    auto snapshot = GetConfigSnapshot();
    if (!snapshot) {
        return;
    }

    std::set<std::string> activeNames;
    for (const auto& config : snapshot->browserOverlays) {
        activeNames.insert(config.name);
        SyncBrowserOverlayEntry(config);
    }

    std::vector<std::shared_ptr<BrowserOverlayCacheEntry>> removedEntries;
    {
        std::lock_guard<std::mutex> lock(g_browserOverlayCacheMutex);
        for (auto it = g_browserOverlayCache.begin(); it != g_browserOverlayCache.end();) {
            if (it->second && (it->second->markedForRemoval || activeNames.find(it->first) == activeNames.end())) {
                removedEntries.push_back(std::move(it->second));
                it = g_browserOverlayCache.erase(it);
            } else {
                ++it;
            }
        }
    }

    for (auto& removed : removedEntries) {
        if (removed) {
            DestroyBrowserOverlayEntry(*removed);
        }
    }
}

void TryCaptureBrowserOverlay(const std::string& overlayId) {
    ComPtr<IStream> stream;
    std::shared_ptr<BrowserOverlayCacheEntry> entryPtr;
    bool shouldUseWindowCapture = false;
    bool forceWindowCaptureOnly = false;

    {
        std::lock_guard<std::mutex> lock(g_browserOverlayCacheMutex);
        auto it = g_browserOverlayCache.find(overlayId);
        if (it == g_browserOverlayCache.end() || !it->second) {
            return;
        }

        BrowserOverlayCacheEntry& entry = *it->second;
        if (!entry.webview || !entry.hasSeenContentLoading || entry.captureInFlight) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        const int captureIntervalMs = 1000 / (std::max)(1, entry.fps);
        if (entry.lastCaptureTime.time_since_epoch().count() != 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - entry.lastCaptureTime).count();
            if (elapsed < captureIntervalMs) {
                if (entry.reloadInterval > 0) {
                    const auto reloadElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - entry.lastReloadTime).count();
                    if (reloadElapsed >= entry.reloadInterval) {
                        RequestBrowserOverlayReload(entry);
                    }
                } else if (entry.pendingReload) {
                    RequestBrowserOverlayReload(entry);
                }
                return;
            }
        }

        if (entry.reloadInterval > 0) {
            const auto reloadElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - entry.lastReloadTime).count();
            if (reloadElapsed >= entry.reloadInterval) {
                RequestBrowserOverlayReload(entry);
            }
        } else if (entry.pendingReload) {
            RequestBrowserOverlayReload(entry);
        }

        HRESULT streamHr = CreateStreamOnHGlobal(nullptr, TRUE, stream.GetAddressOf());
        if (FAILED(streamHr) || !stream) {
            LogBrowserOverlayMessage("[BrowserOverlay] Failed to create in-memory stream for '" + overlayId + "'");
            return;
        }

        entry.captureInFlight = true;
        entry.lastCaptureTime = now;
        entryPtr = it->second;
        shouldUseWindowCapture = !entry.transparentBackground && entry.hostWindow != nullptr;
        forceWindowCaptureOnly = !entry.transparentBackground;
    }

    if (!entryPtr || !entryPtr->webview) {
        return;
    }

    if (shouldUseWindowCapture) {
        if (CaptureBrowserOverlayHostWindow(*entryPtr)) {
            std::lock_guard<std::mutex> lock(g_browserOverlayCacheMutex);
            auto it = g_browserOverlayCache.find(overlayId);
            if (it != g_browserOverlayCache.end() && it->second) {
                it->second->captureInFlight = false;
            }
            return;
        }

        if (forceWindowCaptureOnly) {
            std::lock_guard<std::mutex> lock(g_browserOverlayCacheMutex);
            auto it = g_browserOverlayCache.find(overlayId);
            if (it != g_browserOverlayCache.end() && it->second) {
                it->second->captureInFlight = false;
            }
            return;
        }
    }

    const HRESULT captureHr = entryPtr->webview->CapturePreview(
        COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG,
        stream.Get(),
        Microsoft::WRL::Callback<ICoreWebView2CapturePreviewCompletedHandler>(
            [overlayId, stream](HRESULT result) -> HRESULT {
                if (FAILED(result)) {
                    std::lock_guard<std::mutex> lock(g_browserOverlayCacheMutex);
                    auto it = g_browserOverlayCache.find(overlayId);
                    if (it != g_browserOverlayCache.end() && it->second) {
                        it->second->captureInFlight = false;
                    }
                    LogBrowserOverlayMessage("[BrowserOverlay] CapturePreview failed for '" + overlayId + "': " + FormatHexResult(result));
                    return S_OK;
                }

                STATSTG stat = {};
                if (FAILED(stream->Stat(&stat, STATFLAG_NONAME))) {
                    std::lock_guard<std::mutex> lock(g_browserOverlayCacheMutex);
                    auto it = g_browserOverlayCache.find(overlayId);
                    if (it != g_browserOverlayCache.end() && it->second) {
                        it->second->captureInFlight = false;
                    }
                    return S_OK;
                }

                const size_t encodedSize = static_cast<size_t>(stat.cbSize.QuadPart);
                if (encodedSize == 0 || encodedSize > 256ull * 1024ull * 1024ull) {
                    std::lock_guard<std::mutex> lock(g_browserOverlayCacheMutex);
                    auto it = g_browserOverlayCache.find(overlayId);
                    if (it != g_browserOverlayCache.end() && it->second) {
                        it->second->captureInFlight = false;
                    }
                    return S_OK;
                }

                HGLOBAL hGlobal = nullptr;
                if (FAILED(GetHGlobalFromStream(stream.Get(), &hGlobal)) || !hGlobal) {
                    std::lock_guard<std::mutex> lock(g_browserOverlayCacheMutex);
                    auto it = g_browserOverlayCache.find(overlayId);
                    if (it != g_browserOverlayCache.end() && it->second) {
                        it->second->captureInFlight = false;
                    }
                    return S_OK;
                }

                const void* rawBytes = GlobalLock(hGlobal);
                if (!rawBytes) {
                    std::lock_guard<std::mutex> lock(g_browserOverlayCacheMutex);
                    auto it = g_browserOverlayCache.find(overlayId);
                    if (it != g_browserOverlayCache.end() && it->second) {
                        it->second->captureInFlight = false;
                    }
                    return S_OK;
                }

                BrowserOverlayEncodedFrame frame{};
                frame.overlayId = overlayId;
                frame.encodedBytes.resize(encodedSize);
                memcpy(frame.encodedBytes.data(), rawBytes, encodedSize);
                GlobalUnlock(hGlobal);

                {
                    std::lock_guard<std::mutex> lock(g_browserOverlayCacheMutex);
                    auto it = g_browserOverlayCache.find(overlayId);
                    if (it != g_browserOverlayCache.end() && it->second) {
                        it->second->captureInFlight = false;
                        frame.sequence = ++it->second->latestQueuedDecodeSequence;
                    }
                }

                if (frame.sequence == 0 || frame.encodedBytes.empty()) {
                    return S_OK;
                }

                EnqueueBrowserOverlayDecode(std::move(frame));
                return S_OK;
            })
            .Get());

    if (FAILED(captureHr)) {
        std::lock_guard<std::mutex> lock(g_browserOverlayCacheMutex);
        auto it = g_browserOverlayCache.find(overlayId);
        if (it != g_browserOverlayCache.end() && it->second) {
            it->second->captureInFlight = false;
        }
        LogBrowserOverlayMessage("[BrowserOverlay] CapturePreview start failed for '" + overlayId + "': " + FormatHexResult(captureHr));
    }
}

void BrowserOverlayDecodeThreadFunc() {
    // Image overlays use a global stb vertical flip; browser captures need the raw orientation.
    stbi_set_flip_vertically_on_load_thread(0);

    LogBrowserOverlayMessage("[BrowserOverlay] Decode thread started");

    while (true) {
        BrowserOverlayEncodedFrame frame{};
        {
            std::unique_lock<std::mutex> lock(g_browserOverlayDecodeMutex);
            g_browserOverlayDecodeCv.wait(lock, [] {
                return g_stopBrowserOverlayThread.load(std::memory_order_acquire) || !g_browserOverlayPendingDecodeFrames.empty();
            });

            if (g_stopBrowserOverlayThread.load(std::memory_order_acquire) && g_browserOverlayPendingDecodeFrames.empty()) {
                break;
            }

            auto it = g_browserOverlayPendingDecodeFrames.begin();
            frame = std::move(it->second);
            g_browserOverlayPendingDecodeFrames.erase(it);
        }

        if (frame.encodedBytes.empty() || frame.sequence == 0) {
            continue;
        }

        int width = 0;
        int height = 0;
        int channels = 0;
        unsigned char* pixels = stbi_load_from_memory(frame.encodedBytes.data(), static_cast<int>(frame.encodedBytes.size()), &width,
                                                      &height, &channels, 4);

        if (!pixels || width <= 0 || height <= 0) {
            if (pixels) {
                stbi_image_free(pixels);
            }
            continue;
        }

        std::shared_ptr<BrowserOverlayCacheEntry> entry;
        {
            std::lock_guard<std::mutex> lock(g_browserOverlayCacheMutex);
            auto it = g_browserOverlayCache.find(frame.overlayId);
            if (it != g_browserOverlayCache.end() && it->second && it->second->latestQueuedDecodeSequence == frame.sequence) {
                entry = it->second;
            }
        }

        if (entry) {
            UpdateBrowserOverlayBuffer(*entry, pixels, width, height);
        }

        stbi_image_free(pixels);
    }

    LogBrowserOverlayMessage("[BrowserOverlay] Decode thread stopped");
}

void BrowserOverlayThreadFunc() {
    const HRESULT initHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initHr)) {
        LogBrowserOverlayMessage("[BrowserOverlay] CoInitializeEx failed: " + FormatHexResult(initHr));
        return;
    }

    // Image overlays use a global stb vertical flip; browser captures need the raw orientation.
    stbi_set_flip_vertically_on_load_thread(0);

    LogBrowserOverlayMessage("[BrowserOverlay] Thread started");

    uint64_t lastConfigVersion = UINT64_MAX;
    while (!g_stopBrowserOverlayThread.load(std::memory_order_acquire)) {
        PumpBrowserOverlayMessages();

        const uint64_t currentVersion = g_configSnapshotVersion.load(std::memory_order_acquire);
        if (currentVersion != lastConfigVersion) {
            lastConfigVersion = currentVersion;
            ReconcileBrowserOverlays();
        }

        if (g_browserOverlaysVisible.load(std::memory_order_acquire)) {
            std::vector<std::string> overlayIds;
            {
                std::lock_guard<std::mutex> lock(g_browserOverlayCacheMutex);
                overlayIds.reserve(g_browserOverlayCache.size());
                for (const auto& [overlayId, entry] : g_browserOverlayCache) {
                    if (entry && !entry->markedForRemoval) {
                        overlayIds.push_back(overlayId);
                    }
                }
            }

            for (const auto& overlayId : overlayIds) {
                TryCaptureBrowserOverlay(overlayId);
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }

    std::vector<std::shared_ptr<BrowserOverlayCacheEntry>> removedEntries;
    {
        std::lock_guard<std::mutex> lock(g_browserOverlayCacheMutex);
        for (auto& [_, entry] : g_browserOverlayCache) {
            removedEntries.push_back(std::move(entry));
        }
        g_browserOverlayCache.clear();
    }

    for (auto& entry : removedEntries) {
        if (entry) {
            DestroyBrowserOverlayEntry(*entry);
        }
    }

    for (BrowserOverlayEnvironmentState& environmentState : g_browserOverlayEnvironments) {
        environmentState.environment.Reset();
        environmentState.ready.store(false, std::memory_order_release);
        environmentState.failed.store(false, std::memory_order_release);
        environmentState.requested.store(false, std::memory_order_release);
    }

    CoUninitialize();
    LogBrowserOverlayMessage("[BrowserOverlay] Thread stopped");
}

} // namespace

bool StageBrowserOverlayTestFrame(const BrowserOverlayConfig& config, const std::vector<unsigned char>& rgbaPixels, int width, int height) {
    return StageBrowserOverlayTestFrameInternal(config, rgbaPixels, width, height);
}

const BrowserOverlayConfig* FindBrowserOverlayConfig(const std::string& overlayId) {
    const auto& overlays = g_config.browserOverlays;
    for (const auto& overlay : overlays) {
        if (overlay.name == overlayId) {
            return &overlay;
        }
    }
    return nullptr;
}

const BrowserOverlayConfig* FindBrowserOverlayConfigIn(const std::string& overlayId, const Config& config) {
    const auto& overlays = config.browserOverlays;
    for (const auto& overlay : overlays) {
        if (overlay.name == overlayId) {
            return &overlay;
        }
    }
    return nullptr;
}

void RemoveBrowserOverlayFromCache(const std::string& overlayId) {
    std::lock_guard<std::mutex> lock(g_browserOverlayCacheMutex);
    auto it = g_browserOverlayCache.find(overlayId);
    if (it != g_browserOverlayCache.end() && it->second) {
        it->second->markedForRemoval = true;
    }
}

void RequestBrowserOverlayRefresh(const std::string& overlayId) {
    std::lock_guard<std::mutex> lock(g_browserOverlayCacheMutex);
    auto it = g_browserOverlayCache.find(overlayId);
    if (it != g_browserOverlayCache.end() && it->second) {
        it->second->pendingReload = true;
        it->second->lastCaptureTime = std::chrono::steady_clock::time_point{};
    }
}

void CleanupBrowserOverlayCache() {
    std::vector<std::shared_ptr<BrowserOverlayCacheEntry>> entries;
    {
        std::lock_guard<std::mutex> lock(g_browserOverlayCacheMutex);
        entries.reserve(g_browserOverlayCache.size());
        for (auto& [_, entry] : g_browserOverlayCache) {
            if (!entry) {
                continue;
            }
            entry->markedForRemoval = true;
            entries.push_back(entry);
        }
    }

    for (const auto& entry : entries) {
        if (entry) {
            QueueBrowserOverlayGlCleanup(*entry);
        }
    }

    FlushBrowserOverlayGlCleanup();

    {
        std::lock_guard<std::mutex> lock(g_browserOverlayDecodeMutex);
        g_browserOverlayPendingDecodeFrames.clear();
    }
}

void StartBrowserOverlayThread() {
    if (!g_browserOverlayThread.joinable()) {
        g_stopBrowserOverlayThread.store(false, std::memory_order_release);
        g_browserOverlayThread = std::thread(BrowserOverlayThreadFunc);
    }
    if (!g_browserOverlayDecodeThread.joinable()) {
        g_browserOverlayDecodeThread = std::thread(BrowserOverlayDecodeThreadFunc);
    }
}

void StopBrowserOverlayThread() {
    if (g_browserOverlayThread.joinable()) {
        g_stopBrowserOverlayThread.store(true, std::memory_order_release);
        g_browserOverlayDecodeCv.notify_all();
        g_browserOverlayThread.join();
    }
    if (g_browserOverlayDecodeThread.joinable()) {
        g_browserOverlayDecodeCv.notify_all();
        g_browserOverlayDecodeThread.join();
    }

    {
        std::lock_guard<std::mutex> lock(g_browserOverlayDecodeMutex);
        g_browserOverlayPendingDecodeFrames.clear();
    }
}

bool PrepareBrowserOverlayTexture(const BrowserOverlayConfig& config, BrowserOverlayTextureFrame& outFrame) {
    outFrame = {};

    FlushBrowserOverlayGlCleanup();

    std::shared_ptr<BrowserOverlayCacheEntry> entry;
    {
        std::lock_guard<std::mutex> cacheLock(g_browserOverlayCacheMutex);
        auto it = g_browserOverlayCache.find(config.name);
        if (it == g_browserOverlayCache.end() || !it->second) {
            return false;
        }
        entry = it->second;
    }

    if (!entry) {
        return false;
    }

    if (entry->hasNewFrame.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> swapLock(entry->swapMutex);
        entry->readyBuffer.swap(entry->backBuffer);
        entry->hasNewFrame.store(false, std::memory_order_release);
    }

    BrowserOverlayRenderData* renderData = entry->backBuffer.get();
    std::lock_guard<std::mutex> renderLock(entry->renderStateMutex);
    if (renderData && renderData->pixelData && renderData->width > 0 && renderData->height > 0) {
        if (renderData != entry->lastUploadedRenderData) {
            UploadBrowserOverlayTexture(*entry, *renderData);
        }
    }

    if (entry->glTextureId == 0) {
        return false;
    }

    BindTextureDirect(GL_TEXTURE_2D, entry->glTextureId);
    if (!entry->filterInitialized || entry->lastPixelatedScaling != config.pixelatedScaling) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, config.pixelatedScaling ? GL_NEAREST : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, config.pixelatedScaling ? GL_NEAREST : GL_LINEAR);
        entry->lastPixelatedScaling = config.pixelatedScaling;
        entry->filterInitialized = true;
    }

    outFrame.textureId = entry->glTextureId;
    outFrame.textureWidth = entry->glTextureWidth;
    outFrame.textureHeight = entry->glTextureHeight;
    return outFrame.textureId != 0 && outFrame.textureWidth > 0 && outFrame.textureHeight > 0;
}

bool AcquireBrowserOverlayPixelFrame(
    const BrowserOverlayConfig& config, BrowserOverlayPixelFrame& outFrame) {
    outFrame = {};
    std::shared_ptr<BrowserOverlayCacheEntry> entry;
    {
        std::lock_guard<std::mutex> cacheLock(g_browserOverlayCacheMutex);
        auto it = g_browserOverlayCache.find(config.name);
        if (it == g_browserOverlayCache.end() || !it->second) return false;
        entry = it->second;
    }

    if (entry->hasNewFrame.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> swapLock(entry->swapMutex);
        if (entry->hasNewFrame.load(std::memory_order_relaxed)) {
            entry->readyBuffer.swap(entry->backBuffer);
            entry->hasNewFrame.store(false, std::memory_order_release);
        }
    }

    std::lock_guard<std::mutex> renderLock(entry->renderStateMutex);
    BrowserOverlayRenderData* renderData = entry->backBuffer.get();
    if (!renderData || !renderData->pixelData || renderData->width <= 0 ||
        renderData->height <= 0) {
        return false;
    }
    if (renderData != entry->lastPublishedRenderData || !entry->publishedPixels) {
        const size_t byteCount =
            static_cast<size_t>(renderData->width) *
            static_cast<size_t>(renderData->height) * 4;
        auto pixels = std::make_shared<std::vector<unsigned char>>(
            renderData->pixelData, renderData->pixelData + byteCount);
        entry->publishedPixels = std::move(pixels);
        entry->lastPublishedRenderData = renderData;
        ++entry->publishedGeneration;
    }
    outFrame.pixels = entry->publishedPixels;
    outFrame.width = renderData->width;
    outFrame.height = renderData->height;
    entry->glTextureWidth = renderData->width;
    entry->glTextureHeight = renderData->height;
    outFrame.generation = entry->publishedGeneration;
    return static_cast<bool>(outFrame.pixels);
}
