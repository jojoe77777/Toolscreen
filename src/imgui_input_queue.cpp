#include "imgui_input_queue.h"

#include "imgui.h"

#include <array>

#include <windowsx.h> // GET_X_LPARAM(), GET_Y_LPARAM(), GET_WHEEL_DELTA_WPARAM(), GET_XBUTTON_WPARAM()

// Forward declaration (defined in imgui_impl_win32.cpp, not exposed in the header)
ImGuiKey ImGui_ImplWin32_KeyEventToImGuiKey(WPARAM wParam, LPARAM lParam);

std::atomic<bool> g_imguiWantCaptureMouse{ false };
std::atomic<bool> g_imguiWantCaptureKeyboard{ false };
std::atomic<bool> g_imguiAnyItemActive{ false };

namespace {

static inline bool IsVkDown(int vk) { return (::GetKeyState(vk) & 0x8000) != 0; }

struct ModState {
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
    bool superKey = false;
};

static inline ModState GetMods() {
    ModState m;
    m.ctrl = IsVkDown(VK_CONTROL);
    m.shift = IsVkDown(VK_SHIFT);
    m.alt = IsVkDown(VK_MENU);
    m.superKey = IsVkDown(VK_LWIN) || IsVkDown(VK_RWIN);
    return m;
}

static inline void ApplyMods(ImGuiIO& io, const ModState& m) {
    io.AddKeyEvent(ImGuiMod_Ctrl, m.ctrl);
    io.AddKeyEvent(ImGuiMod_Shift, m.shift);
    io.AddKeyEvent(ImGuiMod_Alt, m.alt);
    io.AddKeyEvent(ImGuiMod_Super, m.superKey);
}

enum class EventType : uint8_t {
    MousePos,
    MouseButton,
    MouseWheel,
    Key,
    CharUTF16,
    Focus,
};

struct Event {
    EventType type{};
    ModState mods{};

    // Mouse
    int mouseX = 0;
    int mouseY = 0;
    int mouseButton = 0; // 0..4
    bool mouseDown = false;
    float wheelX = 0.0f;
    float wheelY = 0.0f;

    // Keyboard
    ImGuiKey key = ImGuiKey_None;
    bool keyDown = false;
    int nativeKeycode = 0;
    int nativeScancode = -1;

    // Text
    ImWchar16 ch = 0;

    // Focus
    bool focused = false;
};

// Single-producer (WndProc thread) / single-consumer (render thread) ring buffer.
static constexpr uint32_t kQueueSize = 2048; // Must be power-of-two? (not required with modulo but kept large)
static std::array<Event, kQueueSize> s_queue;
static std::atomic<uint32_t> s_write{ 0 };
static std::atomic<uint32_t> s_read{ 0 };

// Mouse capture bookkeeping on producer thread.
static int s_mouseButtonsDownMask = 0;

static bool TryPush(const Event& e) {
    uint32_t w = s_write.load(std::memory_order_relaxed);
    uint32_t r = s_read.load(std::memory_order_acquire);
    uint32_t next = (w + 1) % kQueueSize;
    if (next == r) {
        // Queue full: drop event to avoid blocking the window thread.
        return false;
    }

    s_queue[w] = e;
    s_write.store(next, std::memory_order_release);
    return true;
}

static bool TryPop(Event& out) {
    uint32_t r = s_read.load(std::memory_order_relaxed);
    uint32_t w = s_write.load(std::memory_order_acquire);
    if (r == w) return false;

    out = s_queue[r];
    uint32_t next = (r + 1) % kQueueSize;
    s_read.store(next, std::memory_order_release);
    return true;
}

static inline int MouseButtonFromMsg(UINT msg, WPARAM wParam) {
    switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
        return 0;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
        return 1;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
        return 2;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK: {
        WORD xbtn = GET_XBUTTON_WPARAM(wParam);
        return (xbtn == XBUTTON1) ? 3 : 4;
    }
    default:
        return -1;
    }
}

static inline void UpdateMouseCapture(HWND hWnd, int button, bool down) {
    const int bit = 1 << button;
    if (down) {
        s_mouseButtonsDownMask |= bit;
        ::SetCapture(hWnd);
    } else {
        s_mouseButtonsDownMask &= ~bit;
        if (s_mouseButtonsDownMask == 0 && ::GetCapture() == hWnd) {
            ::ReleaseCapture();
        }
    }
}

static inline bool TryEnqueueMousePosFromScreenLParam(HWND hWnd, LPARAM lParam) {
    POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    if (!::ScreenToClient(hWnd, &pt)) return false;

    Event e;
    e.type = EventType::MousePos;
    e.mods = GetMods();
    e.mouseX = pt.x;
    e.mouseY = pt.y;
    return TryPush(e);
}

} // namespace

