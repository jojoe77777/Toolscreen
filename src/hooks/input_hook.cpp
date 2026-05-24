#include "input_hook.h"

#include "features/fake_cursor.h"
#include "features/virtual_camera.h"
#include "common/mode_dimensions.h"
#include "gui/gui.h"
#include "gui/imgui_cache.h"
#include "runtime/logic_thread.h"
#include "common/profiler.h"
#include "render/render.h"
#include "common/utils.h"
#include "version.h"
#include "features/window_overlay.h"

#include "imgui_impl_win32.h"

#include "gui/imgui_input_queue.h"

#include <chrono>
#include <cmath>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <windowsx.h>

extern std::atomic<bool> g_showGui;
extern std::atomic<bool> g_guiNeedsRecenter;
extern std::atomic<bool> g_wasCursorVisible;
extern std::atomic<bool> g_isShuttingDown;
extern std::atomic<HWND> g_subclassedHwnd;
extern WNDPROC g_originalWndProc;
extern std::atomic<bool> g_configLoadFailed;
extern std::atomic<int> g_wmMouseMoveCount;
extern GameVersion g_gameVersion;
extern Config g_config;
extern HMODULE g_hModule;

extern std::string g_currentModeId;
extern std::mutex g_modeIdMutex;
extern std::string g_gameStateBuffers[2];
extern std::atomic<int> g_currentGameStateIndex;
extern std::string g_currentlyEditingMirror;

extern std::atomic<bool> g_imageDragMode;
extern std::atomic<bool> g_windowOverlayDragMode;
extern std::atomic<HCURSOR> g_specialCursorHandle;
// g_glInitialized is declared in render.h as atomic bool
extern std::atomic<bool> g_gameWindowActive;
extern std::atomic<bool> g_ninjabrainOverlayVisible;

extern std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_hotkeyTimestamps;
extern std::mutex g_hotkeyTimestampsMutex;
extern std::unordered_set<DWORD> g_hotkeyMainKeys;
extern std::mutex g_hotkeyMainKeysMutex;
extern std::set<std::string> g_triggerOnReleasePending;
extern std::set<std::string> g_triggerOnReleaseInvalidated;
extern std::mutex g_triggerOnReleaseMutex;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
static size_t s_bestMatchKeyCount = 0;
static std::unordered_map<DWORD, size_t> s_bestMatchKeyCountByMainVk;
static HHOOK s_lowLevelKeyboardHook = NULL;
static std::mutex s_lowLevelKeyboardHookMutex;
static std::atomic<bool> s_deferredFocusRegainWmSizePending{ false };
static constexpr UINT_PTR kToolscreenLocalKeyRepeatTimerId = 0x2A51;
static constexpr UINT_PTR kToolscreenShiftHotkeyPollTimerId = 0x2A52;
static constexpr LPARAM kToolscreenLocalKeyRepeatMessageTag = (static_cast<LPARAM>(1) << 25);
static constexpr ULONG_PTR kToolscreenInjectedExtraInfo = (ULONG_PTR)0x5453434E;
static constexpr ULONG_PTR kToolscreenMenuMaskExtraInfo = (ULONG_PTR)0x54534D4B;
static void UpdateLowLevelKeyboardHookInstalledState();
static void ResetShiftHotkeyPollingState(HWND hWnd);
static void UpdateShiftHotkeyPollingState(HWND hWnd);
static void SyncShiftHotkeyPollingStateFromMessage(UINT uMsg, WPARAM wParam, LPARAM lParam);
static InputHandlerResult HandleShiftHotkeyPolling(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
static bool EnsureLocalKeyRepeatSchedulerInitialized();
static DWORD WINAPI LocalKeyRepeatSchedulerThreadProc(LPVOID parameter);

struct LocalKeyRepeatState {
    DWORD rawVk = 0;
    UINT scanCodeWithFlags = 0;
    bool isSystemKey = false;
    DWORD repeatVk = 0;
    UINT repeatScanCodeWithFlags = 0;
    LPARAM sourceKeyDownLParam = 0;
    DWORD sourceMessageTimeMs = 0;
};

struct SuppressedLocalRepeatChar {
    DWORD rawVk = 0;
    LPARAM lParam = 0;
};

struct LowLevelSuppressedKeyState {
    DWORD rawVk = 0;
    UINT scanCodeWithFlags = 0;
    bool isSystemKey = false;
};

struct LowLevelExactModifierEvent {
    DWORD vk = 0;
    bool isKeyDown = false;
};

struct ExactKeyboardMessageState {
    UINT uMsg = 0;
    WPARAM wParam = 0;
    LPARAM lParam = 0;
    DWORD trackedVk = 0;
    bool isKeyDown = false;
    bool isKeyUp = false;
    bool exactKeyWasDown = false;
};

static std::unordered_map<DWORD, LocalKeyRepeatState> s_localKeyRepeatHeldKeys;
static LocalKeyRepeatState s_localKeyRepeatOwner;
static bool s_localKeyRepeatOwnerActive = false;
static std::vector<SuppressedLocalRepeatChar> s_localKeyRepeatSuppressedChars;
static HANDLE s_localKeyRepeatHighResTimer = NULL;
static HANDLE s_localKeyRepeatWakeEvent = NULL;
static HANDLE s_localKeyRepeatThread = NULL;
static ULONGLONG s_localKeyRepeatDelayGateDeadlineMs = 0;
static std::mutex s_localKeyRepeatSchedulerInitMutex;
static std::atomic<HWND> s_localKeyRepeatScheduledHwnd{ NULL };
static std::atomic<int64_t> s_localKeyRepeatRequestedDelay100ns{ 0 };
static std::unordered_map<DWORD, LowLevelSuppressedKeyState> s_lowLevelSuppressedKeys;
static std::mutex s_lowLevelSuppressedKeysMutex;
static std::unordered_set<DWORD> s_lowLevelExactModifierKeysDown;
static std::deque<LowLevelExactModifierEvent> s_pendingLowLevelExactModifierEvents;
static std::mutex s_lowLevelExactModifierStateMutex;
static bool s_systemAltTabPassthroughActive = false;
static std::unordered_set<DWORD> s_exactKeyboardKeysDown;
static thread_local std::vector<ExactKeyboardMessageState> s_exactKeyboardMessageStateStack;
struct ShiftHotkeyPollState {
    bool timerArmed = false;
    bool exactShiftHotkeysEnabled = false;
    bool lastLeftDown = false;
    bool lastRightDown = false;
};
static ShiftHotkeyPollState s_shiftHotkeyPollState;
struct ActiveHoldHotkeyState {
    std::string hotkeyId;
    std::vector<DWORD> keys;
    DWORD mainKey = 0;
    bool blockKey = false;
};
static std::optional<ActiveHoldHotkeyState> s_activeHoldHotkey;
static std::unordered_map<uint64_t, UINT> s_activeSyntheticRebindOutputsBySource;
static std::unordered_map<UINT, size_t> s_activeSyntheticRebindOutputRefCounts;
static std::mutex s_activeSyntheticRebindOutputsMutex;

#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
struct SyntheticRebindKeyEventForTest {
    UINT scanCodeWithFlags = 0;
    bool keyDown = false;
};

static std::vector<SyntheticRebindKeyEventForTest> s_syntheticRebindKeyEventsForTests;
static std::unordered_map<DWORD, bool> s_physicalModifierDownOverridesForTests;
#endif

static bool SendMenuMaskKeyTap();
static bool SendSynthKeyByScanCode(UINT scanCodeWithFlags, bool keyDown);
static bool SendSyntheticRebindOutput(UINT scanCodeWithFlags, bool keyDown);
static bool HotkeyUsesWindowsKey(const std::vector<DWORD>& keys);
static bool ShouldMaskWindowsKeyForHotkey(const std::vector<DWORD>& keys, bool isKeyDown, bool isAutoRepeatKeyDown);
static uint64_t BuildSyntheticRebindOutputSourceId(DWORD vkCode, DWORD rawVkCode, bool isMouseButton);
static void TrackSyntheticRebindOutputHold(uint64_t sourceId, UINT outputScanCode);
static bool ReleaseTrackedSyntheticRebindOutputHold(uint64_t sourceId);
static void ReleaseAllTrackedSyntheticRebindOutputHolds();
static bool IsNonCharKeyVk(DWORD vk);
static bool IsLocalRepeatDebugEnabled();
static const char* GetLocalRepeatMessageName(UINT uMsg);
static std::string FormatLocalKeyRepeatStateForDebug(const LocalKeyRepeatState& state);
static void LogLocalRepeatDebug(const char* phase, UINT uMsg, WPARAM wParam, LPARAM lParam, bool isLocalRepeatTagged);
static bool RetargetLocalKeyRepeatAliasSource(DWORD rawVk, UINT scanCodeWithFlags, bool isSystemKey, LPARAM sourceKeyDownLParam);
static UINT ResolveLocalKeyRepeatOutputScanCode(DWORD rawVk, UINT incomingScanCodeWithFlags);
static bool TryResolveLocalKeyRepeatVkFromChar(WPARAM charCode, DWORD& outVk, UINT& outScanCodeWithFlags);
static InputHandlerResult HandleLocalKeyRepeat(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, bool isLocalRepeatTagged);
bool GetEffectiveKeyRepeatTimings(int& outStartDelayMs, int& outRepeatDelayMs);
static void ReleaseSuppressedLowLevelRebindKeys(HWND hWnd);

static bool MatchesConfiguredGameStateCondition(const std::vector<std::string>& configuredStates, const std::string& gameState) {
    if (configuredStates.empty()) {
        return true;
    }

    const bool cursorVisible = IsCursorVisible();
    for (const std::string& configuredState : configuredStates) {
        if (configuredState == gameState) {
            return true;
        }
        if (configuredState == "any,cursor_free" && cursorVisible) {
            return true;
        }
        if (configuredState == "any,cursor_grabbed" && !cursorVisible) {
            return true;
        }
    }

    return false;
}

enum class KeyRebindCursorStateMatchPriority {
    None = 0,
    Any = 1,
    Exact = 2,
};

static bool IsConsumeOnlyKeyRebind(const KeyRebind& rebind) {
    return rebind.enabled && rebind.fromKey != 0 && rebind.toKey == 0;
}

static bool IsTriggerOutputDisabled(const KeyRebind& rebind) {
    return !IsConsumeOnlyKeyRebind(rebind) && rebind.triggerOutputDisabled;
}

static bool IsBaseTypedOutputDisabled(const KeyRebind& rebind) {
    return rebind.baseOutputDisabled;
}

static bool IsShiftLayerTypedOutputDisabled(const KeyRebind& rebind) {
    return rebind.shiftLayerEnabled && rebind.shiftLayerOutputDisabled;
}

static bool IsEffectiveTypedOutputDisabled(const KeyRebind& rebind, bool shiftLayerActive) {
    return shiftLayerActive ? IsShiftLayerTypedOutputDisabled(rebind) : IsBaseTypedOutputDisabled(rebind);
}

static KeyRebindCursorStateMatchPriority GetKeyRebindCursorStateMatchPriority(const KeyRebind& rebind, bool cursorVisible) {
    if (rebind.cursorState == kKeyRebindCursorStateCursorFree) {
        return cursorVisible ? KeyRebindCursorStateMatchPriority::Exact : KeyRebindCursorStateMatchPriority::None;
    }
    if (rebind.cursorState == kKeyRebindCursorStateCursorGrabbed) {
        return cursorVisible ? KeyRebindCursorStateMatchPriority::None : KeyRebindCursorStateMatchPriority::Exact;
    }
    return KeyRebindCursorStateMatchPriority::Any;
}

template <typename Predicate>
static const KeyRebind* FindPreferredEnabledKeyRebind(const std::vector<KeyRebind>& rebinds,
                                                      bool cursorVisible,
                                                      Predicate predicate) {
    const KeyRebind* anyMatch = nullptr;

    for (const auto& rebind : rebinds) {
        if (!rebind.enabled || rebind.fromKey == 0) {
            continue;
        }
        if (rebind.toKey == 0 && !IsConsumeOnlyKeyRebind(rebind)) {
            continue;
        }
        if (!predicate(rebind)) {
            continue;
        }

        const KeyRebindCursorStateMatchPriority matchPriority = GetKeyRebindCursorStateMatchPriority(rebind, cursorVisible);
        if (matchPriority == KeyRebindCursorStateMatchPriority::Exact) {
            return &rebind;
        }
        if (matchPriority == KeyRebindCursorStateMatchPriority::Any && anyMatch == nullptr) {
            anyMatch = &rebind;
        }
    }

    return anyMatch;
}

static UINT GetScanCodeWithExtendedFlagFromLParam(LPARAM lParam) {
    UINT scanCodeWithFlags = static_cast<UINT>((lParam >> 16) & 0xFF);
    if ((lParam & (1LL << 24)) != 0) {
        scanCodeWithFlags |= 0xE000;
    }
    return scanCodeWithFlags;
}

static void EnsureSystemCursorVisible() {
    if (g_gameVersion < GameVersion(1, 13, 0)) { return; }

    CURSORINFO ci{ sizeof(CURSORINFO) };
    if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING)) { return; }
    ShowCursor(TRUE);
}

static void EnsureSystemCursorHidden() {
    if (g_gameVersion < GameVersion(1, 13, 0)) { return; }

    CURSORINFO ci{ sizeof(CURSORINFO) };
    if (GetCursorInfo(&ci) && !(ci.flags & CURSOR_SHOWING)) { return; }
    ShowCursor(FALSE);
}

static DWORD NormalizeModifierVkFromKeyMessage(DWORD rawVk, LPARAM lParam) {
    DWORD vk = rawVk;

    const UINT scanCode = static_cast<UINT>((lParam >> 16) & 0xFF);
    const bool isExtended = (lParam & (1LL << 24)) != 0;

    if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) {
        if (scanCode != 0) {
            DWORD mapped = static_cast<DWORD>(::MapVirtualKeyW(scanCode, MAPVK_VSC_TO_VK_EX));
            if (mapped == VK_LSHIFT || mapped == VK_RSHIFT) {
                vk = mapped;
            }
        }
        return vk;
    }

    if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) {
        return isExtended ? VK_RCONTROL : VK_LCONTROL;
    }
    if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) {
        return isExtended ? VK_RMENU : VK_LMENU;
    }

    return vk;
}

static DWORD NormalizeModifierVkFromConfig(DWORD vk, UINT scanCodeWithFlags = 0) {
    const UINT scanLow = scanCodeWithFlags & 0xFF;
    const bool isExtended = (scanCodeWithFlags & 0xFF00) != 0;

    switch (vk) {
    case VK_SHIFT:
        if (scanLow == 0x36) return VK_RSHIFT;
        return VK_LSHIFT;
    case VK_CONTROL:
        return isExtended ? VK_RCONTROL : VK_LCONTROL;
    case VK_MENU:
        return isExtended ? VK_RMENU : VK_LMENU;
    default:
        return vk;
    }
}

static bool IsTrackedSpecificModifierVk(DWORD vk) {
    switch (vk) {
    case VK_LSHIFT:
    case VK_RSHIFT:
    case VK_LCONTROL:
    case VK_RCONTROL:
    case VK_LMENU:
    case VK_RMENU:
        return true;
    default:
        return false;
    }
}

static DWORD GetOppositeTrackedModifierVk(DWORD vk) {
    switch (vk) {
    case VK_LSHIFT:
        return VK_RSHIFT;
    case VK_RSHIFT:
        return VK_LSHIFT;
    case VK_LCONTROL:
        return VK_RCONTROL;
    case VK_RCONTROL:
        return VK_LCONTROL;
    case VK_LMENU:
        return VK_RMENU;
    case VK_RMENU:
        return VK_LMENU;
    default:
        return 0;
    }
}

static bool IsTrackedModifierKeyDownNow(DWORD vk) {
    if (vk == 0) {
        return false;
    }

#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
    if (const auto overrideIt = s_physicalModifierDownOverridesForTests.find(vk);
        overrideIt != s_physicalModifierDownOverridesForTests.end()) {
        return ((GetKeyState(static_cast<int>(vk)) & 0x8000) != 0) || overrideIt->second;
    }
#endif

    return ((GetKeyState(static_cast<int>(vk)) & 0x8000) != 0) || ((GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0);
}

static DWORD GetTrackedModifierFamilyVk(DWORD vk) {
    switch (vk) {
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
        return VK_SHIFT;
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
        return VK_CONTROL;
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
        return VK_MENU;
    default:
        return 0;
    }
}

static bool AreTrackedModifierFamilyMatches(DWORD a, DWORD b) {
    const DWORD familyA = GetTrackedModifierFamilyVk(a);
    return familyA != 0 && familyA == GetTrackedModifierFamilyVk(b);
}

static void ResetLowLevelExactModifierState() {
    std::lock_guard<std::mutex> lock(s_lowLevelExactModifierStateMutex);
    s_lowLevelExactModifierKeysDown.clear();
    s_pendingLowLevelExactModifierEvents.clear();
}

static void RecordLowLevelExactModifierEvent(DWORD vk, bool isKeyDown) {
    if (!IsTrackedSpecificModifierVk(vk)) {
        return;
    }

    std::lock_guard<std::mutex> lock(s_lowLevelExactModifierStateMutex);
    if (isKeyDown) {
        s_lowLevelExactModifierKeysDown.insert(vk);
        return;
    }

    s_lowLevelExactModifierKeysDown.erase(vk);
    if (s_pendingLowLevelExactModifierEvents.size() >= 16) {
        s_pendingLowLevelExactModifierEvents.pop_front();
    }
    s_pendingLowLevelExactModifierEvents.push_back({ vk, false });
}

static DWORD ConsumePendingLowLevelExactModifierKeyup(DWORD trackedVk) {
    if (!IsTrackedSpecificModifierVk(trackedVk)) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(s_lowLevelExactModifierStateMutex);
    for (auto it = s_pendingLowLevelExactModifierEvents.begin(); it != s_pendingLowLevelExactModifierEvents.end(); ++it) {
        if (it->isKeyDown) {
            continue;
        }
        if (!AreTrackedModifierFamilyMatches(it->vk, trackedVk)) {
            continue;
        }

        const DWORD resolvedVk = it->vk;
        s_pendingLowLevelExactModifierEvents.erase(it);
        return resolvedVk;
    }

    return 0;
}

static DWORD ResolveTrackedModifierKeyupFromLowLevelState(DWORD trackedVk) {
    if (!IsTrackedSpecificModifierVk(trackedVk)) {
        return 0;
    }

    if (const DWORD resolvedVk = ConsumePendingLowLevelExactModifierKeyup(trackedVk); resolvedVk != 0) {
        return resolvedVk;
    }

    const DWORD oppositeVk = GetOppositeTrackedModifierVk(trackedVk);
    if (oppositeVk == 0) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(s_lowLevelExactModifierStateMutex);
    const bool trackedIsDown = s_lowLevelExactModifierKeysDown.contains(trackedVk);
    const bool oppositeIsDown = s_lowLevelExactModifierKeysDown.contains(oppositeVk);
    if (trackedIsDown && !oppositeIsDown) {
        return oppositeVk;
    }
    if (!trackedIsDown && oppositeIsDown) {
        return trackedVk;
    }

    return 0;
}

static bool IsTrackedExactKeyboardMessage(UINT uMsg) {
    return uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN || uMsg == WM_KEYUP || uMsg == WM_SYSKEYUP;
}

static DWORD ResolveTrackedKeyboardVkFromMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (!IsTrackedExactKeyboardMessage(uMsg)) {
        return 0;
    }

    const bool isKeyDown = (uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN);
    const bool isKeyUp = (uMsg == WM_KEYUP || uMsg == WM_SYSKEYUP);
    const DWORD rawVk = static_cast<DWORD>(wParam);
    DWORD trackedVk = NormalizeModifierVkFromKeyMessage(rawVk, lParam);
    if (trackedVk == 0) {
        trackedVk = rawVk;
    }

    if (!IsTrackedSpecificModifierVk(trackedVk)) {
        return trackedVk;
    }

    const DWORD oppositeVk = GetOppositeTrackedModifierVk(trackedVk);
    if (oppositeVk == 0) {
        return trackedVk;
    }

    if (isKeyUp) {
        if (const DWORD lowLevelResolvedVk = ResolveTrackedModifierKeyupFromLowLevelState(trackedVk); lowLevelResolvedVk != 0) {
            return lowLevelResolvedVk;
        }
    }

    const bool trackedWasDown = s_exactKeyboardKeysDown.contains(trackedVk);
    const bool oppositeWasDown = s_exactKeyboardKeysDown.contains(oppositeVk);
    const bool trackedIsDownNow = IsTrackedModifierKeyDownNow(trackedVk);
    const bool oppositeIsDownNow = IsTrackedModifierKeyDownNow(oppositeVk);

    if (isKeyDown) {
        if (trackedWasDown && !oppositeWasDown && trackedIsDownNow && oppositeIsDownNow) {
            return oppositeVk;
        }
        if (!trackedWasDown && oppositeWasDown && trackedIsDownNow && oppositeIsDownNow) {
            return trackedVk;
        }
    }

    if (isKeyUp) {
        if (trackedIsDownNow && !oppositeIsDownNow) {
            return oppositeVk;
        }
        if (!trackedIsDownNow && oppositeIsDownNow) {
            return trackedVk;
        }
        if (trackedWasDown != oppositeWasDown) {
            return trackedWasDown ? trackedVk : oppositeVk;
        }
    }

    return trackedVk;
}

static DWORD ResolveEffectiveKeyboardVkForMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (!IsTrackedExactKeyboardMessage(uMsg)) {
        return static_cast<DWORD>(wParam);
    }

    if (!s_exactKeyboardMessageStateStack.empty()) {
        const ExactKeyboardMessageState& current = s_exactKeyboardMessageStateStack.back();
        if (current.uMsg == uMsg && current.wParam == wParam && current.lParam == lParam) {
            return current.trackedVk != 0 ? current.trackedVk : static_cast<DWORD>(wParam);
        }
    }

    const DWORD trackedVk = ResolveTrackedKeyboardVkFromMessage(uMsg, wParam, lParam);
    return trackedVk != 0 ? trackedVk : static_cast<DWORD>(wParam);
}

class ScopedExactKeyboardMessageState {
  public:
    ScopedExactKeyboardMessageState(UINT uMsg, WPARAM wParam, LPARAM lParam) {
        if (!IsTrackedExactKeyboardMessage(uMsg)) {
            return;
        }

        if (!s_exactKeyboardMessageStateStack.empty()) {
            const ExactKeyboardMessageState& current = s_exactKeyboardMessageStateStack.back();
            if (current.uMsg == uMsg && current.wParam == wParam && current.lParam == lParam) {
                return;
            }
        }

        ExactKeyboardMessageState state;
        state.uMsg = uMsg;
        state.wParam = wParam;
        state.lParam = lParam;
        state.trackedVk = ResolveTrackedKeyboardVkFromMessage(uMsg, wParam, lParam);
        state.isKeyDown = (uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN);
        state.isKeyUp = (uMsg == WM_KEYUP || uMsg == WM_SYSKEYUP);
        state.exactKeyWasDown = state.trackedVk != 0 && s_exactKeyboardKeysDown.contains(state.trackedVk);

        s_exactKeyboardMessageStateStack.push_back(state);
        owned_ = true;
    }

    ~ScopedExactKeyboardMessageState() {
        if (!owned_) {
            return;
        }
        if (s_exactKeyboardMessageStateStack.empty()) {
            return;
        }

        const ExactKeyboardMessageState state = s_exactKeyboardMessageStateStack.back();
        s_exactKeyboardMessageStateStack.pop_back();

        if (state.trackedVk == 0) {
            return;
        }

        if (state.isKeyDown) {
            s_exactKeyboardKeysDown.insert(state.trackedVk);
        } else if (state.isKeyUp) {
            s_exactKeyboardKeysDown.erase(state.trackedVk);
        }
    }

  private:
    bool owned_ = false;
};

static void ResetExactKeyboardMessageState() {
    s_exactKeyboardKeysDown.clear();
    s_exactKeyboardMessageStateStack.clear();
}

static bool IsTrackedExactAutoRepeatKeyDown(UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg != WM_KEYDOWN && uMsg != WM_SYSKEYDOWN) {
        return false;
    }
    if ((lParam & (static_cast<LPARAM>(1) << 30)) == 0) {
        return false;
    }

    if (!s_exactKeyboardMessageStateStack.empty()) {
        const ExactKeyboardMessageState& current = s_exactKeyboardMessageStateStack.back();
        if (current.uMsg == uMsg && current.wParam == wParam && current.lParam == lParam) {
            return current.exactKeyWasDown;
        }
    }

    const DWORD trackedVk = ResolveTrackedKeyboardVkFromMessage(uMsg, wParam, lParam);
    return trackedVk != 0 && s_exactKeyboardKeysDown.contains(trackedVk);
}

static bool MatchesRebindSourceKey(DWORD incomingVk, DWORD incomingRawVk, DWORD fromKey) {
    return MatchesConfiguredInputKeyEvent(incomingVk, incomingRawVk, fromKey);
}

static bool AreConfiguredKeysCurrentlyDown(const std::vector<DWORD>& keys) {
    if (keys.empty()) return false;

    for (DWORD key : keys) {
        if (key == 0 || !IsConfiguredInputKeyDown(key)) {
            return false;
        }
    }

    return true;
}

static void ActivateHoldHotkeyState(const std::vector<DWORD>& keys, const std::string& hotkeyId, bool blockKey) {
    if (keys.empty() || hotkeyId.empty()) return;

    s_activeHoldHotkey = ActiveHoldHotkeyState{ hotkeyId, keys, keys.back(), blockKey };
}

