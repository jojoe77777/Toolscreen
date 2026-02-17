#include "input_hook.h"

#include "fake_cursor.h"
#include "gui.h"
#include "logic_thread.h"
#include "profiler.h"
#include "render.h"
#include "utils.h"
#include "version.h"
#include "window_overlay.h"

#include "imgui_impl_win32.h"

#include "imgui_input_queue.h"

#include <chrono>
#include <map>
#include <set>
#include <windowsx.h>

// External globals from dllmain.cpp
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

extern std::string g_currentModeId;
extern std::mutex g_modeIdMutex;
extern std::string g_gameStateBuffers[2];
extern std::atomic<int> g_currentGameStateIndex;
extern std::string g_currentlyEditingMirror;

extern std::atomic<bool> g_imageDragMode;
extern std::atomic<bool> g_windowOverlayDragMode;
extern std::atomic<HCURSOR> g_specialCursorHandle;
// g_glInitialized is declared in render.h as bool (not atomic)
extern std::atomic<bool> g_gameWindowActive;

// Hotkey state
extern std::map<std::string, std::chrono::steady_clock::time_point> g_hotkeyTimestamps;
extern std::mutex g_hotkeyTimestampsMutex;
extern std::set<DWORD> g_hotkeyMainKeys;
extern std::mutex g_hotkeyMainKeysMutex;
extern std::set<std::string> g_triggerOnReleasePending;
extern std::set<std::string> g_triggerOnReleaseInvalidated;
extern std::mutex g_triggerOnReleaseMutex;

// Forward declaration for ImGui handler
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
static bool s_forcedShowCursor = false;

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

InputHandlerResult HandleMouseMoveViewportOffset(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM& lParam) {
    PROFILE_SCOPE("HandleMouseMoveViewportOffset");

    if (uMsg == WM_MOUSEMOVE && !IsCursorVisible() && !g_showGui.load()) {
        // g_wmMouseMoveCount.fetch_add(1);

        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);

        ModeViewportInfo viewport = GetCurrentModeViewport();
        if (viewport.valid) {
            int offsetX = viewport.stretchX + (viewport.stretchWidth - viewport.width) / 2;
            int offsetY = viewport.stretchY + (viewport.stretchHeight - viewport.height) / 2;
            x += offsetX;
            y += offsetY;
        }

        lParam = MAKELPARAM(x, y);
    }
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

InputHandlerResult HandleNonFullscreenCheck(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    PROFILE_SCOPE("HandleNonFullscreenCheck");

    if (!IsFullscreen()) { return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) }; }
    return { false, 0 };
}

void HandleCharLogging(UINT uMsg, WPARAM wParam, LPARAM lParam) {
    // Only log in debug mode - logging is expensive
    auto cfgSnap = GetConfigSnapshot();
    if (uMsg == WM_CHAR && cfgSnap && cfgSnap->debug.showHotkeyDebug) {
        Log("WM_CHAR: " + std::to_string(wParam) + " " + std::to_string(lParam));
    }
}

InputHandlerResult HandleWindowPosChanged(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    PROFILE_SCOPE("HandleWindowPosChanged");

    if (uMsg != WM_WINDOWPOSCHANGED) { return { false, 0 }; }

    WINDOWPOS* pos = reinterpret_cast<WINDOWPOS*>(lParam);
    int flags = pos->flags;
    if (flags == 20) { return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) }; }

    int currentX = pos->x;
    int currentY = pos->y;
    int currentWidth = pos->cx;
    int currentHeight = pos->cy;

    if (currentX == -32000 && currentY == -32000) {
        Log("[RESIZE] Ignoring WM_WINDOWPOSCHANGED with minimized coordinates");
        return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
    }

    Log("[RESIZE] External resize detected to " + std::to_string(currentWidth) + "x" + std::to_string(currentHeight) + " at (" +
        std::to_string(currentX) + "," + std::to_string(currentY) + "), flags=" + std::to_string(flags));

    // Keep the window snapped to the monitor it is currently on (multi-monitor safe).
    RECT targetRect{ 0, 0, GetCachedScreenWidth(), GetCachedScreenHeight() };
    HMONITOR mon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
    if (mon) {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfo(mon, &mi)) {
            targetRect = mi.rcMonitor;
        }
    }
    const int targetW = (targetRect.right - targetRect.left);
    const int targetH = (targetRect.bottom - targetRect.top);
    SetWindowPos(hWnd, HWND_NOTOPMOST, targetRect.left, targetRect.top, targetW, targetH, SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    ModeViewportInfo geo = GetCurrentModeViewport();
    PostMessage(hWnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(geo.width, geo.height));

    // force recalculation of game texture ID on next SwapBuffers
    g_cachedGameTextureId.store(UINT_MAX);

    return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
}

InputHandlerResult HandleAltF4(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    PROFILE_SCOPE("HandleAltF4");

    if (uMsg == WM_SYSKEYDOWN && wParam == VK_F4) { return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) }; }
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
    PROFILE_SCOPE("HandleSetCursor");

    if (uMsg != WM_SETCURSOR) { return { false, 0 }; }

    if (g_showGui.load() && s_forcedShowCursor && g_gameVersion >= GameVersion(1, 13, 0)) {
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
    PROFILE_SCOPE("HandleDestroy");

    if (uMsg != WM_DESTROY) { return { false, 0 }; }

    extern GameVersion g_gameVersion;
    if (g_gameVersion >= GameVersion(1, 13, 0)) { g_isShuttingDown = true; }
    return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
}

