#include "game_test_mode.h"
#include "thread_stack_dump.h"

#include "common/i18n.h"
#include "common/utils.h"
#include "config/config_toml.h"
#include "gui/gui.h"
#include "features/window_overlay.h"
#include "hooks/hook_chain.h"
#include "hooks/input_hook.h"
#include "render/mirror_thread.h"
#include "render/obs_thread.h"
#include "render/render.h"
#include "render/render_backend.h"
#include "render/vulkan/vulkan_renderer.h"
#include "version.h"

#include "imgui.h"
#include "imgui_internal.h"

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

uint64_t GetObsPassImGuiFramesReusedForTests();
bool ReadPublishedObsComposePixelForTests(int x, int y, unsigned char outRgba[4]);
void GetPublishedObsComposeSizeForTests(int& width, int& height);
extern std::atomic<bool> g_configLoaded;
extern std::atomic<void*> g_lastSdlCursorWindow;
extern std::atomic<bool> g_nativeCursorGrabbed;

namespace ToolscreenVulkanLayerTest {
bool GetTrackedDevice(VkDevice& device, PFN_vkGetDeviceProcAddr& layerGdpa, PFN_vkGetDeviceProcAddr& nextGdpa);
}

namespace GameTest {
namespace {

constexpr const char* kTestModeId = "GameTestSmall";
constexpr int kTestModeWidth = 640;
constexpr int kTestModeHeight = 360;
constexpr DWORD kTestHotkeyVk = VK_F9;
constexpr DWORD kTestExactModifierHotkeyVk = VK_F10;
// Injected with SendInput, so it must be a key that no other program on the machine consumes system-wide.
constexpr DWORD kTestRealInputHotkeyVk = VK_F24;

std::atomic<bool> s_enabled{ false };
std::wstring s_resultsPath;
std::string s_expectedVersion;
std::string s_expectedBackend;
std::vector<std::string> s_filters;

std::atomic<uint64_t> s_renderFrames{ 0 };
std::atomic<DWORD> s_renderThreadId{ 0 };
std::mutex s_renderTasksMutex;
std::deque<std::function<void()>> s_renderTasks;

std::atomic<bool> s_forceSharedObsFrame{ false };
// Sleeps the render thread this long in every hooked frame, to make the game lag like a slow machine.
std::atomic<int> s_renderFrameDelayMs{ 0 };
// OpenGL only: 4096x4096 blits added to every frame, and video memory held in textures, to make the GPU act like a
// weak one. The memory is held in whole textures of kGlVramHogTextureMb and released when s_glVramHogMb drops to 0.
std::atomic<int> s_glGpuLoadBlits{ 0 };
std::atomic<int> s_glVramHogMb{ 0 };
constexpr int kGlVramHogTextureMb = 256;
// TOOLSCREEN_GAME_TEST_EXIT: after the tests, close the game normally so DLL_PROCESS_DETACH runs.
// "plain" just closes it; "log_lock" first leaves g_logFileMutex held by a thread that process exit will kill.
std::string s_exitMode;
// TOOLSCREEN_GAME_TEST_WAIT_FOR_TITLE: hold the tests until the game window title contains this, e.g. once a
// quick-play world has finished loading.
std::string s_waitForTitle;
// TOOLSCREEN_GAME_TEST_WINDOW: "borderless" (Toolscreen's borderless toggle) or "fullscreen" (the game's F11) to
// run the tests at monitor size instead of the default small window.
std::string s_windowMode;
std::thread s_runnerThread;
std::atomic<bool> s_stopRequested{ false };
HWND s_gameWindow = NULL;

// Render thread, OpenGL only. Applies s_glVramHogMb and s_glGpuLoadBlits. Calls from this DLL bypass Toolscreen's GL
// hooks, and every binding it changes is put back so the game's GL state cache stays valid.
void ApplyGlGpuPressure() {
    const int blits = s_glGpuLoadBlits.load(std::memory_order_relaxed);
    const int vramMb = s_glVramHogMb.load(std::memory_order_relaxed);
    static std::vector<GLuint> s_vramTextures;
    if ((blits <= 0 && vramMb <= 0 && s_vramTextures.empty()) || GetRenderBackend() != RenderBackend::OpenGL ||
        !wglGetCurrentContext()) {
        return;
    }
    if (!glCreateTextures || !glTextureStorage2D || !glClearTexImage || !glCreateFramebuffers || !glNamedFramebufferTexture) {
        return;
    }

    while (static_cast<int>(s_vramTextures.size()) * kGlVramHogTextureMb < vramMb) {
        // 8192x8192 RGBA8 is 256 MiB; clearing it makes the driver back it with memory.
        GLuint texture = 0;
        glCreateTextures(GL_TEXTURE_2D, 1, &texture);
        glTextureStorage2D(texture, 1, GL_RGBA8, 8192, 8192);
        const GLubyte zero[4] = {};
        glClearTexImage(texture, 0, GL_RGBA, GL_UNSIGNED_BYTE, zero);
        s_vramTextures.push_back(texture);
    }
    if (vramMb <= 0 && !s_vramTextures.empty()) {
        glDeleteTextures(static_cast<GLsizei>(s_vramTextures.size()), s_vramTextures.data());
        s_vramTextures.clear();
    }

    if (blits <= 0) return;
    constexpr int kSize = 4096;
    static GLuint s_textures[2] = {};
    static GLuint s_framebuffers[2] = {};
    if (!s_framebuffers[0]) {
        glCreateTextures(GL_TEXTURE_2D, 2, s_textures);
        glCreateFramebuffers(2, s_framebuffers);
        for (int i = 0; i < 2; ++i) {
            glTextureStorage2D(s_textures[i], 1, GL_RGBA8, kSize, kSize);
            glNamedFramebufferTexture(s_framebuffers[i], GL_COLOR_ATTACHMENT0, s_textures[i], 0);
        }
    }
    GLint readFramebuffer = 0;
    GLint drawFramebuffer = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFramebuffer);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFramebuffer);
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    for (int i = 0; i < blits; ++i) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, s_framebuffers[i % 2]);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_framebuffers[1 - i % 2]);
        BlitFramebufferDirect(0, 0, kSize, kSize, 0, 0, kSize, kSize, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, readFramebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFramebuffer);
    if (scissor) glEnable(GL_SCISSOR_TEST);
}

std::string ReadEnvUtf8(const wchar_t* name) {
    wchar_t buffer[2048] = {};
    const DWORD length = GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));
    if (length == 0 || length >= std::size(buffer)) return {};
    return WideToUtf8(std::wstring(buffer, length));
}

std::string ToLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string JsonEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const char c : value) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char escaped[8];
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned char>(c));
                out += escaped;
            } else {
                out += c;
            }
        }
    }
    return out;
}

void AppendResultLine(const std::string& jsonLine) {
    std::ofstream out(std::filesystem::path(s_resultsPath), std::ios::app | std::ios::binary);
    out << jsonLine << "\n";
}

struct TestFailure {
    std::string message;
};

struct TestSkipped {
    std::string message;
};

void Require(bool condition, const std::string& message) {
    if (!condition) throw TestFailure{ message };
}

void Skip(const std::string& message) { throw TestSkipped{ message }; }

// When the render thread stops making progress, record where every relevant thread is (once per run).
void ReportRenderThreadStall(const std::string& reason) {
    static std::atomic<bool> reported{ false };
    if (reported.exchange(true)) return;
    const std::string stacks = DumpInterestingThreadStacks();
    Log("[GAME TEST] Render thread stalled (" + reason + "). Thread stacks:\n" + stacks);
    AppendResultLine("{\"event\":\"diagnostic\",\"reason\":\"" + JsonEscape(reason) + "\",\"stacks\":\"" + JsonEscape(stacks) + "\"}");
}

bool WaitUntil(const std::function<bool()>& predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (s_stopRequested.load(std::memory_order_acquire)) return false;
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return predicate();
}

bool WaitForFrames(uint64_t count, std::chrono::milliseconds timeout) {
    const uint64_t target = s_renderFrames.load(std::memory_order_acquire) + count;
    return WaitUntil([&] { return s_renderFrames.load(std::memory_order_acquire) >= target; }, timeout);
}

// Runs fn on the render thread (inside the next hooked frame) and waits for it to finish.
template <typename Fn>
auto RunOnRenderThread(Fn fn, std::chrono::milliseconds timeout = std::chrono::seconds(10)) -> decltype(fn()) {
    using Result = decltype(fn());
    auto task = std::make_shared<std::packaged_task<Result()>>(std::move(fn));
    std::future<Result> future = task->get_future();
    {
        std::lock_guard<std::mutex> lock(s_renderTasksMutex);
        s_renderTasks.emplace_back([task] { (*task)(); });
    }
    if (future.wait_for(timeout) != std::future_status::ready) {
        ReportRenderThreadStall("a queued render-thread step did not run");
        throw TestFailure{ "Timed out waiting for the render thread to run a test step." };
    }
    return future.get();
}

std::string BackendName(RenderBackend backend) {
    switch (backend) {
    case RenderBackend::OpenGL: return "opengl";
    case RenderBackend::Vulkan: return "vulkan";
    default: return "unknown";
    }
}

std::string VersionString(const GameVersion& version) {
    if (!version.valid) return "invalid";
    std::string text = std::to_string(version.major) + "." + std::to_string(version.minor);
    if (version.patch != 0 || version.major == 1) text += "." + std::to_string(version.patch);
    return text;
}

LPARAM BuildKeyLParam(DWORD vk, bool keyUp) {
    const UINT scanCode = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LPARAM lParam = 1 | (static_cast<LPARAM>(scanCode & 0xFF) << 16);
    if (keyUp) lParam |= (static_cast<LPARAM>(1) << 30) | (static_cast<LPARAM>(1) << 31);
    return lParam;
}

void PostKeyPress(HWND hwnd, DWORD vk) {
    PostMessageW(hwnd, WM_KEYDOWN, vk, BuildKeyLParam(vk, false));
    PostMessageW(hwnd, WM_KEYUP, vk, BuildKeyLParam(vk, true));
}

// Adds (or replaces) the test mode and hotkeys in the draft config and publishes it. Render thread only.
void InstallTestFixtureOnRenderThread() {
    auto modeIt = std::find_if(g_config.modes.begin(), g_config.modes.end(),
                               [](const ModeConfig& mode) { return EqualsIgnoreCase(mode.id, kTestModeId); });
    if (modeIt == g_config.modes.end()) {
        g_config.modes.emplace_back();
        modeIt = std::prev(g_config.modes.end());
    }
    ModeConfig& mode = *modeIt;
    mode = ModeConfig{};
    mode.id = kTestModeId;
    mode.width = kTestModeWidth;
    mode.height = kTestModeHeight;
    mode.manualWidth = kTestModeWidth;
    mode.manualHeight = kTestModeHeight;

    const std::string defaultMode = g_config.defaultMode.empty() ? "Fullscreen" : g_config.defaultMode;
    const auto upsertHotkey = [&](std::vector<DWORD> keys) {
        auto hotkeyIt = std::find_if(g_config.hotkeys.begin(), g_config.hotkeys.end(),
                                     [&](const HotkeyConfig& hotkey) { return hotkey.keys == keys; });
        if (hotkeyIt == g_config.hotkeys.end()) {
            g_config.hotkeys.emplace_back();
            hotkeyIt = std::prev(g_config.hotkeys.end());
        }
        *hotkeyIt = HotkeyConfig{};
        hotkeyIt->keys = std::move(keys);
        hotkeyIt->mainMode = defaultMode;
        hotkeyIt->secondaryMode = kTestModeId;
        hotkeyIt->debounce = 0;
        hotkeyIt->blockKeyFromGame = true;
    };
    upsertHotkey({ kTestHotkeyVk });
    // A side-specific modifier makes Toolscreen track exact modifiers, which needs the low-level keyboard hook.
    upsertHotkey({ VK_LCONTROL, kTestExactModifierHotkeyVk });
    upsertHotkey({ kTestRealInputHotkeyVk });

    ResizeHotkeySecondaryModes(g_config.hotkeys.size());
    for (size_t i = 0; i < g_config.hotkeys.size(); ++i) { SetHotkeySecondaryMode(i, g_config.hotkeys[i].secondaryMode); }
    RebuildHotkeyMainKeys();
    g_configIsDirty = true;
    PublishGuiConfigSnapshot();
}

constexpr const char* kTestMirrorMatchAll = "GameTestMirrorMatchAll";
constexpr const char* kTestMirrorMatchNone = "GameTestMirrorMatchNone";
constexpr const char* kTestMirrorAdded = "GameTestMirrorAdded";
constexpr const char* kTestMirrorOpaqueRealtime = "GameTestMirrorOpaqueRealtime";
constexpr const char* kTestMirrorOpaqueLimited = "GameTestMirrorOpaqueLimited";
constexpr const char* kTestMirrorStaticBorder = "GameTestMirrorStaticBorder";
constexpr const char* kTestColorKeyOverlay = "GameTestColorKeyOverlay";

void RemoveTestFixtureOnRenderThread() {
    std::erase_if(g_config.mirrors, [](const MirrorConfig& mirror) {
        return mirror.name == kTestMirrorMatchAll || mirror.name == kTestMirrorMatchNone || mirror.name == kTestMirrorAdded;
    });
    std::erase_if(g_config.modes, [](const ModeConfig& mode) { return EqualsIgnoreCase(mode.id, kTestModeId); });
    std::erase_if(g_config.hotkeys, [](const HotkeyConfig& hotkey) { return EqualsIgnoreCase(hotkey.secondaryMode, kTestModeId); });
    ResizeHotkeySecondaryModes(g_config.hotkeys.size());
    for (size_t i = 0; i < g_config.hotkeys.size(); ++i) { SetHotkeySecondaryMode(i, g_config.hotkeys[i].secondaryMode); }
    RebuildHotkeyMainKeys();
    g_configIsDirty = true;
    PublishGuiConfigSnapshot();
}

std::string DefaultModeId() {
    const auto snapshot = GetConfigSnapshot();
    return (snapshot && !snapshot->defaultMode.empty()) ? snapshot->defaultMode : "Fullscreen";
}

// SwitchToMode refuses a switch to the current mode, so treat "already there" as success.
bool SwitchModeOnRenderThread(const std::string& modeId) {
    if (EqualsIgnoreCase(GetPublishedCurrentModeId(), modeId)) return true;
    return RunOnRenderThread([modeId] { return SwitchToMode(modeId, "game test", true); });
}

// Like SwitchModeOnRenderThread, but plays the configured transition instead of forcing a cut.
bool SlideToModeOnRenderThread(const std::string& modeId) {
    if (EqualsIgnoreCase(GetPublishedCurrentModeId(), modeId)) return true;
    return RunOnRenderThread([modeId] { return SwitchToMode(modeId, "game test", false); });
}

bool WaitForPublishedMode(const std::string& modeId, std::chrono::milliseconds timeout) {
    return WaitUntil([&] { return EqualsIgnoreCase(GetPublishedCurrentModeId(), modeId); }, timeout);
}

// ---- Tests -------------------------------------------------------------------------------------

void TestVersionDetected() {
    Require(g_gameVersion.valid, "Toolscreen did not detect a game version.");
    if (!s_expectedVersion.empty()) {
        Require(VersionString(g_gameVersion) == s_expectedVersion,
                "Detected game version " + VersionString(g_gameVersion) + ", expected " + s_expectedVersion + ".");
    }
}

void TestWindowHooksReady() {
    Require(g_gameWindowHooksReady.load(std::memory_order_acquire), "Game-window hooks were not installed.");
    const HWND subclassed = g_subclassedHwnd.load(std::memory_order_acquire);
    Require(WaitUntil([] { return g_subclassedHwnd.load(std::memory_order_acquire) != NULL; }, std::chrono::seconds(10)),
            "The game window was never subclassed.");
    (void)subclassed;
    Require(IsWindow(g_subclassedHwnd.load(std::memory_order_acquire)) != FALSE, "The subclassed window handle is not a window.");
}

void TestRenderBackendLatched() {
    Require(WaitUntil([] { return GetRenderBackend() != RenderBackend::Unknown; }, std::chrono::seconds(20)),
            "No render backend was latched.");
    const std::string backend = BackendName(GetRenderBackend());
    if (!s_expectedBackend.empty()) {
        Require(backend == s_expectedBackend, "Render backend is " + backend + ", expected " + s_expectedBackend + ".");
    }
}

void TestFramesHooked() {
    if (!WaitForFrames(30, std::chrono::seconds(20))) {
        ReportRenderThreadStall("fewer than 30 hooked frames in 20 s");
        Require(false, "Fewer than 30 frames passed through the render hook in 20 s.");
    }
    Require(s_renderThreadId.load(std::memory_order_acquire) != 0, "The render thread was never identified.");
}

void TestConfigIsolated() {
    const std::wstring expectedDir = Utf8ToWide(ReadEnvUtf8(L"TOOLSCREEN_GAME_TEST_TOOLSCREEN_DIR"));
    if (expectedDir.empty()) Skip("TOOLSCREEN_GAME_TEST_TOOLSCREEN_DIR is not set.");
    std::error_code ec;
    Require(std::filesystem::equivalent(std::filesystem::path(g_toolscreenPath), std::filesystem::path(expectedDir), ec),
            "Toolscreen is using " + WideToUtf8(g_toolscreenPath) + " instead of the isolated test directory.");
}

