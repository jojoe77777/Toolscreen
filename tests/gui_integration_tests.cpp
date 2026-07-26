#include "common/i18n.h"
#include "common/ninjabrain_information_messages.h"
#include "common/font_assets.h"
#include "common/mode_dimensions.h"
#include "common/profiler.h"
#include "common/utils.h"
#include "config/config_migration.h"
#include "config/config_toml.h"
#include "features/browser_overlay.h"
#include "features/ninjabrain_client.h"
#include "features/ninjabrain_data.h"
#include "features/window_overlay.h"
#include "gui/gui.h"
#include "gui/gui_internal.h"
#include "hooks/input_hook.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_win32.h"
#include "platform/resource.h"
#include "render/render.h"
#include "runtime/logic_thread.h"

#include <GL/glew.h>
#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <stdexcept>
#include <array>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
extern std::atomic<bool> g_configLoaded;
extern std::atomic<HWND> g_subclassedHwnd;
extern FILTERKEYS g_originalFilterKeys;
extern std::atomic<bool> g_originalFilterKeysCaptured;
bool GetEffectiveKeyRepeatTimings(int& outStartDelayMs, int& outRepeatDelayMs);

namespace {

constexpr int kWindowWidth = 1600;
constexpr int kWindowHeight = 900;
constexpr wchar_t kWindowClassName[] = L"ToolscreenGuiIntegrationTestWindow";
constexpr char kDefaultVisualTestCase[] = "settings-gui-advanced";

std::string g_visualNinjabrainPreviewModeId;

enum class TestRunMode {
    Automated,
    Visual,
};

using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT value);
using SetProcessDPIAwareFn = BOOL(WINAPI*)();

void EnsureProcessDpiAwareness() {
    static const bool configured = []() {
        if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
            if (auto setDpiAwarenessContext = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
                    GetProcAddress(user32, "SetProcessDpiAwarenessContext"))) {
                if (setDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
                    return true;
                }

                if (setDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE)) {
                    return true;
                }
            }

            if (auto setProcessDpiAware = reinterpret_cast<SetProcessDPIAwareFn>(GetProcAddress(user32, "SetProcessDPIAware"))) {
                if (setProcessDpiAware()) {
                    return true;
                }
            }
        }

        return false;
    }();

    (void)configured;
}

static bool g_hasModernGL = false;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string Narrow(const std::wstring& value) {
    return WideToUtf8(value);
}

LRESULT CALLBACK TestWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, message, wParam, lParam)) {
        return TRUE;
    }

    if (message == WM_CLOSE) {
        DestroyWindow(hwnd);
        return 0;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void RegisterWindowClass() {
    static const ATOM windowClass = []() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_OWNDC;
        wc.lpfnWndProc = TestWindowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kWindowClassName;
        return RegisterClassExW(&wc);
    }();

    Expect(windowClass != 0, "Failed to register GUI integration test window class.");
}

class ScopedTabSelection {
  public:
    ScopedTabSelection(const char* topLevelTabLabel, const char* inputsSubTabLabel) {
        SetGuiTabSelectionOverride(topLevelTabLabel, inputsSubTabLabel);
    }

    ~ScopedTabSelection() { ClearGuiTabSelectionOverride(); }
};

class DummyWindow {
  public:
        DummyWindow(int width, int height, bool visible = false) : m_width(width), m_height(height) {
        RegisterWindowClass();

        m_hwnd = CreateWindowExW(0, kWindowClassName, L"Toolscreen GUI Integration Tests", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                 CW_USEDEFAULT, width, height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Expect(m_hwnd != nullptr, "Failed to create GUI integration test window.");

        m_hdc = GetDC(m_hwnd);
        Expect(m_hdc != nullptr, "Failed to acquire device context for GUI integration test window.");

        PIXELFORMATDESCRIPTOR pfd{};
        pfd.nSize = sizeof(pfd);
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 32;
        pfd.cDepthBits = 24;
        pfd.cStencilBits = 8;
        pfd.iLayerType = PFD_MAIN_PLANE;

        const int pixelFormat = ChoosePixelFormat(m_hdc, &pfd);
        Expect(pixelFormat != 0, "Failed to choose a pixel format for GUI integration test window.");
        Expect(SetPixelFormat(m_hdc, pixelFormat, &pfd) == TRUE,
               "Failed to set the pixel format for GUI integration test window.");

        m_previousHdc = wglGetCurrentDC();
        m_previousGlContext = wglGetCurrentContext();

        m_glContext = wglCreateContext(m_hdc);
        Expect(m_glContext != nullptr, "Failed to create an OpenGL context for GUI integration tests.");
        Expect(wglMakeCurrent(m_hdc, m_glContext) == TRUE, "Failed to activate the OpenGL context for GUI integration tests.");

        glewExperimental = GL_TRUE;
        const GLenum glewStatus = glewInit();
        glGetError();
        Expect(glewStatus == GLEW_OK,
               "Failed to initialize GLEW for GUI integration tests: " + std::string(reinterpret_cast<const char*>(glewGetErrorString(glewStatus))));

        const char* glVersionStr = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        m_glMajor = 0;
        if (glVersionStr) { m_glMajor = glVersionStr[0] - '0'; }
        g_hasModernGL = m_glMajor >= 3;

        RefreshClientSize();
        ShowWindow(m_hwnd, visible ? SW_SHOW : SW_HIDE);
        if (visible) {
            UpdateWindow(m_hwnd);
            SetForegroundWindow(m_hwnd);
            SetFocus(m_hwnd);
        }
        g_minecraftHwnd.store(m_hwnd, std::memory_order_release);
    }

    ~DummyWindow() {
        ClearGuiTabSelectionOverride();
        HandleImGuiContextReset();

        if (wglGetCurrentContext() == m_glContext) {
            wglMakeCurrent(nullptr, nullptr);
        }
        if (m_glContext != nullptr) {
            wglDeleteContext(m_glContext);
        }
        if (m_previousGlContext != nullptr && m_previousHdc != nullptr) {
            wglMakeCurrent(m_previousHdc, m_previousGlContext);
        }
        if (m_hwnd != nullptr && m_hdc != nullptr) {
            ReleaseDC(m_hwnd, m_hdc);
        }
        if (m_hwnd != nullptr) {
            DestroyWindow(m_hwnd);
        }

        g_minecraftHwnd.store(nullptr, std::memory_order_release);
    }

    HWND hwnd() const { return m_hwnd; }

    bool isOpen() const { return m_isOpen; }

    bool hasModernGL() const { return m_glMajor >= 3; }

    void SetTitle(const std::string& title) {
        if (m_hwnd != nullptr) {
            SetWindowTextW(m_hwnd, Utf8ToWide(title).c_str());
        }
    }

    void Show(bool visible) {
        if (m_hwnd == nullptr) {
            return;
        }

        ShowWindow(m_hwnd, visible ? SW_SHOW : SW_HIDE);
        if (visible) {
            UpdateWindow(m_hwnd);
            SetForegroundWindow(m_hwnd);
            SetFocus(m_hwnd);
        }
    }

    bool PumpMessages() {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                m_isOpen = false;
                return false;
            }

            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        m_isOpen = m_hwnd != nullptr && IsWindow(m_hwnd) != FALSE;
        return m_isOpen;
    }

    bool PrepareRenderSurface() {
        if (!PumpMessages()) {
            return false;
        }

        Expect(MakeCurrent(), "Failed to reactivate the OpenGL context for GUI integration test window.");

        InitializeImGuiContext(m_hwnd);
        RefreshClientSize();

        glViewport(0, 0, m_width, m_height);
        glClearColor(0.08f, 0.08f, 0.10f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        return true;
    }

    bool BeginFrame() {
        if (!PrepareRenderSurface()) {
            return false;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplWin32_NewFrame();
        SyncImGuiDisplayMetrics(m_hwnd);
        ApplyDynamicGuiFontRefresh();
        ImGui::NewFrame();
        return true;
    }

    void EndFrame() {
        ImGui::Render();
        ValidateDrawData();
        RenderImGuiWithStateProtection(true);
        PresentSurface();
    }

    void PresentSurface() {
        Expect(SwapBuffers(m_hdc) == TRUE, "SwapBuffers failed for GUI integration test window.");
        PumpMessages();
    }

    bool MakeCurrent() const {
        if (m_hdc == nullptr || m_glContext == nullptr) {
            return false;
        }

        if (wglGetCurrentContext() == m_glContext) {
            return true;
        }

        return wglMakeCurrent(m_hdc, m_glContext) == TRUE;
    }

  private:
    void RefreshClientSize() {
        RECT clientRect{};
        if (m_hwnd == nullptr || GetClientRect(m_hwnd, &clientRect) == FALSE) {
            return;
        }

        const int clientWidth = clientRect.right - clientRect.left;
        const int clientHeight = clientRect.bottom - clientRect.top;
        if (clientWidth <= 0 || clientHeight <= 0) {
            return;
        }

        m_width = clientWidth;
        m_height = clientHeight;
        UpdateCachedWindowMetricsFromSize(m_width, m_height);
    }

    static void ValidateDrawData() {
        const ImDrawData* drawData = ImGui::GetDrawData();
        Expect(drawData != nullptr, "Expected ImGui draw data to exist after rendering.");
        Expect(drawData->CmdListsCount > 0, "Expected rendered GUI to contain at least one command list.");
        Expect(drawData->TotalVtxCount > 0, "Expected rendered GUI to contain at least one vertex.");
    }

    HWND m_hwnd = nullptr;
    HDC m_hdc = nullptr;
    HGLRC m_glContext = nullptr;
    HDC m_previousHdc = nullptr;
    HGLRC m_previousGlContext = nullptr;
    int m_width = 0;
    int m_height = 0;
    bool m_isOpen = true;
    int m_glMajor = 0;
};

std::filesystem::path PrepareCaseDirectory(std::string_view caseName) {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "toolscreen_gui_integration" / std::filesystem::path(caseName);

    std::error_code error;
    std::filesystem::remove_all(root, error);
    error.clear();
    std::filesystem::create_directories(root, error);
    Expect(!error, "Failed to create test directory: " + Narrow(root.wstring()));

    return root;
}

class ScopedLogSessionClaim {
  public:
    ScopedLogSessionClaim() = default;

    explicit ScopedLogSessionClaim(const std::filesystem::path& logsDirectory) {
        Acquire(logsDirectory);
    }

    ~ScopedLogSessionClaim() { Release(); }

    void Acquire(const std::filesystem::path& logsDirectory) {
        Release();
        Expect(AcquireLatestLogSession(logsDirectory.wstring(), m_session),
               "Failed to acquire a latest log session for integration testing.");
        m_acquired = true;
    }

    void Release() {
        if (!m_acquired) {
            return;
        }

        ReleaseLatestLogSession(m_session);
        m_acquired = false;
    }

    const LogSession& session() const {
        Expect(m_acquired, "Tried to read an integration-test log session before acquiring it.");
        return m_session;
    }

  private:
    LogSession m_session;
    bool m_acquired = false;
};

std::string ReadTextFileUtf8(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    Expect(in.is_open(), "Failed to open text fixture file: " + Narrow(path.wstring()));
    std::ostringstream stream;
    stream << in.rdbuf();
    return stream.str();
}

void WriteTextFileUtf8(const std::filesystem::path& path, std::string_view contents) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    Expect(!error, "Failed to create parent directory for text fixture: " + Narrow(path.parent_path().wstring()));

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    Expect(out.is_open(), "Failed to open text fixture path for writing: " + Narrow(path.wstring()));
    out << contents;
    out.close();
}

void SetLocalFileLastWriteTime(const std::filesystem::path& path,
                               WORD year,
                               WORD month,
                               WORD day,
                               WORD hour,
                               WORD minute,
                               WORD second) {
    HANDLE file = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    Expect(file != INVALID_HANDLE_VALUE, "Failed to open file for timestamp update: " + Narrow(path.wstring()));

    SYSTEMTIME localTime{};
    localTime.wYear = year;
    localTime.wMonth = month;
    localTime.wDay = day;
    localTime.wHour = hour;
    localTime.wMinute = minute;
    localTime.wSecond = second;

        FILETIME localFileTime{};
        Expect(SystemTimeToFileTime(&localTime, &localFileTime) == TRUE,
            "Failed to convert local SYSTEMTIME to local FILETIME for: " + Narrow(path.wstring()));

        FILETIME utcFileTime{};
        Expect(LocalFileTimeToFileTime(&localFileTime, &utcFileTime) == TRUE,
            "Failed to convert local FILETIME to UTC FILETIME for: " + Narrow(path.wstring()));
        Expect(SetFileTime(file, nullptr, nullptr, &utcFileTime) == TRUE,
           "Failed to apply timestamp to fixture file: " + Narrow(path.wstring()));
    CloseHandle(file);
}

std::vector<std::filesystem::path> ListDirectoryEntriesSorted(const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> entries;
    std::error_code error;
    if (!std::filesystem::exists(directory, error)) {
        return entries;
    }

    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        if (error) {
            break;
        }
        entries.push_back(entry.path());
    }

    std::sort(entries.begin(), entries.end());
    return entries;
}

std::vector<std::string> ListDirectoryFileNamesSorted(const std::filesystem::path& directory) {
    std::vector<std::string> fileNames;
    for (const auto& path : ListDirectoryEntriesSorted(directory)) {
        fileNames.push_back(path.filename().string());
    }
    return fileNames;
}

bool ContainsFileName(const std::vector<std::string>& fileNames, std::string_view expectedName) {
    return std::find(fileNames.begin(), fileNames.end(), std::string(expectedName)) != fileNames.end();
}

void ResetGlobalTestState(const std::filesystem::path& root) {
    StopNinjabrainClient();

    ResetExactKeyboardMessageStateForTest();
    ResetHotkeyRuntimeStateForTest();
    ResetLowLevelExactModifierStateForTest();
    ClearLowLevelSuppressedKeysForTest();

    g_toolscreenPath = root.wstring();
    g_modeFilePath = (root / "mode.txt").wstring();
    g_stateFilePath = (root / "state.txt").wstring();

    ExtractBundledFontAssets(root, &ResetGlobalTestState);

    g_config = Config();
    g_sharedConfig = Config();
    g_configIsDirty.store(false, std::memory_order_release);
    g_configLoadFailed.store(false, std::memory_order_release);
    g_configLoaded.store(false, std::memory_order_release);
    g_showGui.store(true, std::memory_order_release);
    g_guiNeedsRecenter.store(true, std::memory_order_release);
    g_welcomeToastVisible.store(false, std::memory_order_release);
    g_configurePromptDismissedThisSession.store(false, std::memory_order_release);
    g_screenshotRequested.store(false, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lock(g_modeIdMutex);
        g_currentModeId.clear();
        g_modeIdBuffers[0].clear();
        g_modeIdBuffers[1].clear();
    }
    g_currentModeIdIndex.store(0, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lock(g_configErrorMutex);
        g_configLoadError.clear();
    }

    ClearGuiTabSelectionOverride();
    PublishNinjabrainData(NinjabrainData{});
    LoadLangs();
    Expect(!GetLangs().empty(), "Expected embedded language metadata to load.");
    Expect(LoadTranslation("en"), "Expected embedded English translations to load.");
    UpdateCachedWindowMetricsFromSize(kWindowWidth, kWindowHeight);
}

const ModeConfig* ResolveVisualNinjabrainPreviewMode() {
    if (!g_configLoaded.load(std::memory_order_acquire) || g_config.modes.empty()) {
        return nullptr;
    }

    auto findMode = [](const std::string& modeId) -> const ModeConfig* {
        for (const auto& mode : g_config.modes) {
            if (EqualsIgnoreCase(mode.id, modeId)) {
                return &mode;
            }
        }

        return nullptr;
    };

    for (const std::string& allowedMode : g_config.ninjabrainOverlay.allowedModes) {
        if (const ModeConfig* mode = findMode(allowedMode)) {
            return mode;
        }
    }

    if (const ModeConfig* defaultMode = findMode(g_config.defaultMode)) {
        return defaultMode;
    }

    return &g_config.modes.front();
}