static bool TryHandleBrokenHoldHotkey(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, DWORD vkCode, DWORD rawVkCode,
                                      DWORD rebindTargetVk, bool isKeyDown, const Config& cfg, bool enableHotkeyDebug,
                                      InputHandlerResult& outResult) {
    outResult = { false, 0 };

    if (!s_activeHoldHotkey.has_value()) {
        return false;
    }

    const ActiveHoldHotkeyState activeHold = *s_activeHoldHotkey;
    if (AreConfiguredKeysCurrentlyDown(activeHold.keys)) {
        return false;
    }

    if (enableHotkeyDebug) {
        Log("[Hotkey] HOLD RELEASE (state): " + activeHold.hotkeyId + " -> " + cfg.defaultMode);
    }

    s_activeHoldHotkey.reset();
    if (!cfg.defaultMode.empty()) {
        SwitchToMode(cfg.defaultMode, "hotkey (hold release)");
    }

    const bool releasedByMainKey = !isKeyDown &&
                                   (MatchesConfiguredInputKeyEvent(vkCode, rawVkCode, activeHold.mainKey) ||
                                    (rebindTargetVk != 0 &&
                                     MatchesConfiguredInputKeyEvent(rebindTargetVk, rebindTargetVk, activeHold.mainKey)));
    if (!releasedByMainKey) {
        return true;
    }

    if (activeHold.blockKey) {
        outResult = { true, 0 };
    } else {
        outResult = { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
    }

    return true;
}

static bool IsModifierVk(DWORD vk) {
    return vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
           vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN;
}

static bool IsAltVk(DWORD vk) {
    return vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU;
}

static bool IsTabVk(DWORD vk) {
    return vk == VK_TAB;
}

static bool IsShiftVk(DWORD vk) {
    return vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT;
}

static bool IsPhysicalShiftKeyDown(DWORD vk) {
    if (vk == 0) {
        return false;
    }

#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
    if (const auto overrideIt = s_physicalModifierDownOverridesForTests.find(vk);
        overrideIt != s_physicalModifierDownOverridesForTests.end()) {
        return overrideIt->second;
    }
#endif

    return (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
}

static bool IsAltCurrentlyDown() {
    return (GetAsyncKeyState(VK_MENU) & 0x8000) != 0 || (GetAsyncKeyState(VK_LMENU) & 0x8000) != 0 ||
           (GetAsyncKeyState(VK_RMENU) & 0x8000) != 0;
}

static bool IsTabCurrentlyDown() {
    return (GetAsyncKeyState(VK_TAB) & 0x8000) != 0;
}

static void UpdateSystemAltTabPassthroughState() {
    if (!s_systemAltTabPassthroughActive) return;

    if (IsAltCurrentlyDown() || IsTabCurrentlyDown()) {
        return;
    }

    s_systemAltTabPassthroughActive = false;
}

static bool DoesSubclassedWindowOwnForegroundInput() {
    const HWND targetHwnd = g_subclassedHwnd.load(std::memory_order_acquire);
    return IsWindowInForegroundTree(targetHwnd);
}

static bool IsShiftCurrentlyDown() {
    return (GetKeyState(VK_SHIFT) & 0x8000) != 0;
}

static bool IsCapsLockCurrentlyOn() {
    return (GetKeyState(VK_CAPITAL) & 0x0001) != 0;
}

static bool IsShiftDownForIncomingEvent(DWORD incomingVk, DWORD incomingRawVk, bool isKeyDown) {
    if (IsShiftVk(incomingVk) || incomingRawVk == VK_SHIFT) {
        return isKeyDown;
    }
    return IsShiftCurrentlyDown();
}

static bool HasShiftLayerOutputVk(const KeyRebind& rebind) {
    return rebind.shiftLayerEnabled && !rebind.shiftLayerOutputDisabled && rebind.shiftLayerOutputVK != 0;
}

static bool HasShiftLayerOutputUnicode(const KeyRebind& rebind) {
    return rebind.shiftLayerEnabled && !rebind.shiftLayerOutputDisabled && rebind.shiftLayerOutputUnicode != 0;
}

static bool HasBaseOutputUnicode(const KeyRebind& rebind) {
    return rebind.useCustomOutput && rebind.customOutputUnicode != 0;
}

static bool HasShiftLayerOutputOverride(const KeyRebind& rebind) {
    return IsShiftLayerTypedOutputDisabled(rebind) || HasShiftLayerOutputVk(rebind) || HasShiftLayerOutputUnicode(rebind);
}

static bool IsSplitRebindTextMode(const KeyRebind& rebind) {
    if (rebind.triggerOutputDisabled) return true;
    if (rebind.baseOutputDisabled) return true;
    if (rebind.shiftLayerOutputDisabled) return true;
    if (rebind.customOutputUnicode != 0) return true;
    if (rebind.baseOutputShifted) return true;
    if (rebind.shiftLayerUsesCapsLock) return true;
    if (HasShiftLayerOutputOverride(rebind)) return true;

    DWORD baseTextVk = (rebind.useCustomOutput && rebind.customOutputVK != 0) ? rebind.customOutputVK : rebind.fromKey;
    if (baseTextVk == 0) baseTextVk = rebind.fromKey;

    DWORD triggerVk = rebind.toKey;
    if (triggerVk == 0) triggerVk = rebind.fromKey;

    return baseTextVk != triggerVk;
}

static bool ShouldIgnoreCapsLockForRebindText(const KeyRebind& rebind) {
    return IsSplitRebindTextMode(rebind);
}

static bool IsShiftLayerActive(const KeyRebind& rebind, bool shiftDown, bool capsLockOn) {
    if (!HasShiftLayerOutputOverride(rebind)) return false;
    if (shiftDown) return true;
    return rebind.shiftLayerUsesCapsLock && capsLockOn;
}

static bool IsShiftLayerActiveForRebind(const KeyRebind& rebind, DWORD incomingVk, DWORD incomingRawVk, bool isKeyDown) {
    return IsShiftLayerActive(rebind, IsShiftDownForIncomingEvent(incomingVk, incomingRawVk, isKeyDown), IsCapsLockCurrentlyOn());
}

static DWORD ResolveEffectiveCustomOutputVk(const KeyRebind& rebind, bool shiftLayerActive) {
    if (shiftLayerActive && IsShiftLayerTypedOutputDisabled(rebind)) {
        return 0;
    }
    if (!shiftLayerActive && IsBaseTypedOutputDisabled(rebind)) {
        return 0;
    }
    if (shiftLayerActive && HasShiftLayerOutputUnicode(rebind)) {
        return 0;
    }
    if (!shiftLayerActive && HasBaseOutputUnicode(rebind)) {
        return 0;
    }
    if (shiftLayerActive && HasShiftLayerOutputVk(rebind)) {
        return rebind.shiftLayerOutputVK;
    }
    if (rebind.useCustomOutput && rebind.customOutputVK != 0) {
        return rebind.customOutputVK;
    }
    return 0;
}

static DWORD ResolveModifierVkFromScanCode(UINT scanCodeWithFlags) {
    const UINT scanLow = (scanCodeWithFlags & 0xFF);
    if (scanLow == 0) return 0;

    static thread_local std::unordered_map<UINT, DWORD> s_scanCodeToModifierVk;

    DWORD mappedVk = 0;
    auto it = s_scanCodeToModifierVk.find(scanCodeWithFlags);
    if (it != s_scanCodeToModifierVk.end()) {
        mappedVk = it->second;
    } else {
        mappedVk = static_cast<DWORD>(::MapVirtualKeyW(scanCodeWithFlags, MAPVK_VSC_TO_VK_EX));
        if (mappedVk == 0 && (scanCodeWithFlags & 0xFF00) != 0) {
            mappedVk = static_cast<DWORD>(::MapVirtualKeyW(scanLow, MAPVK_VSC_TO_VK_EX));
        }
        if (mappedVk != 0) {
            mappedVk = NormalizeModifierVkFromConfig(mappedVk, scanCodeWithFlags);
        }
        if (!IsModifierVk(mappedVk)) {
            mappedVk = 0;
        }
        s_scanCodeToModifierVk.emplace(scanCodeWithFlags, mappedVk);
    }

    return mappedVk;
}

static bool IsModifierScanCode(UINT scanCodeWithFlags) {
    return ResolveModifierVkFromScanCode(scanCodeWithFlags) != 0;
}

static bool TryGetClientSize(HWND hWnd, int& outW, int& outH) {
    outW = 0;
    outH = 0;
    if (!hWnd) { return false; }

    RECT clientRect{};
    if (!GetClientRect(hWnd, &clientRect)) { return false; }

    const int clientW = clientRect.right - clientRect.left;
    const int clientH = clientRect.bottom - clientRect.top;
    if (clientW <= 0 || clientH <= 0) { return false; }

    outW = clientW;
    outH = clientH;
    return true;
}

static void ResendCurrentModeWmSize(HWND hWnd, const char* source) {
    if (!hWnd || !IsWindow(hWnd)) { return; }

    auto cfgSnap = GetConfigSnapshot();
    if (!cfgSnap) { return; }

    const std::string currentModeId = g_modeIdBuffers[g_currentModeIdIndex.load(std::memory_order_acquire)];
    const ModeConfig* mode = GetModeFromSnapshotOrFallback(*cfgSnap, currentModeId);
    if (!mode || mode->width <= 0 || mode->height <= 0) { return; }

    if (EqualsIgnoreCase(mode->id, "Fullscreen") && mode->useRelativeSize) {
        // Real window-size changes will trigger a logic-thread recalculation that reposts
        // WM_SIZE with the freshly recomputed internal size. Avoid sending the stale
        // pre-recalc fullscreen-relative dimensions here.
        return;
    }

    RequestWindowClientResize(hWnd, mode->width, mode->height, source);
}

static bool IsFocusGainMessage(UINT uMsg) {
    return uMsg == WM_ACTIVATE || uMsg == WM_ACTIVATEAPP || uMsg == WM_SETFOCUS;
}

static void QueueDeferredFocusRegainWmSize(HWND hWnd) {
    if (!hWnd || !IsWindow(hWnd)) { return; }

    if (!PostMessage(hWnd, WM_TOOLSCREEN_APPLY_FOCUS_REGAIN_SIZE, 0, 0)) {
        s_deferredFocusRegainWmSizePending.store(false, std::memory_order_relaxed);
        Log("[WINDOW] Failed to post deferred focus-regain WM_SIZE. Error=" + std::to_string(GetLastError()));
    }
}

static void SyncWindowMetricsFromMessage(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    constexpr int kInactiveTransientClientMin = 32;
    constexpr int kFullscreenTolPx = 1;
    bool shouldInvalidateScreenMetrics = false;
    bool shouldRequestRecalc = false;
    bool shouldInvalidateImGui = false;
    bool shouldRecenterGui = false;
    bool shouldResetGameTexture = false;
    bool sizeMayHaveChanged = false;

    switch (uMsg) {
    case WM_MOVE:
    case WM_MOVING:
        shouldInvalidateScreenMetrics = true;
        shouldRequestRecalc = true;
        break;

    case WM_SIZE:
        shouldInvalidateScreenMetrics = true;
        shouldInvalidateImGui = true;
        shouldRequestRecalc = true;
        sizeMayHaveChanged = (wParam != SIZE_MINIMIZED);
        break;

    case WM_SIZING:
        // During live-resize, client metrics are often transient.
        // Mark dirty only; WM_SIZE/WM_WINDOWPOSCHANGED will commit stable size.
        shouldInvalidateScreenMetrics = true;
        // No-op: resize tracking removed
        break;

    case WM_WINDOWPOSCHANGED: {
        if (lParam == 0) { break; }

        const WINDOWPOS* pos = reinterpret_cast<const WINDOWPOS*>(lParam);
        const bool sizeChanged = (pos->flags & SWP_NOSIZE) == 0;
        const bool moveChanged = (pos->flags & SWP_NOMOVE) == 0;
        if (!sizeChanged && !moveChanged) { break; }

        if (sizeChanged) {
            shouldInvalidateScreenMetrics = true;
            shouldInvalidateImGui = true;
            shouldRequestRecalc = true;
            shouldResetGameTexture = true;
            sizeMayHaveChanged = true;
        } else {
            shouldInvalidateScreenMetrics = true;
            shouldRequestRecalc = true;
        }
        break;
    }

    case WM_DPICHANGED:
    case WM_DISPLAYCHANGE:
        shouldInvalidateScreenMetrics = true;
        shouldInvalidateImGui = true;
        shouldRequestRecalc = true;
        sizeMayHaveChanged = true;
        break;

    default:
        return;
    }

    bool clientSizeChanged = false;
    if (sizeMayHaveChanged) {
        const int prevW = GetCachedWindowWidth();
        const int prevH = GetCachedWindowHeight();

        int clientW = 0;
        int clientH = 0;
        if (TryGetClientSize(hWnd, clientW, clientH)) {
            const bool windowActive = g_gameWindowActive.load(std::memory_order_relaxed);
            const bool isInactiveTinySize = !windowActive && (clientW < kInactiveTransientClientMin || clientH < kInactiveTransientClientMin);
            if (!isInactiveTinySize) {
                RECT monitorRect{};
                const bool haveMonitorRect = GetMonitorRectForWindow(hWnd, monitorRect);
                const int monitorW = haveMonitorRect ? (monitorRect.right - monitorRect.left) : 0;
                const int monitorH = haveMonitorRect ? (monitorRect.bottom - monitorRect.top) : 0;
                const bool previousSizeFilledMonitor = haveMonitorRect && prevW >= (monitorW - kFullscreenTolPx) && prevH >= (monitorH - kFullscreenTolPx);
                const bool currentSizeIsWindowed = haveMonitorRect && (clientW < (monitorW - kFullscreenTolPx) || clientH < (monitorH - kFullscreenTolPx));
                if (g_config.restoreWindowedModeOnFullscreenExit && previousSizeFilledMonitor && currentSizeIsWindowed) {
                    if (CenterWindowedRestoreOnCurrentMonitor(hWnd, "input_hook:fullscreen_exit_restore")) { return; }
                }

                clientSizeChanged = (clientW != prevW) || (clientH != prevH);
                UpdateCachedWindowMetricsFromSize(clientW, clientH);
            }
        }

        if (clientSizeChanged) {
            // Record pending resize for debounced virtual camera update
            int vcClientW = 0, vcClientH = 0;
            if (TryGetClientSize(hWnd, vcClientW, vcClientH)) {
                OnGameWindowResized(static_cast<uint32_t>(vcClientW), static_cast<uint32_t>(vcClientH));
            }
        } else {
            // No-op: resize tracking removed
        }
    }

    if (shouldInvalidateScreenMetrics) { InvalidateCachedScreenMetrics(); }
    if (shouldRequestRecalc) { RequestScreenMetricsRecalculation(); }
    if (shouldInvalidateImGui) { InvalidateImGuiCache(); }
    if (shouldResetGameTexture && clientSizeChanged) { InvalidateTrackedGameTextureId(false, false); }
    if (clientSizeChanged) { shouldRecenterGui = true; }

    if (clientSizeChanged) {
        ResendCurrentModeWmSize(hWnd, "input_hook:external_resize");
    }
    if (shouldRecenterGui) { g_guiNeedsRecenter = true; }
}

InputHandlerResult HandleMouseMoveViewportOffset(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM& lParam) {
    if (uMsg != WM_MOUSEMOVE) { return { false, 0 }; }
    PROFILE_SCOPE("HandleMouseMoveViewportOffset");
    // Legacy compatibility path intentionally disabled.
    // Mouse translation is handled centrally in HandleMouseCoordinateTranslationPhase.
    (void)hWnd;
    (void)uMsg;
    (void)wParam;
    (void)lParam;
    return { false, 0 };
}

InputHandlerResult HandleShutdownCheck(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    PROFILE_SCOPE("HandleShutdownCheck");

    if (g_isShuttingDown.load() && g_originalWndProc) { return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) }; }
    return { false, 0 };
}

InputHandlerResult HandleWindowValidation(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    PROFILE_SCOPE("HandleWindowValidation");

    if (g_subclassedHwnd.load() != hWnd) {
        Log("WARNING: SubclassedWndProc called for unexpected window " + std::to_string(reinterpret_cast<uintptr_t>(hWnd)) + " (expected " +
            std::to_string(reinterpret_cast<uintptr_t>(g_subclassedHwnd.load())) + ")");
        if (g_originalWndProc) { return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) }; }
        return { true, DefWindowProc(hWnd, uMsg, wParam, lParam) };
    }
    return { false, 0 };
}

static UINT GetToolscreenIsInstalledMessageId() {
    static const UINT s_msg = RegisterWindowMessageA("Toolscreen_IsInstalled");
    return s_msg;
}

static UINT GetToolscreenGetVersionMessageId() {
    static const UINT s_msg = RegisterWindowMessageA("Toolscreen_GetVersion");
    return s_msg;
}

static LRESULT EncodeToolscreenVersionNumber() {
    // 0x00MMmmpp (major/minor/patch in 8-bit fields)
    return static_cast<LRESULT>(((TOOLSCREEN_VERSION_MAJOR & 0xFF) << 16) |
                                ((TOOLSCREEN_VERSION_MINOR & 0xFF) << 8) |
                                (TOOLSCREEN_VERSION_PATCH & 0xFF));
}

InputHandlerResult HandleToolscreenQueryMessages(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    const UINT isInstalledMsg = GetToolscreenIsInstalledMessageId();
    const UINT getVersionMsg = GetToolscreenGetVersionMessageId();
    const UINT borderlessToggleMsg = GetToolscreenBorderlessToggleMessageId();
    if (uMsg != isInstalledMsg && uMsg != getVersionMsg && uMsg != borderlessToggleMsg) { return { false, 0 }; }
    PROFILE_SCOPE("HandleToolscreenQueryMessages");
    (void)hWnd;
    (void)wParam;
    (void)lParam;

    if (uMsg == borderlessToggleMsg) {
        ToggleBorderlessWindowedFullscreen(hWnd);
        return { true, 1 };
    }

    if (uMsg == isInstalledMsg) {
        return { true, 1 };
    }

    if (uMsg == getVersionMsg) {
        return { true, EncodeToolscreenVersionNumber() };
    }

    return { false, 0 };
}

InputHandlerResult HandleNonFullscreenCheck(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    PROFILE_SCOPE("HandleNonFullscreenCheck");

    // Windowed mode is supported; do not bypass the hook pipeline.
    return { false, 0 };
}

void HandleCharLogging(UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg != WM_CHAR) { return; }

    auto cfgSnap = GetConfigSnapshot();
    if (cfgSnap && cfgSnap->debug.showHotkeyDebug) {
        Log("WM_CHAR: " + std::to_string(wParam) + " " + std::to_string(lParam));
    }
}

InputHandlerResult HandleAltF4(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg != WM_SYSKEYDOWN) { return { false, 0 }; }
    PROFILE_SCOPE("HandleAltF4");

    auto cfgSnap = GetConfigSnapshot();
    if (!cfgSnap || !cfgSnap->keyRebinds.enabled || !cfgSnap->keyRebinds.allowSystemAltF4) {
        return { false, 0 };
    }

    if (wParam == VK_F4 && IsAltCurrentlyDown()) { return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) }; }
    return { false, 0 };
}

InputHandlerResult HandleConfigLoadFailure(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    PROFILE_SCOPE("HandleConfigLoadFailure");

    if (!g_configLoadFailed.load()) { return { false, 0 }; }

    if (ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam)) { return { true, true }; }

    switch (uMsg) {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case WM_MOUSEWHEEL:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
    case WM_INPUT:
        return { true, 1 };
    default:
        break;
    }
    return { false, 0 };
}

InputHandlerResult HandleSetCursor(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, const std::string& gameState) {
    if (uMsg != WM_SETCURSOR) { return { false, 0 }; }
    PROFILE_SCOPE("HandleSetCursor");

    if (g_showGui.load() && g_forceVisibleCursorWhileGuiOpen.load(std::memory_order_acquire) &&
        g_gameVersion >= GameVersion(1, 13, 0)) {
        EnsureSystemCursorVisible();
        static HCURSOR s_arrowCursor = LoadCursorW(NULL, IDC_ARROW);
        SetCursor(s_arrowCursor);
        return { true, true };
    }

    if (!IsCursorVisible() && !g_showGui.load()) {
        SetCursor(NULL);
        return { true, true };
    }

    const CursorTextures::CursorData* cursorData = CursorTextures::GetSelectedCursor(gameState, 64);
    if (cursorData && cursorData->hCursor) {
        SetCursor(cursorData->hCursor);
        return { true, true };
    }
    return { false, 0 };
}

InputHandlerResult HandleDestroy(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg != WM_DESTROY) { return { false, 0 }; }
    PROFILE_SCOPE("HandleDestroy");

    ResetLocalKeyRepeatState(hWnd);
    ResetShiftHotkeyPollingState(hWnd);
    ReleaseActiveLowLevelRebindKeys(hWnd);

    extern GameVersion g_gameVersion;
    if (g_gameVersion >= GameVersion(1, 13, 0)) { g_isShuttingDown = true; }
    UpdateLowLevelKeyboardHookInstalledState();
    return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
}

InputHandlerResult HandleImGuiInput(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    PROFILE_SCOPE("HandleImGuiInput");

    if (!g_showGui.load()) { return { false, 0 }; }

    // Never call ImGui from this thread.
    // Instead, enqueue the message for the render thread (which owns the ImGui context).
    ImGuiInputQueue_EnqueueWin32Message(hWnd, uMsg, wParam, lParam);
    return { false, 0 };
}

InputHandlerResult HandleGuiToggle(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
        break;
    default:
        return { false, 0 };
    }
    PROFILE_SCOPE("HandleGuiToggle");

    ScopedExactKeyboardMessageState scopedKeyboardMessageState(uMsg, wParam, lParam);

    const bool isAutoRepeatKeyDown = IsTrackedExactAutoRepeatKeyDown(uMsg, wParam, lParam);

    DWORD vkCode = 0;
    bool isEscape = false;
    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        isEscape = (wParam == VK_ESCAPE);
        vkCode = ResolveEffectiveKeyboardVkForMessage(uMsg, wParam, lParam);
        break;
    }
    case WM_LBUTTONDOWN:
        vkCode = VK_LBUTTON;
        break;
    case WM_RBUTTONDOWN:
        vkCode = VK_RBUTTON;
        break;
    case WM_MBUTTONDOWN:
        vkCode = VK_MBUTTON;
        break;
    case WM_XBUTTONDOWN: {
        WORD xButton = GET_XBUTTON_WPARAM(wParam);
        vkCode = (xButton == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
        break;
    }
    default:
        return { false, 0 };
    }

    if (!isEscape && !CheckHotkeyMatch(g_config.guiHotkey, vkCode, {}, false, s_bestMatchKeyCount)) { return { false, 0 }; }

    if (!isEscape && ShouldMaskWindowsKeyForHotkey(g_config.guiHotkey, true, isAutoRepeatKeyDown)) {
        (void)SendMenuMaskKeyTap();
    }

    if (g_showGui.load(std::memory_order_acquire) && !isEscape) {
        switch (uMsg) {
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN:
            return { false, 0 };
        default:
            break;
        }
    }

    bool allow_toggle = true;
    if (isEscape && !g_showGui.load()) { allow_toggle = false; }

    if (!allow_toggle) { return { false, 0 }; }

    // Lock-free debouncing using atomic timestamp
    auto now = std::chrono::steady_clock::now();
    int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    int64_t lastMs = g_lastGuiToggleTimeMs.load(std::memory_order_relaxed);
    if (nowMs - lastMs < 200) {
        return { true, 1 };
    }
    g_lastGuiToggleTimeMs.store(nowMs, std::memory_order_relaxed);

    if (!g_glInitialized.load(std::memory_order_acquire)) {
        Log("GUI toggle ignored - OpenGL not initialized yet");
        return { true, 1 };
    }

    bool is_closing = g_showGui.load();

    if (isEscape && g_imguiAnyItemActive.load(std::memory_order_acquire)) { is_closing = false; }
    if (isEscape && IsHotkeyBindingActive()) { is_closing = false; }
    if (isEscape && IsRebindBindingActive()) { is_closing = false; }
    if (isEscape && IsMirrorColorPickerPopupActive()) { is_closing = false; }

    if (is_closing) {
        CloseSettingsGuiWindow();
    } else if (!isEscape) {
        g_showGui = true;
        InvalidateImGuiCache();
        const bool wasCursorVisible = IsCursorVisible();
        g_wasCursorVisible = wasCursorVisible;
        g_forceVisibleCursorWhileGuiOpen.store(false, std::memory_order_release);
        g_guiNeedsRecenter = true;
        if (!ApplyConfineCursorToGameWindow()) {
            ClipCursor(NULL);
        }
        if (!wasCursorVisible && g_gameVersion >= GameVersion(1, 13, 0)) {
            g_forceVisibleCursorWhileGuiOpen.store(true, std::memory_order_release);
            EnsureSystemCursorVisible();
            static HCURSOR s_arrowCursor = LoadCursorW(NULL, IDC_ARROW);
            SetCursor(s_arrowCursor);
        }

        g_configurePromptDismissedThisSession.store(true, std::memory_order_release);

        if (!g_toolscreenPath.empty()) {
            std::wstring flagPath = g_toolscreenPath + L"\\has_opened";
            HANDLE hFile = CreateFileW(flagPath.c_str(), GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hFile != INVALID_HANDLE_VALUE) { CloseHandle(hFile); }
        }
    }
    return { true, 1 };
}