void TestModeSwitchResizesGame() {
    if (!IsResolutionChangeSupported(g_gameVersion)) Skip("Resolution changes are not supported on this game version.");
    RunOnRenderThread([] { InstallTestFixtureOnRenderThread(); });

    Require(SwitchModeOnRenderThread(kTestModeId), "SwitchToMode refused the test mode.");
    Require(WaitForPublishedMode(kTestModeId, std::chrono::seconds(5)), "The test mode was not published as current.");

    if (GetRenderBackend() == RenderBackend::OpenGL) {
        int width = 0, height = 0;
        const bool resized = WaitUntil(
            [&] { return GetLatestGameViewportSize(width, height) && width == kTestModeWidth && height == kTestModeHeight; },
            std::chrono::seconds(5));
        Require(resized, "The game viewport did not become " + std::to_string(kTestModeWidth) + "x" + std::to_string(kTestModeHeight) +
                             " (last seen " + std::to_string(width) + "x" + std::to_string(height) + ").");
    } else {
        Require(WaitForFrames(10, std::chrono::seconds(5)), "Frames stopped after switching modes.");
    }

    const std::string defaultMode = DefaultModeId();
    Require(SwitchModeOnRenderThread(defaultMode), "SwitchToMode refused the default mode.");
    Require(WaitForPublishedMode(defaultMode, std::chrono::seconds(5)), "Switching back to the default mode did not publish it.");
    if (GetRenderBackend() == RenderBackend::OpenGL) {
        int width = 0, height = 0;
        Require(WaitUntil([&] { return GetLatestGameViewportSize(width, height) && (width != kTestModeWidth || height != kTestModeHeight); },
                          std::chrono::seconds(5)),
                "The game viewport stayed at the test mode size after switching back.");
    }
}

void TestHotkeySwitchesMode() {
    if (!IsResolutionChangeSupported(g_gameVersion)) Skip("Resolution changes are not supported on this game version.");
    RunOnRenderThread([] { InstallTestFixtureOnRenderThread(); });
    const HWND hwnd = g_subclassedHwnd.load(std::memory_order_acquire);
    Require(hwnd != NULL, "No subclassed game window to send keys to.");

    const std::string defaultMode = DefaultModeId();
    Require(SwitchModeOnRenderThread(defaultMode), "Could not start from the default mode.");
    Require(WaitForPublishedMode(defaultMode, std::chrono::seconds(5)), "The default mode was not published.");

    PostKeyPress(hwnd, kTestHotkeyVk);
    Require(WaitForPublishedMode(kTestModeId, std::chrono::seconds(5)),
            "Pressing the test hotkey did not switch to the test mode (current: " + GetPublishedCurrentModeId() + ").");

    PostKeyPress(hwnd, kTestHotkeyVk);
    Require(WaitForPublishedMode(defaultMode, std::chrono::seconds(5)),
            "Pressing the test hotkey again did not return to the default mode (current: " + GetPublishedCurrentModeId() + ").");
}

void TestGuiToggleRendersImGui() {
    g_showGui.store(true, std::memory_order_release);
    struct CloseGui {
        ~CloseGui() { g_showGui.store(false, std::memory_order_release); }
    } closeGui;

    Require(WaitForFrames(5, std::chrono::seconds(5)), "Frames stopped after opening the settings GUI.");
    const int firstFrame = RunOnRenderThread([] { return ImGui::GetCurrentContext() ? ImGui::GetFrameCount() : -1; });
    Require(firstFrame >= 0, "No ImGui context exists after opening the settings GUI.");
    Require(WaitForFrames(10, std::chrono::seconds(5)), "Frames stopped while the settings GUI was open.");
    const int laterFrame = RunOnRenderThread([] { return ImGui::GetCurrentContext() ? ImGui::GetFrameCount() : -1; });
    Require(laterFrame > firstFrame, "ImGui did not render new frames while the settings GUI was open.");
}

// EyeZoom renders a tall game image through the zoom snapshot, then keeps that snapshot alive while it transitions out.
// Switching in, out and between modes quickly (including mid-transition) exercises the snapshot's lifetime.
void TestEyeZoomRendersAndSwitches() {
    if (!IsResolutionChangeSupported(g_gameVersion)) Skip("Resolution changes are not supported on this game version.");
    const auto snapshot = GetConfigSnapshot();
    const bool hasEyeZoom = snapshot && std::any_of(snapshot->modes.begin(), snapshot->modes.end(),
                                                    [](const ModeConfig& mode) { return EqualsIgnoreCase(mode.id, "EyeZoom"); });
    if (!hasEyeZoom) Skip("The config has no EyeZoom mode.");
    RunOnRenderThread([] { InstallTestFixtureOnRenderThread(); });

    const std::string defaultMode = DefaultModeId();
    Require(SwitchModeOnRenderThread(defaultMode), "Could not start from the default mode.");
    Require(WaitForPublishedMode(defaultMode, std::chrono::seconds(5)), "The default mode was not published.");

    Require(SwitchModeOnRenderThread("EyeZoom"), "SwitchToMode refused EyeZoom.");
    Require(WaitForPublishedMode("EyeZoom", std::chrono::seconds(5)), "EyeZoom was not published as current.");
    Require(WaitUntil([] { return g_showEyeZoom.load(std::memory_order_acquire); }, std::chrono::seconds(5)),
            "EyeZoom became the current mode but the zoom overlay never showed.");
    Require(WaitForFrames(120, std::chrono::seconds(10)), "Frames stopped while EyeZoom was showing.");
    {
        const ModeConfig* eyeZoomMode = GetModeFromSnapshotOrFallback(*snapshot, "EyeZoom");
        const int wantW = eyeZoomMode ? eyeZoomMode->width : 0;
        const int wantH = eyeZoomMode ? eyeZoomMode->height : 0;
        int gameW = 0, gameH = 0;
        const bool sized = WaitUntil([&] { return GetLatestGameViewportSize(gameW, gameH) && gameW == wantW && gameH == wantH; },
                                     std::chrono::seconds(5));
        RECT client{};
        GetClientRect(g_subclassedHwnd.load(std::memory_order_acquire), &client);
        Log("[GAME TEST] EyeZoom game frame " + std::to_string(gameW) + "x" + std::to_string(gameH) + ", window client " +
            std::to_string(client.right) + "x" + std::to_string(client.bottom) + ", mode " + std::to_string(wantW) + "x" +
            std::to_string(wantH));
        Require(sized, "In EyeZoom the game rendered " + std::to_string(gameW) + "x" + std::to_string(gameH) + " instead of the mode's " +
                           std::to_string(wantW) + "x" + std::to_string(wantH) + " (window client " + std::to_string(client.right) + "x" +
                           std::to_string(client.bottom) + ").");
    }

    // The settings GUI draws an EyeZoom preview from the same snapshot.
    g_showGui.store(true, std::memory_order_release);
    const bool guiFrames = WaitForFrames(60, std::chrono::seconds(10));
    g_showGui.store(false, std::memory_order_release);
    Require(guiFrames, "Frames stopped with the settings GUI open in EyeZoom.");

    Require(SwitchModeOnRenderThread(defaultMode), "SwitchToMode refused the default mode after EyeZoom.");
    Require(WaitForPublishedMode(defaultMode, std::chrono::seconds(5)), "Leaving EyeZoom did not publish the default mode.");
    Require(WaitUntil([] { return !g_showEyeZoom.load(std::memory_order_acquire) &&
                                  !g_isTransitioningFromEyeZoom.load(std::memory_order_acquire); },
                      std::chrono::seconds(5)),
            "The EyeZoom overlay or its exit transition never finished after leaving EyeZoom.");
    Require(WaitForFrames(30, std::chrono::seconds(5)), "Frames stopped after leaving EyeZoom.");

    // EyeZoom straight to another resized mode and back, with no stop at the default mode.
    Require(SwitchModeOnRenderThread("EyeZoom"), "SwitchToMode refused EyeZoom the second time.");
    Require(WaitForFrames(20, std::chrono::seconds(5)), "Frames stopped re-entering EyeZoom.");
    Require(SwitchModeOnRenderThread(kTestModeId), "SwitchToMode refused EyeZoom -> test mode.");
    Require(WaitForFrames(20, std::chrono::seconds(5)), "Frames stopped switching EyeZoom -> test mode.");
    Require(SwitchModeOnRenderThread("EyeZoom"), "SwitchToMode refused test mode -> EyeZoom.");
    Require(WaitForFrames(20, std::chrono::seconds(5)), "Frames stopped switching test mode -> EyeZoom.");

    // Rapid toggles, each landing before the previous resize or exit transition has settled.
    for (int i = 0; i < 20; ++i) {
        const std::string target = (i % 2 == 0) ? defaultMode : std::string("EyeZoom");
        Require(SwitchModeOnRenderThread(target), "SwitchToMode refused " + target + " during rapid toggling (step " + std::to_string(i) + ").");
        Require(WaitForFrames(static_cast<uint64_t>(1 + i % 4), std::chrono::seconds(5)),
                "Frames stopped during rapid EyeZoom toggling (step " + std::to_string(i) + ").");
    }

    Require(SwitchModeOnRenderThread(defaultMode), "Could not return to the default mode.");
    Require(WaitForPublishedMode(defaultMode, std::chrono::seconds(5)), "The default mode was not published after toggling.");
    Require(WaitUntil([] { return !g_showEyeZoom.load(std::memory_order_acquire) &&
                                  !g_isTransitioningFromEyeZoom.load(std::memory_order_acquire); },
                      std::chrono::seconds(5)),
            "EyeZoom stayed visible after rapid toggling ended on the default mode.");
    Require(WaitForFrames(60, std::chrono::seconds(10)), "Frames stopped after rapid EyeZoom toggling.");
}

struct ModeTransitionSetting {
    std::string modeId;
    GameTransitionType gameTransition = GameTransitionType::Cut;
    int durationMs = 0;
};

// Sets the game transition and its duration of the named modes and publishes the config. Returns the previous values.
// Render thread only.
std::vector<ModeTransitionSetting> SetGameTransitionsOnRenderThread(const std::vector<ModeTransitionSetting>& transitions) {
    std::vector<ModeTransitionSetting> previous;
    for (const ModeTransitionSetting& setting : transitions) {
        for (ModeConfig& mode : g_config.modes) {
            if (!EqualsIgnoreCase(mode.id, setting.modeId)) continue;
            previous.push_back({ mode.id, mode.gameTransition, mode.transitionDurationMs });
            mode.gameTransition = setting.gameTransition;
            mode.transitionDurationMs = setting.durationMs;
        }
    }
    g_configIsDirty = true;
    PublishGuiConfigSnapshot();
    return previous;
}

// Restores transitions changed by SetGameTransitionsOnRenderThread when it goes out of scope.
struct RestoreGameTransitions {
    std::vector<ModeTransitionSetting> values;
    ~RestoreGameTransitions() {
        if (values.empty()) return;
        try {
            RunOnRenderThread([values = values] { SetGameTransitionsOnRenderThread(values); });
        } catch (...) {}
    }
};

// A slide (Bounce) transition out of EyeZoom keeps drawing the zoom from a frozen snapshot while the game resizes.
void TestEyeZoomSlideTransitions() {
    if (!IsResolutionChangeSupported(g_gameVersion)) Skip("Resolution changes are not supported on this game version.");
    const auto snapshot = GetConfigSnapshot();
    const bool hasEyeZoom = snapshot && std::any_of(snapshot->modes.begin(), snapshot->modes.end(),
                                                    [](const ModeConfig& mode) { return EqualsIgnoreCase(mode.id, "EyeZoom"); });
    if (!hasEyeZoom) Skip("The config has no EyeZoom mode.");

    const std::string defaultMode = DefaultModeId();
    Require(SwitchModeOnRenderThread(defaultMode), "Could not start from the default mode.");
    Require(WaitForPublishedMode(defaultMode, std::chrono::seconds(5)), "The default mode was not published.");

    // On Vulkan the slide out draws from a snapshot of the area EyeZoom magnifies, kept only while a slide can use it.
    const bool vulkan = GetRenderBackend() == RenderBackend::Vulkan;
    const auto snapshotStats = [] { return RunOnRenderThread([] { return VulkanRenderer::GetEyeZoomSnapshotStatsForTests(); }); };
    const bool configSlidesOut = std::any_of(snapshot->modes.begin(), snapshot->modes.end(), [](const ModeConfig& mode) {
        return !EqualsIgnoreCase(mode.id, "EyeZoom") && mode.gameTransition == GameTransitionType::Bounce;
    });
    if (vulkan && !configSlidesOut) {
        Require(SwitchModeOnRenderThread("EyeZoom"), "SwitchToMode refused EyeZoom with Cut transitions.");
        Require(WaitForPublishedMode("EyeZoom", std::chrono::seconds(5)), "EyeZoom was not published with Cut transitions.");
        Require(WaitUntil([] { return g_showEyeZoom.load(std::memory_order_acquire); }, std::chrono::seconds(5)),
                "EyeZoom never showed with Cut transitions.");
        Require(WaitForFrames(20, std::chrono::seconds(5)), "Frames stopped in EyeZoom with Cut transitions.");
        const VulkanRenderer::EyeZoomSnapshotStats stats = snapshotStats();
        Require(stats.allocatedSlots == 0, "EyeZoom kept " + std::to_string(stats.allocatedSlots) +
                                               " snapshot images although no transition can slide it out.");
        Require(SwitchModeOnRenderThread(defaultMode), "SwitchToMode refused the default mode after Cut EyeZoom.");
        Require(WaitForPublishedMode(defaultMode, std::chrono::seconds(5)), "The default mode was not published after Cut EyeZoom.");
    }
    const ModeConfig* eyeZoomMode = GetModeFromSnapshotOrFallback(*snapshot, "EyeZoom");
    const int cloneW = std::clamp(snapshot->eyezoom.cloneWidth, 1, (std::max)(1, eyeZoomMode ? eyeZoomMode->width : 1));
    const int cloneH = std::clamp(snapshot->eyezoom.cloneHeight, 1, (std::max)(1, eyeZoomMode ? eyeZoomMode->height : 1));

    // Long enough that the game renders frames at the new size before the slide ends, even when it lags.
    constexpr int kSlideMs = 600;
    RestoreGameTransitions restore;
    restore.values = RunOnRenderThread([defaultMode] {
        return SetGameTransitionsOnRenderThread(
            { { defaultMode, GameTransitionType::Bounce, kSlideMs }, { "EyeZoom", GameTransitionType::Bounce, kSlideMs } });
    });

    // Full slide in and out, three times. Test switches are cuts unless they ask for the configured transition.
    bool sawSlideOut = false;
    for (int round = 0; round < 3; ++round) {
        Require(SlideToModeOnRenderThread("EyeZoom"), "SwitchToMode refused EyeZoom (round " + std::to_string(round) + ").");
        Require(WaitUntil([] { return g_showEyeZoom.load(std::memory_order_acquire); }, std::chrono::seconds(5)),
                "EyeZoom never showed with a slide transition (round " + std::to_string(round) + ").");
        Require(WaitUntil([] { return !IsModeTransitionActive(); }, std::chrono::seconds(5)),
                "The slide into EyeZoom never finished (round " + std::to_string(round) + ").");
        Require(WaitForFrames(20, std::chrono::seconds(5)), "Frames stopped in EyeZoom after a slide in.");
        if (vulkan) {
            const VulkanRenderer::EyeZoomSnapshotStats stats = snapshotStats();
            Require(stats.latestWidth == cloneW && stats.latestHeight == cloneH,
                    "The EyeZoom snapshot is " + std::to_string(stats.latestWidth) + "x" + std::to_string(stats.latestHeight) +
                        " instead of the " + std::to_string(cloneW) + "x" + std::to_string(cloneH) + " area EyeZoom magnifies.");
        }

        const uint64_t snapshotFramesBefore = vulkan ? snapshotStats().framesShown : 0;
        Require(SlideToModeOnRenderThread(defaultMode), "SwitchToMode refused the default mode (round " + std::to_string(round) + ").");
        sawSlideOut |= WaitUntil([] { return g_isTransitioningFromEyeZoom.load(std::memory_order_acquire); }, std::chrono::seconds(2));
        Require(WaitUntil([] { return !IsModeTransitionActive() && !g_isTransitioningFromEyeZoom.load(std::memory_order_acquire) &&
                                      !g_showEyeZoom.load(std::memory_order_acquire); },
                          std::chrono::seconds(5)),
                "The slide out of EyeZoom never finished (round " + std::to_string(round) + ").");
        Require(WaitForFrames(20, std::chrono::seconds(5)), "Frames stopped after a slide out of EyeZoom.");
        if (vulkan) {
            // The small images stay for the next entry, but no frame of this one may show in a later slide.
            const VulkanRenderer::EyeZoomSnapshotStats stats = snapshotStats();
            Require(stats.framesShown > snapshotFramesBefore,
                    "The slide out of EyeZoom never used the EyeZoom snapshot (round " + std::to_string(round) + ").");
            Require(stats.allocatedSlots > 0, "The EyeZoom snapshot images were freed on leaving EyeZoom.");
            Require(stats.latestWidth == 0, "The EyeZoom snapshot still offers a frame after EyeZoom ended.");
        }
    }
    Require(sawSlideOut, "Leaving EyeZoom with a slide transition never reported the EyeZoom exit transition.");

    // Reverse each slide part-way through, so a new transition starts while the frozen snapshot is still in use.
    for (int i = 0; i < 20; ++i) {
        const std::string target = (i % 2 == 0) ? std::string("EyeZoom") : defaultMode;
        Require(SlideToModeOnRenderThread(target), "SwitchToMode refused " + target + " mid-slide (step " + std::to_string(i) + ").");
        Require(WaitForFrames(static_cast<uint64_t>(2 + i % 5), std::chrono::seconds(5)),
                "Frames stopped while reversing slides (step " + std::to_string(i) + ").");
    }

    Require(SwitchModeOnRenderThread(defaultMode), "Could not return to the default mode.");
    Require(WaitUntil([] { return !IsModeTransitionActive() && !g_isTransitioningFromEyeZoom.load(std::memory_order_acquire) &&
                                  !g_showEyeZoom.load(std::memory_order_acquire); },
                      std::chrono::seconds(5)),
            "EyeZoom stayed visible after the reversed slides ended on the default mode.");
    Require(WaitForFrames(60, std::chrono::seconds(10)), "Frames stopped after reversing EyeZoom slides.");
}