class ScopedVisualNinjabrainPreviewSession {
  public:
    ScopedVisualNinjabrainPreviewSession() {
        const ModeConfig* previewMode = ResolveVisualNinjabrainPreviewMode();
        if (previewMode == nullptr) {
            return;
        }

        originalOverlay_ = g_config.ninjabrainOverlay;
        {
            std::lock_guard<std::mutex> lock(g_modeIdMutex);
            originalCurrentModeId_ = g_currentModeId;
            originalModeIdBuffers_[0] = g_modeIdBuffers[0];
            originalModeIdBuffers_[1] = g_modeIdBuffers[1];
        }
        originalModeIdIndex_ = g_currentModeIdIndex.load(std::memory_order_acquire);

        g_visualNinjabrainPreviewModeId = previewMode->id;
        {
            std::lock_guard<std::mutex> lock(g_modeIdMutex);
            g_currentModeId = previewMode->id;
            const int nextIndex = 1 - g_currentModeIdIndex.load(std::memory_order_relaxed);
            g_modeIdBuffers[nextIndex] = previewMode->id;
            g_currentModeIdIndex.store(nextIndex, std::memory_order_release);
        }

        g_config.ninjabrainOverlay.enabled = true;
        g_config.ninjabrainOverlay.onlyOnMyScreen = false;
        g_config.ninjabrainOverlay.onlyOnObs = false;
        g_config.ninjabrainOverlay.relativeTo = "topLeftScreen";
        g_config.ninjabrainOverlay.x = 48;
        g_config.ninjabrainOverlay.y = 40;
        PublishConfigSnapshot();
        RequestDynamicGuiFontRefresh(true);
        StartNinjabrainClient();
        active_ = true;
    }

    ~ScopedVisualNinjabrainPreviewSession() {
        g_config.ninjabrainOverlay = originalOverlay_;
        {
            std::lock_guard<std::mutex> lock(g_modeIdMutex);
            g_currentModeId = originalCurrentModeId_;
            g_modeIdBuffers[0] = originalModeIdBuffers_[0];
            g_modeIdBuffers[1] = originalModeIdBuffers_[1];
        }
        g_currentModeIdIndex.store(originalModeIdIndex_, std::memory_order_release);
        PublishConfigSnapshot();
        RequestDynamicGuiFontRefresh(true);

        if (!active_) {
            g_visualNinjabrainPreviewModeId.clear();
            return;
        }

        StopNinjabrainClient();
        PublishNinjabrainData(NinjabrainData{});
        g_visualNinjabrainPreviewModeId.clear();
    }

  private:
    bool active_ = false;
        NinjabrainOverlayConfig originalOverlay_;
        std::string originalCurrentModeId_;
        std::array<std::string, 2> originalModeIdBuffers_{};
        int originalModeIdIndex_ = 0;
};

bool RenderModeOverlayFrame(DummyWindow& window, const Config& config, const ModeConfig& mode, GLuint gameTextureId = 0,
                            bool renderGui = false, bool expectRendered = true);
const ModeConfig& FindModeOrThrow(std::string_view modeId);

void RenderSettingsFrame(DummyWindow& window, const char* topLevelTabLabel, const char* inputsSubTabLabel = nullptr) {
    if (!window.hasModernGL()) { std::cout << "SKIP (no GL 3.3+)" << std::endl; return; }
    ScopedTabSelection scopedSelection(topLevelTabLabel, inputsSubTabLabel);

    if (!g_visualNinjabrainPreviewModeId.empty()) {
        const ModeConfig& previewMode = FindModeOrThrow(g_visualNinjabrainPreviewModeId);
        RenderModeOverlayFrame(window, g_config, previewMode, 0, true);
        return;
    }

    Expect(window.BeginFrame(), "GUI integration test window closed unexpectedly.");
    RenderSettingsGUI();
    window.EndFrame();
}

void RenderSettingsSearchFrame(DummyWindow& window, const char* searchQuery, const char* topLevelTabLabel = nullptr,
                               const char* inputsSubTabLabel = nullptr) {
    if (!window.hasModernGL()) { std::cout << "SKIP (no GL 3.3+)" << std::endl; return; }

    if (!g_visualNinjabrainPreviewModeId.empty()) {
        RequestGuiTestSetConfigSearchQuery(searchQuery != nullptr ? std::string(searchQuery) : std::string());
        const ModeConfig& previewMode = FindModeOrThrow(g_visualNinjabrainPreviewModeId);
        RenderModeOverlayFrame(window, g_config, previewMode, 0, true);
        return;
    }

    const int frameCount = topLevelTabLabel != nullptr ? 2 : 1;
    for (int frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
        RequestGuiTestSetConfigSearchQuery(searchQuery != nullptr ? std::string(searchQuery) : std::string());

        if (topLevelTabLabel != nullptr) {
            ScopedTabSelection scopedSelection(topLevelTabLabel, inputsSubTabLabel);
            Expect(window.BeginFrame(), "GUI integration test window closed unexpectedly.");
            RenderSettingsGUI();
            window.EndFrame();
            continue;
        }

        Expect(window.BeginFrame(), "GUI integration test window closed unexpectedly.");
        RenderSettingsGUI();
        window.EndFrame();
    }
}

bool HasGuiInteractionRect(const char* id) {
    GuiTestInteractionRect rect;
    return GetGuiTestInteractionRect(id, rect);
}

void ExpectGuiInteractionRectPresence(const char* id, bool expected, const std::string& message) {
    const bool present = HasGuiInteractionRect(id);
    Expect(present == expected, message);
}

void RenderInteractiveSettingsFrame(DummyWindow& window) {
    if (!g_visualNinjabrainPreviewModeId.empty()) {
        const ModeConfig& previewMode = FindModeOrThrow(g_visualNinjabrainPreviewModeId);
        RenderModeOverlayFrame(window, g_config, previewMode, 0, true);
        return;
    }

    if (!window.BeginFrame()) {
        return;
    }

    RenderSettingsGUI();
    window.EndFrame();
}

void RenderConfigErrorFrame(DummyWindow& window, bool presentFrame = false) {
    if (!window.hasModernGL()) { std::cout << "SKIP (no GL 3.3+)" << std::endl; return; }
    Expect(window.PrepareRenderSurface(), "GUI integration test window closed unexpectedly.");
    HandleConfigLoadFailed(nullptr, nullptr);

    const ImDrawData* drawData = ImGui::GetDrawData();
    Expect(drawData != nullptr, "Expected config-error rendering path to produce ImGui draw data.");
    Expect(drawData->DisplaySize.x > 0.0f && drawData->DisplaySize.y > 0.0f,
           "Expected config-error rendering path to populate a valid ImGui display size.");

    if (presentFrame) {
        window.PresentSurface();
    }
}

void RenderInteractiveConfigErrorFrame(DummyWindow& window) {
    RenderConfigErrorFrame(window, true);
}

template <typename RenderFrameFn>
void RunVisualLoop(DummyWindow& window, std::string_view testCaseName, RenderFrameFn&& renderFrame);

template <typename RenderFrameFn>
void RunVisualLoopWithNinjabrainPreview(DummyWindow& window, std::string_view testCaseName, RenderFrameFn&& renderFrame) {
    ScopedVisualNinjabrainPreviewSession previewSession;
    RunVisualLoop(window, testCaseName, std::forward<RenderFrameFn>(renderFrame));
}

constexpr char kPrimaryModeId[] = "Primary Mode";
constexpr char kPrecisionModeId[] = "Precision Mode";
constexpr char kRelativeModeId[] = "Relative Mode";
constexpr char kExpressionModeId[] = "Expression Mode";
constexpr char kVerifierMirrorName[] = "Verifier Mirror";
constexpr char kAuxMirrorName[] = "Aux Mirror";
constexpr char kVerifierGroupName[] = "Verifier Group";
constexpr char kVerifierImageName[] = "Checklist Image";
constexpr char kVerifierWindowOverlayName[] = "Verifier Window";
constexpr char kVerifierBrowserOverlayName[] = "Verifier Browser";

bool NearlyEqual(float actual, float expected, float epsilon = 0.0001f) {
    return std::fabs(actual - expected) <= epsilon;
}

void ExpectFloatNear(float actual, float expected, const std::string& message, float epsilon = 0.0001f) {
    Expect(NearlyEqual(actual, expected, epsilon), message + " (expected " + std::to_string(expected) + ", got " +
                                             std::to_string(actual) + ")");
}

void ExpectColorNear(const Color& actual, const Color& expected, const std::string& message, float epsilon = 0.005f) {
    ExpectFloatNear(actual.r, expected.r, message + " [r]", epsilon);
    ExpectFloatNear(actual.g, expected.g, message + " [g]", epsilon);
    ExpectFloatNear(actual.b, expected.b, message + " [b]", epsilon);
    ExpectFloatNear(actual.a, expected.a, message + " [a]", epsilon);
}

bool IsColorNear(const Color& actual, const Color& expected, float epsilon = 0.02f) {
    return NearlyEqual(actual.r, expected.r, epsilon) && NearlyEqual(actual.g, expected.g, epsilon) &&
           NearlyEqual(actual.b, expected.b, epsilon) && NearlyEqual(actual.a, expected.a, epsilon);
}

std::vector<unsigned char> MakeSolidRgbaPixels(int width, int height, std::uint8_t r, std::uint8_t g, std::uint8_t b,
                                               std::uint8_t a = 255) {
    const size_t byteCount = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
    std::vector<unsigned char> pixels(byteCount);
    for (size_t i = 0; i < byteCount; i += 4) {
        pixels[i + 0] = r;
        pixels[i + 1] = g;
        pixels[i + 2] = b;
        pixels[i + 3] = a;
    }
    return pixels;
}

Color ReadFramebufferPixelColor(int screenX, int screenY, int surfaceHeight) {
    std::array<unsigned char, 4> pixel{ 0, 0, 0, 0 };
    glReadPixels(screenX, surfaceHeight - screenY - 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
    return {
        static_cast<float>(pixel[0]) / 255.0f,
        static_cast<float>(pixel[1]) / 255.0f,
        static_cast<float>(pixel[2]) / 255.0f,
        static_cast<float>(pixel[3]) / 255.0f,
    };
}

void ExpectFramebufferPixelColorNear(int screenX, int screenY, int surfaceHeight, const Color& expected,
                                     const std::string& message, float epsilon = 0.02f) {
    glFinish();
    ExpectColorNear(ReadFramebufferPixelColor(screenX, screenY, surfaceHeight), expected, message, epsilon);
}

void ExpectFramebufferPixelChannelDominance(int screenX, int screenY, int surfaceHeight, int dominantChannel,
                                            float minDominant, float minMargin, const std::string& message) {
    glFinish();

    const Color actual = ReadFramebufferPixelColor(screenX, screenY, surfaceHeight);
    const float channels[3] = { actual.r, actual.g, actual.b };
    Expect(dominantChannel >= 0 && dominantChannel < 3, message + " dominant channel index was invalid.");
    Expect(actual.a >= 0.95f, message + " alpha was lower than expected.");
    Expect(channels[dominantChannel] >= minDominant,
           message + " dominant channel was too low. got " + std::to_string(channels[dominantChannel]));

    for (int i = 0; i < 3; ++i) {
        if (i == dominantChannel) {
            continue;
        }

        Expect(channels[dominantChannel] >= channels[i] + minMargin,
               message + " dominant channel margin was too small. dominant=" +
                   std::to_string(channels[dominantChannel]) + ", other=" + std::to_string(channels[i]));
    }
}

void ExpectFramebufferNeighborhoodContainsColor(int screenX, int screenY, int radius, int surfaceWidth, int surfaceHeight,
                                                const Color& expected, const std::string& message,
                                                float epsilon = 0.02f) {
    glFinish();

    const int minX = (std::max)(0, screenX - radius);
    const int maxX = (std::min)(surfaceWidth - 1, screenX + radius);
    const int minY = (std::max)(0, screenY - radius);
    const int maxY = (std::min)(surfaceHeight - 1, screenY + radius);
    for (int y = minY; y <= maxY; ++y) {
        for (int x = minX; x <= maxX; ++x) {
            if (IsColorNear(ReadFramebufferPixelColor(x, y, surfaceHeight), expected, epsilon)) {
                return;
            }
        }
    }

    throw std::runtime_error(message);
}

struct SurfaceSize {
    int width = 0;
    int height = 0;
};

struct SimulatedOverlayGeometry {
    std::string label;
    int fullW = 1;
    int fullH = 1;
    int gameX = 0;
    int gameY = 0;
    int gameW = 1;
    int gameH = 1;
};

struct MirrorAnchorCaseDefinition {
    std::string name;
    std::string relativeTo;
    int captureWidth = 1;
    int captureHeight = 1;
    int outputX = 0;
    int outputY = 0;
    float scale = 1.0f;
};

struct MirrorAnchorRenderScenario {
    SimulatedOverlayGeometry geometry;
    std::vector<MirrorAnchorCaseDefinition> mirrors;
    bool expectVisibleRender = true;
};

class ScopedFramebufferSurface {
  public:
    ScopedFramebufferSurface(int width, int height) : m_width((std::max)(1, width)), m_height((std::max)(1, height)) {
        GLint previousTexture = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);

        glGenTextures(1, &m_colorTexture);
        Expect(m_colorTexture != 0, "Failed to create offscreen texture for GUI integration tests.");
        BindTextureDirect(GL_TEXTURE_2D, m_colorTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_width, m_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        BindTextureDirect(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));

        GLint previousReadFramebuffer = 0;
        GLint previousDrawFramebuffer = 0;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);

        glGenFramebuffers(1, &m_framebuffer);
        Expect(m_framebuffer != 0, "Failed to create offscreen framebuffer for GUI integration tests.");
        glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_colorTexture, 0);
        const GLenum framebufferStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousReadFramebuffer));
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previousDrawFramebuffer));

        Expect(framebufferStatus == GL_FRAMEBUFFER_COMPLETE,
               "Offscreen framebuffer for GUI integration tests was incomplete.");
    }

    ~ScopedFramebufferSurface() {
        if (m_restoreStateCaptured) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, m_previousReadFramebuffer);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_previousDrawFramebuffer);
            glViewport(m_previousViewport[0], m_previousViewport[1], m_previousViewport[2], m_previousViewport[3]);
        }

        if (m_framebuffer != 0) {
            glDeleteFramebuffers(1, &m_framebuffer);
        }
        if (m_colorTexture != 0) {
            glDeleteTextures(1, &m_colorTexture);
        }
    }

    void BindAndClear() {
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &m_previousReadFramebuffer);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &m_previousDrawFramebuffer);
        glGetIntegerv(GL_VIEWPORT, m_previousViewport);
        m_restoreStateCaptured = true;

        glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
        glViewport(0, 0, m_width, m_height);
        glClearColor(0.08f, 0.08f, 0.10f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    SurfaceSize size() const { return { m_width, m_height }; }

  private:
    GLuint m_framebuffer = 0;
    GLuint m_colorTexture = 0;
    int m_width = 0;
    int m_height = 0;
    GLint m_previousReadFramebuffer = 0;
    GLint m_previousDrawFramebuffer = 0;
    GLint m_previousViewport[4]{ 0, 0, 0, 0 };
    bool m_restoreStateCaptured = false;
};