InputHandlerResult HandleBorderlessToggle(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
        break;
    default:
        return { false, 0 };
    }
    PROFILE_SCOPE("HandleBorderlessToggle");

    ScopedExactKeyboardMessageState scopedKeyboardMessageState(uMsg, wParam, lParam);

    const bool isAutoRepeatKeyDown = IsTrackedExactAutoRepeatKeyDown(uMsg, wParam, lParam);

    if (g_showGui.load(std::memory_order_acquire)) { return { false, 0 }; }

    if (g_config.borderlessHotkey.empty()) { return { false, 0 }; }

    if (IsHotkeyBindingActive() || IsRebindBindingActive()) { return { false, 0 }; }

    DWORD vkCode = 0;
    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        vkCode = ResolveEffectiveKeyboardVkForMessage(uMsg, wParam, lParam);
        break;
    }
    case WM_LBUTTONDOWN:
        vkCode = VK_LBUTTON;
        break;
    case WM_RBUTTONDOWN:
        vkCode = VK_RBUTTON;
        break;
    case WM_MBUTTONDOWN:
        vkCode = VK_MBUTTON;
        break;
    case WM_XBUTTONDOWN: {
        WORD xButton = GET_XBUTTON_WPARAM(wParam);
        vkCode = (xButton == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
        break;
    }
    default:
        return { false, 0 };
    }

    if (!CheckHotkeyMatch(g_config.borderlessHotkey, vkCode, {}, false, s_bestMatchKeyCount)) { return { false, 0 }; }

    if (ShouldMaskWindowsKeyForHotkey(g_config.borderlessHotkey, true, isAutoRepeatKeyDown)) {
        (void)SendMenuMaskKeyTap();
    }

    static std::atomic<int64_t> s_lastToggleMs{ 0 };
    auto now = std::chrono::steady_clock::now();
    int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    int64_t lastMs = s_lastToggleMs.load(std::memory_order_relaxed);
    if (nowMs - lastMs < 250) { return { true, 1 }; }
    s_lastToggleMs.store(nowMs, std::memory_order_relaxed);

    ToggleBorderlessWindowedFullscreen(hWnd);
    return { true, 1 };
}

InputHandlerResult HandleImageOverlaysToggle(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
        break;
    default:
        return { false, 0 };
    }
    PROFILE_SCOPE("HandleImageOverlaysToggle");

    ScopedExactKeyboardMessageState scopedKeyboardMessageState(uMsg, wParam, lParam);

    const bool isAutoRepeatKeyDown = IsTrackedExactAutoRepeatKeyDown(uMsg, wParam, lParam);

    if (g_config.imageOverlaysHotkey.empty()) { return { false, 0 }; }

    if (IsHotkeyBindingActive() || IsRebindBindingActive()) { return { false, 0 }; }

    DWORD vkCode = 0;
    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        vkCode = ResolveEffectiveKeyboardVkForMessage(uMsg, wParam, lParam);
        break;
    }
    case WM_LBUTTONDOWN:
        vkCode = VK_LBUTTON;
        break;
    case WM_RBUTTONDOWN:
        vkCode = VK_RBUTTON;
        break;
    case WM_MBUTTONDOWN:
        vkCode = VK_MBUTTON;
        break;
    case WM_XBUTTONDOWN: {
        WORD xButton = GET_XBUTTON_WPARAM(wParam);
        vkCode = (xButton == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
        break;
    }
    default:
        return { false, 0 };
    }

    if (!CheckHotkeyMatch(g_config.imageOverlaysHotkey, vkCode, {}, false, s_bestMatchKeyCount)) { return { false, 0 }; }

    if (ShouldMaskWindowsKeyForHotkey(g_config.imageOverlaysHotkey, true, isAutoRepeatKeyDown)) {
        (void)SendMenuMaskKeyTap();
    }

    static std::atomic<int64_t> s_lastToggleMs{ 0 };
    auto now = std::chrono::steady_clock::now();
    int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    int64_t lastMs = s_lastToggleMs.load(std::memory_order_relaxed);
    if (nowMs - lastMs < 250) { return { true, 1 }; }
    s_lastToggleMs.store(nowMs, std::memory_order_relaxed);

    bool newVisible = !g_imageOverlaysVisible.load(std::memory_order_acquire);
    g_imageOverlaysVisible.store(newVisible, std::memory_order_release);

    return { true, 1 };
}

InputHandlerResult HandleWindowOverlaysToggle(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
        break;
    default:
        return { false, 0 };
    }
    PROFILE_SCOPE("HandleWindowOverlaysToggle");

    ScopedExactKeyboardMessageState scopedKeyboardMessageState(uMsg, wParam, lParam);

    const bool isAutoRepeatKeyDown = IsTrackedExactAutoRepeatKeyDown(uMsg, wParam, lParam);

    if (g_config.windowOverlaysHotkey.empty()) { return { false, 0 }; }

    if (IsHotkeyBindingActive() || IsRebindBindingActive()) { return { false, 0 }; }

    DWORD vkCode = 0;
    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        vkCode = ResolveEffectiveKeyboardVkForMessage(uMsg, wParam, lParam);
        break;
    }
    case WM_LBUTTONDOWN:
        vkCode = VK_LBUTTON;
        break;
    case WM_RBUTTONDOWN:
        vkCode = VK_RBUTTON;
        break;
    case WM_MBUTTONDOWN:
        vkCode = VK_MBUTTON;
        break;
    case WM_XBUTTONDOWN: {
        WORD xButton = GET_XBUTTON_WPARAM(wParam);
        vkCode = (xButton == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
        break;
    }
    default:
        return { false, 0 };
    }

    if (!CheckHotkeyMatch(g_config.windowOverlaysHotkey, vkCode, {}, false, s_bestMatchKeyCount)) { return { false, 0 }; }

    if (ShouldMaskWindowsKeyForHotkey(g_config.windowOverlaysHotkey, true, isAutoRepeatKeyDown)) {
        (void)SendMenuMaskKeyTap();
    }

    static std::atomic<int64_t> s_lastToggleMs{ 0 };
    auto now = std::chrono::steady_clock::now();
    int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    int64_t lastMs = s_lastToggleMs.load(std::memory_order_relaxed);
    if (nowMs - lastMs < 250) { return { true, 1 }; }
    s_lastToggleMs.store(nowMs, std::memory_order_relaxed);

    bool newVisible = !g_windowOverlaysVisible.load(std::memory_order_acquire);
    g_windowOverlaysVisible.store(newVisible, std::memory_order_release);

    if (!newVisible) {
        UnfocusWindowOverlay();
    }

    return { true, 1 };
}

InputHandlerResult HandleNinjabrainOverlayToggle(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
        break;
    default:
        return { false, 0 };
    }
    PROFILE_SCOPE("HandleNinjabrainOverlayToggle");

    ScopedExactKeyboardMessageState scopedKeyboardMessageState(uMsg, wParam, lParam);

    const bool isAutoRepeatKeyDown = IsTrackedExactAutoRepeatKeyDown(uMsg, wParam, lParam);

    if (g_config.ninjabrainOverlayHotkey.empty()) { return { false, 0 }; }

    if (IsHotkeyBindingActive() || IsRebindBindingActive()) { return { false, 0 }; }

    DWORD vkCode = 0;
    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        vkCode = ResolveEffectiveKeyboardVkForMessage(uMsg, wParam, lParam);
        break;
    }
    case WM_LBUTTONDOWN:
        vkCode = VK_LBUTTON;
        break;
    case WM_RBUTTONDOWN:
        vkCode = VK_RBUTTON;
        break;
    case WM_MBUTTONDOWN:
        vkCode = VK_MBUTTON;
        break;
    case WM_XBUTTONDOWN: {
        WORD xButton = GET_XBUTTON_WPARAM(wParam);
        vkCode = (xButton == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
        break;
    }
    default:
        return { false, 0 };
    }

    if (!CheckHotkeyMatch(g_config.ninjabrainOverlayHotkey, vkCode, {}, false, s_bestMatchKeyCount)) { return { false, 0 }; }

    if (ShouldMaskWindowsKeyForHotkey(g_config.ninjabrainOverlayHotkey, true, isAutoRepeatKeyDown)) {
        (void)SendMenuMaskKeyTap();
    }

    static std::atomic<int64_t> s_lastToggleMs{ 0 };
    auto now = std::chrono::steady_clock::now();
    int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    int64_t lastMs = s_lastToggleMs.load(std::memory_order_relaxed);
    if (nowMs - lastMs < 250) { return { true, 1 }; }
    s_lastToggleMs.store(nowMs, std::memory_order_relaxed);

    bool newVisible = !g_ninjabrainOverlayVisible.load(std::memory_order_acquire);
    g_ninjabrainOverlayVisible.store(newVisible, std::memory_order_release);

    return { true, 1 };
}

InputHandlerResult HandleKeyRebindsToggle(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
        break;
    default:
        return { false, 0 };
    }
    PROFILE_SCOPE("HandleKeyRebindsToggle");

    ScopedExactKeyboardMessageState scopedKeyboardMessageState(uMsg, wParam, lParam);

    const bool isAutoRepeatKeyDown = IsTrackedExactAutoRepeatKeyDown(uMsg, wParam, lParam);

    if (g_config.keyRebinds.toggleHotkey.empty()) { return { false, 0 }; }

    if (IsHotkeyBindingActive() || IsRebindBindingActive()) { return { false, 0 }; }

    DWORD vkCode = 0;
    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        vkCode = ResolveEffectiveKeyboardVkForMessage(uMsg, wParam, lParam);
        break;
    }
    case WM_LBUTTONDOWN:
        vkCode = VK_LBUTTON;
        break;
    case WM_RBUTTONDOWN:
        vkCode = VK_RBUTTON;
        break;
    case WM_MBUTTONDOWN:
        vkCode = VK_MBUTTON;
        break;
    case WM_XBUTTONDOWN: {
        WORD xButton = GET_XBUTTON_WPARAM(wParam);
        vkCode = (xButton == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
        break;
    }
    default:
        return { false, 0 };
    }

    if (!CheckHotkeyMatch(g_config.keyRebinds.toggleHotkey, vkCode, {}, false, s_bestMatchKeyCount)) { return { false, 0 }; }

    if (ShouldMaskWindowsKeyForHotkey(g_config.keyRebinds.toggleHotkey, true, isAutoRepeatKeyDown)) {
        (void)SendMenuMaskKeyTap();
    }

    static std::atomic<int64_t> s_lastToggleMs{ 0 };
    auto now = std::chrono::steady_clock::now();
    int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    int64_t lastMs = s_lastToggleMs.load(std::memory_order_relaxed);
    if (nowMs - lastMs < 250) { return { true, 1 }; }
    s_lastToggleMs.store(nowMs, std::memory_order_relaxed);

    if (g_config.keyRebinds.enabled) {
        ReleaseActiveLowLevelRebindKeys(hWnd);
    }

    g_config.keyRebinds.enabled = !g_config.keyRebinds.enabled;
    g_configIsDirty = true;

    {
        std::lock_guard<std::mutex> hotkeyLock(g_hotkeyMainKeysMutex);
        RebuildHotkeyMainKeys_Internal();
    }

    PublishConfigSnapshot();

    (void)hWnd;
    return { true, 1 };
}

InputHandlerResult HandleWindowOverlayKeyboard(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg != WM_KEYDOWN && uMsg != WM_KEYUP && uMsg != WM_SYSKEYDOWN && uMsg != WM_SYSKEYUP) { return { false, 0 }; }
    PROFILE_SCOPE("HandleWindowOverlayKeyboard");

    if (!g_windowOverlaysVisible.load(std::memory_order_acquire)) { return { false, 0 }; }

    bool isOverlayInteractionActive = IsWindowOverlayFocused();

    if (!isOverlayInteractionActive) { return { false, 0 }; }

    // Never query ImGui from this thread. Use state published by render thread.
    bool imguiWantsKeyboard = g_showGui.load() && g_imguiWantCaptureKeyboard.load(std::memory_order_acquire);

    if (!imguiWantsKeyboard) {
        if (ForwardKeyboardToWindowOverlay(uMsg, wParam, lParam)) { return { true, 1 }; }
    }
    return { false, 0 };
}

InputHandlerResult HandleWindowOverlayMouse(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg < WM_MOUSEFIRST || uMsg > WM_MOUSELAST) { return { false, 0 }; }
    PROFILE_SCOPE("HandleWindowOverlayMouse");

    if (!g_windowOverlaysVisible.load(std::memory_order_acquire)) { return { false, 0 }; }

    int mouseX, mouseY;

    // WM_MOUSEWHEEL and WM_MOUSEHWHEEL use SCREEN coordinates in lParam, not client coordinates
    if (uMsg == WM_MOUSEWHEEL || uMsg == WM_MOUSEHWHEEL) {
        POINT cursorPos;
        GetCursorPos(&cursorPos);
        if (ScreenToClient(hWnd, &cursorPos)) {
            mouseX = cursorPos.x;
            mouseY = cursorPos.y;
        } else {
            mouseX = cursorPos.x;
            mouseY = cursorPos.y;
        }
    } else {
        mouseX = GET_X_LPARAM(lParam);
        mouseY = GET_Y_LPARAM(lParam);
    }

    const int screenW = GetCachedWindowWidth();
    const int screenH = GetCachedWindowHeight();

    bool isOverlayInteractionActive = IsWindowOverlayFocused();

    if (isOverlayInteractionActive) {
        if (uMsg == WM_LBUTTONDOWN || uMsg == WM_RBUTTONDOWN || uMsg == WM_MBUTTONDOWN) {
            std::string focusedName = GetFocusedWindowOverlayName();
            std::string overlayAtPoint = GetWindowOverlayAtPoint(mouseX, mouseY, screenW, screenH);

            if (overlayAtPoint.empty() || overlayAtPoint != focusedName) {
                UnfocusWindowOverlay();
                if (!overlayAtPoint.empty()) {
                    FocusWindowOverlay(overlayAtPoint);
                    ForwardMouseToWindowOverlay(uMsg, mouseX, mouseY, wParam, screenW, screenH);
                    return { true, 1 };
                }
            } else {
                ForwardMouseToWindowOverlay(uMsg, mouseX, mouseY, wParam, screenW, screenH);
                return { true, 1 };
            }
        } else {
            ForwardMouseToWindowOverlay(uMsg, mouseX, mouseY, wParam, screenW, screenH);
            return { true, 1 };
        }
    } else if ((uMsg == WM_LBUTTONDOWN || uMsg == WM_RBUTTONDOWN || uMsg == WM_MBUTTONDOWN)) {
        const bool cursorVisible = IsCursorVisible();
        if (!(g_showGui.load() || cursorVisible)) {
            return { false, 0 };
        }
        std::string overlayAtPoint = GetWindowOverlayAtPoint(mouseX, mouseY, screenW, screenH);
        if (!overlayAtPoint.empty()) {
            FocusWindowOverlay(overlayAtPoint);
            ForwardMouseToWindowOverlay(uMsg, mouseX, mouseY, wParam, screenW, screenH);
            return { true, 1 };
        }
    }
    return { false, 0 };
}

InputHandlerResult HandleGuiInputBlocking(UINT uMsg) {
    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_CHAR:
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case WM_MOUSEWHEEL:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
    case WM_INPUT:
        break;
    default:
        return { false, 0 };
    }

    PROFILE_SCOPE("HandleGuiInputBlocking");

    if (!g_showGui.load()) { return { false, 0 }; }

    return { true, 1 };
}

void RestoreWindowsMouseSpeed();
void ApplyWindowsMouseSpeed();
void RestoreKeyRepeatSettings();
void ApplyKeyRepeatSettings();

InputHandlerResult HandleActivate(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    bool becameInactive = false;
    bool becameActive = false;
    const char* focusSource = nullptr;

    switch (uMsg) {
    case WM_ACTIVATE:
        becameInactive = (LOWORD(wParam) == WA_INACTIVE);
        becameActive = !becameInactive;
        focusSource = becameActive ? "input_hook:focus_regain_activate" : "input_hook:focus_loss_activate";
        break;

    case WM_ACTIVATEAPP:
        becameActive = (wParam != FALSE);
        becameInactive = !becameActive;
        focusSource = becameActive ? "input_hook:focus_regain_activateapp" : "input_hook:focus_loss_activateapp";
        break;

    case WM_SETFOCUS:
        becameActive = true;
        focusSource = "input_hook:focus_regain_setfocus";
        break;

    case WM_KILLFOCUS:
        becameInactive = true;
        focusSource = "input_hook:focus_loss_killfocus";
        break;

    default:
        return { false, 0 };
    }

    PROFILE_SCOPE("HandleActivate");
    (void)lParam;

    const bool ownsForeground = becameActive && IsWindowInForegroundTree(hWnd);

    if (becameInactive) {
        ImGuiInputQueue_EnqueueFocus(false);

        ResetExactKeyboardMessageState();
        ResetLowLevelExactModifierState();
        ResetLocalKeyRepeatState(hWnd);
        ReleaseActiveLowLevelRebindKeys(hWnd);

        if (auto cs = GetConfigSnapshot(); cs && cs->debug.showHotkeyDebug) {
            Log(std::string("[WINDOW] Window became inactive via ") + focusSource + ".");
        }
        extern std::atomic<bool> g_isGameFocused;
        g_isGameFocused.store(false);
        g_gameWindowActive.store(false);

        if (g_config.confineCursor) {
            ClipCursorDirect(NULL);
        }
        RestoreWindowsMouseSpeed();
        RestoreKeyRepeatSettings();
        UpdateLowLevelKeyboardHookInstalledState();
    } else {
        ImGuiInputQueue_EnqueueFocus(ownsForeground);

        if (auto cs = GetConfigSnapshot(); cs && cs->debug.showHotkeyDebug) {
            Log(std::string("[WINDOW] Window became active via ") + focusSource + ".");
        }
        extern std::atomic<bool> g_isGameFocused;
        g_isGameFocused.store(ownsForeground);
        g_gameWindowActive.store(ownsForeground);

        if (!ownsForeground) {
            UpdateLowLevelKeyboardHookInstalledState();
            return { false, 0 };
        }

        ApplyWindowsMouseSpeed();
        ApplyKeyRepeatSettings();
        ApplyConfineCursorToGameWindow();
        UpdateLowLevelKeyboardHookInstalledState();

        if (IsCursorVisible() && g_gameVersion >= GameVersion(1, 13, 0)) {
            EnsureSystemCursorVisible();
            static HCURSOR s_arrowCursor = LoadCursorW(NULL, IDC_ARROW);
            SetCursor(s_arrowCursor);
        }

        int clientW = 0;
        int clientH = 0;
        if (TryGetClientSize(hWnd, clientW, clientH)) {
            UpdateCachedWindowMetricsFromSize(clientW, clientH);
        }

        RequestScreenMetricsRecalculation();
        InvalidateImGuiCache();
        g_guiNeedsRecenter = true;
        s_deferredFocusRegainWmSizePending.store(true, std::memory_order_relaxed);
    }
    return { false, 0 };
}

InputHandlerResult HandleWmSizeModeDimensions(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, const std::string& currentModeId) {
    if (uMsg != WM_SIZE) { return { false, 0 }; }
    PROFILE_SCOPE("HandleWmSizeModeDimensions");

    if (wParam == SIZE_MINIMIZED) { return { false, 0 }; }

    const int msgW = LOWORD(lParam);
    const int msgH = HIWORD(lParam);
    if (msgW <= 0 || msgH <= 0) { return { false, 0 }; }

    auto cfgSnap = GetConfigSnapshot();
    const ModeConfig* mode = cfgSnap ? GetModeFromSnapshotOrFallback(*cfgSnap, currentModeId) : nullptr;
    if (!mode || mode->width <= 0 || mode->height <= 0) { return { false, 0 }; }

    int liveClientW = 0;
    int liveClientH = 0;
    const bool haveLiveClientSize = TryGetClientSize(hWnd, liveClientW, liveClientH);
    const bool messageMatchesLiveClient = haveLiveClientSize && liveClientW == msgW && liveClientH == msgH;

    int targetW = mode->width;
    int targetH = mode->height;

    if (EqualsIgnoreCase(mode->id, "Fullscreen")) {
        if (!messageMatchesLiveClient) {
            // Toolscreen-posted WM_SIZE already carries the computed internal render size.
            // Recomputing from the live client here would compound relative sizing.
            return { false, 0 };
        }

        // OS WM_SIZE reports the live client area after a real resize/maximize. Rewrite it
        // to the computed internal render size immediately so the game never races ahead
        // with the raw client dimensions.
        targetW = ResolveModeDisplayWidth(*mode, liveClientW, liveClientH);
        targetH = ResolveModeDisplayHeight(*mode, liveClientW, liveClientH);
    } else {
        // IMPORTANT: use already-recalculated mode dimensions as authoritative target.
        // Re-applying relative/expression math against WM_SIZE repeatedly causes compounding
        // shrink (e.g. 98.4% of 900 -> 885, then 98.4% of 885 -> 870).
        targetW = mode->width;
        targetH = mode->height;
    }

    if (targetW <= 0 || targetH <= 0 || (msgW == targetW && msgH == targetH)) { return { false, 0 }; }

    RememberRequestedWindowClientResize(targetW, targetH);
    const LPARAM adjustedSize = MAKELPARAM(targetW, targetH);
    InvalidateTrackedGameTextureId(false, false);
    LRESULT forwarded = CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, adjustedSize);

    return { true, forwarded };
}