void TestLowLevelHookOnDedicatedThread() {
    RunOnRenderThread([] { InstallTestFixtureOnRenderThread(); });
    const HWND hwnd = g_subclassedHwnd.load(std::memory_order_acquire);
    if (!IsWindowInForegroundTree(hwnd)) Skip("The game window is not in the foreground, so the low-level hook is not requested.");
    // The hook state is re-evaluated on the next window message.
    PostMessageW(hwnd, WM_NULL, 0, 0);

    DWORD hookThreadId = 0;
    const bool installed =
        WaitUntil([&] { return GetLowLevelKeyboardHookState(hookThreadId) && hookThreadId != 0; }, std::chrono::seconds(5));
    Require(installed, "The low-level keyboard hook was not installed for a side-specific modifier hotkey.");
    const DWORD windowThreadId = GetWindowThreadProcessId(hwnd, nullptr);
    Require(hookThreadId != windowThreadId, "The low-level keyboard hook runs on the game's window thread.");
    Require(hookThreadId != s_renderThreadId.load(std::memory_order_acquire), "The low-level keyboard hook runs on the render thread.");
}

void SendRealKey(DWORD vk, bool keyUp) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = static_cast<WORD>(vk);
    input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    input.ki.dwFlags = keyUp ? KEYEVENTF_KEYUP : 0;
    SendInput(1, &input, sizeof(input));
}

// A real key press must reach Windows (so other programs and shell shortcuts still see it) and must trigger the
// hotkey exactly once. On Vulkan the low-level hook hands the window an early copy of each key, so this also checks
// that the delayed native message is dropped instead of toggling the mode a second time.
void TestRealKeyPassesThroughOnce() {
    if (!IsResolutionChangeSupported(g_gameVersion)) Skip("Resolution changes are not supported on this game version.");
    RunOnRenderThread([] { InstallTestFixtureOnRenderThread(); });
    const HWND hwnd = g_subclassedHwnd.load(std::memory_order_acquire);
    Require(hwnd != NULL, "No subclassed game window to send keys to.");
    if (s_gameWindow) SetForegroundWindow(s_gameWindow);
    if (!WaitUntil([&] { return IsWindowInForegroundTree(hwnd); }, std::chrono::seconds(2))) {
        Skip("The game window is not in the foreground, so injected keys would go elsewhere.");
    }
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DWORD hookThreadId = 0;
    Require(WaitUntil([&] { return GetLowLevelKeyboardHookState(hookThreadId); }, std::chrono::seconds(5)),
            "The low-level keyboard hook was not installed.");

    const std::string defaultMode = DefaultModeId();
    Require(SwitchModeOnRenderThread(defaultMode), "Could not start from the default mode.");
    Require(WaitForPublishedMode(defaultMode, std::chrono::seconds(5)), "The default mode was not published.");

    SendRealKey(kTestRealInputHotkeyVk, false);
    const bool keyStateDown = WaitUntil([] { return (GetAsyncKeyState(kTestRealInputHotkeyVk) & 0x8000) != 0; }, std::chrono::seconds(1));
    const bool switched = WaitForPublishedMode(kTestModeId, std::chrono::seconds(5));
    SendRealKey(kTestRealInputHotkeyVk, true);
    Require(keyStateDown, "The low-level hook kept the key press from reaching Windows.");
    Require(switched, "A real press of the test hotkey did not switch to the test mode (current: " + GetPublishedCurrentModeId() + ").");

    // A duplicate key-down from the delayed native message would toggle straight back to the default mode.
    Require(WaitForFrames(30, std::chrono::seconds(5)), "Frames stopped after the hotkey press.");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    Require(EqualsIgnoreCase(GetPublishedCurrentModeId(), kTestModeId),
            "One real press of the test hotkey toggled the mode twice (current: " + GetPublishedCurrentModeId() + ").");
    Require(WaitUntil([] { return (GetAsyncKeyState(kTestRealInputHotkeyVk) & 0x8000) == 0; }, std::chrono::seconds(1)),
            "The key release did not reach Windows.");

    Require(SwitchModeOnRenderThread(defaultMode), "Could not return to the default mode.");
}

void TestConfigSaveRoundTrip() {
    // Modes live in the active profile file, beside the shared config.toml.
    const std::string activeProfile = RunOnRenderThread([] {
        InstallTestFixtureOnRenderThread();
        SaveConfigImmediate();
        return g_profilesConfig.activeProfile;
    });
    const std::wstring configPath = g_toolscreenPath + L"\\config.toml";
    Require(std::filesystem::exists(std::filesystem::path(configPath)), "config.toml was not written to " + WideToUtf8(configPath) + ".");
    Config sharedConfig;
    Require(LoadConfigFromTomlFile(configPath, sharedConfig), "The saved config.toml could not be parsed.");

    const std::wstring profilePath = g_toolscreenPath + L"\\profiles\\" + Utf8ToWide(activeProfile) + L".toml";
    Config profileConfig;
    Require(LoadConfigFromTomlFile(profilePath, profileConfig),
            "The active profile " + WideToUtf8(profilePath) + " could not be parsed.");
    const bool hasMode = std::any_of(profileConfig.modes.begin(), profileConfig.modes.end(),
                                     [](const ModeConfig& mode) { return EqualsIgnoreCase(mode.id, kTestModeId); });
    Require(hasMode, "The saved profile " + activeProfile + " does not contain the test mode.");
}

// Loads a copy of tests/game/overlay (staged beside Toolscreen.dll), which detours wglSwapBuffers the way a
// third-party overlay does, and returns its module.
struct TestOverlay {
    HMODULE module = NULL;
    void* detour = nullptr;
    LONG(*callCount)() = nullptr;
    BOOL(*remove)() = nullptr;
};

TestOverlay LoadTestOverlay(const wchar_t* fileName) {
    HMODULE self = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&LoadTestOverlay), &self);
    wchar_t selfPath[MAX_PATH] = {};
    GetModuleFileNameW(self, selfPath, MAX_PATH);
    const std::filesystem::path overlayPath = std::filesystem::path(selfPath).parent_path() / fileName;
    if (!std::filesystem::exists(overlayPath)) Skip("Test overlay " + WideToUtf8(overlayPath.wstring()) + " is not staged.");

    TestOverlay overlay;
    overlay.module = LoadLibraryW(overlayPath.c_str());
    Require(overlay.module != NULL, "Could not load " + WideToUtf8(overlayPath.wstring()) + ".");
    overlay.detour = reinterpret_cast<void*>(GetProcAddress(overlay.module, "GameTestOverlaySwapBuffers"));
    overlay.callCount = reinterpret_cast<LONG (*)()>(GetProcAddress(overlay.module, "GameTestOverlayCallCount"));
    overlay.remove = reinterpret_cast<BOOL (*)()>(GetProcAddress(overlay.module, "GameTestOverlayRemove"));
    auto install = reinterpret_cast<BOOL (*)()>(GetProcAddress(overlay.module, "GameTestOverlayInstall"));
    Require(overlay.detour && overlay.callCount && overlay.remove && install, "The test overlay is missing exports.");
    Require(install() != FALSE, "The test overlay could not hook wglSwapBuffers.");
    return overlay;
}

// Unhooks the overlay, lets any frame already inside it finish, then unloads it.
void UnloadTestOverlay(TestOverlay& overlay) {
    if (!overlay.module) return;
    overlay.remove();
    WaitForFrames(10, std::chrono::seconds(5));
    FreeLibrary(overlay.module);
    overlay = {};
}

void RequireChainedThrough(TestOverlay& overlay, const char* label) {
    const bool chained = WaitUntil([&] { return HookChain::GetThirdPartyWglSwapBuffersHookTarget() == overlay.detour; },
                                   std::chrono::seconds(10));
    Require(chained, std::string("Toolscreen did not chain behind ") + label + " (chained target: " +
                         HookChain::DescribeAddressWithOwner(HookChain::GetThirdPartyWglSwapBuffersHookTarget()) + ").");

    const LONG overlayCallsBefore = overlay.callCount();
    Require(WaitForFrames(20, std::chrono::seconds(10)), std::string("Toolscreen stopped rendering frames while chained behind ") + label + ".");
    Require(overlay.callCount() > overlayCallsBefore, std::string("Frames stopped reaching ") + label + " after Toolscreen chained behind it.");
}

void TestThirdPartySwapBuffersChain() {
    WaitUntil([] { return GetRenderBackend() != RenderBackend::Unknown; }, std::chrono::seconds(20));
    if (GetRenderBackend() != RenderBackend::OpenGL) Skip("wglSwapBuffers hook chaining only applies to OpenGL.");
    Require(HookChain::GetThirdPartyWglSwapBuffersHookTarget() == nullptr, "Toolscreen is already chained behind another overlay.");

    TestOverlay first = LoadTestOverlay(L"game_test_overlay_a.dll");
    struct UnloadOnExit {
        TestOverlay& overlay;
        ~UnloadOnExit() { UnloadTestOverlay(overlay); }
    } unloadFirst{ first };
    RequireChainedThrough(first, "the first overlay");
    const void* firstDetour = first.detour;

    // The overlay goes away: Toolscreen must notice its target is stale and drop the chain.
    const auto unloadAndRequireUnchained = [](TestOverlay& overlay, const char* label) {
        UnloadTestOverlay(overlay);
        Require(WaitUntil([] { return HookChain::GetThirdPartyWglSwapBuffersHookTarget() == nullptr; }, std::chrono::seconds(10)),
                std::string("Toolscreen kept chaining through ") + label + " after it unloaded.");
        Require(WaitForFrames(20, std::chrono::seconds(10)), std::string("Toolscreen stopped rendering after ") + label + " unloaded.");
    };
    unloadAndRequireUnchained(first, "the first overlay");

    // The same overlay reloads. Windows maps the same file back at its old base, where MinHook still holds
    // the dead module's hook entry; Toolscreen must chain behind the fresh code anyway.
    TestOverlay reloaded = LoadTestOverlay(L"game_test_overlay_a.dll");
    UnloadOnExit unloadReloaded{ reloaded };
    RequireChainedThrough(reloaded, reloaded.detour == firstDetour ? "the reloaded overlay (same address)" : "the reloaded overlay");
    unloadAndRequireUnchained(reloaded, "the reloaded overlay");

    // A different overlay arrives, exercising a reinstall that reuses Toolscreen's trampoline slot.
    TestOverlay second = LoadTestOverlay(L"game_test_overlay_b.dll");
    UnloadOnExit unloadSecond{ second };
    RequireChainedThrough(second, "a second overlay");
}

// While one thread blocks in vkWaitForFences through Toolscreen's Vulkan layer, another thread's call through
// the layer must not queue behind it (the layer used to hold its global mutex across the wait).
void TestVulkanLayerWaitDoesNotBlockOtherCalls() {
    WaitUntil([] { return GetRenderBackend() != RenderBackend::Unknown; }, std::chrono::seconds(20));
    if (GetRenderBackend() != RenderBackend::Vulkan) Skip("Toolscreen's Vulkan layer only runs on the Vulkan backend.");

    VkDevice device = VK_NULL_HANDLE;
    PFN_vkGetDeviceProcAddr layerGdpa = nullptr;
    PFN_vkGetDeviceProcAddr nextGdpa = nullptr;
    if (!ToolscreenVulkanLayerTest::GetTrackedDevice(device, layerGdpa, nextGdpa)) {
        Skip("Toolscreen's Vulkan layer is not tracking a device in this process.");
    }
    auto createFence = reinterpret_cast<PFN_vkCreateFence>(nextGdpa(device, "vkCreateFence"));
    auto layerWait = reinterpret_cast<PFN_vkWaitForFences>(layerGdpa(device, "vkWaitForFences"));
    auto layerDestroyFence = reinterpret_cast<PFN_vkDestroyFence>(layerGdpa(device, "vkDestroyFence"));
    Require(createFence && layerWait && layerDestroyFence, "Could not resolve the fence functions.");

    VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence neverSignalled = VK_NULL_HANDLE;
    VkFence other = VK_NULL_HANDLE;
    Require(createFence(device, &fenceInfo, nullptr, &neverSignalled) == VK_SUCCESS &&
                createFence(device, &fenceInfo, nullptr, &other) == VK_SUCCESS,
            "Could not create test fences.");

    constexpr uint64_t kWaitNs = 2'000'000'000ull;
    std::atomic<VkResult> waitResult{ VK_SUCCESS };
    std::thread waiter([&] { waitResult = layerWait(device, 1, &neverSignalled, VK_TRUE, kWaitNs); });
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    const auto start = std::chrono::steady_clock::now();
    layerDestroyFence(device, other, nullptr);
    const auto blockedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

    waiter.join();
    layerDestroyFence(device, neverSignalled, nullptr);

    Require(waitResult.load() == VK_TIMEOUT, "vkWaitForFences on an unsignalled fence did not time out.");
    Require(blockedMs < 500, "vkDestroyFence through the layer was blocked for " + std::to_string(blockedMs) +
                                 " ms behind another thread's vkWaitForFences.");
}

// Overlay frames must only count as finished once the GPU has run them, and every frame must get a tracking
// slot; otherwise retired textures, fonts and mirror snapshots can be destroyed while still in use.
void TestVulkanFrameCompletionTracking() {
    WaitUntil([] { return GetRenderBackend() != RenderBackend::Unknown; }, std::chrono::seconds(20));
    if (GetRenderBackend() != RenderBackend::Vulkan) Skip("GPU frame-completion tracking is part of the Vulkan renderer.");
    Require(WaitUntil([] { return VulkanRenderer::GetFrameTrackingStats().completedFrames > 0; }, std::chrono::seconds(20)),
            "No overlay frame was ever reported complete.");

    VulkanRenderer::SetFrameTrackingProbeEnabled(true);
    const VulkanRenderer::FrameTrackingStats before = VulkanRenderer::GetFrameTrackingStats();
    const bool rendered = WaitForFrames(120, std::chrono::seconds(30));
    const VulkanRenderer::FrameTrackingStats after = VulkanRenderer::GetFrameTrackingStats();
    VulkanRenderer::SetFrameTrackingProbeEnabled(false);
    Require(rendered, "Fewer than 120 frames were rendered.");

    const uint64_t completed = after.completedFrames - before.completedFrames;
    const uint64_t early = after.earlyAvailabilityFrames - before.earlyAvailabilityFrames;
    const uint64_t untracked = after.untrackedFrames - before.untrackedFrames;
    Log("[GAME TEST] Vulkan frame tracking over 120 frames: completed=" + std::to_string(completed) +
        " heldBackFromEarlyAvailability=" + std::to_string(early) + " untracked=" + std::to_string(untracked) +
        " presentProbeStaleAvailability=" + std::to_string(after.probeStaleAvailability - before.probeStaleAvailability) + "/" +
        std::to_string(after.probeSamples - before.probeSamples));
    Require(completed >= 60, "Only " + std::to_string(completed) + " of 120 overlay frames were reported complete.");
    Require(untracked == 0, std::to_string(untracked) + " overlay frames had no free tracking slot.");
}