struct ExpectedMirrorRect {
    std::string name;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

constexpr Color kExpectedMirrorRenderGreen{ 0.0f, 1.0f, 0.0f, 1.0f };
constexpr Color kExpectedRenderSurfaceClear{ 0.08f, 0.08f, 0.10f, 1.0f };
constexpr Color kExpectedPngFixtureColor{ 32.0f / 255.0f, 192.0f / 255.0f, 96.0f / 255.0f, 1.0f };

constexpr std::string_view kEmbeddedPngFixtureBase64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAACXBIWXMAAAABAAAAAQBPJcTWAAAAEklEQVR4nGOU3xfPwMDA"
    "wgAGAA7IAULTTL/NAAAAAElFTkSuQmCC";

constexpr std::string_view kEmbeddedMpegFixtureBase64 =
    "AAABuiEAAQABwzNnAAABuwAJwzNnACH/4ODmAAAB4AQ0MQADe7ERAANfkQAAAbMBABAT///gGAAAAbgACABAAAABAAAP//gA"
    "AAEBE/IUpS+Zv3CAAAABswEAEBP//+AYAAABuAAIAMAAAAEAAA//+AAAAQET8hSlL5m/cIAAAAGzAQAQE///4BgAAAG4AAgB"
    "QAAAAQAAD//4AAABARPyFKUvmb9wgAAAAbMBABAT///gGAAAAbgACAHAAAABAAAP//gAAAEBE/IUpS+Zv3CAAAABswEAEBP/"
    "/+AYAAABuAAIAkAAAAEAAA//+AAAAQET8hSlL5m/cIAAAAGzAQAQE///4BgAAAG4AAgCwAAAAQAAD//4AAABARPyFKUvmb9w"
    "gAAAAbMBABAT///gGAAAAbgACANAAAABAAAP//gAAAEBE/IUpS+Zv3CAAAABswEAEBP//+AYAAABuAAIA8AAAAEAAA//+AAA"
    "AQET8hSlL5m/cIAAAAGzAQAQE///4BgAAAG4AAgEQAAAAQAAD//4AAABARPyFKUvmb9wgAAAAbMBABAT///gGAAAAbgACATA"
    "AAABAAAP//gAAAEBE/IUpS+Zv3CAAAABswEAEBP//+AYAAABuAAIBUAAAAEAAA//+AAAAQET8hSlL5m/cIAAAAGzAQAQE///"
    "4BgAAAG4AAgFwAAAAQAAD//4AAABARPyFKUvmb9wgAAAAbMBABAT///gGAAAAbgACAZAAAABAAAP//gAAAEBE/IUpS+Zv3CA"
    "AAABswEAEBP//+AYAAABuAAIBsAAAAEAAA//+AAAAQET+UUpS/cLzYAAAAGzAQAQE///4BgAAAG4AAgHQAAAAQAAD//4AAAB"
    "ARP5RSlL9wvNgAAAAbMBABAT///gGAAAAbgACAfAAAABAAAP//gAAAEBE/lFKUv3C82AAAABswEAEBP//+AYAAABuAAICEAA"
    "AAEAAA//+AAAAQET+UUpS/cLzYAAAAGzAQAQE///4BgAAAG4AAgIwAAAAQAAD//4AAABARP5RSlL9wvNgAAAAbMBABAT///g"
    "GAAAAbgACAlAAAABAAAP//gAAAEBE/lFKUv3C82AAAABswEAEBP//+AYAAABuAAICcAAAAEAAA//+AAAAQET+UUpS/cLzYAA"
    "AAGzAQAQE///4BgAAAG4AAgKQAAAAQAAD//4AAABARP5RSlL9wvNgAAAAbMBABAT///gGAAAAbgACArAAAABAAAP//gAAAEB"
    "E/lFKUv3C82AAAABswEAEBP//+AYAAABuAAIC0AAAAEAAA//+AAAAQET+UUpS/cLzYAAAAGzAQAQE///4BgAAAG4AAgLwAAA"
    "AQAAD//4AAABARP5RSlL9wvNgAAAAbMBABAT///gGAAAAbgACAxAAAABAAAP//gAAAEBE/lFKUv3C82AAAABswEAEBP//+AY"
    "AAABuAAIIEAAAAEAAA//+AAAAQET+UUpS/cLzYAAAAG+A6UP////////////////////////////////////////////////"
    "////////////////////////////////////////////////////////////////////////////////////////////////"
    "////////////////////////////////////////////////////////////////////////////////////////////////"
    "////////////////////////////////////////////////////////////////////////////////////////////////"
    "////////////////////////////////////////////////////////////////////////////////////////////////"
    "////////////////////////////////////////////////////////////////////////////////////////////////"
    "////////////////////////////////////////////////////////////////////////////////////////////////"
    "////////////////////////////////////////////////////////////////////////////////////////////////"
    "////////////////////////////////////////////////////////////////////////////////////////////////"
    "////////////////////////////////////////////////////////////////////////////////////////////////"
    "////////////////////////////////////////////////////////////////////////////////////////////////"
    "////////////////////////////////////////////////////////////////////////////////////////////////"
    "////////////////////////////////////////////////////////////////////////////////////////////////"
    "//////////////////////////////////////////8=";

int DecodeBase64Value(char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return ch - 'A';
    }
    if (ch >= 'a' && ch <= 'z') {
        return ch - 'a' + 26;
    }
    if (ch >= '0' && ch <= '9') {
        return ch - '0' + 52;
    }
    if (ch == '+') {
        return 62;
    }
    if (ch == '/') {
        return 63;
    }
    return -1;
}

std::vector<unsigned char> DecodeBase64(std::string_view encoded) {
    std::vector<unsigned char> decoded;
    decoded.reserve((encoded.size() * 3u) / 4u);

    unsigned int accumulator = 0;
    int bitCount = -8;
    for (char ch : encoded) {
        if (ch == '=') {
            break;
        }

        const int value = DecodeBase64Value(ch);
        if (value < 0) {
            continue;
        }

        accumulator = (accumulator << 6) | static_cast<unsigned int>(value);
        bitCount += 6;
        if (bitCount >= 0) {
            decoded.push_back(static_cast<unsigned char>((accumulator >> bitCount) & 0xFFu));
            bitCount -= 8;
        }
    }

    return decoded;
}

std::filesystem::path WriteEmbeddedFixtureToDisk(const std::filesystem::path& root, const std::filesystem::path& relativePath,
                                                 std::string_view base64Payload) {
    const std::filesystem::path fixturePath = root / relativePath;
    std::error_code error;
    std::filesystem::create_directories(fixturePath.parent_path(), error);
    Expect(!error, "Failed to create fixture directory: " + Narrow(fixturePath.parent_path().wstring()));

    const std::vector<unsigned char> decodedBytes = DecodeBase64(base64Payload);
    Expect(!decodedBytes.empty(), "Embedded fixture payload decoded to zero bytes for: " + relativePath.generic_string());

    std::ofstream out(fixturePath, std::ios::binary | std::ios::trunc);
    Expect(out.is_open(), "Failed to open embedded fixture path for writing: " + Narrow(fixturePath.wstring()));
    out.write(reinterpret_cast<const char*>(decodedBytes.data()), static_cast<std::streamsize>(decodedBytes.size()));
    out.close();
    Expect(std::filesystem::exists(fixturePath), "Failed to write embedded fixture file: " + Narrow(fixturePath.wstring()));
    return fixturePath;
}

std::filesystem::path WriteSolidBmpFixtureToDisk(const std::filesystem::path& root, const std::filesystem::path& relativePath,
                                                int width, int height, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    Expect(width > 0 && height > 0, "Solid BMP fixture dimensions must be positive.");

    const std::filesystem::path fixturePath = root / relativePath;
    std::error_code error;
    std::filesystem::create_directories(fixturePath.parent_path(), error);
    Expect(!error, "Failed to create BMP fixture directory: " + Narrow(fixturePath.parent_path().wstring()));

    const int bytesPerPixel = 3;
    const int rowStride = ((width * bytesPerPixel) + 3) & ~3;
    const std::uint32_t pixelBytes = static_cast<std::uint32_t>(rowStride * height);
    const std::uint32_t fileSize = 14u + 40u + pixelBytes;

    std::vector<unsigned char> fileBytes(fileSize, 0);
    fileBytes[0] = 'B';
    fileBytes[1] = 'M';
    std::memcpy(fileBytes.data() + 2, &fileSize, sizeof(fileSize));

    const std::uint32_t pixelOffset = 14u + 40u;
    std::memcpy(fileBytes.data() + 10, &pixelOffset, sizeof(pixelOffset));

    const std::uint32_t dibHeaderSize = 40u;
    const std::int32_t dibWidth = width;
    const std::int32_t dibHeight = height;
    const std::uint16_t planes = 1u;
    const std::uint16_t bitsPerPixel = 24u;
    const std::uint32_t compression = 0u;
    const std::uint32_t imageSize = pixelBytes;
    const std::int32_t pixelsPerMeter = 2835;
    const std::uint32_t colorsUsed = 0u;
    const std::uint32_t importantColors = 0u;

    std::memcpy(fileBytes.data() + 14, &dibHeaderSize, sizeof(dibHeaderSize));
    std::memcpy(fileBytes.data() + 18, &dibWidth, sizeof(dibWidth));
    std::memcpy(fileBytes.data() + 22, &dibHeight, sizeof(dibHeight));
    std::memcpy(fileBytes.data() + 26, &planes, sizeof(planes));
    std::memcpy(fileBytes.data() + 28, &bitsPerPixel, sizeof(bitsPerPixel));
    std::memcpy(fileBytes.data() + 30, &compression, sizeof(compression));
    std::memcpy(fileBytes.data() + 34, &imageSize, sizeof(imageSize));
    std::memcpy(fileBytes.data() + 38, &pixelsPerMeter, sizeof(pixelsPerMeter));
    std::memcpy(fileBytes.data() + 42, &pixelsPerMeter, sizeof(pixelsPerMeter));
    std::memcpy(fileBytes.data() + 46, &colorsUsed, sizeof(colorsUsed));
    std::memcpy(fileBytes.data() + 50, &importantColors, sizeof(importantColors));

    for (int y = 0; y < height; ++y) {
        unsigned char* row = fileBytes.data() + pixelOffset + static_cast<size_t>(y) * static_cast<size_t>(rowStride);
        for (int x = 0; x < width; ++x) {
            const size_t pixelOffsetInRow = static_cast<size_t>(x) * static_cast<size_t>(bytesPerPixel);
            row[pixelOffsetInRow + 0] = b;
            row[pixelOffsetInRow + 1] = g;
            row[pixelOffsetInRow + 2] = r;
        }
    }

    std::ofstream out(fixturePath, std::ios::binary | std::ios::trunc);
    Expect(out.is_open(), "Failed to open BMP fixture path for writing: " + Narrow(fixturePath.wstring()));
    out.write(reinterpret_cast<const char*>(fileBytes.data()), static_cast<std::streamsize>(fileBytes.size()));
    out.close();
    Expect(std::filesystem::exists(fixturePath), "Failed to write BMP fixture file: " + Narrow(fixturePath.wstring()));
    return fixturePath;
}

bool WaitForUserImageTextureUpload(const std::string& imageName, int timeoutMs = 2000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        ProcessPendingDecodedImages();
        {
            std::lock_guard<std::mutex> lock(g_userImagesMutex);
            auto it = g_userImages.find(imageName);
            if (it != g_userImages.end() && it->second.textureId != 0) {
                return true;
            }
        }
        Sleep(10);
    }

    return false;
}

ImageConfig MakeTopLeftImageRenderTestConfig(std::string_view name, std::string path, int x, int y, float scale) {
    ImageConfig image;
    image.name = std::string(name);
    image.path = std::move(path);
    image.x = x;
    image.y = y;
    image.scale = scale;
    image.relativeSizing = true;
    image.relativeTo = "topLeftScreen";
    image.opacity = 1.0f;
    image.onlyOnMyScreen = false;
    image.pixelatedScaling = true;
    image.background.enabled = false;
    image.border.enabled = false;
    return image;
}

void ResetOverlayRenderTestResources();

void LoadImageFixtureForRenderTest(DummyWindow& window, const ImageConfig& image) {
    Expect(window.PrepareRenderSurface(), "GUI integration test window closed unexpectedly while preparing image fixture upload.");
    ResetOverlayRenderTestResources();
    LoadImageAsync(DecodedImageData::Type::UserImage, image.name, image.path, g_toolscreenPath);
    Expect(WaitForUserImageTextureUpload(image.name), "Timed out waiting for image fixture upload to reach the GPU.");
}

bool EndsWith(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
}

SurfaceSize GetWindowClientSize(HWND hwnd) {
    RECT clientRect{};
    Expect(hwnd != nullptr && GetClientRect(hwnd, &clientRect) == TRUE, "Failed to read GUI integration test window client rect.");
    return {
        clientRect.right - clientRect.left,
        clientRect.bottom - clientRect.top,
    };
}

class ScopedTexture2D {
  public:
    ScopedTexture2D(int width, int height, const std::vector<unsigned char>& rgbaPixels) {
        Expect(width > 0 && height > 0, "Texture dimensions must be positive.");
        Expect(rgbaPixels.size() == static_cast<size_t>(width) * static_cast<size_t>(height) * 4u,
               "Texture pixel payload size did not match the requested dimensions.");

        glGenTextures(1, &m_textureId);
        Expect(m_textureId != 0, "Failed to create OpenGL texture for mirror integration test.");

        GLint previousTexture = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);

        BindTextureDirect(GL_TEXTURE_2D, m_textureId);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgbaPixels.data());
        BindTextureDirect(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
    }

    ~ScopedTexture2D() {
        if (m_textureId != 0) {
            glDeleteTextures(1, &m_textureId);
        }
    }

    GLuint id() const { return m_textureId; }

  private:
    GLuint m_textureId = 0;
};

MirrorConfig MakeMirrorRenderTestConfig(std::string_view name, int captureWidth, int captureHeight, std::string_view relativeTo,
                                        int outputX, int outputY, float scale) {
    MirrorConfig mirror;
    mirror.name = std::string(name);
    mirror.captureWidth = captureWidth;
    mirror.captureHeight = captureHeight;
    mirror.input = { { 0, 0, "topLeftScreen" } };
    mirror.output.x = outputX;
    mirror.output.y = outputY;
    mirror.output.scale = scale;
    mirror.output.relativeTo = std::string(relativeTo);
    mirror.border.type = MirrorBorderType::Dynamic;
    mirror.border.dynamicThickness = 0;
    mirror.fps = 0;
    mirror.opacity = 1.0f;
    mirror.rawOutput = true;
    mirror.colorPassthrough = false;
    mirror.gradientOutput = false;
    return mirror;
}

MirrorConfig BuildExpectedGroupedMirrorConfig(const MirrorConfig& mirror, const MirrorGroupConfig& group, const MirrorGroupItem& item) {
    MirrorConfig groupedMirror = mirror;
    groupedMirror.output.x = group.output.x + item.offsetX;
    groupedMirror.output.y = group.output.y + item.offsetY;
    groupedMirror.output.relativeTo = group.output.relativeTo;
    groupedMirror.output.useRelativePosition = group.output.useRelativePosition;
    groupedMirror.output.relativeX = group.output.relativeX;
    groupedMirror.output.relativeY = group.output.relativeY;

    groupedMirror.output.separateScale = true;
    const float baseScaleX = mirror.output.separateScale ? mirror.output.scaleX : mirror.output.scale;
    const float baseScaleY = mirror.output.separateScale ? mirror.output.scaleY : mirror.output.scale;
    groupedMirror.output.scaleX = baseScaleX * item.widthPercent;
    groupedMirror.output.scaleY = baseScaleY * item.heightPercent;
    return groupedMirror;
}

ExpectedMirrorRect ComputeExpectedMirrorRect(const MirrorConfig& mirror, int fullW, int fullH, int gameX, int gameY, int gameW, int gameH) {
    const float scaleX = mirror.output.separateScale ? mirror.output.scaleX : mirror.output.scale;
    const float scaleY = mirror.output.separateScale ? mirror.output.scaleY : mirror.output.scale;

    ExpectedMirrorRect rect;
    rect.name = mirror.name;
    rect.width = static_cast<int>(static_cast<float>(mirror.captureWidth) * scaleX);
    rect.height = static_cast<int>(static_cast<float>(mirror.captureHeight) * scaleY);

    std::string anchor = mirror.output.relativeTo;
    bool isScreenRelative = false;
    if (EndsWith(anchor, "Screen")) {
        anchor.resize(anchor.size() - std::string_view("Screen").size());
        isScreenRelative = true;
    } else if (EndsWith(anchor, "Viewport")) {
        anchor.resize(anchor.size() - std::string_view("Viewport").size());
    }

    const int containerX = isScreenRelative ? 0 : gameX;
    const int containerY = isScreenRelative ? 0 : gameY;
    const int containerW = isScreenRelative ? fullW : gameW;
    const int containerH = isScreenRelative ? fullH : gameH;

    if (anchor == "topLeft") {
        rect.x = containerX + mirror.output.x;
        rect.y = containerY + mirror.output.y;
    } else if (anchor == "topRight") {
        rect.x = containerX + containerW - rect.width - mirror.output.x;
        rect.y = containerY + mirror.output.y;
    } else if (anchor == "bottomLeft") {
        rect.x = containerX + mirror.output.x;
        rect.y = containerY + containerH - rect.height - mirror.output.y;
    } else if (anchor == "bottomRight") {
        rect.x = containerX + containerW - rect.width - mirror.output.x;
        rect.y = containerY + containerH - rect.height - mirror.output.y;
    } else if (anchor == "center") {
        rect.x = containerX + (containerW - rect.width) / 2 + mirror.output.x;
        rect.y = containerY + (containerH - rect.height) / 2 + mirror.output.y;
    } else {
        throw std::runtime_error("Unsupported mirror test anchor: " + mirror.output.relativeTo);
    }

    return rect;
}