InputHandlerResult HandleHotkeys(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, const std::string& currentModeId,
                                 const std::string& gameState) {
    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        break;
    default:
        return { false, 0 };
    }
    PROFILE_SCOPE("HandleHotkeys");

    ScopedExactKeyboardMessageState scopedKeyboardMessageState(uMsg, wParam, lParam);

    DWORD rawVkCode = 0;
    DWORD vkCode = 0;
    bool isKeyDown = false;

    if (uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN) {
        rawVkCode = static_cast<DWORD>(wParam);
        isKeyDown = true;
    } else if (uMsg == WM_KEYUP || uMsg == WM_SYSKEYUP) {
        rawVkCode = static_cast<DWORD>(wParam);
        isKeyDown = false;
    } else if (uMsg == WM_XBUTTONDOWN) {
        WORD xButton = GET_XBUTTON_WPARAM(wParam);
        rawVkCode = (xButton == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
        isKeyDown = true;
    } else if (uMsg == WM_XBUTTONUP) {
        WORD xButton = GET_XBUTTON_WPARAM(wParam);
        rawVkCode = (xButton == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
        isKeyDown = false;
    } else if (uMsg == WM_LBUTTONDOWN) {
        rawVkCode = VK_LBUTTON;
        isKeyDown = true;
    } else if (uMsg == WM_LBUTTONUP) {
        rawVkCode = VK_LBUTTON;
        isKeyDown = false;
    } else if (uMsg == WM_RBUTTONDOWN) {
        rawVkCode = VK_RBUTTON;
        isKeyDown = true;
    } else if (uMsg == WM_RBUTTONUP) {
        rawVkCode = VK_RBUTTON;
        isKeyDown = false;
    } else if (uMsg == WM_MBUTTONDOWN) {
        rawVkCode = VK_MBUTTON;
        isKeyDown = true;
    } else if (uMsg == WM_MBUTTONUP) {
        rawVkCode = VK_MBUTTON;
        isKeyDown = false;
    } else {
        return { false, 0 };
    }

    const bool isAutoRepeatKeyDown = isKeyDown && IsTrackedExactAutoRepeatKeyDown(uMsg, wParam, lParam);

    // This mirrors imgui_impl_win32 behavior and enables reliable hotkeys + key rebinding.
    vkCode = rawVkCode;
    if (uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN || uMsg == WM_KEYUP || uMsg == WM_SYSKEYUP) {
        vkCode = ResolveEffectiveKeyboardVkForMessage(uMsg, wParam, lParam);
    }

    // Even if resolution-change features are unsupported, we must not short-circuit the input pipeline.
    if (!IsResolutionChangeSupported(g_gameVersion)) { return { false, 0 }; }

    bool isHotkeyMainKey = false;
    {
        std::lock_guard<std::mutex> hotkeyLock(g_hotkeyMainKeysMutex);
        isHotkeyMainKey = (g_hotkeyMainKeys.find(rawVkCode) != g_hotkeyMainKeys.end()) ||
                          (g_hotkeyMainKeys.find(vkCode) != g_hotkeyMainKeys.end());
    }

    const bool hasActiveHoldHotkey = s_activeHoldHotkey.has_value();

    if (!isHotkeyMainKey && !hasActiveHoldHotkey) {
        // This key is not a hotkey main key, but it might invalidate pending trigger-on-release hotkeys
        if (isKeyDown) {
            std::lock_guard<std::mutex> lock(g_triggerOnReleaseMutex);
            // Any key press (that's not a hotkey) invalidates ALL pending trigger-on-release hotkeys
            for (const auto& pendingHotkeyId : g_triggerOnReleasePending) { g_triggerOnReleaseInvalidated.insert(pendingHotkeyId); }
        }
        // Do not return "consumed" here.
        return { false, 0 };
    }

    // Use config snapshot for thread-safe hotkey iteration
    auto cfgSnap = GetConfigSnapshot();
    if (!cfgSnap) { return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) }; }
    const Config& cfg = *cfgSnap;

    DWORD rebindTargetVk = 0;
    const bool cursorVisible = (cfg.keyRebinds.enabled && cfg.keyRebinds.resolveRebindTargetsForHotkeys) ? IsCursorVisible() : false;
    if (cfg.keyRebinds.enabled && cfg.keyRebinds.resolveRebindTargetsForHotkeys) {
        const KeyRebind* matchedRebind = FindPreferredEnabledKeyRebind(
            cfg.keyRebinds.rebinds,
            cursorVisible,
            [&](const KeyRebind& rebind) { return MatchesRebindSourceKey(vkCode, rawVkCode, rebind.fromKey); });
        if (matchedRebind != nullptr) {
            const bool shiftLayerActive = IsShiftLayerActiveForRebind(*matchedRebind, vkCode, rawVkCode, isKeyDown);
            const DWORD effectiveCustomOutputVk = ResolveEffectiveCustomOutputVk(*matchedRebind, shiftLayerActive);
            rebindTargetVk = (effectiveCustomOutputVk != 0) ? effectiveCustomOutputVk : matchedRebind->toKey;
        }
    }

    bool s_enableHotkeyDebug = cfg.debug.showHotkeyDebug;

    InputHandlerResult brokenHoldResult;
    if (TryHandleBrokenHoldHotkey(hWnd, uMsg, wParam, lParam, vkCode, rawVkCode, rebindTargetVk, isKeyDown, cfg, s_enableHotkeyDebug,
                                  brokenHoldResult)) {
        if (brokenHoldResult.consumed) {
            return brokenHoldResult;
        }
        if (!isHotkeyMainKey) {
            return { false, 0 };
        }
    }

    if (!isHotkeyMainKey) {
        if (isKeyDown) {
            std::lock_guard<std::mutex> lock(g_triggerOnReleaseMutex);
            for (const auto& pendingHotkeyId : g_triggerOnReleasePending) { g_triggerOnReleaseInvalidated.insert(pendingHotkeyId); }
        }
        return { false, 0 };
    }

    if (s_enableHotkeyDebug) {
        Log("[Hotkey] Key/button pressed: " + std::to_string(vkCode) + " (raw=" + std::to_string(rawVkCode) + ") in mode: " +
            currentModeId);
    }
    if (s_enableHotkeyDebug) {
        Log("[Hotkey] Current game state: " + gameState);
        Log("[Hotkey] Evaluating " + std::to_string(cfg.hotkeys.size()) + " configured hotkeys");
    }

    for (size_t hotkeyIdx = 0; hotkeyIdx < cfg.hotkeys.size(); ++hotkeyIdx) {
        const auto& hotkey = cfg.hotkeys[hotkeyIdx];
        if (s_enableHotkeyDebug) {
            Log("[Hotkey] Checking: " + GetKeyComboString(hotkey.keys) + " (main: " + hotkey.mainMode + ", sec: " + hotkey.secondaryMode +
                ")");
        }

        bool conditionsMet = MatchesConfiguredGameStateCondition(hotkey.conditions.gameState, gameState);

        std::string currentSecMode;
        bool wouldExitToFullscreen = false;
        if (hotkey.allowExitToFullscreenRegardlessOfGameState) {
            currentSecMode = GetHotkeySecondaryMode(hotkeyIdx);
            wouldExitToFullscreen = !currentSecMode.empty() && EqualsIgnoreCase(currentModeId, currentSecMode);
        }

        if (!conditionsMet) {
            bool allowBypass = (hotkey.allowExitToFullscreenRegardlessOfGameState && wouldExitToFullscreen) ||
                               (hotkey.triggerOnHold && !isKeyDown); // Hold release must always revert
            if (!allowBypass) {
                if (s_enableHotkeyDebug) { Log("[Hotkey] SKIP: Game state conditions not met"); }
                continue;
            }
            if (s_enableHotkeyDebug) {
                Log("[Hotkey] BYPASS: Allowing exit even though game state conditions are not met");
            }
        }

        // Hold-mode helper: activate target mode on press, revert to default on release
        auto handleHoldMode = [&](const std::vector<DWORD>& holdKeys, const std::string& targetMode, const std::string& hotkeyId,
                                  bool blockKey) -> InputHandlerResult {
            if (isKeyDown) {
                if (isAutoRepeatKeyDown) {
                    if (s_enableHotkeyDebug) { Log("[Hotkey] HOLD DOWN repeat ignored: " + hotkeyId); }
                    if (blockKey) return { true, 0 };
                    return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
                }

                auto now = std::chrono::steady_clock::now();
                bool debounced = false;
                {
                    std::lock_guard<std::mutex> tsLock(g_hotkeyTimestampsMutex);
                    auto it = g_hotkeyTimestamps.find(hotkeyId);
                    if (it != g_hotkeyTimestamps.end() &&
                        std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second).count() < hotkey.debounce) {
                        debounced = true;
                    } else {
                        g_hotkeyTimestamps[hotkeyId] = now;
                    }
                }
                if (!debounced && !targetMode.empty()) {
                    ActivateHoldHotkeyState(holdKeys, hotkeyId, blockKey);
                    if (s_enableHotkeyDebug) { Log("[Hotkey] HOLD DOWN: " + hotkeyId + " -> " + targetMode); }
                    SwitchToMode(targetMode, "hotkey (hold)");
                }
            } else {
                if (s_activeHoldHotkey.has_value() && s_activeHoldHotkey->hotkeyId == hotkeyId) {
                    s_activeHoldHotkey.reset();
                }
                if (s_enableHotkeyDebug) { Log("[Hotkey] HOLD RELEASE: " + hotkeyId + " -> " + cfg.defaultMode); }
                SwitchToMode(cfg.defaultMode, "hotkey (hold release)");
            }
            if (blockKey) return { true, 0 };
            return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
        };

        for (const auto& alt : hotkey.altSecondaryModes) {
            bool skipExclusions = hotkey.triggerOnRelease || (hotkey.triggerOnHold && !isKeyDown);
            bool matched = CheckHotkeyMatch(alt.keys, vkCode, hotkey.conditions.exclusions, skipExclusions, s_bestMatchKeyCount, rawVkCode, true,
                                            isKeyDown);
            bool matchedViaRebind = !matched && rebindTargetVk &&
                                    CheckHotkeyMatch(alt.keys, rebindTargetVk, hotkey.conditions.exclusions, skipExclusions,
                                                     s_bestMatchKeyCount);
            if (matched || matchedViaRebind) {
                bool blockKey = hotkey.blockKeyFromGame || matchedViaRebind;
                std::string hotkeyId = GetKeyComboString(alt.keys);

                if (ShouldMaskWindowsKeyForHotkey(alt.keys, isKeyDown, isAutoRepeatKeyDown)) {
                    (void)SendMenuMaskKeyTap();
                }

                if (hotkey.triggerOnHold) { return handleHoldMode(alt.keys, alt.mode, hotkeyId, blockKey); }

                // Handle trigger-on-release invalidation tracking
                if (hotkey.triggerOnRelease) {
                    if (isKeyDown) {
                        std::lock_guard<std::mutex> lock(g_triggerOnReleaseMutex);
                        for (const auto& pendingHotkeyId : g_triggerOnReleasePending) {
                            if (pendingHotkeyId != hotkeyId) { g_triggerOnReleaseInvalidated.insert(pendingHotkeyId); }
                        }
                        g_triggerOnReleasePending.insert(hotkeyId);
                        if (s_enableHotkeyDebug) { Log("[Hotkey] Alt trigger-on-release hotkey pressed, added to pending: " + hotkeyId); }
                        if (blockKey) return { true, 0 };
                        return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
                    } else {
                        // Key released - check if invalidated
                        bool wasInvalidated = false;
                        {
                            std::lock_guard<std::mutex> lock(g_triggerOnReleaseMutex);
                            wasInvalidated = g_triggerOnReleaseInvalidated.count(hotkeyId) > 0;
                            g_triggerOnReleasePending.erase(hotkeyId);
                            g_triggerOnReleaseInvalidated.erase(hotkeyId);
                        }

                        if (wasInvalidated) {
                            if (s_enableHotkeyDebug) { Log("[Hotkey] Alt trigger-on-release hotkey invalidated: " + hotkeyId); }
                            if (blockKey) return { true, 0 };
                            return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
                        }
                    }
                }

                // Check if this hotkey should trigger based on triggerOnRelease setting
                // When triggerOnRelease is true, only fire on key UP; when false (default), only fire on key DOWN
                if (hotkey.triggerOnRelease != isKeyDown) {
                    auto now = std::chrono::steady_clock::now();
                    bool debounced = false;
                    {
                        std::lock_guard<std::mutex> tsLock(g_hotkeyTimestampsMutex);
                        auto it = g_hotkeyTimestamps.find(hotkeyId);
                        if (it != g_hotkeyTimestamps.end() &&
                            std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second).count() < hotkey.debounce) {
                            debounced = true;
                        } else {
                            g_hotkeyTimestamps[hotkeyId] = now;
                        }
                    }
                    if (debounced) {
                        if (s_enableHotkeyDebug) { Log("[Hotkey] Alt hotkey matched but debounced: " + hotkeyId); }
                        if (blockKey) return { true, 0 };
                        return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
                    }

                    std::string currentSecMode = GetHotkeySecondaryMode(hotkeyIdx);
                    std::string newSecMode = (currentSecMode == alt.mode) ? hotkey.secondaryMode : alt.mode;
                    SetHotkeySecondaryMode(hotkeyIdx, newSecMode);

                    if (s_enableHotkeyDebug) { Log("[Hotkey] ✓✓✓ ALT HOTKEY TRIGGERED: " + hotkeyId + " -> " + newSecMode); }

                    if (!newSecMode.empty()) { SwitchToMode(newSecMode, "alt hotkey"); }
                }
                if (blockKey) return { true, 0 };
                return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
            }
        }

        {
            bool skipExclusions = hotkey.triggerOnRelease || (hotkey.triggerOnHold && !isKeyDown);
            bool matched = CheckHotkeyMatch(hotkey.keys, vkCode, hotkey.conditions.exclusions, skipExclusions, s_bestMatchKeyCount,
                                            rawVkCode, true, isKeyDown);
            bool matchedViaRebind = !matched && rebindTargetVk &&
                                    CheckHotkeyMatch(hotkey.keys, rebindTargetVk, hotkey.conditions.exclusions, skipExclusions,
                                                     s_bestMatchKeyCount);
            if (matched || matchedViaRebind) {
                bool blockKey = hotkey.blockKeyFromGame || matchedViaRebind;
                std::string hotkeyId = GetKeyComboString(hotkey.keys);

                if (ShouldMaskWindowsKeyForHotkey(hotkey.keys, isKeyDown, isAutoRepeatKeyDown)) {
                    (void)SendMenuMaskKeyTap();
                }

                if (hotkey.triggerOnHold) {
                    if (currentSecMode.empty()) { currentSecMode = GetHotkeySecondaryMode(hotkeyIdx); }
                    return handleHoldMode(hotkey.keys, currentSecMode, hotkeyId, blockKey);
                }

                if (hotkey.triggerOnRelease) {
                    if (isKeyDown) {
                        std::lock_guard<std::mutex> lock(g_triggerOnReleaseMutex);
                        // Invalidate all other pending trigger-on-release hotkeys
                        for (const auto& pendingHotkeyId : g_triggerOnReleasePending) {
                            if (pendingHotkeyId != hotkeyId) { g_triggerOnReleaseInvalidated.insert(pendingHotkeyId); }
                        }
                        g_triggerOnReleasePending.insert(hotkeyId);
                        if (s_enableHotkeyDebug) { Log("[Hotkey] Trigger-on-release hotkey pressed, added to pending: " + hotkeyId); }
                        if (blockKey) return { true, 0 };
                        return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
                    } else {
                        // Key released - check if invalidated
                        bool wasInvalidated = false;
                        {
                            std::lock_guard<std::mutex> lock(g_triggerOnReleaseMutex);
                            wasInvalidated = g_triggerOnReleaseInvalidated.count(hotkeyId) > 0;
                            g_triggerOnReleasePending.erase(hotkeyId);
                            g_triggerOnReleaseInvalidated.erase(hotkeyId);
                        }

                        if (wasInvalidated) {
                            if (s_enableHotkeyDebug) {
                                Log("[Hotkey] Trigger-on-release hotkey invalidated (another key was pressed): " + hotkeyId);
                            }
                            if (blockKey) return { true, 0 };
                            return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
                        }
                    }
                }

                // Check if this hotkey should trigger based on triggerOnRelease setting
                // When triggerOnRelease is true, only fire on key UP; when false (default), only fire on key DOWN
                if (hotkey.triggerOnRelease != isKeyDown) {
                    auto now = std::chrono::steady_clock::now();
                    bool debounced = false;
                    {
                        std::lock_guard<std::mutex> tsLock(g_hotkeyTimestampsMutex);
                        auto it = g_hotkeyTimestamps.find(hotkeyId);
                        if (it != g_hotkeyTimestamps.end() &&
                            std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second).count() < hotkey.debounce) {
                            debounced = true;
                        } else {
                            g_hotkeyTimestamps[hotkeyId] = now;
                        }
                    }
                    if (debounced) {
                        if (s_enableHotkeyDebug) { Log("[Hotkey] Main hotkey matched but debounced: " + hotkeyId); }
                        if (blockKey) return { true, 0 };
                        return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
                    }

                    // Lock-free read of current mode ID from double-buffer
                    std::string current = g_modeIdBuffers[g_currentModeIdIndex.load(std::memory_order_acquire)];
                    std::string targetMode;

                    if (currentSecMode.empty()) {
                        currentSecMode = GetHotkeySecondaryMode(hotkeyIdx);
                    }

                    if (EqualsIgnoreCase(current, currentSecMode)) {
                        targetMode = cfg.defaultMode;
                    } else {
                        targetMode = currentSecMode;
                    }

                    if (s_enableHotkeyDebug) {
                        Log("[Hotkey] ✓✓✓ MAIN HOTKEY TRIGGERED: " + hotkeyId + " (current: " + current + " -> target: " + targetMode + ")");
                    }

                    if (!targetMode.empty()) { SwitchToMode(targetMode, "main hotkey"); }
                }
                if (blockKey) return { true, 0 };
                return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
            }
        }
    }

    for (size_t sensIdx = 0; sensIdx < cfg.sensitivityHotkeys.size(); ++sensIdx) {
        const auto& sensHotkey = cfg.sensitivityHotkeys[sensIdx];
        if (s_enableHotkeyDebug) {
            Log("[Hotkey] Checking sensitivity hotkey: " + GetKeyComboString(sensHotkey.keys) +
                " -> sens=" + std::to_string(sensHotkey.sensitivity));
        }

        bool conditionsMet = MatchesConfiguredGameStateCondition(sensHotkey.conditions.gameState, gameState);
        if (!conditionsMet) {
            if (s_enableHotkeyDebug) { Log("[Hotkey] SKIP sensitivity: Game state conditions not met"); }
            continue;
        }

        // Sensitivity hotkeys only trigger on key down (no triggerOnRelease support)
        if (!isKeyDown) { continue; }

        {
            bool matched = CheckHotkeyMatch(sensHotkey.keys, vkCode, sensHotkey.conditions.exclusions, false, s_bestMatchKeyCount);
            bool matchedViaRebind = !matched && rebindTargetVk && CheckHotkeyMatch(sensHotkey.keys, rebindTargetVk, sensHotkey.conditions.exclusions, false, s_bestMatchKeyCount);
            if (matched || matchedViaRebind) {
                bool blockKey = matchedViaRebind;
                std::string hotkeyId = "sens_" + GetKeyComboString(sensHotkey.keys);

                if (ShouldMaskWindowsKeyForHotkey(sensHotkey.keys, isKeyDown, isAutoRepeatKeyDown)) {
                    (void)SendMenuMaskKeyTap();
                }

                auto now = std::chrono::steady_clock::now();
                bool debounced = false;
                {
                    std::lock_guard<std::mutex> tsLock(g_hotkeyTimestampsMutex);
                    auto it = g_hotkeyTimestamps.find(hotkeyId);
                    if (it != g_hotkeyTimestamps.end() &&
                        std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second).count() < sensHotkey.debounce) {
                        debounced = true;
                    } else {
                        g_hotkeyTimestamps[hotkeyId] = now;
                    }
                }
                if (debounced) {
                    if (s_enableHotkeyDebug) { Log("[Hotkey] Sensitivity hotkey matched but debounced: " + hotkeyId); }
                    if (blockKey) return { true, 0 };
                    return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
                }

                if (sensHotkey.toggle) {
                    extern TempSensitivityOverride g_tempSensitivityOverride;
                    extern std::mutex g_tempSensitivityMutex;
                    std::lock_guard<std::mutex> lock(g_tempSensitivityMutex);

                    if (g_tempSensitivityOverride.active && g_tempSensitivityOverride.activeSensHotkeyIndex == static_cast<int>(sensIdx)) {
                        g_tempSensitivityOverride.active = false;
                        g_tempSensitivityOverride.sensitivityX = 1.0f;
                        g_tempSensitivityOverride.sensitivityY = 1.0f;
                        g_tempSensitivityOverride.activeSensHotkeyIndex = -1;

                        if (s_enableHotkeyDebug) { Log("[Hotkey] ✓✓✓ SENSITIVITY HOTKEY TOGGLED OFF: " + hotkeyId); }

                        if (blockKey) return { true, 0 };
                        return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
                    }

                    g_tempSensitivityOverride.active = true;
                    if (sensHotkey.separateXY) {
                        g_tempSensitivityOverride.sensitivityX = sensHotkey.sensitivityX;
                        g_tempSensitivityOverride.sensitivityY = sensHotkey.sensitivityY;
                    } else {
                        g_tempSensitivityOverride.sensitivityX = sensHotkey.sensitivity;
                        g_tempSensitivityOverride.sensitivityY = sensHotkey.sensitivity;
                    }
                    g_tempSensitivityOverride.activeSensHotkeyIndex = static_cast<int>(sensIdx);

                    if (s_enableHotkeyDebug) {
                        Log("[Hotkey] ✓✓✓ SENSITIVITY HOTKEY TOGGLED ON: " + hotkeyId + " -> sens=" + std::to_string(sensHotkey.sensitivity));
                    }
                } else {
                    {
                        extern TempSensitivityOverride g_tempSensitivityOverride;
                        extern std::mutex g_tempSensitivityMutex;
                        std::lock_guard<std::mutex> lock(g_tempSensitivityMutex);
                        g_tempSensitivityOverride.active = true;
                        if (sensHotkey.separateXY) {
                            g_tempSensitivityOverride.sensitivityX = sensHotkey.sensitivityX;
                            g_tempSensitivityOverride.sensitivityY = sensHotkey.sensitivityY;
                        } else {
                            g_tempSensitivityOverride.sensitivityX = sensHotkey.sensitivity;
                            g_tempSensitivityOverride.sensitivityY = sensHotkey.sensitivity;
                        }
                        g_tempSensitivityOverride.activeSensHotkeyIndex = -1;
                    }

                    if (s_enableHotkeyDebug) {
                        Log("[Hotkey] ✓✓✓ SENSITIVITY HOTKEY TRIGGERED: " + hotkeyId + " -> sens=" + std::to_string(sensHotkey.sensitivity));
                    }
                }

                if (blockKey) return { true, 0 };
                return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
            }
        }
    }

    return { false, 0 };
}

InputHandlerResult HandleMouseCoordinateTranslationPhase(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM& lParam) {
    // Only translate messages whose lParam is already in CLIENT coordinates.
    // Wheel messages use SCREEN coordinates and must not be transformed here.
    switch (uMsg) {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
        break;
    default:
        return { false, 0 };
    }

    if (!IsCursorVisible() && !g_showGui.load(std::memory_order_acquire)) {
        return { false, 0 };
    }

    PROFILE_SCOPE("HandleMouseCoordinateTranslation");

    RECT clientRect{};
    if (!GetClientRect(hWnd, &clientRect)) { return { false, 0 }; }
    const int clientW = clientRect.right - clientRect.left;
    const int clientH = clientRect.bottom - clientRect.top;
    if (clientW <= 0 || clientH <= 0) { return { false, 0 }; }

    ModeViewportInfo geo;
    const std::string currentModeId = g_modeIdBuffers[g_currentModeIdIndex.load(std::memory_order_acquire)];
    auto cfgSnap = GetConfigSnapshot();
    const ModeConfig* currentMode = cfgSnap ? GetModeFromSnapshotOrFallback(*cfgSnap, currentModeId) : nullptr;
    const bool fullscreenMode = currentMode && EqualsIgnoreCase(currentMode->id, "Fullscreen");

    // Start from the same presented viewport helper the GL hooks use, then correct the
    // source size and any screen-sized output rects using live state from this message.
    if (!ResolvePresentedGameViewport(geo)) {
        if (!currentMode || currentMode->width <= 0 || currentMode->height <= 0) {
            return { false, 0 };
        }

        geo.valid = true;
        geo.x = 0;
        geo.y = 0;
        geo.width = currentMode->width;
        geo.height = currentMode->height;
        geo.stretchEnabled = currentMode->stretch.enabled;
        if (fullscreenMode) {
            geo.stretchX = 0;
            geo.stretchY = 0;
            geo.stretchWidth = clientW;
            geo.stretchHeight = clientH;
        } else if (currentMode->stretch.enabled) {
            geo.stretchX = currentMode->stretch.x;
            geo.stretchY = currentMode->stretch.y;
            geo.stretchWidth = currentMode->stretch.width;
            geo.stretchHeight = currentMode->stretch.height;
        } else {
            geo.stretchWidth = currentMode->width;
            geo.stretchHeight = currentMode->height;
            geo.stretchX = GetCenteredAxisOffset(clientW, geo.stretchWidth);
            geo.stretchY = GetCenteredAxisOffset(clientH, geo.stretchHeight);
        }
    }

    int liveViewportW = 0;
    int liveViewportH = 0;
    if (GetLatestGameViewportSize(liveViewportW, liveViewportH)) {
        geo.width = liveViewportW;
        geo.height = liveViewportH;
    }

    if (fullscreenMode) {
        geo.stretchEnabled = true;
        geo.stretchX = 0;
        geo.stretchY = 0;
        geo.stretchWidth = clientW;
        geo.stretchHeight = clientH;
    } else if (!geo.stretchEnabled) {
        const int outputWidth = geo.stretchWidth > 0 ? geo.stretchWidth : (currentMode ? currentMode->width : geo.width);
        const int outputHeight = geo.stretchHeight > 0 ? geo.stretchHeight : (currentMode ? currentMode->height : geo.height);
        if (outputWidth <= 0 || outputHeight <= 0) {
            return { false, 0 };
        }

        geo.stretchWidth = outputWidth;
        geo.stretchHeight = outputHeight;
        geo.stretchX = GetCenteredAxisOffset(clientW, outputWidth);
        geo.stretchY = GetCenteredAxisOffset(clientH, outputHeight);
    }

    if (!geo.valid || geo.width <= 0 || geo.height <= 0 || geo.stretchWidth <= 0 || geo.stretchHeight <= 0) { return { false, 0 }; }

    const int viewportLeft = geo.stretchX;
    const int viewportTop = geo.stretchY;
    const int viewportRight = geo.stretchX + geo.stretchWidth;
    const int viewportBottom = geo.stretchY + geo.stretchHeight;

    const int visibleLeft = (std::max)(0, viewportLeft);
    const int visibleTop = (std::max)(0, viewportTop);
    const int visibleRight = (std::min)(clientW, viewportRight);
    const int visibleBottom = (std::min)(clientH, viewportBottom);

    const int visibleW = visibleRight - visibleLeft;
    const int visibleH = visibleBottom - visibleTop;
    if (visibleW <= 0 || visibleH <= 0) { return { false, 0 }; }

    int mouseX = GET_X_LPARAM(lParam);
    int mouseY = GET_Y_LPARAM(lParam);

    if (mouseX < visibleLeft) mouseX = visibleLeft;
    if (mouseX >= visibleRight) mouseX = visibleRight - 1;
    if (mouseY < visibleTop) mouseY = visibleTop;
    if (mouseY >= visibleBottom) mouseY = visibleBottom - 1;

    const float scaleX = static_cast<float>(geo.width) / static_cast<float>(geo.stretchWidth);
    const float scaleY = static_cast<float>(geo.height) / static_cast<float>(geo.stretchHeight);

    const float srcOffsetX = static_cast<float>(visibleLeft - viewportLeft) * scaleX;
    const float srcOffsetY = static_cast<float>(visibleTop - viewportTop) * scaleY;

    const float localVisibleX = static_cast<float>(mouseX - visibleLeft);
    const float localVisibleY = static_cast<float>(mouseY - visibleTop);

    int newX = static_cast<int>(srcOffsetX + localVisibleX * scaleX);
    int newY = static_cast<int>(srcOffsetY + localVisibleY * scaleY);

    if (newX < 0) newX = 0;
    if (newY < 0) newY = 0;
    if (newX >= geo.width) newX = geo.width - 1;
    if (newY >= geo.height) newY = geo.height - 1;

    lParam = MAKELPARAM(newX, newY);
    return { false, 0 };
}

static UINT GetScanCodeWithExtendedFlag(DWORD vkCode) {
    auto isExtendedVk = [](DWORD vk) {
        switch (vk) {
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
        case VK_INSERT:
        case VK_DELETE:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_RCONTROL:
        case VK_RMENU:
        case VK_DIVIDE:
        case VK_NUMLOCK:
        case VK_SNAPSHOT:
            return true;
        default:
            return false;
        }
    };

    UINT scanCodeWithFlags = MapVirtualKey(static_cast<UINT>(vkCode), MAPVK_VK_TO_VSC_EX);
    if (scanCodeWithFlags == 0) {
        scanCodeWithFlags = MapVirtualKey(static_cast<UINT>(vkCode), MAPVK_VK_TO_VSC);
    }

    if ((scanCodeWithFlags & 0xFF00) == 0 && isExtendedVk(vkCode) && (scanCodeWithFlags & 0xFF) != 0) { scanCodeWithFlags |= 0xE000; }

    return scanCodeWithFlags;
}

static LPARAM BuildKeyboardMessageLParam(UINT scanCodeWithFlags, bool isKeyDown, bool isSystemKey, UINT repeatCount, bool previousKeyState,
                                         bool transitionState) {
    const UINT scanLow = scanCodeWithFlags & 0xFF;
    const bool isExtended = (scanCodeWithFlags & 0xFF00) != 0;

    LPARAM out = static_cast<LPARAM>(repeatCount == 0 ? 1 : repeatCount);
    out |= (static_cast<LPARAM>(scanLow) << 16);
    if (isExtended) out |= (1LL << 24);
    if (isSystemKey) out |= (1LL << 29);
    if (previousKeyState) out |= (1LL << 30);
    if (transitionState) out |= (1LL << 31);

    if (!isKeyDown) out |= (1LL << 30) | (1LL << 31);

    return out;
}

static bool IsLocalKeyRepeatTrackedKeyboardMessage(UINT uMsg) {
    return uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN || uMsg == WM_KEYUP || uMsg == WM_SYSKEYUP;
}

static bool IsLocalKeyRepeatTrackedCharMessage(UINT uMsg) {
    return uMsg == WM_CHAR || uMsg == WM_SYSCHAR || uMsg == WM_DEADCHAR || uMsg == WM_SYSDEADCHAR;
}