InputHandlerResult HandleImGuiInput(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    PROFILE_SCOPE("HandleImGuiInput");

    if (!g_showGui.load()) { return { false, 0 }; }

    // IMPORTANT: Never call ImGui from this thread.
    // Instead, enqueue the message for the render thread (which owns the ImGui context).
    ImGuiInputQueue_EnqueueWin32Message(hWnd, uMsg, wParam, lParam);
    return { false, 0 };
}

InputHandlerResult HandleGuiToggle(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    PROFILE_SCOPE("HandleGuiToggle");

    DWORD vkCode = 0;
    bool isEscape = false;
    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        vkCode = static_cast<DWORD>(wParam);
        isEscape = (wParam == VK_ESCAPE);

        // Normalize generic modifier VKs to left/right variants.
        if (vkCode == VK_SHIFT) {
            DWORD mapped = static_cast<DWORD>(::MapVirtualKeyW((UINT)((lParam >> 16) & 0xff), MAPVK_VSC_TO_VK_EX));
            if (mapped != 0) vkCode = mapped;
        } else if (vkCode == VK_CONTROL) {
            vkCode = (HIWORD(lParam) & KF_EXTENDED) ? VK_RCONTROL : VK_LCONTROL;
        } else if (vkCode == VK_MENU) {
            vkCode = (HIWORD(lParam) & KF_EXTENDED) ? VK_RMENU : VK_LMENU;
        }
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

    // Escape always toggles GUI (with additional guards below). Otherwise, require the configured GUI hotkey.
    if (!isEscape && !CheckHotkeyMatch(g_config.guiHotkey, vkCode)) { return { false, 0 }; }

    bool allow_toggle = true;
    if (isEscape && !g_showGui.load()) { allow_toggle = false; }

    if (!allow_toggle) { return { false, 0 }; }

    // Lock-free debouncing using atomic timestamp
    auto now = std::chrono::steady_clock::now();
    int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    int64_t lastMs = g_lastGuiToggleTimeMs.load(std::memory_order_relaxed);
    if (nowMs - lastMs < 200) {
        return { true, 1 }; // Debounced
    }
    g_lastGuiToggleTimeMs.store(nowMs, std::memory_order_relaxed);

    if (!g_glInitialized) {
        Log("GUI toggle ignored - OpenGL not initialized yet");
        return { true, 1 };
    }

    bool is_closing = g_showGui.load();

    if (isEscape && g_imguiAnyItemActive.load(std::memory_order_acquire)) { is_closing = false; }
    if (isEscape && IsHotkeyBindingActive()) { is_closing = false; }
    if (isEscape && IsRebindBindingActive()) { is_closing = false; }

    if (is_closing) {
        g_showGui = false;
        if (s_forcedShowCursor) {
            EnsureSystemCursorHidden();
            s_forcedShowCursor = false;
        }

        // Flush any queued ImGui input and release any mouse capture we may have taken.
        ImGuiInputQueue_Clear();
        ImGuiInputQueue_ResetMouseCapture(hWnd);

        if (!g_wasCursorVisible.load()) {
            RECT fullScreenRect;
            fullScreenRect.left = 0;
            fullScreenRect.top = 0;
            fullScreenRect.right = GetCachedScreenWidth();
            fullScreenRect.bottom = GetCachedScreenHeight();
            ClipCursor(&fullScreenRect);
            SetCursor(NULL);

            if (g_gameVersion < GameVersion(1, 13, 0)) {
                HCURSOR airCursor = g_specialCursorHandle.load();
                if (airCursor) SetCursor(airCursor);
            }
        }
        g_currentlyEditingMirror = "";

        g_imageDragMode.store(false);
        g_windowOverlayDragMode.store(false);

        // Clear image overlay drag state
        extern std::string s_hoveredImageName;
        extern std::string s_draggedImageName;
        extern bool s_isDragging;
        s_hoveredImageName = "";
        s_draggedImageName = "";
        s_isDragging = false;

        // Clear window overlay drag state
        extern std::string s_hoveredWindowOverlayName;
        extern std::string s_draggedWindowOverlayName;
        extern bool s_isWindowOverlayDragging;
        s_hoveredWindowOverlayName = "";
        s_draggedWindowOverlayName = "";
        s_isWindowOverlayDragging = false;
    } else if (!isEscape) {
        g_showGui = true;
        const bool wasCursorVisible = IsCursorVisible();
        g_wasCursorVisible = wasCursorVisible;
        g_guiNeedsRecenter = true;
        ClipCursor(NULL);
        if (!wasCursorVisible && g_gameVersion >= GameVersion(1, 13, 0)) {
            s_forcedShowCursor = true;
            EnsureSystemCursorVisible();
            static HCURSOR s_arrowCursor = LoadCursorW(NULL, IDC_ARROW);
            SetCursor(s_arrowCursor);
        }

        // Dismiss ONLY the fullscreen configure prompt (toast2) for THIS SESSION once the user opens the GUI.
        // toast1 (windowed fullscreenPrompt) should continue to show in windowed mode.
        g_configurePromptDismissedThisSession.store(true, std::memory_order_release);

        // Touch "has_opened" flag file to preserve existing first-open marker behavior
        if (!g_toolscreenPath.empty()) {
            std::wstring flagPath = g_toolscreenPath + L"\\has_opened";
            HANDLE hFile = CreateFileW(flagPath.c_str(), GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hFile != INVALID_HANDLE_VALUE) { CloseHandle(hFile); }
        }
    }
    return { true, 1 };
}