// A window overlay's streaming texture must be replaced safely when its size changes, and its GPU resources
// must be released once it stops being drawn (deleted overlays used to keep them until the swapchain went away).
void TestVulkanStreamingTextureLifetime() {
    WaitUntil([] { return GetRenderBackend() != RenderBackend::Unknown; }, std::chrono::seconds(20));
    if (GetRenderBackend() != RenderBackend::Vulkan) Skip("Streaming textures are part of the Vulkan renderer.");
    if (!IsResolutionChangeSupported(g_gameVersion)) Skip("Modes are not supported on this game version.");

    static constexpr const char* kOverlayName = "GameTestStream";
    const std::string key = std::string("window:") + kOverlayName;
    WindowOverlayConfig overlay;
    overlay.name = kOverlayName;
    overlay.windowTitle = "Toolscreen game test window that does not exist";
    const auto stage = [&](int size, unsigned char shade) {
        std::vector<unsigned char> pixels(static_cast<size_t>(size) * size * 4, shade);
        Require(StageWindowOverlayTestFrame(overlay, pixels, size, size), "Could not stage a window overlay frame.");
    };
    const auto stats = [&] { return RunOnRenderThread([&] { return VulkanRenderer::GetStreamingTextureStats(key); }); };

    RunOnRenderThread([&] {
        InstallTestFixtureOnRenderThread();
        auto mode = std::find_if(g_config.modes.begin(), g_config.modes.end(),
                                 [](const ModeConfig& m) { return EqualsIgnoreCase(m.id, kTestModeId); });
        mode->sources.push_back({ ModeSourceType::WindowOverlay, kOverlayName });
        g_config.windowOverlays.push_back(overlay);
        g_configIsDirty = true;
        PublishGuiConfigSnapshot();
    });
    struct RemoveOverlay {
        ~RemoveOverlay() {
            try {
                RunOnRenderThread([] {
                    std::erase_if(g_config.windowOverlays, [](const WindowOverlayConfig& o) { return o.name == kOverlayName; });
                    for (ModeConfig& mode : g_config.modes) {
                        std::erase_if(mode.sources, [](const ModeSourceRef& s) { return s.id == kOverlayName; });
                    }
                    g_configIsDirty = true;
                    PublishGuiConfigSnapshot();
                });
            } catch (...) {}
            RemoveWindowOverlayFromCache(kOverlayName);
        }
    } removeOverlay;

    stage(64, 40);
    Require(SwitchModeOnRenderThread(kTestModeId), "Could not switch to the test mode.");
    Require(WaitUntil([&] { return stats().slots > 0; }, std::chrono::seconds(10)),
            "The window overlay never got a streaming texture.");

    // A new size replaces the slot; the old one must be retired, not left behind or destroyed while in flight.
    stage(96, 200);
    Require(WaitUntil([&] { return stats().largestWidth == 96; }, std::chrono::seconds(10)),
            "The streaming texture was not recreated at the new size.");
    Require(WaitForFrames(30, std::chrono::seconds(10)), "Frames stopped after resizing the streaming texture.");

    // Delete the overlay while staying in the same mode, so no swapchain recreation frees anything for us.
    // Its slots must be released once they have been idle for the eviction window.
    RunOnRenderThread([] {
        std::erase_if(g_config.windowOverlays, [](const WindowOverlayConfig& o) { return o.name == kOverlayName; });
        for (ModeConfig& mode : g_config.modes) {
            std::erase_if(mode.sources, [](const ModeSourceRef& s) { return s.id == kOverlayName; });
        }
        g_configIsDirty = true;
        PublishGuiConfigSnapshot();
    });
    RemoveWindowOverlayFromCache(kOverlayName);
    const bool released = WaitUntil([&] { return stats().keys == 0; }, std::chrono::seconds(30));
    const VulkanRenderer::StreamingTextureStats remaining = stats();
    Require(released, "The deleted overlay still holds " + std::to_string(remaining.slots) + " streaming texture slots under " +
                          std::to_string(remaining.keys) + " key(s).");
    Require(WaitUntil([&] { return stats().retiredAwaitingGpu == 0; }, std::chrono::seconds(10)),
            "Retired texture resources were never destroyed (" + std::to_string(stats().retiredAwaitingGpu) + " remain).");
    Require(SwitchModeOnRenderThread(DefaultModeId()), "Could not switch back to the default mode.");
}

// The mirrors tab's "add new mirror" button calls AddMirrorToCurrentMode from the GUI pass. On OpenGL that creates
// the mirror's FBOs on the spot; on Vulkan there is no GL context, so it must leave creation to the Vulkan renderer.
void TestAddMirrorToCurrentMode() {
    Require(WaitUntil([] { return GetRenderBackend() != RenderBackend::Unknown; }, std::chrono::seconds(20)),
            "The render backend never latched.");
    if (!IsResolutionChangeSupported(g_gameVersion)) Skip("Resolution changes are not supported on this game version.");

    RunOnRenderThread([] { InstallTestFixtureOnRenderThread(); });
    Require(SwitchModeOnRenderThread(kTestModeId), "SwitchToMode refused the test mode.");
    Require(WaitForPublishedMode(kTestModeId, std::chrono::seconds(5)), "The test mode was not published as current.");

    RunOnRenderThread([] {
        MirrorConfig mirror;
        mirror.name = kTestMirrorAdded;
        mirror.output.relativeTo = "topLeftScreen";
        mirror.output.x = 10;
        mirror.output.y = 10;
        mirror.rawOutput = true;
        mirror.border.type = MirrorBorderType::Static;
        MirrorCaptureConfig zone;
        zone.relativeTo = "topLeftScreen";
        mirror.input.push_back(zone);
        AddMirrorToCurrentMode(std::move(mirror));
        PublishGuiConfigSnapshot();
    });

    const bool created = WaitUntil([] {
        return RunOnRenderThread([] {
            std::shared_lock<std::shared_mutex> lock(g_mirrorInstancesMutex);
            const auto it = g_mirrorInstances.find(kTestMirrorAdded);
            if (it == g_mirrorInstances.end()) return false;
            if (GetRenderBackend() == RenderBackend::OpenGL) return it->second.fbo != 0 && it->second.finalFbo != 0;
            return it->second.cachedRenderState.isValid;
        });
    }, std::chrono::seconds(5));
    Require(WaitForFrames(10, std::chrono::seconds(5)), "Frames stopped after adding a mirror.");

    RunOnRenderThread([] { RemoveTestFixtureOnRenderThread(); });
    SwitchModeOnRenderThread(DefaultModeId());

    Require(created, "The added mirror was never created or rendered by the active backend.");
}

// Minecraft leaves partial alpha in its frame where it blends translucent geometry (and in the sky with Fabulous
// graphics). Mirrors draw the game as an opaque copy, so a mirror over such a pixel must show its color exactly
// instead of blending it with whatever is underneath. Covers both the realtime source view and the fps-limited copy.
void TestVulkanMirrorsDrawGameOpaque() {
    WaitUntil([] { return GetRenderBackend() != RenderBackend::Unknown; }, std::chrono::seconds(20));
    if (GetRenderBackend() != RenderBackend::Vulkan) Skip("This reads back the Vulkan renderer's presented frame.");
    Require(WaitForFrames(120, std::chrono::seconds(20)), "Frames stopped before the test started.");

    int gameW = 0, gameH = 0;
    Require(GetLatestGameViewportSize(gameW, gameH), "The game viewport size is unknown.");

    const auto sampleSource = [](int x, int y, std::array<float, 4>& color) {
        return WaitUntil([&] { return RunOnRenderThread([&] { return VulkanRenderer::TryGetColorPickerSample(x, y, color); }); },
                         std::chrono::seconds(3));
    };
    const auto samplePresented = [](int x, int y, std::array<float, 4>& color) {
        return WaitUntil([&] { return RunOnRenderThread([&] { return VulkanRenderer::TryGetPresentedPixelForTests(x, y, color); }); },
                         std::chrono::seconds(3));
    };
    const auto rgbDistance = [](const std::array<float, 4>& a, const std::array<float, 4>& b) {
        return (std::max)({ std::abs(a[0] - b[0]), std::abs(a[1] - b[1]), std::abs(a[2] - b[2]) });
    };

    // Find a translucent pixel, keeping its 2x2 block uniform so the scaled mirror shows one color.
    int sourceX = -1, sourceY = -1;
    std::array<float, 4> source{};
    for (int gy = 0; gy < 18 && sourceX < 0; ++gy) {
        for (int gx = 0; gx < 32 && sourceX < 0; ++gx) {
            const int x = gameW * (2 * gx + 1) / 64, y = gameH * (2 * gy + 1) / 36;
            std::array<float, 4> color{}, right{}, below{};
            if (!sampleSource(x, y, color) || color[3] > 0.9f) continue;
            if (!sampleSource(x + 1, y, right) || !sampleSource(x, y + 1, below) || rgbDistance(color, right) > 0.02f ||
                rgbDistance(color, below) > 0.02f) {
                continue;
            }
            sourceX = x;
            sourceY = y;
            source = color;
        }
    }
    if (sourceX < 0) Skip("No translucent pixel is in view (load a world with Improved Transparency on, or water or leaves on screen).");

    struct Probe {
        const char* name;
        int fps;
        int outputX;
    };
    const Probe probes[] = { { kTestMirrorOpaqueRealtime, kMirrorRealtimeFps, 40 }, { kTestMirrorOpaqueLimited, 30, 120 } };
    constexpr int kOutputY = 40;
    constexpr int kScale = 16;
    std::array<float, 4> under[2]{};
    for (size_t i = 0; i < 2; ++i) {
        Require(samplePresented(probes[i].outputX + kScale, kOutputY + kScale, under[i]), "Could not read the presented frame.");
        if (rgbDistance(under[i], source) < 0.1f) Skip("The frame under the mirror output matches the source pixel.");
    }

    const std::string modeId = GetPublishedCurrentModeId();
    RunOnRenderThread([&] {
        for (const Probe& probe : probes) {
            MirrorConfig mirror;
            mirror.name = probe.name;
            mirror.captureWidth = 2;
            mirror.captureHeight = 2;
            mirror.fps = probe.fps;
            mirror.rawOutput = true;
            mirror.border.type = MirrorBorderType::Static;
            mirror.border.staticThickness = 0;
            mirror.input.push_back(MirrorCaptureConfig{ sourceX, sourceY, "topLeftScreen" });
            mirror.output.relativeTo = "topLeftScreen";
            mirror.output.x = probe.outputX;
            mirror.output.y = kOutputY;
            mirror.output.scale = static_cast<float>(kScale);
            AddMirrorToCurrentMode(std::move(mirror));
        }
        PublishGuiConfigSnapshot();
    });
    const auto cleanup = [&] {
        RunOnRenderThread([&] {
            for (ModeConfig& mode : g_config.modes) {
                for (const Probe& probe : probes) RemoveAllModeSources(mode, ModeSourceType::Mirror, probe.name);
            }
            std::erase_if(g_config.mirrors, [&](const MirrorConfig& mirror) {
                return mirror.name == kTestMirrorOpaqueRealtime || mirror.name == kTestMirrorOpaqueLimited;
            });
            g_configIsDirty = true;
            PublishGuiConfigSnapshot();
        });
    };

    std::array<float, 4> shown[2]{};
    bool read[2]{};
    try {
        Require(WaitForFrames(30, std::chrono::seconds(5)), "Frames stopped after adding the mirrors.");
        for (size_t i = 0; i < 2; ++i) read[i] = samplePresented(probes[i].outputX + kScale, kOutputY + kScale, shown[i]);
    } catch (...) {
        cleanup();
        throw;
    }
    cleanup();

    const auto describe = [](const std::array<float, 4>& c) {
        std::ostringstream out;
        out << c[0] << "," << c[1] << "," << c[2] << "," << c[3];
        return out.str();
    };
    Log("[GAME TEST] Opaque mirror check: source (" + std::to_string(sourceX) + "," + std::to_string(sourceY) + ")=" + describe(source) +
        " realtime=" + describe(shown[0]) + " over " + describe(under[0]) + ", limited=" + describe(shown[1]) + " over " +
        describe(under[1]) + " in mode " + modeId);
    for (size_t i = 0; i < 2; ++i) {
        Require(read[i], std::string("Could not read the presented frame under mirror ") + probes[i].name + ".");
        Require(rgbDistance(shown[i], source) < 0.03f,
                std::string("Mirror ") + probes[i].name + " showed " + describe(shown[i]) + " instead of the source pixel " +
                    describe(source) + "; it blended the game's alpha with the frame underneath.");
    }
}

// Static mirror borders and color-keyed overlays draw with Toolscreen's own pipelines, whose layout has a fragment
// push-constant range that ImGui's lacks. Their texture and push constants must go through that layout, so the
// presented frame must show the border in its configured color and the overlay with its keyed color removed.
void TestVulkanToolscreenPipelineDraws() {
    WaitUntil([] { return GetRenderBackend() != RenderBackend::Unknown; }, std::chrono::seconds(20));
    if (GetRenderBackend() != RenderBackend::Vulkan) Skip("This reads back the Vulkan renderer's presented frame.");
    Require(WaitForFrames(60, std::chrono::seconds(20)), "Frames stopped before the test started.");

    const auto samplePresented = [](int x, int y, std::array<float, 4>& color) {
        return WaitUntil([&] { return RunOnRenderThread([&] { return VulkanRenderer::TryGetPresentedPixelForTests(x, y, color); }); },
                         std::chrono::seconds(3));
    };
    const auto rgbDistance = [](const std::array<float, 4>& a, const std::array<float, 4>& b) {
        return (std::max)({ std::abs(a[0] - b[0]), std::abs(a[1] - b[1]), std::abs(a[2] - b[2]) });
    };
    const auto describe = [](const std::array<float, 4>& c) {
        std::ostringstream out;
        out << c[0] << "," << c[1] << "," << c[2] << "," << c[3];
        return out.str();
    };

    // A 2x2 capture scaled to 32x32 at (40, 40), with a magenta static border 6 pixels thick around it.
    constexpr int kMirrorX = 40, kMirrorY = 40, kMirrorSize = 32, kBorderThickness = 6;
    // A 16x16 frame scaled to 64x64 at (120, 40): the left half is green, which the overlay keys out, the right red.
    constexpr int kOverlayX = 120, kOverlayY = 40, kOverlayFrame = 16, kOverlayScale = 4;
    constexpr int kOverlaySize = kOverlayFrame * kOverlayScale;
    const std::array<float, 4> magenta{ 1.0f, 0.0f, 1.0f, 1.0f }, green{ 0.0f, 1.0f, 0.0f, 1.0f }, red{ 1.0f, 0.0f, 0.0f, 1.0f };
    const int borderX = kMirrorX - kBorderThickness / 2, borderY = kMirrorY + kMirrorSize / 2;
    const int keyedX = kOverlayX + kOverlaySize / 4, keptX = kOverlayX + kOverlaySize * 3 / 4, overlayY = kOverlayY + kOverlaySize / 2;

    std::array<float, 4> underKeyed{};
    Require(samplePresented(keyedX, overlayY, underKeyed), "Could not read the presented frame.");
    if (rgbDistance(underKeyed, green) < 0.3f) Skip("The frame under the overlay is too close to the keyed green.");

    WindowOverlayConfig overlay;
    overlay.name = kTestColorKeyOverlay;
    overlay.windowTitle = "Toolscreen game test window that does not exist";
    overlay.x = kOverlayX;
    overlay.y = kOverlayY;
    overlay.scale = static_cast<float>(kOverlayScale);
    overlay.pixelatedScaling = true;
    overlay.enableColorKey = true;
    overlay.colorKeys = { ColorKeyConfig{ Color{ 0.0f, 1.0f, 0.0f, 1.0f }, 0.05f } };
    std::vector<unsigned char> pixels(static_cast<size_t>(kOverlayFrame) * kOverlayFrame * 4);
    for (int y = 0; y < kOverlayFrame; ++y) {
        for (int x = 0; x < kOverlayFrame; ++x) {
            unsigned char* pixel = &pixels[(static_cast<size_t>(y) * kOverlayFrame + x) * 4];
            pixel[0] = x < kOverlayFrame / 2 ? 0 : 255;
            pixel[1] = x < kOverlayFrame / 2 ? 255 : 0;
            pixel[2] = 0;
            pixel[3] = 255;
        }
    }

    const std::string modeId = GetPublishedCurrentModeId();
    RunOnRenderThread([&] {
        MirrorConfig mirror;
        mirror.name = kTestMirrorStaticBorder;
        mirror.captureWidth = 2;
        mirror.captureHeight = 2;
        mirror.rawOutput = true;
        mirror.border.type = MirrorBorderType::Static;
        mirror.border.staticThickness = kBorderThickness;
        mirror.border.staticColor = Color{ 1.0f, 0.0f, 1.0f, 1.0f };
        mirror.input.push_back(MirrorCaptureConfig{ 0, 0, "topLeftScreen" });
        mirror.output.relativeTo = "topLeftScreen";
        mirror.output.x = kMirrorX;
        mirror.output.y = kMirrorY;
        mirror.output.scale = static_cast<float>(kMirrorSize / 2);
        AddMirrorToCurrentMode(std::move(mirror));
        g_config.windowOverlays.push_back(overlay);
        for (ModeConfig& mode : g_config.modes) {
            if (EqualsIgnoreCase(mode.id, modeId)) mode.sources.push_back({ ModeSourceType::WindowOverlay, kTestColorKeyOverlay });
        }
        g_configIsDirty = true;
        PublishGuiConfigSnapshot();
    });
    struct Cleanup {
        ~Cleanup() {
            try {
                RunOnRenderThread([] {
                    for (ModeConfig& mode : g_config.modes) {
                        RemoveAllModeSources(mode, ModeSourceType::Mirror, kTestMirrorStaticBorder);
                        RemoveAllModeSources(mode, ModeSourceType::WindowOverlay, kTestColorKeyOverlay);
                    }
                    std::erase_if(g_config.mirrors, [](const MirrorConfig& m) { return m.name == kTestMirrorStaticBorder; });
                    std::erase_if(g_config.windowOverlays, [](const WindowOverlayConfig& o) { return o.name == kTestColorKeyOverlay; });
                    g_configIsDirty = true;
                    PublishGuiConfigSnapshot();
                });
            } catch (...) {}
            RemoveWindowOverlayFromCache(kTestColorKeyOverlay);
        }
    } cleanup;
    Require(StageWindowOverlayTestFrame(overlay, pixels, kOverlayFrame, kOverlayFrame), "Could not stage a window overlay frame.");
    Require(WaitUntil([] { return RunOnRenderThread([] {
                return VulkanRenderer::GetStreamingTextureStats(std::string("window:") + kTestColorKeyOverlay).slots > 0;
            }); }, std::chrono::seconds(10)),
            "The color-keyed overlay never got a streaming texture.");
    Require(WaitForFrames(30, std::chrono::seconds(5)), "Frames stopped after adding the mirror and overlay.");

    std::array<float, 4> border{}, keyed{}, kept{};
    Require(samplePresented(borderX, borderY, border), "Could not read the presented frame at the mirror border.");
    Require(samplePresented(keyedX, overlayY, keyed), "Could not read the presented frame at the keyed half of the overlay.");
    Require(samplePresented(keptX, overlayY, kept), "Could not read the presented frame at the kept half of the overlay.");
    Log("[GAME TEST] Toolscreen pipeline draws: border=" + describe(border) + " keyed=" + describe(keyed) + " over " +
        describe(underKeyed) + " kept=" + describe(kept) + " in mode " + modeId);
    Require(rgbDistance(border, magenta) < 0.03f, "The static mirror border showed " + describe(border) + " instead of magenta.");
    Require(rgbDistance(keyed, green) > 0.3f, "The overlay's keyed green half was drawn (" + describe(keyed) + ").");
    Require(rgbDistance(kept, red) < 0.03f, "The overlay's red half showed " + describe(kept) + " instead of red.");
}