ExpectedMirrorRect GetCachedMirrorRect(std::string_view mirrorName) {
    std::shared_lock<std::shared_mutex> lock(g_mirrorInstancesMutex);
    auto it = g_mirrorInstances.find(std::string(mirrorName));
    Expect(it != g_mirrorInstances.end(), "Missing cached mirror instance for integration test: " + std::string(mirrorName));
    Expect(it->second.cachedRenderState.isValid, "Missing cached mirror render state for integration test: " + std::string(mirrorName));
    return {
        it->first,
        it->second.cachedRenderState.mirrorScreenX,
        it->second.cachedRenderState.mirrorScreenY,
        it->second.cachedRenderState.mirrorScreenW,
        it->second.cachedRenderState.mirrorScreenH,
    };
}

    void ExpectMirrorRectNear(const ExpectedMirrorRect& actual, const ExpectedMirrorRect& expected, const std::string& label,
                     int tolerance = 2) {
        Expect(std::abs(actual.x - expected.x) <= tolerance,
            label + " x was outside tolerance. expected " + std::to_string(expected.x) + ", got " + std::to_string(actual.x));
        Expect(std::abs(actual.y - expected.y) <= tolerance,
            label + " y was outside tolerance. expected " + std::to_string(expected.y) + ", got " + std::to_string(actual.y));
        Expect(std::abs(actual.width - expected.width) <= tolerance,
            label + " width was outside tolerance. expected " + std::to_string(expected.width) + ", got " +
             std::to_string(actual.width));
        Expect(std::abs(actual.height - expected.height) <= tolerance,
            label + " height was outside tolerance. expected " + std::to_string(expected.height) + ", got " +
             std::to_string(actual.height));
    }

void ExpectSolidColorRect(const ExpectedMirrorRect& rect, int surfaceHeight, const Color& expected, const std::string& label) {
    Expect(rect.width > 0 && rect.height > 0, label + " must have positive dimensions.");

    const int insetX = rect.width > 4 ? rect.width / 5 : 0;
    const int insetY = rect.height > 4 ? rect.height / 5 : 0;
    const int left = rect.x + insetX;
    const int right = rect.x + rect.width - 1 - insetX;
    const int top = rect.y + insetY;
    const int bottom = rect.y + rect.height - 1 - insetY;
    const int centerX = rect.x + rect.width / 2;
    const int centerY = rect.y + rect.height / 2;

    ExpectFramebufferPixelColorNear(centerX, centerY, surfaceHeight, expected, label + " should match at center.");
    ExpectFramebufferPixelColorNear(left, top, surfaceHeight, expected, label + " should match at top-left interior sample.");
    ExpectFramebufferPixelColorNear(right, top, surfaceHeight, expected, label + " should match at top-right interior sample.");
    ExpectFramebufferPixelColorNear(left, bottom, surfaceHeight, expected, label + " should match at bottom-left interior sample.");
    ExpectFramebufferPixelColorNear(right, bottom, surfaceHeight, expected, label + " should match at bottom-right interior sample.");
}

void ExpectBackgroundPixel(int screenX, int screenY, int surfaceHeight, const std::string& label) {
    ExpectFramebufferPixelColorNear(screenX, screenY, surfaceHeight, kExpectedRenderSurfaceClear, label);
}

void ResetOverlayRenderTestResources();

void ExpectMirrorRenderMatchesExpectedPlacement(const MirrorConfig& mirror, const SimulatedOverlayGeometry& geometry,
                                                const SurfaceSize& surface, const std::string& label,
                                                bool expectVisibleRender) {
    const ExpectedMirrorRect expectedRect = ComputeExpectedMirrorRect(mirror, surface.width, surface.height, geometry.gameX,
                                                                      geometry.gameY, geometry.gameW, geometry.gameH);

    Expect(expectedRect.width > 0 && expectedRect.height > 0,
           label + " should resolve to a positive mirror size on the simulated surface.");
    Expect(expectedRect.x >= 0 && expectedRect.y >= 0 && expectedRect.x + expectedRect.width <= surface.width &&
               expectedRect.y + expectedRect.height <= surface.height,
           label + " should remain fully inside the simulated surface.");

    (void)expectVisibleRender;
}

void InitializeMirrorRenderTestResources() {
    if (!g_hasModernGL) return;
    ResetOverlayRenderTestResources();
    for (const auto& mirror : g_config.mirrors) {
        CreateMirrorGPUResources(mirror);
    }
}

void ResetOverlayRenderTestResources() {
    if (!g_hasModernGL) return;
    InvalidateConfigLookupCaches();
    g_windowOverlaysVisible.store(true, std::memory_order_release);
    g_browserOverlaysVisible.store(true, std::memory_order_release);
    CleanupBrowserOverlayCache();
    CleanupWindowOverlayCache();
    CleanupGPUResources();
    CleanupShaders();
    InitializeGPUResources();
}

bool RenderModeOverlayFrame(DummyWindow& window, const Config& config, const ModeConfig& mode, GLuint gameTextureId,
                            bool renderGui, bool expectRendered) {
    if (!window.hasModernGL()) {
        std::cout << "SKIP (no GL 3.3+)" << std::endl;
        return false;
    }
    Expect(window.PrepareRenderSurface(), "GUI integration test window closed unexpectedly.");

    GLState state{};
    SaveGLState(&state);

    const int surfaceWidth = (std::max)(1, GetCachedWindowWidth());
    const int surfaceHeight = (std::max)(1, GetCachedWindowHeight());
    const bool rendered = RenderModeOverlaysForIntegrationTest(config, mode, state, surfaceWidth, surfaceHeight, 0, 0,
                                                               surfaceWidth, surfaceHeight, false, gameTextureId, renderGui);
    if (expectRendered) {
        Expect(rendered, "Expected mode overlay render path to produce overlay output.");
    }
    return rendered;
}

template <typename AssertFn>
void RenderModeOverlayFrameToSimulatedSurface(DummyWindow& window, const Config& config, const ModeConfig& mode,
                                              const SimulatedOverlayGeometry& geometry, GLuint gameTextureId,
                                              AssertFn&& assertFn) {
    if (!window.hasModernGL()) { std::cout << "SKIP (no GL 3.3+)" << std::endl; return; }
    Expect(window.PrepareRenderSurface(), "GUI integration test window closed unexpectedly.");

    ScopedFramebufferSurface surface(geometry.fullW, geometry.fullH);
    surface.BindAndClear();

    GLState state{};
    SaveGLState(&state);

    const bool rendered = RenderModeOverlaysForIntegrationTest(config, mode, state, (std::max)(1, geometry.fullW),
                                                               (std::max)(1, geometry.fullH), geometry.gameX, geometry.gameY,
                                                               (std::max)(1, geometry.gameW), (std::max)(1, geometry.gameH),
                                                               false, gameTextureId);
    Expect(rendered, "Expected simulated mode overlay render path to produce overlay output.");

    glFinish();
    assertFn(surface.size());
}

template <typename AssertFn>
void RenderModeOverlayFrameWithGeometry(DummyWindow& window, const Config& config, const ModeConfig& mode,
                                        const SimulatedOverlayGeometry& geometry, GLuint gameTextureId,
                                        AssertFn&& assertFn) {
    if (!window.hasModernGL()) { std::cout << "SKIP (no GL 3.3+)" << std::endl; return; }
    Expect(window.PrepareRenderSurface(), "GUI integration test window closed unexpectedly.");

    const SurfaceSize surface = GetWindowClientSize(window.hwnd());

    GLState state{};
    SaveGLState(&state);

    const bool rendered = RenderModeOverlaysForIntegrationTest(config, mode, state, surface.width, surface.height,
                                                               geometry.gameX, geometry.gameY, geometry.gameW,
                                                               geometry.gameH, false, gameTextureId);
    Expect(rendered, "Expected window-backed mode overlay render path to produce overlay output.");

    glFinish();
    assertFn(surface);
}

template <typename T>
void ExpectVectorEquals(const std::vector<T>& actual, const std::vector<T>& expected, const std::string& message) {
    Expect(actual == expected, message);
}

std::filesystem::path GetCurrentConfigPath() {
    return std::filesystem::path(g_toolscreenPath) / "config.toml";
}

void ResetRuntimeStateForReload() {
    g_config = Config();
    g_sharedConfig = Config();
    g_configIsDirty.store(false, std::memory_order_release);
    g_configLoadFailed.store(false, std::memory_order_release);
    g_configLoaded.store(false, std::memory_order_release);
    g_showGui.store(true, std::memory_order_release);
    g_welcomeToastVisible.store(false, std::memory_order_release);
    g_configurePromptDismissedThisSession.store(false, std::memory_order_release);
    g_screenshotRequested.store(false, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lock(g_modeIdMutex);
        g_currentModeId.clear();
        g_modeIdBuffers[0].clear();
        g_modeIdBuffers[1].clear();
    }
    g_currentModeIdIndex.store(0, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lock(g_configErrorMutex);
        g_configLoadError.clear();
    }

    ResetTransientBindingUiState();
}

void ReloadConfigFromDisk() {
    ResetRuntimeStateForReload();
    LoadConfig();
}

void SaveAndReloadCurrentConfig() {
    g_configIsDirty.store(true, std::memory_order_release);
    SaveConfigImmediate();
    ReloadConfigFromDisk();
}

void WriteConfigFixtureToDisk(const Config& config) {
    g_config = config;
    Expect(SaveConfigToTomlFile(config, GetCurrentConfigPath().wstring()),
           "Failed to serialize config fixture to disk.");
    Expect(std::filesystem::exists(GetCurrentConfigPath()), "Failed to write config fixture to disk.");
}

void WriteRawConfigTomlToDisk(std::string_view tomlText) {
    const std::filesystem::path configPath = GetCurrentConfigPath();
    std::ofstream out(configPath, std::ios::binary | std::ios::trunc);
    Expect(out.is_open(), "Failed to open config fixture for raw TOML write.");
    out << tomlText;
    out.close();
    Expect(std::filesystem::exists(configPath), "Failed to write raw config fixture to disk.");
}

void ExpectConfigLoadSucceeded(const std::string& context) {
    std::string loadError;
    {
        std::lock_guard<std::mutex> lock(g_configErrorMutex);
        loadError = g_configLoadError;
    }

    Expect(!g_configLoadFailed.load(std::memory_order_acquire),
           context + " should load without config errors." + (loadError.empty() ? std::string() : (" Error: " + loadError)));
    Expect(g_configLoaded.load(std::memory_order_acquire), context + " should mark the config as loaded.");
}

const ModeConfig& FindModeOrThrow(std::string_view modeId) {
    for (const auto& mode : g_config.modes) {
        if (EqualsIgnoreCase(mode.id, std::string(modeId))) {
            return mode;
        }
    }

    throw std::runtime_error("Missing mode: " + std::string(modeId));
}

std::vector<std::string> SourceIdsOfType(const ModeConfig& mode, ModeSourceType type) {
    std::vector<std::string> ids;
    for (const auto& source : mode.sources) {
        if (source.type == type) { ids.push_back(source.id); }
    }
    return ids;
}

const MirrorConfig& FindMirrorOrThrow(std::string_view mirrorName) {
    for (const auto& mirror : g_config.mirrors) {
        if (EqualsIgnoreCase(mirror.name, std::string(mirrorName))) {
            return mirror;
        }
    }

    throw std::runtime_error("Missing mirror: " + std::string(mirrorName));
}

const MirrorGroupConfig& FindMirrorGroupOrThrow(std::string_view groupName) {
    for (const auto& group : g_config.mirrorGroups) {
        if (EqualsIgnoreCase(group.name, std::string(groupName))) {
            return group;
        }
    }

    throw std::runtime_error("Missing mirror group: " + std::string(groupName));
}

const ImageConfig& FindImageOrThrow(std::string_view imageName) {
    for (const auto& image : g_config.images) {
        if (EqualsIgnoreCase(image.name, std::string(imageName))) {
            return image;
        }
    }

    throw std::runtime_error("Missing image: " + std::string(imageName));
}

const WindowOverlayConfig& FindWindowOverlayOrThrow(std::string_view overlayName) {
    for (const auto& overlay : g_config.windowOverlays) {
        if (EqualsIgnoreCase(overlay.name, std::string(overlayName))) {
            return overlay;
        }
    }

    throw std::runtime_error("Missing window overlay: " + std::string(overlayName));
}

const BrowserOverlayConfig& FindBrowserOverlayOrThrow(std::string_view overlayName) {
    for (const auto& overlay : g_config.browserOverlays) {
        if (EqualsIgnoreCase(overlay.name, std::string(overlayName))) {
            return overlay;
        }
    }

    throw std::runtime_error("Missing browser overlay: " + std::string(overlayName));
}

const HotkeyConfig& FindHotkeyBySecondaryModeOrThrow(std::string_view secondaryModeId) {
    for (const auto& hotkey : g_config.hotkeys) {
        if (EqualsIgnoreCase(hotkey.secondaryMode, std::string(secondaryModeId))) {
            return hotkey;
        }
    }

    throw std::runtime_error("Missing hotkey for mode: " + std::string(secondaryModeId));
}

const HotkeyConfig& FindHotkeyByKeysOrThrow(const std::vector<DWORD>& keys) {
    for (const auto& hotkey : g_config.hotkeys) {
        if (hotkey.keys == keys) {
            return hotkey;
        }
    }

    throw std::runtime_error("Missing hotkey fixture.");
}

const SensitivityHotkeyConfig& FindSensitivityHotkeyOrThrow(const std::vector<DWORD>& keys) {
    for (const auto& hotkey : g_config.sensitivityHotkeys) {
        if (hotkey.keys == keys) {
            return hotkey;
        }
    }

    throw std::runtime_error("Missing sensitivity hotkey fixture.");
}