InputHandlerResult HandleWindowOverlayKeyboard(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    PROFILE_SCOPE("HandleWindowOverlayKeyboard");

    bool isOverlayInteractionActive = IsWindowOverlayFocused();

    if (!isOverlayInteractionActive) { return { false, 0 }; }

    // Only handle key down/up messages, NOT WM_CHAR
    // WM_CHAR is generated by TranslateMessage() from WM_KEYDOWN, so if we forward both
    // WM_KEYDOWN and WM_CHAR, the target window will receive double input
    // (once from our WM_KEYDOWN being translated, once from our forwarded WM_CHAR)
    if (uMsg != WM_KEYDOWN && uMsg != WM_KEYUP && uMsg != WM_SYSKEYDOWN && uMsg != WM_SYSKEYUP) { return { false, 0 }; }

    // Never query ImGui from this thread. Use state published by render thread.
    bool imguiWantsKeyboard = g_showGui.load() && g_imguiWantCaptureKeyboard.load(std::memory_order_acquire);

    if (!imguiWantsKeyboard) {
        if (ForwardKeyboardToWindowOverlay(uMsg, wParam, lParam)) { return { true, 1 }; }
    }
    return { false, 0 };
}

InputHandlerResult HandleWindowOverlayMouse(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    PROFILE_SCOPE("HandleWindowOverlayMouse");

    if (uMsg < WM_MOUSEFIRST || uMsg > WM_MOUSELAST) { return { false, 0 }; }

    int mouseX, mouseY;

    // WM_MOUSEWHEEL and WM_MOUSEHWHEEL use SCREEN coordinates in lParam, not client coordinates
    // Use GetCursorPos for more reliable position since lParam can be stale
    if (uMsg == WM_MOUSEWHEEL || uMsg == WM_MOUSEHWHEEL) {
        POINT cursorPos;
        GetCursorPos(&cursorPos);
        // Convert to game window client coordinates.
        // IMPORTANT: On multi-monitor systems, screen coordinates are VIRTUAL SCREEN coordinates,
        // so (0,0) is not necessarily the top-left of the monitor the game is on.
        // Client coords are always relative to the game window (0,0 at its top-left),
        // which is the coordinate space used by our overlay hit-testing and forwarding.
        if (ScreenToClient(hWnd, &cursorPos)) {
            mouseX = cursorPos.x;
            mouseY = cursorPos.y;
        } else {
            // Fallback: keep screen coords if conversion fails
            mouseX = cursorPos.x;
            mouseY = cursorPos.y;
        }
    } else {
        // Other mouse messages use client coordinates directly
        mouseX = GET_X_LPARAM(lParam);
        mouseY = GET_Y_LPARAM(lParam);
    }

    const int screenW = GetCachedScreenWidth();
    const int screenH = GetCachedScreenHeight();

    bool cursorVisible = IsCursorVisible();
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
    } else if ((g_showGui.load() || cursorVisible) && (uMsg == WM_LBUTTONDOWN || uMsg == WM_RBUTTONDOWN || uMsg == WM_MBUTTONDOWN)) {
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
    PROFILE_SCOPE("HandleGuiInputBlocking");

    if (!g_showGui.load()) { return { false, 0 }; }

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
        return { true, 1 };
    }
    return { false, 0 };
}

// Forward declarations for external functions
void RestoreWindowsMouseSpeed();
void ApplyWindowsMouseSpeed();
void RestoreKeyRepeatSettings();
void ApplyKeyRepeatSettings();

InputHandlerResult HandleActivate(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, const std::string& currentModeId) {
    PROFILE_SCOPE("HandleActivate");

    if (uMsg != WM_ACTIVATE) { return { false, 0 }; }

    if (wParam == WA_INACTIVE) {
        ImGuiInputQueue_EnqueueFocus(false);

        // Log only in debug mode to avoid I/O on every focus change
        if (auto cs = GetConfigSnapshot(); cs && cs->debug.showHotkeyDebug) Log("[WINDOW] Window became inactive.");
        extern std::atomic<bool> g_isGameFocused;
        g_isGameFocused.store(false);
        g_gameWindowActive.store(false);

        RestoreWindowsMouseSpeed();
        RestoreKeyRepeatSettings();
    } else {
        ImGuiInputQueue_EnqueueFocus(true);

        // Log only in debug mode
        if (auto cs = GetConfigSnapshot(); cs && cs->debug.showHotkeyDebug) Log("[WINDOW] Window became active.");
        extern std::atomic<bool> g_isGameFocused;
        g_isGameFocused.store(true);
        g_gameWindowActive.store(true);

        ApplyWindowsMouseSpeed();
        ApplyKeyRepeatSettings();

        int modeWidth = 0, modeHeight = 0;
        {
            auto inputSnap = GetConfigSnapshot();
            const ModeConfig* mode = inputSnap ? GetModeFromSnapshot(*inputSnap, currentModeId) : nullptr;
            if (mode) {
                modeWidth = mode->width;
                modeHeight = mode->height;
            } else {
                Log("[WINDOW] WARNING: Current mode '" + currentModeId + "' not found in configuration!");
                return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
            }
        }
        PostMessage(hWnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(modeWidth, modeHeight));
    }
    return { false, 0 };
}