static bool HasLocalKeyRepeatTag(UINT uMsg, LPARAM lParam) {
    if (!IsLocalKeyRepeatTrackedKeyboardMessage(uMsg) && !IsLocalKeyRepeatTrackedCharMessage(uMsg)) {
        return false;
    }
    return (lParam & kToolscreenLocalKeyRepeatMessageTag) != 0;
}

static LPARAM StripLocalKeyRepeatTag(LPARAM lParam) {
    return lParam & ~kToolscreenLocalKeyRepeatMessageTag;
}

static bool IsLocalRepeatDebugEnabled() {
    auto cfgSnap = GetConfigSnapshot();
    return cfgSnap && cfgSnap->debug.showHotkeyDebug;
}

static const char* GetLocalRepeatMessageName(UINT uMsg) {
    switch (uMsg) {
    case WM_KEYDOWN: return "WM_KEYDOWN";
    case WM_SYSKEYDOWN: return "WM_SYSKEYDOWN";
    case WM_KEYUP: return "WM_KEYUP";
    case WM_SYSKEYUP: return "WM_SYSKEYUP";
    case WM_CHAR: return "WM_CHAR";
    case WM_SYSCHAR: return "WM_SYSCHAR";
    case WM_DEADCHAR: return "WM_DEADCHAR";
    case WM_SYSDEADCHAR: return "WM_SYSDEADCHAR";
    case WM_TIMER: return "WM_TIMER";
    case WM_TOOLSCREEN_LOCAL_KEY_REPEAT: return "WM_TOOLSCREEN_LOCAL_KEY_REPEAT";
    default: return "OTHER";
    }
}

static std::string FormatLocalKeyRepeatStateForDebug(const LocalKeyRepeatState& state) {
    std::ostringstream stream;
    stream << "{rawVk=" << state.rawVk << ",scan=" << state.scanCodeWithFlags << ",sys=" << (state.isSystemKey ? 1 : 0)
           << ",repeatVk=" << state.repeatVk << ",repeatScan=" << state.repeatScanCodeWithFlags
           << ",srcLParam=" << static_cast<long long>(state.sourceKeyDownLParam)
           << ",srcTime=" << state.sourceMessageTimeMs << "}";
    return stream.str();
}

static void LogLocalRepeatDebug(const char* phase, UINT uMsg, WPARAM wParam, LPARAM lParam, bool isLocalRepeatTagged) {
    if (!IsLocalRepeatDebugEnabled()) {
        return;
    }

    const bool previousState = (lParam & (static_cast<LPARAM>(1) << 30)) != 0;
    const bool transitionState = (lParam & (static_cast<LPARAM>(1) << 31)) != 0;
    std::ostringstream stream;
    stream << "[LocalRepeat] " << phase << " msg=" << GetLocalRepeatMessageName(uMsg) << " wParam="
           << static_cast<unsigned long long>(wParam) << " scan=" << GetScanCodeWithExtendedFlagFromLParam(lParam)
           << " prev=" << (previousState ? 1 : 0) << " up=" << (transitionState ? 1 : 0)
           << " tagged=" << (isLocalRepeatTagged ? 1 : 0)
           << " extraInfo=" << static_cast<unsigned long long>(GetMessageExtraInfo())
           << " owner=" << FormatLocalKeyRepeatStateForDebug(s_localKeyRepeatOwner)
           << " held=" << s_localKeyRepeatHeldKeys.size();
    Log(stream.str());
}

static DWORD WINAPI LocalKeyRepeatSchedulerThreadProc(LPVOID) {
    HANDLE handles[2] = { s_localKeyRepeatWakeEvent, s_localKeyRepeatHighResTimer };
    for (;;) {
        const DWORD waitResult = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (waitResult == WAIT_OBJECT_0) {
            if (s_localKeyRepeatHighResTimer) {
                (void)CancelWaitableTimer(s_localKeyRepeatHighResTimer);
            }

            const HWND targetHwnd = s_localKeyRepeatScheduledHwnd.load(std::memory_order_acquire);
            const int64_t delay100ns = s_localKeyRepeatRequestedDelay100ns.load(std::memory_order_acquire);
            if (!targetHwnd || !IsWindow(targetHwnd) || delay100ns <= 0 || !s_localKeyRepeatHighResTimer) {
                continue;
            }

            LARGE_INTEGER dueTime;
            dueTime.QuadPart = -static_cast<LONGLONG>(delay100ns);
            if (!SetWaitableTimer(s_localKeyRepeatHighResTimer, &dueTime, 0, NULL, NULL, FALSE)) {
                Log("WARNING: Failed to arm high-resolution local key repeat timer");
            }
            continue;
        }

        if (waitResult == (WAIT_OBJECT_0 + 1)) {
            const HWND targetHwnd = s_localKeyRepeatScheduledHwnd.load(std::memory_order_acquire);
            if (targetHwnd && IsWindow(targetHwnd)) {
                if (::PostMessageW(targetHwnd, WM_TOOLSCREEN_LOCAL_KEY_REPEAT, 0, 0) == FALSE) {
                    Log("WARNING: Failed to post high-resolution local key repeat tick");
                }
            }
            continue;
        }

        break;
    }

    return 0;
}

static bool EnsureLocalKeyRepeatSchedulerInitialized() {
    if (s_localKeyRepeatWakeEvent && s_localKeyRepeatHighResTimer && s_localKeyRepeatThread) {
        return true;
    }

    std::lock_guard<std::mutex> lock(s_localKeyRepeatSchedulerInitMutex);
    if (s_localKeyRepeatWakeEvent && s_localKeyRepeatHighResTimer && s_localKeyRepeatThread) {
        return true;
    }

    if (!s_localKeyRepeatWakeEvent) {
        s_localKeyRepeatWakeEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (!s_localKeyRepeatWakeEvent) {
            Log("WARNING: Failed to create local key repeat wake event; falling back to WM_TIMER");
            return false;
        }
    }

    if (!s_localKeyRepeatHighResTimer) {
        s_localKeyRepeatHighResTimer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!s_localKeyRepeatHighResTimer) {
            s_localKeyRepeatHighResTimer = CreateWaitableTimerW(NULL, FALSE, NULL);
        }
        if (!s_localKeyRepeatHighResTimer) {
            Log("WARNING: Failed to create high-resolution local key repeat timer; falling back to WM_TIMER");
            return false;
        }
    }

    if (!s_localKeyRepeatThread) {
        s_localKeyRepeatThread = CreateThread(NULL, 0, &LocalKeyRepeatSchedulerThreadProc, NULL, 0, NULL);
        if (!s_localKeyRepeatThread) {
            Log("WARNING: Failed to create local key repeat scheduler thread; falling back to WM_TIMER");
            return false;
        }
    }

    return true;
}

static void StopLocalKeyRepeatTimer(HWND hWnd) {
    const HWND targetHwnd = hWnd ? hWnd : g_subclassedHwnd.load(std::memory_order_acquire);
    if (targetHwnd && IsWindow(targetHwnd)) {
        (void)KillTimer(targetHwnd, kToolscreenLocalKeyRepeatTimerId);
    }

    s_localKeyRepeatDelayGateDeadlineMs = 0;

    if (s_localKeyRepeatWakeEvent && s_localKeyRepeatHighResTimer) {
        s_localKeyRepeatScheduledHwnd.store(NULL, std::memory_order_release);
        s_localKeyRepeatRequestedDelay100ns.store(0, std::memory_order_release);
        (void)SetEvent(s_localKeyRepeatWakeEvent);
    }
}

static void ArmLocalKeyRepeatTimer(HWND hWnd, int delayMs) {
    if (!hWnd || !IsWindow(hWnd)) {
        return;
    }

    const int intervalMs = (std::max)(delayMs, 1);
    (void)KillTimer(hWnd, kToolscreenLocalKeyRepeatTimerId);

    if (EnsureLocalKeyRepeatSchedulerInitialized()) {
        s_localKeyRepeatScheduledHwnd.store(hWnd, std::memory_order_release);
        s_localKeyRepeatRequestedDelay100ns.store(static_cast<int64_t>(intervalMs) * 10000,
                                                  std::memory_order_release);
        if (SetEvent(s_localKeyRepeatWakeEvent) != FALSE) {
            return;
        }

        Log("WARNING: Failed to signal high-resolution local key repeat timer; falling back to WM_TIMER");
    }

    const UINT fallbackIntervalMs = static_cast<UINT>((std::max)(intervalMs, 1));
    if (SetTimer(hWnd, kToolscreenLocalKeyRepeatTimerId, fallbackIntervalMs, NULL) == 0) {
        Log("WARNING: Failed to arm local key repeat timer");
    }
}

void ResetLocalKeyRepeatState(HWND hWnd) {
    StopLocalKeyRepeatTimer(hWnd);
    s_localKeyRepeatHeldKeys.clear();
    s_localKeyRepeatOwner = {};
    s_localKeyRepeatOwnerActive = false;
    s_localKeyRepeatSuppressedChars.clear();
}

static bool IsLocalKeyRepeatOwnerStillHeld() {
    if (!s_localKeyRepeatOwnerActive || s_localKeyRepeatOwner.rawVk == 0) {
        return false;
    }

    return s_localKeyRepeatHeldKeys.find(s_localKeyRepeatOwner.rawVk) != s_localKeyRepeatHeldKeys.end();
}

static void RestartLocalKeyRepeatStartDelay(HWND hWnd) {
    if (!hWnd || !IsWindow(hWnd) || !IsLocalKeyRepeatOwnerStillHeld()) {
        return;
    }

    int startDelayMs = 250;
    int repeatDelayMs = 33;
    (void)GetEffectiveKeyRepeatTimings(startDelayMs, repeatDelayMs);
    s_localKeyRepeatDelayGateDeadlineMs = GetTickCount64() + static_cast<ULONGLONG>((std::max)(startDelayMs, 1));
    ArmLocalKeyRepeatTimer(hWnd, startDelayMs);
}

static void BeginLocalKeyRepeatTracking(HWND hWnd, DWORD rawVk, UINT scanCodeWithFlags, bool isSystemKey, LPARAM sourceKeyDownLParam) {
    if (!hWnd || rawVk == 0) {
        return;
    }

    LocalKeyRepeatState state;
    state.rawVk = rawVk;
    state.scanCodeWithFlags = (scanCodeWithFlags != 0) ? scanCodeWithFlags : GetScanCodeWithExtendedFlag(rawVk);
    state.isSystemKey = isSystemKey;
    state.repeatVk = rawVk;
    state.repeatScanCodeWithFlags = ResolveLocalKeyRepeatOutputScanCode(rawVk, state.scanCodeWithFlags);
    state.sourceKeyDownLParam = sourceKeyDownLParam;
    state.sourceMessageTimeMs = static_cast<DWORD>(GetMessageTime());

    s_localKeyRepeatHeldKeys[rawVk] = state;
    s_localKeyRepeatOwner = state;
    s_localKeyRepeatOwnerActive = true;

    int startDelayMs = 250;
    int repeatDelayMs = 33;
    (void)GetEffectiveKeyRepeatTimings(startDelayMs, repeatDelayMs);
    ArmLocalKeyRepeatTimer(hWnd, startDelayMs);
}

static bool RetargetLocalKeyRepeatAliasSource(DWORD rawVk, UINT scanCodeWithFlags, bool isSystemKey, LPARAM sourceKeyDownLParam) {
    if (!s_localKeyRepeatOwnerActive) {
        return false;
    }
    if (!IsNonCharKeyVk(rawVk)) {
        return false;
    }
    if (IsModifierVk(rawVk)) {
        return false;
    }
    if (s_localKeyRepeatHeldKeys.size() != 1) {
        return false;
    }
    if (s_localKeyRepeatOwner.rawVk != s_localKeyRepeatOwner.repeatVk) {
        return false;
    }
    if (s_localKeyRepeatOwner.repeatVk == 0 || IsNonCharKeyVk(s_localKeyRepeatOwner.repeatVk)) {
        return false;
    }

    const DWORD messageTimeMs = static_cast<DWORD>(GetMessageTime());
    if (messageTimeMs < s_localKeyRepeatOwner.sourceMessageTimeMs ||
        (messageTimeMs - s_localKeyRepeatOwner.sourceMessageTimeMs) > 2) {
        return false;
    }

    LocalKeyRepeatState retargetedState = s_localKeyRepeatOwner;
    retargetedState.rawVk = rawVk;
    retargetedState.scanCodeWithFlags = (scanCodeWithFlags != 0) ? scanCodeWithFlags : GetScanCodeWithExtendedFlag(rawVk);
    retargetedState.isSystemKey = isSystemKey;
    retargetedState.sourceKeyDownLParam = sourceKeyDownLParam;
    retargetedState.sourceMessageTimeMs = messageTimeMs;

    s_localKeyRepeatHeldKeys.clear();
    s_localKeyRepeatHeldKeys[rawVk] = retargetedState;
    s_localKeyRepeatOwner = retargetedState;

    if (IsLocalRepeatDebugEnabled()) {
        Log("[LocalRepeat] retarget alias source owner=" + FormatLocalKeyRepeatStateForDebug(s_localKeyRepeatOwner));
    }

    return true;
}

static bool PostLocalKeyRepeatKeyDown(HWND hWnd) {
    if (!hWnd || !IsWindow(hWnd) || !IsLocalKeyRepeatOwnerStillHeld()) {
        if (IsLocalRepeatDebugEnabled()) {
            Log("[LocalRepeat] post-repeat skipped because owner is not held owner=" + FormatLocalKeyRepeatStateForDebug(s_localKeyRepeatOwner));
        }
        return false;
    }

    const UINT msg = s_localKeyRepeatOwner.isSystemKey ? WM_SYSKEYDOWN : WM_KEYDOWN;
    LPARAM repeatLParam =
        BuildKeyboardMessageLParam(s_localKeyRepeatOwner.repeatScanCodeWithFlags,
                                   true,
                                   s_localKeyRepeatOwner.isSystemKey,
                                   1,
                                   true,
                                   false);
    repeatLParam |= kToolscreenLocalKeyRepeatMessageTag;
    if (IsLocalRepeatDebugEnabled()) {
        Log("[LocalRepeat] post-repeat vk=" + std::to_string(s_localKeyRepeatOwner.repeatVk) +
            " scan=" + std::to_string(s_localKeyRepeatOwner.repeatScanCodeWithFlags) +
            " owner=" + FormatLocalKeyRepeatStateForDebug(s_localKeyRepeatOwner));
    }
    return ::PostMessage(hWnd, msg, static_cast<WPARAM>(s_localKeyRepeatOwner.repeatVk), repeatLParam) != FALSE;
}

static InputHandlerResult HandleLocalKeyRepeat(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, bool isLocalRepeatTagged) {
    const bool isRepeatTickMessage =
        (uMsg == WM_TIMER && static_cast<UINT_PTR>(wParam) == kToolscreenLocalKeyRepeatTimerId) || uMsg == WM_TOOLSCREEN_LOCAL_KEY_REPEAT;

    if (isRepeatTickMessage || IsLocalKeyRepeatTrackedKeyboardMessage(uMsg) || IsLocalKeyRepeatTrackedCharMessage(uMsg)) {
        LogLocalRepeatDebug("enter", uMsg, wParam, lParam, isLocalRepeatTagged);
    }

    if (true) {
        if (s_localKeyRepeatOwnerActive || !s_localKeyRepeatHeldKeys.empty()) {
            ResetLocalKeyRepeatState(hWnd);
        }
        if (isRepeatTickMessage) {
            if (IsLocalRepeatDebugEnabled()) {
                Log("[LocalRepeat] consume repeat tick because system repeat is enabled");
            }
            return { true, 0 };
        }
        return { false, 0 };
    }

    if (isRepeatTickMessage) {
        if (!IsLocalKeyRepeatOwnerStillHeld()) {
            if (IsLocalRepeatDebugEnabled()) {
                Log("[LocalRepeat] reset on repeat tick because owner is no longer held");
            }
            ResetLocalKeyRepeatState(hWnd);
            return { true, 0 };
        }

        const ULONGLONG nowMs = GetTickCount64();
        if (s_localKeyRepeatDelayGateDeadlineMs != 0 && nowMs < s_localKeyRepeatDelayGateDeadlineMs) {
            const ULONGLONG remainingDelayMs64 = s_localKeyRepeatDelayGateDeadlineMs - nowMs;
            const int remainingDelayMs =
                remainingDelayMs64 > static_cast<ULONGLONG>(0x7fffffff) ? 0x7fffffff : static_cast<int>(remainingDelayMs64);
            if (IsLocalRepeatDebugEnabled()) {
                Log("[LocalRepeat] ignore repeat tick before due time remainingMs=" + std::to_string(remainingDelayMs));
            }
            ArmLocalKeyRepeatTimer(hWnd, remainingDelayMs);
            return { true, 0 };
        }
        s_localKeyRepeatDelayGateDeadlineMs = 0;

        int startDelayMs = 250;
        int repeatDelayMs = 33;
        (void)GetEffectiveKeyRepeatTimings(startDelayMs, repeatDelayMs);
        if (!PostLocalKeyRepeatKeyDown(hWnd)) {
            if (IsLocalRepeatDebugEnabled()) {
                Log("[LocalRepeat] reset on repeat tick because posting repeat keydown failed");
            }
            ResetLocalKeyRepeatState(hWnd);
            return { true, 0 };
        }

        ArmLocalKeyRepeatTimer(hWnd, repeatDelayMs);
        return { true, 0 };
    }

    if (IsLocalKeyRepeatTrackedCharMessage(uMsg)) {
        if (!isLocalRepeatTagged && s_localKeyRepeatOwnerActive && s_localKeyRepeatOwner.sourceKeyDownLParam == lParam &&
            IsNonCharKeyVk(s_localKeyRepeatOwner.rawVk)) {
            DWORD repeatVk = 0;
            UINT repeatScanCodeWithFlags = 0;
            if (TryResolveLocalKeyRepeatVkFromChar(wParam, repeatVk, repeatScanCodeWithFlags)) {
                if (IsLocalRepeatDebugEnabled()) {
                    Log("[LocalRepeat] learn repeat from char=" + std::to_string(static_cast<unsigned int>(wParam)) +
                        " repeatVk=" + std::to_string(repeatVk) + " repeatScan=" + std::to_string(repeatScanCodeWithFlags));
                }
                s_localKeyRepeatOwner.repeatVk = repeatVk;
                s_localKeyRepeatOwner.repeatScanCodeWithFlags = repeatScanCodeWithFlags;

                auto heldIt = s_localKeyRepeatHeldKeys.find(s_localKeyRepeatOwner.rawVk);
                if (heldIt != s_localKeyRepeatHeldKeys.end()) {
                    heldIt->second.repeatVk = repeatVk;
                    heldIt->second.repeatScanCodeWithFlags = repeatScanCodeWithFlags;
                }
            }
        }

        if (!isLocalRepeatTagged) {
            auto pendingSuppressedChar = std::find_if(s_localKeyRepeatSuppressedChars.begin(),
                                                      s_localKeyRepeatSuppressedChars.end(),
                                                      [&](const SuppressedLocalRepeatChar& entry) { return entry.lParam == lParam; });
            if (pendingSuppressedChar != s_localKeyRepeatSuppressedChars.end()) {
                if (IsLocalRepeatDebugEnabled()) {
                    Log("[LocalRepeat] suppress char because a duplicate held keydown was swallowed");
                }
                s_localKeyRepeatSuppressedChars.erase(pendingSuppressedChar);
                return { true, 0 };
            }
        }

        const bool isRepeatChar = (lParam & (static_cast<LPARAM>(1) << 30)) != 0;
        if (isRepeatChar && !isLocalRepeatTagged) {
            if (IsLocalRepeatDebugEnabled()) {
                Log("[LocalRepeat] suppress char because bit30 marks it as repeat");
            }
            return { true, 0 };
        }
        return { false, 0 };
    }

    if (!IsLocalKeyRepeatTrackedKeyboardMessage(uMsg)) {
        return { false, 0 };
    }

    const ULONG_PTR extraInfo = GetMessageExtraInfo();
    if (extraInfo == kToolscreenInjectedExtraInfo || extraInfo == kToolscreenMenuMaskExtraInfo) {
        if (IsLocalRepeatDebugEnabled()) {
            Log("[LocalRepeat] ignore keyboard event because Toolscreen injected it extraInfo=" +
                std::to_string(static_cast<unsigned long long>(extraInfo)));
        }
        return { false, 0 };
    }

    if (isLocalRepeatTagged) {
        if (IsLocalRepeatDebugEnabled()) {
            Log("[LocalRepeat] allow tagged synthetic repeat to continue through the normal pipeline");
        }
        return { false, 0 };
    }

    const bool isKeyDown = (uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN);
    const DWORD rawVk = static_cast<DWORD>(wParam);
    if (rawVk == 0) {
        return { false, 0 };
    }

    DWORD trackedVk = NormalizeModifierVkFromKeyMessage(rawVk, lParam);
    if (trackedVk == 0) {
        trackedVk = rawVk;
    }

    if (isKeyDown) {
        const bool alreadyHeld = s_localKeyRepeatHeldKeys.find(trackedVk) != s_localKeyRepeatHeldKeys.end();
        if ((lParam & (static_cast<LPARAM>(1) << 30)) != 0 && alreadyHeld) {
            if (IsLocalRepeatDebugEnabled()) {
                Log("[LocalRepeat] suppress keydown because bit30 marks exact key as repeat rawVk=" + std::to_string(rawVk) +
                    " trackedVk=" + std::to_string(trackedVk));
            }
            return { true, 0 };
        }

        if ((lParam & (static_cast<LPARAM>(1) << 30)) != 0 && IsLocalRepeatDebugEnabled()) {
            Log("[LocalRepeat] allow keydown despite bit30 because exact key is not held rawVk=" + std::to_string(rawVk) +
                " trackedVk=" + std::to_string(trackedVk));
        }

        if (alreadyHeld) {
            if (!IsNonCharKeyVk(trackedVk)) {
                s_localKeyRepeatSuppressedChars.push_back({ trackedVk, lParam });
            }
            if (IsLocalRepeatDebugEnabled()) {
                Log("[LocalRepeat] suppress duplicate held keydown rawVk=" + std::to_string(rawVk) +
                    " trackedVk=" + std::to_string(trackedVk) +
                    " owner=" + FormatLocalKeyRepeatStateForDebug(s_localKeyRepeatOwner));
            }
            return { true, 0 };
        }

        if (RetargetLocalKeyRepeatAliasSource(trackedVk, GetScanCodeWithExtendedFlagFromLParam(lParam), uMsg == WM_SYSKEYDOWN, lParam)) {
            return { false, 0 };
        }

        if (IsModifierVk(trackedVk)) {
            if (s_localKeyRepeatOwnerActive || !s_localKeyRepeatHeldKeys.empty()) {
                if (IsLocalRepeatDebugEnabled()) {
                    Log("[LocalRepeat] interrupt active repeat on modifier keydown rawVk=" + std::to_string(rawVk) +
                        " trackedVk=" + std::to_string(trackedVk));
                }
                ResetLocalKeyRepeatState(hWnd);
            } else if (IsLocalRepeatDebugEnabled()) {
                Log("[LocalRepeat] allow modifier keydown with no active repeat to interrupt rawVk=" + std::to_string(rawVk) +
                    " trackedVk=" + std::to_string(trackedVk));
            }
            return { false, 0 };
        }

        BeginLocalKeyRepeatTracking(hWnd, trackedVk, GetScanCodeWithExtendedFlagFromLParam(lParam), uMsg == WM_SYSKEYDOWN, lParam);
        if (IsLocalRepeatDebugEnabled()) {
            Log("[LocalRepeat] begin tracking owner=" + FormatLocalKeyRepeatStateForDebug(s_localKeyRepeatOwner));
        }
        return { false, 0 };
    }

    std::erase_if(s_localKeyRepeatSuppressedChars, [trackedVk](const SuppressedLocalRepeatChar& entry) { return entry.rawVk == trackedVk; });

    const bool matchesRemappedOwnerKeyUp =
        s_localKeyRepeatOwnerActive && IsNonCharKeyVk(s_localKeyRepeatOwner.rawVk) && s_localKeyRepeatOwner.repeatVk != 0 &&
        s_localKeyRepeatOwner.repeatVk == trackedVk;

    size_t erasedHeldCount = s_localKeyRepeatHeldKeys.erase(trackedVk);
    if (erasedHeldCount == 0 && matchesRemappedOwnerKeyUp) {
        erasedHeldCount = s_localKeyRepeatHeldKeys.erase(s_localKeyRepeatOwner.rawVk);
    }

    if (s_localKeyRepeatOwnerActive && (s_localKeyRepeatOwner.rawVk == trackedVk || matchesRemappedOwnerKeyUp)) {
        if (IsLocalRepeatDebugEnabled()) {
            Log("[LocalRepeat] release owner rawVk=" + std::to_string(rawVk) +
                " trackedVk=" + std::to_string(trackedVk) +
                " remappedMatch=" + std::to_string(matchesRemappedOwnerKeyUp ? 1 : 0) +
                " owner=" + FormatLocalKeyRepeatStateForDebug(s_localKeyRepeatOwner));
        }
        s_localKeyRepeatOwner = {};
        s_localKeyRepeatOwnerActive = false;
        StopLocalKeyRepeatTimer(hWnd);
    }

    if (IsModifierVk(trackedVk) && IsLocalKeyRepeatOwnerStillHeld()) {
        if (IsLocalRepeatDebugEnabled()) {
            Log("[LocalRepeat] restart repeat delay after modifier keyup trackedVk=" + std::to_string(trackedVk) +
                " owner=" + FormatLocalKeyRepeatStateForDebug(s_localKeyRepeatOwner));
        }
        RestartLocalKeyRepeatStartDelay(hWnd);
    }

    return { false, 0 };
}