// Manual check, skipped unless TOOLSCREEN_GAME_TEST_SCREENSHOT_DIR is set: switches to EyeZoom and saves the presented
// frame as <dir>\<TOOLSCREEN_GAME_TEST_SCREENSHOT_TAG>.bmp. Reads the frame from the Vulkan renderer, so other windows
// and focus do not matter. With TOOLSCREEN_GAME_TEST_SCREENSHOT_SLIDE_OUT set it leaves EyeZoom with a slide transition
// and also saves a frame of the slide out as <tag>-slide-out.bmp.
void TestEyeZoomScreenshots() {
    const std::wstring dir = Utf8ToWide(ReadEnvUtf8(L"TOOLSCREEN_GAME_TEST_SCREENSHOT_DIR"));
    if (dir.empty()) Skip("TOOLSCREEN_GAME_TEST_SCREENSHOT_DIR is not set.");
    const std::wstring tag = Utf8ToWide(ReadEnvUtf8(L"TOOLSCREEN_GAME_TEST_SCREENSHOT_TAG"));
    const bool slideOut = !ReadEnvUtf8(L"TOOLSCREEN_GAME_TEST_SCREENSHOT_SLIDE_OUT").empty();
    WaitUntil([] { return GetRenderBackend() != RenderBackend::Unknown; }, std::chrono::seconds(20));
    if (GetRenderBackend() != RenderBackend::Vulkan) Skip("Frame capture reads the Vulkan renderer's presented frame.");
    // Give distant chunks time to load so fog at the render distance is visible.
    Require(WaitForFrames(900, std::chrono::seconds(60)), "Frames stopped before the screenshot.");

    const std::string defaultMode = DefaultModeId();
    RestoreGameTransitions restore;
    // The game frame shows the slide only with animations visible in game.
    struct RestoreHideAnimations {
        bool active = false;
        bool value = false;
        ~RestoreHideAnimations() {
            if (!active) return;
            try {
                RunOnRenderThread([value = value] {
                    g_config.hideAnimationsInGame = value;
                    g_configIsDirty = true;
                    PublishGuiConfigSnapshot();
                });
            } catch (...) {}
        }
    } restoreHideAnimations;
    if (slideOut) {
        restore.values = RunOnRenderThread([defaultMode] {
            return SetGameTransitionsOnRenderThread(
                { { defaultMode, GameTransitionType::Bounce, 1000 }, { "EyeZoom", GameTransitionType::Bounce, 1000 } });
        });
        restoreHideAnimations.value = RunOnRenderThread([] {
            const bool previous = g_config.hideAnimationsInGame;
            g_config.hideAnimationsInGame = false;
            g_configIsDirty = true;
            PublishGuiConfigSnapshot();
            return previous;
        });
        restoreHideAnimations.active = true;
    }

    Require(SwitchModeOnRenderThread("EyeZoom"), "SwitchToMode refused EyeZoom.");
    Require(WaitForPublishedMode("EyeZoom", std::chrono::seconds(5)), "EyeZoom was not published as current.");
    Require(WaitUntil([] { return !IsModeTransitionActive(); }, std::chrono::seconds(5)), "The switch into EyeZoom never finished.");
    Require(WaitForFrames(120, std::chrono::seconds(10)), "Frames stopped while EyeZoom was showing.");

    const auto capture = [](std::vector<uint8_t>& rgba, int& width, int& height) {
        RunOnRenderThread([] { VulkanRenderer::RequestFrameCaptureForTests(); });
        return WaitUntil([&] {
            return RunOnRenderThread([&] { return VulkanRenderer::TryGetFrameCaptureForTests(rgba, width, height); });
        }, std::chrono::seconds(5)) && width > 0 && height > 0;
    };
    const auto save = [&](const std::wstring& name, std::vector<uint8_t>& rgba, int width, int height) {
        BITMAPINFOHEADER info{};
        info.biSize = sizeof(info);
        info.biWidth = width;
        info.biHeight = -height;
        info.biPlanes = 1;
        info.biBitCount = 32;
        info.biCompression = BI_RGB;
        for (size_t i = 0; i < rgba.size(); i += 4) {
            std::swap(rgba[i], rgba[i + 2]);
            rgba[i + 3] = 255;
        }
        BITMAPFILEHEADER file{};
        file.bfType = 0x4D42;
        file.bfOffBits = sizeof(file) + sizeof(info);
        file.bfSize = file.bfOffBits + static_cast<DWORD>(rgba.size());
        std::ofstream out(std::filesystem::path(dir + L"\\" + name + L".bmp"), std::ios::binary);
        out.write(reinterpret_cast<const char*>(&file), sizeof(file));
        out.write(reinterpret_cast<const char*>(&info), sizeof(info));
        out.write(reinterpret_cast<const char*>(rgba.data()), static_cast<std::streamsize>(rgba.size()));
        Require(out.good(), "Could not write the screenshot.");
    };

    std::vector<uint8_t> rgba;
    int width = 0, height = 0;
    const bool captured = capture(rgba, width, height);
    std::vector<uint8_t> slideRgba;
    int slideWidth = 0, slideHeight = 0;
    bool slideCaptured = false;
    if (slideOut && captured) {
        const uint64_t shownBefore =
            RunOnRenderThread([] { return VulkanRenderer::GetEyeZoomSnapshotStatsForTests().framesShown; });
        Require(SlideToModeOnRenderThread(defaultMode), "SwitchToMode refused the default mode.");
        // Capture the frame after the first one drawn from the snapshot, early in the slide while the zoom is on screen.
        const bool requested = WaitUntil([&] {
            return RunOnRenderThread([&] {
                if (VulkanRenderer::GetEyeZoomSnapshotStatsForTests().framesShown == shownBefore) return false;
                VulkanRenderer::RequestFrameCaptureForTests();
                return true;
            });
        }, std::chrono::seconds(2));
        Require(requested, "The slide out of EyeZoom never used the EyeZoom snapshot.");
        slideCaptured = WaitUntil([&] {
            return RunOnRenderThread([&] { return VulkanRenderer::TryGetFrameCaptureForTests(slideRgba, slideWidth, slideHeight); });
        }, std::chrono::seconds(5)) && slideWidth > 0 && slideHeight > 0;
    }
    SwitchModeOnRenderThread(defaultMode);
    Require(captured, "The presented frame was never captured.");
    save(tag, rgba, width, height);
    if (slideOut) {
        Require(slideCaptured, "No frame of the slide out was captured.");
        save(tag + L"-slide-out", slideRgba, slideWidth, slideHeight);
    }
}

// With the OBS compose pass running and the settings GUI open, ImGui must build one frame per swap: the OBS pass
// reuses the screen pass's draw data instead of running NewFrame and the whole settings GUI a second time.
// A color-filtered mirror reports content only when some captured pixel matches a target color. Vulkan measures this
// with an occlusion query over the filter's surviving fragments; if the driver counts discarded fragments too, every
// filtered mirror reports content and its static border always shows.
void TestVulkanMirrorContentDetection() {
    WaitUntil([] { return GetRenderBackend() != RenderBackend::Unknown; }, std::chrono::seconds(20));
    if (GetRenderBackend() != RenderBackend::Vulkan) Skip("This checks the Vulkan renderer's mirror content queries.");
    if (!IsResolutionChangeSupported(g_gameVersion)) Skip("Resolution changes are not supported on this game version.");

    RunOnRenderThread([] {
        InstallTestFixtureOnRenderThread();
        const auto addMirror = [](const char* name, Color target, float sensitivity, int outputX) {
            MirrorConfig mirror{};
            mirror.name = name;
            mirror.captureWidth = 32;
            mirror.captureHeight = 32;
            mirror.input.push_back(MirrorCaptureConfig{ 0, 0, "topLeftScreen" });
            mirror.output.x = outputX;
            mirror.output.y = 10;
            mirror.output.relativeTo = "topLeftScreen";
            mirror.colors.targetColors = { target };
            mirror.colors.output = Color{ 1.0f, 1.0f, 1.0f, 1.0f };
            mirror.colorSensitivity = sensitivity;
            g_config.mirrors.push_back(mirror);
        };
        // Every color is within 2.0 of black, so every pixel matches.
        addMirror(kTestMirrorMatchAll, Color{ 0.0f, 0.0f, 0.0f, 1.0f }, 2.0f, 10);
        // Exact pure magenta does not occur in the game image.
        addMirror(kTestMirrorMatchNone, Color{ 1.0f, 0.0f, 1.0f, 1.0f }, 0.0001f, 60);
        for (ModeConfig& mode : g_config.modes) {
            if (!EqualsIgnoreCase(mode.id, kTestModeId)) continue;
            mode.sources.push_back(ModeSourceRef{ ModeSourceType::Mirror, kTestMirrorMatchAll });
            mode.sources.push_back(ModeSourceRef{ ModeSourceType::Mirror, kTestMirrorMatchNone });
        }
        g_configIsDirty = true;
        PublishGuiConfigSnapshot();
    });

    Require(SwitchModeOnRenderThread(kTestModeId), "SwitchToMode refused the test mode.");
    Require(WaitForPublishedMode(kTestModeId, std::chrono::seconds(5)), "The test mode was not published as current.");

    bool matchAll = false;
    bool matchNone = true;
    const bool measured = WaitUntil([&] {
        return RunOnRenderThread([&] {
            return VulkanRenderer::GetMirrorHasContentForTests(kTestMirrorMatchAll, matchAll) &&
                   VulkanRenderer::GetMirrorHasContentForTests(kTestMirrorMatchNone, matchNone);
        });
    }, std::chrono::seconds(5));
    // Let several frames of query results settle after the mode switch.
    Require(WaitForFrames(30, std::chrono::seconds(5)), "Frames stopped while measuring mirror content.");
    RunOnRenderThread([&] {
        VulkanRenderer::GetMirrorHasContentForTests(kTestMirrorMatchAll, matchAll);
        VulkanRenderer::GetMirrorHasContentForTests(kTestMirrorMatchNone, matchNone);
    });

    RunOnRenderThread([] { RemoveTestFixtureOnRenderThread(); });
    SwitchModeOnRenderThread(DefaultModeId());

    Require(measured, "The Vulkan renderer never read back content results for the test mirrors.");
    Require(matchAll, "A mirror whose filter matches every pixel reported no content.");
    Require(!matchNone, "A mirror whose filter matches no pixel reported content (discarded fragments were counted).");
}

void TestObsPassReusesScreenImGuiFrame() {
    WaitUntil([] { return GetRenderBackend() != RenderBackend::Unknown; }, std::chrono::seconds(20));
    if (GetRenderBackend() != RenderBackend::OpenGL) Skip("The same-thread OBS compose pass is the OpenGL path.");
    if (!IsResolutionChangeSupported(g_gameVersion)) Skip("Modes are not supported on this game version.");

    // Fullscreen shows the startup welcome toast for a few seconds, which draws its own ImGui frame; measure in the
    // test mode so only the screen and OBS passes build ImGui frames.
    RunOnRenderThread([] { InstallTestFixtureOnRenderThread(); });
    Require(SwitchModeOnRenderThread(kTestModeId), "Could not switch to the test mode.");
    s_forceSharedObsFrame.store(true, std::memory_order_relaxed);
    g_showGui.store(true, std::memory_order_release);
    struct Restore {
        ~Restore() {
            g_showGui.store(false, std::memory_order_release);
            s_forceSharedObsFrame.store(false, std::memory_order_relaxed);
            try {
                SwitchModeOnRenderThread(DefaultModeId());
            } catch (...) {}
        }
    } restore;
    Require(WaitForFrames(10, std::chrono::seconds(5)), "Frames stopped after opening the settings GUI.");

    const uint64_t framesBefore = s_renderFrames.load(std::memory_order_acquire);
    const int imguiBefore = RunOnRenderThread([] { return ImGui::GetCurrentContext() ? ImGui::GetFrameCount() : -1; });
    const uint64_t reusedBefore = GetObsPassImGuiFramesReusedForTests();
    Require(imguiBefore >= 0, "No ImGui context exists while the settings GUI is open.");
    Require(WaitForFrames(60, std::chrono::seconds(10)), "Frames stopped while the settings GUI was open.");
    const int imguiAfter = RunOnRenderThread([] { return ImGui::GetFrameCount(); });
    const uint64_t frames = s_renderFrames.load(std::memory_order_acquire) - framesBefore;
    const uint64_t reused = GetObsPassImGuiFramesReusedForTests() - reusedBefore;
    const int imguiFrames = imguiAfter - imguiBefore;

    Log("[GAME TEST] OBS pass ImGui: swaps=" + std::to_string(frames) + " imguiFrames=" + std::to_string(imguiFrames) +
        " obsReusedScreenFrame=" + std::to_string(reused));
    Require(reused > 0, "The OBS pass never reused the screen pass's ImGui frame.");
    Require(static_cast<uint64_t>(imguiFrames) <= frames + 2,
            "ImGui built " + std::to_string(imguiFrames) + " frames over " + std::to_string(frames) + " swaps.");

    // The OBS output must still show the settings GUI: sample its window centre (and the vertically mirrored
    // point, in case the compose texture is bottom-up) with the GUI open, then the same points with it closed.
    struct Sample {
        bool ok = false;
        int x = 0, y = 0, mirroredY = 0;
        unsigned char a[4] = {};
        unsigned char b[4] = {};
    };
    const Sample withGui = RunOnRenderThread([] {
        Sample sample;
        int width = 0, height = 0;
        GetPublishedObsComposeSizeForTests(width, height);
        const std::string title = "Toolscreen v" + GetToolscreenVersionString() + " by jojoe77777";
        ImGuiWindow* window = ImGui::GetCurrentContext() ? ImGui::FindWindowByName(title.c_str()) : nullptr;
        if (!window || width <= 0 || height <= 0) return sample;
        sample.x = static_cast<int>(window->Pos.x + window->Size.x * 0.5f);
        sample.y = static_cast<int>(window->Pos.y + window->Size.y * 0.5f);
        sample.mirroredY = height - 1 - sample.y;
        sample.ok = ReadPublishedObsComposePixelForTests(sample.x, sample.y, sample.a) &&
                    ReadPublishedObsComposePixelForTests(sample.x, sample.mirroredY, sample.b);
        return sample;
    });
    Require(withGui.ok, "Could not sample the OBS compose texture at the settings window.");
    g_showGui.store(false, std::memory_order_release);
    Require(WaitForFrames(10, std::chrono::seconds(5)), "Frames stopped after closing the settings GUI.");
    const Sample withoutGui = RunOnRenderThread([&] {
        Sample sample = withGui;
        sample.ok = ReadPublishedObsComposePixelForTests(sample.x, sample.y, sample.a) &&
                    ReadPublishedObsComposePixelForTests(sample.x, sample.mirroredY, sample.b);
        return sample;
    });
    Require(withoutGui.ok, "Could not sample the OBS compose texture with the settings GUI closed.");
    const bool differs = std::memcmp(withGui.a, withoutGui.a, 3) != 0 || std::memcmp(withGui.b, withoutGui.b, 3) != 0;
    Require(differs, "The OBS output looks the same with the settings GUI open and closed.");
}