void PopulateRichConfigFixture() {
    g_config.lang = "zh_CN";
    g_config.fontPath = "C:\\Windows\\Fonts\\consola.ttf";
    g_config.fpsLimit = 144;
    g_config.fpsLimitSleepThreshold = 7;
    g_config.mirrorGammaMode = MirrorGammaMode::AssumeLinear;
    g_config.disableHookChaining = true;
    g_config.allowCursorEscape = true;
    g_config.mouseSensitivity = 1.75f;
    g_config.windowsMouseSpeed = 13;
    g_config.hideAnimationsInGame = true;
    g_config.limitCaptureFramerate = false;
    g_config.obsFramerate = 73;
    g_config.useSystemKeyRepeat = true;
    g_config.keyRepeatStartDelay = 275;
    g_config.keyRepeatDelay = 4;
    g_config.basicModeEnabled = false;
    g_config.restoreWindowedModeOnFullscreenExit = false;
    g_config.disableFullscreenPrompt = true;
    g_config.startupIndicatorMode = 2;
    g_config.startupIndicatorImagePath = "C:\\images\\startup.gif";
    g_config.guiHotkey = { VK_CONTROL, VK_SHIFT, 'G' };
    g_config.borderlessHotkey = { VK_MENU, VK_RETURN };
    g_config.autoBorderless = true;
    g_config.imageOverlaysHotkey = { VK_F8 };
    g_config.windowOverlaysHotkey = { VK_F7 };
    g_config.ninjabrainOverlayHotkey = { VK_F10 };

    g_config.debug.showPerformanceOverlay = true;
    g_config.debug.showProfiler = true;
    g_config.debug.profilerScale = 1.25f;
    g_config.debug.showHotkeyDebug = true;
    g_config.debug.fakeCursor = true;
    g_config.debug.showTextureGrid = true;
    g_config.debug.delayRenderingUntilFinished = true;
    g_config.debug.virtualCameraEnabled = true;
    g_config.debug.videoCacheBudgetMiB = 384;
    g_config.debug.logModeSwitch = true;
    g_config.debug.logAnimation = true;
    g_config.debug.logHotkey = true;
    g_config.debug.logObs = true;
    g_config.debug.logWindowOverlay = true;
    g_config.debug.logFileMonitor = true;
    g_config.debug.logImageMonitor = true;
    g_config.debug.logPerformance = true;
    g_config.debug.logTextureOps = true;
    g_config.debug.logGui = true;
    g_config.debug.logInit = true;
    g_config.debug.logCursorTextures = true;

    g_config.cursors.enabled = true;
    g_config.cursors.title.cursorName = "title.cur";
    g_config.cursors.title.cursorSize = 64;
    g_config.cursors.wall.cursorName = "wall.cur";
    g_config.cursors.wall.cursorSize = 72;
    g_config.cursors.ingame.cursorName = "ingame.cur";
    g_config.cursors.ingame.cursorSize = 88;

    g_config.eyezoom.cloneWidth = 28;
    g_config.eyezoom.overlayWidth = 9;
    g_config.eyezoom.cloneHeight = 1500;
    g_config.eyezoom.stretchWidth = 600;
    g_config.eyezoom.windowWidth = 420;
    g_config.eyezoom.windowHeight = 9000;
    g_config.eyezoom.zoomAreaWidth = 222;
    g_config.eyezoom.zoomAreaHeight = 777;
    g_config.eyezoom.useCustomSizePosition = true;
    g_config.eyezoom.positionX = 33;
    g_config.eyezoom.positionY = 44;
    g_config.eyezoom.fontSizeMode = EyeZoomFontSizeMode::Manual;
    g_config.eyezoom.textFontSize = 31;
    g_config.eyezoom.textFontPath = "C:\\Windows\\Fonts\\verdana.ttf";
    g_config.eyezoom.rectHeight = 35;
    g_config.eyezoom.linkRectToFont = false;
    g_config.eyezoom.gridColor1 = { 0.2f, 0.4f, 0.6f, 1.0f };
    g_config.eyezoom.gridColor1Opacity = 0.8f;
    g_config.eyezoom.gridColor2 = { 0.6f, 0.3f, 0.2f, 1.0f };
    g_config.eyezoom.gridColor2Opacity = 0.7f;
    g_config.eyezoom.centerLineColor = { 0.9f, 0.8f, 0.1f, 1.0f };
    g_config.eyezoom.centerLineColorOpacity = 0.65f;
    g_config.eyezoom.textColor = { 0.1f, 0.2f, 0.3f, 1.0f };
    g_config.eyezoom.textColorOpacity = 0.95f;
    g_config.eyezoom.slideZoomIn = true;
    g_config.eyezoom.slideMirrorsIn = true;
    g_config.eyezoom.overlays = {
        { "Overlay One", "C:\\temp\\overlay-one.png", EyeZoomOverlayDisplayMode::Fit, 100, 100, false, 0.5f },
        { "Overlay Two", "C:\\temp\\overlay-two.png", EyeZoomOverlayDisplayMode::Manual, 240, 140, false, 0.85f },
    };
    g_config.eyezoom.activeOverlayIndex = 1;

    g_config.keyRebinds.enabled = true;
    g_config.keyRebinds.resolveRebindTargetsForHotkeys = false;
    g_config.keyRebinds.toggleHotkey = { VK_F4 };
    KeyRebind rebind;
    rebind.fromKey = 'J';
    rebind.toKey = 'K';
    rebind.enabled = true;
    rebind.triggerOutputDisabled = true;
    rebind.useCustomOutput = true;
    rebind.baseOutputDisabled = true;
    rebind.cursorState = kKeyRebindCursorStateCursorGrabbed;
    rebind.customOutputVK = 'L';
    rebind.customOutputUnicode = 0x00F8;
    rebind.customOutputScanCode = 0x26;
    rebind.baseOutputShifted = true;
    rebind.shiftLayerEnabled = true;
    rebind.shiftLayerUsesCapsLock = true;
    rebind.shiftLayerOutputDisabled = true;
    rebind.shiftLayerOutputVK = 'P';
    rebind.shiftLayerOutputUnicode = 0x00D8;
    rebind.shiftLayerOutputShifted = true;
    g_config.keyRebinds.rebinds = { rebind };

    g_config.appearance.theme = "Sunrise";
    g_config.appearance.customColors["WindowBg"] = { 0.1f, 0.11f, 0.12f, 1.0f };
    g_config.appearance.customColors["Header"] = { 0.8f, 0.4f, 0.2f, 1.0f };

    g_config.ninjabrainOverlay.showTitleBar = true;
    g_config.ninjabrainOverlay.titleText = "Stronghold Helper";
    g_config.ninjabrainOverlay.bgEnabled = true;
    g_config.ninjabrainOverlay.bgOpacity = 0.83f;
    g_config.ninjabrainOverlay.bgColor = { 0.11f, 0.12f, 0.13f, 1.0f };
    g_config.ninjabrainOverlay.chromeColor = { 0.18f, 0.19f, 0.21f, 1.0f };
    g_config.ninjabrainOverlay.headerFillColor = { 0.22f, 0.23f, 0.25f, 1.0f };
    g_config.ninjabrainOverlay.throwsBackgroundColor = { 0.14f, 0.15f, 0.16f, 1.0f };
    g_config.ninjabrainOverlay.dividerColor = { 0.30f, 0.31f, 0.33f, 1.0f };
    g_config.ninjabrainOverlay.headerDividerColor = { 0.42f, 0.43f, 0.45f, 1.0f };
    g_config.ninjabrainOverlay.dataColor = { 0.91f, 0.92f, 0.93f, 1.0f };
    g_config.ninjabrainOverlay.titleTextColor = { 0.97f, 0.98f, 0.99f, 1.0f };
    g_config.ninjabrainOverlay.throwsTextColor = { 0.84f, 0.85f, 0.86f, 1.0f };
    g_config.ninjabrainOverlay.divineTextColor = { 0.74f, 0.82f, 0.91f, 1.0f };
    g_config.ninjabrainOverlay.versionTextColor = { 0.65f, 0.66f, 0.67f, 1.0f };
    g_config.ninjabrainOverlay.showDirectionToStronghold = false;
    g_config.ninjabrainOverlay.textColor = { 0.55f, 0.56f, 0.57f, 1.0f };
    g_config.ninjabrainOverlay.outlineColor = { 0.08f, 0.09f, 0.10f, 0.72f };
    g_config.ninjabrainOverlay.certaintyColor = { 0.12f, 0.98f, 0.34f, 1.0f };
    g_config.ninjabrainOverlay.certaintyMidColor = { 0.94f, 0.88f, 0.20f, 1.0f };
    g_config.ninjabrainOverlay.certaintyLowColor = { 0.82f, 0.18f, 0.22f, 1.0f };
    g_config.ninjabrainOverlay.subpixelPositiveColor = { 0.32f, 0.87f, 0.41f, 1.0f };
    g_config.ninjabrainOverlay.subpixelNegativeColor = { 0.81f, 0.34f, 0.37f, 1.0f };
    g_config.ninjabrainOverlay.coordPositiveColor = { 0.61f, 0.72f, 0.83f, 1.0f };
    g_config.ninjabrainOverlay.coordNegativeColor = { 0.73f, 0.24f, 0.27f, 1.0f };
    g_config.ninjabrainOverlay.resultsMarginLeft = 9.0f;
    g_config.ninjabrainOverlay.resultsMarginRight = 5.0f;
    g_config.ninjabrainOverlay.informationMessagesMarginLeft = 17.0f;
    g_config.ninjabrainOverlay.informationMessagesMarginRight = 11.0f;
    g_config.ninjabrainOverlay.throwsMarginLeft = 7.0f;
    g_config.ninjabrainOverlay.throwsMarginRight = 15.0f;
    g_config.ninjabrainOverlay.failureMarginLeft = 21.0f;
    g_config.ninjabrainOverlay.failureMarginRight = 13.0f;
    g_config.ninjabrainOverlay.sidePadding = 18.0f;
    g_config.ninjabrainOverlay.eyeThrowRows = 6;
    g_config.ninjabrainOverlay.showBoatStateInTopBar = true;
    g_config.ninjabrainOverlay.boatStateSize = 26.0f;
    g_config.ninjabrainOverlay.boatStateMarginRight = 14.0f;
    g_config.ninjabrainOverlay.hideIfStale = true;
    g_config.ninjabrainOverlay.hideIfStaleDelaySeconds = 37;

    MirrorConfig verifierMirror;
    verifierMirror.name = kVerifierMirrorName;
    verifierMirror.captureWidth = 73;
    verifierMirror.captureHeight = 41;
    verifierMirror.input = {
        { 10, 20, "centerViewport" },
        { -5, 8, "topLeftScreen" },
    };
    verifierMirror.output.x = 30;
    verifierMirror.output.y = -40;
    verifierMirror.output.useRelativePosition = true;
    verifierMirror.output.relativeX = 0.25f;
    verifierMirror.output.relativeY = 0.75f;
    verifierMirror.output.scale = 1.25f;
    verifierMirror.output.separateScale = true;
    verifierMirror.output.scaleX = 0.9f;
    verifierMirror.output.scaleY = 1.1f;
    verifierMirror.output.relativeTo = "centerViewport";
    verifierMirror.colors.targetColors = {
        { 1.0f, 0.0f, 0.0f, 1.0f },
        { 0.0f, 1.0f, 0.0f, 1.0f },
    };
    verifierMirror.colors.output = { 0.1f, 0.2f, 0.3f, 0.4f };
    verifierMirror.colors.border = { 0.7f, 0.8f, 0.2f, 1.0f };
    verifierMirror.colorSensitivity = 0.123f;
    verifierMirror.border.type = MirrorBorderType::Static;
    verifierMirror.border.staticShape = MirrorBorderShape::Circle;
    verifierMirror.border.staticColor = { 0.4f, 0.5f, 0.6f, 1.0f };
    verifierMirror.border.staticThickness = 5;
    verifierMirror.border.staticRadius = 18;
    verifierMirror.border.staticOffsetX = 7;
    verifierMirror.border.staticOffsetY = -4;
    verifierMirror.border.staticWidth = 91;
    verifierMirror.border.staticHeight = 87;
    verifierMirror.fps = 48;
    verifierMirror.opacity = 0.654f;
    verifierMirror.rawOutput = true;
    verifierMirror.colorPassthrough = true;
    verifierMirror.gradientOutput = true;
    verifierMirror.gradient.gradientStops = {
        { { 0.2f, 0.1f, 0.8f, 1.0f }, 0.0f },
        { { 0.8f, 0.9f, 0.2f, 1.0f }, 1.0f },
    };
    verifierMirror.gradient.gradientAngle = 42.0f;
    verifierMirror.gradient.gradientAnimation = GradientAnimationType::Rotate;
    verifierMirror.gradient.gradientAnimationSpeed = 1.7f;
    verifierMirror.gradient.gradientColorFade = true;
    verifierMirror.onlyOnMyScreen = true;

    MirrorConfig auxMirror;
    auxMirror.name = kAuxMirrorName;
    auxMirror.captureWidth = 88;
    auxMirror.captureHeight = 66;
    auxMirror.input = { { 3, 4, "centerViewport" } };
    auxMirror.output.x = 12;
    auxMirror.output.y = 18;
    auxMirror.output.scale = 0.95f;
    auxMirror.output.relativeTo = "topLeftScreen";
    auxMirror.fps = 30;
    auxMirror.opacity = 0.95f;

    g_config.mirrors.push_back(verifierMirror);
    g_config.mirrors.push_back(auxMirror);

    MirrorGroupConfig group;
    group.name = kVerifierGroupName;
    group.output.x = 15;
    group.output.y = 25;
    group.output.useRelativePosition = true;
    group.output.relativeX = 0.5f;
    group.output.relativeY = 0.4f;
    group.output.separateScale = true;
    group.output.scaleX = 1.3f;
    group.output.scaleY = 0.7f;
    group.output.relativeTo = "topLeftScreen";
    group.mirrors = {
        { kVerifierMirrorName, true, 0.6f, 0.4f, 10, -10 },
        { kAuxMirrorName, false, 0.4f, 0.6f, -20, 30 },
    };
    g_config.mirrorGroups.push_back(group);

    ImageConfig image;
    image.name = kVerifierImageName;
    image.path = "C:\\temp\\checklist.png";
    image.x = 11;
    image.y = 22;
    image.scale = 1.6f;
    image.relativeSizing = false;
    image.width = 320;
    image.height = 180;
    image.relativeTo = "centerViewport";
    image.crop_top = 3;
    image.crop_bottom = 4;
    image.crop_left = 5;
    image.crop_right = 6;
    image.enableColorKey = true;
    image.colorKeys = {
        { { 1.0f, 0.0f, 1.0f, 1.0f }, 0.02f },
        { { 0.0f, 1.0f, 1.0f, 1.0f }, 0.03f },
    };
    image.opacity = 0.81f;
    image.background.enabled = true;
    image.background.color = { 0.2f, 0.3f, 0.4f, 1.0f };
    image.background.opacity = 0.33f;
    image.pixelatedScaling = true;
    image.onlyOnMyScreen = true;
    image.border.enabled = true;
    image.border.color = { 0.9f, 0.5f, 0.1f, 1.0f };
    image.border.width = 7;
    image.border.radius = 12;
    g_config.images.push_back(image);

    WindowOverlayConfig windowOverlay;
    windowOverlay.name = kVerifierWindowOverlayName;
    windowOverlay.windowTitle = "Speedrun Timer";
    windowOverlay.windowClass = "TimerClass";
    windowOverlay.executableName = "Timer.exe";
    windowOverlay.windowMatchPriority = "title_executable";
    windowOverlay.x = -30;
    windowOverlay.y = 45;
    windowOverlay.scale = 1.2f;
    windowOverlay.relativeTo = "centerViewport";
    windowOverlay.crop_top = 4;
    windowOverlay.crop_bottom = 5;
    windowOverlay.crop_left = 6;
    windowOverlay.crop_right = 7;
    windowOverlay.enableColorKey = true;
    windowOverlay.colorKeys = {
        { { 0.1f, 0.9f, 0.1f, 1.0f }, 0.05f },
    };
    windowOverlay.opacity = 0.71f;
    windowOverlay.background.enabled = true;
    windowOverlay.background.color = { 0.05f, 0.06f, 0.07f, 1.0f };
    windowOverlay.background.opacity = 0.4f;
    windowOverlay.pixelatedScaling = true;
    windowOverlay.onlyOnMyScreen = true;
    windowOverlay.fps = 27;
    windowOverlay.searchInterval = 2500;
    windowOverlay.captureMethod = "BitBlt";
    windowOverlay.forceUpdate = true;
    windowOverlay.enableInteraction = true;
    windowOverlay.border.enabled = true;
    windowOverlay.border.color = { 0.4f, 0.7f, 0.9f, 1.0f };
    windowOverlay.border.width = 3;
    windowOverlay.border.radius = 8;
    g_config.windowOverlays.push_back(windowOverlay);

    BrowserOverlayConfig browserOverlay;
    browserOverlay.name = kVerifierBrowserOverlayName;
    browserOverlay.url = "https://example.com/dashboard";
    browserOverlay.customCss = "body { background: transparent; }";
    browserOverlay.browserWidth = 1024;
    browserOverlay.browserHeight = 576;
    browserOverlay.x = 9;
    browserOverlay.y = 19;
    browserOverlay.scale = 1.15f;
    browserOverlay.relativeTo = "centerViewport";
    browserOverlay.crop_top = 8;
    browserOverlay.crop_bottom = 7;
    browserOverlay.crop_left = 6;
    browserOverlay.crop_right = 5;
    browserOverlay.enableColorKey = true;
    browserOverlay.colorKeys = {
        { { 0.9f, 0.2f, 0.2f, 1.0f }, 0.04f },
    };
    browserOverlay.opacity = 0.67f;
    browserOverlay.background.enabled = true;
    browserOverlay.background.color = { 0.1f, 0.11f, 0.12f, 1.0f };
    browserOverlay.background.opacity = 0.44f;
    browserOverlay.pixelatedScaling = true;
    browserOverlay.onlyOnMyScreen = true;
    browserOverlay.fps = 25;
    browserOverlay.transparentBackground = true;
    browserOverlay.muteAudio = false;
    browserOverlay.allowSystemMediaKeys = false;
    browserOverlay.reloadOnUpdate = true;
    browserOverlay.reloadInterval = 2500;
    browserOverlay.border.enabled = true;
    browserOverlay.border.color = { 0.8f, 0.3f, 0.2f, 1.0f };
    browserOverlay.border.width = 4;
    browserOverlay.border.radius = 10;
    g_config.browserOverlays.push_back(browserOverlay);

    ModeConfig primaryMode;
    primaryMode.id = kPrimaryModeId;
    primaryMode.width = 1280;
    primaryMode.height = 720;
    primaryMode.manualWidth = 1280;
    primaryMode.manualHeight = 720;
    primaryMode.background.selectedMode = "gradient";
    primaryMode.background.imageFit = "fit";
    primaryMode.background.gradientStops = {
        { { 0.15f, 0.2f, 0.35f, 1.0f }, 0.0f },
        { { 0.7f, 0.4f, 0.2f, 1.0f }, 1.0f },
    };
    primaryMode.background.gradientAngle = 55.0f;
    primaryMode.background.gradientAnimation = GradientAnimationType::Slide;
    primaryMode.background.gradientAnimationSpeed = 1.3f;
    primaryMode.background.gradientColorFade = true;
    primaryMode.sources = {
        { ModeSourceType::Mirror, kVerifierMirrorName },
        { ModeSourceType::MirrorGroup, kVerifierGroupName },
        { ModeSourceType::Image, kVerifierImageName },
        { ModeSourceType::WindowOverlay, kVerifierWindowOverlayName },
        { ModeSourceType::BrowserOverlay, kVerifierBrowserOverlayName },
    };
    primaryMode.stretch.enabled = true;
    primaryMode.stretch.width = 1400;
    primaryMode.stretch.height = 800;
    primaryMode.stretch.x = 15;
    primaryMode.stretch.y = 25;
    primaryMode.gameTransition = GameTransitionType::Bounce;
    primaryMode.overlayTransition = OverlayTransitionType::Cut;
    primaryMode.backgroundTransition = BackgroundTransitionType::Cut;
    primaryMode.transitionDurationMs = 777;
    primaryMode.easeInPower = 2.5f;
    primaryMode.easeOutPower = 4.25f;
    primaryMode.bounceCount = 3;
    primaryMode.bounceIntensity = 0.42f;
    primaryMode.bounceDurationMs = 333;
    primaryMode.relativeStretching = true;
    primaryMode.skipAnimateX = true;
    primaryMode.skipAnimateY = false;
    primaryMode.border.enabled = true;
    primaryMode.border.color = { 0.3f, 0.4f, 0.5f, 1.0f };
    primaryMode.border.width = 9;
    primaryMode.border.radius = 14;
    primaryMode.sensitivityOverrideEnabled = true;
    primaryMode.modeSensitivity = 0.88f;
    primaryMode.separateXYSensitivity = true;
    primaryMode.modeSensitivityX = 0.91f;
    primaryMode.modeSensitivityY = 0.72f;
    primaryMode.slideMirrorsIn = true;
    g_config.modes.push_back(primaryMode);

    ModeConfig precisionMode;
    precisionMode.id = kPrecisionModeId;
    precisionMode.width = 960;
    precisionMode.height = 540;
    precisionMode.manualWidth = 960;
    precisionMode.manualHeight = 540;
    precisionMode.background.selectedMode = "color";
    precisionMode.background.color = { 0.02f, 0.03f, 0.04f, 1.0f };
    precisionMode.sources = { { ModeSourceType::Mirror, kAuxMirrorName } };
    g_config.modes.push_back(precisionMode);

    g_config.defaultMode = kPrimaryModeId;

    HotkeyConfig hotkey;
    hotkey.keys = { VK_F6, 'Q' };
    hotkey.mainMode = "Fullscreen";
    hotkey.secondaryMode = kPrimaryModeId;
    hotkey.altSecondaryModes = { { { 'E', 'R' }, kPrecisionModeId } };
    hotkey.conditions.gameState = { "ingame", "wall" };
    hotkey.conditions.exclusions = { VK_LSHIFT, VK_RBUTTON };
    hotkey.debounce = 220;
    hotkey.triggerOnRelease = true;
    hotkey.triggerOnHold = true;
    hotkey.blockKeyFromGame = true;
    hotkey.allowExitToFullscreenRegardlessOfGameState = true;
    g_config.hotkeys.push_back(hotkey);

    SensitivityHotkeyConfig sensitivityHotkey;
    sensitivityHotkey.keys = { VK_F9 };
    sensitivityHotkey.sensitivity = 0.55f;
    sensitivityHotkey.separateXY = true;
    sensitivityHotkey.sensitivityX = 0.75f;
    sensitivityHotkey.sensitivityY = 0.95f;
    sensitivityHotkey.toggle = true;
    sensitivityHotkey.conditions.gameState = { "menu" };
    sensitivityHotkey.conditions.exclusions = { VK_MENU };
    sensitivityHotkey.debounce = 350;
    g_config.sensitivityHotkeys.push_back(sensitivityHotkey);

    ResizeHotkeySecondaryModes(g_config.hotkeys.size());
    ResetAllHotkeySecondaryModes(g_config);
    {
        std::lock_guard<std::mutex> hotkeyLock(g_hotkeyMainKeysMutex);
        RebuildHotkeyMainKeys_Internal();
    }

    g_configIsDirty.store(true, std::memory_order_release);
}