static UINT ResolveOutputScanCode(DWORD outputVk, UINT configuredScanCodeWithFlags) {
    if (configuredScanCodeWithFlags == 0) { return GetScanCodeWithExtendedFlag(outputVk); }

    if ((configuredScanCodeWithFlags & 0xFF00) == 0) {
        UINT vkScan = GetScanCodeWithExtendedFlag(outputVk);
        if ((vkScan & 0xFF00) != 0 && ((vkScan & 0xFF) == (configuredScanCodeWithFlags & 0xFF))) { return vkScan; }
    }

    return configuredScanCodeWithFlags;
}

static bool IsMouseButtonVk(DWORD vk) {
    return vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
}

static bool IsMouseWheelPseudoVk(DWORD vk) {
    return vk == VK_TOOLSCREEN_SCROLL_UP || vk == VK_TOOLSCREEN_SCROLL_DOWN;
}

static short GetMouseWheelDeltaForPseudoVk(DWORD vk) {
    return (vk == VK_TOOLSCREEN_SCROLL_DOWN) ? static_cast<short>(-WHEEL_DELTA) : static_cast<short>(WHEEL_DELTA);
}

static bool IsMouseLikeVk(DWORD vk) {
    return IsMouseButtonVk(vk) || IsMouseWheelPseudoVk(vk);
}

static bool IsNonCharKeyVk(DWORD vk) {
    if (IsModifierVk(vk)) return true;
    if (IsMouseLikeVk(vk)) return true;
    if (vk == VK_LWIN || vk == VK_RWIN) return true;
    if (vk >= VK_F1 && vk <= VK_F24) return true;

    switch (vk) {
    case VK_INSERT:
    case VK_DELETE:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_LEFT:
    case VK_RIGHT:
    case VK_UP:
    case VK_DOWN:
    case VK_CLEAR:
    case VK_ESCAPE:
    case VK_PAUSE:
    case VK_SNAPSHOT:
    case VK_CAPITAL:
    case VK_NUMLOCK:
    case VK_SCROLL:
    case VK_APPS:
        return true;
    default:
        return false;
    }
}

static UINT ResolveLocalKeyRepeatOutputScanCode(DWORD rawVk, UINT incomingScanCodeWithFlags) {
    const UINT canonicalScanCodeWithFlags = GetScanCodeWithExtendedFlag(rawVk);
    if (incomingScanCodeWithFlags == 0) {
        return canonicalScanCodeWithFlags;
    }
    if (rawVk == 0) {
        return incomingScanCodeWithFlags;
    }

    DWORD incomingMappedVk = static_cast<DWORD>(::MapVirtualKeyW(incomingScanCodeWithFlags, MAPVK_VSC_TO_VK_EX));
    if (incomingMappedVk == 0 && (incomingScanCodeWithFlags & 0xFF00) != 0) {
        incomingMappedVk = static_cast<DWORD>(::MapVirtualKeyW(incomingScanCodeWithFlags & 0xFF, MAPVK_VSC_TO_VK_EX));
    }
    if (incomingMappedVk == 0 || incomingMappedVk == rawVk) {
        return incomingScanCodeWithFlags;
    }

    if (!IsNonCharKeyVk(rawVk) && IsNonCharKeyVk(incomingMappedVk) && canonicalScanCodeWithFlags != 0) {
        return canonicalScanCodeWithFlags;
    }

    return incomingScanCodeWithFlags;
}

static bool TryResolveLocalKeyRepeatVkFromChar(WPARAM charCode, DWORD& outVk, UINT& outScanCodeWithFlags) {
    outVk = 0;
    outScanCodeWithFlags = 0;

    if (charCode == 0 || charCode > 0xFFFFu) {
        return false;
    }

    const HKL keyboardLayout = GetKeyboardLayout(0);
    const SHORT vkAndShiftState = VkKeyScanExW(static_cast<WCHAR>(charCode), keyboardLayout);
    if (vkAndShiftState == -1) {
        return false;
    }

    const BYTE shiftState = HIBYTE(vkAndShiftState);
    if ((shiftState & ~static_cast<BYTE>(1)) != 0) {
        return false;
    }

    outVk = static_cast<DWORD>(LOBYTE(vkAndShiftState));
    if (outVk == 0) {
        return false;
    }

    outScanCodeWithFlags = GetScanCodeWithExtendedFlag(outVk);
    return outScanCodeWithFlags != 0;
}

static bool RebindCannotType(const KeyRebind& rebind) {
    return DoesKeyRebindTriggerCannotType(rebind);
}

static bool TryTranslateVkToChar(DWORD vkCode, bool shiftDown, WCHAR& outChar) {
    BYTE keyboardState[256] = {};
    if (shiftDown) keyboardState[VK_SHIFT] = 0x80;

    HKL keyboardLayout = GetKeyboardLayout(0);
    UINT scanCode = GetScanCodeWithExtendedFlag(vkCode) & 0xFF;
    WCHAR utf16Buffer[8] = {};

    int translated = ToUnicodeEx(static_cast<UINT>(vkCode), scanCode, keyboardState, utf16Buffer, 8, 0, keyboardLayout);
    if (translated == 1) {
        outChar = utf16Buffer[0];
        return outChar != 0;
    }

    if (translated < 0) {
        BYTE emptyState[256] = {};
        WCHAR clearBuffer[8] = {};
        ToUnicodeEx(static_cast<UINT>(vkCode), scanCode, emptyState, clearBuffer, 8, 0, keyboardLayout);
    }

    return false;
}

static bool TryTranslateVkToCharWithKeyboardState(DWORD vkCode, const BYTE keyboardState[256], WCHAR& outChar) {
    HKL keyboardLayout = GetKeyboardLayout(0);
    UINT scanCode = GetScanCodeWithExtendedFlag(vkCode) & 0xFF;

    WCHAR utf16Buffer[8] = {};
    BYTE ksCopy[256] = {};
    memcpy(ksCopy, keyboardState, 256);

    int translated = ToUnicodeEx(static_cast<UINT>(vkCode), scanCode, ksCopy, utf16Buffer, 8, 0, keyboardLayout);
    if (translated == 1) {
        outChar = utf16Buffer[0];
        return outChar != 0;
    }

    if (translated < 0) {
        BYTE emptyState[256] = {};
        WCHAR clearBuffer[8] = {};
        ToUnicodeEx(static_cast<UINT>(vkCode), scanCode, emptyState, clearBuffer, 8, 0, keyboardLayout);
    }

    return false;
}

static bool TryTranslateVkToCharPreferShiftState(DWORD vkCode, bool preferShifted, WCHAR& outChar);
static LRESULT SendUnicodeScalarAsCharMessage(HWND hWnd, UINT charMsg, uint32_t cp, LPARAM lParam);

static bool ResolvePreferredOutputShiftState(const KeyRebind& rebind, bool shiftLayerActive, bool fallbackShifted) {
    if (shiftLayerActive && HasShiftLayerOutputOverride(rebind)) {
        return rebind.shiftLayerOutputShifted;
    }
    if (rebind.baseOutputShifted) {
        return true;
    }
    return fallbackShifted;
}

static void ApplyPreferredOutputShiftState(const KeyRebind& rebind, bool shiftLayerActive, BYTE keyboardState[256]) {
    if (!keyboardState) return;

    if (ShouldIgnoreCapsLockForRebindText(rebind)) {
        keyboardState[VK_CAPITAL] = 0;
    }

    bool shouldOverrideShiftState = false;
    BYTE shiftState = 0;
    if (shiftLayerActive && HasShiftLayerOutputVk(rebind)) {
        shouldOverrideShiftState = true;
        shiftState = rebind.shiftLayerOutputShifted ? 0x80 : 0;
    } else if (rebind.baseOutputShifted) {
        shouldOverrideShiftState = true;
        shiftState = 0x80;
    }
    if (!shouldOverrideShiftState) return;

    keyboardState[VK_SHIFT] = shiftState;
    keyboardState[VK_LSHIFT] = shiftState;
    keyboardState[VK_RSHIFT] = shiftState;
}

static void EmitRebindTypedChar(HWND hWnd, const KeyRebind& rebind, bool shiftLayerActive, DWORD textVK,
                                bool preferShiftedText, UINT charMsg, LPARAM charLParam) {
    const uint32_t configuredUnicodeText =
        (shiftLayerActive && HasShiftLayerOutputUnicode(rebind))
            ? static_cast<uint32_t>(rebind.shiftLayerOutputUnicode)
            : ((!shiftLayerActive && rebind.useCustomOutput && rebind.customOutputUnicode != 0)
                   ? static_cast<uint32_t>(rebind.customOutputUnicode)
                   : 0u);

    if (configuredUnicodeText != 0) {
        if (configuredUnicodeText <= 0xFFFFu) {
            SendMessage(hWnd, WM_TOOLSCREEN_CHAR_NO_REBIND, static_cast<WPARAM>(static_cast<WCHAR>(configuredUnicodeText)), charLParam);
        } else {
            SendUnicodeScalarAsCharMessage(hWnd, charMsg, configuredUnicodeText, charLParam);
        }
        return;
    }

    if (textVK == 0) {
        return;
    }

    WCHAR outChar = 0;

    if (textVK == VK_RETURN) {
        outChar = L'\r';
    } else if (textVK == VK_TAB) {
        outChar = L'\t';
    } else if (textVK == VK_BACK) {
        outChar = L'\b';
    } else {
        BYTE ks[256] = {};
        if (GetKeyboardState(ks)) {
            if (rebind.fromKey == VK_SHIFT || rebind.fromKey == VK_LSHIFT || rebind.fromKey == VK_RSHIFT) {
                ks[VK_SHIFT] = 0;
                ks[VK_LSHIFT] = 0;
                ks[VK_RSHIFT] = 0;
            } else if (rebind.fromKey == VK_CONTROL || rebind.fromKey == VK_LCONTROL || rebind.fromKey == VK_RCONTROL) {
                ks[VK_CONTROL] = 0;
                ks[VK_LCONTROL] = 0;
                ks[VK_RCONTROL] = 0;
            } else if (rebind.fromKey == VK_MENU || rebind.fromKey == VK_LMENU || rebind.fromKey == VK_RMENU) {
                ks[VK_MENU] = 0;
                ks[VK_LMENU] = 0;
                ks[VK_RMENU] = 0;
            }

            ApplyPreferredOutputShiftState(rebind, shiftLayerActive, ks);
            (void)TryTranslateVkToCharWithKeyboardState(textVK, ks, outChar);
        }

        if (outChar == 0) {
            (void)TryTranslateVkToCharPreferShiftState(textVK, preferShiftedText, outChar);
        }
    }

    if (outChar != 0) {
        SendMessage(hWnd, WM_TOOLSCREEN_CHAR_NO_REBIND, static_cast<WPARAM>(outChar), charLParam);
    }
}

static bool TryTranslateVkToCharPreferShiftState(DWORD vkCode, bool preferShifted, WCHAR& outChar) {
    if (TryTranslateVkToChar(vkCode, preferShifted, outChar) && outChar != 0) {
        return true;
    }
    if (TryTranslateVkToChar(vkCode, !preferShifted, outChar) && outChar != 0) {
        return true;
    }
    return false;
}

static bool IsValidUnicodeScalar(uint32_t cp) {
    if (cp == 0) return false;
    if (cp > 0x10FFFFu) return false;
    if (cp >= 0xD800u && cp <= 0xDFFFu) return false;
    return true;
}

static LRESULT SendUnicodeScalarAsCharMessage(HWND hWnd, UINT charMsg, uint32_t cp, LPARAM lParam) {
    if (!IsValidUnicodeScalar(cp)) return 0;

    if (cp <= 0xFFFFu) {
        return CallWindowProc(g_originalWndProc, hWnd, charMsg, (WPARAM)(WCHAR)cp, lParam);
    }

    uint32_t v = cp - 0x10000u;
    WCHAR high = (WCHAR)(0xD800u + (v >> 10));
    WCHAR low = (WCHAR)(0xDC00u + (v & 0x3FFu));
    (void)CallWindowProc(g_originalWndProc, hWnd, charMsg, (WPARAM)high, lParam);
    return CallWindowProc(g_originalWndProc, hWnd, charMsg, (WPARAM)low, lParam);
}

static constexpr WORD kToolscreenMenuMaskVk = 0xFF;

static bool SendSynthKeyByVirtualKey(WORD virtualKey, bool keyDown, ULONG_PTR extraInfo) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = virtualKey;
    in.ki.wScan = 0;
    in.ki.dwFlags = keyDown ? 0 : KEYEVENTF_KEYUP;
    in.ki.time = 0;
    in.ki.dwExtraInfo = extraInfo;
    return ::SendInput(1, &in, sizeof(INPUT)) == 1;
}

static bool SendMenuMaskKeyTap() {
    const bool downSent = SendSynthKeyByVirtualKey(kToolscreenMenuMaskVk, true, kToolscreenMenuMaskExtraInfo);
    const bool upSent = SendSynthKeyByVirtualKey(kToolscreenMenuMaskVk, false, kToolscreenMenuMaskExtraInfo);
    return downSent && upSent;
}

static bool HotkeyUsesWindowsKey(const std::vector<DWORD>& keys) {
    return std::find(keys.begin(), keys.end(), VK_LWIN) != keys.end() || std::find(keys.begin(), keys.end(), VK_RWIN) != keys.end();
}

static bool ShouldMaskWindowsKeyForHotkey(const std::vector<DWORD>& keys, bool isKeyDown, bool isAutoRepeatKeyDown) {
    return isKeyDown && !isAutoRepeatKeyDown && HotkeyUsesWindowsKey(keys);
}

static bool IsMenuActivationModifierVk(DWORD vk) {
    return vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN;
}

static bool DoesRebindPreserveMenuModifier(const KeyRebind& rebind, DWORD triggerVk) {
    if (rebind.useCustomOutput) return false;

    switch (rebind.fromKey) {
    case VK_MENU:
        return triggerVk == VK_MENU || triggerVk == VK_LMENU || triggerVk == VK_RMENU;
    case VK_LMENU:
        return triggerVk == VK_LMENU;
    case VK_RMENU:
        return triggerVk == VK_RMENU;
    case VK_LWIN:
        return triggerVk == VK_LWIN;
    case VK_RWIN:
        return triggerVk == VK_RWIN;
    default:
        return false;
    }
}

static bool ShouldMaskMenuModifierForRebind(const KeyRebind& rebind, DWORD incomingVk, DWORD incomingRawVk, bool isKeyDown,
                                            bool isAutoRepeatKeyDown, DWORD triggerVk) {
    if (!isKeyDown || isAutoRepeatKeyDown) return false;
    if (!IsMenuActivationModifierVk(incomingVk) && !IsMenuActivationModifierVk(incomingRawVk) &&
        !IsMenuActivationModifierVk(rebind.fromKey)) {
        return false;
    }

    return !DoesRebindPreserveMenuModifier(rebind, triggerVk);
}

static bool IsDeepSuppressionEligibleSourceVk(DWORD vk) {
    return IsMenuActivationModifierVk(vk);
}

static UINT BuildScanCodeWithFlagsFromLowLevelEvent(const KBDLLHOOKSTRUCT& info) {
    UINT scanCodeWithFlags = static_cast<UINT>(info.scanCode & 0xFF);
    if ((info.flags & LLKHF_EXTENDED) != 0) {
        scanCodeWithFlags |= 0xE000;
    }
    return scanCodeWithFlags;
}

static InputHandlerResult ExecuteMatchedKeyRebind(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, DWORD rawVkCode, DWORD vkCode,
                                                  bool isMouseButton, bool isKeyDown, bool isAutoRepeatKeyDown,
                                                  const KeyRebind& rebind);

static bool IsCapsLockSuppressionEnabled() {
    if (g_isShuttingDown.load(std::memory_order_acquire)) return false;
    if (!DoesSubclassedWindowOwnForegroundInput()) return false;
    if (!g_gameWindowActive.load(std::memory_order_acquire)) return false;
    if (IsHotkeyBindingActive() || IsRebindBindingActive()) return false;

    const auto cfg = GetConfigSnapshot();
    if (!cfg) return false;
    return cfg->keyRebinds.suppressCapsLockToggle;
}

static bool ShouldSuppressLowLevelMenuModifierKey(DWORD rawVk) {
    if (!IsDeepSuppressionEligibleSourceVk(rawVk)) return false;
    if (g_isShuttingDown.load(std::memory_order_acquire)) return false;
    if (!DoesSubclassedWindowOwnForegroundInput()) return false;
    if (!g_gameWindowActive.load(std::memory_order_acquire)) return false;
    if (IsHotkeyBindingActive() || IsRebindBindingActive()) return false;

    const auto cfg = GetConfigSnapshot();
    if (!cfg || !cfg->keyRebinds.enabled) return false;

    const bool cursorVisible = IsCursorVisible();
    const KeyRebind* matchedRebind = FindPreferredEnabledKeyRebind(
        cfg->keyRebinds.rebinds,
        cursorVisible,
        [&](const KeyRebind& rebind) {
            if (!IsDeepSuppressionEligibleSourceVk(rebind.fromKey)) return false;
            if (!MatchesRebindSourceKey(rawVk, rawVk, rebind.fromKey)) return false;
            if ((cfg->keyRebinds.allowSystemAltTab || cfg->keyRebinds.allowSystemAltF4) && IsAltVk(rawVk) && IsAltVk(rebind.fromKey)) {
                return false;
            }
            return true;
        });
    if (matchedRebind != nullptr) {
        const DWORD triggerVk = NormalizeModifierVkFromConfig(matchedRebind->toKey,
                                                              (matchedRebind->useCustomOutput ? matchedRebind->customOutputScanCode : 0));
        if (ShouldMaskMenuModifierForRebind(*matchedRebind, rawVk, rawVk, true, false, triggerVk)) {
            return true;
        }
    }

    return false;
}

static bool PostSuppressedLowLevelKeyMessage(HWND hWnd, const LowLevelSuppressedKeyState& state, bool isKeyDown, bool previousKeyState) {
    if (!hWnd || state.rawVk == 0) return false;

    const UINT msg = isKeyDown ? (state.isSystemKey ? WM_SYSKEYDOWN : WM_KEYDOWN) : (state.isSystemKey ? WM_SYSKEYUP : WM_KEYUP);
    const LPARAM msgLParam = BuildKeyboardMessageLParam(state.scanCodeWithFlags, isKeyDown, state.isSystemKey, 1, previousKeyState, !isKeyDown);
    return ::PostMessage(hWnd, msg, static_cast<WPARAM>(state.rawVk), msgLParam) != FALSE;
}

static LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code < 0 || lParam == 0) {
        return CallNextHookEx(s_lowLevelKeyboardHook, code, wParam, lParam);
    }

    const KBDLLHOOKSTRUCT* info = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
    if (!info) {
        return CallNextHookEx(s_lowLevelKeyboardHook, code, wParam, lParam);
    }

    if (info->dwExtraInfo == kToolscreenInjectedExtraInfo || info->dwExtraInfo == kToolscreenMenuMaskExtraInfo) {
        return CallNextHookEx(s_lowLevelKeyboardHook, code, wParam, lParam);
    }

    const bool isKeyDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
    const bool isKeyUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
    if (!isKeyDown && !isKeyUp) {
        return CallNextHookEx(s_lowLevelKeyboardHook, code, wParam, lParam);
    }

    const DWORD rawVk = static_cast<DWORD>(info->vkCode);

    // Exact-sided modifier resolution for later window messages depends on seeing the
    // low-level transition even if the suppression path declines to act on this event.
    RecordLowLevelExactModifierEvent(rawVk, isKeyDown);

    const HWND targetHwnd = g_subclassedHwnd.load(std::memory_order_acquire);
    if (!targetHwnd) {
        return CallNextHookEx(s_lowLevelKeyboardHook, code, wParam, lParam);
    }
    if (!IsWindowInForegroundTree(targetHwnd) || !g_gameWindowActive.load(std::memory_order_acquire)) {
        return CallNextHookEx(s_lowLevelKeyboardHook, code, wParam, lParam);
    }

    LowLevelSuppressedKeyState existingState{};
    bool hasExistingState = false;
    {
        std::lock_guard<std::mutex> lock(s_lowLevelSuppressedKeysMutex);
        auto it = s_lowLevelSuppressedKeys.find(rawVk);
        if (it != s_lowLevelSuppressedKeys.end()) {
            existingState = it->second;
            hasExistingState = true;
            if (isKeyUp) {
                s_lowLevelSuppressedKeys.erase(it);
            }
        }
    }

    if (hasExistingState) {
        (void)PostSuppressedLowLevelKeyMessage(targetHwnd, existingState, isKeyDown, true);
        return 1;
    }

    if (!isKeyDown) {
        return CallNextHookEx(s_lowLevelKeyboardHook, code, wParam, lParam);
    }

    const bool suppressCapsLock = (rawVk == VK_CAPITAL) && IsCapsLockSuppressionEnabled();
    if (!suppressCapsLock && !ShouldSuppressLowLevelMenuModifierKey(rawVk)) {
        return CallNextHookEx(s_lowLevelKeyboardHook, code, wParam, lParam);
    }

    LowLevelSuppressedKeyState newState{};
    newState.rawVk = rawVk;
    newState.scanCodeWithFlags = BuildScanCodeWithFlagsFromLowLevelEvent(*info);
    newState.isSystemKey = (rawVk == VK_MENU || rawVk == VK_LMENU || rawVk == VK_RMENU);

    {
        std::lock_guard<std::mutex> lock(s_lowLevelSuppressedKeysMutex);
        s_lowLevelSuppressedKeys[rawVk] = newState;
    }

    (void)PostSuppressedLowLevelKeyMessage(targetHwnd, newState, true, false);
    return 1;
}

static void EnsureLowLevelKeyboardHookInstalled() {
    if (s_lowLevelKeyboardHook != NULL) return;
    if (g_isShuttingDown.load(std::memory_order_acquire)) return;

    std::lock_guard<std::mutex> lock(s_lowLevelKeyboardHookMutex);
    if (s_lowLevelKeyboardHook != NULL) return;
    if (g_hModule == NULL) return;

    s_lowLevelKeyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, g_hModule, 0);
    if (s_lowLevelKeyboardHook == NULL) {
        Log("WARNING: Failed to install low-level keyboard hook for deep key rebind suppression.");
        return;
    }

    LogCategory("init", "Installed low-level keyboard hook for exact modifier tracking and deep key rebind suppression.");
}

static bool HasDeepSuppressionEligibleEnabledRebind() {
    if (!DoesSubclassedWindowOwnForegroundInput()) return false;
    if (!g_gameWindowActive.load(std::memory_order_acquire)) return false;
    if (IsHotkeyBindingActive() || IsRebindBindingActive()) return false;

    const auto cfg = GetConfigSnapshot();
    if (!cfg || !cfg->keyRebinds.enabled) return false;

    for (const auto& rebind : cfg->keyRebinds.rebinds) {
        if (!rebind.enabled) continue;
        if (!IsDeepSuppressionEligibleSourceVk(rebind.fromKey)) continue;
        if ((cfg->keyRebinds.allowSystemAltTab || cfg->keyRebinds.allowSystemAltF4) && IsAltVk(rebind.fromKey)) {
            continue;
        }

        const DWORD triggerVk = NormalizeModifierVkFromConfig(rebind.toKey,
                                                              (rebind.useCustomOutput ? rebind.customOutputScanCode : 0));
        if (ShouldMaskMenuModifierForRebind(rebind, rebind.fromKey, rebind.fromKey, true, false, triggerVk)) {
            return true;
        }
    }

    return false;
}

static bool HotkeyKeysNeedExactModifierTracking(const std::vector<DWORD>& keys) {
    for (DWORD key : keys) {
        if (IsTrackedSpecificModifierVk(key)) {
            return true;
        }
    }

    return false;
}

static bool HasExactModifierTrackingNeed() {
    if (!DoesSubclassedWindowOwnForegroundInput()) return false;
    if (!g_gameWindowActive.load(std::memory_order_acquire)) return false;
    if (IsHotkeyBindingActive() || IsRebindBindingActive()) return false;

    const auto cfg = GetConfigSnapshot();
    if (!cfg) return false;

    if (HotkeyKeysNeedExactModifierTracking(cfg->guiHotkey)) return true;
    if (HotkeyKeysNeedExactModifierTracking(cfg->borderlessHotkey)) return true;
    if (HotkeyKeysNeedExactModifierTracking(cfg->imageOverlaysHotkey)) return true;
    if (HotkeyKeysNeedExactModifierTracking(cfg->windowOverlaysHotkey)) return true;
    if (HotkeyKeysNeedExactModifierTracking(cfg->ninjabrainOverlayHotkey)) return true;
    if (HotkeyKeysNeedExactModifierTracking(cfg->keyRebinds.toggleHotkey)) return true;

    for (const auto& hotkey : cfg->hotkeys) {
        if (HotkeyKeysNeedExactModifierTracking(hotkey.keys)) return true;
        for (const auto& alt : hotkey.altSecondaryModes) {
            if (HotkeyKeysNeedExactModifierTracking(alt.keys)) return true;
        }
    }

    for (const auto& hotkey : cfg->sensitivityHotkeys) {
        if (HotkeyKeysNeedExactModifierTracking(hotkey.keys)) return true;
    }

    if (!cfg->keyRebinds.enabled) {
        return false;
    }

    for (const auto& rebind : cfg->keyRebinds.rebinds) {
        if (!rebind.enabled) continue;
        if (IsTrackedSpecificModifierVk(rebind.fromKey)) {
            return true;
        }
    }

    return false;
}

static void UninstallLowLevelKeyboardHook() {
    std::lock_guard<std::mutex> lock(s_lowLevelKeyboardHookMutex);
    if (s_lowLevelKeyboardHook == NULL) return;

    HHOOK hook = s_lowLevelKeyboardHook;
    s_lowLevelKeyboardHook = NULL;

    if (UnhookWindowsHookEx(hook) == FALSE) {
        Log("WARNING: Failed to uninstall low-level keyboard hook for deep key rebind suppression.");
        return;
    }

    LogCategory("init", "Uninstalled low-level keyboard hook for exact modifier tracking and deep key rebind suppression.");
}