// Needs a config.toml that cannot load or be restored, e.g. run_game_tests.ps1 -ConfigFixture tests/game/fixtures/broken_config.toml.
void TestConfigLoadErrorScreen() {
    if (!g_configLoadFailed.load(std::memory_order_acquire)) Skip("config.toml loaded; this test needs a broken config fixture.");

    Require(WaitUntil([] { return g_subclassedHwnd.load(std::memory_order_acquire) != NULL; }, std::chrono::seconds(10)),
            "The game window was never subclassed, so the config error screen cannot receive input.");
    Require(WaitForFrames(10, std::chrono::seconds(20)), "Frames stopped passing through the render hook.");

    const bool errorScreenDrawn = RunOnRenderThread([] {
        std::lock_guard<std::recursive_mutex> lock(GetImGuiContextMutex());
        if (ImGui::GetCurrentContext() == nullptr) return false;
        const ImGuiWindow* window = ImGui::FindWindowByName(trc("error.configuration_error"));
        return window != nullptr && window->WasActive;
    });
    Require(errorScreenDrawn, "The config error screen was not drawn.");
}

// Needs a damaged config.toml next to a good backup, e.g.
// run_game_tests.ps1 -ConfigFixture tests/game/fixtures/corrupt_config_with_backup (whose backup sets fpsLimit = 77).
void TestConfigRecoveredFromBackup() {
    const std::vector<ConfigRecoveryNotice> notices = GetConfigRecoveryNotices();
    const auto configNotice = std::find_if(notices.begin(), notices.end(), [](const ConfigRecoveryNotice& n) { return n.fileName == "config.toml"; });
    if (configNotice == notices.end()) Skip("config.toml was not restored this launch; this test needs a corrupt config fixture.");

    Require(!configNotice->restoredFrom.empty(), "config.toml was moved aside but no backup was restored.");
    Require(!g_configLoadFailed.load(std::memory_order_acquire), "The config error screen showed although a backup was restored.");
    Require(g_configLoaded.load(std::memory_order_acquire), "The restored config was not marked as loaded.");

    const auto snapshot = GetConfigSnapshot();
    Require(snapshot != nullptr, "No config snapshot was published after the restore.");
    Require(snapshot->fpsLimit == 77, "The running config does not hold the backup's values (fpsLimit is " +
                                          std::to_string(snapshot->fpsLimit) + ", expected 77).");

    Config onDisk;
    Require(LoadConfigFromTomlFile(g_toolscreenPath + L"\\config.toml", onDisk) && onDisk.fpsLimit == 77,
            "The backup was not written back to config.toml.");
    Require(std::filesystem::exists(std::filesystem::path(g_toolscreenPath) / Utf8ToWide(configNotice->movedAsideAs)),
            "The damaged config.toml was not kept next to the restored file.");
}