bool ImGuiInputQueue_EnqueueWin32Message(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // We intentionally accept and queue messages even if there's currently no ImGui context;
    // consumer will safely drain and discard if needed.

    Event e;
    e.mods = GetMods();

    switch (msg) {
    case WM_MOUSEMOVE: {
        e.type = EventType::MousePos;
        e.mouseX = GET_X_LPARAM(lParam);
        e.mouseY = GET_Y_LPARAM(lParam);
        return TryPush(e);
    }

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
    case WM_XBUTTONDBLCLK: {
        const int btn = MouseButtonFromMsg(msg, wParam);
        if (btn < 0) return false;

        e.type = EventType::MouseButton;
        e.mouseButton = btn;
        e.mouseDown = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK || msg == WM_RBUTTONDOWN || msg == WM_RBUTTONDBLCLK ||
                       msg == WM_MBUTTONDOWN || msg == WM_MBUTTONDBLCLK || msg == WM_XBUTTONDOWN || msg == WM_XBUTTONDBLCLK);

        // Ensure position is up-to-date at click time.
        {
            Event pos;
            pos.type = EventType::MousePos;
            pos.mods = e.mods;
            pos.mouseX = GET_X_LPARAM(lParam);
            pos.mouseY = GET_Y_LPARAM(lParam);
            TryPush(pos);
        }

        // Maintain capture on the window thread (required to keep receiving move events while dragging outside).
        UpdateMouseCapture(hWnd, btn, e.mouseDown);

        return TryPush(e);
    }

    case WM_MOUSEWHEEL: {
        // WM_MOUSEWHEEL uses screen coords in lParam.
        TryEnqueueMousePosFromScreenLParam(hWnd, lParam);

        const float wheelY = (float)GET_WHEEL_DELTA_WPARAM(wParam) / (float)WHEEL_DELTA;
        e.type = EventType::MouseWheel;
        e.wheelX = 0.0f;
        e.wheelY = wheelY;
        return TryPush(e);
    }

    case WM_MOUSEHWHEEL: {
        // WM_MOUSEHWHEEL uses screen coords in lParam.
        TryEnqueueMousePosFromScreenLParam(hWnd, lParam);

        // Match imgui_impl_win32.cpp convention: flip horizontal wheel.
        const float wheelX = -(float)GET_WHEEL_DELTA_WPARAM(wParam) / (float)WHEEL_DELTA;
        e.type = EventType::MouseWheel;
        e.wheelX = wheelX;
        e.wheelY = 0.0f;
        return TryPush(e);
    }

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        e.type = EventType::Key;
        e.keyDown = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);

        // Fix generic VK_* to left/right variants like imgui_impl_win32 does.
        WPARAM vk = wParam;
        if (vk == VK_SHIFT) {
            vk = ::MapVirtualKeyW((UINT)((lParam >> 16) & 0xff), MAPVK_VSC_TO_VK_EX);
        } else if (vk == VK_CONTROL) {
            vk = (HIWORD(lParam) & KF_EXTENDED) ? VK_RCONTROL : VK_LCONTROL;
        } else if (vk == VK_MENU) {
            vk = (HIWORD(lParam) & KF_EXTENDED) ? VK_RMENU : VK_LMENU;
        }

        e.nativeKeycode = (int)vk;
        e.nativeScancode = (int)((lParam >> 16) & 0xff);

        e.key = ImGui_ImplWin32_KeyEventToImGuiKey(vk, lParam);
        if (e.key == ImGuiKey_None)
            return false;

        return TryPush(e);
    }

    case WM_CHAR: {
        e.type = EventType::CharUTF16;
        e.ch = (ImWchar16)wParam;
        return TryPush(e);
    }

    case WM_SETFOCUS:
    case WM_KILLFOCUS: {
        e.type = EventType::Focus;
        e.focused = (msg == WM_SETFOCUS);
        return TryPush(e);
    }

    default:
        break;
    }

    return false;
}

void ImGuiInputQueue_EnqueueFocus(bool focused) {
    Event e;
    e.type = EventType::Focus;
    e.focused = focused;
    e.mods = GetMods();
    TryPush(e);
}

void ImGuiInputQueue_Clear() {
    // Drain by moving read to write.
    const uint32_t w = s_write.load(std::memory_order_acquire);
    s_read.store(w, std::memory_order_release);
}

void ImGuiInputQueue_ResetMouseCapture(HWND hWnd) {
    s_mouseButtonsDownMask = 0;
    if (hWnd && ::GetCapture() == hWnd) {
        ::ReleaseCapture();
    }
}

void ImGuiInputQueue_DrainToImGui() {
    if (!ImGui::GetCurrentContext()) {
        // No context available: discard events to avoid unbounded buildup.
        ImGuiInputQueue_Clear();
        return;
    }

    ImGuiIO& io = ImGui::GetIO();

    Event e;
    while (TryPop(e)) {
        switch (e.type) {
        case EventType::MousePos:
            ApplyMods(io, e.mods);
            io.AddMousePosEvent((float)e.mouseX, (float)e.mouseY);
            break;
        case EventType::MouseButton:
            ApplyMods(io, e.mods);
            io.AddMouseButtonEvent(e.mouseButton, e.mouseDown);
            break;
        case EventType::MouseWheel:
            ApplyMods(io, e.mods);
            io.AddMouseWheelEvent(e.wheelX, e.wheelY);
            break;
        case EventType::Key:
            ApplyMods(io, e.mods);
            io.AddKeyEvent(e.key, e.keyDown);
            io.SetKeyEventNativeData(e.key, e.nativeKeycode, e.nativeScancode);
            break;
        case EventType::CharUTF16:
            io.AddInputCharacterUTF16(e.ch);
            break;
        case EventType::Focus:
            io.AddFocusEvent(e.focused);
            break;
        default:
            break;
        }
    }
}

void ImGuiInputQueue_PublishCaptureState() {
    if (!ImGui::GetCurrentContext()) {
        g_imguiWantCaptureMouse.store(false, std::memory_order_release);
        g_imguiWantCaptureKeyboard.store(false, std::memory_order_release);
        g_imguiAnyItemActive.store(false, std::memory_order_release);
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    g_imguiWantCaptureMouse.store(io.WantCaptureMouse, std::memory_order_release);
    g_imguiWantCaptureKeyboard.store(io.WantCaptureKeyboard, std::memory_order_release);
    g_imguiAnyItemActive.store(ImGui::IsAnyItemActive(), std::memory_order_release);
}