InputHandlerResult HandleHotkeys(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, const std::string& currentModeId,
                                 const std::string& gameState) {
    PROFILE_SCOPE("HandleHotkeys");

    // Determine the virtual key code based on message type
    DWORD rawVkCode = 0;
    DWORD vkCode = 0; // Normalized (left/right variants for Ctrl/Shift/Alt)
    bool isKeyDown = false;

    if (uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN) {
        rawVkCode = static_cast<DWORD>(wParam);
        isKeyDown = true;
    } else if (uMsg == WM_KEYUP || uMsg == WM_SYSKEYUP) {
        rawVkCode = static_cast<DWORD>(wParam);
        isKeyDown = false;
    } else if (uMsg == WM_XBUTTONDOWN) {
        // Side mouse buttons (Mouse 4/5)
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

    // Normalize generic modifier VKs to left/right variants (needed for RSHIFT/RCTRL/RALT, etc.).
    // This mirrors imgui_impl_win32 behavior and enables reliable hotkeys + key rebinding.
    vkCode = rawVkCode;
    if (uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN || uMsg == WM_KEYUP || uMsg == WM_SYSKEYUP) {
        if (vkCode == VK_SHIFT) {
            vkCode = static_cast<DWORD>(::MapVirtualKeyW((UINT)((lParam >> 16) & 0xff), MAPVK_VSC_TO_VK_EX));
        } else if (vkCode == VK_CONTROL) {
            vkCode = (HIWORD(lParam) & KF_EXTENDED) ? VK_RCONTROL : VK_LCONTROL;
        } else if (vkCode == VK_MENU) {
            vkCode = (HIWORD(lParam) & KF_EXTENDED) ? VK_RMENU : VK_LMENU;
        }
        if (vkCode == 0) vkCode = rawVkCode;
    }

    // Even if resolution-change features are unsupported, we must not short-circuit the input pipeline.
    // Key rebinding, mouse coordinate translation, overlays, etc. may still rely on downstream handlers.
    if (!IsResolutionChangeSupported(g_gameVersion)) { return { false, 0 }; }

    // Lock-free check of hotkey main keys - acceptable to race (worst case: miss one keypress)
    // Check both raw and normalized VK so Shift/Ctrl/Alt variants and generic VKs are handled.
    if (g_hotkeyMainKeys.find(rawVkCode) == g_hotkeyMainKeys.end() && g_hotkeyMainKeys.find(vkCode) == g_hotkeyMainKeys.end()) {
        // This key is not a hotkey main key, but it might invalidate pending trigger-on-release hotkeys
        if (isKeyDown) {
            std::lock_guard<std::mutex> lock(g_triggerOnReleaseMutex);
            // Any key press (that's not a hotkey) invalidates ALL pending trigger-on-release hotkeys
            for (const auto& pendingHotkeyId : g_triggerOnReleasePending) { g_triggerOnReleaseInvalidated.insert(pendingHotkeyId); }
        }
        // IMPORTANT: Do not return "consumed" here.
        // We intentionally skip scanning hotkeys for non-main keys, but we still want later phases
        // (mouse coordinate translation, key rebinding, etc.) to run and the message to be forwarded once.
        return { false, 0 };
    }

    // Use config snapshot for thread-safe hotkey iteration
    auto cfgSnap = GetConfigSnapshot();
    if (!cfgSnap) { return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) }; }
    const Config& cfg = *cfgSnap;

    bool s_enableHotkeyDebug = cfg.debug.showHotkeyDebug;

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

        // Game-state conditions normally gate ALL transitions.
        // Optional behavior: allow exiting the current secondary mode back to Fullscreen even if game state doesn't match.
        bool conditionsMet = hotkey.conditions.gameState.empty() ||
                             std::find(hotkey.conditions.gameState.begin(), hotkey.conditions.gameState.end(), gameState) !=
                                 hotkey.conditions.gameState.end();

        // Determine if this hotkey is currently in its active secondary mode (meaning main hotkey would exit to Fullscreen).
        // Note: GetHotkeySecondaryMode is thread-safe.
        std::string currentSecMode = GetHotkeySecondaryMode(hotkeyIdx);
        bool wouldExitToFullscreen = !currentSecMode.empty() && EqualsIgnoreCase(currentModeId, currentSecMode);

        if (!conditionsMet) {
            if (!(hotkey.allowExitToFullscreenRegardlessOfGameState && wouldExitToFullscreen)) {
                if (s_enableHotkeyDebug) { Log("[Hotkey] SKIP: Game state conditions not met"); }
                continue;
            }
            if (s_enableHotkeyDebug) {
                Log("[Hotkey] BYPASS: Allowing exit to Fullscreen even though game state conditions are not met");
            }
        }

        // Check Alt secondary mode hotkeys first
        for (const auto& alt : hotkey.altSecondaryModes) {
            if (CheckHotkeyMatch(alt.keys, vkCode, hotkey.conditions.exclusions, hotkey.triggerOnRelease)) {
                std::string hotkeyId = GetKeyComboString(alt.keys);

                // Handle trigger-on-release invalidation tracking
                if (hotkey.triggerOnRelease) {
                    if (isKeyDown) {
                        // Key pressed - add to pending set and invalidate OTHER pending hotkeys
                        std::lock_guard<std::mutex> lock(g_triggerOnReleaseMutex);
                        for (const auto& pendingHotkeyId : g_triggerOnReleasePending) {
                            if (pendingHotkeyId != hotkeyId) { g_triggerOnReleaseInvalidated.insert(pendingHotkeyId); }
                        }
                        g_triggerOnReleasePending.insert(hotkeyId);
                        if (s_enableHotkeyDebug) { Log("[Hotkey] Alt trigger-on-release hotkey pressed, added to pending: " + hotkeyId); }
                        // Pass through the key-down event to the game so modifier keys work with other combos
                        if (hotkey.blockKeyFromGame) return { true, 0 };
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
                            if (hotkey.blockKeyFromGame) return { true, 0 };
                            return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
                        }
                    }
                }

                // Check if this hotkey should trigger based on triggerOnRelease setting
                // When triggerOnRelease is true, only fire on key UP; when false (default), only fire on key DOWN
                if (hotkey.triggerOnRelease != isKeyDown) {
                    auto now = std::chrono::steady_clock::now();
                    // Lock-free debouncing - race is acceptable (worst case: occasional double-trigger)
                    if (g_hotkeyTimestamps.count(hotkeyId) &&
                        std::chrono::duration_cast<std::chrono::milliseconds>(now - g_hotkeyTimestamps[hotkeyId]).count() <
                            hotkey.debounce) {
                        if (s_enableHotkeyDebug) { Log("[Hotkey] Alt hotkey matched but debounced: " + hotkeyId); }
                        if (hotkey.blockKeyFromGame) return { true, 0 };
                        return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
                    }
                    g_hotkeyTimestamps[hotkeyId] = now;

                    std::string currentSecMode = GetHotkeySecondaryMode(hotkeyIdx);
                    std::string newSecMode = (currentSecMode == alt.mode) ? hotkey.secondaryMode : alt.mode;
                    SetHotkeySecondaryMode(hotkeyIdx, newSecMode);

                    if (s_enableHotkeyDebug) { Log("[Hotkey] ✓✓✓ ALT HOTKEY TRIGGERED: " + hotkeyId + " -> " + newSecMode); }

                    if (!newSecMode.empty()) { SwitchToMode(newSecMode, "alt hotkey"); }
                }
                if (hotkey.blockKeyFromGame) return { true, 0 };
                return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
            }
        }

        // Check Main hotkey
        if (CheckHotkeyMatch(hotkey.keys, vkCode, hotkey.conditions.exclusions, hotkey.triggerOnRelease)) {
            std::string hotkeyId = GetKeyComboString(hotkey.keys);

            // Handle trigger-on-release invalidation tracking
            if (hotkey.triggerOnRelease) {
                if (isKeyDown) {
                    // Key pressed - add to pending set and invalidate OTHER pending hotkeys
                    std::lock_guard<std::mutex> lock(g_triggerOnReleaseMutex);
                    // Invalidate all other pending trigger-on-release hotkeys
                    for (const auto& pendingHotkeyId : g_triggerOnReleasePending) {
                        if (pendingHotkeyId != hotkeyId) { g_triggerOnReleaseInvalidated.insert(pendingHotkeyId); }
                    }
                    // Add this hotkey to pending
                    g_triggerOnReleasePending.insert(hotkeyId);
                    if (s_enableHotkeyDebug) { Log("[Hotkey] Trigger-on-release hotkey pressed, added to pending: " + hotkeyId); }
                    // Pass through the key-down event to the game so modifier keys work with other combos
                    if (hotkey.blockKeyFromGame) return { true, 0 };
                    return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
                } else {
                    // Key released - check if invalidated
                    bool wasInvalidated = false;
                    {
                        std::lock_guard<std::mutex> lock(g_triggerOnReleaseMutex);
                        wasInvalidated = g_triggerOnReleaseInvalidated.count(hotkeyId) > 0;
                        // Clean up tracking sets
                        g_triggerOnReleasePending.erase(hotkeyId);
                        g_triggerOnReleaseInvalidated.erase(hotkeyId);
                    }

                    if (wasInvalidated) {
                        if (s_enableHotkeyDebug) {
                            Log("[Hotkey] Trigger-on-release hotkey invalidated (another key was pressed): " + hotkeyId);
                        }
                        if (hotkey.blockKeyFromGame) return { true, 0 };
                        return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
                    }
                    // Fall through to trigger the hotkey
                }
            }

            // Check if this hotkey should trigger based on triggerOnRelease setting
            // When triggerOnRelease is true, only fire on key UP; when false (default), only fire on key DOWN
            if (hotkey.triggerOnRelease != isKeyDown) {
                auto now = std::chrono::steady_clock::now();
                // Lock-free debouncing - race is acceptable (worst case: occasional double-trigger)
                if (g_hotkeyTimestamps.count(hotkeyId) &&
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - g_hotkeyTimestamps[hotkeyId]).count() < hotkey.debounce) {
                    if (s_enableHotkeyDebug) { Log("[Hotkey] Main hotkey matched but debounced: " + hotkeyId); }
                    if (hotkey.blockKeyFromGame) return { true, 0 };
                    return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
                }
                g_hotkeyTimestamps[hotkeyId] = now;

                // Lock-free read of current mode ID from double-buffer
                std::string current = g_modeIdBuffers[g_currentModeIdIndex.load(std::memory_order_acquire)];
                std::string targetMode;

                if (EqualsIgnoreCase(current, currentSecMode)) {
                    targetMode = "Fullscreen";
                } else {
                    targetMode = currentSecMode;
                }

                if (s_enableHotkeyDebug) {
                    Log("[Hotkey] ✓✓✓ MAIN HOTKEY TRIGGERED: " + hotkeyId + " (current: " + current + " -> target: " + targetMode + ")");
                }

                if (!targetMode.empty()) { SwitchToMode(targetMode, "main hotkey"); }
            }
            if (hotkey.blockKeyFromGame) return { true, 0 };
            return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
        }
    }

    // Check sensitivity hotkeys (temporary sensitivity override)
    for (size_t sensIdx = 0; sensIdx < cfg.sensitivityHotkeys.size(); ++sensIdx) {
        const auto& sensHotkey = cfg.sensitivityHotkeys[sensIdx];
        if (s_enableHotkeyDebug) {
            Log("[Hotkey] Checking sensitivity hotkey: " + GetKeyComboString(sensHotkey.keys) +
                " -> sens=" + std::to_string(sensHotkey.sensitivity));
        }

        // Check game state conditions
        bool conditionsMet = sensHotkey.conditions.gameState.empty() ||
                             std::find(sensHotkey.conditions.gameState.begin(), sensHotkey.conditions.gameState.end(), gameState) !=
                                 sensHotkey.conditions.gameState.end();
        if (!conditionsMet) {
            if (s_enableHotkeyDebug) { Log("[Hotkey] SKIP sensitivity: Game state conditions not met"); }
            continue;
        }

        // Sensitivity hotkeys only trigger on key down (no triggerOnRelease support)
        if (!isKeyDown) { continue; }

        if (CheckHotkeyMatch(sensHotkey.keys, vkCode, sensHotkey.conditions.exclusions, false)) {
            std::string hotkeyId = "sens_" + GetKeyComboString(sensHotkey.keys);

            auto now = std::chrono::steady_clock::now();
            // Debouncing
            if (g_hotkeyTimestamps.count(hotkeyId) &&
                std::chrono::duration_cast<std::chrono::milliseconds>(now - g_hotkeyTimestamps[hotkeyId]).count() < sensHotkey.debounce) {
                if (s_enableHotkeyDebug) { Log("[Hotkey] Sensitivity hotkey matched but debounced: " + hotkeyId); }
                return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
            }
            g_hotkeyTimestamps[hotkeyId] = now;

            // Toggle logic: if this hotkey has toggle enabled and it's the currently active override, clear it
            if (sensHotkey.toggle) {
                extern TempSensitivityOverride g_tempSensitivityOverride;
                extern std::mutex g_tempSensitivityMutex;
                std::lock_guard<std::mutex> lock(g_tempSensitivityMutex);

                if (g_tempSensitivityOverride.active && g_tempSensitivityOverride.activeSensHotkeyIndex == static_cast<int>(sensIdx)) {
                    // Toggle OFF - clear the override
                    g_tempSensitivityOverride.active = false;
                    g_tempSensitivityOverride.sensitivityX = 1.0f;
                    g_tempSensitivityOverride.sensitivityY = 1.0f;
                    g_tempSensitivityOverride.activeSensHotkeyIndex = -1;

                    if (s_enableHotkeyDebug) { Log("[Hotkey] ✓✓✓ SENSITIVITY HOTKEY TOGGLED OFF: " + hotkeyId); }

                    return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
                }

                // Toggle ON - apply the override
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
                // Non-toggle: apply the override (one-shot, no toggle tracking)
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
                    g_tempSensitivityOverride.activeSensHotkeyIndex = -1; // Non-toggle, no index tracking
                }

                if (s_enableHotkeyDebug) {
                    Log("[Hotkey] ✓✓✓ SENSITIVITY HOTKEY TRIGGERED: " + hotkeyId + " -> sens=" + std::to_string(sensHotkey.sensitivity));
                }
            }

            return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
        }
    }

    return { false, 0 };
}