static void UpdateLowLevelKeyboardHookInstalledState() {
    const bool needsDeepSuppression = HasDeepSuppressionEligibleEnabledRebind();
    const bool needsExactModifierTracking = HasExactModifierTrackingNeed();
    const bool needsCapsLockSuppression = IsCapsLockSuppressionEnabled();

    if (g_isShuttingDown.load(std::memory_order_acquire) || (!needsDeepSuppression && !needsExactModifierTracking && !needsCapsLockSuppression)) {
        const HWND targetHwnd = g_subclassedHwnd.load(std::memory_order_acquire);
        ReleaseSuppressedLowLevelRebindKeys(targetHwnd);
        ResetLowLevelExactModifierState();
        UninstallLowLevelKeyboardHook();
        return;
    }

    EnsureLowLevelKeyboardHookInstalled();
}

static uint64_t BuildSyntheticRebindOutputSourceId(DWORD vkCode, DWORD rawVkCode, bool isMouseButton) {
    const uint64_t sourceKindBit = isMouseButton ? (1ull << 32) : 0ull;
    const DWORD sourceVk = isMouseButton ? rawVkCode : (vkCode != 0 ? vkCode : rawVkCode);
    return sourceKindBit | static_cast<uint64_t>(sourceVk);
}

static void TrackSyntheticRebindOutputHold(uint64_t sourceId, UINT outputScanCode) {
    if (sourceId == 0 || outputScanCode == 0) return;

    UINT releaseScanCode = 0;
    bool sendKeyDown = false;
    {
        std::lock_guard<std::mutex> lock(s_activeSyntheticRebindOutputsMutex);

        auto existing = s_activeSyntheticRebindOutputsBySource.find(sourceId);
        if (existing != s_activeSyntheticRebindOutputsBySource.end()) {
            if (existing->second == outputScanCode) {
                return;
            }

            auto oldRef = s_activeSyntheticRebindOutputRefCounts.find(existing->second);
            if (oldRef != s_activeSyntheticRebindOutputRefCounts.end()) {
                if (oldRef->second <= 1) {
                    releaseScanCode = existing->second;
                    s_activeSyntheticRebindOutputRefCounts.erase(oldRef);
                } else {
                    --oldRef->second;
                }
            }

            existing->second = outputScanCode;
        } else {
            s_activeSyntheticRebindOutputsBySource[sourceId] = outputScanCode;
        }

        size_t& refCount = s_activeSyntheticRebindOutputRefCounts[outputScanCode];
        sendKeyDown = (refCount == 0);
        ++refCount;
    }

    if (releaseScanCode != 0) {
        (void)SendSyntheticRebindOutput(releaseScanCode, false);
    }
    if (sendKeyDown) {
        (void)SendSyntheticRebindOutput(outputScanCode, true);
    }
}

static bool ReleaseTrackedSyntheticRebindOutputHold(uint64_t sourceId) {
    if (sourceId == 0) return false;

    UINT releaseScanCode = 0;
    {
        std::lock_guard<std::mutex> lock(s_activeSyntheticRebindOutputsMutex);

        const auto existing = s_activeSyntheticRebindOutputsBySource.find(sourceId);
        if (existing == s_activeSyntheticRebindOutputsBySource.end()) {
            return false;
        }

        releaseScanCode = existing->second;
        s_activeSyntheticRebindOutputsBySource.erase(existing);

        auto refCount = s_activeSyntheticRebindOutputRefCounts.find(releaseScanCode);
        if (refCount == s_activeSyntheticRebindOutputRefCounts.end()) {
            releaseScanCode = 0;
        } else if (refCount->second <= 1) {
            s_activeSyntheticRebindOutputRefCounts.erase(refCount);
        } else {
            --refCount->second;
            releaseScanCode = 0;
        }
    }

    if (releaseScanCode != 0) {
        (void)SendSyntheticRebindOutput(releaseScanCode, false);
    }
    return true;
}

static void ReleaseAllTrackedSyntheticRebindOutputHolds() {
    std::vector<UINT> scanCodesToRelease;
    {
        std::lock_guard<std::mutex> lock(s_activeSyntheticRebindOutputsMutex);
        if (s_activeSyntheticRebindOutputRefCounts.empty()) {
            s_activeSyntheticRebindOutputsBySource.clear();
            return;
        }

        scanCodesToRelease.reserve(s_activeSyntheticRebindOutputRefCounts.size());
        for (const auto& [scanCodeWithFlags, refCount] : s_activeSyntheticRebindOutputRefCounts) {
            if (refCount != 0) {
                scanCodesToRelease.push_back(scanCodeWithFlags);
            }
        }

        s_activeSyntheticRebindOutputsBySource.clear();
        s_activeSyntheticRebindOutputRefCounts.clear();
    }

    for (const UINT scanCodeWithFlags : scanCodesToRelease) {
        (void)SendSyntheticRebindOutput(scanCodeWithFlags, false);
    }
}

static void ReleaseSuppressedLowLevelRebindKeys(HWND hWnd) {
    std::vector<LowLevelSuppressedKeyState> activeKeys;
    {
        std::lock_guard<std::mutex> lock(s_lowLevelSuppressedKeysMutex);
        activeKeys.reserve(s_lowLevelSuppressedKeys.size());
        for (const auto& [vk, state] : s_lowLevelSuppressedKeys) {
            (void)vk;
            activeKeys.push_back(state);
        }
        s_lowLevelSuppressedKeys.clear();
    }

    if (hWnd) {
        for (const auto& state : activeKeys) {
            const UINT msg = state.isSystemKey ? WM_SYSKEYUP : WM_KEYUP;
            const LPARAM msgLParam = BuildKeyboardMessageLParam(state.scanCodeWithFlags, false, state.isSystemKey, 1, true, true);
            const InputHandlerResult result = HandleKeyRebinding(hWnd, msg, static_cast<WPARAM>(state.rawVk), msgLParam);
            if (!result.consumed) {
                (void)PostMessage(hWnd, msg, static_cast<WPARAM>(state.rawVk), msgLParam);
            }
        }
    }
}

void ReleaseActiveLowLevelRebindKeys(HWND hWnd) {
    s_systemAltTabPassthroughActive = false;

    ReleaseSuppressedLowLevelRebindKeys(hWnd);

    ReleaseAllTrackedSyntheticRebindOutputHolds();
}

static bool SendSynthKeyByScanCode(UINT scanCodeWithFlags, bool keyDown) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = 0;
    in.ki.wScan = (WORD)(scanCodeWithFlags & 0xFF);
    DWORD flags = KEYEVENTF_SCANCODE;
    if ((scanCodeWithFlags & 0xFF00) != 0) flags |= KEYEVENTF_EXTENDEDKEY;
    if (!keyDown) flags |= KEYEVENTF_KEYUP;
    in.ki.dwFlags = flags;
    in.ki.time = 0;
    in.ki.dwExtraInfo = kToolscreenInjectedExtraInfo;
    return ::SendInput(1, &in, sizeof(INPUT)) == 1;
}

static bool SendSyntheticRebindOutput(UINT scanCodeWithFlags, bool keyDown) {
#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
    s_syntheticRebindKeyEventsForTests.push_back(SyntheticRebindKeyEventForTest{ scanCodeWithFlags, keyDown });
#endif

    return SendSynthKeyByScanCode(scanCodeWithFlags, keyDown);
}

#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
void ResetSyntheticRebindKeyEventsForTest() { s_syntheticRebindKeyEventsForTests.clear(); }

size_t GetSyntheticRebindKeyEventCountForTest() { return s_syntheticRebindKeyEventsForTests.size(); }

bool GetSyntheticRebindKeyEventForTest(size_t index, UINT& outScanCodeWithFlags, bool& outKeyDown) {
    if (index >= s_syntheticRebindKeyEventsForTests.size()) {
        return false;
    }

    const SyntheticRebindKeyEventForTest& event = s_syntheticRebindKeyEventsForTests[index];
    outScanCodeWithFlags = event.scanCodeWithFlags;
    outKeyDown = event.keyDown;
    return true;
}

size_t GetActiveSyntheticRebindOutputCountForTest() {
    std::lock_guard<std::mutex> lock(s_activeSyntheticRebindOutputsMutex);
    return s_activeSyntheticRebindOutputsBySource.size();
}

void ResetExactKeyboardMessageStateForTest() { ResetExactKeyboardMessageState(); }

void ResetHotkeyRuntimeStateForTest() {
    s_bestMatchKeyCount = 0;
    s_bestMatchKeyCountByMainVk.clear();
    s_activeHoldHotkey.reset();
    s_shiftHotkeyPollState = {};
}

void ResetLowLevelExactModifierStateForTest() { ResetLowLevelExactModifierState(); }

void SetLowLevelExactModifierDownForTest(DWORD vk, bool isDown) {
    if (!IsTrackedSpecificModifierVk(vk)) {
        return;
    }

    std::lock_guard<std::mutex> lock(s_lowLevelExactModifierStateMutex);
    if (isDown) {
        s_lowLevelExactModifierKeysDown.insert(vk);
    } else {
        s_lowLevelExactModifierKeysDown.erase(vk);
    }
}

void SetPhysicalModifierDownForTest(DWORD vk, bool isDown) { s_physicalModifierDownOverridesForTests[vk] = isDown; }

void ResetPhysicalModifierStateForTest() { s_physicalModifierDownOverridesForTests.clear(); }

void QueueSuppressedLowLevelKeyForTest(DWORD vk, UINT scanCodeWithFlags, bool isSystemKey) {
    if (vk == 0) {
        return;
    }

    LowLevelSuppressedKeyState state{};
    state.rawVk = vk;
    state.scanCodeWithFlags = scanCodeWithFlags;
    state.isSystemKey = isSystemKey;

    std::lock_guard<std::mutex> lock(s_lowLevelSuppressedKeysMutex);
    s_lowLevelSuppressedKeys[vk] = state;
}

void QueueLowLevelExactModifierKeydownForTest(DWORD vk) { RecordLowLevelExactModifierEvent(vk, true); }

void QueueLowLevelExactModifierKeyupForTest(DWORD vk) { RecordLowLevelExactModifierEvent(vk, false); }

DWORD ResolveTrackedKeyboardVkFromMessageForTest(UINT uMsg, WPARAM wParam, LPARAM lParam) {
    return ResolveTrackedKeyboardVkFromMessage(uMsg, wParam, lParam);
}
#endif

InputHandlerResult HandleInjectedMenuMaskKey(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP:
        break;
    default:
        return { false, 0 };
    }
    PROFILE_SCOPE("HandleInjectedMenuMaskKey");

    (void)hWnd;
    (void)lParam;

    if (GetMessageExtraInfo() != kToolscreenMenuMaskExtraInfo) { return { false, 0 }; }
    if (static_cast<WORD>(wParam) != kToolscreenMenuMaskVk) { return { false, 0 }; }
    return { true, 0 };
}

static InputHandlerResult ExecuteMatchedKeyRebind(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, DWORD rawVkCode, DWORD vkCode,
                                                  bool isMouseButton, bool isKeyDown, bool isAutoRepeatKeyDown,
                                                  const KeyRebind& rebind) {
    if (IsConsumeOnlyKeyRebind(rebind)) {
        return { true, 0 };
    }

    const bool shiftLayerActive = IsShiftLayerActiveForRebind(rebind, vkCode, rawVkCode, isKeyDown);
    const DWORD defaultTextVK = NormalizeModifierVkFromConfig(rebind.fromKey);
    const DWORD effectiveCustomOutputVk = ResolveEffectiveCustomOutputVk(rebind, shiftLayerActive);
    const DWORD normalizedCustomOutputVk =
        (effectiveCustomOutputVk != 0)
            ? NormalizeModifierVkFromConfig(effectiveCustomOutputVk, (rebind.useCustomOutput ? rebind.customOutputScanCode : 0))
            : 0;
    const DWORD textVK = NormalizeModifierVkFromConfig((effectiveCustomOutputVk != 0) ? effectiveCustomOutputVk : defaultTextVK);
    const bool typedOutputDisabled = IsEffectiveTypedOutputDisabled(rebind, shiftLayerActive);
    const bool preferShiftedText =
        ResolvePreferredOutputShiftState(rebind, shiftLayerActive, IsShiftDownForIncomingEvent(vkCode, rawVkCode, isKeyDown));
    const bool sourceIsScrollWheel =
        IsMouseWheelPseudoVk(rebind.fromKey) || IsMouseWheelPseudoVk(vkCode) || IsMouseWheelPseudoVk(rawVkCode);

    DWORD triggerVK = IsTriggerOutputDisabled(rebind)
                          ? 0
                          : NormalizeModifierVkFromConfig(rebind.toKey, (rebind.useCustomOutput ? rebind.customOutputScanCode : 0));
    if (normalizedCustomOutputVk != 0 && IsNonCharKeyVk(normalizedCustomOutputVk)) {
        triggerVK = normalizedCustomOutputVk;
    }

    const bool isSystemKeyMsg = (uMsg == WM_SYSKEYDOWN || uMsg == WM_SYSKEYUP);
    const bool sourceIsAlt = IsAltVk(vkCode) || IsAltVk(rawVkCode);
    const bool altContextActive = isSystemKeyMsg && !sourceIsAlt;
    const bool outputIsAlt = IsAltVk(triggerVK);
    const bool outputHasAltContext = altContextActive || outputIsAlt;
    const bool outputUsesSystemMessage = outputHasAltContext;
    const UINT outputCharMsg = outputHasAltContext ? WM_SYSCHAR : WM_CHAR;

    if (ShouldMaskMenuModifierForRebind(rebind, vkCode, rawVkCode, isKeyDown, isAutoRepeatKeyDown, triggerVK)) {
        (void)SendMenuMaskKeyTap();
    }

    UINT outputScanCode = GetScanCodeWithExtendedFlag(triggerVK);
    if (rebind.useCustomOutput && rebind.customOutputScanCode != 0) {
        outputScanCode = ResolveOutputScanCode(triggerVK, rebind.customOutputScanCode);
    }
    const bool outputScanIsModifier = IsModifierScanCode(outputScanCode);

    auto buildMouseKeyState = [&](DWORD buttonVk, bool buttonDown) -> WORD {
        WORD mk = 0;
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) mk |= MK_CONTROL;
        if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) mk |= MK_SHIFT;

        auto setBtn = [&](int vk, WORD mask, bool isThisButton) {
            bool down = (GetKeyState(vk) & 0x8000) != 0;
            if (isThisButton) down = buttonDown;
            if (down) mk |= mask;
        };

        setBtn(VK_LBUTTON, MK_LBUTTON, buttonVk == VK_LBUTTON);
        setBtn(VK_RBUTTON, MK_RBUTTON, buttonVk == VK_RBUTTON);
        setBtn(VK_MBUTTON, MK_MBUTTON, buttonVk == VK_MBUTTON);
        setBtn(VK_XBUTTON1, MK_XBUTTON1, buttonVk == VK_XBUTTON1);
        setBtn(VK_XBUTTON2, MK_XBUTTON2, buttonVk == VK_XBUTTON2);
        return mk;
    };

    auto resolveMouseButtonLParam = [&]() -> LPARAM {
        if (isMouseButton && uMsg != WM_MOUSEWHEEL && uMsg != WM_MOUSEHWHEEL) {
            return lParam;
        }

        POINT pt{};
        if (GetCursorPos(&pt) && ScreenToClient(hWnd, &pt)) {
            return MAKELPARAM(pt.x, pt.y);
        }

        RECT clientRect{};
        if (GetClientRect(hWnd, &clientRect)) {
            return MAKELPARAM((clientRect.right - clientRect.left) / 2, (clientRect.bottom - clientRect.top) / 2);
        }

        return lParam;
    };

    auto resolveMouseWheelLParam = [&]() -> LPARAM {
        if (uMsg == WM_MOUSEWHEEL || uMsg == WM_MOUSEHWHEEL) {
            return lParam;
        }

        POINT pt{};
        if (GetCursorPos(&pt)) {
            return MAKELPARAM(pt.x, pt.y);
        }

        RECT clientRect{};
        if (GetClientRect(hWnd, &clientRect)) {
            POINT center{ (clientRect.right - clientRect.left) / 2, (clientRect.bottom - clientRect.top) / 2 };
            if (ClientToScreen(hWnd, &center)) {
                return MAKELPARAM(center.x, center.y);
            }
        }

        return lParam;
    };

    const bool fromKeyIsNonChar = isMouseButton || IsNonCharKeyVk(rebind.fromKey);
    auto emitTypedCharForSource = [&](LPARAM charLParam) {
        if (typedOutputDisabled) {
            return;
        }
        EmitRebindTypedChar(hWnd, rebind, shiftLayerActive, textVK, preferShiftedText, outputCharMsg, charLParam);
    };

    if (triggerVK == 0) {
        if (isKeyDown && fromKeyIsNonChar) {
            const UINT textScanCode = GetScanCodeWithExtendedFlag((effectiveCustomOutputVk != 0) ? textVK : defaultTextVK);
            const LPARAM charLParam = BuildKeyboardMessageLParam(textScanCode, true, outputHasAltContext, 1, false, false);
            emitTypedCharForSource(charLParam);
        }
        return { true, 0 };
    }

    if (IsMouseLikeVk(triggerVK)) {
        LRESULT mouseResult = 0;
        const bool fromKeyIsNonCharMouse = fromKeyIsNonChar;

        if (IsMouseWheelPseudoVk(triggerVK)) {
            const WORD mkState = buildMouseKeyState(0, false);
            const WPARAM newWParam = MAKEWPARAM(mkState, static_cast<WORD>(GetMouseWheelDeltaForPseudoVk(triggerVK)));
            mouseResult = CallWindowProc(g_originalWndProc, hWnd, WM_MOUSEWHEEL, newWParam, resolveMouseWheelLParam());
        } else {
            const LPARAM mouseLParam = resolveMouseButtonLParam();
            auto dispatchMouseButton = [&](bool buttonDown) {
                UINT newMsg = 0;
                WORD mkState = buildMouseKeyState(triggerVK, buttonDown);
                WPARAM newWParam = mkState;

                if (triggerVK == VK_LBUTTON) {
                    newMsg = buttonDown ? WM_LBUTTONDOWN : WM_LBUTTONUP;
                } else if (triggerVK == VK_RBUTTON) {
                    newMsg = buttonDown ? WM_RBUTTONDOWN : WM_RBUTTONUP;
                } else if (triggerVK == VK_MBUTTON) {
                    newMsg = buttonDown ? WM_MBUTTONDOWN : WM_MBUTTONUP;
                } else if (triggerVK == VK_XBUTTON1) {
                    newMsg = buttonDown ? WM_XBUTTONDOWN : WM_XBUTTONUP;
                    newWParam = MAKEWPARAM(mkState, XBUTTON1);
                } else if (triggerVK == VK_XBUTTON2) {
                    newMsg = buttonDown ? WM_XBUTTONDOWN : WM_XBUTTONUP;
                    newWParam = MAKEWPARAM(mkState, XBUTTON2);
                }

                if (newMsg != 0) {
                    mouseResult = CallWindowProc(g_originalWndProc, hWnd, newMsg, newWParam, mouseLParam);
                }
            };

            if (sourceIsScrollWheel) {
                dispatchMouseButton(true);
                dispatchMouseButton(false);
            } else {
                dispatchMouseButton(isKeyDown);
            }
        }

        if (isKeyDown && fromKeyIsNonCharMouse) {
            const UINT textScanCode = GetScanCodeWithExtendedFlag(textVK);
            LPARAM charLParam = BuildKeyboardMessageLParam(textScanCode, true, outputHasAltContext, 1, false, false);
            emitTypedCharForSource(charLParam);
        }

        return { true, mouseResult };
    }
    UINT outputMsg = isKeyDown ? (outputUsesSystemMessage ? WM_SYSKEYDOWN : WM_KEYDOWN)
                               : (outputUsesSystemMessage ? WM_SYSKEYUP : WM_KEYUP);

    UINT repeatCount = 1;
    bool previousState = !isKeyDown;
    bool transitionState = !isKeyDown;
    if (!isMouseButton) {
        repeatCount = static_cast<UINT>(lParam & 0xFFFF);
        if (repeatCount == 0) repeatCount = 1;

        previousState = ((lParam & (1LL << 30)) != 0);
        transitionState = ((lParam & (1LL << 31)) != 0);
    }

    if (IsModifierVk(triggerVK) || outputScanIsModifier) {
        const uint64_t syntheticOutputSourceId = BuildSyntheticRebindOutputSourceId(vkCode, rawVkCode, isMouseButton);
        const bool sourceIsModifier = IsModifierVk(rebind.fromKey) || IsModifierVk(vkCode) || IsModifierVk(rawVkCode);
        if (isAutoRepeatKeyDown && !sourceIsModifier) {
            return { true, 0 };
        }

        if (sourceIsScrollWheel) {
            (void)SendSyntheticRebindOutput(outputScanCode, true);
            (void)SendSyntheticRebindOutput(outputScanCode, false);
        } else {
            if (isKeyDown) {
                TrackSyntheticRebindOutputHold(syntheticOutputSourceId, outputScanCode);
            } else if (!ReleaseTrackedSyntheticRebindOutputHold(syntheticOutputSourceId)) {
                (void)SendSyntheticRebindOutput(outputScanCode, false);
            }
        }

        if (isKeyDown && fromKeyIsNonChar && !outputScanIsModifier) {
            const UINT textScanCode = GetScanCodeWithExtendedFlag(textVK);
            LPARAM charLParam =
                BuildKeyboardMessageLParam(textScanCode, true, outputHasAltContext, repeatCount, previousState, transitionState);
            emitTypedCharForSource(charLParam);
        }

        return { true, 0 };
    }

    const DWORD msgVk = [&]() -> DWORD {
        if (triggerVK == VK_LSHIFT || triggerVK == VK_RSHIFT) return VK_SHIFT;
        if (triggerVK == VK_LCONTROL || triggerVK == VK_RCONTROL) return VK_CONTROL;
        if (triggerVK == VK_LMENU || triggerVK == VK_RMENU) return VK_MENU;
        return triggerVK;
    }();

    if (sourceIsScrollWheel) {
        const UINT keyDownMsg = outputUsesSystemMessage ? WM_SYSKEYDOWN : WM_KEYDOWN;
        const UINT keyUpMsg = outputUsesSystemMessage ? WM_SYSKEYUP : WM_KEYUP;
        const LPARAM keyDownLParam = BuildKeyboardMessageLParam(outputScanCode, true, outputHasAltContext, 1, false, false);
        LRESULT keyResult = CallWindowProc(g_originalWndProc, hWnd, keyDownMsg, msgVk, keyDownLParam);
        const LPARAM keyUpLParam = BuildKeyboardMessageLParam(outputScanCode, false, outputHasAltContext, 1, true, true);
        keyResult = CallWindowProc(g_originalWndProc, hWnd, keyUpMsg, msgVk, keyUpLParam);

        if (fromKeyIsNonChar) {
            emitTypedCharForSource(keyDownLParam);
        }

        return { true, keyResult };
    }

    LPARAM newLParam =
        BuildKeyboardMessageLParam(outputScanCode, isKeyDown, outputHasAltContext, repeatCount, previousState, transitionState);

    LRESULT keyResult = CallWindowProc(g_originalWndProc, hWnd, outputMsg, msgVk, newLParam);

    if (isKeyDown && fromKeyIsNonChar) {
        emitTypedCharForSource(newLParam);
    }

    return { true, keyResult };
}