// SDL3's relative mouse mode reads raw input on its own thread, and the game turns the camera by the motion
// events' xrel/yrel. A sensitivity override must scale those deltas, the same as it scales GLFW's.
void TestSensitivityOverrideScalesSdlMotion() {
    HMODULE sdl = GetModuleHandleW(L"SDL3.dll");
    if (!sdl) Skip("The game does not use SDL3.");
    using SdlEventFilter = bool (*)(void* userdata, void* event);
    const auto addEventWatch = reinterpret_cast<bool (*)(SdlEventFilter, void*)>(GetProcAddress(sdl, "SDL_AddEventWatch"));
    const auto removeEventWatch = reinterpret_cast<void (*)(SdlEventFilter, void*)>(GetProcAddress(sdl, "SDL_RemoveEventWatch"));
    const auto setRelativeMouseMode =
        reinterpret_cast<bool (*)(void*, bool)>(GetProcAddress(sdl, "SDL_SetWindowRelativeMouseMode"));
    Require(addEventWatch && removeEventWatch && setRelativeMouseMode, "SDL3 is missing the event-watch or relative-mouse API.");
    Require(WaitUntil([] { return g_lastSdlCursorWindow.load(std::memory_order_acquire) != nullptr; }, std::chrono::seconds(20)),
            "Toolscreen never tracked the SDL window.");
    void* window = g_lastSdlCursorWindow.load(std::memory_order_acquire);

    struct MotionSum {
        std::atomic<int> events{ 0 };
        std::atomic<double> xrel{ 0.0 };
    };
    static MotionSum s_sum;
    // Watches see each event after the event filter, so they read the deltas the game will get.
    const SdlEventFilter watch = [](void*, void* event) -> bool {
        if (*static_cast<const uint32_t*>(event) == 0x400) { // SDL_EVENT_MOUSE_MOTION
            float xrel = 0.0f;
            std::memcpy(&xrel, static_cast<const char*>(event) + 36, sizeof(xrel));
            s_sum.events.fetch_add(1);
            s_sum.xrel.store(s_sum.xrel.load() + xrel);
        }
        return true;
    };
    const auto setOverride = [](bool active, float sensitivity) {
        std::lock_guard<std::mutex> lock(g_tempSensitivityMutex);
        g_tempSensitivityOverride.active = active;
        g_tempSensitivityOverride.sensitivityX = active ? sensitivity : 1.0f;
        g_tempSensitivityOverride.sensitivityY = active ? sensitivity : 1.0f;
        g_tempSensitivityOverride.activeSensHotkeyIndex = -1;
    };
    // Injects relative moves and returns the total xrel the game received for them.
    const auto measure = [&]() {
        s_sum.events.store(0);
        s_sum.xrel.store(0.0);
        for (int i = 0; i < 40; ++i) {
            INPUT input{};
            input.type = INPUT_MOUSE;
            input.mi.dx = 5;
            input.mi.dwFlags = MOUSEEVENTF_MOVE;
            SendInput(1, &input, sizeof(input));
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
        // Let the raw input thread drain, then wait for the total to settle.
        double last = -1.0;
        WaitUntil([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const double now = s_sum.xrel.load();
            const bool settled = now == last;
            last = now;
            return settled;
        }, std::chrono::seconds(3));
        return s_sum.xrel.load();
    };

    if (s_gameWindow) SetForegroundWindow(s_gameWindow);
    Require(RunOnRenderThread([&] { return setRelativeMouseMode(window, true); }), "SDL_SetWindowRelativeMouseMode(true) failed.");
    Require(addEventWatch(watch, nullptr), "SDL_AddEventWatch failed.");
    double baseline = 0.0;
    double scaled = 0.0;
    try {
        setOverride(false, 1.0f);
        // Entering relative mode can report one jump as SDL recenters the cursor; keep it out of the baseline.
        measure();
        baseline = measure();
        setOverride(true, 2.0f);
        scaled = measure();
    } catch (...) {
        setOverride(false, 1.0f);
        removeEventWatch(watch, nullptr);
        RunOnRenderThread([&] { return setRelativeMouseMode(window, false); });
        throw;
    }
    setOverride(false, 1.0f);
    removeEventWatch(watch, nullptr);
    RunOnRenderThread([&] { return setRelativeMouseMode(window, false); });

    Require(baseline > 0.0, "The game received no relative mouse motion; the game window probably lacks focus.");
    const double ratio = scaled / baseline;
    Require(ratio > 1.8 && ratio < 2.2, "A 2x sensitivity override changed SDL motion by " + std::to_string(ratio) +
                                            "x (baseline xrel " + std::to_string(baseline) + ", scaled xrel " +
                                            std::to_string(scaled) + ").");
}

// Minecraft 26.x warps the cursor with SDL_WarpMouseInWindow to the centre of what it believes is its window (the
// mode size) when it grabs or releases the mouse. The cursor must land on the centre of the presented game, not that
// many pixels from the real client's top-left corner.
void TestSdlWarpLandsInPresentedGame() {
    HMODULE sdl = GetModuleHandleW(L"SDL3.dll");
    if (!sdl) Skip("The game does not use SDL3.");
    if (!IsResolutionChangeSupported(g_gameVersion)) Skip("Resolution changes are not supported on this game version.");
    const auto warpMouse = reinterpret_cast<void (*)(void*, float, float)>(GetProcAddress(sdl, "SDL_WarpMouseInWindow"));
    const auto setRelativeMouseMode =
        reinterpret_cast<bool (*)(void*, bool)>(GetProcAddress(sdl, "SDL_SetWindowRelativeMouseMode"));
    const auto getRelativeMouseMode = reinterpret_cast<bool (*)(void*)>(GetProcAddress(sdl, "SDL_GetWindowRelativeMouseMode"));
    Require(warpMouse && setRelativeMouseMode && getRelativeMouseMode, "SDL3 is missing the warp or relative-mouse API.");
    Require(WaitUntil([] { return g_lastSdlCursorWindow.load(std::memory_order_acquire) != nullptr; }, std::chrono::seconds(20)),
            "Toolscreen never tracked the SDL window.");
    void* window = g_lastSdlCursorWindow.load(std::memory_order_acquire);
    Require(WaitUntil([] { return g_subclassedHwnd.load(std::memory_order_acquire) != NULL; }, std::chrono::seconds(20)),
            "Toolscreen never subclassed the game window.");
    const HWND hwnd = g_subclassedHwnd.load(std::memory_order_acquire);
    if (s_gameWindow) SetForegroundWindow(s_gameWindow);
    if (!WaitUntil([&] { return IsWindowInForegroundTree(hwnd); }, std::chrono::seconds(2))) {
        Skip("The game window is not in the foreground, so cursor warps are left alone.");
    }

    RunOnRenderThread([] { InstallTestFixtureOnRenderThread(); });
    Require(SwitchModeOnRenderThread(kTestModeId), "SwitchToMode refused the test mode.");
    Require(WaitForPublishedMode(kTestModeId, std::chrono::seconds(5)), "The test mode was not published as current.");
    Require(WaitForFrames(10, std::chrono::seconds(5)), "Frames stopped after switching modes.");

    struct WarpResult {
        POINT cursor{};
        RECT client{};
    };
    const bool wasRelative = RunOnRenderThread([&] { return getRelativeMouseMode(window); });
    // SDL and the cursor queries run on the game's thread so they share its DPI awareness.
    const auto warpTo = [&](float x, float y) {
        return RunOnRenderThread([&] {
            WarpResult result;
            setRelativeMouseMode(window, false);
            warpMouse(window, x, y);
            GetCursorPos(&result.cursor);
            ScreenToClient(hwnd, &result.cursor);
            GetClientRect(hwnd, &result.client);
            return result;
        });
    };
    WarpResult result;
    try {
        result = warpTo(kTestModeWidth / 2.0f, kTestModeHeight / 2.0f);
    } catch (...) {
        RunOnRenderThread([&] { return setRelativeMouseMode(window, wasRelative); });
        SwitchModeOnRenderThread(DefaultModeId());
        throw;
    }
    RunOnRenderThread([&] { return setRelativeMouseMode(window, wasRelative); });
    Require(SwitchModeOnRenderThread(DefaultModeId()), "SwitchToMode refused the default mode.");

    // The test mode is unstretched, so the game is centred in the client area.
    const int expectedX = GetCenteredAxisOffset(result.client.right, kTestModeWidth) + kTestModeWidth / 2;
    const int expectedY = GetCenteredAxisOffset(result.client.bottom, kTestModeHeight) + kTestModeHeight / 2;
    Require(std::abs(result.cursor.x - expectedX) <= 2 && std::abs(result.cursor.y - expectedY) <= 2,
            "Warping to the game's centre put the cursor at client " + std::to_string(result.cursor.x) + "," +
                std::to_string(result.cursor.y) + " instead of " + std::to_string(expectedX) + "," + std::to_string(expectedY) +
                " (client " + std::to_string(result.client.right) + "x" + std::to_string(result.client.bottom) + ").");
}

// Watches for a freeze from another thread while a test runs: the longest gap between hooked frames, and the longest
// time the game window took to answer a message (its thread pumps SDL/GLFW events, so this is input latency).
class FreezeWatchdog {
public:
    FreezeWatchdog(HWND hwnd) : m_hwnd(hwnd), m_thread([this] { Run(); }) {}
    ~FreezeWatchdog() { Stop(); }
    void Stop() {
        m_stop.store(true);
        if (m_thread.joinable()) m_thread.join();
    }
    long long MaxFrameGapMs() const { return m_maxFrameGapMs.load(); }
    long long MaxMessageLatencyMs() const { return m_maxMessageLatencyMs.load(); }
    int MessageTimeouts() const { return m_messageTimeouts.load(); }

private:
    void Run() {
        using Clock = std::chrono::steady_clock;
        uint64_t lastFrames = s_renderFrames.load(std::memory_order_acquire);
        auto lastFrameChange = Clock::now();
        auto nextPing = Clock::now();
        while (!m_stop.load()) {
            const auto now = Clock::now();
            const uint64_t frames = s_renderFrames.load(std::memory_order_acquire);
            if (frames != lastFrames) {
                lastFrames = frames;
                lastFrameChange = now;
            }
            const long long gap = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastFrameChange).count();
            if (gap > m_maxFrameGapMs.load()) m_maxFrameGapMs.store(gap);

            if (now >= nextPing) {
                DWORD_PTR result = 0;
                const auto sent = Clock::now();
                const LRESULT ok = SendMessageTimeoutW(m_hwnd, WM_NULL, 0, 0, SMTO_NORMAL, 2000, &result);
                const long long latency = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - sent).count();
                if (!ok) m_messageTimeouts.fetch_add(1);
                if (latency > m_maxMessageLatencyMs.load()) m_maxMessageLatencyMs.store(latency);
                nextPing = Clock::now() + std::chrono::milliseconds(50);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    HWND m_hwnd;
    std::atomic<bool> m_stop{ false };
    std::atomic<long long> m_maxFrameGapMs{ 0 };
    std::atomic<long long> m_maxMessageLatencyMs{ 0 };
    std::atomic<int> m_messageTimeouts{ 0 };
    std::thread m_thread;
};

// Counts the SDL key events the game receives, per SDL scancode.
struct SdlKeyCounter {
    static constexpr int kScancodes = 512;
    std::atomic<int> downs[kScancodes]{};
    std::atomic<int> ups[kScancodes]{};
    std::atomic<int> anyKeyEvents{ 0 };
    std::atomic<uint32_t> lastScancode{ 0 };
};
SdlKeyCounter s_sdlKeys;

bool SdlKeyWatch(void*, void* event) {
    const uint32_t type = *static_cast<const uint32_t*>(event);
    if (type == 0x300 || type == 0x301) { // SDL_EVENT_KEY_DOWN / SDL_EVENT_KEY_UP
        uint32_t scancode = 0;
        bool repeat = false;
        std::memcpy(&scancode, static_cast<const char*>(event) + 24, sizeof(scancode));
        std::memcpy(&repeat, static_cast<const char*>(event) + 37, sizeof(repeat));
        s_sdlKeys.anyKeyEvents.fetch_add(1);
        s_sdlKeys.lastScancode.store(scancode);
        if (scancode < SdlKeyCounter::kScancodes && !repeat) {
            (type == 0x300 ? s_sdlKeys.downs : s_sdlKeys.ups)[scancode].fetch_add(1);
        }
    }
    return true;
}

struct RealKeyStats {
    int sendFailures = 0;
    int pressedWithoutFocus = 0;
    std::string lastForeground;
};
RealKeyStats s_realKeyStats;

std::string DescribeForegroundWindow() {
    const HWND fg = GetForegroundWindow();
    if (!fg) return "none";
    wchar_t title[128] = {};
    GetWindowTextW(fg, title, static_cast<int>(std::size(title)));
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return "pid " + std::to_string(pid) + " '" + WideToUtf8(title) + "'";
}

// Presses vk for real and returns how long Windows took to report it down, or -1 if it never did.
long long PressRealKey(DWORD vk, std::chrono::milliseconds hold) {
    const HWND hwnd = g_subclassedHwnd.load(std::memory_order_acquire);
    if (!IsWindowInForegroundTree(hwnd)) {
        // SendInput goes to whatever window has focus, so never type into another program.
        ++s_realKeyStats.pressedWithoutFocus;
        s_realKeyStats.lastForeground = DescribeForegroundWindow();
        Skip("The game window lost focus (foreground: " + s_realKeyStats.lastForeground + "), so real key presses stopped.");
    }
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = static_cast<WORD>(vk);
    input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    const auto start = std::chrono::steady_clock::now();
    if (SendInput(1, &input, sizeof(input)) != 1) ++s_realKeyStats.sendFailures;
    long long downLatency = -1;
    if (WaitUntil([vk] { return (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0; }, std::chrono::seconds(3))) {
        downLatency = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    }
    std::this_thread::sleep_for(hold);
    input.ki.dwFlags = KEYEVENTF_KEYUP;
    if (SendInput(1, &input, sizeof(input)) != 1) ++s_realKeyStats.sendFailures;
    WaitUntil([vk] { return (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) == 0; }, std::chrono::seconds(3));
    return downLatency;
}

// Reported freeze: after EyeZoom toggles the game stopped responding without crashing. Drive EyeZoom with real presses
// of its configured hotkey and check, throughout and afterwards, that frames keep coming, the window keeps answering
// messages, keys reach Windows promptly, and the game itself still receives every key down and up.
void TestResponsiveThroughEyeZoomToggles() {
    if (!IsResolutionChangeSupported(g_gameVersion)) Skip("Resolution changes are not supported on this game version.");
    const auto snapshot = GetConfigSnapshot();
    DWORD eyeZoomVk = 0;
    std::string mainMode;
    if (snapshot) {
        for (const HotkeyConfig& hotkey : snapshot->hotkeys) {
            if (hotkey.keys.size() == 1 && EqualsIgnoreCase(hotkey.secondaryMode, "EyeZoom")) {
                eyeZoomVk = hotkey.keys[0];
                mainMode = hotkey.mainMode;
                break;
            }
        }
    }
    if (eyeZoomVk == 0) Skip("The config has no single-key EyeZoom hotkey.");

    const HWND hwnd = g_subclassedHwnd.load(std::memory_order_acquire);
    Require(hwnd != NULL, "No subclassed game window to send keys to.");
    if (s_gameWindow) SetForegroundWindow(s_gameWindow);
    if (!WaitUntil([&] { return IsWindowInForegroundTree(hwnd); }, std::chrono::seconds(2))) {
        Skip("The game window is not in the foreground, so injected keys would go elsewhere.");
    }
    Require(SwitchModeOnRenderThread(mainMode), "Could not start from the EyeZoom hotkey's main mode.");
    Require(WaitForPublishedMode(mainMode, std::chrono::seconds(5)), "The main mode was not published.");
    Require(WaitForFrames(30, std::chrono::seconds(5)), "Frames stopped before the test started.");

    HMODULE sdl = GetModuleHandleW(L"SDL3.dll");
    using SdlEventFilter = bool (*)(void*, void*);
    const auto addEventWatch = sdl ? reinterpret_cast<bool (*)(SdlEventFilter, void*)>(GetProcAddress(sdl, "SDL_AddEventWatch")) : nullptr;
    const auto removeEventWatch = sdl ? reinterpret_cast<void (*)(SdlEventFilter, void*)>(GetProcAddress(sdl, "SDL_RemoveEventWatch")) : nullptr;
    const bool watchingSdl = addEventWatch && removeEventWatch && addEventWatch(&SdlKeyWatch, nullptr);
    for (int i = 0; i < SdlKeyCounter::kScancodes; ++i) {
        s_sdlKeys.downs[i].store(0);
        s_sdlKeys.ups[i].store(0);
    }
    struct RemoveWatch {
        bool active;
        void (*remove)(SdlEventFilter, void*);
        ~RemoveWatch() { if (active) remove(&SdlKeyWatch, nullptr); }
    } removeWatch{ watchingSdl, removeEventWatch };

    s_realKeyStats = RealKeyStats{};
    FreezeWatchdog watchdog(hwnd);
    long long maxKeyLatencyMs = 0;
    int keysNeverDown = 0;
    const auto press = [&](DWORD vk, std::chrono::milliseconds hold) {
        const long long latency = PressRealKey(vk, hold);
        if (latency < 0) ++keysNeverDown;
        else maxKeyLatencyMs = std::max(maxKeyLatencyMs, latency);
    };
    // A movement key the game must still see once EyeZoom has been toggled. SDL_SCANCODE_W is 26.
    constexpr DWORD kProbeVk = 'W';
    constexpr int kProbeScancode = 26;
    int probesSent = 0;
    const auto probeGameInput = [&](const std::string& when) {
        if (!watchingSdl) return;
        ++probesSent;
        const long long latency = PressRealKey(kProbeVk, std::chrono::milliseconds(60));
        if (latency < 0) ++keysNeverDown;
        else maxKeyLatencyMs = std::max(maxKeyLatencyMs, latency);
        const bool received = WaitUntil([&] { return s_sdlKeys.downs[kProbeScancode].load() >= probesSent &&
                                                     s_sdlKeys.ups[kProbeScancode].load() >= probesSent; },
                                        std::chrono::seconds(2));
        Require(received, "The game stopped receiving keys " + when + " (W down " + std::to_string(s_sdlKeys.downs[kProbeScancode].load()) +
                              ", up " + std::to_string(s_sdlKeys.ups[kProbeScancode].load()) + " of " + std::to_string(probesSent) + "; key-to-Windows latency " + std::to_string(latency) +
                              " ms, SDL key events seen " + std::to_string(s_sdlKeys.anyKeyEvents.load()) + ", last SDL scancode " +
                              std::to_string(s_sdlKeys.lastScancode.load()) + ", foreground " +
                              (IsWindowInForegroundTree(hwnd) ? "yes" : "no") + ").");
    };

    probeGameInput("before any EyeZoom toggle");

    // Paced toggles: each press must switch modes, and the game must still see other keys in both modes.
    std::string expected = mainMode;
    for (int i = 0; i < 12; ++i) {
        expected = EqualsIgnoreCase(expected, "EyeZoom") ? mainMode : std::string("EyeZoom");
        press(eyeZoomVk, std::chrono::milliseconds(40));
        Require(WaitForPublishedMode(expected, std::chrono::seconds(3)),
                "Press " + std::to_string(i + 1) + " of the EyeZoom hotkey did not switch to " + expected + " (current: " +
                    GetPublishedCurrentModeId() + ", foreground " + DescribeForegroundWindow() + ", presses without focus " +
                    std::to_string(s_realKeyStats.pressedWithoutFocus) + ", SendInput failures " +
                    std::to_string(s_realKeyStats.sendFailures) + ").");
        Require(WaitForFrames(10, std::chrono::seconds(3)), "Frames stopped after EyeZoom hotkey press " + std::to_string(i + 1) + ".");
        probeGameInput("in " + expected + " after " + std::to_string(i + 1) + " EyeZoom toggles");
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }

    // Burst: presses just past the hotkey debounce, landing while the previous resize is still settling.
    for (int i = 0; i < 16; ++i) {
        press(eyeZoomVk, std::chrono::milliseconds(20));
        std::this_thread::sleep_for(std::chrono::milliseconds(110));
    }
    Require(WaitUntil([] { return !IsModeTransitionActive(); }, std::chrono::seconds(5)), "A mode transition never finished after the burst.");
    Require(WaitForFrames(30, std::chrono::seconds(5)), "Frames stopped after a burst of EyeZoom hotkey presses.");
    probeGameInput("after a burst of EyeZoom toggles");

    // The hotkey must still work after the burst.
    const std::string afterBurst = GetPublishedCurrentModeId();
    const std::string next = EqualsIgnoreCase(afterBurst, "EyeZoom") ? mainMode : std::string("EyeZoom");
    press(eyeZoomVk, std::chrono::milliseconds(40));
    Require(WaitForPublishedMode(next, std::chrono::seconds(3)),
            "The EyeZoom hotkey stopped working after the burst (still " + GetPublishedCurrentModeId() + ").");
    if (!EqualsIgnoreCase(next, mainMode)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        press(eyeZoomVk, std::chrono::milliseconds(40));
        Require(WaitForPublishedMode(mainMode, std::chrono::seconds(3)), "The EyeZoom hotkey did not return to " + mainMode + ".");
    }
    Require(WaitForFrames(60, std::chrono::seconds(5)), "Frames stopped after leaving EyeZoom.");
    probeGameInput("after leaving EyeZoom");
    watchdog.Stop();

    const bool keyStuck = (GetAsyncKeyState(static_cast<int>(eyeZoomVk)) & 0x8000) != 0 || (GetAsyncKeyState(kProbeVk) & 0x8000) != 0;
    const int hotkeyScancode = static_cast<int>(MapVirtualKeyW(eyeZoomVk, MAPVK_VK_TO_VSC));
    std::ostringstream stats;
    stats << "max frame gap " << watchdog.MaxFrameGapMs() << " ms, max window message latency " << watchdog.MaxMessageLatencyMs()
          << " ms, message timeouts " << watchdog.MessageTimeouts() << ", max key-to-Windows latency " << maxKeyLatencyMs
          << " ms, keys never down " << keysNeverDown << ", SendInput failures " << s_realKeyStats.sendFailures
          << ", presses without focus " << s_realKeyStats.pressedWithoutFocus << " (last foreground " << s_realKeyStats.lastForeground
          << "), hotkey PS/2 scancode " << hotkeyScancode;
    Log("[GAME TEST] Responsiveness through EyeZoom toggles: " + stats.str());

    Require(keysNeverDown == 0, "Some injected keys never reached Windows: " + stats.str());
    Require(maxKeyLatencyMs < 300, "Keys took too long to reach Windows (low-level hook stall): " + stats.str());
    Require(watchdog.MessageTimeouts() == 0 && watchdog.MaxMessageLatencyMs() < 1000,
            "The game window stopped answering messages: " + stats.str());
    Require(watchdog.MaxFrameGapMs() < 1000, "Rendering stalled: " + stats.str());
    Require(!keyStuck, "A key stayed down after the test released it: " + stats.str());
}

// Manual stress check, skipped unless TOOLSCREEN_GAME_TEST_STRESS_SECONDS is set. In a loaded world it presses the
// EyeZoom hotkey over and over while the camera sweeps around and down, the way the 26.3 EyeZoom freeze was reported.
// Run it with a high render distance and -TestTimeoutSeconds above the stress time; it fails with thread stacks if
// frames stop. Options:
// - TOOLSCREEN_GAME_TEST_STRESS_NO_MDI: presses F3+M so 26.x draws terrain without multi-draw-indirect, through the
//   "Chunk Sections UBO" ring buffer from the report. 26.x only handles F3+M when the JVM has
//   -DMC_DEBUG_ENABLED=true -DMC_DEBUG_HOTKEYS=true, e.g. through JAVA_TOOL_OPTIONS.
// - TOOLSCREEN_GAME_TEST_STRESS_FORCE_OBS (OpenGL only): runs the per-frame OBS capture and compose as if OBS game
//   capture were hooked.
// - TOOLSCREEN_GAME_TEST_STRESS_FRAME_DELAY_MS: sleeps the render thread this long every frame (a slow CPU).
// - TOOLSCREEN_GAME_TEST_STRESS_GL_GPU_BLITS (OpenGL only): adds this many 4096x4096 blits to every frame (a slow GPU).
// - TOOLSCREEN_GAME_TEST_STRESS_GL_VRAM_MB (OpenGL only): holds this much video memory, rounded up to 256 MiB, until
//   the test ends (a GPU short on memory).
void TestEyeZoomStressLookAround() {
    const std::string secondsText = ReadEnvUtf8(L"TOOLSCREEN_GAME_TEST_STRESS_SECONDS");
    if (secondsText.empty()) Skip("TOOLSCREEN_GAME_TEST_STRESS_SECONDS is not set.");
    const int seconds = (std::max)(10, std::atoi(secondsText.c_str()));
    if (!IsResolutionChangeSupported(g_gameVersion)) Skip("Resolution changes are not supported on this game version.");
    const bool disableMdi = !ReadEnvUtf8(L"TOOLSCREEN_GAME_TEST_STRESS_NO_MDI").empty();
    if (disableMdi) {
        const std::string jvmOptions = WideToUtf8(GetCommandLineW()) + " " + ReadEnvUtf8(L"JAVA_TOOL_OPTIONS");
        if (jvmOptions.find("-DMC_DEBUG_ENABLED=true") == std::string::npos ||
            jvmOptions.find("-DMC_DEBUG_HOTKEYS=true") == std::string::npos) {
            Skip("TOOLSCREEN_GAME_TEST_STRESS_NO_MDI needs -DMC_DEBUG_ENABLED=true -DMC_DEBUG_HOTKEYS=true in JAVA_TOOL_OPTIONS.");
        }
    }
    const bool forceObs = !ReadEnvUtf8(L"TOOLSCREEN_GAME_TEST_STRESS_FORCE_OBS").empty();
    WaitUntil([] { return GetRenderBackend() != RenderBackend::Unknown; }, std::chrono::seconds(20));
    if (forceObs && GetRenderBackend() != RenderBackend::OpenGL) {
        Skip("TOOLSCREEN_GAME_TEST_STRESS_FORCE_OBS only drives the OpenGL capture path.");
    }
    const int frameDelayMs = std::atoi(ReadEnvUtf8(L"TOOLSCREEN_GAME_TEST_STRESS_FRAME_DELAY_MS").c_str());
    const int gpuBlits = std::atoi(ReadEnvUtf8(L"TOOLSCREEN_GAME_TEST_STRESS_GL_GPU_BLITS").c_str());
    const int vramMb = std::atoi(ReadEnvUtf8(L"TOOLSCREEN_GAME_TEST_STRESS_GL_VRAM_MB").c_str());
    if ((gpuBlits > 0 || vramMb > 0) && GetRenderBackend() != RenderBackend::OpenGL) {
        Skip("The GPU load options only apply to OpenGL.");
    }
    const auto snapshot = GetConfigSnapshot();
    DWORD eyeZoomVk = 0;
    std::string mainMode;
    if (snapshot) {
        for (const HotkeyConfig& hotkey : snapshot->hotkeys) {
            if (hotkey.keys.size() == 1 && EqualsIgnoreCase(hotkey.secondaryMode, "EyeZoom")) {
                eyeZoomVk = hotkey.keys[0];
                mainMode = hotkey.mainMode;
                break;
            }
        }
    }
    if (eyeZoomVk == 0) Skip("The config has no single-key EyeZoom hotkey.");

    const HWND hwnd = g_subclassedHwnd.load(std::memory_order_acquire);
    Require(hwnd != NULL, "No subclassed game window to send input to.");
    // Let the world load and far chunks build before stressing it.
    Require(WaitForFrames(600, std::chrono::seconds(120)), "Frames stopped before the stress test started.");
    if (s_gameWindow) SetForegroundWindow(s_gameWindow);
    if (!WaitUntil([&] { return IsWindowInForegroundTree(hwnd); }, std::chrono::seconds(2))) {
        Skip("The game window is not in the foreground, so injected input would go elsewhere.");
    }
    if (!g_nativeCursorGrabbed.load(std::memory_order_acquire)) {
        // A pause screen is open (focus was elsewhere when the world loaded); Escape closes it and grabs the mouse.
        PressRealKey(VK_ESCAPE, std::chrono::milliseconds(40));
        WaitUntil([] { return g_nativeCursorGrabbed.load(std::memory_order_acquire); }, std::chrono::seconds(3));
    }
    Require(g_nativeCursorGrabbed.load(std::memory_order_acquire), "The game never grabbed the mouse, so the camera cannot be turned.");
    if (disableMdi) {
        if (!IsWindowInForegroundTree(hwnd)) Skip("The game window lost focus before F3+M.");
        struct HeldF3 {
            INPUT input{};
            HeldF3() {
                input.type = INPUT_KEYBOARD;
                input.ki.wVk = VK_F3;
                input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(VK_F3, MAPVK_VK_TO_VSC));
                SendInput(1, &input, sizeof(input));
            }
            // PressRealKey skips (throws) when focus is lost, so release F3 on every path.
            ~HeldF3() {
                input.ki.dwFlags = KEYEVENTF_KEYUP;
                SendInput(1, &input, sizeof(input));
            }
        };
        {
            HeldF3 f3;
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            PressRealKey('M', std::chrono::milliseconds(60));
        }
        Require(WaitForFrames(30, std::chrono::seconds(10)), "Frames stopped after F3+M.");
    }
    Require(SwitchModeOnRenderThread(mainMode), "Could not start from the EyeZoom hotkey's main mode.");
    Require(WaitForPublishedMode(mainMode, std::chrono::seconds(5)), "The main mode was not published.");

    const auto moveMouse = [](int dx, int dy) {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dx = dx;
        input.mi.dy = dy;
        input.mi.dwFlags = MOUSEEVENTF_MOVE;
        SendInput(1, &input, sizeof(input));
    };
    // Look down first: the report says the freeze is easiest to hit while looking downward.
    for (int i = 0; i < 20; ++i) {
        moveMouse(0, 40);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (forceObs) s_forceSharedObsFrame.store(true, std::memory_order_relaxed);
    struct ClearForcedObs {
        bool active;
        ~ClearForcedObs() { if (active) s_forceSharedObsFrame.store(false, std::memory_order_relaxed); }
    } clearForcedObs{ forceObs };
    struct StopLag {
        ~StopLag() {
            s_renderFrameDelayMs.store(0, std::memory_order_relaxed);
            s_glGpuLoadBlits.store(0, std::memory_order_relaxed);
            s_glVramHogMb.store(0, std::memory_order_relaxed);
        }
    } stopLag;
    s_glVramHogMb.store((std::max)(0, vramMb), std::memory_order_relaxed);
    s_glGpuLoadBlits.store((std::max)(0, gpuBlits), std::memory_order_relaxed);
    s_renderFrameDelayMs.store((std::max)(0, frameDelayMs), std::memory_order_relaxed);

    FreezeWatchdog watchdog(hwnd);
    s_realKeyStats = RealKeyStats{};
    int presses = 0;
    int modeChanges = 0;
    std::string lastMode = GetPublishedCurrentModeId();
    const uint64_t framesAtStart = s_renderFrames.load(std::memory_order_acquire);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto pressStart = std::chrono::steady_clock::now();
        const uint64_t framesBefore = s_renderFrames.load(std::memory_order_acquire);
        PressRealKey(eyeZoomVk, std::chrono::milliseconds(20 + (presses % 3) * 15));
        ++presses;
        // Sweep the camera between presses: a varying pitch and yaw keeps changing the visible chunk sections.
        const int steps = 2 + presses % 6;
        for (int step = 0; step < steps; ++step) {
            const int dx = ((presses / 4) % 2 == 0 ? 1 : -1) * (30 + (presses * 17) % 50);
            const int dy = ((presses / 7) % 2 == 0 ? 1 : -1) * ((presses * 13) % 25);
            moveMouse(dx, dy);
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
        if (!WaitUntil([&] { return s_renderFrames.load(std::memory_order_acquire) > framesBefore; }, std::chrono::seconds(10))) {
            ReportRenderThreadStall("no frame for 10 s while spamming EyeZoom (press " + std::to_string(presses) + ")");
            Require(false, "Rendering stopped while spamming EyeZoom after " + std::to_string(presses) + " presses.");
        }
        const std::string mode = GetPublishedCurrentModeId();
        if (!EqualsIgnoreCase(mode, lastMode)) ++modeChanges;
        lastMode = mode;
        // Space presses just past the hotkey's 100 ms debounce, by a varying amount so they land at different points
        // of the previous resize.
        const auto interval = std::chrono::milliseconds(105 + (presses % 4) * 20);
        const auto elapsed = std::chrono::steady_clock::now() - pressStart;
        if (elapsed < interval) std::this_thread::sleep_for(interval - elapsed);
    }

    // Leave EyeZoom and make sure the game still renders.
    const double averageFps = static_cast<double>(s_renderFrames.load(std::memory_order_acquire) - framesAtStart) / seconds;
    const bool left = SwitchModeOnRenderThread(mainMode) && WaitForPublishedMode(mainMode, std::chrono::seconds(5));
    const bool settled = WaitForFrames(60, std::chrono::seconds(10));
    watchdog.Stop();
    const int heldVramMb = vramMb > 0 ? (vramMb + kGlVramHogTextureMb - 1) / kGlVramHogTextureMb * kGlVramHogTextureMb : 0;
    std::ostringstream stats;
    stats << presses << " presses, " << modeChanges << " mode changes, " << averageFps << " fps, max frame gap " << watchdog.MaxFrameGapMs()
          << " ms, max window message latency " << watchdog.MaxMessageLatencyMs() << " ms, message timeouts "
          << watchdog.MessageTimeouts() << ", presses without focus " << s_realKeyStats.pressedWithoutFocus << ", frame delay "
          << frameDelayMs << " ms, GL blits " << gpuBlits << ", GL VRAM held " << heldVramMb << " MiB";
    const VulkanRenderer::FrameTrackingStats tracking = VulkanRenderer::GetFrameTrackingStats();
    stats << ", vulkan frames completed " << tracking.completedFrames << ", untracked " << tracking.untrackedFrames;
    Log("[GAME TEST] EyeZoom stress: " + stats.str());
    if (!settled) ReportRenderThreadStall("no frames after the EyeZoom stress test");
    Require(settled, "Frames stopped after the EyeZoom stress test: " + stats.str());
    Require(left, "Could not leave EyeZoom after the stress test: " + stats.str());
    // A lagging game can handle two presses between polls, so only require that the hotkey switched modes at all.
    Require(modeChanges > 0, "The EyeZoom hotkey never switched modes: " + stats.str());
    Require(watchdog.MaxFrameGapMs() < 5000, "Rendering stalled during the EyeZoom stress test: " + stats.str());
}

// A lagging game must not make Toolscreen republish its config every frame. The window keeps its real size in EyeZoom,
// so a screen-metrics recalc that compares the window with the mode size must not keep re-requesting it.
void TestEyeZoomConfigStableWhenLagging() {
    if (!IsResolutionChangeSupported(g_gameVersion)) Skip("Resolution changes are not supported on this game version.");
    const auto snapshot = GetConfigSnapshot();
    const bool hasEyeZoom = snapshot && std::any_of(snapshot->modes.begin(), snapshot->modes.end(),
                                                    [](const ModeConfig& mode) { return EqualsIgnoreCase(mode.id, "EyeZoom"); });
    if (!hasEyeZoom) Skip("The config has no EyeZoom mode.");
    const std::string defaultMode = DefaultModeId();
    Require(SwitchModeOnRenderThread(defaultMode), "Could not start from the default mode.");
    Require(WaitForPublishedMode(defaultMode, std::chrono::seconds(5)), "The default mode was not published.");

    // Lag first, so the resize that entering EyeZoom posts is handled at the lagging frame rate.
    constexpr int kDelayMs = 80;
    constexpr uint64_t kFrames = 30;
    struct StopLag {
        ~StopLag() { s_renderFrameDelayMs.store(0, std::memory_order_relaxed); }
    } stopLag;
    s_renderFrameDelayMs.store(kDelayMs, std::memory_order_relaxed);
    Require(WaitForFrames(3, std::chrono::seconds(5)), "Frames stopped once the render thread lagged.");
    Require(SwitchModeOnRenderThread("EyeZoom"), "SwitchToMode refused EyeZoom.");
    Require(WaitForPublishedMode("EyeZoom", std::chrono::seconds(5)), "EyeZoom was not published as current.");
    // Let the switch's own recalc and republish land before counting.
    Require(WaitForFrames(5, std::chrono::seconds(5)), "Frames stopped after entering EyeZoom.");
    const uint64_t versionBefore = g_configSnapshotVersion.load(std::memory_order_acquire);
    const auto start = std::chrono::steady_clock::now();
    Require(WaitForFrames(kFrames, std::chrono::seconds(15)), "Frames stopped while the render thread lagged in EyeZoom.");
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    const uint64_t republished = g_configSnapshotVersion.load(std::memory_order_acquire) - versionBefore;
    s_renderFrameDelayMs.store(0, std::memory_order_relaxed);

    Require(SwitchModeOnRenderThread(defaultMode), "SwitchToMode refused the default mode.");
    Require(WaitForPublishedMode(defaultMode, std::chrono::seconds(5)), "Leaving EyeZoom did not publish the default mode.");
    Log("[GAME TEST] Config republished " + std::to_string(republished) + " times in " + std::to_string(kFrames) +
        " lagging EyeZoom frames (" + std::to_string(elapsedMs) + " ms).");
    Require(elapsedMs >= static_cast<long long>(kFrames) * kDelayMs * 9 / 10,
            "The render thread did not lag: " + std::to_string(kFrames) + " frames took " + std::to_string(elapsedMs) + " ms.");
    Require(republished <= 2, "The config was republished " + std::to_string(republished) + " times in " + std::to_string(kFrames) +
                                  " lagging EyeZoom frames with nothing changing.");
}

struct TestCase {
    const char* name;
    void (*fn)();
};

const TestCase kTests[] = {
    { "startup.version_detected", &TestVersionDetected },
    { "startup.window_hooks_ready", &TestWindowHooksReady },
    { "startup.config_isolated", &TestConfigIsolated },
    { "render.backend_latched", &TestRenderBackendLatched },
    { "render.frames_hooked", &TestFramesHooked },
    { "mode.switch_resizes_game", &TestModeSwitchResizesGame },
    { "mode.eyezoom_renders_and_switches", &TestEyeZoomRendersAndSwitches },
    { "mode.eyezoom_slide_transitions", &TestEyeZoomSlideTransitions },
    { "mode.eyezoom_config_stable_when_lagging", &TestEyeZoomConfigStableWhenLagging },
    { "input.hotkey_switches_mode", &TestHotkeySwitchesMode },
    { "input.low_level_hook_dedicated_thread", &TestLowLevelHookOnDedicatedThread },
    { "input.real_key_passes_through_once", &TestRealKeyPassesThroughOnce },
    { "input.sensitivity_override_scales_sdl_motion", &TestSensitivityOverrideScalesSdlMotion },
    { "input.sdl_warp_lands_in_presented_game", &TestSdlWarpLandsInPresentedGame },
    { "input.responsive_through_eyezoom_toggles", &TestResponsiveThroughEyeZoomToggles },
    { "gui.toggle_renders_imgui", &TestGuiToggleRendersImGui },
    { "gui.obs_pass_reuses_screen_frame", &TestObsPassReusesScreenImGuiFrame },
    { "hooks.third_party_swap_chain", &TestThirdPartySwapBuffersChain },
    { "vulkan.layer_wait_does_not_block_other_calls", &TestVulkanLayerWaitDoesNotBlockOtherCalls },
    { "vulkan.frame_completion_tracking", &TestVulkanFrameCompletionTracking },
    { "vulkan.streaming_texture_lifetime", &TestVulkanStreamingTextureLifetime },
    { "vulkan.mirror_content_detection", &TestVulkanMirrorContentDetection },
    { "mirror.add_to_current_mode", &TestAddMirrorToCurrentMode },
    { "vulkan.mirrors_draw_game_opaque", &TestVulkanMirrorsDrawGameOpaque },
    { "vulkan.toolscreen_pipeline_draws", &TestVulkanToolscreenPipelineDraws },
    { "manual.eyezoom_screenshots", &TestEyeZoomScreenshots },
    { "manual.eyezoom_stress_look_around", &TestEyeZoomStressLookAround },
    { "config.save_round_trip", &TestConfigSaveRoundTrip },
    { "config.load_error_screen", &TestConfigLoadErrorScreen },
    { "config.recovered_from_backup", &TestConfigRecoveredFromBackup },
};

bool MatchesFilter(const std::string& name) {
    if (s_filters.empty()) return true;
    return std::any_of(s_filters.begin(), s_filters.end(), [&](const std::string& filter) { return name.find(filter) != std::string::npos; });
}

// Closes the game the way the user would, so the process exits normally and DLL_PROCESS_DETACH runs.
void RequestGameExit() {
    if (s_exitMode == "log_lock") {
        // A thread that holds the log file lock when ExitProcess terminates it, as the log thread does while it
        // writes. Detach must not wait on that lock.
        std::thread([] {
            g_logFileMutex.lock();
            for (;;) Sleep(INFINITE);
        }).detach();
        Sleep(100);
    }
    // Let the game finish starting up, then close its current top-level window (the handle Toolscreen first
    // subclassed may have been replaced). Keep asking until the process goes away.
    WaitForFrames(60, std::chrono::seconds(20));
    AppendResultLine("{\"event\":\"exit-requested\",\"mode\":\"" + JsonEscape(s_exitMode) + "\"}");
    for (int attempt = 0; attempt < 30; ++attempt) {
        HWND target = NULL;
        EnumWindows(
            [](HWND hwnd, LPARAM param) -> BOOL {
                DWORD pid = 0;
                GetWindowThreadProcessId(hwnd, &pid);
                if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != NULL) return TRUE;
                *reinterpret_cast<HWND*>(param) = hwnd;
                return FALSE;
            },
            reinterpret_cast<LPARAM>(&target));
        if (target) PostMessageW(target, WM_CLOSE, 0, 0);
        Sleep(2000);
    }
}

void RunAllTests() {
    AppendResultLine("{\"event\":\"start\",\"version\":\"" + JsonEscape(VersionString(g_gameVersion)) + "\"}");
    if (!s_waitForTitle.empty()) {
        const bool reached = WaitUntil([] {
            wchar_t title[256] = {};
            GetWindowTextW(s_gameWindow, title, static_cast<int>(std::size(title)));
            return WideToUtf8(title).find(s_waitForTitle) != std::string::npos;
        }, std::chrono::seconds(180));
        if (!reached) {
            AppendResultLine("{\"event\":\"error\",\"message\":\"The window title never contained '" + JsonEscape(s_waitForTitle) + "'.\"}");
        }
        // Let the first chunks load and the loading overlay fade.
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    if (!s_windowMode.empty()) {
        RECT before{};
        GetClientRect(s_gameWindow, &before);
        if (s_windowMode == "borderless") ToggleBorderlessWindowedFullscreen(s_gameWindow);
        else if (s_windowMode == "fullscreen") PostKeyPress(g_subclassedHwnd.load(std::memory_order_acquire), VK_F11);
        std::this_thread::sleep_for(std::chrono::seconds(3));
        RECT after{};
        GetClientRect(s_gameWindow, &after);
        Log("[GAME TEST] Window mode '" + s_windowMode + "': client " + std::to_string(before.right) + "x" + std::to_string(before.bottom) +
            " -> " + std::to_string(after.right) + "x" + std::to_string(after.bottom));
    }
    int passed = 0, failed = 0, skipped = 0;

    for (const TestCase& test : kTests) {
        if (s_stopRequested.load(std::memory_order_acquire)) break;
        if (!MatchesFilter(test.name)) continue;

        Log(std::string("[GAME TEST] Running ") + test.name);
        const auto start = std::chrono::steady_clock::now();
        std::string status = "pass";
        std::string message;
        try {
            test.fn();
        } catch (const TestFailure& failure) {
            status = "fail";
            message = failure.message;
        } catch (const TestSkipped& skip) {
            status = "skip";
            message = skip.message;
        } catch (const std::exception& e) {
            status = "fail";
            message = std::string("Unexpected exception: ") + e.what();
        } catch (...) {
            status = "fail";
            message = "Unknown exception.";
        }
        const auto durationMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

        if (status == "pass") ++passed;
        else if (status == "fail") ++failed;
        else ++skipped;
        Log(std::string("[GAME TEST] ") + test.name + ": " + status + (message.empty() ? "" : " - " + message));
        AppendResultLine("{\"event\":\"test\",\"name\":\"" + JsonEscape(test.name) + "\",\"status\":\"" + status + "\",\"message\":\"" +
                         JsonEscape(message) + "\",\"durationMs\":" + std::to_string(durationMs) + "}");
    }

    try {
        RunOnRenderThread([] {
            RemoveTestFixtureOnRenderThread();
            SwitchToMode(DefaultModeId(), "game test cleanup", true);
        });
    } catch (...) {}

    AppendResultLine("{\"event\":\"done\",\"passed\":" + std::to_string(passed) + ",\"failed\":" + std::to_string(failed) +
                     ",\"skipped\":" + std::to_string(skipped) + "}");

    if (!s_exitMode.empty()) RequestGameExit();
}

} // namespace

void InitializeFromEnvironment() {
    const std::string resultsPath = ReadEnvUtf8(L"TOOLSCREEN_GAME_TEST_RESULTS");
    if (resultsPath.empty()) return;

    s_resultsPath = Utf8ToWide(resultsPath);
    s_exitMode = ToLower(ReadEnvUtf8(L"TOOLSCREEN_GAME_TEST_EXIT"));
    s_waitForTitle = ReadEnvUtf8(L"TOOLSCREEN_GAME_TEST_WAIT_FOR_TITLE");
    s_windowMode = ToLower(ReadEnvUtf8(L"TOOLSCREEN_GAME_TEST_WINDOW"));
    s_expectedVersion = ReadEnvUtf8(L"TOOLSCREEN_GAME_TEST_EXPECTED_VERSION");
    s_expectedBackend = ToLower(ReadEnvUtf8(L"TOOLSCREEN_GAME_TEST_EXPECTED_BACKEND"));
    std::stringstream filters(ReadEnvUtf8(L"TOOLSCREEN_GAME_TEST_FILTER"));
    for (std::string filter; std::getline(filters, filter, ',');) {
        if (!filter.empty()) s_filters.push_back(filter);
    }
    s_enabled.store(true, std::memory_order_release);
}

bool IsEnabled() { return s_enabled.load(std::memory_order_acquire); }

void StartRunner(HWND gameWindow) {
    if (!IsEnabled() || s_runnerThread.joinable()) return;
    s_gameWindow = gameWindow;
    Log("[GAME TEST] Test mode enabled; results go to " + WideToUtf8(s_resultsPath));
    s_runnerThread = std::thread([] {
        try {
            RunAllTests();
        } catch (const std::exception& e) {
            AppendResultLine("{\"event\":\"error\",\"message\":\"" + JsonEscape(e.what()) + "\"}");
        } catch (...) {
            AppendResultLine("{\"event\":\"error\",\"message\":\"Unknown runner error.\"}");
        }
    });
}

void OnRenderThreadFrame() {
    if (!s_enabled.load(std::memory_order_relaxed)) return;
    s_renderThreadId.store(GetCurrentThreadId(), std::memory_order_release);
    s_renderFrames.fetch_add(1, std::memory_order_acq_rel);

    std::deque<std::function<void()>> tasks;
    {
        std::lock_guard<std::mutex> lock(s_renderTasksMutex);
        tasks.swap(s_renderTasks);
    }
    for (auto& task : tasks) { task(); }
    ApplyGlGpuPressure();
    if (const int delayMs = s_renderFrameDelayMs.load(std::memory_order_relaxed); delayMs > 0) Sleep(static_cast<DWORD>(delayMs));
}

void RecordDetachComplete() {
    if (!IsEnabled()) return;
    HANDLE file = CreateFileW(s_resultsPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    static const char kLine[] = "{\"event\":\"detached\"}\n";
    DWORD written = 0;
    WriteFile(file, kLine, static_cast<DWORD>(sizeof(kLine) - 1), &written, nullptr);
    CloseHandle(file);
}

bool ShouldForceSharedObsFrame() { return s_forceSharedObsFrame.load(std::memory_order_relaxed); }

void Shutdown() {
    if (!IsEnabled()) return;
    s_stopRequested.store(true, std::memory_order_release);
    // The runner waits on render-thread work with timeouts, so it exits promptly; never block DllMain on it.
    if (s_runnerThread.joinable()) s_runnerThread.detach();
}

} // namespace GameTest