InputHandlerResult HandleMouseCoordinateTranslation(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    PROFILE_SCOPE("HandleMouseCoordinateTranslation");

    if (uMsg < WM_MOUSEFIRST || uMsg > WM_MOUSELAST) { return { false, 0 }; }

    ModeViewportInfo geo = GetCurrentModeViewport();

    int mouseX = GET_X_LPARAM(lParam);
    int mouseY = GET_Y_LPARAM(lParam);

    float relativeX = static_cast<float>(mouseX - geo.stretchX);
    float relativeY = static_cast<float>(mouseY - geo.stretchY);
    int newX = static_cast<int>((relativeX / geo.stretchWidth) * geo.width);
    int newY = static_cast<int>((relativeY / geo.stretchHeight) * geo.height);

    return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, MAKELPARAM(newX, newY)) };
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

    // MAPVK_VK_TO_VSC_EX encodes extended keys in the high byte (0xE0/0xE1)
    UINT scanCodeWithFlags = MapVirtualKey(static_cast<UINT>(vkCode), MAPVK_VK_TO_VSC_EX);
    if (scanCodeWithFlags == 0) {
        // Fallback for legacy/unsupported mappings
        scanCodeWithFlags = MapVirtualKey(static_cast<UINT>(vkCode), MAPVK_VK_TO_VSC);
    }

    // Some systems/layouts return low-byte scan only for extended keys (e.g. arrows).
    // Restore E0 flag from VK semantics so LEFT/RIGHT are not treated as NUMPAD 4/6.
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

    // Ensure key-up has expected state even if caller passes inconsistent bits.
    if (!isKeyDown) out |= (1LL << 30) | (1LL << 31);

    return out;
}