InputHandlerResult HandleKeyRebinding(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP:
    case WM_MOUSEWHEEL:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        break;
    default:
        return { false, 0 };
    }
    PROFILE_SCOPE("HandleKeyRebinding");

    ScopedExactKeyboardMessageState scopedKeyboardMessageState(uMsg, wParam, lParam);

    if (uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN || uMsg == WM_KEYUP || uMsg == WM_SYSKEYUP) {
        if (GetMessageExtraInfo() == kToolscreenInjectedExtraInfo) {
            return { false, 0 };
        }
    }

    DWORD rawVkCode = 0;
    DWORD vkCode = 0;
    bool isMouseButton = false;
    bool isKeyDown = false;

    if (uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN) {
        rawVkCode = static_cast<DWORD>(wParam);
        isKeyDown = true;
    } else if (uMsg == WM_KEYUP || uMsg == WM_SYSKEYUP) {
        rawVkCode = static_cast<DWORD>(wParam);
        isKeyDown = false;
    } else if (uMsg == WM_MOUSEWHEEL) {
        const short wheelDelta = GET_WHEEL_DELTA_WPARAM(wParam);
        if (wheelDelta == 0) return { false, 0 };
        rawVkCode = (wheelDelta > 0) ? VK_TOOLSCREEN_SCROLL_UP : VK_TOOLSCREEN_SCROLL_DOWN;
        isMouseButton = true;
        isKeyDown = true;
    } else if (uMsg == WM_XBUTTONDOWN) {
        WORD xButton = GET_XBUTTON_WPARAM(wParam);
        rawVkCode = (xButton == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
        isMouseButton = true;
        isKeyDown = true;
    } else if (uMsg == WM_XBUTTONUP) {
        WORD xButton = GET_XBUTTON_WPARAM(wParam);
        rawVkCode = (xButton == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
        isMouseButton = true;
        isKeyDown = false;
    } else if (uMsg == WM_LBUTTONDOWN) {
        rawVkCode = VK_LBUTTON;
        isMouseButton = true;
        isKeyDown = true;
    } else if (uMsg == WM_LBUTTONUP) {
        rawVkCode = VK_LBUTTON;
        isMouseButton = true;
        isKeyDown = false;
    } else if (uMsg == WM_RBUTTONDOWN) {
        rawVkCode = VK_RBUTTON;
        isMouseButton = true;
        isKeyDown = true;
    } else if (uMsg == WM_RBUTTONUP) {
        rawVkCode = VK_RBUTTON;
        isMouseButton = true;
        isKeyDown = false;
    } else if (uMsg == WM_MBUTTONDOWN) {
        rawVkCode = VK_MBUTTON;
        isMouseButton = true;
        isKeyDown = true;
    } else if (uMsg == WM_MBUTTONUP) {
        rawVkCode = VK_MBUTTON;
        isMouseButton = true;
        isKeyDown = false;
    } else {
        return { false, 0 };
    }

    vkCode = rawVkCode;
    if (!isMouseButton && (uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN || uMsg == WM_KEYUP || uMsg == WM_SYSKEYUP)) {
        vkCode = ResolveEffectiveKeyboardVkForMessage(uMsg, wParam, lParam);
    }

    const uint64_t syntheticOutputSourceId = BuildSyntheticRebindOutputSourceId(vkCode, rawVkCode, isMouseButton);
    if (!isKeyDown && ReleaseTrackedSyntheticRebindOutputHold(syntheticOutputSourceId)) {
        return { true, 0 };
    }

    if (isMouseButton && g_showGui.load(std::memory_order_acquire)) { return { false, 0 }; }

    const bool isAutoRepeatKeyDown = !isMouseButton && isKeyDown && IsTrackedExactAutoRepeatKeyDown(uMsg, wParam, lParam);

    // Use config snapshot for thread-safe access to key rebinds
    auto rebindCfg = GetConfigSnapshot();
    if (!rebindCfg || !rebindCfg->keyRebinds.enabled) { return { false, 0 }; }
    const bool cursorVisible = IsCursorVisible();

    const bool allowSystemAltTab = !isMouseButton && rebindCfg->keyRebinds.allowSystemAltTab;
    const bool currentIsAlt = !isMouseButton && (IsAltVk(vkCode) || IsAltVk(rawVkCode));
    const bool currentIsTab = !isMouseButton && (IsTabVk(vkCode) || IsTabVk(rawVkCode));

    if (allowSystemAltTab && currentIsTab && isKeyDown && IsAltCurrentlyDown()) {
        s_systemAltTabPassthroughActive = true;
    }

    if (allowSystemAltTab && currentIsTab && s_systemAltTabPassthroughActive) {
        if (!isKeyDown) {
            UpdateSystemAltTabPassthroughState();
        }
        return { false, 0 };
    }

    const KeyRebind* matchedRebind = FindPreferredEnabledKeyRebind(
        rebindCfg->keyRebinds.rebinds,
        cursorVisible,
        [&](const KeyRebind& rebind) { return MatchesRebindSourceKey(vkCode, rawVkCode, rebind.fromKey); });
    if (matchedRebind != nullptr) {
        return ExecuteMatchedKeyRebind(hWnd, uMsg, wParam, lParam, rawVkCode, vkCode, isMouseButton, isKeyDown,
                                       isAutoRepeatKeyDown, *matchedRebind);
    }

    if (allowSystemAltTab && !isKeyDown && (currentIsAlt || currentIsTab)) {
        UpdateSystemAltTabPassthroughState();
    }

    return { false, 0 };
}

InputHandlerResult HandleCustomKeyNoRebind(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg != WM_TOOLSCREEN_KEYDOWN_NO_REBIND && uMsg != WM_TOOLSCREEN_KEYUP_NO_REBIND) { return { false, 0 }; }
    PROFILE_SCOPE("HandleCustomKeyNoRebind");

    const UINT forwardedMsg = (uMsg == WM_TOOLSCREEN_KEYDOWN_NO_REBIND) ? WM_KEYDOWN : WM_KEYUP;

    if (g_showGui.load()) {
        ImGuiInputQueue_EnqueueWin32Message(hWnd, forwardedMsg, wParam, lParam);
        return { true, 1 };
    }

    if (g_originalWndProc) { return { true, CallWindowProc(g_originalWndProc, hWnd, forwardedMsg, wParam, lParam) }; }
    return { true, DefWindowProc(hWnd, forwardedMsg, wParam, lParam) };
}

InputHandlerResult HandleCustomCharNoRebind(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg != WM_TOOLSCREEN_CHAR_NO_REBIND) { return { false, 0 }; }
    PROFILE_SCOPE("HandleCustomCharNoRebind");

    const UINT forwardedMsg = ((lParam & (static_cast<LPARAM>(1) << 29)) != 0) ? WM_SYSCHAR : WM_CHAR;

    if (g_showGui.load()) {
        ImGuiInputQueue_EnqueueWin32Message(hWnd, forwardedMsg, wParam, lParam);
        return { true, 1 };
    }

    if (g_originalWndProc) { return { true, CallWindowProc(g_originalWndProc, hWnd, forwardedMsg, wParam, lParam) }; }
    return { true, DefWindowProc(hWnd, forwardedMsg, wParam, lParam) };
}

InputHandlerResult HandleCharRebinding(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg != WM_CHAR && uMsg != WM_SYSCHAR) { return { false, 0 }; }
    PROFILE_SCOPE("HandleCharRebinding");

    auto charRebindCfg = GetConfigSnapshot();
    if (!charRebindCfg || !charRebindCfg->keyRebinds.enabled) { return { false, 0 }; }
    const bool logHotkeyDebug = charRebindCfg->debug.showHotkeyDebug;
    const bool cursorVisible = IsCursorVisible();

    WCHAR inputChar = static_cast<WCHAR>(wParam);

    for (int pass = 0; pass < 2; ++pass) {
        const KeyRebindCursorStateMatchPriority passPriority =
            (pass == 0) ? KeyRebindCursorStateMatchPriority::Exact : KeyRebindCursorStateMatchPriority::Any;

        for (const auto& rebind : charRebindCfg->keyRebinds.rebinds) {
            if (!rebind.enabled || rebind.fromKey == 0) continue;
            if (rebind.toKey == 0 && !IsConsumeOnlyKeyRebind(rebind)) continue;
            if (GetKeyRebindCursorStateMatchPriority(rebind, cursorVisible) != passPriority) continue;

            WCHAR fromUnshifted = 0;
            WCHAR fromShifted = 0;
            bool hasFromUnshifted = TryTranslateVkToChar(rebind.fromKey, false, fromUnshifted);
            bool hasFromShifted = TryTranslateVkToChar(rebind.fromKey, true, fromShifted);

            bool matched = false;

            if (hasFromUnshifted && inputChar == fromUnshifted) {
                matched = true;
            } else if (hasFromShifted && inputChar == fromShifted) {
                matched = true;
            }

            if (matched) {
                if (IsConsumeOnlyKeyRebind(rebind)) {
                    return { true, 0 };
                }

                const bool shiftDown = IsShiftCurrentlyDown();
                const bool shiftLayerActive = IsShiftLayerActive(rebind, shiftDown, IsCapsLockCurrentlyOn());
                const bool typedOutputDisabled = IsEffectiveTypedOutputDisabled(rebind, shiftLayerActive);
                const bool preferShiftedText = ResolvePreferredOutputShiftState(rebind, shiftLayerActive, shiftDown);

                if (typedOutputDisabled) {
                    return { true, 0 };
                }

                if (shiftLayerActive && HasShiftLayerOutputUnicode(rebind)) {
                    LRESULT r = SendUnicodeScalarAsCharMessage(hWnd, uMsg, (uint32_t)rebind.shiftLayerOutputUnicode, lParam);
                    return { true, r };
                }

                if (!shiftLayerActive && rebind.useCustomOutput && rebind.customOutputUnicode != 0) {
                    LRESULT r = SendUnicodeScalarAsCharMessage(hWnd, uMsg, (uint32_t)rebind.customOutputUnicode, lParam);
                    return { true, r };
                }

                DWORD outputVK = ResolveEffectiveCustomOutputVk(rebind, shiftLayerActive);

                if (outputVK == 0) {
                    if (RebindCannotType(rebind)) {
                        if (logHotkeyDebug) {
                            Log("[REBIND WM_CHAR] Consuming char code " + std::to_string(static_cast<unsigned int>(inputChar)) +
                                " (trigger cannot type)");
                        }
                        return { true, 0 };
                    }
                    return { false, 0 };
                }

                outputVK = NormalizeModifierVkFromConfig(outputVK);

                WCHAR outputChar = 0;
                if (outputVK == VK_RETURN) {
                    outputChar = L'\r';
                } else if (outputVK == VK_TAB) {
                    outputChar = L'\t';
                } else if (outputVK == VK_BACK) {
                    outputChar = L'\b';
                } else {
                    BYTE ks[256] = {};
                    if (GetKeyboardState(ks)) {
                        ApplyPreferredOutputShiftState(rebind, shiftLayerActive, ks);
                        (void)TryTranslateVkToCharWithKeyboardState(outputVK, ks, outputChar);
                    }

                    if (outputChar == 0) {
                        (void)TryTranslateVkToCharPreferShiftState(outputVK, preferShiftedText, outputChar);
                    }
                }

                if (outputChar == 0) {
                    if (logHotkeyDebug) {
                        Log("[REBIND WM_CHAR] Consuming char code " + std::to_string(static_cast<unsigned int>(inputChar)) +
                            " (output VK has no WM_CHAR)");
                    }
                    return { true, 0 };
                }

                if (logHotkeyDebug) {
                    Log("[REBIND WM_CHAR] Remapping char code " + std::to_string(static_cast<unsigned int>(inputChar)) + " -> " +
                        std::to_string(static_cast<unsigned int>(outputChar)));
                }

                return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, outputChar, lParam) };
            }
        }
    }
    return { false, 0 };
}

static void ResolveHotkeyPriority(UINT uMsg, WPARAM wParam, LPARAM lParam) {
    DWORD vkCode = 0;
    bool isKeyDownMessage = false;
    bool isKeyUpMessage = false;
    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        vkCode = ResolveEffectiveKeyboardVkForMessage(uMsg, wParam, lParam);
        isKeyDownMessage = true;
        break;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        vkCode = ResolveEffectiveKeyboardVkForMessage(uMsg, wParam, lParam);
        isKeyUpMessage = true;
        break;
    case WM_LBUTTONDOWN:
        vkCode = VK_LBUTTON;
        isKeyDownMessage = true;
        break;
    case WM_RBUTTONDOWN:
        vkCode = VK_RBUTTON;
        isKeyDownMessage = true;
        break;
    case WM_MBUTTONDOWN:
        vkCode = VK_MBUTTON;
        isKeyDownMessage = true;
        break;
    case WM_LBUTTONUP:
        vkCode = VK_LBUTTON;
        isKeyUpMessage = true;
        break;
    case WM_RBUTTONUP:
        vkCode = VK_RBUTTON;
        isKeyUpMessage = true;
        break;
    case WM_MBUTTONUP:
        vkCode = VK_MBUTTON;
        isKeyUpMessage = true;
        break;
    case WM_XBUTTONDOWN:
        vkCode = (GET_XBUTTON_WPARAM(wParam) == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
        isKeyDownMessage = true;
        break;
    case WM_XBUTTONUP:
        vkCode = (GET_XBUTTON_WPARAM(wParam) == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
        isKeyUpMessage = true;
        break;
    default:
        s_bestMatchKeyCount = 0;
        return;
    }

    if (isKeyUpMessage) {
        auto it = s_bestMatchKeyCountByMainVk.find(vkCode);
        s_bestMatchKeyCount = (it != s_bestMatchKeyCountByMainVk.end()) ? it->second : 0;
        if (it != s_bestMatchKeyCountByMainVk.end()) {
            s_bestMatchKeyCountByMainVk.erase(it);
        }
        return;
    }

    if (!isKeyDownMessage) {
        s_bestMatchKeyCount = 0;
        return;
    }

    s_bestMatchKeyCount = 0;

    { // Skip resolution entirely for keys that aren't bound to any hotkey
        std::lock_guard<std::mutex> lock(g_hotkeyMainKeysMutex);
        if (g_hotkeyMainKeys.find(vkCode) == g_hotkeyMainKeys.end()) {
            s_bestMatchKeyCountByMainVk.erase(vkCode);
            return;
        }
    }

    auto check = [&](const std::vector<DWORD>& keys, const std::vector<DWORD>& exclusions = {}) {
        if (!keys.empty() && CheckHotkeyMatch(keys, vkCode, exclusions, false))
            s_bestMatchKeyCount = (std::max)(s_bestMatchKeyCount, keys.size());
    };

    check(g_config.guiHotkey);
    check(g_config.borderlessHotkey);
    check(g_config.imageOverlaysHotkey);
    check(g_config.windowOverlaysHotkey);
    check(g_config.ninjabrainOverlayHotkey);
    check(g_config.keyRebinds.toggleHotkey);

    for (const auto& hk : g_config.hotkeys) {
        check(hk.keys, hk.conditions.exclusions);
        for (const auto& alt : hk.altSecondaryModes)
            check(alt.keys, hk.conditions.exclusions);
    }

    for (const auto& sh : g_config.sensitivityHotkeys)
        check(sh.keys, sh.conditions.exclusions);

    s_bestMatchKeyCountByMainVk[vkCode] = s_bestMatchKeyCount;
}

static bool KeyComboUsesExactShift(const std::vector<DWORD>& keys) {
    for (DWORD key : keys) {
        if (key == VK_LSHIFT || key == VK_RSHIFT) {
            return true;
        }
    }

    return false;
}

static bool ConfigUsesExactShiftHotkeys(const Config& cfg) {
    auto usesExactShift = [](const std::vector<DWORD>& keys) { return KeyComboUsesExactShift(keys); };

    if (usesExactShift(cfg.guiHotkey) || usesExactShift(cfg.borderlessHotkey) || usesExactShift(cfg.imageOverlaysHotkey) ||
        usesExactShift(cfg.windowOverlaysHotkey) || usesExactShift(cfg.ninjabrainOverlayHotkey) ||
        usesExactShift(cfg.keyRebinds.toggleHotkey)) {
        return true;
    }

    for (const auto& hotkey : cfg.hotkeys) {
        if (usesExactShift(hotkey.keys)) {
            return true;
        }

        for (const auto& alt : hotkey.altSecondaryModes) {
            if (usesExactShift(alt.keys)) {
                return true;
            }
        }
    }

    for (const auto& hotkey : cfg.sensitivityHotkeys) {
        if (usesExactShift(hotkey.keys)) {
            return true;
        }
    }

    return false;
}

static void ResetShiftHotkeyPollingState(HWND hWnd) {
    const HWND targetHwnd = hWnd ? hWnd : g_subclassedHwnd.load(std::memory_order_acquire);
    if (s_shiftHotkeyPollState.timerArmed && targetHwnd && IsWindow(targetHwnd)) {
        (void)KillTimer(targetHwnd, kToolscreenShiftHotkeyPollTimerId);
    }

    s_shiftHotkeyPollState = {};
}

static void UpdateShiftHotkeyPollingState(HWND hWnd) {
    auto cfgSnap = GetConfigSnapshot();
    const bool shouldEnable = cfgSnap && ConfigUsesExactShiftHotkeys(*cfgSnap);
    if (!shouldEnable || !hWnd || !IsWindow(hWnd)) {
        ResetShiftHotkeyPollingState(hWnd);
        return;
    }

    const bool leftDown = IsPhysicalShiftKeyDown(VK_LSHIFT);
    const bool rightDown = IsPhysicalShiftKeyDown(VK_RSHIFT);
    const bool wasEnabled = s_shiftHotkeyPollState.exactShiftHotkeysEnabled;

    if (!s_shiftHotkeyPollState.timerArmed) {
        if (SetTimer(hWnd, kToolscreenShiftHotkeyPollTimerId, 10, NULL) == 0) {
            if (cfgSnap->debug.showHotkeyDebug) {
                Log("[Hotkey] WARNING: Failed to arm sided-shift polling timer");
            }
            s_shiftHotkeyPollState = {};
            return;
        }
        s_shiftHotkeyPollState.timerArmed = true;
    }

    s_shiftHotkeyPollState.exactShiftHotkeysEnabled = true;
    if (!wasEnabled) {
        s_shiftHotkeyPollState.lastLeftDown = leftDown;
        s_shiftHotkeyPollState.lastRightDown = rightDown;
    }
}

static void SyncShiftHotkeyPollingStateFromMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (!s_shiftHotkeyPollState.exactShiftHotkeysEnabled || !IsTrackedExactKeyboardMessage(uMsg)) {
        return;
    }

    const DWORD vkCode = ResolveEffectiveKeyboardVkForMessage(uMsg, wParam, lParam);
    if (vkCode != VK_LSHIFT && vkCode != VK_RSHIFT) {
        return;
    }

    s_shiftHotkeyPollState.lastLeftDown = IsPhysicalShiftKeyDown(VK_LSHIFT);
    s_shiftHotkeyPollState.lastRightDown = IsPhysicalShiftKeyDown(VK_RSHIFT);
}

static InputHandlerResult DispatchSyntheticShiftHotkeyEvent(HWND hWnd, DWORD vkCode, bool isKeyDown, bool previousWasDown) {
    const UINT uMsg = isKeyDown ? WM_KEYDOWN : WM_KEYUP;
    const LPARAM lParam =
        BuildKeyboardMessageLParam(GetScanCodeWithExtendedFlag(vkCode), isKeyDown, false, 1, previousWasDown, !isKeyDown);

    ScopedExactKeyboardMessageState scopedKeyboardMessageState(uMsg, static_cast<WPARAM>(vkCode), lParam);

    ResolveHotkeyPriority(uMsg, static_cast<WPARAM>(vkCode), lParam);

    InputHandlerResult result = HandleBorderlessToggle(hWnd, uMsg, static_cast<WPARAM>(vkCode), lParam);
    if (result.consumed) return result;

    result = HandleImageOverlaysToggle(hWnd, uMsg, static_cast<WPARAM>(vkCode), lParam);
    if (result.consumed) return result;

    result = HandleWindowOverlaysToggle(hWnd, uMsg, static_cast<WPARAM>(vkCode), lParam);
    if (result.consumed) return result;

    result = HandleNinjabrainOverlayToggle(hWnd, uMsg, static_cast<WPARAM>(vkCode), lParam);
    if (result.consumed) return result;

    result = HandleKeyRebindsToggle(hWnd, uMsg, static_cast<WPARAM>(vkCode), lParam);
    if (result.consumed) return result;

    result = HandleGuiToggle(hWnd, uMsg, static_cast<WPARAM>(vkCode), lParam);
    if (result.consumed) return result;

    const std::string currentModeId = g_modeIdBuffers[g_currentModeIdIndex.load(std::memory_order_acquire)];
    const std::string localGameState = g_gameStateBuffers[g_currentGameStateIndex.load(std::memory_order_acquire)];
    return HandleHotkeys(hWnd, uMsg, static_cast<WPARAM>(vkCode), lParam, currentModeId, localGameState);
}

static InputHandlerResult HandleShiftHotkeyPolling(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg != WM_TIMER || static_cast<UINT_PTR>(wParam) != kToolscreenShiftHotkeyPollTimerId) {
        return { false, 0 };
    }

    if (!s_shiftHotkeyPollState.exactShiftHotkeysEnabled) {
        ResetShiftHotkeyPollingState(hWnd);
        return { true, 0 };
    }

    const bool leftDownNow = IsPhysicalShiftKeyDown(VK_LSHIFT);
    const bool rightDownNow = IsPhysicalShiftKeyDown(VK_RSHIFT);
    const bool leftChanged = leftDownNow != s_shiftHotkeyPollState.lastLeftDown;
    const bool rightChanged = rightDownNow != s_shiftHotkeyPollState.lastRightDown;

    if (!leftChanged && !rightChanged) {
        return { true, 0 };
    }

    auto cfgSnap = GetConfigSnapshot();
    const bool logHotkeyDebug = cfgSnap && cfgSnap->debug.showHotkeyDebug;
    auto processTransition = [&](DWORD vkCode, bool wasDown, bool isDownNow, const char* label, InputHandlerResult& lastResult,
                                 bool& anyConsumed) {
        if (logHotkeyDebug) {
            Log(std::string("[Hotkey] shift poll dispatch ") + label + (isDownNow ? " down" : " up"));
        }
        lastResult = DispatchSyntheticShiftHotkeyEvent(hWnd, vkCode, isDownNow, wasDown);
        anyConsumed = anyConsumed || lastResult.consumed;
    };

    InputHandlerResult lastResult{ false, 0 };
    bool anyConsumed = false;

    if (s_shiftHotkeyPollState.lastLeftDown && !leftDownNow) {
        processTransition(VK_LSHIFT, true, false, "LShift", lastResult, anyConsumed);
    }
    if (s_shiftHotkeyPollState.lastRightDown && !rightDownNow) {
        processTransition(VK_RSHIFT, true, false, "RShift", lastResult, anyConsumed);
    }
    if (!s_shiftHotkeyPollState.lastLeftDown && leftDownNow) {
        processTransition(VK_LSHIFT, false, true, "LShift", lastResult, anyConsumed);
    }
    if (!s_shiftHotkeyPollState.lastRightDown && rightDownNow) {
        processTransition(VK_RSHIFT, false, true, "RShift", lastResult, anyConsumed);
    }

    s_shiftHotkeyPollState.lastLeftDown = leftDownNow;
    s_shiftHotkeyPollState.lastRightDown = rightDownNow;
    return { true, anyConsumed ? lastResult.result : 0 };
}

LRESULT CALLBACK SubclassedWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    PROFILE_SCOPE("SubclassedWndProc");

    const bool isLocalRepeatTagged = HasLocalKeyRepeatTag(uMsg, lParam);
    if (isLocalRepeatTagged) {
        lParam = StripLocalKeyRepeatTag(lParam);
    }

    const HWND expectedHwnd = g_subclassedHwnd.load();
    if (expectedHwnd != NULL && hWnd != expectedHwnd) {
        return DefWindowProc(hWnd, uMsg, wParam, lParam);
    }

    ScopedExactKeyboardMessageState scopedKeyboardMessageState(uMsg, wParam, lParam);

    const bool guiOpen = g_showGui.load(std::memory_order_acquire);
    if (guiOpen && g_forceVisibleCursorWhileGuiOpen.load(std::memory_order_acquire) && g_gameVersion >= GameVersion(1, 13, 0)) {
        EnsureSystemCursorVisible();
        static HCURSOR s_arrowCursor = LoadCursorW(NULL, IDC_ARROW);
        SetCursor(s_arrowCursor);
    }
    if (!guiOpen && g_forceVisibleCursorWhileGuiOpen.load(std::memory_order_acquire)) {
        EnsureSystemCursorHidden();
        g_forceVisibleCursorWhileGuiOpen.store(false, std::memory_order_release);
    }

    UpdateLowLevelKeyboardHookInstalledState();
    UpdateShiftHotkeyPollingState(hWnd);
    SyncShiftHotkeyPollingStateFromMessage(uMsg, wParam, lParam);

    InputHandlerResult result;

    result = HandleShiftHotkeyPolling(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleLocalKeyRepeat(hWnd, uMsg, wParam, lParam, isLocalRepeatTagged);
    if (result.consumed) return result.result;

    if (!isLocalRepeatTagged) {
        RegisterBindingInputEvent(uMsg, wParam, lParam);
    }

    // Keep all window metrics/cache updates in one place to avoid split-brain resize state.
    SyncWindowMetricsFromMessage(hWnd, uMsg, wParam, lParam);

    result = HandleShutdownCheck(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleWindowValidation(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleToolscreenQueryMessages(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    if (uMsg == WM_TOOLSCREEN_APPLY_FOCUS_REGAIN_SIZE) {
        s_deferredFocusRegainWmSizePending.store(false, std::memory_order_relaxed);
        ResendCurrentModeWmSize(hWnd, "input_hook:focus_regain_deferred");
        return 0;
    }

    result = HandleInjectedMenuMaskKey(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP:
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
        ResolveHotkeyPriority(uMsg, wParam, lParam);
        break;
    default:
        s_bestMatchKeyCount = 0;
        break;
    }

    result = HandleBorderlessToggle(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleImageOverlaysToggle(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;
    result = HandleWindowOverlaysToggle(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;
    result = HandleNinjabrainOverlayToggle(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;
    result = HandleKeyRebindsToggle(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    //HandleCharLogging(uMsg, wParam, lParam);

    result = HandleAltF4(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleConfigLoadFailure(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    if (uMsg == WM_SETCURSOR) {
        const std::string localGameState = g_gameStateBuffers[g_currentGameStateIndex.load(std::memory_order_acquire)];
        result = HandleSetCursor(hWnd, uMsg, wParam, lParam, localGameState);
        if (result.consumed) return result.result;
    }

    result = HandleDestroy(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    if (g_isShuttingDown.load()) { return CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam); }

    result = HandleImGuiInput(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleGuiToggle(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleWindowOverlayKeyboard(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleWindowOverlayMouse(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleGuiInputBlocking(uMsg);
    if (result.consumed) return result.result;

    result = HandleActivate(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    if (uMsg == WM_SIZE) {
        const std::string currentModeId = g_modeIdBuffers[g_currentModeIdIndex.load(std::memory_order_acquire)];
        result = HandleWmSizeModeDimensions(hWnd, uMsg, wParam, lParam, currentModeId);
        if (result.consumed) return result.result;
    }

    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP: {
        const std::string currentModeId = g_modeIdBuffers[g_currentModeIdIndex.load(std::memory_order_acquire)];
        const std::string localGameState = g_gameStateBuffers[g_currentGameStateIndex.load(std::memory_order_acquire)];
        result = HandleHotkeys(hWnd, uMsg, wParam, lParam, currentModeId, localGameState);
        if (result.consumed) return result.result;
        break;
    }
    default:
        break;
    }

    result = HandleMouseCoordinateTranslationPhase(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleCustomKeyNoRebind(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleKeyRebinding(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleCustomCharNoRebind(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleCharRebinding(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    const LRESULT forwarded = CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam);
    if (IsFocusGainMessage(uMsg) && s_deferredFocusRegainWmSizePending.exchange(false, std::memory_order_relaxed)) {
        QueueDeferredFocusRegainWmSize(hWnd);
    }
    return forwarded;
}