void VerifyRichGlobalSettings() {
    Expect(g_config.lang == "zh_CN", "Expected language to roundtrip.");
    Expect(g_config.fontPath == "C:\\Windows\\Fonts\\consola.ttf", "Expected font path to roundtrip.");
    Expect(g_config.defaultMode == kPrimaryModeId, "Expected default mode to roundtrip.");
    Expect(g_config.fpsLimit == 144, "Expected fps limit to roundtrip.");
    Expect(g_config.fpsLimitSleepThreshold == 7, "Expected fps sleep threshold to roundtrip.");
    Expect(g_config.mirrorGammaMode == MirrorGammaMode::AssumeLinear, "Expected mirror gamma mode to roundtrip.");
    Expect(g_config.disableHookChaining, "Expected disableHookChaining to roundtrip.");
    Expect(g_config.allowCursorEscape, "Expected allowCursorEscape to roundtrip.");
    ExpectFloatNear(g_config.mouseSensitivity, 1.75f, "Expected mouse sensitivity to roundtrip.");
    Expect(g_config.windowsMouseSpeed == 13, "Expected Windows mouse speed to roundtrip.");
    Expect(g_config.hideAnimationsInGame, "Expected hideAnimationsInGame to roundtrip.");
    Expect(!g_config.limitCaptureFramerate, "Expected limitCaptureFramerate to roundtrip.");
    Expect(g_config.obsFramerate == 73, "Expected obsFramerate to roundtrip.");
    Expect(g_config.useSystemKeyRepeat, "Expected useSystemKeyRepeat to roundtrip.");
    Expect(g_config.keyRepeatStartDelay == 275, "Expected keyRepeatStartDelay to roundtrip.");
    Expect(g_config.keyRepeatDelay == 4, "Expected keyRepeatDelay to roundtrip.");
    Expect(!g_config.basicModeEnabled, "Expected basicModeEnabled to roundtrip.");
    Expect(!g_config.restoreWindowedModeOnFullscreenExit, "Expected restoreWindowedModeOnFullscreenExit to roundtrip.");
    Expect(g_config.disableFullscreenPrompt, "Expected disableFullscreenPrompt to roundtrip.");
    Expect(g_config.startupIndicatorMode == 2, "Expected startupIndicatorMode to roundtrip.");
    Expect(g_config.startupIndicatorImagePath == "C:\\images\\startup.gif", "Expected startupIndicatorImagePath to roundtrip.");
    ExpectVectorEquals(g_config.guiHotkey, std::vector<DWORD>{ VK_CONTROL, VK_SHIFT, 'G' }, "Expected GUI hotkey to roundtrip.");
    ExpectVectorEquals(g_config.borderlessHotkey, std::vector<DWORD>{ VK_MENU, VK_RETURN }, "Expected borderless hotkey to roundtrip.");
    Expect(g_config.autoBorderless, "Expected autoBorderless to roundtrip.");
    ExpectVectorEquals(g_config.imageOverlaysHotkey, std::vector<DWORD>{ VK_F8 }, "Expected image overlay hotkey to roundtrip.");
    ExpectVectorEquals(g_config.windowOverlaysHotkey, std::vector<DWORD>{ VK_F7 }, "Expected window overlay hotkey to roundtrip.");
    ExpectVectorEquals(g_config.ninjabrainOverlayHotkey, std::vector<DWORD>{ VK_F10 }, "Expected Ninjabrain overlay hotkey to roundtrip.");

    std::string currentModeId;
    {
        std::lock_guard<std::mutex> lock(g_modeIdMutex);
        currentModeId = g_currentModeId;
    }
    Expect(EqualsIgnoreCase(currentModeId, kPrimaryModeId), "Expected LoadConfig to apply the saved default mode as current mode.");
}

void VerifyRichDebugSettings() {
    Expect(g_config.debug.showPerformanceOverlay, "Expected debug.showPerformanceOverlay to roundtrip.");
    Expect(g_config.debug.showProfiler, "Expected debug.showProfiler to roundtrip.");
    ExpectFloatNear(g_config.debug.profilerScale, 1.25f, "Expected debug.profilerScale to roundtrip.");
    Expect(g_config.debug.showHotkeyDebug, "Expected debug.showHotkeyDebug to roundtrip.");
    Expect(g_config.debug.fakeCursor, "Expected debug.fakeCursor to roundtrip.");
    Expect(g_config.debug.showTextureGrid, "Expected debug.showTextureGrid to roundtrip.");
    Expect(g_config.debug.delayRenderingUntilFinished, "Expected debug.delayRenderingUntilFinished to roundtrip.");
    Expect(g_config.debug.virtualCameraEnabled, "Expected debug.virtualCameraEnabled to roundtrip.");
    Expect(g_config.debug.videoCacheBudgetMiB == 384, "Expected debug.videoCacheBudgetMiB to roundtrip.");
    Expect(g_config.debug.logModeSwitch, "Expected debug.logModeSwitch to roundtrip.");
    Expect(g_config.debug.logAnimation, "Expected debug.logAnimation to roundtrip.");
    Expect(g_config.debug.logHotkey, "Expected debug.logHotkey to roundtrip.");
    Expect(g_config.debug.logObs, "Expected debug.logObs to roundtrip.");
    Expect(g_config.debug.logWindowOverlay, "Expected debug.logWindowOverlay to roundtrip.");
    Expect(g_config.debug.logFileMonitor, "Expected debug.logFileMonitor to roundtrip.");
    Expect(g_config.debug.logImageMonitor, "Expected debug.logImageMonitor to roundtrip.");
    Expect(g_config.debug.logPerformance, "Expected debug.logPerformance to roundtrip.");
    Expect(g_config.debug.logTextureOps, "Expected debug.logTextureOps to roundtrip.");
    Expect(g_config.debug.logGui, "Expected debug.logGui to roundtrip.");
    Expect(g_config.debug.logInit, "Expected debug.logInit to roundtrip.");
    Expect(g_config.debug.logCursorTextures, "Expected debug.logCursorTextures to roundtrip.");
}

void VerifyRichModes() {
        const ModeConfig& primaryMode = FindModeOrThrow(kPrimaryModeId);
        Expect(primaryMode.width == 1280, "Expected primary mode width to roundtrip.");
        Expect(primaryMode.height == 720, "Expected primary mode height to roundtrip.");
        Expect(primaryMode.manualWidth == 1280, "Expected primary mode manualWidth to roundtrip.");
        Expect(primaryMode.manualHeight == 720, "Expected primary mode manualHeight to roundtrip.");
        Expect(primaryMode.background.selectedMode == "gradient", "Expected primary mode background mode to roundtrip.");
        Expect(primaryMode.background.imageFit == "fit", "Expected primary mode background image fit to roundtrip.");
        Expect(primaryMode.background.gradientStops.size() == 2, "Expected primary mode gradient stops to roundtrip.");
        ExpectColorNear(primaryMode.background.gradientStops[0].color, { 0.15f, 0.2f, 0.35f, 1.0f },
                  "Expected primary mode first gradient color to roundtrip.");
        ExpectFloatNear(primaryMode.background.gradientAngle, 55.0f, "Expected primary mode gradient angle to roundtrip.");
        Expect(primaryMode.background.gradientAnimation == GradientAnimationType::Slide,
            "Expected primary mode gradient animation to roundtrip.");
        ExpectFloatNear(primaryMode.background.gradientAnimationSpeed, 1.3f,
                  "Expected primary mode gradient animation speed to roundtrip.");
        Expect(primaryMode.background.gradientColorFade, "Expected primary mode gradient color fade to roundtrip.");
        ExpectVectorEquals(SourceIdsOfType(primaryMode, ModeSourceType::Mirror), std::vector<std::string>{ kVerifierMirrorName },
                  "Expected primary mode mirror sources to roundtrip.");
        ExpectVectorEquals(SourceIdsOfType(primaryMode, ModeSourceType::MirrorGroup), std::vector<std::string>{ kVerifierGroupName },
                  "Expected primary mode mirror-group sources to roundtrip.");
        ExpectVectorEquals(SourceIdsOfType(primaryMode, ModeSourceType::Image), std::vector<std::string>{ kVerifierImageName },
                  "Expected primary mode image sources to roundtrip.");
        ExpectVectorEquals(SourceIdsOfType(primaryMode, ModeSourceType::WindowOverlay), std::vector<std::string>{ kVerifierWindowOverlayName },
                  "Expected primary mode window-overlay sources to roundtrip.");
        ExpectVectorEquals(SourceIdsOfType(primaryMode, ModeSourceType::BrowserOverlay), std::vector<std::string>{ kVerifierBrowserOverlayName },
                  "Expected primary mode browser-overlay sources to roundtrip.");
        Expect(primaryMode.stretch.enabled, "Expected primary mode stretch.enabled to roundtrip.");
        Expect(primaryMode.stretch.width == 1400 && primaryMode.stretch.height == 800,
            "Expected primary mode stretch size to roundtrip.");
        Expect(primaryMode.stretch.x == 15 && primaryMode.stretch.y == 25,
            "Expected primary mode stretch position to roundtrip.");
        Expect(primaryMode.gameTransition == GameTransitionType::Bounce, "Expected primary mode game transition to roundtrip.");
        Expect(primaryMode.transitionDurationMs == 777, "Expected primary mode transitionDurationMs to roundtrip.");
        ExpectFloatNear(primaryMode.easeInPower, 2.5f, "Expected primary mode easeInPower to roundtrip.");
        ExpectFloatNear(primaryMode.easeOutPower, 4.25f, "Expected primary mode easeOutPower to roundtrip.");
        Expect(primaryMode.bounceCount == 3, "Expected primary mode bounceCount to roundtrip.");
        ExpectFloatNear(primaryMode.bounceIntensity, 0.42f, "Expected primary mode bounceIntensity to roundtrip.");
        Expect(primaryMode.bounceDurationMs == 333, "Expected primary mode bounceDurationMs to roundtrip.");
        Expect(primaryMode.relativeStretching, "Expected primary mode relativeStretching to roundtrip.");
        Expect(primaryMode.skipAnimateX, "Expected primary mode skipAnimateX to roundtrip.");
        Expect(!primaryMode.skipAnimateY, "Expected primary mode skipAnimateY to roundtrip.");
        Expect(primaryMode.border.enabled, "Expected primary mode border.enabled to roundtrip.");
        ExpectColorNear(primaryMode.border.color, { 0.3f, 0.4f, 0.5f, 1.0f }, "Expected primary mode border color to roundtrip.");
        Expect(primaryMode.border.width == 9 && primaryMode.border.radius == 14,
            "Expected primary mode border settings to roundtrip.");
        Expect(primaryMode.sensitivityOverrideEnabled, "Expected primary mode sensitivityOverrideEnabled to roundtrip.");
        ExpectFloatNear(primaryMode.modeSensitivity, 0.88f, "Expected primary mode modeSensitivity to roundtrip.");
        Expect(primaryMode.separateXYSensitivity, "Expected primary mode separateXYSensitivity to roundtrip.");
        ExpectFloatNear(primaryMode.modeSensitivityX, 0.91f, "Expected primary mode modeSensitivityX to roundtrip.");
        ExpectFloatNear(primaryMode.modeSensitivityY, 0.72f, "Expected primary mode modeSensitivityY to roundtrip.");
        Expect(primaryMode.slideMirrorsIn, "Expected primary mode slideMirrorsIn to roundtrip.");

    const ModeConfig& precisionMode = FindModeOrThrow(kPrecisionModeId);
    Expect(precisionMode.width == 960 && precisionMode.height == 540, "Expected precision mode dimensions to roundtrip.");
    ExpectVectorEquals(SourceIdsOfType(precisionMode, ModeSourceType::Mirror), std::vector<std::string>{ kAuxMirrorName },
                       "Expected precision mode to keep its mirror source.");
}