static UINT ResolveOutputScanCode(DWORD outputVk, UINT configuredScanCodeWithFlags) {
    // If no configured scan code, derive from VK.
    if (configuredScanCodeWithFlags == 0) { return GetScanCodeWithExtendedFlag(outputVk); }

    // Preserve legacy stored values, but restore extended flag from VK mapping when possible.
    // Older configs may have scan code low-byte only (no 0xE0/0xE1 high-byte information).
    if ((configuredScanCodeWithFlags & 0xFF00) == 0) {
        UINT vkScan = GetScanCodeWithExtendedFlag(outputVk);
        if ((vkScan & 0xFF00) != 0 && ((vkScan & 0xFF) == (configuredScanCodeWithFlags & 0xFF))) { return vkScan; }
    }

    return configuredScanCodeWithFlags;
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
        // Dead key state cleanup
        BYTE emptyState[256] = {};
        WCHAR clearBuffer[8] = {};
        ToUnicodeEx(static_cast<UINT>(vkCode), scanCode, emptyState, clearBuffer, 8, 0, keyboardLayout);
    }

    return false;
}

InputHandlerResult HandleKeyRebinding(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    PROFILE_SCOPE("HandleKeyRebinding");

    // Determine the virtual key code based on message type
    DWORD rawVkCode = 0;
    DWORD vkCode = 0; // Normalized (left/right variants for Ctrl/Shift/Alt)
    bool isMouseButton = false;
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

    // Normalize generic modifier VKs to left/right variants so rebinds like VK_RSHIFT work.
    vkCode = rawVkCode;
    if (!isMouseButton && (uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN || uMsg == WM_KEYUP || uMsg == WM_SYSKEYUP)) {
        if (vkCode == VK_SHIFT) {
            vkCode = static_cast<DWORD>(::MapVirtualKeyW((UINT)((lParam >> 16) & 0xff), MAPVK_VSC_TO_VK_EX));
        } else if (vkCode == VK_CONTROL) {
            vkCode = (HIWORD(lParam) & KF_EXTENDED) ? VK_RCONTROL : VK_LCONTROL;
        } else if (vkCode == VK_MENU) {
            vkCode = (HIWORD(lParam) & KF_EXTENDED) ? VK_RMENU : VK_LMENU;
        }
        if (vkCode == 0) vkCode = rawVkCode;
    }

    // Use config snapshot for thread-safe access to key rebinds
    auto rebindCfg = GetConfigSnapshot();
    if (!rebindCfg || !rebindCfg->keyRebinds.enabled) { return { false, 0 }; }

    auto matchesFromKey = [&](DWORD incomingVk, DWORD incomingRawVk, DWORD fromKey) -> bool {
        if (fromKey == 0) return false;
        if (incomingVk == fromKey) return true;

        // Allow generic modifiers to match either left/right variant.
        if (fromKey == VK_CONTROL) {
            return incomingVk == VK_LCONTROL || incomingVk == VK_RCONTROL || incomingRawVk == VK_CONTROL;
        }
        if (fromKey == VK_SHIFT) {
            return incomingVk == VK_LSHIFT || incomingVk == VK_RSHIFT || incomingRawVk == VK_SHIFT;
        }
        if (fromKey == VK_MENU) {
            return incomingVk == VK_LMENU || incomingVk == VK_RMENU || incomingRawVk == VK_MENU;
        }

        // Also allow left/right modifier bindings to match generic incoming VK_*.
        if (incomingRawVk == VK_CONTROL && (fromKey == VK_LCONTROL || fromKey == VK_RCONTROL)) return true;
        if (incomingRawVk == VK_SHIFT && (fromKey == VK_LSHIFT || fromKey == VK_RSHIFT)) return true;
        if (incomingRawVk == VK_MENU && (fromKey == VK_LMENU || fromKey == VK_RMENU)) return true;

        return false;
    };

    for (size_t i = 0; i < rebindCfg->keyRebinds.rebinds.size(); ++i) {
        const auto& rebind = rebindCfg->keyRebinds.rebinds[i];

        if (rebind.enabled && rebind.fromKey != 0 && rebind.toKey != 0 && matchesFromKey(vkCode, rawVkCode, rebind.fromKey)) {
            DWORD outputVK;
            UINT outputScanCode;

            if (rebind.useCustomOutput) {
                outputVK = (rebind.customOutputVK != 0) ? rebind.customOutputVK : rebind.toKey;
                outputScanCode = ResolveOutputScanCode(outputVK, rebind.customOutputScanCode);
            } else {
                outputVK = rebind.toKey;
                outputScanCode = GetScanCodeWithExtendedFlag(rebind.toKey);
            }

            // For mouse button output, synthesize the appropriate mouse message
            if (outputVK == VK_LBUTTON || outputVK == VK_RBUTTON || outputVK == VK_MBUTTON || outputVK == VK_XBUTTON1 ||
                outputVK == VK_XBUTTON2) {
                UINT newMsg = 0;
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

                LPARAM mouseLParam = lParam;
                if (!isMouseButton) {
                    POINT pt{};
                    if (GetCursorPos(&pt) && ScreenToClient(hWnd, &pt)) {
                        mouseLParam = MAKELPARAM(pt.x, pt.y);
                    } else {
                        mouseLParam = MAKELPARAM(0, 0);
                    }
                }

                WORD mkState = buildMouseKeyState(outputVK, isKeyDown);
                WPARAM newWParam = mkState;

                if (outputVK == VK_LBUTTON) {
                    newMsg = isKeyDown ? WM_LBUTTONDOWN : WM_LBUTTONUP;
                } else if (outputVK == VK_RBUTTON) {
                    newMsg = isKeyDown ? WM_RBUTTONDOWN : WM_RBUTTONUP;
                } else if (outputVK == VK_MBUTTON) {
                    newMsg = isKeyDown ? WM_MBUTTONDOWN : WM_MBUTTONUP;
                } else if (outputVK == VK_XBUTTON1) {
                    newMsg = isKeyDown ? WM_XBUTTONDOWN : WM_XBUTTONUP;
                    newWParam = MAKEWPARAM(mkState, XBUTTON1);
                } else if (outputVK == VK_XBUTTON2) {
                    newMsg = isKeyDown ? WM_XBUTTONDOWN : WM_XBUTTONUP;
                    newWParam = MAKEWPARAM(mkState, XBUTTON2);
                }

                return { true, CallWindowProc(g_originalWndProc, hWnd, newMsg, newWParam, mouseLParam) };
            }

            // For keyboard output from keyboard/mouse input
            const bool isSystemKeyMsg = (uMsg == WM_SYSKEYDOWN || uMsg == WM_SYSKEYUP);
            UINT outputMsg = isKeyDown ? (isSystemKeyMsg ? WM_SYSKEYDOWN : WM_KEYDOWN) : (isSystemKeyMsg ? WM_SYSKEYUP : WM_KEYUP);

            UINT repeatCount = 1;
            bool previousState = !isKeyDown;
            bool transitionState = !isKeyDown;
            if (!isMouseButton) {
                repeatCount = static_cast<UINT>(lParam & 0xFFFF);
                if (repeatCount == 0) repeatCount = 1;

                // Mirror source-message semantics (especially for key repeat/down transitions).
                previousState = ((lParam & (1LL << 30)) != 0);
                transitionState = ((lParam & (1LL << 31)) != 0);
            }

            LPARAM newLParam =
                BuildKeyboardMessageLParam(outputScanCode, isKeyDown, isSystemKeyMsg, repeatCount, previousState, transitionState);
            return { true, CallWindowProc(g_originalWndProc, hWnd, outputMsg, outputVK, newLParam) };
        }
    }
    return { false, 0 };
}

