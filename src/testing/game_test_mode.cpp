#include "game_test_mode.h"
#include "thread_stack_dump.h"

#include "common/utils.h"
#include "config/config_toml.h"
#include "gui/gui.h"
#include "features/window_overlay.h"
#include "hooks/hook_chain.h"
#include "hooks/input_hook.h"
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
#include <cstdio>
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
std::thread s_runnerThread;
std::atomic<bool> s_stopRequested{ false };
HWND s_gameWindow = NULL;

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

    ResizeHotkeySecondaryModes(g_config.hotkeys.size());
    for (size_t i = 0; i < g_config.hotkeys.size(); ++i) { SetHotkeySecondaryMode(i, g_config.hotkeys[i].secondaryMode); }
    RebuildHotkeyMainKeys();
    g_configIsDirty = true;
    PublishGuiConfigSnapshot();
}

void RemoveTestFixtureOnRenderThread() {
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

// With the OBS compose pass running and the settings GUI open, ImGui must build one frame per swap: the OBS pass
// reuses the screen pass's draw data instead of running NewFrame and the whole settings GUI a second time.
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
    { "input.hotkey_switches_mode", &TestHotkeySwitchesMode },
    { "input.low_level_hook_dedicated_thread", &TestLowLevelHookOnDedicatedThread },
    { "gui.toggle_renders_imgui", &TestGuiToggleRendersImGui },
    { "gui.obs_pass_reuses_screen_frame", &TestObsPassReusesScreenImGuiFrame },
    { "hooks.third_party_swap_chain", &TestThirdPartySwapBuffersChain },
    { "vulkan.layer_wait_does_not_block_other_calls", &TestVulkanLayerWaitDoesNotBlockOtherCalls },
    { "vulkan.frame_completion_tracking", &TestVulkanFrameCompletionTracking },
    { "vulkan.streaming_texture_lifetime", &TestVulkanStreamingTextureLifetime },
    { "config.save_round_trip", &TestConfigSaveRoundTrip },
};

bool MatchesFilter(const std::string& name) {
    if (s_filters.empty()) return true;
    return std::any_of(s_filters.begin(), s_filters.end(), [&](const std::string& filter) { return name.find(filter) != std::string::npos; });
}

void RunAllTests() {
    AppendResultLine("{\"event\":\"start\",\"version\":\"" + JsonEscape(VersionString(g_gameVersion)) + "\"}");
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
}

} // namespace

void InitializeFromEnvironment() {
    const std::string resultsPath = ReadEnvUtf8(L"TOOLSCREEN_GAME_TEST_RESULTS");
    if (resultsPath.empty()) return;

    s_resultsPath = Utf8ToWide(resultsPath);
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
}

bool ShouldForceSharedObsFrame() { return s_forceSharedObsFrame.load(std::memory_order_relaxed); }

void Shutdown() {
    if (!IsEnabled()) return;
    s_stopRequested.store(true, std::memory_order_release);
    // The runner waits on render-thread work with timeouts, so it exits promptly; never block DllMain on it.
    if (s_runnerThread.joinable()) s_runnerThread.detach();
}

} // namespace GameTest