void VerifyRichMirrors() {
    const MirrorConfig& verifierMirror = FindMirrorOrThrow(kVerifierMirrorName);
    Expect(verifierMirror.captureWidth == 73 && verifierMirror.captureHeight == 41,
           "Expected verifier mirror capture dimensions to roundtrip.");
    Expect(verifierMirror.input.size() == 2, "Expected verifier mirror input zones to roundtrip.");
    Expect(verifierMirror.input[0].x == 10 && verifierMirror.input[0].y == 20 && verifierMirror.input[0].relativeTo == "centerViewport",
           "Expected verifier mirror first input zone to roundtrip.");
    Expect(verifierMirror.output.useRelativePosition, "Expected verifier mirror relative output positioning to roundtrip.");
    ExpectFloatNear(verifierMirror.output.relativeX, 0.25f, "Expected verifier mirror output.relativeX to roundtrip.");
    ExpectFloatNear(verifierMirror.output.relativeY, 0.75f, "Expected verifier mirror output.relativeY to roundtrip.");
    ExpectFloatNear(verifierMirror.output.scale, 1.25f, "Expected verifier mirror output.scale to roundtrip.");
    Expect(verifierMirror.output.separateScale, "Expected verifier mirror output.separateScale to roundtrip.");
    ExpectFloatNear(verifierMirror.output.scaleX, 0.9f, "Expected verifier mirror output.scaleX to roundtrip.");
    ExpectFloatNear(verifierMirror.output.scaleY, 1.1f, "Expected verifier mirror output.scaleY to roundtrip.");
    Expect(verifierMirror.colors.targetColors.size() == 2, "Expected verifier mirror target colors to roundtrip.");
    ExpectColorNear(verifierMirror.colors.output, { 0.1f, 0.2f, 0.3f, 0.4f }, "Expected verifier mirror output color to roundtrip.");
    ExpectColorNear(verifierMirror.colors.border, { 0.7f, 0.8f, 0.2f, 1.0f }, "Expected verifier mirror border color to roundtrip.");
    ExpectFloatNear(verifierMirror.colorSensitivity, 0.123f, "Expected verifier mirror color sensitivity to roundtrip.");
    Expect(verifierMirror.border.type == MirrorBorderType::Static, "Expected verifier mirror border type to roundtrip.");
    Expect(verifierMirror.border.staticShape == MirrorBorderShape::Circle, "Expected verifier mirror border shape to roundtrip.");
    ExpectColorNear(verifierMirror.border.staticColor, { 0.4f, 0.5f, 0.6f, 1.0f },
                    "Expected verifier mirror static border color to roundtrip.");
    Expect(verifierMirror.border.staticThickness == 5 && verifierMirror.border.staticRadius == 18,
           "Expected verifier mirror static border size to roundtrip.");
    Expect(verifierMirror.border.staticOffsetX == 7 && verifierMirror.border.staticOffsetY == -4,
           "Expected verifier mirror static border offsets to roundtrip.");
    Expect(verifierMirror.border.staticWidth == 91 && verifierMirror.border.staticHeight == 87,
           "Expected verifier mirror static border dimensions to roundtrip.");
    Expect(verifierMirror.fps == 48, "Expected verifier mirror fps to roundtrip.");
    ExpectFloatNear(verifierMirror.opacity, 0.654f, "Expected verifier mirror opacity to roundtrip.");
    Expect(verifierMirror.rawOutput, "Expected verifier mirror rawOutput to roundtrip.");
    Expect(verifierMirror.colorPassthrough, "Expected verifier mirror colorPassthrough to roundtrip.");
    Expect(verifierMirror.gradientOutput, "Expected verifier mirror gradientOutput to roundtrip.");
    Expect(verifierMirror.gradient.gradientStops.size() == 2, "Expected verifier mirror gradient stops to roundtrip.");
    ExpectFloatNear(verifierMirror.gradient.gradientAngle, 42.0f, "Expected verifier mirror gradient angle to roundtrip.");
    Expect(verifierMirror.gradient.gradientAnimation == GradientAnimationType::Rotate,
           "Expected verifier mirror gradient animation to roundtrip.");
    ExpectFloatNear(verifierMirror.gradient.gradientAnimationSpeed, 1.7f,
                    "Expected verifier mirror gradient animation speed to roundtrip.");
    Expect(verifierMirror.gradient.gradientColorFade, "Expected verifier mirror gradient color fade to roundtrip.");
    Expect(verifierMirror.onlyOnMyScreen, "Expected verifier mirror onlyOnMyScreen to roundtrip.");

    const MirrorConfig& auxMirror = FindMirrorOrThrow(kAuxMirrorName);
    Expect(auxMirror.captureWidth == 88 && auxMirror.captureHeight == 66, "Expected aux mirror capture dimensions to roundtrip.");
}

void VerifyRichMirrorGroups() {
    const MirrorGroupConfig& group = FindMirrorGroupOrThrow(kVerifierGroupName);
    Expect(group.output.useRelativePosition, "Expected mirror group relative positioning to roundtrip.");
    ExpectFloatNear(group.output.relativeX, 0.5f, "Expected mirror group output.relativeX to roundtrip.");
    ExpectFloatNear(group.output.relativeY, 0.4f, "Expected mirror group output.relativeY to roundtrip.");
    Expect(group.output.separateScale, "Expected mirror group separateScale to roundtrip.");
    ExpectFloatNear(group.output.scaleX, 1.3f, "Expected mirror group scaleX to roundtrip.");
    ExpectFloatNear(group.output.scaleY, 0.7f, "Expected mirror group scaleY to roundtrip.");
    Expect(group.mirrors.size() == 2, "Expected mirror group items to roundtrip.");
    Expect(group.mirrors[0].mirrorId == kVerifierMirrorName && group.mirrors[0].enabled,
           "Expected mirror group first mirror item to roundtrip.");
    ExpectFloatNear(group.mirrors[0].widthPercent, 0.6f, "Expected mirror group widthPercent to roundtrip.");
    ExpectFloatNear(group.mirrors[1].heightPercent, 0.6f, "Expected mirror group second heightPercent to roundtrip.");
    Expect(group.mirrors[1].offsetX == -20 && group.mirrors[1].offsetY == 30,
           "Expected mirror group second offsets to roundtrip.");
}

void VerifyRichImages() {
    const ImageConfig& image = FindImageOrThrow(kVerifierImageName);
    Expect(image.path == "C:\\temp\\checklist.png", "Expected image path to roundtrip.");
    Expect(image.x == 11 && image.y == 22, "Expected image position to roundtrip.");
    ExpectFloatNear(image.scale, 1.6f, "Expected image scale to roundtrip.");
    Expect(!image.relativeSizing, "Expected image relativeSizing to roundtrip.");
    Expect(image.width == 320 && image.height == 180, "Expected image dimensions to roundtrip.");
    Expect(image.relativeTo == "centerViewport", "Expected image relativeTo to roundtrip.");
    Expect(image.crop_top == 3 && image.crop_bottom == 4 && image.crop_left == 5 && image.crop_right == 6,
           "Expected image crop values to roundtrip.");
    Expect(image.enableColorKey, "Expected image enableColorKey to roundtrip.");
    Expect(image.colorKeys.size() == 2, "Expected image color keys to roundtrip.");
    ExpectFloatNear(image.colorKeys[0].sensitivity, 0.02f, "Expected image color key sensitivity to roundtrip.");
    ExpectFloatNear(image.opacity, 0.81f, "Expected image opacity to roundtrip.");
    Expect(image.background.enabled, "Expected image background.enabled to roundtrip.");
    ExpectColorNear(image.background.color, { 0.2f, 0.3f, 0.4f, 1.0f }, "Expected image background color to roundtrip.");
    ExpectFloatNear(image.background.opacity, 0.33f, "Expected image background opacity to roundtrip.");
    Expect(image.pixelatedScaling, "Expected image pixelatedScaling to roundtrip.");
    Expect(image.onlyOnMyScreen, "Expected image onlyOnMyScreen to roundtrip.");
    Expect(image.border.enabled, "Expected image border.enabled to roundtrip.");
    Expect(image.border.width == 7 && image.border.radius == 12, "Expected image border geometry to roundtrip.");
}

void VerifyRichWindowOverlays() {
    const WindowOverlayConfig& overlay = FindWindowOverlayOrThrow(kVerifierWindowOverlayName);
    Expect(overlay.windowTitle == "Speedrun Timer", "Expected window overlay title to roundtrip.");
    Expect(overlay.windowClass == "TimerClass", "Expected window overlay class to roundtrip.");
    Expect(overlay.executableName == "Timer.exe", "Expected window overlay executable to roundtrip.");
    Expect(overlay.windowMatchPriority == "title_executable", "Expected window overlay match priority to roundtrip.");
    Expect(overlay.x == -30 && overlay.y == 45, "Expected window overlay position to roundtrip.");
    ExpectFloatNear(overlay.scale, 1.2f, "Expected window overlay scale to roundtrip.");
    Expect(overlay.relativeTo == "centerViewport", "Expected window overlay relativeTo to roundtrip.");
    Expect(overlay.crop_top == 4 && overlay.crop_bottom == 5 && overlay.crop_left == 6 && overlay.crop_right == 7,
           "Expected window overlay crop values to roundtrip.");
    Expect(overlay.enableColorKey, "Expected window overlay enableColorKey to roundtrip.");
    Expect(overlay.colorKeys.size() == 1, "Expected window overlay color keys to roundtrip.");
    ExpectFloatNear(overlay.opacity, 0.71f, "Expected window overlay opacity to roundtrip.");
    Expect(overlay.background.enabled, "Expected window overlay background.enabled to roundtrip.");
    Expect(overlay.pixelatedScaling, "Expected window overlay pixelatedScaling to roundtrip.");
    Expect(overlay.onlyOnMyScreen, "Expected window overlay onlyOnMyScreen to roundtrip.");
    Expect(overlay.fps == 27, "Expected window overlay fps to roundtrip.");
    Expect(overlay.searchInterval == 2500, "Expected window overlay searchInterval to roundtrip.");
    Expect(overlay.captureMethod == "BitBlt", "Expected window overlay captureMethod to roundtrip.");
    Expect(overlay.forceUpdate, "Expected window overlay forceUpdate to roundtrip.");
    Expect(overlay.enableInteraction, "Expected window overlay enableInteraction to roundtrip.");
    Expect(overlay.border.enabled, "Expected window overlay border.enabled to roundtrip.");

    const NinjabrainOverlayConfig& ninjabrain = g_config.ninjabrainOverlay;
    Expect(ninjabrain.showTitleBar, "Expected Ninjabrain overlay showTitleBar to roundtrip.");
    Expect(ninjabrain.titleText == "Stronghold Helper", "Expected Ninjabrain overlay title text to roundtrip.");
    ExpectFloatNear(ninjabrain.bgOpacity, 0.83f, "Expected Ninjabrain overlay bgOpacity to roundtrip.");
    ExpectColorNear(ninjabrain.bgColor, { 0.11f, 0.12f, 0.13f, 1.0f }, "Expected Ninjabrain result background color to roundtrip.");
    ExpectColorNear(ninjabrain.chromeColor, { 0.18f, 0.19f, 0.21f, 1.0f }, "Expected Ninjabrain title bar color to roundtrip.");
    ExpectColorNear(ninjabrain.headerFillColor, { 0.22f, 0.23f, 0.25f, 1.0f }, "Expected Ninjabrain header background color to roundtrip.");
    ExpectColorNear(ninjabrain.throwsBackgroundColor, { 0.14f, 0.15f, 0.16f, 1.0f }, "Expected Ninjabrain throws background color to roundtrip.");
    ExpectColorNear(ninjabrain.dividerColor, { 0.30f, 0.31f, 0.33f, 1.0f }, "Expected Ninjabrain divider color to roundtrip.");
    ExpectColorNear(ninjabrain.headerDividerColor, { 0.42f, 0.43f, 0.45f, 1.0f }, "Expected Ninjabrain header divider color to roundtrip.");
    ExpectColorNear(ninjabrain.dataColor, { 0.91f, 0.92f, 0.93f, 1.0f }, "Expected Ninjabrain body text color to roundtrip.");
    ExpectColorNear(ninjabrain.titleTextColor, { 0.97f, 0.98f, 0.99f, 1.0f }, "Expected Ninjabrain title text color to roundtrip.");
    ExpectColorNear(ninjabrain.throwsTextColor, { 0.84f, 0.85f, 0.86f, 1.0f }, "Expected Ninjabrain throws text color to roundtrip.");
    ExpectColorNear(ninjabrain.divineTextColor, { 0.74f, 0.82f, 0.91f, 1.0f }, "Expected Ninjabrain divine text color to roundtrip.");
    ExpectColorNear(ninjabrain.versionTextColor, { 0.65f, 0.66f, 0.67f, 1.0f }, "Expected Ninjabrain version text color to roundtrip.");
    Expect(!ninjabrain.showDirectionToStronghold, "Expected Ninjabrain direction-to-stronghold toggle to roundtrip.");
    ExpectColorNear(ninjabrain.textColor, { 0.55f, 0.56f, 0.57f, 1.0f }, "Expected Ninjabrain header text color to roundtrip.");
    ExpectColorNear(ninjabrain.outlineColor, { 0.08f, 0.09f, 0.10f, 0.72f }, "Expected Ninjabrain text outline color to roundtrip.");
    ExpectColorNear(ninjabrain.certaintyColor, { 0.12f, 0.98f, 0.34f, 1.0f }, "Expected Ninjabrain certainty high color to roundtrip.");
    ExpectColorNear(ninjabrain.certaintyMidColor, { 0.94f, 0.88f, 0.20f, 1.0f }, "Expected Ninjabrain certainty mid color to roundtrip.");
    ExpectColorNear(ninjabrain.certaintyLowColor, { 0.82f, 0.18f, 0.22f, 1.0f }, "Expected Ninjabrain certainty low color to roundtrip.");
    ExpectColorNear(ninjabrain.subpixelPositiveColor, { 0.32f, 0.87f, 0.41f, 1.0f }, "Expected Ninjabrain subpixel positive color to roundtrip.");
    ExpectColorNear(ninjabrain.subpixelNegativeColor, { 0.81f, 0.34f, 0.37f, 1.0f }, "Expected Ninjabrain subpixel negative color to roundtrip.");
    ExpectColorNear(ninjabrain.coordPositiveColor, { 0.61f, 0.72f, 0.83f, 1.0f }, "Expected Ninjabrain positive coordinate color to roundtrip.");
    ExpectColorNear(ninjabrain.coordNegativeColor, { 0.73f, 0.24f, 0.27f, 1.0f }, "Expected Ninjabrain negative coordinate color to roundtrip.");
    ExpectFloatNear(ninjabrain.resultsMarginLeft, 9.0f, "Expected Ninjabrain results left margin to roundtrip.");
    ExpectFloatNear(ninjabrain.resultsMarginRight, 5.0f, "Expected Ninjabrain results right margin to roundtrip.");
    ExpectFloatNear(ninjabrain.informationMessagesMarginLeft, 17.0f, "Expected Ninjabrain information-message left margin to roundtrip.");
    ExpectFloatNear(ninjabrain.informationMessagesMarginRight, 11.0f, "Expected Ninjabrain information-message right margin to roundtrip.");
    ExpectFloatNear(ninjabrain.throwsMarginLeft, 7.0f, "Expected Ninjabrain throws left margin to roundtrip.");
    ExpectFloatNear(ninjabrain.throwsMarginRight, 15.0f, "Expected Ninjabrain throws right margin to roundtrip.");
    ExpectFloatNear(ninjabrain.failureMarginLeft, 21.0f, "Expected Ninjabrain failed-result left margin to roundtrip.");
    ExpectFloatNear(ninjabrain.failureMarginRight, 13.0f, "Expected Ninjabrain failed-result right margin to roundtrip.");
    ExpectFloatNear(ninjabrain.sidePadding, 18.0f, "Expected Ninjabrain sidePadding to roundtrip.");
    Expect(ninjabrain.eyeThrowRows == 6, "Expected Ninjabrain eyeThrowRows to roundtrip.");
    Expect(ninjabrain.showBoatStateInTopBar, "Expected Ninjabrain showBoatStateInTopBar to roundtrip.");
    ExpectFloatNear(ninjabrain.boatStateSize, 26.0f, "Expected Ninjabrain boatStateSize to roundtrip.");
    ExpectFloatNear(ninjabrain.boatStateMarginRight, 14.0f, "Expected Ninjabrain boatStateMarginRight to roundtrip.");
    Expect(ninjabrain.hideIfStale, "Expected Ninjabrain hideIfStale to roundtrip.");
    Expect(ninjabrain.hideIfStaleDelaySeconds == 37,
           "Expected Ninjabrain hideIfStaleDelaySeconds to roundtrip.");
}

void VerifyRichBrowserOverlays() {
    const BrowserOverlayConfig& overlay = FindBrowserOverlayOrThrow(kVerifierBrowserOverlayName);
    Expect(overlay.url == "https://example.com/dashboard", "Expected browser overlay URL to roundtrip.");
    Expect(overlay.customCss == "body { background: transparent; }", "Expected browser overlay CSS to roundtrip.");
    Expect(overlay.browserWidth == 1024 && overlay.browserHeight == 576, "Expected browser overlay dimensions to roundtrip.");
    Expect(overlay.x == 9 && overlay.y == 19, "Expected browser overlay position to roundtrip.");
    ExpectFloatNear(overlay.scale, 1.15f, "Expected browser overlay scale to roundtrip.");
    Expect(overlay.relativeTo == "centerViewport", "Expected browser overlay relativeTo to roundtrip.");
    Expect(overlay.enableColorKey, "Expected browser overlay enableColorKey to roundtrip.");
    Expect(overlay.colorKeys.size() == 1, "Expected browser overlay color keys to roundtrip.");
    ExpectFloatNear(overlay.opacity, 0.67f, "Expected browser overlay opacity to roundtrip.");
    Expect(overlay.background.enabled, "Expected browser overlay background.enabled to roundtrip.");
    Expect(overlay.pixelatedScaling, "Expected browser overlay pixelatedScaling to roundtrip.");
    Expect(overlay.onlyOnMyScreen, "Expected browser overlay onlyOnMyScreen to roundtrip.");
    Expect(overlay.fps == 25, "Expected browser overlay fps to roundtrip.");
    Expect(overlay.transparentBackground, "Expected browser overlay transparentBackground to roundtrip.");
    Expect(!overlay.muteAudio, "Expected browser overlay muteAudio to roundtrip.");
    Expect(!overlay.allowSystemMediaKeys, "Expected browser overlay allowSystemMediaKeys to roundtrip.");
    Expect(overlay.reloadOnUpdate, "Expected browser overlay reloadOnUpdate to roundtrip.");
    Expect(overlay.reloadInterval == 2500, "Expected browser overlay reloadInterval to roundtrip.");
    Expect(overlay.border.enabled, "Expected browser overlay border.enabled to roundtrip.");
}