InputHandlerResult HandleCharRebinding(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    PROFILE_SCOPE("HandleCharRebinding");

    auto charRebindCfg = GetConfigSnapshot();
    if (uMsg != WM_CHAR || !charRebindCfg || !charRebindCfg->keyRebinds.enabled) { return { false, 0 }; }

    WCHAR inputChar = static_cast<WCHAR>(wParam);

    for (const auto& rebind : charRebindCfg->keyRebinds.rebinds) {
        if (!rebind.enabled || rebind.fromKey == 0 || rebind.toKey == 0) continue;

        WCHAR fromUnshifted = 0;
        WCHAR fromShifted = 0;
        bool hasFromUnshifted = TryTranslateVkToChar(rebind.fromKey, false, fromUnshifted);
        bool hasFromShifted = TryTranslateVkToChar(rebind.fromKey, true, fromShifted);

        bool matched = false;
        bool matchedShifted = false;

        if (hasFromUnshifted && inputChar == fromUnshifted) {
            matched = true;
            matchedShifted = false;
        } else if (hasFromShifted && inputChar == fromShifted) {
            matched = true;
            matchedShifted = true;
        }

        if (matched) {
            DWORD outputVK = rebind.useCustomOutput ? rebind.customOutputVK : rebind.toKey;

            WCHAR outputChar = 0;
            if (!TryTranslateVkToChar(outputVK, matchedShifted, outputChar) || outputChar == 0) {
                // Fallback: try unshifted mapping if shifted equivalent doesn't exist
                if (!TryTranslateVkToChar(outputVK, false, outputChar) || outputChar == 0) { continue; }
            }

            Log("[REBIND WM_CHAR] Remapping char code " + std::to_string(static_cast<unsigned int>(inputChar)) + " -> " +
                std::to_string(static_cast<unsigned int>(outputChar)));

            return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, outputChar, lParam) };
        }
    }
    return { false, 0 };
}

