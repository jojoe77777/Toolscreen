#include "game_test_mode.h"

#include "common/utils.h"
#include "config/config_toml.h"
#include "gui/gui.h"
#include "hooks/input_hook.h"
#include "render/render_backend.h"
#include "version.h"

#include "imgui.h"

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
    Require(WaitForFrames(30, std::chrono::seconds(20)), "Fewer than 30 frames passed through the render hook in 20 s.");
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

void Shutdown() {
    if (!IsEnabled()) return;
    s_stopRequested.store(true, std::memory_order_release);
    // The runner waits on render-thread work with timeouts, so it exits promptly; never block DllMain on it.
    if (s_runnerThread.joinable()) s_runnerThread.detach();
}

} // namespace GameTest