void VerifyRichHotkeys() {
    const HotkeyConfig& hotkey = FindHotkeyBySecondaryModeOrThrow(kPrimaryModeId);
    ExpectVectorEquals(hotkey.keys, std::vector<DWORD>{ VK_F6, 'Q' }, "Expected hotkey keys to roundtrip.");
    Expect(hotkey.mainMode == "Fullscreen", "Expected hotkey main mode to roundtrip.");
    Expect(hotkey.secondaryMode == kPrimaryModeId, "Expected hotkey secondary mode to roundtrip.");
    Expect(hotkey.altSecondaryModes.size() == 1, "Expected alt secondary modes to roundtrip.");
    ExpectVectorEquals(hotkey.altSecondaryModes[0].keys, std::vector<DWORD>{ 'E', 'R' },
                       "Expected alt secondary mode keys to roundtrip.");
    Expect(hotkey.altSecondaryModes[0].mode == kPrecisionModeId, "Expected alt secondary mode target to roundtrip.");
    ExpectVectorEquals(hotkey.conditions.gameState, std::vector<std::string>{ "ingame", "wall" },
                       "Expected hotkey gameState conditions to roundtrip.");
    ExpectVectorEquals(hotkey.conditions.exclusions, std::vector<DWORD>{ VK_LSHIFT, VK_RBUTTON },
                       "Expected hotkey exclusions to roundtrip.");
    Expect(hotkey.debounce == 220, "Expected hotkey debounce to roundtrip.");
    Expect(hotkey.triggerOnRelease, "Expected hotkey triggerOnRelease to roundtrip.");
    Expect(hotkey.triggerOnHold, "Expected hotkey triggerOnHold to roundtrip.");
    Expect(hotkey.blockKeyFromGame, "Expected hotkey blockKeyFromGame to roundtrip.");
    Expect(hotkey.allowExitToFullscreenRegardlessOfGameState,
           "Expected hotkey allowExitToFullscreenRegardlessOfGameState to roundtrip.");
}

void VerifyRichSensitivityHotkeys() {
    const SensitivityHotkeyConfig& hotkey = FindSensitivityHotkeyOrThrow({ VK_F9 });
    ExpectFloatNear(hotkey.sensitivity, 0.55f, "Expected sensitivity hotkey sensitivity to roundtrip.");
    Expect(hotkey.separateXY, "Expected sensitivity hotkey separateXY to roundtrip.");
    ExpectFloatNear(hotkey.sensitivityX, 0.75f, "Expected sensitivity hotkey sensitivityX to roundtrip.");
    ExpectFloatNear(hotkey.sensitivityY, 0.95f, "Expected sensitivity hotkey sensitivityY to roundtrip.");
    Expect(hotkey.toggle, "Expected sensitivity hotkey toggle to roundtrip.");
    ExpectVectorEquals(hotkey.conditions.gameState, std::vector<std::string>{ "menu" },
                       "Expected sensitivity hotkey gameState conditions to roundtrip.");
    ExpectVectorEquals(hotkey.conditions.exclusions, std::vector<DWORD>{ VK_MENU },
                       "Expected sensitivity hotkey exclusions to roundtrip.");
    Expect(hotkey.debounce == 350, "Expected sensitivity hotkey debounce to roundtrip.");
}

void VerifyRichCursorsAndEyeZoom() {
    Expect(g_config.cursors.enabled, "Expected cursors.enabled to roundtrip.");
    Expect(g_config.cursors.title.cursorName == "title.cur" && g_config.cursors.title.cursorSize == 64,
           "Expected title cursor config to roundtrip.");
    Expect(g_config.cursors.wall.cursorName == "wall.cur" && g_config.cursors.wall.cursorSize == 72,
           "Expected wall cursor config to roundtrip.");
    Expect(g_config.cursors.ingame.cursorName == "ingame.cur" && g_config.cursors.ingame.cursorSize == 88,
           "Expected ingame cursor config to roundtrip.");

    Expect(g_config.eyezoom.cloneWidth == 28, "Expected eyezoom cloneWidth to roundtrip.");
    Expect(g_config.eyezoom.overlayWidth == 9, "Expected eyezoom overlayWidth to roundtrip.");
    Expect(g_config.eyezoom.cloneHeight == 1500, "Expected eyezoom cloneHeight to roundtrip.");
    Expect(g_config.eyezoom.stretchWidth == 600, "Expected eyezoom stretchWidth to roundtrip.");
    Expect(g_config.eyezoom.windowWidth == 420, "Expected eyezoom windowWidth to roundtrip.");
    Expect(g_config.eyezoom.windowHeight == 9000, "Expected eyezoom windowHeight to roundtrip.");
    Expect(g_config.eyezoom.zoomAreaWidth == 222 && g_config.eyezoom.zoomAreaHeight == 777,
           "Expected eyezoom zoom area to roundtrip.");
    Expect(g_config.eyezoom.useCustomSizePosition, "Expected eyezoom useCustomSizePosition to roundtrip.");
    Expect(g_config.eyezoom.positionX == 33 && g_config.eyezoom.positionY == 44,
           "Expected eyezoom position to roundtrip.");
        Expect(g_config.eyezoom.fontSizeMode == EyeZoomFontSizeMode::Manual,
            "Expected eyezoom fontSizeMode to roundtrip.");
    Expect(g_config.eyezoom.textFontSize == 31, "Expected eyezoom textFontSize to roundtrip.");
    Expect(g_config.eyezoom.textFontPath == "C:\\Windows\\Fonts\\verdana.ttf", "Expected eyezoom font path to roundtrip.");
    Expect(g_config.eyezoom.rectHeight == 35, "Expected eyezoom rectHeight to roundtrip.");
    Expect(!g_config.eyezoom.linkRectToFont, "Expected eyezoom linkRectToFont to roundtrip.");
    ExpectColorNear(g_config.eyezoom.gridColor1, { 0.2f, 0.4f, 0.6f, 1.0f }, "Expected eyezoom gridColor1 to roundtrip.");
    ExpectFloatNear(g_config.eyezoom.gridColor1Opacity, 0.8f, "Expected eyezoom gridColor1Opacity to roundtrip.");
    ExpectColorNear(g_config.eyezoom.gridColor2, { 0.6f, 0.3f, 0.2f, 1.0f }, "Expected eyezoom gridColor2 to roundtrip.");
    ExpectFloatNear(g_config.eyezoom.gridColor2Opacity, 0.7f, "Expected eyezoom gridColor2Opacity to roundtrip.");
    ExpectColorNear(g_config.eyezoom.centerLineColor, { 0.9f, 0.8f, 0.1f, 1.0f }, "Expected eyezoom centerLineColor to roundtrip.");
    ExpectFloatNear(g_config.eyezoom.centerLineColorOpacity, 0.65f, "Expected eyezoom centerLineColorOpacity to roundtrip.");
    ExpectColorNear(g_config.eyezoom.textColor, { 0.1f, 0.2f, 0.3f, 1.0f }, "Expected eyezoom textColor to roundtrip.");
    ExpectFloatNear(g_config.eyezoom.textColorOpacity, 0.95f, "Expected eyezoom textColorOpacity to roundtrip.");
    Expect(g_config.eyezoom.slideZoomIn, "Expected eyezoom slideZoomIn to roundtrip.");
    Expect(g_config.eyezoom.slideMirrorsIn, "Expected eyezoom slideMirrorsIn to roundtrip.");
    Expect(g_config.eyezoom.overlays.size() == 2, "Expected eyezoom overlays to roundtrip.");
    Expect(g_config.eyezoom.overlays[1].name == "Overlay Two", "Expected eyezoom second overlay name to roundtrip.");
    Expect(g_config.eyezoom.overlays[1].displayMode == EyeZoomOverlayDisplayMode::Manual,
           "Expected eyezoom second overlay displayMode to roundtrip.");
    Expect(g_config.eyezoom.activeOverlayIndex == 1, "Expected eyezoom activeOverlayIndex to roundtrip.");
}

void VerifyRichKeyRebindsAndAppearance() {
    Expect(g_config.keyRebinds.enabled, "Expected keyRebinds.enabled to roundtrip.");
    Expect(!g_config.keyRebinds.resolveRebindTargetsForHotkeys,
           "Expected keyRebinds.resolveRebindTargetsForHotkeys to roundtrip.");
    ExpectVectorEquals(g_config.keyRebinds.toggleHotkey, std::vector<DWORD>{ VK_F4 },
                       "Expected keyRebinds.toggleHotkey to roundtrip.");
    Expect(g_config.keyRebinds.rebinds.size() == 1, "Expected key rebind list to roundtrip.");
    const KeyRebind& rebind = g_config.keyRebinds.rebinds.front();
    Expect(rebind.fromKey == 'J' && rebind.toKey == 'K', "Expected key rebind source/target to roundtrip.");
    Expect(rebind.enabled, "Expected key rebind enabled flag to roundtrip.");
    Expect(rebind.triggerOutputDisabled, "Expected key rebind triggerOutputDisabled to roundtrip.");
    Expect(rebind.useCustomOutput, "Expected key rebind useCustomOutput to roundtrip.");
    Expect(rebind.baseOutputDisabled, "Expected key rebind baseOutputDisabled to roundtrip.");
        Expect(rebind.cursorState == kKeyRebindCursorStateCursorGrabbed,
            "Expected key rebind cursorState to roundtrip.");
    Expect(rebind.customOutputVK == 'L', "Expected key rebind customOutputVK to roundtrip.");
    Expect(rebind.customOutputUnicode == 0x00F8, "Expected key rebind customOutputUnicode to roundtrip.");
    Expect(rebind.customOutputScanCode == 0x26, "Expected key rebind customOutputScanCode to roundtrip.");
    Expect(rebind.baseOutputShifted, "Expected key rebind baseOutputShifted to roundtrip.");
    Expect(rebind.shiftLayerEnabled, "Expected key rebind shiftLayerEnabled to roundtrip.");
    Expect(rebind.shiftLayerUsesCapsLock, "Expected key rebind shiftLayerUsesCapsLock to roundtrip.");
    Expect(rebind.shiftLayerOutputDisabled, "Expected key rebind shiftLayerOutputDisabled to roundtrip.");
    Expect(rebind.shiftLayerOutputVK == 'P', "Expected key rebind shiftLayerOutputVK to roundtrip.");
    Expect(rebind.shiftLayerOutputUnicode == 0x00D8, "Expected key rebind shiftLayerOutputUnicode to roundtrip.");
    Expect(rebind.shiftLayerOutputShifted, "Expected key rebind shiftLayerOutputShifted to roundtrip.");

    Expect(g_config.appearance.theme == "Sunrise", "Expected appearance theme to roundtrip.");
    Expect(g_config.appearance.customColors.size() == 2, "Expected appearance custom colors to roundtrip.");
    ExpectColorNear(g_config.appearance.customColors.at("WindowBg"), { 0.1f, 0.11f, 0.12f, 1.0f },
                    "Expected appearance WindowBg custom color to roundtrip.");
    ExpectColorNear(g_config.appearance.customColors.at("Header"), { 0.8f, 0.4f, 0.2f, 1.0f },
                    "Expected appearance Header custom color to roundtrip.");
}

template <typename VerifyFn>
void RunRichConfigRoundtripCase(std::string_view caseName, VerifyFn&& verify, TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    const std::filesystem::path root = PrepareCaseDirectory(caseName);
    ResetGlobalTestState(root);

    LoadConfig();
    ExpectConfigLoadSucceeded(std::string(caseName) + " initial default load");

    PopulateRichConfigFixture();
    SaveAndReloadCurrentConfig();
    ExpectConfigLoadSucceeded(std::string(caseName) + " roundtrip reload");

    verify();

    if (runMode == TestRunMode::Visual) {
        RunVisualLoopWithNinjabrainPreview(window, caseName, &RenderInteractiveSettingsFrame);
    }
}

template <typename WriteFixtureFn, typename VerifyFn>
void RunConfigLoadCase(std::string_view caseName, WriteFixtureFn&& writeFixture, VerifyFn&& verify,
                       TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    const std::filesystem::path root = PrepareCaseDirectory(caseName);
    ResetGlobalTestState(root);

    writeFixture();
    LoadConfig();
    verify();

    if (runMode == TestRunMode::Visual) {
        RunVisualLoopWithNinjabrainPreview(window, caseName, &RenderInteractiveSettingsFrame);
    }
}

void PrepareRichConfigForGui(std::string_view caseName) {
    const std::filesystem::path root = PrepareCaseDirectory(caseName);
    ResetGlobalTestState(root);
    LoadConfig();
    ExpectConfigLoadSucceeded(std::string(caseName) + " initial GUI fixture load");
    PopulateRichConfigFixture();
    SaveAndReloadCurrentConfig();
    ExpectConfigLoadSucceeded(std::string(caseName) + " GUI fixture reload");
}

void PrepareDefaultConfigForGui(std::string_view caseName, bool basicModeEnabled) {
    const std::filesystem::path root = PrepareCaseDirectory(caseName);
    ResetGlobalTestState(root);
    LoadConfig();
    ExpectConfigLoadSucceeded(std::string(caseName) + " default GUI fixture load");
    g_config.basicModeEnabled = basicModeEnabled;
    g_configIsDirty.store(false, std::memory_order_release);
}

void RunDefaultSettingsTabCase(std::string_view caseName, const std::string& topLevelTabLabel,
                               const std::string& inputsSubTabLabel, bool basicModeEnabled,
                               TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    PrepareDefaultConfigForGui(caseName, basicModeEnabled);

    if (runMode == TestRunMode::Visual) {
        RunVisualLoopWithNinjabrainPreview(window, caseName, [&](DummyWindow& visualWindow) {
            RenderSettingsFrame(visualWindow, topLevelTabLabel.c_str(),
                                inputsSubTabLabel.empty() ? nullptr : inputsSubTabLabel.c_str());
        });
        return;
    }

    RenderSettingsFrame(window, topLevelTabLabel.c_str(), inputsSubTabLabel.empty() ? nullptr : inputsSubTabLabel.c_str());
}

void RunPopulatedSettingsTabCase(std::string_view caseName, const std::string& topLevelTabLabel,
                                 const std::string& inputsSubTabLabel = std::string(),
                                 TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    PrepareRichConfigForGui(caseName);

    if (runMode == TestRunMode::Visual) {
        RunVisualLoopWithNinjabrainPreview(window, caseName, [&](DummyWindow& visualWindow) {
            RenderSettingsFrame(visualWindow, topLevelTabLabel.c_str(),
                                inputsSubTabLabel.empty() ? nullptr : inputsSubTabLabel.c_str());
        });
        return;
    }

    RenderSettingsFrame(window, topLevelTabLabel.c_str(), inputsSubTabLabel.empty() ? nullptr : inputsSubTabLabel.c_str());
}

template <typename RenderFrameFn>
void RunVisualLoop(DummyWindow& window, std::string_view testCaseName, RenderFrameFn&& renderFrame) {
    window.SetTitle("Toolscreen GUI Integration Tests [visual] - " + std::string(testCaseName));

    std::cout << "VISUAL " << testCaseName << std::endl;
    std::cout << "Close the dummy test window to exit visual mode." << std::endl;

    while (window.PumpMessages()) {
        renderFrame(window);
        Sleep(16);
    }

    std::cout << "EXIT " << testCaseName << std::endl;
}

// The suite is split across tests/gui_integration/*.inl so new coverage does not accumulate in one monolithic file.
#include "gui_integration/config_tests.inl"
#include "gui_integration/rebind_tests.inl"
#include "gui_integration/render_tests.inl"
#include "gui_integration/ui_and_log_tests.inl"
#include "gui_integration/runner.inl"