LRESULT CALLBACK SubclassedWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    PROFILE_SCOPE("SubclassedWndProc");
    if (g_showGui.load() && s_forcedShowCursor && g_gameVersion >= GameVersion(1, 13, 0)) {
        EnsureSystemCursorVisible();
        static HCURSOR s_arrowCursor = LoadCursorW(NULL, IDC_ARROW);
        SetCursor(s_arrowCursor);
    }
    if (!g_showGui.load() && s_forcedShowCursor) {
        EnsureSystemCursorHidden();
        s_forcedShowCursor = false;
    }

    RegisterBindingInputEvent(uMsg, wParam, lParam);

    // Keep cached monitor dimensions in sync when the window is moved/resized across monitors.
    // This must run even in windowed mode (HandleNonFullscreenCheck returns early).
    switch (uMsg) {
    case WM_MOVE:
    case WM_MOVING:
    case WM_SIZE:
    case WM_SIZING:
    case WM_WINDOWPOSCHANGED:
    case WM_DPICHANGED:
    case WM_DISPLAYCHANGE:
        InvalidateCachedScreenMetrics();
        break;
    default:
        break;
    }

    InputHandlerResult result;

    // --- Phase 1: Early Processing ---
    result = HandleMouseMoveViewportOffset(hWnd, uMsg, wParam, lParam);
    // Note: lParam may be modified by this handler

    result = HandleShutdownCheck(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleWindowValidation(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleNonFullscreenCheck(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    // --- Phase 2: Message Logging ---
    HandleCharLogging(uMsg, wParam, lParam);

    // --- Phase 3: Window Messages ---
    result = HandleWindowPosChanged(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleAltF4(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleConfigLoadFailure(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    // --- Phase 4: Get Current State (lock-free) ---
    // Read mode ID from double-buffer atomically
    std::string currentModeId = g_modeIdBuffers[g_currentModeIdIndex.load(std::memory_order_acquire)];

    std::string localGameState = g_gameStateBuffers[g_currentGameStateIndex.load(std::memory_order_acquire)];

    // --- Phase 5: Cursor Handling ---
    result = HandleSetCursor(hWnd, uMsg, wParam, lParam, localGameState);
    if (result.consumed) return result.result;

    result = HandleDestroy(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    if (g_isShuttingDown.load()) { return CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam); }

    // --- Phase 6: GUI and Input Handling ---
    result = HandleImGuiInput(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleGuiToggle(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    // --- Phase 7: Window Overlay Interaction ---
    result = HandleWindowOverlayKeyboard(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleWindowOverlayMouse(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleGuiInputBlocking(uMsg);
    if (result.consumed) return result.result;

    // --- Phase 8: Window State ---
    result = HandleActivate(hWnd, uMsg, wParam, lParam, currentModeId);
    if (result.consumed) return result.result;

    // --- Phase 9: Hotkeys ---
    result = HandleHotkeys(hWnd, uMsg, wParam, lParam, currentModeId, localGameState);
    if (result.consumed) return result.result;

    // --- Phase 10: Mouse Coordinate Translation ---
    result = HandleMouseCoordinateTranslation(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    // --- Phase 11: Key Rebinding ---
    result = HandleKeyRebinding(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleCharRebinding(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    // --- Default: Pass to original WndProc ---
    return CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam);
}
