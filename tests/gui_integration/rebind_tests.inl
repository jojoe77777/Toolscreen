struct CapturedWindowMessage {
    UINT message = 0;
    WPARAM wParam = 0;
    LPARAM lParam = 0;
};

constexpr wchar_t kRebindCapturePropName[] = L"ToolscreenRebindCapture";
constexpr wchar_t kSubclassedInputCapturePropName[] = L"ToolscreenSubclassedInputCapture";
constexpr LRESULT kCapturedWndProcResult = 0x5A17;

class ScopedKeyboardStateOverride {
  public:
    ScopedKeyboardStateOverride() {
        m_valid = (GetKeyboardState(m_originalState) == TRUE);
        if (!m_valid) {
            memset(m_originalState, 0, sizeof(m_originalState));
        }
        memcpy(m_state, m_originalState, sizeof(m_state));
        for (BYTE& keyState : m_state) {
            keyState &= static_cast<BYTE>(~0x80);
        }
    }

    ~ScopedKeyboardStateOverride() {
        if (m_valid) {
            (void)SetKeyboardState(m_originalState);
        }
    }

    void SetKeyDown(int vk, bool down) {
        SetHighBit(vk, down);
        switch (vk) {
        case VK_SHIFT:
            SetHighBit(VK_LSHIFT, down);
            SetHighBit(VK_RSHIFT, down);
            break;
        case VK_CONTROL:
            SetHighBit(VK_LCONTROL, down);
            SetHighBit(VK_RCONTROL, down);
            break;
        case VK_MENU:
            SetHighBit(VK_LMENU, down);
            SetHighBit(VK_RMENU, down);
            break;
        default:
            break;
        }
    }

    void SetToggle(int vk, bool enabled) {
        if (vk < 0 || vk >= 256) return;
        if (enabled) {
            m_state[vk] |= 0x01;
        } else {
            m_state[vk] &= static_cast<BYTE>(~0x01);
        }
    }

    void Apply() {
        Expect(SetKeyboardState(m_state) == TRUE, "Failed to apply keyboard state override for rebind integration test.");
    }

  private:
    void SetHighBit(int vk, bool down) {
        if (vk < 0 || vk >= 256) return;
        if (down) {
            m_state[vk] |= 0x80;
        } else {
            m_state[vk] &= static_cast<BYTE>(~0x80);
        }
    }

    BYTE m_originalState[256] = {};
    BYTE m_state[256] = {};
    bool m_valid = false;
};

class ScopedCursorVisibilityOverride {
    public:
        explicit ScopedCursorVisibilityOverride(bool visible) : m_originalVisible(IsCursorVisible()) {
                SetVisible(visible);
        }

        ~ScopedCursorVisibilityOverride() { SetVisible(m_originalVisible); }

    private:
        static void SetVisible(bool visible) {
                for (int attempt = 0; attempt < 32 && IsCursorVisible() != visible; ++attempt) {
                        ShowCursor(visible ? TRUE : FALSE);
                }

                Expect(IsCursorVisible() == visible,
                             std::string("Failed to set cursor visibility to ") + (visible ? "visible" : "hidden") + " for rebind integration test.");
        }

        bool m_originalVisible = false;
};

class ScopedRebindMessageCapture {
  public:
    explicit ScopedRebindMessageCapture(HWND hwnd) : m_hwnd(hwnd) {
        Expect(m_hwnd != nullptr, "ScopedRebindMessageCapture requires a valid HWND.");
        m_previousWindowProc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(m_hwnd, GWLP_WNDPROC));
        Expect(m_previousWindowProc != nullptr, "Failed to read the previous WNDPROC for rebind integration test capture.");
        const BOOL setPropOk = SetPropW(m_hwnd, kRebindCapturePropName, this);
        Expect(setPropOk == TRUE, "Failed to associate rebind message capture with the integration-test window.");
        const LONG_PTR setProcResult = SetWindowLongPtrW(m_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&CaptureRouterWndProc));
        Expect(setProcResult != 0, "Failed to install the rebind message capture window procedure.");
        m_previousOriginalWndProc = g_originalWndProc;
        g_originalWndProc = &CaptureOriginalWndProc;
    }

    ~ScopedRebindMessageCapture() {
        g_originalWndProc = m_previousOriginalWndProc;
        if (m_hwnd != nullptr && m_previousWindowProc != nullptr) {
            (void)SetWindowLongPtrW(m_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(m_previousWindowProc));
            (void)RemovePropW(m_hwnd, kRebindCapturePropName);
        }
    }

    void Clear() { messages.clear(); }

    std::vector<CapturedWindowMessage> messages;

  private:
    static ScopedRebindMessageCapture* FromWindow(HWND hwnd) {
        return reinterpret_cast<ScopedRebindMessageCapture*>(GetPropW(hwnd, kRebindCapturePropName));
    }

    static LRESULT CALLBACK CaptureOriginalWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        if (auto* capture = FromWindow(hwnd)) {
            capture->messages.push_back({ message, wParam, lParam });
        }
        return kCapturedWndProcResult;
    }

    static LRESULT CALLBACK CaptureRouterWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_TOOLSCREEN_KEYDOWN_NO_REBIND || message == WM_TOOLSCREEN_KEYUP_NO_REBIND) {
            InputHandlerResult result = HandleCustomKeyNoRebind(hwnd, message, wParam, lParam);
            if (result.consumed) { return result.result; }
        }

        if (message == WM_TOOLSCREEN_CHAR_NO_REBIND) {
            InputHandlerResult result = HandleCustomCharNoRebind(hwnd, message, wParam, lParam);
            if (result.consumed) { return result.result; }
        }

        if (auto* capture = FromWindow(hwnd)) {
            return CallWindowProc(capture->m_previousWindowProc, hwnd, message, wParam, lParam);
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    HWND m_hwnd = nullptr;
    WNDPROC m_previousWindowProc = nullptr;
    WNDPROC m_previousOriginalWndProc = nullptr;
};

class ScopedSubclassedInputCapture {
  public:
    explicit ScopedSubclassedInputCapture(HWND hwnd) : m_hwnd(hwnd) {
        Expect(m_hwnd != nullptr, "ScopedSubclassedInputCapture requires a valid HWND.");
        m_previousWindowProc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(m_hwnd, GWLP_WNDPROC));
        Expect(m_previousWindowProc != nullptr, "Failed to read the previous WNDPROC for subclassed input capture.");
        const BOOL setPropOk = SetPropW(m_hwnd, kSubclassedInputCapturePropName, this);
        Expect(setPropOk == TRUE, "Failed to associate subclassed input capture with the integration-test window.");
        const LONG_PTR setProcResult = SetWindowLongPtrW(m_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&SubclassedWndProc));
        Expect(setProcResult != 0, "Failed to install the subclassed input capture window procedure.");
        m_previousOriginalWndProc = g_originalWndProc;
        m_previousSubclassedHwnd = ::g_subclassedHwnd.load(std::memory_order_acquire);
        g_originalWndProc = &CaptureOriginalWndProc;
        ::g_subclassedHwnd.store(m_hwnd, std::memory_order_release);
    }

    ~ScopedSubclassedInputCapture() {
        ResetLocalKeyRepeatState(m_hwnd);
        ::g_subclassedHwnd.store(m_previousSubclassedHwnd, std::memory_order_release);
        g_originalWndProc = m_previousOriginalWndProc;
        if (m_hwnd != nullptr && m_previousWindowProc != nullptr) {
            (void)SetWindowLongPtrW(m_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(m_previousWindowProc));
            (void)RemovePropW(m_hwnd, kSubclassedInputCapturePropName);
        }
    }

    void Clear() { messages.clear(); }

    std::vector<CapturedWindowMessage> messages;

  private:
    static ScopedSubclassedInputCapture* FromWindow(HWND hwnd) {
        return reinterpret_cast<ScopedSubclassedInputCapture*>(GetPropW(hwnd, kSubclassedInputCapturePropName));
    }

    static LRESULT CALLBACK CaptureOriginalWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        if (auto* capture = FromWindow(hwnd)) {
            capture->messages.push_back({ message, wParam, lParam });
        }
        return kCapturedWndProcResult;
    }

    HWND m_hwnd = nullptr;
    HWND m_previousSubclassedHwnd = nullptr;
    WNDPROC m_previousWindowProc = nullptr;
    WNDPROC m_previousOriginalWndProc = nullptr;
};

static LPARAM BuildTestKeyboardMessageLParamFromScan(UINT scanCodeWithFlags, bool isKeyDown, bool isSystemKey = false, UINT repeatCount = 1) {
    LPARAM out = static_cast<LPARAM>(repeatCount & 0xFFFFu);
    out |= static_cast<LPARAM>(scanCodeWithFlags & 0xFFu) << 16;
    if ((scanCodeWithFlags & 0xFF00u) != 0) {
        out |= static_cast<LPARAM>(1) << 24;
    }
    if (isSystemKey) {
        out |= static_cast<LPARAM>(1) << 29;
    }
    if (!isKeyDown) {
        out |= (static_cast<LPARAM>(1) << 30) | (static_cast<LPARAM>(1) << 31);
    }
    return out;
}

static LPARAM BuildTestKeyboardMessageLParam(DWORD vk, bool isKeyDown, bool isSystemKey = false, UINT repeatCount = 1) {
    const UINT scanCodeWithFlags = static_cast<UINT>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX));
    return BuildTestKeyboardMessageLParamFromScan(scanCodeWithFlags, isKeyDown, isSystemKey, repeatCount);
}

static KeyRebind MakeEnabledRebind(DWORD fromKey, DWORD toKey) {
    KeyRebind rebind;
    rebind.fromKey = fromKey;
    rebind.toKey = toKey;
    rebind.enabled = true;
    return rebind;
}

static void PrepareRebindRuntimeCase(std::string_view caseName, const std::vector<KeyRebind>& rebinds) {
    const std::filesystem::path root = PrepareCaseDirectory(caseName);
    ResetGlobalTestState(root);
    g_showGui.store(false, std::memory_order_release);
    g_config.keyRebinds = {};
    g_config.keyRebinds.enabled = true;
    g_config.keyRebinds.resolveRebindTargetsForHotkeys = false;
    g_config.keyRebinds.toggleHotkey.clear();
    g_config.keyRebinds.rebinds = rebinds;
    g_config.hotkeys.clear();
    g_config.sensitivityHotkeys.clear();
    PublishConfigSnapshot();
}

static void ExpectCapturedMessage(const ScopedRebindMessageCapture& capture, size_t index, UINT expectedMessage, WPARAM expectedWParam,
                                  const std::string& label) {
    Expect(index < capture.messages.size(), label + " missing captured message at index " + std::to_string(index) + ".");
    const CapturedWindowMessage& message = capture.messages[index];
    Expect(message.message == expectedMessage,
           label + " expected message " + std::to_string(expectedMessage) + ", got " + std::to_string(message.message) + ".");
    Expect(message.wParam == expectedWParam,
           label + " expected wParam " + std::to_string(static_cast<unsigned long long>(expectedWParam)) + ", got " +
               std::to_string(static_cast<unsigned long long>(message.wParam)) + ".");
}

static void ExpectCapturedMessage(const ScopedSubclassedInputCapture& capture, size_t index, UINT expectedMessage, WPARAM expectedWParam,
                                  const std::string& label) {
    Expect(index < capture.messages.size(), label + " missing captured message at index " + std::to_string(index) + ".");
    const CapturedWindowMessage& message = capture.messages[index];
    Expect(message.message == expectedMessage,
           label + " expected message " + std::to_string(expectedMessage) + ", got " + std::to_string(message.message) + ".");
    Expect(message.wParam == expectedWParam,
           label + " expected wParam " + std::to_string(static_cast<unsigned long long>(expectedWParam)) + ", got " +
               std::to_string(static_cast<unsigned long long>(message.wParam)) + ".");
}

static UINT ExtractScanCodeWithFlagsFromLParam(LPARAM lParam) {
    const auto raw = static_cast<unsigned long long>(lParam);
    UINT scanCode = static_cast<UINT>((raw >> 16) & 0xFFu);
    if ((raw & (1ull << 24)) != 0) {
        scanCode |= 0xE000u;
    }
    return scanCode;
}

static void ExpectCapturedScanCode(const ScopedRebindMessageCapture& capture, size_t index, UINT expectedScanCode, const std::string& label) {
    Expect(index < capture.messages.size(), label + " missing captured message at index " + std::to_string(index) + ".");
    const UINT actualScanCode = ExtractScanCodeWithFlagsFromLParam(capture.messages[index].lParam);
    Expect(actualScanCode == expectedScanCode,
           label + " expected scan code " + std::to_string(expectedScanCode) + ", got " + std::to_string(actualScanCode) + ".");
}

static void ExpectCapturedScanCode(const ScopedSubclassedInputCapture& capture, size_t index, UINT expectedScanCode,
                                   const std::string& label) {
    Expect(index < capture.messages.size(), label + " missing captured message at index " + std::to_string(index) + ".");
    const UINT actualScanCode = ExtractScanCodeWithFlagsFromLParam(capture.messages[index].lParam);
    Expect(actualScanCode == expectedScanCode,
           label + " expected scan code " + std::to_string(expectedScanCode) + ", got " + std::to_string(actualScanCode) + ".");
}

static void ExpectSyntheticRebindKeyEvent(size_t index, UINT expectedScanCodeWithFlags, bool expectedKeyDown, const std::string& label) {
    UINT actualScanCodeWithFlags = 0;
    bool actualKeyDown = false;
    Expect(GetSyntheticRebindKeyEventForTest(index, actualScanCodeWithFlags, actualKeyDown),
        label + " missing synthetic rebind key event at index " + std::to_string(index) + ".");
    Expect(actualScanCodeWithFlags == expectedScanCodeWithFlags,
        label + " expected scan code " + std::to_string(expectedScanCodeWithFlags) + ", got " +
         std::to_string(actualScanCodeWithFlags) + ".");
    Expect(actualKeyDown == expectedKeyDown,
        label + " expected keyDown=" + std::string(expectedKeyDown ? "true" : "false") + ", got " +
         (actualKeyDown ? "true" : "false") + ".");
}

static void PrepareRebindGuiCase(std::string_view caseName, const std::vector<KeyRebind>& rebinds = {}) {
    const std::filesystem::path root = PrepareCaseDirectory(caseName);
    ResetGlobalTestState(root);
    g_config.basicModeEnabled = false;
    g_config.keyRebinds = {};
    g_config.keyRebinds.enabled = true;
    g_config.keyRebinds.resolveRebindTargetsForHotkeys = false;
    g_config.keyRebinds.toggleHotkey.clear();
    g_config.keyRebinds.rebinds = rebinds;
    g_config.hotkeys.clear();
    g_config.sensitivityHotkeys.clear();
    PublishConfigSnapshot();
}

static void RenderKeyboardInputsFrame(DummyWindow& window) {
    RenderSettingsFrame(window, trc("tabs.inputs"), trc("inputs.keyboard"));
}

static bool SkipIfNoModernGuiTestGL(const DummyWindow& window) {
    if (window.hasModernGL()) {
        return false;
    }

    std::cout << "SKIP (no GL 3.3+)" << std::endl;
    return true;
}

static GuiTestInteractionRect ExpectGuiInteractionRect(const char* id, const std::string& label) {
    GuiTestInteractionRect rect;
    Expect(GetGuiTestInteractionRect(id, rect), label + " missing GUI interaction rect '" + id + "'.");
    Expect(std::isfinite(rect.minX) && std::isfinite(rect.minY) && std::isfinite(rect.maxX) && std::isfinite(rect.maxY),
           label + " produced a non-finite GUI interaction rect.");
    if (rect.maxX <= rect.minX) {
        rect.maxX = rect.minX + 8.0f;
    }
    if (rect.maxY <= rect.minY) {
        rect.maxY = rect.minY + 8.0f;
    }
    return rect;
}

static GuiTestKeyboardLayoutKeyLabels ExpectKeyboardLayoutKeyLabels(DWORD vk, const std::string& label) {
    GuiTestKeyboardLayoutKeyLabels labels;
    Expect(GetGuiTestKeyboardLayoutKeyLabels(vk, labels),
           label + " missing keyboard-layout labels for VK " + std::to_string(static_cast<unsigned>(vk)) + ".");
    return labels;
}

static POINT GetClientCenterPoint(HWND hwnd, const GuiTestInteractionRect& rect, const std::string& label) {
    POINT point{};
    point.x = static_cast<LONG>(std::lround((rect.minX + rect.maxX) * 0.5f));
    point.y = static_cast<LONG>(std::lround((rect.minY + rect.maxY) * 0.5f));
    Expect(ScreenToClient(hwnd, &point) == TRUE, label + " failed to convert a screen-space GUI rect to client coordinates.");
    return point;
}

static LPARAM MakeMouseClientLParam(const POINT& point) {
    return MAKELPARAM(static_cast<WORD>(point.x & 0xFFFF), static_cast<WORD>(point.y & 0xFFFF));
}

static void SendGuiMouseMessage(HWND hwnd, UINT message, WPARAM wParam, const POINT& point) {
    (void)SendMessageW(hwnd, message, wParam, MakeMouseClientLParam(point));
}

static void ClickGuiInteractionRect(DummyWindow& window, const char* id, bool rightButton, const std::string& label) {
    const GuiTestInteractionRect rect = ExpectGuiInteractionRect(id, label);
    const POINT point = GetClientCenterPoint(window.hwnd(), rect, label);
    const UINT downMessage = rightButton ? WM_RBUTTONDOWN : WM_LBUTTONDOWN;
    const UINT upMessage = rightButton ? WM_RBUTTONUP : WM_LBUTTONUP;
    const WPARAM downWParam = rightButton ? MK_RBUTTON : MK_LBUTTON;

    SendGuiMouseMessage(window.hwnd(), WM_MOUSEMOVE, 0, point);
    RenderKeyboardInputsFrame(window);

    SendGuiMouseMessage(window.hwnd(), downMessage, downWParam, point);
    RenderKeyboardInputsFrame(window);

    SendGuiMouseMessage(window.hwnd(), upMessage, 0, point);
    RenderKeyboardInputsFrame(window);
}

static void SubmitKeyboardBindingEvent(DWORD vk) {
    RegisterBindingInputEvent(WM_KEYDOWN, static_cast<WPARAM>(vk), BuildTestKeyboardMessageLParam(vk, true));
}

static void OpenKeyboardLayoutContext(DummyWindow& window, DWORD sourceVk) {
    RenderKeyboardInputsFrame(window);
    RenderKeyboardInputsFrame(window);
    RequestGuiTestOpenKeyboardLayout();
    RenderKeyboardInputsFrame(window);
    RequestGuiTestOpenKeyboardLayoutContext(sourceVk);
    RenderKeyboardInputsFrame(window);
}

static void OpenKeyboardLayout(DummyWindow& window) {
    RenderKeyboardInputsFrame(window);
    RenderKeyboardInputsFrame(window);
    RequestGuiTestOpenKeyboardLayout();
    RenderKeyboardInputsFrame(window);
}

static void RunKeyboardLayoutTriggerLabelMappingCase(std::string_view caseName, DWORD triggerVk, DWORD forcedScanCode,
                                                     const char* expectedTriggerLabel, TestRunMode runMode) {
    constexpr DWORD kSourceVk = 'A';

    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    if (SkipIfNoModernGuiTestGL(window)) { return; }

    KeyRebind rebind = MakeEnabledRebind(kSourceVk, triggerVk);
    rebind.useCustomOutput = true;
    rebind.customOutputScanCode = forcedScanCode;
    PrepareRebindGuiCase(caseName, { rebind });

    OpenKeyboardLayout(window);
    ResetGuiTestInteractionRects();
    RenderKeyboardInputsFrame(window);

    const GuiTestKeyboardLayoutKeyLabels labels = ExpectKeyboardLayoutKeyLabels(
        kSourceVk, std::string("Expected keyboard-layout trigger labels for case '") + std::string(caseName) + "'.");
    Expect(labels.primaryText == "CT",
           std::string("Expected keyboard-layout trigger mapping case '") + std::string(caseName) +
               "' to render the compact cannot-type indicator on the source key.");
    Expect(labels.secondaryText == expectedTriggerLabel,
           std::string("Expected keyboard-layout trigger mapping case '") + std::string(caseName) +
               "' to render '" + expectedTriggerLabel + "' instead of a scan-derived fallback label.");
    Expect(labels.shiftLayerText.empty(),
           std::string("Expected keyboard-layout trigger mapping case '") + std::string(caseName) +
               "' to avoid rendering a Shift-layer label.");
}

static void BindKeyboardLayoutTarget(DummyWindow& window, bool splitMode, DWORD targetVk) {
    if (splitMode) {
        RequestGuiTestKeyboardLayoutSetSplitMode(true);
        RenderKeyboardInputsFrame(window);
        RequestGuiTestKeyboardLayoutBeginBind(GuiTestKeyboardLayoutBindTarget::TriggersVk);
    } else {
        RequestGuiTestKeyboardLayoutBeginBind(GuiTestKeyboardLayoutBindTarget::FullOutputVk);
    }
    RenderKeyboardInputsFrame(window);
    SubmitKeyboardBindingEvent(targetVk);
    RenderKeyboardInputsFrame(window);
}

static void OpenKeyboardLayoutScanPicker(DummyWindow& window) {
    RequestGuiTestKeyboardLayoutOpenScanPicker();
    RenderKeyboardInputsFrame(window);
}

static void SetKeyboardLayoutScanFilter(DummyWindow& window, GuiTestKeyboardLayoutScanFilterGroup group) {
    RequestGuiTestKeyboardLayoutSetScanFilter(group);
    RenderKeyboardInputsFrame(window);
}

static void SelectKeyboardLayoutScan(DummyWindow& window, DWORD scan) {
    RequestGuiTestKeyboardLayoutSelectScan(scan);
    RenderKeyboardInputsFrame(window);
}

static void ResetKeyboardLayoutScanToDefault(DummyWindow& window) {
    RequestGuiTestKeyboardLayoutResetScanToDefault();
    RenderKeyboardInputsFrame(window);
}

void RunConfigLoadKeyRebindShiftLayerCapsLockParsedTest(TestRunMode runMode = TestRunMode::Automated) {
    RunConfigLoadCase("config_load_key_rebind_shift_layer_caps_lock_parsed",
                      []() {
                          WriteRawConfigTomlToDisk(R"(configVersion = 4
defaultMode = "Fullscreen"

[keyRebinds]
enabled = true
resolveRebindTargetsForHotkeys = false
toggleHotkey = []

[[keyRebinds.rebinds]]
fromKey = 74
toKey = 75
enabled = true
shiftLayerEnabled = true
shiftLayerUsesCapsLock = true
shiftLayerOutputVK = 80
)");
                      },
                      []() {
                          ExpectConfigLoadSucceeded("config-load-key-rebind-shift-layer-caps-lock-parsed");
                          Expect(g_config.keyRebinds.rebinds.size() == 1, "Expected exactly one key rebind fixture to load.");
                          const KeyRebind& rebind = g_config.keyRebinds.rebinds.front();
                          Expect(rebind.shiftLayerEnabled, "Expected shiftLayerEnabled fixture state to load.");
                          Expect(rebind.shiftLayerUsesCapsLock,
                                 "Expected shiftLayerUsesCapsLock to parse from the key rebind config fixture.");
                          Expect(rebind.shiftLayerOutputVK == 80, "Expected shiftLayerOutputVK fixture state to load.");
                      },
                      runMode);
}

void RunConfigLoadKeyRebindShiftLayerCapsLockDefaultedTest(TestRunMode runMode = TestRunMode::Automated) {
    RunConfigLoadCase("config_load_key_rebind_shift_layer_caps_lock_defaulted",
                      []() {
                          WriteRawConfigTomlToDisk(R"(configVersion = 4
defaultMode = "Fullscreen"

[keyRebinds]
enabled = true
resolveRebindTargetsForHotkeys = false
toggleHotkey = []

[[keyRebinds.rebinds]]
fromKey = 74
toKey = 75
enabled = true
shiftLayerEnabled = true
shiftLayerOutputVK = 80
)");
                      },
                      []() {
                          ExpectConfigLoadSucceeded("config-load-key-rebind-shift-layer-caps-lock-defaulted");
                          Expect(g_config.keyRebinds.rebinds.size() == 1, "Expected exactly one key rebind fixture to load.");
                          const KeyRebind& rebind = g_config.keyRebinds.rebinds.front();
                          Expect(!rebind.shiftLayerUsesCapsLock,
                                 "Expected shiftLayerUsesCapsLock to default to false when omitted from config.");
                      },
                      runMode);
}

void RunConfigLoadKeyRebindCursorStateDefaultedTest(TestRunMode runMode = TestRunMode::Automated) {
    RunConfigLoadCase("config_load_key_rebind_cursor_state_defaulted",
                      []() {
                          WriteRawConfigTomlToDisk(R"(configVersion = 4
defaultMode = "Fullscreen"

[keyRebinds]
enabled = true
resolveRebindTargetsForHotkeys = false
toggleHotkey = []

[[keyRebinds.rebinds]]
fromKey = 74
toKey = 75
enabled = true
)");
                      },
                      []() {
                          ExpectConfigLoadSucceeded("config-load-key-rebind-cursor-state-defaulted");
                          Expect(g_config.keyRebinds.rebinds.size() == 1, "Expected exactly one key rebind fixture to load.");
                          const KeyRebind& rebind = g_config.keyRebinds.rebinds.front();
                          Expect(rebind.cursorState == kKeyRebindCursorStateAny,
                                 "Expected key rebind cursorState to default to any when omitted from config.");
                      },
                      runMode);
}

void RunKeyRebindRuntimeFullForwardingTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    KeyRebind rebind = MakeEnabledRebind('A', 'B');
    rebind.useCustomOutput = true;
    rebind.customOutputVK = 'B';
    PrepareRebindRuntimeCase("key_rebind_runtime_full_forwarding", { rebind });
    ScopedRebindMessageCapture capture(window.hwnd());

    ScopedKeyboardStateOverride keyboardState;
    keyboardState.SetKeyDown(VK_SHIFT, false);
    keyboardState.SetToggle(VK_CAPITAL, false);
    keyboardState.Apply();

    const LPARAM keyDownLParam = BuildTestKeyboardMessageLParam('A', true);
    const InputHandlerResult keyDownResult = HandleKeyRebinding(window.hwnd(), WM_KEYDOWN, 'A', keyDownLParam);
    Expect(keyDownResult.consumed, "Expected full rebind WM_KEYDOWN to be consumed.");
    Expect(keyDownResult.result == kCapturedWndProcResult, "Expected full rebind WM_KEYDOWN to forward through the original WNDPROC.");
    Expect(capture.messages.size() == 1, "Expected full rebind WM_KEYDOWN to forward exactly one key message.");
    ExpectCapturedMessage(capture, 0, WM_KEYDOWN, 'B', "Full rebind WM_KEYDOWN");

    capture.Clear();
    const InputHandlerResult charResult = HandleCharRebinding(window.hwnd(), WM_CHAR, static_cast<WPARAM>('a'), keyDownLParam);
    Expect(charResult.consumed, "Expected full rebind WM_CHAR to be consumed.");
    Expect(charResult.result == kCapturedWndProcResult, "Expected full rebind WM_CHAR to forward through the original WNDPROC.");
    Expect(capture.messages.size() == 1, "Expected full rebind WM_CHAR to forward exactly one character message.");
    ExpectCapturedMessage(capture, 0, WM_CHAR, 'b', "Full rebind WM_CHAR");

    capture.Clear();
    const InputHandlerResult keyUpResult = HandleKeyRebinding(window.hwnd(), WM_KEYUP, 'A', BuildTestKeyboardMessageLParam('A', false));
    Expect(keyUpResult.consumed, "Expected full rebind WM_KEYUP to be consumed.");
    Expect(keyUpResult.result == kCapturedWndProcResult, "Expected full rebind WM_KEYUP to forward through the original WNDPROC.");
    Expect(capture.messages.size() == 1, "Expected full rebind WM_KEYUP to forward exactly one key message.");
    ExpectCapturedMessage(capture, 0, WM_KEYUP, 'B', "Full rebind WM_KEYUP");

    capture.Clear();
    const LPARAM systemKeyDownLParam = BuildTestKeyboardMessageLParam('A', true, true);
    const InputHandlerResult systemKeyDownResult = HandleKeyRebinding(window.hwnd(), WM_SYSKEYDOWN, 'A', systemKeyDownLParam);
    Expect(systemKeyDownResult.consumed, "Expected full rebind WM_SYSKEYDOWN to be consumed.");
    Expect(systemKeyDownResult.result == kCapturedWndProcResult,
           "Expected full rebind WM_SYSKEYDOWN to forward through the original WNDPROC.");
    Expect(capture.messages.size() == 1, "Expected full rebind WM_SYSKEYDOWN to forward exactly one key message.");
    ExpectCapturedMessage(capture, 0, WM_SYSKEYDOWN, 'B', "Full rebind WM_SYSKEYDOWN");

    capture.Clear();
    const InputHandlerResult systemCharResult = HandleCharRebinding(window.hwnd(), WM_SYSCHAR, static_cast<WPARAM>('a'), systemKeyDownLParam);
    Expect(systemCharResult.consumed, "Expected full rebind WM_SYSCHAR to be consumed.");
    Expect(systemCharResult.result == kCapturedWndProcResult,
           "Expected full rebind WM_SYSCHAR to forward through the original WNDPROC.");
    Expect(capture.messages.size() == 1, "Expected full rebind WM_SYSCHAR to forward exactly one character message.");
    ExpectCapturedMessage(capture, 0, WM_SYSCHAR, 'b', "Full rebind WM_SYSCHAR");

    capture.Clear();
    const InputHandlerResult systemKeyUpResult = HandleKeyRebinding(window.hwnd(), WM_SYSKEYUP, 'A', BuildTestKeyboardMessageLParam('A', false, true));
    Expect(systemKeyUpResult.consumed, "Expected full rebind WM_SYSKEYUP to be consumed.");
    Expect(systemKeyUpResult.result == kCapturedWndProcResult,
           "Expected full rebind WM_SYSKEYUP to forward through the original WNDPROC.");
    Expect(capture.messages.size() == 1, "Expected full rebind WM_SYSKEYUP to forward exactly one key message.");
    ExpectCapturedMessage(capture, 0, WM_SYSKEYUP, 'B', "Full rebind WM_SYSKEYUP");
}

void RunHotkeyRuntimeSpecificShiftReleaseMatchesExactKeyupTest(TestRunMode runMode = TestRunMode::Automated) {
    (void)runMode;

    const std::filesystem::path root = PrepareCaseDirectory("hotkey_runtime_specific_shift_release_matches_exact_keyup");
    ResetGlobalTestState(root);

    Expect(CheckHotkeyMatch({ VK_RSHIFT }, VK_RSHIFT, {}, true, 1, VK_SHIFT, true, false),
           "Expected a right-Shift hold/release hotkey to match an exact right-Shift keyup.");
    Expect(!CheckHotkeyMatch({ VK_RSHIFT }, VK_LSHIFT, {}, true, 1, VK_SHIFT, true, false),
           "Expected a right-Shift hold/release hotkey not to match a left-Shift keyup.");
}

void RunHotkeyRuntimeExclusionDetectsLowLevelSuppressedKeyTest(TestRunMode runMode = TestRunMode::Automated) {
    (void)runMode;

    const std::filesystem::path root = PrepareCaseDirectory("hotkey_runtime_exclusion_detects_low_level_suppressed_key");
    ResetGlobalTestState(root);

    QueueSuppressedLowLevelKeyForTest(VK_CAPITAL, 0x003A, false);

    Expect(!CheckHotkeyMatch({ 0x46 }, 0x46, { VK_CAPITAL }, false, 1, 0x46, true, true),
           "Expected hotkey to NOT match when Caps Lock is low-level suppressed and listed as exclusion.");

    Expect(CheckHotkeyMatch({ 0x46 }, 0x46, {}, false, 1, 0x46, true, true),
           "Expected hotkey to match when no exclusion keys are specified.");

    ClearLowLevelSuppressedKeysForTest();

    Expect(CheckHotkeyMatch({ 0x46 }, 0x46, { VK_CAPITAL }, false, 1, 0x46, true, true),
           "Expected hotkey to match when Caps Lock exclusion is listed but key is neither pressed nor suppressed.");
}

void RunKeyRebindDeepSuppressionEligibilityTest(TestRunMode runMode = TestRunMode::Automated) {
    (void)runMode;

    Expect(IsDeepSuppressionEligibleSourceVkForTest(VK_CONTROL), "Expected VK_CONTROL to be deep-suppression eligible.");
    Expect(IsDeepSuppressionEligibleSourceVkForTest(VK_LCONTROL), "Expected VK_LCONTROL to be deep-suppression eligible.");
    Expect(IsDeepSuppressionEligibleSourceVkForTest(VK_RCONTROL), "Expected VK_RCONTROL to be deep-suppression eligible.");
    Expect(IsDeepSuppressionEligibleSourceVkForTest(VK_SHIFT), "Expected VK_SHIFT to be deep-suppression eligible.");
    Expect(IsDeepSuppressionEligibleSourceVkForTest(VK_LSHIFT), "Expected VK_LSHIFT to be deep-suppression eligible.");
    Expect(IsDeepSuppressionEligibleSourceVkForTest(VK_RSHIFT), "Expected VK_RSHIFT to be deep-suppression eligible.");
    Expect(IsDeepSuppressionEligibleSourceVkForTest(VK_LMENU), "Expected VK_LMENU to remain deep-suppression eligible.");
    Expect(IsDeepSuppressionEligibleSourceVkForTest(VK_LWIN), "Expected VK_LWIN to remain deep-suppression eligible.");
    Expect(!IsDeepSuppressionEligibleSourceVkForTest('A'), "Expected a regular key to NOT be deep-suppression eligible.");
    Expect(!IsDeepSuppressionEligibleSourceVkForTest(VK_F3), "Expected F3 to NOT be deep-suppression eligible.");

    Expect(ShouldDeepSuppressRebindForTest(VK_CONTROL, VK_F3),
           "Expected Ctrl->F3 to deep-suppress the physical Ctrl so chord reads (narrator/GUI) do not see it.");
    Expect(ShouldDeepSuppressRebindForTest(VK_SHIFT, VK_F3),
           "Expected Shift->F3 to deep-suppress the physical Shift.");
    Expect(ShouldDeepSuppressRebindForTest(VK_CONTROL, 0),
           "Expected a consume-only Ctrl rebind to deep-suppress the physical Ctrl.");
    Expect(!ShouldDeepSuppressRebindForTest(VK_CONTROL, VK_CONTROL),
           "Expected Ctrl->Ctrl (preserving) to NOT deep-suppress.");
    Expect(!ShouldDeepSuppressRebindForTest(VK_SHIFT, VK_SHIFT),
           "Expected Shift->Shift (preserving) to NOT deep-suppress.");
}

void RunHotkeyRuntimeExclusionDetectsSuppressedCtrlShiftTest(TestRunMode runMode = TestRunMode::Automated) {
    (void)runMode;

    const std::filesystem::path root = PrepareCaseDirectory("hotkey_runtime_exclusion_detects_suppressed_ctrl_shift");
    ResetGlobalTestState(root);

    QueueSuppressedLowLevelKeyForTest(VK_LCONTROL, 0x001D, false);
    Expect(!CheckHotkeyMatch({ 0x46 }, 0x46, { VK_CONTROL }, false, 1, 0x46, true, true),
           "Expected hotkey to NOT match when low-level-suppressed Ctrl is listed as exclusion.");
    ClearLowLevelSuppressedKeysForTest();

    QueueSuppressedLowLevelKeyForTest(VK_LSHIFT, 0x002A, false);
    Expect(!CheckHotkeyMatch({ 0x46 }, 0x46, { VK_SHIFT }, false, 1, 0x46, true, true),
           "Expected hotkey to NOT match when low-level-suppressed Shift is listed as exclusion.");
    ClearLowLevelSuppressedKeysForTest();

    Expect(CheckHotkeyMatch({ 0x46 }, 0x46, { VK_CONTROL, VK_SHIFT }, false, 1, 0x46, true, true),
           "Expected hotkey to match when Ctrl/Shift exclusions are neither pressed nor suppressed.");
}

void RunKeyRebindRuntimeSplitVkOutputTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    KeyRebind rebind = MakeEnabledRebind('A', 'B');
    rebind.useCustomOutput = true;
    rebind.customOutputVK = 'C';
    PrepareRebindRuntimeCase("key_rebind_runtime_split_vk_output", { rebind });
    ScopedRebindMessageCapture capture(window.hwnd());

    ScopedKeyboardStateOverride keyboardState;
    keyboardState.SetKeyDown(VK_SHIFT, false);
    keyboardState.SetToggle(VK_CAPITAL, false);
    keyboardState.Apply();

    const LPARAM sourceLParam = BuildTestKeyboardMessageLParam('A', true);
    const InputHandlerResult keyDownResult = HandleKeyRebinding(window.hwnd(), WM_KEYDOWN, 'A', sourceLParam);
    Expect(keyDownResult.consumed, "Expected split rebind WM_KEYDOWN to be consumed.");
    Expect(capture.messages.size() == 1, "Expected split rebind WM_KEYDOWN to forward exactly one key message.");
    ExpectCapturedMessage(capture, 0, WM_KEYDOWN, 'B', "Split rebind trigger WM_KEYDOWN");

    capture.Clear();
    const InputHandlerResult charResult = HandleCharRebinding(window.hwnd(), WM_CHAR, static_cast<WPARAM>('a'), sourceLParam);
    Expect(charResult.consumed, "Expected split rebind WM_CHAR to be consumed.");
    Expect(capture.messages.size() == 1, "Expected split rebind WM_CHAR to forward exactly one character message.");
    ExpectCapturedMessage(capture, 0, WM_CHAR, 'c', "Split rebind typed WM_CHAR");

    capture.Clear();
    const LPARAM systemSourceLParam = BuildTestKeyboardMessageLParam('A', true, true);
    const InputHandlerResult systemKeyDownResult = HandleKeyRebinding(window.hwnd(), WM_SYSKEYDOWN, 'A', systemSourceLParam);
    Expect(systemKeyDownResult.consumed, "Expected split rebind WM_SYSKEYDOWN to be consumed.");
    Expect(capture.messages.size() == 1, "Expected split rebind WM_SYSKEYDOWN to forward exactly one key message.");
    ExpectCapturedMessage(capture, 0, WM_SYSKEYDOWN, 'B', "Split rebind trigger WM_SYSKEYDOWN");

    capture.Clear();
    const InputHandlerResult systemCharResult = HandleCharRebinding(window.hwnd(), WM_SYSCHAR, static_cast<WPARAM>('a'), systemSourceLParam);
    Expect(systemCharResult.consumed, "Expected split rebind WM_SYSCHAR to be consumed.");
    Expect(capture.messages.size() == 1, "Expected split rebind WM_SYSCHAR to forward exactly one character message.");
    ExpectCapturedMessage(capture, 0, WM_SYSCHAR, 'c', "Split rebind typed WM_SYSCHAR");
}

void RunKeyRebindRuntimeSplitUnicodeOutputTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    KeyRebind rebind = MakeEnabledRebind('A', 'B');
    rebind.useCustomOutput = true;
    rebind.customOutputUnicode = 0x00F8;
    PrepareRebindRuntimeCase("key_rebind_runtime_split_unicode_output", { rebind });
    ScopedRebindMessageCapture capture(window.hwnd());

    ScopedKeyboardStateOverride keyboardState;
    keyboardState.SetKeyDown(VK_SHIFT, false);
    keyboardState.SetToggle(VK_CAPITAL, false);
    keyboardState.Apply();

    const InputHandlerResult charResult =
        HandleCharRebinding(window.hwnd(), WM_CHAR, static_cast<WPARAM>('a'), BuildTestKeyboardMessageLParam('A', true));
    Expect(charResult.consumed, "Expected Unicode split rebind WM_CHAR to be consumed.");
    Expect(capture.messages.size() == 1, "Expected Unicode split rebind to forward exactly one character message.");
    ExpectCapturedMessage(capture, 0, WM_CHAR, 0x00F8, "Split rebind Unicode WM_CHAR");
}

void RunKeyRebindRuntimeUnicodeOutputIgnoresVkTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    KeyRebind rebind = MakeEnabledRebind('A', 'B');
    rebind.useCustomOutput = true;
    rebind.customOutputUnicode = '5';
    rebind.customOutputVK = VK_LSHIFT;
    PrepareRebindRuntimeCase("key_rebind_runtime_unicode_output_ignores_vk", { rebind });
    ScopedRebindMessageCapture capture(window.hwnd());

    ResetSyntheticRebindKeyEventsForTest();
    Expect(GetSyntheticRebindKeyEventCountForTest() == 0,
           "Expected the synthetic rebind key event log to start empty for the Unicode-over-VK test.");

    ScopedKeyboardStateOverride keyboardState;
    keyboardState.SetKeyDown(VK_SHIFT, false);
    keyboardState.SetToggle(VK_CAPITAL, false);
    keyboardState.Apply();

    const LPARAM sourceLParam = BuildTestKeyboardMessageLParam('A', true);
    const InputHandlerResult keyDownResult = HandleKeyRebinding(window.hwnd(), WM_KEYDOWN, 'A', sourceLParam);
    Expect(keyDownResult.consumed, "Expected the Unicode-over-VK rebind to consume WM_KEYDOWN.");
    Expect(keyDownResult.result == kCapturedWndProcResult,
           "Expected the Unicode-over-VK rebind to forward the trigger WM_KEYDOWN through the original WNDPROC.");
    Expect(capture.messages.size() == 1, "Expected the Unicode-over-VK rebind to forward exactly one trigger key-down message.");
    ExpectCapturedMessage(capture, 0, WM_KEYDOWN, 'B', "Unicode-over-VK rebind trigger WM_KEYDOWN");
    Expect(GetSyntheticRebindKeyEventCountForTest() == 0,
           "Expected Unicode output to ignore the configured VK when handling WM_KEYDOWN.");

    capture.Clear();
    const InputHandlerResult charResult = HandleCharRebinding(window.hwnd(), WM_CHAR, static_cast<WPARAM>('a'), sourceLParam);
    Expect(charResult.consumed, "Expected the Unicode-over-VK rebind WM_CHAR to be consumed.");
    Expect(capture.messages.size() == 1, "Expected the Unicode-over-VK rebind to forward exactly one character message.");
    ExpectCapturedMessage(capture, 0, WM_CHAR, '5', "Unicode-over-VK rebind WM_CHAR");

    capture.Clear();
    const InputHandlerResult keyUpResult = HandleKeyRebinding(window.hwnd(), WM_KEYUP, 'A', BuildTestKeyboardMessageLParam('A', false));
    Expect(keyUpResult.consumed, "Expected the Unicode-over-VK rebind to consume WM_KEYUP.");
    Expect(keyUpResult.result == kCapturedWndProcResult,
           "Expected the Unicode-over-VK rebind to forward the trigger WM_KEYUP through the original WNDPROC.");
    Expect(capture.messages.size() == 1, "Expected the Unicode-over-VK rebind to forward exactly one trigger key-up message.");
    ExpectCapturedMessage(capture, 0, WM_KEYUP, 'B', "Unicode-over-VK rebind trigger WM_KEYUP");
    Expect(GetSyntheticRebindKeyEventCountForTest() == 0,
           "Expected Unicode output to ignore the configured VK when handling WM_KEYUP.");
}

void RunKeyRebindRuntimeShiftLayerUnicodeOutputIgnoresVkTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    KeyRebind rebind = MakeEnabledRebind('A', 'B');
    rebind.useCustomOutput = true;
    rebind.customOutputUnicode = 'c';
    rebind.shiftLayerEnabled = true;
    rebind.shiftLayerOutputUnicode = '5';
    rebind.shiftLayerOutputVK = VK_LSHIFT;
    PrepareRebindRuntimeCase("key_rebind_runtime_shift_layer_unicode_output_ignores_vk", { rebind });
    ScopedRebindMessageCapture capture(window.hwnd());

    ResetSyntheticRebindKeyEventsForTest();
    Expect(GetSyntheticRebindKeyEventCountForTest() == 0,
           "Expected the synthetic rebind key event log to start empty for the Shift-layer Unicode-over-VK test.");

    ScopedKeyboardStateOverride keyboardState;
    keyboardState.SetKeyDown(VK_SHIFT, true);
    keyboardState.SetToggle(VK_CAPITAL, false);
    keyboardState.Apply();

    const LPARAM sourceLParam = BuildTestKeyboardMessageLParam('A', true);
    const InputHandlerResult keyDownResult = HandleKeyRebinding(window.hwnd(), WM_KEYDOWN, 'A', sourceLParam);
    Expect(keyDownResult.consumed, "Expected the Shift-layer Unicode-over-VK rebind to consume WM_KEYDOWN.");
    Expect(keyDownResult.result == kCapturedWndProcResult,
           "Expected the Shift-layer Unicode-over-VK rebind to forward the trigger WM_KEYDOWN through the original WNDPROC.");
    Expect(capture.messages.size() == 1,
           "Expected the Shift-layer Unicode-over-VK rebind to forward exactly one trigger key-down message.");
    ExpectCapturedMessage(capture, 0, WM_KEYDOWN, 'B', "Shift-layer Unicode-over-VK rebind trigger WM_KEYDOWN");
    Expect(GetSyntheticRebindKeyEventCountForTest() == 0,
           "Expected Shift-layer Unicode output to ignore the configured VK when handling WM_KEYDOWN.");

    capture.Clear();
    const InputHandlerResult charResult =
        HandleCharRebinding(window.hwnd(), WM_CHAR, static_cast<WPARAM>('A'), sourceLParam);
    Expect(charResult.consumed, "Expected the Shift-layer Unicode-over-VK rebind WM_CHAR to be consumed.");
    Expect(capture.messages.size() == 1,
           "Expected the Shift-layer Unicode-over-VK rebind to forward exactly one character message.");
    ExpectCapturedMessage(capture, 0, WM_CHAR, '5', "Shift-layer Unicode-over-VK rebind WM_CHAR");

    capture.Clear();
    const InputHandlerResult keyUpResult = HandleKeyRebinding(window.hwnd(), WM_KEYUP, 'A', BuildTestKeyboardMessageLParam('A', false));
    Expect(keyUpResult.consumed, "Expected the Shift-layer Unicode-over-VK rebind to consume WM_KEYUP.");
    Expect(keyUpResult.result == kCapturedWndProcResult,
           "Expected the Shift-layer Unicode-over-VK rebind to forward the trigger WM_KEYUP through the original WNDPROC.");
    Expect(capture.messages.size() == 1,
           "Expected the Shift-layer Unicode-over-VK rebind to forward exactly one trigger key-up message.");
    ExpectCapturedMessage(capture, 0, WM_KEYUP, 'B', "Shift-layer Unicode-over-VK rebind trigger WM_KEYUP");
    Expect(GetSyntheticRebindKeyEventCountForTest() == 0,
           "Expected Shift-layer Unicode output to ignore the configured VK when handling WM_KEYUP.");
}

void RunKeyRebindRuntimeShiftLayerShiftActivatedTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    KeyRebind rebind = MakeEnabledRebind('A', 'B');
    rebind.useCustomOutput = true;
    rebind.customOutputVK = 'C';
    rebind.shiftLayerEnabled = true;
    rebind.shiftLayerOutputVK = 'D';
    rebind.shiftLayerOutputShifted = true;
    PrepareRebindRuntimeCase("key_rebind_runtime_shift_layer_shift_activated", { rebind });
    ScopedRebindMessageCapture capture(window.hwnd());

    ScopedKeyboardStateOverride keyboardState;
    keyboardState.SetKeyDown(VK_SHIFT, true);
    keyboardState.SetToggle(VK_CAPITAL, false);
    keyboardState.Apply();

    const InputHandlerResult charResult =
        HandleCharRebinding(window.hwnd(), WM_CHAR, static_cast<WPARAM>('A'), BuildTestKeyboardMessageLParam('A', true));
    Expect(charResult.consumed, "Expected Shift-layer WM_CHAR to be consumed while Shift is down.");
    Expect(capture.messages.size() == 1, "Expected Shift-layer WM_CHAR to forward exactly one character message.");
    ExpectCapturedMessage(capture, 0, WM_CHAR, 'D', "Shift-layer WM_CHAR");
}

void RunKeyRebindRuntimeShiftLayerCapsLockActivatedTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    KeyRebind rebind = MakeEnabledRebind('A', 'B');
    rebind.useCustomOutput = true;
    rebind.customOutputVK = 'C';
    rebind.shiftLayerEnabled = true;
    rebind.shiftLayerUsesCapsLock = true;
    rebind.shiftLayerOutputUnicode = 0x00D8;
    PrepareRebindRuntimeCase("key_rebind_runtime_shift_layer_caps_lock_activated", { rebind });
    ScopedRebindMessageCapture capture(window.hwnd());

    ScopedKeyboardStateOverride keyboardState;
    keyboardState.SetKeyDown(VK_SHIFT, false);
    keyboardState.SetToggle(VK_CAPITAL, true);
    keyboardState.Apply();

    const InputHandlerResult charResult =
        HandleCharRebinding(window.hwnd(), WM_CHAR, static_cast<WPARAM>('A'), BuildTestKeyboardMessageLParam('A', true));
    Expect(charResult.consumed, "Expected Caps-Lock-driven Shift-layer WM_CHAR to be consumed.");
    Expect(capture.messages.size() == 1,
           "Expected Caps-Lock-driven Shift-layer WM_CHAR to forward exactly one character message.");
    ExpectCapturedMessage(capture, 0, WM_CHAR, 0x00D8, "Caps-Lock Shift-layer WM_CHAR");
}

void RunKeyRebindRuntimeFullCapsLockHonoredTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    KeyRebind rebind = MakeEnabledRebind('A', 'B');
    rebind.useCustomOutput = true;
    rebind.customOutputVK = 'B';
    PrepareRebindRuntimeCase("key_rebind_runtime_full_caps_lock_honored", { rebind });
    ScopedRebindMessageCapture capture(window.hwnd());

    ScopedKeyboardStateOverride keyboardState;
    keyboardState.SetKeyDown(VK_SHIFT, false);
    keyboardState.SetToggle(VK_CAPITAL, true);
    keyboardState.Apply();

    const InputHandlerResult charResult =
        HandleCharRebinding(window.hwnd(), WM_CHAR, static_cast<WPARAM>('A'), BuildTestKeyboardMessageLParam('A', true));
    Expect(charResult.consumed, "Expected full rebind WM_CHAR to be consumed while Caps Lock is on.");
    Expect(capture.messages.size() == 1, "Expected full rebind WM_CHAR to forward exactly one character message.");
    ExpectCapturedMessage(capture, 0, WM_CHAR, 'B', "Full rebind Caps Lock WM_CHAR");
}

void RunKeyRebindRuntimeSplitCapsLockIgnoredWithoutOptInTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    KeyRebind rebind = MakeEnabledRebind('A', 'B');
    rebind.useCustomOutput = true;
    rebind.customOutputVK = 'C';
    PrepareRebindRuntimeCase("key_rebind_runtime_split_caps_lock_ignored", { rebind });
    ScopedRebindMessageCapture capture(window.hwnd());

    ScopedKeyboardStateOverride keyboardState;
    keyboardState.SetKeyDown(VK_SHIFT, false);
    keyboardState.SetToggle(VK_CAPITAL, true);
    keyboardState.Apply();

    const InputHandlerResult charResult =
        HandleCharRebinding(window.hwnd(), WM_CHAR, static_cast<WPARAM>('A'), BuildTestKeyboardMessageLParam('A', true));
    Expect(charResult.consumed, "Expected split rebind WM_CHAR to be consumed while Caps Lock is on.");
    Expect(capture.messages.size() == 1, "Expected split rebind WM_CHAR to forward exactly one character message.");
    ExpectCapturedMessage(capture, 0, WM_CHAR, 'c', "Split rebind Caps Lock WM_CHAR");
}

void RunKeyRebindRuntimeNonTypableTriggerConsumesCharTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    PrepareRebindRuntimeCase("key_rebind_runtime_non_typable_trigger", { MakeEnabledRebind('A', VK_HOME) });
    ScopedRebindMessageCapture capture(window.hwnd());

    ScopedKeyboardStateOverride keyboardState;
    keyboardState.SetKeyDown(VK_SHIFT, false);
    keyboardState.SetToggle(VK_CAPITAL, false);
    keyboardState.Apply();

    const InputHandlerResult charResult =
        HandleCharRebinding(window.hwnd(), WM_CHAR, static_cast<WPARAM>('a'), BuildTestKeyboardMessageLParam('A', true));
    Expect(charResult.consumed, "Expected a rebind that cannot type to consume WM_CHAR.");
    Expect(charResult.result == 0, "Expected a rebind that cannot type to return result 0.");
    Expect(capture.messages.empty(), "Expected a rebind that cannot type to avoid forwarding WM_CHAR.");
}

void RunKeyRebindRuntimeTriggerDisabledStillTypesTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    KeyRebind rebind = MakeEnabledRebind('A', 'B');
    rebind.triggerOutputDisabled = true;
    rebind.useCustomOutput = true;
    rebind.customOutputVK = 'C';
    PrepareRebindRuntimeCase("key_rebind_runtime_trigger_disabled_still_types", { rebind });
    ScopedRebindMessageCapture capture(window.hwnd());

    ScopedKeyboardStateOverride keyboardState;
    keyboardState.SetKeyDown(VK_SHIFT, false);
    keyboardState.SetToggle(VK_CAPITAL, false);
    keyboardState.Apply();

    const LPARAM sourceLParam = BuildTestKeyboardMessageLParam('A', true);
    const InputHandlerResult keyDownResult = HandleKeyRebinding(window.hwnd(), WM_KEYDOWN, 'A', sourceLParam);
    Expect(keyDownResult.consumed, "Expected a trigger-disabled rebind to consume WM_KEYDOWN.");
    Expect(capture.messages.empty(), "Expected a trigger-disabled rebind to avoid forwarding WM_KEYDOWN.");

    const InputHandlerResult charResult = HandleCharRebinding(window.hwnd(), WM_CHAR, static_cast<WPARAM>('a'), sourceLParam);
    Expect(charResult.consumed, "Expected a trigger-disabled rebind to consume WM_CHAR.");
    Expect(capture.messages.size() == 1, "Expected a trigger-disabled rebind to still emit one WM_CHAR message.");
    ExpectCapturedMessage(capture, 0, WM_CHAR, 'c', "Trigger-disabled rebind WM_CHAR");
}

void RunKeyRebindRuntimeTypesDisabledStillTriggersTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    KeyRebind rebind = MakeEnabledRebind('A', 'B');
    rebind.baseOutputDisabled = true;
    rebind.useCustomOutput = true;
    rebind.customOutputVK = 'C';
    PrepareRebindRuntimeCase("key_rebind_runtime_types_disabled_still_triggers", { rebind });
    ScopedRebindMessageCapture capture(window.hwnd());

    ScopedKeyboardStateOverride keyboardState;
    keyboardState.SetKeyDown(VK_SHIFT, false);
    keyboardState.SetToggle(VK_CAPITAL, false);
    keyboardState.Apply();

    const LPARAM sourceLParam = BuildTestKeyboardMessageLParam('A', true);
    const InputHandlerResult keyDownResult = HandleKeyRebinding(window.hwnd(), WM_KEYDOWN, 'A', sourceLParam);
    Expect(keyDownResult.consumed, "Expected a types-disabled rebind to consume WM_KEYDOWN.");
    Expect(capture.messages.size() == 1, "Expected a types-disabled rebind to still forward one trigger WM_KEYDOWN message.");
    ExpectCapturedMessage(capture, 0, WM_KEYDOWN, 'B', "Types-disabled rebind trigger WM_KEYDOWN");

    capture.Clear();
    const InputHandlerResult charResult = HandleCharRebinding(window.hwnd(), WM_CHAR, static_cast<WPARAM>('a'), sourceLParam);
    Expect(charResult.consumed, "Expected a types-disabled rebind to consume WM_CHAR.");
    Expect(capture.messages.empty(), "Expected a types-disabled rebind to avoid forwarding WM_CHAR.");
}

void RunKeyRebindRuntimeShiftTypesDisabledStillTriggersTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    KeyRebind rebind = MakeEnabledRebind('A', 'B');
    rebind.useCustomOutput = true;
    rebind.customOutputVK = 'C';
    rebind.shiftLayerEnabled = true;
    rebind.shiftLayerOutputDisabled = true;
    rebind.shiftLayerOutputVK = 'D';
    PrepareRebindRuntimeCase("key_rebind_runtime_shift_types_disabled_still_triggers", { rebind });
    ScopedRebindMessageCapture capture(window.hwnd());

    ScopedKeyboardStateOverride keyboardState;
    keyboardState.SetKeyDown(VK_SHIFT, false);
    keyboardState.SetToggle(VK_CAPITAL, false);
    keyboardState.Apply();

    const LPARAM sourceLParam = BuildTestKeyboardMessageLParam('A', true);
    const InputHandlerResult baseCharResult =
        HandleCharRebinding(window.hwnd(), WM_CHAR, static_cast<WPARAM>('a'), sourceLParam);
    Expect(baseCharResult.consumed, "Expected the base split rebind WM_CHAR to be consumed before Shift-layer disable activates.");
    Expect(capture.messages.size() == 1, "Expected the base split rebind to emit one WM_CHAR message before Shift-layer disable activates.");
    ExpectCapturedMessage(capture, 0, WM_CHAR, 'c', "Base split rebind WM_CHAR before Shift-layer disable");

    capture.Clear();
    keyboardState.SetKeyDown(VK_SHIFT, true);
    keyboardState.Apply();

    const InputHandlerResult keyDownResult = HandleKeyRebinding(window.hwnd(), WM_KEYDOWN, 'A', sourceLParam);
    Expect(keyDownResult.consumed, "Expected a Shift-layer-types-disabled rebind to consume WM_KEYDOWN.");
    Expect(capture.messages.size() == 1, "Expected a Shift-layer-types-disabled rebind to still forward one trigger WM_KEYDOWN message.");
    ExpectCapturedMessage(capture, 0, WM_KEYDOWN, 'B', "Shift-layer-types-disabled rebind trigger WM_KEYDOWN");

    capture.Clear();
    const InputHandlerResult shiftCharResult =
        HandleCharRebinding(window.hwnd(), WM_CHAR, static_cast<WPARAM>('A'), sourceLParam);
    Expect(shiftCharResult.consumed, "Expected a Shift-layer-types-disabled rebind to consume WM_CHAR while Shift is down.");
    Expect(capture.messages.empty(), "Expected a Shift-layer-types-disabled rebind to avoid forwarding WM_CHAR while Shift is down.");
}

void RunKeyRebindRuntimeMouseSourceEmitsKeyAndCharTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    KeyRebind rebind = MakeEnabledRebind(VK_LBUTTON, 'B');
    rebind.useCustomOutput = true;
    rebind.customOutputVK = 'B';
    PrepareRebindRuntimeCase("key_rebind_runtime_mouse_source_emits_key_and_char", { rebind });
    ScopedRebindMessageCapture capture(window.hwnd());

    ScopedKeyboardStateOverride keyboardState;
    keyboardState.SetKeyDown(VK_SHIFT, false);
    keyboardState.SetToggle(VK_CAPITAL, false);
    keyboardState.Apply();

    const InputHandlerResult mouseDownResult = HandleKeyRebinding(window.hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(12, 34));
    Expect(mouseDownResult.consumed, "Expected a mouse-source rebind to consume WM_LBUTTONDOWN.");
    Expect(mouseDownResult.result == kCapturedWndProcResult,
           "Expected a mouse-source rebind to forward the rebinding result through the original WNDPROC.");
    Expect(capture.messages.size() == 2,
           "Expected a mouse-source rebind to forward both the target WM_KEYDOWN and generated WM_CHAR.");
    ExpectCapturedMessage(capture, 0, WM_KEYDOWN, 'B', "Mouse-source rebind WM_KEYDOWN");
    ExpectCapturedMessage(capture, 1, WM_CHAR, 'b', "Mouse-source rebind WM_CHAR");

    capture.Clear();
    const InputHandlerResult mouseUpResult = HandleKeyRebinding(window.hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(12, 34));
    Expect(mouseUpResult.consumed, "Expected a mouse-source rebind to consume WM_LBUTTONUP.");
    Expect(capture.messages.size() == 1, "Expected a mouse-source rebind WM_LBUTTONUP to forward exactly one key-up message.");
    ExpectCapturedMessage(capture, 0, WM_KEYUP, 'B', "Mouse-source rebind WM_KEYUP");
}

void RunKeyRebindRuntimePlainKeyOutputReleasedOnTeardownTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    PrepareRebindRuntimeCase("key_rebind_runtime_plain_key_output_released_on_teardown",
                             { MakeEnabledRebind('A', 'B') });
    ScopedRebindMessageCapture capture(window.hwnd());

    const InputHandlerResult keyDownResult =
        HandleKeyRebinding(window.hwnd(), WM_KEYDOWN, 'A', BuildTestKeyboardMessageLParam('A', true));
    Expect(keyDownResult.consumed, "Expected the plain key-to-key rebind to consume WM_KEYDOWN.");
    Expect(capture.messages.size() == 1, "Expected the plain key rebind to forward exactly one WM_KEYDOWN.");
    ExpectCapturedMessage(capture, 0, WM_KEYDOWN, 'B', "Plain key rebind WM_KEYDOWN");

    capture.Clear();
    (void)HandleActivate(window.hwnd(), WM_KILLFOCUS, 0, 0);
    Expect(capture.messages.size() == 1,
           "Expected losing focus mid-hold to forward a matching WM_KEYUP for the held output key.");
    ExpectCapturedMessage(capture, 0, WM_KEYUP, 'B', "Plain key rebind forced-release WM_KEYUP");

    capture.Clear();
    (void)HandleActivate(window.hwnd(), WM_KILLFOCUS, 0, 0);
    Expect(capture.messages.empty(),
           "Expected a second teardown to forward nothing once the held output was already released.");
}

void RunKeyRebindRuntimePassthroughSourceReleasedOnEnableTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    PrepareRebindRuntimeCase("key_rebind_runtime_passthrough_source_released_on_enable",
                             { MakeEnabledRebind(VK_LCONTROL, 'Z') });
    g_config.keyRebinds.enabled = false;
    g_config.keyRebinds.toggleHotkey = { VK_F8 };
    PublishConfigSnapshot();

    ScopedRebindMessageCapture capture(window.hwnd());

    const InputHandlerResult downResult =
        HandleKeyRebinding(window.hwnd(), WM_KEYDOWN, VK_CONTROL, BuildTestKeyboardMessageLParam(VK_LCONTROL, true));
    Expect(!downResult.consumed, "Expected a source keydown while rebinds are disabled to pass through untouched.");
    Expect(capture.messages.empty(), "Expected the disabled-passthrough keydown not to forward any synthetic message.");

    const InputHandlerResult toggleResult =
        HandleKeyRebindsToggle(window.hwnd(), WM_KEYDOWN, VK_F8, BuildTestKeyboardMessageLParam(VK_F8, true));
    Expect(toggleResult.consumed, "Expected the rebind toggle hotkey to be consumed.");
    Expect(g_config.keyRebinds.enabled, "Expected the toggle hotkey to enable rebinds.");

    Expect(capture.messages.size() == 1, "Expected enabling mid-hold to forward exactly one real source key-up.");
    ExpectCapturedMessage(capture, 0, WM_KEYUP, VK_CONTROL, "Passthrough source release WM_KEYUP");
}

void RunHotkeyRuntimeExclusionHonoredOnTriggerOnReleaseTest(TestRunMode /*runMode*/ = TestRunMode::Automated) {
    const std::vector<DWORD> keys = { VK_MENU };
    const std::vector<DWORD> exclusions = { VK_SHIFT };

    ScopedKeyboardStateOverride keyboardState;
    keyboardState.SetKeyDown(VK_SHIFT, true);
    keyboardState.SetKeyDown(VK_MENU, false);
    keyboardState.Apply();

    Expect(!CheckHotkeyMatch(keys, VK_MENU, exclusions, true, 0, VK_MENU, true, false, false),
           "Expected a held Shift exclusion to block an Alt trigger-on-release match.");
    Expect(CheckHotkeyMatch(keys, VK_MENU, exclusions, true, 0, VK_MENU, true, false, true),
           "Expected trigger-on-hold release to skip exclusions so the mode can always deactivate.");

    keyboardState.SetKeyDown(VK_SHIFT, false);
    keyboardState.Apply();
    Expect(CheckHotkeyMatch(keys, VK_MENU, exclusions, true, 0, VK_MENU, true, false, false),
           "Expected the Alt trigger-on-release match to succeed when the excluded key is not held.");
}

    void RunKeyRebindRuntimeModifierOutputReleasedOnDeactivateTest(TestRunMode runMode = TestRunMode::Automated) {
        DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
        PrepareRebindRuntimeCase("key_rebind_runtime_modifier_output_released_on_deactivate",
                     { MakeEnabledRebind('A', VK_LCONTROL) });

        ResetSyntheticRebindKeyEventsForTest();
        Expect(GetSyntheticRebindKeyEventCountForTest() == 0,
            "Expected the synthetic rebind key event log to start empty.");
        Expect(GetActiveSyntheticRebindOutputCountForTest() == 0,
            "Expected no held synthetic rebind outputs before the modifier rebind test runs.");

        const UINT expectedScanCodeWithFlags = static_cast<UINT>(MapVirtualKeyW(VK_LCONTROL, MAPVK_VK_TO_VSC_EX));
        const InputHandlerResult keyDownResult =
         HandleKeyRebinding(window.hwnd(), WM_KEYDOWN, 'A', BuildTestKeyboardMessageLParam('A', true));
        Expect(keyDownResult.consumed, "Expected the modifier-output rebind to consume WM_KEYDOWN.");
        Expect(GetSyntheticRebindKeyEventCountForTest() == 1,
            "Expected the modifier-output rebind to synthesize exactly one key-down event.");
        ExpectSyntheticRebindKeyEvent(0, expectedScanCodeWithFlags, true, "Modifier-output rebind WM_KEYDOWN");
        Expect(GetActiveSyntheticRebindOutputCountForTest() == 1,
            "Expected the modifier-output rebind to track one held synthetic output after key down.");

        (void)HandleActivate(window.hwnd(), WM_KILLFOCUS, 0, 0);

        Expect(GetSyntheticRebindKeyEventCountForTest() == 2,
            "Expected the modifier-output rebind to synthesize a matching key-up event on focus loss.");
        ExpectSyntheticRebindKeyEvent(1, expectedScanCodeWithFlags, false, "Modifier-output rebind focus-loss release");
        Expect(GetActiveSyntheticRebindOutputCountForTest() == 0,
            "Expected focus loss to clear all held synthetic rebind outputs.");
    }

    void RunKeyRebindRuntimeSuppressedCapsLockReleasedOnDeactivateTest(TestRunMode runMode = TestRunMode::Automated) {
        DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
        PrepareRebindRuntimeCase("key_rebind_runtime_suppressed_caps_lock_released_on_deactivate", {});
        g_config.keyRebinds.enabled = false;
        g_config.keyRebinds.suppressCapsLockToggle = true;
        PublishConfigSnapshot();

        Expect(window.PumpMessages(), "Expected the test window to stay open before suppressed Caps Lock capture setup.");
        ScopedSubclassedInputCapture capture(window.hwnd());
        Expect(window.PumpMessages(), "Expected the test window to stay open after suppressed Caps Lock capture setup.");
        capture.Clear();

        const UINT capsLockScanCodeWithFlags = static_cast<UINT>(MapVirtualKeyW(VK_CAPITAL, MAPVK_VK_TO_VSC_EX));
        QueueSuppressedLowLevelKeyForTest(VK_CAPITAL, capsLockScanCodeWithFlags, false);

        ReleaseActiveLowLevelRebindKeys(window.hwnd());
        Expect(window.PumpMessages(), "Expected releasing suppressed Caps Lock on deactivate to keep the window open.");

        Expect(capture.messages.size() == 1,
            "Expected releasing suppressed Caps Lock without a matching rebind to forward one WM_KEYUP message.");
        ExpectCapturedMessage(capture, 0, WM_KEYUP, VK_CAPITAL, "Suppressed Caps Lock focus-loss release WM_KEYUP");
        ExpectCapturedScanCode(capture, 0, capsLockScanCodeWithFlags, "Suppressed Caps Lock focus-loss release scan code");
    }

    void RunKeyRebindRuntimeCustomModifierOutputUsesSyntheticKeyTest(TestRunMode runMode = TestRunMode::Automated) {
        DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
        KeyRebind rebind = MakeEnabledRebind('A', 'B');
        rebind.useCustomOutput = true;
        rebind.customOutputVK = VK_LSHIFT;
        PrepareRebindRuntimeCase("key_rebind_runtime_custom_modifier_output_uses_synthetic_key", { rebind });
        ScopedRebindMessageCapture capture(window.hwnd());

        ResetSyntheticRebindKeyEventsForTest();
        Expect(GetSyntheticRebindKeyEventCountForTest() == 0,
            "Expected the synthetic rebind key event log to start empty for the custom modifier-output test.");
        Expect(GetActiveSyntheticRebindOutputCountForTest() == 0,
            "Expected no held synthetic outputs before the custom modifier-output test runs.");

        ScopedKeyboardStateOverride keyboardState;
        keyboardState.SetKeyDown(VK_SHIFT, false);
        keyboardState.SetToggle(VK_CAPITAL, false);
        keyboardState.Apply();

        const UINT expectedScanCodeWithFlags = static_cast<UINT>(MapVirtualKeyW(VK_LSHIFT, MAPVK_VK_TO_VSC_EX));
        const LPARAM sourceLParam = BuildTestKeyboardMessageLParam('A', true);

        const InputHandlerResult keyDownResult = HandleKeyRebinding(window.hwnd(), WM_KEYDOWN, 'A', sourceLParam);
        Expect(keyDownResult.consumed, "Expected a custom modifier-output rebind to consume WM_KEYDOWN.");
        Expect(capture.messages.empty(), "Expected a custom modifier-output rebind to avoid forwarding WM_KEYDOWN messages.");
        Expect(GetSyntheticRebindKeyEventCountForTest() == 1,
            "Expected a custom modifier-output rebind to synthesize one key-down event.");
        ExpectSyntheticRebindKeyEvent(0, expectedScanCodeWithFlags, true, "Custom modifier-output rebind WM_KEYDOWN");
        Expect(GetActiveSyntheticRebindOutputCountForTest() == 1,
            "Expected a custom modifier-output rebind to track one held synthetic output after key down.");

        capture.Clear();
        const InputHandlerResult keyUpResult =
            HandleKeyRebinding(window.hwnd(), WM_KEYUP, 'A', BuildTestKeyboardMessageLParam('A', false));
        Expect(keyUpResult.consumed, "Expected a custom modifier-output rebind to consume WM_KEYUP.");
        Expect(capture.messages.empty(), "Expected a custom modifier-output rebind to avoid forwarding WM_KEYUP messages.");
        Expect(GetSyntheticRebindKeyEventCountForTest() == 2,
            "Expected a custom modifier-output rebind to synthesize a matching key-up event.");
        ExpectSyntheticRebindKeyEvent(1, expectedScanCodeWithFlags, false, "Custom modifier-output rebind WM_KEYUP");
        Expect(GetActiveSyntheticRebindOutputCountForTest() == 0,
            "Expected the custom modifier-output rebind to clear held synthetic output state on key up.");
    }

    void RunKeyRebindRuntimeWndProcKeepsSyntheticModifierHeldTest(TestRunMode runMode = TestRunMode::Automated) {
        DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
        KeyRebind rebind = MakeEnabledRebind('N', VK_LSHIFT);
        PrepareRebindRuntimeCase("key_rebind_runtime_wndproc_keeps_synthetic_modifier_held", { rebind });
        ScopedRebindMessageCapture capture(window.hwnd());

        const HWND previousSubclassedHwnd = g_subclassedHwnd.load(std::memory_order_acquire);
        g_subclassedHwnd.store(window.hwnd(), std::memory_order_release);

        ResetSyntheticRebindKeyEventsForTest();
        Expect(GetActiveSyntheticRebindOutputCountForTest() == 0,
            "Expected no held synthetic outputs before the subclassed WndProc modifier-hold test runs.");

        const UINT expectedScanCodeWithFlags = static_cast<UINT>(MapVirtualKeyW(VK_LSHIFT, MAPVK_VK_TO_VSC_EX));
        const LPARAM sourceDownLParam = BuildTestKeyboardMessageLParam('N', true);
        const LPARAM sourceUpLParam = BuildTestKeyboardMessageLParam('N', false);

        try {
            const LRESULT keyDownResult = SubclassedWndProc(window.hwnd(), WM_KEYDOWN, 'N', sourceDownLParam);
            Expect(keyDownResult == 0 || keyDownResult == kCapturedWndProcResult,
                "Expected the subclassed WndProc modifier-hold test to return a handled result for WM_KEYDOWN.");
            Expect(GetSyntheticRebindKeyEventCountForTest() == 1,
                "Expected the subclassed WndProc modifier-hold test to synthesize one key-down event.");
            ExpectSyntheticRebindKeyEvent(0, expectedScanCodeWithFlags, true, "Subclassed WndProc modifier-hold WM_KEYDOWN");
            Expect(GetActiveSyntheticRebindOutputCountForTest() == 1,
                "Expected the subclassed WndProc modifier-hold test to keep the synthetic modifier held after WM_KEYDOWN.");

            (void)SubclassedWndProc(window.hwnd(), WM_MOUSEMOVE, 0, MAKELPARAM(32, 48));
            Expect(GetActiveSyntheticRebindOutputCountForTest() == 1,
                "Expected unrelated WndProc traffic to preserve a held synthetic modifier output.");

            const LRESULT keyUpResult = SubclassedWndProc(window.hwnd(), WM_KEYUP, 'N', sourceUpLParam);
            Expect(keyUpResult == 0 || keyUpResult == kCapturedWndProcResult,
                "Expected the subclassed WndProc modifier-hold test to return a handled result for WM_KEYUP.");
            Expect(GetSyntheticRebindKeyEventCountForTest() == 2,
                "Expected the subclassed WndProc modifier-hold test to synthesize a matching key-up event.");
            ExpectSyntheticRebindKeyEvent(1, expectedScanCodeWithFlags, false, "Subclassed WndProc modifier-hold WM_KEYUP");
            Expect(GetActiveSyntheticRebindOutputCountForTest() == 0,
                "Expected the subclassed WndProc modifier-hold test to release the synthetic modifier on WM_KEYUP.");
        } catch (...) {
            g_subclassedHwnd.store(previousSubclassedHwnd, std::memory_order_release);
            throw;
        }

        g_subclassedHwnd.store(previousSubclassedHwnd, std::memory_order_release);
    }

void RunKeyRebindRuntimeModifierSourceDualShiftReleaseTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    PrepareRebindRuntimeCase("key_rebind_runtime_modifier_source_dual_shift_release",
                 { MakeEnabledRebind(VK_LSHIFT, VK_LCONTROL) });

    ResetSyntheticRebindKeyEventsForTest();
    Expect(GetSyntheticRebindKeyEventCountForTest() == 0,
        "Expected the synthetic rebind key event log to start empty for the dual-shift release test.");
    Expect(GetActiveSyntheticRebindOutputCountForTest() == 0,
        "Expected no held synthetic rebind outputs before the dual-shift release test runs.");

    ScopedKeyboardStateOverride keyboardState;
    keyboardState.SetKeyDown(VK_SHIFT, false);
    keyboardState.Apply();

    const UINT expectedScanCodeWithFlags = static_cast<UINT>(MapVirtualKeyW(VK_LCONTROL, MAPVK_VK_TO_VSC_EX));
    const LPARAM lShiftDownLParam = BuildTestKeyboardMessageLParam(VK_LSHIFT, true);
    const LPARAM rShiftDownLParam = BuildTestKeyboardMessageLParam(VK_RSHIFT, true);
    const LPARAM lShiftUpLParam = BuildTestKeyboardMessageLParam(VK_LSHIFT, false);

    SetPhysicalModifierDownForTest(VK_LSHIFT, true);
    SetPhysicalModifierDownForTest(VK_RSHIFT, false);
    keyboardState.SetKeyDown(VK_LSHIFT, true);
    keyboardState.Apply();

    const InputHandlerResult lShiftDownResult =
        HandleKeyRebinding(window.hwnd(), WM_KEYDOWN, VK_SHIFT, lShiftDownLParam);
    Expect(lShiftDownResult.consumed, "Expected the LShift modifier-source rebind to consume WM_KEYDOWN.");
    Expect(GetSyntheticRebindKeyEventCountForTest() == 1,
        "Expected one synthetic key-down event after LShift down.");
    ExpectSyntheticRebindKeyEvent(0, expectedScanCodeWithFlags, true, "Dual-shift LShift keydown");
    Expect(GetActiveSyntheticRebindOutputCountForTest() == 1,
        "Expected one held synthetic output after LShift down.");

    SetPhysicalModifierDownForTest(VK_RSHIFT, true);
    keyboardState.SetKeyDown(VK_RSHIFT, true);
    keyboardState.Apply();

    const InputHandlerResult rShiftDownResult =
        HandleKeyRebinding(window.hwnd(), WM_KEYDOWN, VK_SHIFT, rShiftDownLParam);
    Expect(!rShiftDownResult.consumed, "Expected RShift keydown not to match the LShift-only rebind.");
    Expect(GetActiveSyntheticRebindOutputCountForTest() == 1,
        "Expected the synthetic output to remain held after unrelated RShift keydown.");

    keyboardState.SetKeyDown(VK_LSHIFT, false);
    keyboardState.SetKeyDown(VK_RSHIFT, false);
    keyboardState.Apply();
    SetPhysicalModifierDownForTest(VK_LSHIFT, true);
    SetPhysicalModifierDownForTest(VK_RSHIFT, false);

    const InputHandlerResult lShiftUpResult =
        HandleKeyRebinding(window.hwnd(), WM_KEYUP, VK_SHIFT, lShiftUpLParam);
    Expect(lShiftUpResult.consumed,
        "Expected LShift keyup to release the synthetic output even when VK resolution is inverted by stale physical state.");
    Expect(GetSyntheticRebindKeyEventCountForTest() == 2,
        "Expected a matching synthetic key-up event after LShift release.");
    ExpectSyntheticRebindKeyEvent(1, expectedScanCodeWithFlags, false, "Dual-shift LShift keyup");
    Expect(GetActiveSyntheticRebindOutputCountForTest() == 0,
        "Expected no stuck synthetic outputs after LShift release with stale physical state.");

    ResetPhysicalModifierStateForTest();
}

void RunKeyRebindRuntimeDisabledRebindIgnoredTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    KeyRebind rebind = MakeEnabledRebind('A', 'B');
    rebind.enabled = false;
    PrepareRebindRuntimeCase("key_rebind_runtime_disabled_rebind_ignored", { rebind });
    ScopedRebindMessageCapture capture(window.hwnd());

    const InputHandlerResult keyResult = HandleKeyRebinding(window.hwnd(), WM_KEYDOWN, 'A', BuildTestKeyboardMessageLParam('A', true));
    Expect(!keyResult.consumed, "Expected disabled rebinds to be ignored for WM_KEYDOWN.");
    Expect(capture.messages.empty(), "Expected disabled rebinds to avoid forwarding WM_KEYDOWN.");

    const InputHandlerResult charResult =
        HandleCharRebinding(window.hwnd(), WM_CHAR, static_cast<WPARAM>('a'), BuildTestKeyboardMessageLParam('A', true));
    Expect(!charResult.consumed, "Expected disabled rebinds to be ignored for WM_CHAR.");
    Expect(capture.messages.empty(), "Expected disabled rebinds to avoid forwarding WM_CHAR.");
}

void RunKeyRebindRuntimeCursorStateKeyupPassthroughTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);

    KeyRebind cursorGrabbedRebind = MakeEnabledRebind(VK_SPACE, VK_BACK);
    cursorGrabbedRebind.cursorState = kKeyRebindCursorStateCursorGrabbed;

    PrepareRebindRuntimeCase("key_rebind_runtime_cursor_state_keyup_passthrough", { cursorGrabbedRebind });
    ScopedRebindMessageCapture capture(window.hwnd());

    ScopedKeyboardStateOverride keyboardState;
    keyboardState.SetKeyDown(VK_SHIFT, false);
    keyboardState.SetToggle(VK_CAPITAL, false);
    keyboardState.Apply();

    ScopedCursorVisibilityOverride cursorVisible(true);

    const LPARAM spaceDownLParam = BuildTestKeyboardMessageLParam(VK_SPACE, true);
    const LPARAM spaceUpLParam = BuildTestKeyboardMessageLParam(VK_SPACE, false);

    const InputHandlerResult keyDownResult =
        HandleKeyRebinding(window.hwnd(), WM_KEYDOWN, VK_SPACE, spaceDownLParam);
    Expect(!keyDownResult.consumed,
        "Expected spacebar keydown to pass through when the cursor is free and the rebind is cursor-grabbed only.");
    Expect(GetUnreboundKeyDownCountForTest() == 1,
        "Expected the unrebound keydown to be tracked for passthrough.");

    capture.Clear();

    KeyRebind cursorFreeRebind = MakeEnabledRebind(VK_SPACE, VK_BACK);
    cursorFreeRebind.cursorState = kKeyRebindCursorStateCursorFree;
    g_config.keyRebinds.rebinds = { cursorFreeRebind };
    PublishConfigSnapshot();

    const InputHandlerResult keyUpResult =
        HandleKeyRebinding(window.hwnd(), WM_KEYUP, VK_SPACE, spaceUpLParam);
    Expect(!keyUpResult.consumed,
        "Expected spacebar keyup to pass through when its keydown was not rebound, even though a rebind now matches.");
    Expect(GetUnreboundKeyDownCountForTest() == 0,
        "Expected the passthrough tracking to be cleared after keyup.");

    capture.Clear();

    const InputHandlerResult reboundDownResult =
        HandleKeyRebinding(window.hwnd(), WM_KEYDOWN, VK_SPACE, spaceDownLParam);
    Expect(reboundDownResult.consumed,
        "Expected spacebar keydown to be consumed when the rebind matches.");
    Expect(capture.messages.size() == 1,
        "Expected the cursor-free rebind to forward exactly one WM_KEYDOWN message.");
    ExpectCapturedMessage(capture, 0, WM_KEYDOWN, VK_BACK, "Cursor-free rebind WM_KEYDOWN");

    capture.Clear();
    const InputHandlerResult reboundUpResult =
        HandleKeyRebinding(window.hwnd(), WM_KEYUP, VK_SPACE, spaceUpLParam);
    Expect(reboundUpResult.consumed,
        "Expected spacebar keyup to be consumed when its keydown was rebound.");
    Expect(capture.messages.size() == 1,
        "Expected the cursor-free rebind to forward exactly one WM_KEYUP message.");
    ExpectCapturedMessage(capture, 0, WM_KEYUP, VK_BACK, "Cursor-free rebind WM_KEYUP");
    Expect(GetUnreboundKeyDownCountForTest() == 0,
        "Expected no passthrough tracking after a fully rebound key down+up cycle.");
}

void RunKeyRebindRuntimeHeldOutputReleasedOnRebindChangeTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);

    KeyRebind initialRebind = MakeEnabledRebind('A', VK_F3);
    PrepareRebindRuntimeCase("key_rebind_runtime_held_output_released_on_rebind_change", { initialRebind });
    ScopedRebindMessageCapture capture(window.hwnd());

    ScopedKeyboardStateOverride keyboardState;
    keyboardState.SetKeyDown(VK_SHIFT, false);
    keyboardState.SetToggle(VK_CAPITAL, false);
    keyboardState.Apply();

    ScopedCursorVisibilityOverride cursorVisible(true);

    const LPARAM downLParam = BuildTestKeyboardMessageLParam('A', true);
    const LPARAM upLParam = BuildTestKeyboardMessageLParam('A', false);

    const InputHandlerResult downResult = HandleKeyRebinding(window.hwnd(), WM_KEYDOWN, 'A', downLParam);
    Expect(downResult.consumed, "Expected the rebind to consume the keydown.");
    Expect(capture.messages.size() == 1, "Expected exactly one forwarded WM_KEYDOWN.");
    ExpectCapturedMessage(capture, 0, WM_KEYDOWN, VK_F3, "Held-output rebind WM_KEYDOWN");

    capture.Clear();

    KeyRebind changedRebind = MakeEnabledRebind('A', VK_BACK);
    g_config.keyRebinds.rebinds = { changedRebind };
    PublishConfigSnapshot();

    const InputHandlerResult upResult = HandleKeyRebinding(window.hwnd(), WM_KEYUP, 'A', upLParam);
    Expect(upResult.consumed, "Expected the keyup to be consumed while a rebound output is held.");
    Expect(capture.messages.size() == 1, "Expected exactly one forwarded WM_KEYUP.");
    ExpectCapturedMessage(capture, 0, WM_KEYUP, VK_F3,
        "Release must key-up the output asserted on keydown (F3), not the output the now-active rebind resolves to.");
}

void RunKeyRebindRuntimeHeldOutputReleasedOnNoMatchUpTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);

    PrepareRebindRuntimeCase("key_rebind_runtime_held_output_released_on_no_match_up",
                             { MakeEnabledRebind('A', VK_F3) });
    ScopedRebindMessageCapture capture(window.hwnd());

    ScopedKeyboardStateOverride keyboardState;
    keyboardState.SetKeyDown(VK_SHIFT, false);
    keyboardState.SetToggle(VK_CAPITAL, false);
    keyboardState.Apply();

    ScopedCursorVisibilityOverride cursorVisible(true);

    const LPARAM downLParam = BuildTestKeyboardMessageLParam('A', true);
    const LPARAM upLParam = BuildTestKeyboardMessageLParam('A', false);

    HandleKeyRebinding(window.hwnd(), WM_KEYDOWN, 'A', downLParam);
    ExpectCapturedMessage(capture, 0, WM_KEYDOWN, VK_F3, "Held-output DOWN forwards F3");

    capture.Clear();
    g_config.keyRebinds.rebinds = {};
    PublishConfigSnapshot();

    HandleKeyRebinding(window.hwnd(), WM_KEYUP, 'A', upLParam);
    Expect(capture.messages.size() == 1,
        "Expected the held F3 to be released even though no rebind matches the keyup.");
    ExpectCapturedMessage(capture, 0, WM_KEYUP, VK_F3,
        "Expected exactly the F3 key-up that the keydown asserted.");
}

void RunKeyRebindRuntimeHeldOutputReleasedOnMouseOutputUpTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);

    PrepareRebindRuntimeCase("key_rebind_runtime_held_output_released_on_mouse_output_up",
                             { MakeEnabledRebind('A', VK_F3) });
    ScopedRebindMessageCapture capture(window.hwnd());

    ScopedKeyboardStateOverride keyboardState;
    keyboardState.SetKeyDown(VK_SHIFT, false);
    keyboardState.SetToggle(VK_CAPITAL, false);
    keyboardState.Apply();

    ScopedCursorVisibilityOverride cursorVisible(true);

    const LPARAM downLParam = BuildTestKeyboardMessageLParam('A', true);
    const LPARAM upLParam = BuildTestKeyboardMessageLParam('A', false);

    HandleKeyRebinding(window.hwnd(), WM_KEYDOWN, 'A', downLParam);
    ExpectCapturedMessage(capture, 0, WM_KEYDOWN, VK_F3, "Held-output DOWN forwards F3");

    capture.Clear();
    g_config.keyRebinds.rebinds = { MakeEnabledRebind('A', VK_RBUTTON) };
    PublishConfigSnapshot();

    HandleKeyRebinding(window.hwnd(), WM_KEYUP, 'A', upLParam);
    Expect(capture.messages.size() == 1,
        "Expected the held F3 to be released even though the keyup now resolves to a mouse-button output.");
    ExpectCapturedMessage(capture, 0, WM_KEYUP, VK_F3,
        "Expected the F3 key-up the keydown asserted, not a mouse-button up.");
}

void RunKeyRebindRuntimeHeldOutputReleasedOnModifierOutputUpTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);

    PrepareRebindRuntimeCase("key_rebind_runtime_held_output_released_on_modifier_output_up",
                             { MakeEnabledRebind('A', VK_F3) });
    ScopedRebindMessageCapture capture(window.hwnd());

    ScopedKeyboardStateOverride keyboardState;
    keyboardState.SetKeyDown(VK_SHIFT, false);
    keyboardState.SetToggle(VK_CAPITAL, false);
    keyboardState.Apply();

    ScopedCursorVisibilityOverride cursorVisible(true);

    const LPARAM downLParam = BuildTestKeyboardMessageLParam('A', true);
    const LPARAM upLParam = BuildTestKeyboardMessageLParam('A', false);

    HandleKeyRebinding(window.hwnd(), WM_KEYDOWN, 'A', downLParam);
    ExpectCapturedMessage(capture, 0, WM_KEYDOWN, VK_F3, "Held-output DOWN forwards F3");

    capture.Clear();
    g_config.keyRebinds.rebinds = { MakeEnabledRebind('A', VK_LSHIFT) };
    PublishConfigSnapshot();

    HandleKeyRebinding(window.hwnd(), WM_KEYUP, 'A', upLParam);
    Expect(capture.messages.size() == 1,
        "Expected the held F3 to be released even though the keyup now resolves to a modifier output.");
    ExpectCapturedMessage(capture, 0, WM_KEYUP, VK_F3,
        "Expected the F3 key-up the keydown asserted, not a modifier release.");
}

void RunKeyRebindRuntimeHeldOutputReleasedOnGuiOpenTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);

    PrepareRebindRuntimeCase("key_rebind_runtime_held_output_released_on_gui_open",
                             { MakeEnabledRebind(VK_LBUTTON, VK_F3) });
    g_config.guiHotkey = { VK_F6 };
    ScopedRebindMessageCapture capture(window.hwnd());

    ScopedKeyboardStateOverride keyboardState;
    keyboardState.SetKeyDown(VK_SHIFT, false);
    keyboardState.SetToggle(VK_CAPITAL, false);
    keyboardState.Apply();

    ScopedCursorVisibilityOverride cursorVisible(true);

    g_showGui.store(false, std::memory_order_release);
    const bool previousGlInitialized = g_glInitialized.load(std::memory_order_acquire);
    g_glInitialized.store(true, std::memory_order_release);

    const InputHandlerResult downResult =
        HandleKeyRebinding(window.hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(12, 34));
    Expect(downResult.consumed, "Expected the mouse-source rebind to consume the button down.");
    ExpectCapturedMessage(capture, 0, WM_KEYDOWN, VK_F3, "Mouse-source DOWN forwards F3");

    capture.Clear();

    const InputHandlerResult toggleResult =
        HandleGuiToggle(window.hwnd(), WM_KEYDOWN, VK_F6, BuildTestKeyboardMessageLParam(VK_F6, true));
    const bool guiOpened = g_showGui.load(std::memory_order_acquire);

    g_showGui.store(false, std::memory_order_release);
    g_glInitialized.store(previousGlInitialized, std::memory_order_release);

    Expect(toggleResult.consumed, "Expected the GUI toggle hotkey to be handled.");
    Expect(guiOpened, "Expected the GUI to open on the toggle hotkey.");
    Expect(capture.messages.size() == 1,
        "Expected opening the GUI while a rebind output is held to release it.");
    ExpectCapturedMessage(capture, 0, WM_KEYUP, VK_F3,
        "Expected the held F3 to be released when the Toolscreen GUI opens mid-hold.");
}

void RunKeyRebindRuntimeCursorStatePriorityAndFallbackTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);

    KeyRebind anyRebind = MakeEnabledRebind('A', 'B');
    anyRebind.useCustomOutput = true;
    anyRebind.customOutputVK = 'B';
    anyRebind.cursorState = kKeyRebindCursorStateAny;

    KeyRebind cursorFreeRebind = MakeEnabledRebind('A', 'C');
    cursorFreeRebind.useCustomOutput = true;
    cursorFreeRebind.customOutputVK = 'C';
    cursorFreeRebind.cursorState = kKeyRebindCursorStateCursorFree;

    KeyRebind cursorGrabbedRebind = MakeEnabledRebind('A', 'D');
    cursorGrabbedRebind.useCustomOutput = true;
    cursorGrabbedRebind.customOutputVK = 'D';
    cursorGrabbedRebind.cursorState = kKeyRebindCursorStateCursorGrabbed;

    PrepareRebindRuntimeCase("key_rebind_runtime_cursor_state_priority_and_fallback",
                             { anyRebind, cursorFreeRebind, cursorGrabbedRebind });
    ScopedRebindMessageCapture capture(window.hwnd());

    ScopedKeyboardStateOverride keyboardState;
    keyboardState.SetKeyDown(VK_SHIFT, false);
    keyboardState.SetToggle(VK_CAPITAL, false);
    keyboardState.Apply();

    const LPARAM keyDownLParam = BuildTestKeyboardMessageLParam('A', true);

    {
        ScopedCursorVisibilityOverride cursorVisible(true);

        const InputHandlerResult keyDownResult = HandleKeyRebinding(window.hwnd(), WM_KEYDOWN, 'A', keyDownLParam);
        Expect(keyDownResult.consumed, "Expected cursor-free rebind to consume WM_KEYDOWN when the cursor is visible.");
        Expect(capture.messages.size() == 1, "Expected visible-cursor rebind to forward exactly one WM_KEYDOWN message.");
        ExpectCapturedMessage(capture, 0, WM_KEYDOWN, 'C', "Cursor-free priority WM_KEYDOWN");

        capture.Clear();
        const InputHandlerResult charResult = HandleCharRebinding(window.hwnd(), WM_CHAR, static_cast<WPARAM>('a'), keyDownLParam);
        Expect(charResult.consumed, "Expected cursor-free rebind to consume WM_CHAR when the cursor is visible.");
        Expect(capture.messages.size() == 1, "Expected visible-cursor rebind to forward exactly one WM_CHAR message.");
        ExpectCapturedMessage(capture, 0, WM_CHAR, 'c', "Cursor-free priority WM_CHAR");
    }

    capture.Clear();
    g_config.keyRebinds.rebinds = { anyRebind, cursorGrabbedRebind };
    PublishConfigSnapshot();

    {
        ScopedCursorVisibilityOverride cursorVisible(true);

        const InputHandlerResult keyDownResult = HandleKeyRebinding(window.hwnd(), WM_KEYDOWN, 'A', keyDownLParam);
        Expect(keyDownResult.consumed,
               "Expected Any rebind to consume WM_KEYDOWN when only a non-matching grabbed-specific rebind exists.");
        Expect(capture.messages.size() == 1, "Expected visible-cursor fallback to forward exactly one WM_KEYDOWN message.");
        ExpectCapturedMessage(capture, 0, WM_KEYDOWN, 'B', "Cursor-state fallback WM_KEYDOWN");

        capture.Clear();
        const InputHandlerResult charResult = HandleCharRebinding(window.hwnd(), WM_CHAR, static_cast<WPARAM>('a'), keyDownLParam);
        Expect(charResult.consumed,
               "Expected Any rebind to consume WM_CHAR when only a non-matching grabbed-specific rebind exists.");
        Expect(capture.messages.size() == 1, "Expected visible-cursor fallback to forward exactly one WM_CHAR message.");
        ExpectCapturedMessage(capture, 0, WM_CHAR, 'b', "Cursor-state fallback WM_CHAR");
    }
}

void RunKeyRepeatRuntimeLastPressedOnlyTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    PrepareRebindRuntimeCase("key_repeat_runtime_last_pressed_only", {});
    g_config.useSystemKeyRepeat = false;
    PublishConfigSnapshot();
    Expect(window.PumpMessages(), "Expected the test window to stay open before local repeat capture setup.");
    ScopedSubclassedInputCapture capture(window.hwnd());
    Expect(window.PumpMessages(), "Expected the test window to stay open after local repeat capture setup.");
    capture.Clear();

    ScopedKeyboardStateOverride keyboardState;
    keyboardState.SetKeyDown(VK_SHIFT, false);
    keyboardState.SetToggle(VK_CAPITAL, false);
    keyboardState.Apply();

    auto postAndPump = [&](UINT message, WPARAM wParam, LPARAM lParam, const std::string& label) {
        Expect(PostMessageW(window.hwnd(), message, wParam, lParam) != FALSE, label + " should post successfully.");
        Expect(window.PumpMessages(), label + " should keep the window open while dispatching messages.");
    };

    postAndPump(WM_KEYDOWN, 'A', BuildTestKeyboardMessageLParam('A', true), "Initial A keydown");
    Expect(capture.messages.size() == 2, "Expected the initial A keydown to forward WM_KEYDOWN and WM_CHAR.");
    ExpectCapturedMessage(capture, 0, WM_KEYDOWN, 'A', "Initial A WM_KEYDOWN");
    ExpectCapturedMessage(capture, 1, WM_CHAR, 'a', "Initial A WM_CHAR");

    capture.Clear();
    postAndPump(WM_TOOLSCREEN_LOCAL_KEY_REPEAT, 0, 0, "First local repeat tick");
    Expect(capture.messages.size() == 2, "Expected the first local repeat tick to repeat the A keydown and character.");
    ExpectCapturedMessage(capture, 0, WM_KEYDOWN, 'A', "Repeated A WM_KEYDOWN");
    ExpectCapturedMessage(capture, 1, WM_CHAR, 'a', "Repeated A WM_CHAR");

    capture.Clear();
    postAndPump(WM_KEYDOWN, 'B', BuildTestKeyboardMessageLParam('B', true), "Initial B keydown");
    Expect(capture.messages.size() == 2, "Expected the initial B keydown to forward WM_KEYDOWN and WM_CHAR.");
    ExpectCapturedMessage(capture, 0, WM_KEYDOWN, 'B', "Initial B WM_KEYDOWN");
    ExpectCapturedMessage(capture, 1, WM_CHAR, 'b', "Initial B WM_CHAR");

    capture.Clear();
    postAndPump(WM_TOOLSCREEN_LOCAL_KEY_REPEAT, 0, 0, "Second local repeat tick");
    Expect(capture.messages.size() == 2, "Expected the second local repeat tick to repeat only the most recently pressed key.");
    ExpectCapturedMessage(capture, 0, WM_KEYDOWN, 'B', "Repeated B WM_KEYDOWN");
    ExpectCapturedMessage(capture, 1, WM_CHAR, 'b', "Repeated B WM_CHAR");

    capture.Clear();
    postAndPump(WM_KEYUP, 'B', BuildTestKeyboardMessageLParam('B', false), "B keyup");
    Expect(capture.messages.size() == 1, "Expected releasing B to forward one WM_KEYUP message.");
    ExpectCapturedMessage(capture, 0, WM_KEYUP, 'B', "B WM_KEYUP");

    capture.Clear();
    postAndPump(WM_TOOLSCREEN_LOCAL_KEY_REPEAT, 0, 0, "Third local repeat tick");
    Expect(capture.messages.empty(), "Expected the earlier A key to stay non-repeating after the newer B key is released.");

    capture.Clear();
    postAndPump(WM_KEYUP, 'A', BuildTestKeyboardMessageLParam('A', false), "A keyup");
}

void RunKeyRepeatRuntimeLocalRepeatUsesRebindTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    KeyRebind rebind = MakeEnabledRebind('A', 'B');
    rebind.useCustomOutput = true;
    rebind.customOutputVK = 'B';
    PrepareRebindRuntimeCase("key_repeat_runtime_local_repeat_uses_rebind", { rebind });
    g_config.useSystemKeyRepeat = false;
    PublishConfigSnapshot();
    Expect(window.PumpMessages(), "Expected the test window to stay open before subclassed repeat capture setup.");
    ScopedSubclassedInputCapture capture(window.hwnd());
    Expect(window.PumpMessages(), "Expected the test window to stay open after subclassed repeat capture setup.");
    capture.Clear();

    ScopedKeyboardStateOverride keyboardState;
    keyboardState.SetKeyDown(VK_SHIFT, false);
    keyboardState.SetToggle(VK_CAPITAL, false);
    keyboardState.Apply();

    auto postAndPump = [&](UINT message, WPARAM wParam, LPARAM lParam, const std::string& label) {
        Expect(PostMessageW(window.hwnd(), message, wParam, lParam) != FALSE, label + " should post successfully.");
        Expect(window.PumpMessages(), label + " should keep the window open while dispatching messages.");
    };

    postAndPump(WM_KEYDOWN, 'A', BuildTestKeyboardMessageLParam('A', true), "Initial rebound A keydown");
    Expect(capture.messages.size() == 2, "Expected the initial rebound A keydown to forward the remapped WM_KEYDOWN and WM_CHAR.");
    ExpectCapturedMessage(capture, 0, WM_KEYDOWN, 'B', "Initial rebound WM_KEYDOWN");
    ExpectCapturedMessage(capture, 1, WM_CHAR, 'b', "Initial rebound WM_CHAR");

    capture.Clear();
    LPARAM physicalRepeatLParam = BuildTestKeyboardMessageLParam('A', true);
    physicalRepeatLParam |= (static_cast<LPARAM>(1) << 30);
    postAndPump(WM_KEYDOWN, 'A', physicalRepeatLParam, "Physical repeat A keydown");
    Expect(capture.messages.empty(), "Expected physical auto-repeat WM_KEYDOWN/WM_CHAR to be suppressed when local repeat is enabled.");

    capture.Clear();
    postAndPump(WM_TOOLSCREEN_LOCAL_KEY_REPEAT, 0, 0, "Local repeat tick with rebound key");
    Expect(capture.messages.size() == 2, "Expected the local repeat tick to flow through the rebind pipeline.");
    ExpectCapturedMessage(capture, 0, WM_KEYDOWN, 'B', "Repeated rebound WM_KEYDOWN");
    ExpectCapturedMessage(capture, 1, WM_CHAR, 'b', "Repeated rebound WM_CHAR");

    capture.Clear();
    postAndPump(WM_KEYUP, 'A', BuildTestKeyboardMessageLParam('A', false), "Rebound A keyup");
    Expect(capture.messages.size() == 1, "Expected releasing the rebound A key to forward one remapped WM_KEYUP.");
    ExpectCapturedMessage(capture, 0, WM_KEYUP, 'B', "Rebound WM_KEYUP");
}

void RunKeyRepeatRuntimeAutohotkeyDuplicateKeydownTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    PrepareRebindRuntimeCase("key_repeat_runtime_autohotkey_duplicate_keydown", {});
    g_config.useSystemKeyRepeat = false;
    PublishConfigSnapshot();
    Expect(window.PumpMessages(), "Expected the test window to stay open before AutoHotkey duplicate repeat capture setup.");
    ScopedSubclassedInputCapture capture(window.hwnd());
    Expect(window.PumpMessages(), "Expected the test window to stay open after AutoHotkey duplicate repeat capture setup.");
    capture.Clear();

    ScopedKeyboardStateOverride keyboardState;
    keyboardState.SetKeyDown(VK_SHIFT, false);
    keyboardState.SetToggle(VK_CAPITAL, false);
    keyboardState.Apply();

    auto postAndPump = [&](UINT message, WPARAM wParam, LPARAM lParam, const std::string& label) {
        Expect(PostMessageW(window.hwnd(), message, wParam, lParam) != FALSE, label + " should post successfully.");
        Expect(window.PumpMessages(), label + " should keep the window open while dispatching messages.");
    };

    const LPARAM initialKeyDownLParam = BuildTestKeyboardMessageLParam('B', true);
    postAndPump(WM_KEYDOWN, 'B', initialKeyDownLParam, "Initial remapped B keydown");
    Expect(capture.messages.size() == 2, "Expected the initial remapped B keydown to forward WM_KEYDOWN and WM_CHAR.");
    ExpectCapturedMessage(capture, 0, WM_KEYDOWN, 'B', "Initial remapped B WM_KEYDOWN");
    ExpectCapturedMessage(capture, 1, WM_CHAR, 'b', "Initial remapped B WM_CHAR");

    capture.Clear();
    postAndPump(WM_KEYDOWN, 'B', initialKeyDownLParam, "AutoHotkey-style duplicate B keydown");
    Expect(capture.messages.empty(),
           "Expected duplicate held-key WM_KEYDOWN/WM_CHAR without bit 30 to be suppressed when local repeat is enabled.");

    capture.Clear();
    postAndPump(WM_TOOLSCREEN_LOCAL_KEY_REPEAT, 0, 0, "Local repeat tick after duplicate remapped keydown");
    Expect(capture.messages.size() == 2, "Expected local repeat to continue after an AutoHotkey-style duplicate held-key keydown.");
    ExpectCapturedMessage(capture, 0, WM_KEYDOWN, 'B', "Repeated remapped B WM_KEYDOWN");
    ExpectCapturedMessage(capture, 1, WM_CHAR, 'b', "Repeated remapped B WM_CHAR");

    capture.Clear();
    postAndPump(WM_KEYUP, 'B', BuildTestKeyboardMessageLParam('B', false), "Remapped B keyup");
    Expect(capture.messages.size() == 1, "Expected releasing the remapped B key to forward one WM_KEYUP.");
    ExpectCapturedMessage(capture, 0, WM_KEYUP, 'B', "Remapped B WM_KEYUP");

    capture.Clear();
    postAndPump(WM_KEYDOWN, 'B', initialKeyDownLParam, "Fresh remapped B keydown after release");
    Expect(capture.messages.size() == 2, "Expected a fresh remapped B keydown after release to forward WM_KEYDOWN and WM_CHAR.");
    ExpectCapturedMessage(capture, 0, WM_KEYDOWN, 'B', "Fresh remapped B WM_KEYDOWN");
    ExpectCapturedMessage(capture, 1, WM_CHAR, 'b', "Fresh remapped B WM_CHAR");

    capture.Clear();
    postAndPump(WM_KEYUP, 'B', BuildTestKeyboardMessageLParam('B', false), "Final remapped B keyup");
}

void RunKeyRepeatRuntimeAutohotkeyRetainedNumpadScanTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    PrepareRebindRuntimeCase("key_repeat_runtime_autohotkey_retained_numpad_scan", {});
    g_config.useSystemKeyRepeat = false;
    PublishConfigSnapshot();
    Expect(window.PumpMessages(), "Expected the test window to stay open before retained-scan repeat capture setup.");
    ScopedSubclassedInputCapture capture(window.hwnd());
    Expect(window.PumpMessages(), "Expected the test window to stay open after retained-scan repeat capture setup.");
    capture.Clear();

    ScopedKeyboardStateOverride keyboardState;
    keyboardState.SetKeyDown(VK_SHIFT, false);
    keyboardState.SetToggle(VK_CAPITAL, false);
    keyboardState.Apply();

    auto postAndPump = [&](UINT message, WPARAM wParam, LPARAM lParam, const std::string& label) {
        Expect(PostMessageW(window.hwnd(), message, wParam, lParam) != FALSE, label + " should post successfully.");
        Expect(window.PumpMessages(), label + " should keep the window open while dispatching messages.");
    };

    const UINT numpadClearScanCodeWithFlags = 0x4C;
    const LPARAM retainedScanKeyDownLParam = BuildTestKeyboardMessageLParamFromScan(numpadClearScanCodeWithFlags, true);
    postAndPump(WM_KEYDOWN, '5', retainedScanKeyDownLParam, "Initial remapped 5 keydown with retained numpad-clear scan");

    capture.Clear();
    postAndPump(WM_TOOLSCREEN_LOCAL_KEY_REPEAT, 0, 0, "Local repeat tick after retained numpad-clear scan");
    Expect(capture.messages.size() == 2,
           "Expected local repeat to normalize a retained numpad-clear scan so the remapped digit keeps producing WM_KEYDOWN and WM_CHAR.");
    ExpectCapturedMessage(capture, 0, WM_KEYDOWN, '5', "Repeated retained-scan WM_KEYDOWN");
    ExpectCapturedMessage(capture, 1, WM_CHAR, '5', "Repeated retained-scan WM_CHAR");

    capture.Clear();
    postAndPump(WM_KEYUP, '5', BuildTestKeyboardMessageLParamFromScan(numpadClearScanCodeWithFlags, false), "Retained-scan remapped 5 keyup");
}

void RunKeyRepeatRuntimeAutohotkeyNumpadClearCharOverrideTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    PrepareRebindRuntimeCase("key_repeat_runtime_autohotkey_numpadclear_char_override", {});
    g_config.useSystemKeyRepeat = false;
    PublishConfigSnapshot();
    Expect(window.PumpMessages(), "Expected the test window to stay open before numpad-clear char override capture setup.");
    ScopedSubclassedInputCapture capture(window.hwnd());
    Expect(window.PumpMessages(), "Expected the test window to stay open after numpad-clear char override capture setup.");
    capture.Clear();

    ScopedKeyboardStateOverride keyboardState;
    keyboardState.SetKeyDown(VK_SHIFT, false);
    keyboardState.SetToggle(VK_CAPITAL, false);
    keyboardState.Apply();

    auto postAndPump = [&](UINT message, WPARAM wParam, LPARAM lParam, const std::string& label) {
        Expect(PostMessageW(window.hwnd(), message, wParam, lParam) != FALSE, label + " should post successfully.");
        Expect(window.PumpMessages(), label + " should keep the window open while dispatching messages.");
    };

    const UINT numpadClearScanCodeWithFlags = 0x4C;
    const LPARAM numpadClearKeyDownLParam = BuildTestKeyboardMessageLParamFromScan(numpadClearScanCodeWithFlags, true);
    postAndPump(WM_KEYDOWN, VK_CLEAR, numpadClearKeyDownLParam, "Initial NumpadClear source keydown");

    capture.Clear();
    postAndPump(WM_CHAR, '5', numpadClearKeyDownLParam, "Remapped digit 5 char after NumpadClear keydown");
    Expect(capture.messages.size() == 1, "Expected the remapped digit char to forward once after the source NumpadClear keydown.");
    ExpectCapturedMessage(capture, 0, WM_CHAR, '5', "Initial remapped digit WM_CHAR");

    capture.Clear();
    postAndPump(WM_TOOLSCREEN_LOCAL_KEY_REPEAT, 0, 0, "Local repeat tick after NumpadClear char override");
    Expect(capture.messages.size() == 2,
           "Expected local repeat to reuse the observed remapped digit after a non-typing NumpadClear source keydown.");
    ExpectCapturedMessage(capture, 0, WM_KEYDOWN, '5', "Repeated char-override WM_KEYDOWN");
    ExpectCapturedMessage(capture, 1, WM_CHAR, '5', "Repeated char-override WM_CHAR");

    capture.Clear();
    postAndPump(WM_KEYUP, '5', BuildTestKeyboardMessageLParam('5', false), "Remapped digit 5 keyup after NumpadClear source");
    Expect(capture.messages.size() == 1, "Expected the remapped destination keyup to forward once after the NumpadClear source release.");
    ExpectCapturedMessage(capture, 0, WM_KEYUP, '5', "Remapped destination WM_KEYUP");

    capture.Clear();
    postAndPump(WM_TOOLSCREEN_LOCAL_KEY_REPEAT, 0, 0, "Local repeat tick after remapped destination keyup");
    Expect(capture.messages.empty(), "Expected repeat to stop after the remapped destination keyup releases the held NumpadClear source.");
}

    void RunKeyRepeatRuntimeAutohotkeyTypingThenClearAliasTest(TestRunMode runMode = TestRunMode::Automated) {
        DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
        PrepareRebindRuntimeCase("key_repeat_runtime_autohotkey_typing_then_clear_alias", {});
        g_config.useSystemKeyRepeat = false;
        PublishConfigSnapshot();
        Expect(window.PumpMessages(), "Expected the test window to stay open before typing-then-clear alias capture setup.");
        ScopedSubclassedInputCapture capture(window.hwnd());
        Expect(window.PumpMessages(), "Expected the test window to stay open after typing-then-clear alias capture setup.");
        capture.Clear();

        ScopedKeyboardStateOverride keyboardState;
        keyboardState.SetKeyDown(VK_SHIFT, false);
        keyboardState.SetToggle(VK_CAPITAL, false);
        keyboardState.Apply();

        const UINT numpadClearScanCodeWithFlags = 0x4C;
        const LPARAM topRowFiveKeyDownLParam = BuildTestKeyboardMessageLParam('5', true);
        const LPARAM numpadClearKeyDownLParam = BuildTestKeyboardMessageLParamFromScan(numpadClearScanCodeWithFlags, true);

        Expect(PostMessageW(window.hwnd(), WM_KEYDOWN, '5', topRowFiveKeyDownLParam) != FALSE,
            "Expected the remapped digit keydown to post successfully.");
        Expect(PostMessageW(window.hwnd(), WM_KEYDOWN, VK_CLEAR, numpadClearKeyDownLParam) != FALSE,
            "Expected the trailing VK_CLEAR alias keydown to post successfully.");
        Expect(window.PumpMessages(), "Expected the typing-then-clear alias sequence to keep the window open while dispatching messages.");

        capture.Clear();
        Expect(PostMessageW(window.hwnd(), WM_TOOLSCREEN_LOCAL_KEY_REPEAT, 0, 0) != FALSE,
            "Expected the local repeat tick to post successfully after the alias sequence.");
        Expect(window.PumpMessages(), "Expected the local repeat tick after the alias sequence to keep the window open.");
        Expect(capture.messages.size() == 2,
            "Expected the typing key to remain the repeat output even after an immediate trailing VK_CLEAR alias keydown.");
        ExpectCapturedMessage(capture, 0, WM_KEYDOWN, '5', "Repeated typing-alias WM_KEYDOWN");
        ExpectCapturedMessage(capture, 1, WM_CHAR, '5', "Repeated typing-alias WM_CHAR");

        capture.Clear();
        Expect(PostMessageW(window.hwnd(), WM_KEYUP, VK_CLEAR, BuildTestKeyboardMessageLParamFromScan(numpadClearScanCodeWithFlags, false)) != FALSE,
            "Expected the VK_CLEAR alias keyup to post successfully.");
        Expect(window.PumpMessages(), "Expected the VK_CLEAR alias keyup to keep the window open while dispatching messages.");

        capture.Clear();
        Expect(PostMessageW(window.hwnd(), WM_TOOLSCREEN_LOCAL_KEY_REPEAT, 0, 0) != FALSE,
            "Expected the local repeat tick after alias release to post successfully.");
        Expect(window.PumpMessages(), "Expected the local repeat tick after alias release to keep the window open.");
        Expect(capture.messages.empty(), "Expected repeat to stop after the trailing VK_CLEAR alias keyup.");
    }

    void RunKeyRepeatRuntimeModifiersInterruptWhenEnabledTest(TestRunMode runMode = TestRunMode::Automated) {
        DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
        PrepareRebindRuntimeCase("key_repeat_runtime_modifiers_interrupt_when_enabled", {});
        g_config.useSystemKeyRepeat = false;
        PublishConfigSnapshot();
        Expect(window.PumpMessages(), "Expected the test window to stay open before modifier interrupt capture setup.");
        ScopedSubclassedInputCapture capture(window.hwnd());
        Expect(window.PumpMessages(), "Expected the test window to stay open after modifier interrupt capture setup.");
        capture.Clear();

        ScopedKeyboardStateOverride keyboardState;
        keyboardState.SetKeyDown(VK_SHIFT, false);
        keyboardState.SetToggle(VK_CAPITAL, false);
        keyboardState.Apply();

        auto postAndPump = [&](UINT message, WPARAM wParam, LPARAM lParam, const std::string& label) {
            Expect(PostMessageW(window.hwnd(), message, wParam, lParam) != FALSE, label + " should post successfully.");
            Expect(window.PumpMessages(), label + " should keep the window open while dispatching messages.");
        };

        postAndPump(WM_KEYDOWN, 'A', BuildTestKeyboardMessageLParam('A', true), "Initial A keydown before interrupting modifier");
        Expect(capture.messages.size() == 2, "Expected the initial A keydown to forward WM_KEYDOWN and WM_CHAR.");

        capture.Clear();
        keyboardState.SetKeyDown(VK_SHIFT, true);
        keyboardState.Apply();
        postAndPump(WM_KEYDOWN, VK_SHIFT, BuildTestKeyboardMessageLParam(VK_SHIFT, true), "Shift keydown while A is held with interruption enabled");
        Expect(capture.messages.size() == 1, "Expected Shift keydown to forward once when modifier interruption is enabled.");
        ExpectCapturedMessage(capture, 0, WM_KEYDOWN, VK_SHIFT, "Modifier WM_KEYDOWN with repeat interruption");

        capture.Clear();
        postAndPump(WM_TOOLSCREEN_LOCAL_KEY_REPEAT, 0, 0, "Local repeat tick after interrupting Shift keydown");
         Expect(capture.messages.empty(),
             "Expected pressing an interrupting modifier to stop the active local key repeat instead of switching repeat ownership to the modifier.");

        capture.Clear();
        keyboardState.SetKeyDown(VK_SHIFT, false);
        keyboardState.Apply();
        postAndPump(WM_KEYUP, VK_SHIFT, BuildTestKeyboardMessageLParam(VK_SHIFT, false), "Shift keyup after interrupting repeat");
        Expect(capture.messages.size() == 1, "Expected releasing Shift to forward one WM_KEYUP.");
        ExpectCapturedMessage(capture, 0, WM_KEYUP, VK_SHIFT, "Modifier WM_KEYUP after interrupting repeat");

        capture.Clear();
        postAndPump(WM_TOOLSCREEN_LOCAL_KEY_REPEAT, 0, 0, "Local repeat tick after releasing interrupting modifier");
        Expect(capture.messages.empty(), "Expected the original A key to stay non-repeating after the interrupting modifier is released.");

        capture.Clear();
        postAndPump(WM_KEYUP, 'A', BuildTestKeyboardMessageLParam('A', false), "A keyup after interrupting repeat");
    }

    void RunKeyRepeatRuntimeModifierReleaseRestartsDelayTest(TestRunMode runMode = TestRunMode::Automated) {
        DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
        PrepareRebindRuntimeCase("key_repeat_runtime_modifier_release_restarts_delay", {});
        g_config.useSystemKeyRepeat = false;
        g_config.keyRepeatStartDelay = 100;
        g_config.keyRepeatDelay = 1;
        PublishConfigSnapshot();
        Expect(window.PumpMessages(), "Expected the test window to stay open before modifier-release repeat capture setup.");
        ScopedSubclassedInputCapture capture(window.hwnd());
        Expect(window.PumpMessages(), "Expected the test window to stay open after modifier-release repeat capture setup.");
        capture.Clear();

        ScopedKeyboardStateOverride keyboardState;
        keyboardState.SetKeyDown(VK_SHIFT, true);
        keyboardState.SetToggle(VK_CAPITAL, false);
        keyboardState.Apply();

        auto postAndPump = [&](UINT message, WPARAM wParam, LPARAM lParam, const std::string& label) {
            Expect(PostMessageW(window.hwnd(), message, wParam, lParam) != FALSE, label + " should post successfully.");
            Expect(window.PumpMessages(), label + " should keep the window open while dispatching messages.");
        };

        int effectiveStartDelayMs = 0;
        int effectiveRepeatDelayMs = 0;
        Expect(GetEffectiveKeyRepeatTimings(effectiveStartDelayMs, effectiveRepeatDelayMs),
               "Expected effective key repeat timings to resolve for the modifier-release restart test.");

        postAndPump(WM_KEYDOWN, 'A', BuildTestKeyboardMessageLParam('A', true), "Initial shifted A keydown");
        Expect(capture.messages.size() == 2, "Expected the initial shifted A keydown to forward WM_KEYDOWN and uppercase WM_CHAR.");
        ExpectCapturedMessage(capture, 0, WM_KEYDOWN, 'A', "Initial shifted A WM_KEYDOWN");
        ExpectCapturedMessage(capture, 1, WM_CHAR, 'A', "Initial shifted A WM_CHAR");

        capture.Clear();
        postAndPump(WM_TOOLSCREEN_LOCAL_KEY_REPEAT, 0, 0, "Local repeat tick while Shift is held");
        Expect(capture.messages.size() == 2, "Expected local repeat to emit shifted output while Shift is held.");
        ExpectCapturedMessage(capture, 0, WM_KEYDOWN, 'A', "Repeated shifted A WM_KEYDOWN");
        ExpectCapturedMessage(capture, 1, WM_CHAR, 'A', "Repeated shifted A WM_CHAR");

        capture.Clear();
        keyboardState.SetKeyDown(VK_SHIFT, false);
        keyboardState.Apply();
        postAndPump(WM_KEYUP, VK_SHIFT, BuildTestKeyboardMessageLParam(VK_SHIFT, false), "Shift keyup while A remains held");
        Expect(capture.messages.size() == 1, "Expected releasing Shift to forward one WM_KEYUP.");
        ExpectCapturedMessage(capture, 0, WM_KEYUP, VK_SHIFT, "Shift WM_KEYUP while A remains held");

        capture.Clear();
        postAndPump(WM_TOOLSCREEN_LOCAL_KEY_REPEAT, 0, 0, "Immediate local repeat tick after Shift keyup");
        Expect(capture.messages.empty(), "Expected modifier release to restart the repeat delay before lowercase repeat resumes.");

        std::this_thread::sleep_for(std::chrono::milliseconds(effectiveStartDelayMs + 20));

        capture.Clear();
        Expect(window.PumpMessages(), "Expected the restarted local repeat timer to keep the window open after Shift keyup.");
        Expect(capture.messages.size() == 2,
               "Expected repeat to resume after the restarted delay once Shift is released; got " +
                   std::to_string(capture.messages.size()) + " captured messages.");
        ExpectCapturedMessage(capture, 0, WM_KEYDOWN, 'A', "Repeated lowercase A WM_KEYDOWN after Shift release");
        ExpectCapturedMessage(capture, 1, WM_CHAR, 'a', "Repeated lowercase A WM_CHAR after Shift release");

        capture.Clear();
        postAndPump(WM_KEYUP, 'A', BuildTestKeyboardMessageLParam('A', false), "A keyup after Shift-release repeat delay restart");
    }

void RunKeyRebindGuiKeyboardLayoutFullBindAndTriggerTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    if (SkipIfNoModernGuiTestGL(window)) { return; }
    PrepareRebindGuiCase("key_rebind_gui_keyboard_layout_full_bind_and_trigger");

    RenderKeyboardInputsFrame(window);
    RenderKeyboardInputsFrame(window);
    RequestGuiTestOpenKeyboardLayout();
    RenderKeyboardInputsFrame(window);
    RequestGuiTestOpenKeyboardLayoutContext('A');
    RenderKeyboardInputsFrame(window);
    RequestGuiTestKeyboardLayoutBeginBind(GuiTestKeyboardLayoutBindTarget::FullOutputVk);
    RenderKeyboardInputsFrame(window);

    SubmitKeyboardBindingEvent('B');
    RenderKeyboardInputsFrame(window);

    Expect(g_config.keyRebinds.rebinds.size() == 1, "Expected the GUI full rebind flow to create exactly one key rebind.");
    const KeyRebind& rebind = g_config.keyRebinds.rebinds.front();
    Expect(rebind.fromKey == 'A', "Expected the GUI full rebind flow to bind from the clicked A key.");
    Expect(rebind.toKey == 'B', "Expected the GUI full rebind flow to bind the trigger output to B.");
    Expect(rebind.useCustomOutput, "Expected the GUI full rebind flow to enable the mirrored output binding state.");
    Expect(rebind.customOutputVK == 'B', "Expected the GUI full rebind flow to store B as the custom output VK.");
    Expect(!rebind.shiftLayerEnabled, "Expected the GUI full rebind flow to leave the shift layer disabled.");

    g_showGui.store(false, std::memory_order_release);
    PublishConfigSnapshot();

    ScopedRebindMessageCapture capture(window.hwnd());
    ScopedKeyboardStateOverride keyboardState;
    keyboardState.SetKeyDown(VK_SHIFT, false);
    keyboardState.SetToggle(VK_CAPITAL, false);
    keyboardState.Apply();

    const LPARAM keyDownLParam = BuildTestKeyboardMessageLParam('A', true);
    const InputHandlerResult keyDownResult = HandleKeyRebinding(window.hwnd(), WM_KEYDOWN, 'A', keyDownLParam);
    Expect(keyDownResult.consumed, "Expected the GUI-created full rebind to consume WM_KEYDOWN.");
    Expect(capture.messages.size() == 1, "Expected the GUI-created full rebind to forward exactly one WM_KEYDOWN message.");
    ExpectCapturedMessage(capture, 0, WM_KEYDOWN, 'B', "GUI full rebind WM_KEYDOWN");

    capture.Clear();
    const InputHandlerResult charResult = HandleCharRebinding(window.hwnd(), WM_CHAR, static_cast<WPARAM>('a'), keyDownLParam);
    Expect(charResult.consumed, "Expected the GUI-created full rebind to consume WM_CHAR.");
    Expect(capture.messages.size() == 1, "Expected the GUI-created full rebind to forward exactly one WM_CHAR message.");
    ExpectCapturedMessage(capture, 0, WM_CHAR, 'b', "GUI full rebind WM_CHAR");
}

void RunKeyRebindGuiKeyboardLayoutSplitBindAndTriggerTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    if (SkipIfNoModernGuiTestGL(window)) { return; }
    PrepareRebindGuiCase("key_rebind_gui_keyboard_layout_split_bind_and_trigger");

    RenderKeyboardInputsFrame(window);
    RenderKeyboardInputsFrame(window);
    RequestGuiTestOpenKeyboardLayout();
    RenderKeyboardInputsFrame(window);
    RequestGuiTestOpenKeyboardLayoutContext('A');
    RenderKeyboardInputsFrame(window);
    RequestGuiTestKeyboardLayoutSetSplitMode(true);
    RenderKeyboardInputsFrame(window);

    RequestGuiTestKeyboardLayoutBeginBind(GuiTestKeyboardLayoutBindTarget::TypesVk);
    RenderKeyboardInputsFrame(window);
    SubmitKeyboardBindingEvent('C');
    RenderKeyboardInputsFrame(window);

    RequestGuiTestKeyboardLayoutBeginBind(GuiTestKeyboardLayoutBindTarget::TypesVkShift);
    RenderKeyboardInputsFrame(window);
    SubmitKeyboardBindingEvent('D');
    RenderKeyboardInputsFrame(window);

    RequestGuiTestKeyboardLayoutSetShiftLayerUppercase(true);
    RenderKeyboardInputsFrame(window);
    RequestGuiTestKeyboardLayoutSetShiftLayerUsesCapsLock(true);
    RenderKeyboardInputsFrame(window);
    RequestGuiTestKeyboardLayoutBeginBind(GuiTestKeyboardLayoutBindTarget::TriggersVk);
    RenderKeyboardInputsFrame(window);
    SubmitKeyboardBindingEvent('B');
    RenderKeyboardInputsFrame(window);

    Expect(g_config.keyRebinds.rebinds.size() == 1, "Expected the GUI split rebind flow to create exactly one key rebind.");
    const KeyRebind& rebind = g_config.keyRebinds.rebinds.front();
    Expect(rebind.fromKey == 'A', "Expected the GUI split rebind flow to bind from the clicked A key.");
    Expect(rebind.toKey == 'B', "Expected the GUI split rebind flow to bind B as the trigger key.");
    Expect(rebind.useCustomOutput, "Expected the GUI split rebind flow to enable a custom base output.");
    Expect(rebind.customOutputVK == 'C', "Expected the GUI split rebind flow to store C as the base output VK.");
    Expect(rebind.shiftLayerEnabled, "Expected the GUI split rebind flow to enable the shift layer.");
    Expect(rebind.shiftLayerOutputVK == 'D', "Expected the GUI split rebind flow to store D as the shift-layer output VK.");
    Expect(rebind.shiftLayerOutputShifted, "Expected the GUI split rebind flow to store the shift-layer uppercase toggle.");
    Expect(rebind.shiftLayerUsesCapsLock, "Expected the GUI split rebind flow to store the Caps Lock shift-layer toggle.");

    g_showGui.store(false, std::memory_order_release);
    PublishConfigSnapshot();

    ScopedRebindMessageCapture capture(window.hwnd());
    ScopedKeyboardStateOverride keyboardState;
    keyboardState.SetKeyDown(VK_SHIFT, false);
    keyboardState.SetToggle(VK_CAPITAL, true);
    keyboardState.Apply();

    const LPARAM keyDownLParam = BuildTestKeyboardMessageLParam('A', true);
    const InputHandlerResult keyDownResult = HandleKeyRebinding(window.hwnd(), WM_KEYDOWN, 'A', keyDownLParam);
    Expect(keyDownResult.consumed, "Expected the GUI-created split rebind to consume WM_KEYDOWN.");
    Expect(capture.messages.size() == 1, "Expected the GUI-created split rebind to forward exactly one WM_KEYDOWN message.");
    ExpectCapturedMessage(capture, 0, WM_KEYDOWN, 'B', "GUI split rebind WM_KEYDOWN");

    capture.Clear();
    const InputHandlerResult charResult = HandleCharRebinding(window.hwnd(), WM_CHAR, static_cast<WPARAM>('A'), keyDownLParam);
    Expect(charResult.consumed, "Expected the GUI-created split rebind to consume WM_CHAR.");
    Expect(capture.messages.size() == 1, "Expected the GUI-created split rebind to forward exactly one WM_CHAR message.");
    ExpectCapturedMessage(capture, 0, WM_CHAR, 'D', "GUI split rebind WM_CHAR");
}

    void RunKeyRebindGuiTextOverridePickRejectsNonTypableKeyTest(TestRunMode runMode = TestRunMode::Automated) {
        DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
        if (SkipIfNoModernGuiTestGL(window)) { return; }

        KeyRebind rebind = MakeEnabledRebind('A', 'B');
        PrepareRebindGuiCase("key_rebind_gui_text_override_pick_rejects_non_typable_key", { rebind });

        RenderKeyboardInputsFrame(window);
        RenderKeyboardInputsFrame(window);

        RequestGuiTestOpenRebindTextOverrideBind(0);
        RenderKeyboardInputsFrame(window);

        SubmitKeyboardBindingEvent(VK_HOME);
        RenderKeyboardInputsFrame(window);

        Expect(g_config.keyRebinds.rebinds.size() == 1,
            "Expected the text-override Pick flow to keep the original rebind after rejecting Home.");
        const KeyRebind& rejectedRebind = g_config.keyRebinds.rebinds.front();
        Expect(rejectedRebind.fromKey == 'A', "Expected the rejected text-override Pick flow to keep the source key unchanged.");
        Expect(rejectedRebind.toKey == 'B', "Expected the rejected text-override Pick flow to keep the trigger key unchanged.");
        Expect(rejectedRebind.customOutputVK == 0,
            "Expected the text-override Pick flow to reject Home instead of storing it as a typed-output VK.");
        Expect(!rejectedRebind.useCustomOutput,
            "Expected the text-override Pick flow to leave custom output disabled after rejecting Home.");

        SubmitKeyboardBindingEvent('C');
        RenderKeyboardInputsFrame(window);

        Expect(g_config.keyRebinds.rebinds.size() == 1,
            "Expected the text-override Pick flow to keep a single rebind after accepting a valid typed-output key.");
        const KeyRebind& acceptedRebind = g_config.keyRebinds.rebinds.front();
        Expect(acceptedRebind.useCustomOutput,
            "Expected the text-override Pick flow to remain active after rejecting Home and then accepting C.");
        Expect(acceptedRebind.customOutputVK == 'C',
            "Expected the text-override Pick flow to keep waiting and accept C after rejecting Home.");
    }

    void RunKeyRebindGuiKeyboardLayoutDisabledOutputTest(TestRunMode runMode = TestRunMode::Automated) {
        DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
        if (SkipIfNoModernGuiTestGL(window)) { return; }
        PrepareRebindGuiCase("key_rebind_gui_keyboard_layout_disabled_output");

        OpenKeyboardLayoutContext(window, 'A');
        ResetGuiTestInteractionRects();
        RenderKeyboardInputsFrame(window);

        GuiTestInteractionRect popupRect;
        Expect(GetGuiTestInteractionRect("inputs.keyboard_layout.popup.full_rebind", popupRect),
            "Expected the keyboard-layout popup to expose the full rebind option.");
        Expect(GetGuiTestInteractionRect("inputs.keyboard_layout.popup.split_rebind", popupRect),
            "Expected the keyboard-layout popup to expose the split rebind option.");
        Expect(GetGuiTestInteractionRect("inputs.keyboard_layout.popup.output_disabled", popupRect),
            "Expected the keyboard-layout popup to expose the disabled output option.");

        RequestGuiTestKeyboardLayoutSetOutputDisabled(true);
        RenderKeyboardInputsFrame(window);
        RenderKeyboardInputsFrame(window);

        ResetGuiTestInteractionRects();
        RenderKeyboardInputsFrame(window);

        Expect(!GetGuiTestInteractionRect("inputs.keyboard_layout.popup.output", popupRect),
            "Expected the keyboard-layout Disable key mode to hide the full output binding row.");
        Expect(!GetGuiTestInteractionRect("inputs.keyboard_layout.popup.types", popupRect),
            "Expected the keyboard-layout Disable key mode to hide the types binding row.");
        Expect(!GetGuiTestInteractionRect("inputs.keyboard_layout.popup.types_shift", popupRect),
            "Expected the keyboard-layout Disable key mode to hide the Shift-layer types binding row.");
        Expect(!GetGuiTestInteractionRect("inputs.keyboard_layout.popup.triggers", popupRect),
            "Expected the keyboard-layout Disable key mode to hide the triggers binding row.");
        Expect(!GetGuiTestInteractionRect("inputs.keyboard_layout.popup.custom_input", popupRect),
            "Expected the keyboard-layout Disable key mode to hide the custom-input row.");

        Expect(g_config.keyRebinds.rebinds.size() == 1,
            "Expected the keyboard-layout disabled-output flow to create exactly one key rebind.");
        const KeyRebind& rebind = g_config.keyRebinds.rebinds.front();
        Expect(rebind.fromKey == 'A', "Expected the keyboard-layout disabled-output flow to bind from A.");
        Expect(rebind.toKey == 0, "Expected the keyboard-layout disabled-output flow to store a consume-only output.");
        Expect(rebind.enabled, "Expected the keyboard-layout disabled-output flow to keep the consume-only output active.");

        const GuiTestKeyboardLayoutKeyLabels labels =
         ExpectKeyboardLayoutKeyLabels('A', "Expected disabled-output keyboard-layout labels for A.");
        Expect(labels.primaryText == trc("label.none"),
            "Expected the disabled-output keyboard-layout key to render None as the primary label.");
        Expect(labels.secondaryText.empty(),
            "Expected the disabled-output keyboard-layout key to hide the trigger label.");

        g_showGui.store(false, std::memory_order_release);
        PublishConfigSnapshot();

        ScopedRebindMessageCapture capture(window.hwnd());
        const LPARAM keyDownLParam = BuildTestKeyboardMessageLParam('A', true);
        const InputHandlerResult keyDownResult = HandleKeyRebinding(window.hwnd(), WM_KEYDOWN, 'A', keyDownLParam);
        Expect(keyDownResult.consumed, "Expected the GUI-created disabled-output rebind to consume WM_KEYDOWN.");
        Expect(capture.messages.empty(), "Expected the GUI-created disabled-output rebind to avoid forwarding WM_KEYDOWN.");

        const InputHandlerResult charResult = HandleCharRebinding(window.hwnd(), WM_CHAR, static_cast<WPARAM>('a'), keyDownLParam);
        Expect(charResult.consumed, "Expected the GUI-created disabled-output rebind to consume WM_CHAR.");
        Expect(capture.messages.empty(), "Expected the GUI-created disabled-output rebind to avoid forwarding WM_CHAR.");

        g_showGui.store(true, std::memory_order_release);
        RequestGuiTestOpenKeyboardLayoutContext('A');
        RenderKeyboardInputsFrame(window);
        RequestGuiTestKeyboardLayoutSetOutputDisabled(false);
        RenderKeyboardInputsFrame(window);

        Expect(g_config.keyRebinds.rebinds.empty(),
            "Expected clearing the keyboard-layout disabled-output option to remove the consume-only override.");
    }

        void RunKeyRebindGuiKeyboardLayoutSplitDisabledTargetsTest(TestRunMode runMode = TestRunMode::Automated) {
            DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
            if (SkipIfNoModernGuiTestGL(window)) { return; }
            PrepareRebindGuiCase("key_rebind_gui_keyboard_layout_split_disabled_targets");

            OpenKeyboardLayoutContext(window, 'A');
            RequestGuiTestKeyboardLayoutSetSplitMode(true);
            ResetGuiTestInteractionRects();
            RenderKeyboardInputsFrame(window);

            GuiTestInteractionRect popupRect;
            Expect(GetGuiTestInteractionRect("inputs.keyboard_layout.popup.types_disabled", popupRect),
                "Expected the keyboard-layout split popup to expose the disabled toggle for Types.");
            Expect(GetGuiTestInteractionRect("inputs.keyboard_layout.popup.types_shift_disabled", popupRect),
                "Expected the keyboard-layout split popup to expose the disabled toggle for Types (Shift).");
            Expect(GetGuiTestInteractionRect("inputs.keyboard_layout.popup.triggers_disabled", popupRect),
                "Expected the keyboard-layout split popup to expose the disabled toggle for Triggers.");

            RequestGuiTestKeyboardLayoutSetDisabledTarget(GuiTestKeyboardLayoutDisableTarget::Types, true);
            RenderKeyboardInputsFrame(window);
            RenderKeyboardInputsFrame(window);

            Expect(g_config.keyRebinds.rebinds.size() == 1,
                "Expected the keyboard-layout split disabled-target flow to create exactly one rebind.");
            const KeyRebind& typesDisabledRebind = g_config.keyRebinds.rebinds.front();
            Expect(typesDisabledRebind.baseOutputDisabled,
                "Expected the keyboard-layout split disabled-target flow to mark Types as disabled.");
            GuiTestKeyboardLayoutKeyLabels labels =
             ExpectKeyboardLayoutKeyLabels('A', "Expected split disabled-target keyboard-layout labels after disabling Types.");
            Expect(labels.primaryText == trc("label.none"),
                "Expected the split disabled-target keyboard-layout key to render None as the primary label when Types is disabled.");

            RequestGuiTestKeyboardLayoutSetDisabledTarget(GuiTestKeyboardLayoutDisableTarget::TypesVkShift, true);
            RenderKeyboardInputsFrame(window);
            RenderKeyboardInputsFrame(window);

            const KeyRebind& shiftDisabledRebind = g_config.keyRebinds.rebinds.front();
            Expect(shiftDisabledRebind.shiftLayerEnabled,
                "Expected the keyboard-layout split disabled-target flow to keep the Shift layer enabled when disabling Types (Shift).");
            Expect(shiftDisabledRebind.shiftLayerOutputDisabled,
                "Expected the keyboard-layout split disabled-target flow to mark Types (Shift) as disabled.");
            labels = ExpectKeyboardLayoutKeyLabels('A', "Expected split disabled-target keyboard-layout labels after disabling Types (Shift).");
                Expect(labels.shiftLayerText == trc("label.none"),
                    "Expected the split disabled-target keyboard-layout key to render None for the Shift-layer label.");

                RequestGuiTestKeyboardLayoutSetShiftLayerUsesCapsLock(true);
                RenderKeyboardInputsFrame(window);
                RenderKeyboardInputsFrame(window);

                const KeyRebind& shiftDisabledAfterCapsLockRebind = g_config.keyRebinds.rebinds.front();
                Expect(shiftDisabledAfterCapsLockRebind.shiftLayerOutputDisabled,
                    "Expected toggling the Caps Lock shift-layer checkbox to preserve a disabled Types (Shift) state.");
                labels = ExpectKeyboardLayoutKeyLabels('A', "Expected split disabled-target keyboard-layout labels after toggling Caps Lock activation.");
                Expect(labels.shiftLayerText == trc("label.none"),
                    "Expected the split disabled-target keyboard-layout key to keep rendering None for the Shift-layer label after toggling Caps Lock activation.");

            RequestGuiTestKeyboardLayoutSetDisabledTarget(GuiTestKeyboardLayoutDisableTarget::Triggers, true);
            RenderKeyboardInputsFrame(window);
            RenderKeyboardInputsFrame(window);

            const KeyRebind& triggersDisabledRebind = g_config.keyRebinds.rebinds.front();
            Expect(triggersDisabledRebind.triggerOutputDisabled,
                "Expected the keyboard-layout split disabled-target flow to mark Triggers as disabled.");
            labels = ExpectKeyboardLayoutKeyLabels('A', "Expected split disabled-target keyboard-layout labels after disabling Triggers.");
                Expect(labels.secondaryText == trc("label.none"),
                    "Expected the split disabled-target keyboard-layout key to render None as the trigger label.");
        }

void RunKeyRebindGuiKeyboardLayoutMouseSourceBindAndTriggerTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    if (SkipIfNoModernGuiTestGL(window)) { return; }
    PrepareRebindGuiCase("key_rebind_gui_keyboard_layout_mouse_source_bind_and_trigger");

    RenderKeyboardInputsFrame(window);
    RenderKeyboardInputsFrame(window);
    RequestGuiTestOpenKeyboardLayout();
    RenderKeyboardInputsFrame(window);
    RequestGuiTestOpenKeyboardLayoutContext(VK_LBUTTON);
    RenderKeyboardInputsFrame(window);
    RequestGuiTestKeyboardLayoutBeginBind(GuiTestKeyboardLayoutBindTarget::FullOutputVk);
    RenderKeyboardInputsFrame(window);

    SubmitKeyboardBindingEvent('Q');
    RenderKeyboardInputsFrame(window);

    Expect(g_config.keyRebinds.rebinds.size() == 1, "Expected the GUI mouse-source rebind flow to create exactly one key rebind.");
    const KeyRebind& rebind = g_config.keyRebinds.rebinds.front();
    Expect(rebind.fromKey == VK_LBUTTON, "Expected the GUI mouse-source rebind flow to bind from MB1.");
    Expect(rebind.toKey == 'Q', "Expected the GUI mouse-source rebind flow to bind Q as the target key.");
    Expect(rebind.useCustomOutput, "Expected the GUI mouse-source rebind flow to enable mirrored output binding state.");
    Expect(rebind.customOutputVK == 'Q', "Expected the GUI mouse-source rebind flow to store Q as the custom output VK.");

    g_showGui.store(false, std::memory_order_release);
    PublishConfigSnapshot();

    ScopedRebindMessageCapture capture(window.hwnd());
    ScopedKeyboardStateOverride keyboardState;
    keyboardState.SetKeyDown(VK_SHIFT, false);
    keyboardState.SetToggle(VK_CAPITAL, false);
    keyboardState.Apply();

    const InputHandlerResult mouseDownResult = HandleKeyRebinding(window.hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(12, 34));
    Expect(mouseDownResult.consumed, "Expected the GUI-created mouse-source rebind to consume WM_LBUTTONDOWN.");
    Expect(capture.messages.size() == 2,
           "Expected the GUI-created mouse-source rebind to forward both WM_KEYDOWN and WM_CHAR for the target key.");
    ExpectCapturedMessage(capture, 0, WM_KEYDOWN, 'Q', "GUI mouse-source rebind WM_KEYDOWN");
    ExpectCapturedMessage(capture, 1, WM_CHAR, 'q', "GUI mouse-source rebind WM_CHAR");

    capture.Clear();
    const InputHandlerResult mouseUpResult = HandleKeyRebinding(window.hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(12, 34));
    Expect(mouseUpResult.consumed, "Expected the GUI-created mouse-source rebind to consume WM_LBUTTONUP.");
    Expect(capture.messages.size() == 1, "Expected the GUI-created mouse-source rebind to forward exactly one WM_KEYUP message.");
    ExpectCapturedMessage(capture, 0, WM_KEYUP, 'Q', "GUI mouse-source rebind WM_KEYUP");
}

void RunKeyRebindGuiKeyboardLayoutMouseTriggerLabelMappingTest(TestRunMode runMode = TestRunMode::Automated) {
    RunKeyboardLayoutTriggerLabelMappingCase("key_rebind_gui_keyboard_layout_mouse_trigger_label_mapping",
                                             VK_LBUTTON, 0x0001, "MB1", runMode);
}

void RunKeyRebindGuiKeyboardLayoutXButtonTriggerLabelMappingTest(TestRunMode runMode = TestRunMode::Automated) {
    RunKeyboardLayoutTriggerLabelMappingCase("key_rebind_gui_keyboard_layout_xbutton_trigger_label_mapping",
                                             VK_XBUTTON1, 0x0001, "MB4", runMode);
}

void RunKeyRebindGuiKeyboardLayoutScrollTriggerLabelMappingTest(TestRunMode runMode = TestRunMode::Automated) {
    RunKeyboardLayoutTriggerLabelMappingCase("key_rebind_gui_keyboard_layout_scroll_trigger_label_mapping",
                                             VK_TOOLSCREEN_SCROLL_UP, 0x0001, "SCROLL UP", runMode);
}

void RunKeyRebindGuiKeyboardLayoutScrollSourcePopupOptionsTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    if (SkipIfNoModernGuiTestGL(window)) { return; }
    PrepareRebindGuiCase("key_rebind_gui_keyboard_layout_scroll_source_popup_options");

    OpenKeyboardLayoutContext(window, VK_TOOLSCREEN_SCROLL_UP);
    ResetGuiTestInteractionRects();
    RenderKeyboardInputsFrame(window);

    GuiTestInteractionRect popupRect;
    Expect(GetGuiTestInteractionRect("inputs.keyboard_layout.popup.scroll_enabled", popupRect),
        "Expected the scroll-wheel keyboard-layout popup to expose the default scroll option.");
    Expect(GetGuiTestInteractionRect("inputs.keyboard_layout.popup.scroll_disabled", popupRect),
        "Expected the scroll-wheel keyboard-layout popup to expose the disabled option.");
    Expect(!GetGuiTestInteractionRect("inputs.keyboard_layout.popup.full_rebind", popupRect),
        "Expected the scroll-wheel keyboard-layout popup to hide the full rebind option.");
    Expect(!GetGuiTestInteractionRect("inputs.keyboard_layout.popup.split_rebind", popupRect),
        "Expected the scroll-wheel keyboard-layout popup to hide the split rebind option.");
    Expect(!GetGuiTestInteractionRect("inputs.keyboard_layout.popup.output", popupRect),
        "Expected the scroll-wheel keyboard-layout popup to hide the output binding row.");
    Expect(!GetGuiTestInteractionRect("inputs.keyboard_layout.popup.types", popupRect),
        "Expected the scroll-wheel keyboard-layout popup to hide the types binding row.");
    Expect(!GetGuiTestInteractionRect("inputs.keyboard_layout.popup.triggers", popupRect),
        "Expected the scroll-wheel keyboard-layout popup to hide the triggers binding row.");

    RequestGuiTestKeyboardLayoutSetScrollWheelEnabled(false);
    RenderKeyboardInputsFrame(window);

    Expect(g_config.keyRebinds.rebinds.size() == 1,
        "Expected disabling the scroll-wheel source in the keyboard-layout popup to create exactly one override rebind.");
    const KeyRebind& disabledRebind = g_config.keyRebinds.rebinds.front();
    Expect(disabledRebind.fromKey == VK_TOOLSCREEN_SCROLL_UP,
        "Expected disabling the scroll-wheel source in the keyboard-layout popup to keep Scroll Up as the source key.");
        Expect(disabledRebind.toKey == 0,
            "Expected disabling the scroll-wheel source in the keyboard-layout popup to store a consume-only override.");
        Expect(disabledRebind.enabled,
            "Expected disabling the scroll-wheel source in the keyboard-layout popup to keep the consume-only override active.");
    Expect(disabledRebind.cursorState == kKeyRebindCursorStateAny,
        "Expected disabling the scroll-wheel source in the keyboard-layout popup to target the active cursor-state view.");

        g_showGui.store(false, std::memory_order_release);
        PublishConfigSnapshot();
        ScopedRebindMessageCapture capture(window.hwnd());
        const InputHandlerResult blockedWheelResult =
         HandleKeyRebinding(window.hwnd(), WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM(320, 240));
        Expect(blockedWheelResult.consumed,
            "Expected disabling the scroll-wheel source in the keyboard-layout popup to consume scroll wheel input.");
        Expect(capture.messages.empty(),
            "Expected disabling the scroll-wheel source in the keyboard-layout popup to avoid forwarding wheel messages.");
        g_showGui.store(true, std::memory_order_release);

    ResetGuiTestInteractionRects();
    RenderKeyboardInputsFrame(window);
        RequestGuiTestKeyboardLayoutSetScrollWheelEnabled(true);
        RenderKeyboardInputsFrame(window);

    Expect(g_config.keyRebinds.rebinds.empty(),
        "Expected re-selecting the default scroll option in the keyboard-layout popup to clear the temporary scroll override.");
}

void RunKeyRebindGuiKeyboardLayoutCursorStateOverrideTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    if (SkipIfNoModernGuiTestGL(window)) { return; }
    PrepareRebindGuiCase("key_rebind_gui_keyboard_layout_cursor_state_override");

    OpenKeyboardLayoutContext(window, 'A');
    BindKeyboardLayoutTarget(window, false, 'B');

    Expect(g_config.keyRebinds.rebinds.size() == 1, "Expected the Any layout bind flow to create exactly one key rebind.");
    Expect(g_config.keyRebinds.rebinds.front().cursorState == kKeyRebindCursorStateAny,
           "Expected the initial keyboard layout bind to target the Any cursor-state layout.");

    RequestGuiTestKeyboardLayoutSetCursorStateView(GuiTestKeyboardLayoutCursorStateView::CursorFree);
    RenderKeyboardInputsFrame(window);
    RequestGuiTestOpenKeyboardLayoutContext('A');
    RenderKeyboardInputsFrame(window);
    RequestGuiTestKeyboardLayoutBeginBind(GuiTestKeyboardLayoutBindTarget::FullOutputVk);
    RenderKeyboardInputsFrame(window);
    SubmitKeyboardBindingEvent('C');
    RenderKeyboardInputsFrame(window);

    Expect(g_config.keyRebinds.rebinds.size() == 2,
           "Expected creating a cursor-free layout override to keep the Any rebind and add a second rebind.");

    const KeyRebind* anyLayoutRebind = nullptr;
    const KeyRebind* cursorFreeLayoutRebind = nullptr;
    for (const KeyRebind& rebind : g_config.keyRebinds.rebinds) {
        if (rebind.fromKey != 'A') continue;
        if (rebind.cursorState == kKeyRebindCursorStateAny) {
            anyLayoutRebind = &rebind;
        } else if (rebind.cursorState == kKeyRebindCursorStateCursorFree) {
            cursorFreeLayoutRebind = &rebind;
        }
    }

    Expect(anyLayoutRebind != nullptr, "Expected the Any keyboard layout rebind to remain after creating a cursor-free override.");
    Expect(cursorFreeLayoutRebind != nullptr,
           "Expected the keyboard layout cursor-free view to create a cursor-free override rebind.");
    Expect(anyLayoutRebind->toKey == 'B' && anyLayoutRebind->customOutputVK == 'B',
           "Expected the Any keyboard layout rebind to preserve its original target.");
    Expect(cursorFreeLayoutRebind->toKey == 'C' && cursorFreeLayoutRebind->customOutputVK == 'C',
           "Expected the cursor-free keyboard layout override to store its own output target.");
}

    void RunKeyRebindGuiKeyboardLayoutAddCustomBindButtonTest(TestRunMode runMode = TestRunMode::Automated) {
        constexpr DWORD kCustomLayoutSourceVk = VK_F13;

        DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
        if (SkipIfNoModernGuiTestGL(window)) { return; }
        PrepareRebindGuiCase("key_rebind_gui_keyboard_layout_add_custom_bind_button");

        RenderKeyboardInputsFrame(window);
        RenderKeyboardInputsFrame(window);
        RequestGuiTestOpenKeyboardLayout();
        RenderKeyboardInputsFrame(window);
            RequestGuiTestKeyboardLayoutBeginAddCustomBind();
            RenderKeyboardInputsFrame(window);
        SubmitKeyboardBindingEvent(kCustomLayoutSourceVk);
        RenderKeyboardInputsFrame(window);

        Expect(g_config.keyRebinds.layoutExtraKeys.size() == 1,
            "Expected the Add Custom Bind flow to store exactly one extra layout source key.");
        Expect(g_config.keyRebinds.layoutExtraKeys.front() == kCustomLayoutSourceVk,
            "Expected the Add Custom Bind flow to store F13 as the extra layout source key.");

        const std::string customKeyRectId =
         std::string("inputs.keyboard_layout.custom_key.") + std::to_string(static_cast<unsigned>(kCustomLayoutSourceVk));
            (void)ExpectGuiInteractionRect(customKeyRectId.c_str(),
                            "Expected the custom layout key button to be rendered below the keyboard layout.");
            RequestGuiTestOpenKeyboardLayoutContext(kCustomLayoutSourceVk);
            RenderKeyboardInputsFrame(window);

        RequestGuiTestKeyboardLayoutBeginBind(GuiTestKeyboardLayoutBindTarget::FullOutputVk);
        RenderKeyboardInputsFrame(window);
        SubmitKeyboardBindingEvent('B');
        RenderKeyboardInputsFrame(window);

        Expect(g_config.keyRebinds.rebinds.size() == 1,
            "Expected configuring the custom layout key to create exactly one key rebind.");
        const KeyRebind& rebind = g_config.keyRebinds.rebinds.front();
        Expect(rebind.fromKey == kCustomLayoutSourceVk,
            "Expected the custom layout key rebind to bind from F13.");
        Expect(rebind.toKey == 'B', "Expected the custom layout key rebind to bind B as the target key.");
        Expect(rebind.useCustomOutput,
            "Expected the custom layout key full-bind flow to enable mirrored output binding state.");
        Expect(rebind.customOutputVK == 'B',
            "Expected the custom layout key full-bind flow to store B as the custom output VK.");
    }

    void RunKeyRebindGuiKeyboardLayoutRemoveCustomBindButtonTest(TestRunMode runMode = TestRunMode::Automated) {
        constexpr DWORD kCustomLayoutSourceVk = VK_F13;

        DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
        if (SkipIfNoModernGuiTestGL(window)) { return; }
        PrepareRebindGuiCase("key_rebind_gui_keyboard_layout_remove_custom_bind_button");

        RenderKeyboardInputsFrame(window);
        RenderKeyboardInputsFrame(window);
        RequestGuiTestOpenKeyboardLayout();
        RenderKeyboardInputsFrame(window);
        RequestGuiTestKeyboardLayoutBeginAddCustomBind();
        RenderKeyboardInputsFrame(window);
        SubmitKeyboardBindingEvent(kCustomLayoutSourceVk);
        RenderKeyboardInputsFrame(window);

        RequestGuiTestOpenKeyboardLayoutContext(kCustomLayoutSourceVk);
        RenderKeyboardInputsFrame(window);
        RequestGuiTestKeyboardLayoutBeginBind(GuiTestKeyboardLayoutBindTarget::FullOutputVk);
        RenderKeyboardInputsFrame(window);
        SubmitKeyboardBindingEvent('B');
        RenderKeyboardInputsFrame(window);

        Expect(g_config.keyRebinds.layoutExtraKeys.size() == 1,
               "Expected the remove custom bind fixture to start with one extra layout source key.");
        Expect(g_config.keyRebinds.rebinds.size() == 1,
               "Expected the remove custom bind fixture to start with one configured rebind.");

        RequestGuiTestKeyboardLayoutRemoveCustomKey(kCustomLayoutSourceVk);
        RenderKeyboardInputsFrame(window);
        RequestGuiTestKeyboardLayoutConfirmRemoveCustomKey();
        RenderKeyboardInputsFrame(window);

        Expect(g_config.keyRebinds.layoutExtraKeys.empty(),
               "Expected removing a custom layout key to clear the stored extra layout keys.");
        Expect(g_config.keyRebinds.rebinds.empty(),
               "Expected removing a custom layout key to also delete its saved rebinds.");

        ResetGuiTestInteractionRects();
        RenderKeyboardInputsFrame(window);
        GuiTestInteractionRect removedKeyRect;
        const std::string customKeyRectId =
            std::string("inputs.keyboard_layout.custom_key.") + std::to_string(static_cast<unsigned>(kCustomLayoutSourceVk));
        Expect(!GetGuiTestInteractionRect(customKeyRectId.c_str(), removedKeyRect),
               "Expected the removed custom layout key button to disappear from the keyboard layout.");
    }

        void RunKeyRebindGuiKeyboardLayoutAddBuiltInCustomBindButtonTest(TestRunMode runMode = TestRunMode::Automated) {
            constexpr DWORD kBuiltInLayoutSourceVk = VK_F1;

            DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
            if (SkipIfNoModernGuiTestGL(window)) { return; }
            PrepareRebindGuiCase("key_rebind_gui_keyboard_layout_add_built_in_custom_bind_button");

            RenderKeyboardInputsFrame(window);
            RenderKeyboardInputsFrame(window);
            RequestGuiTestOpenKeyboardLayout();
            RenderKeyboardInputsFrame(window);
            RequestGuiTestKeyboardLayoutBeginAddCustomBind();
            RenderKeyboardInputsFrame(window);
            SubmitKeyboardBindingEvent(kBuiltInLayoutSourceVk);
            RenderKeyboardInputsFrame(window);

            Expect(g_config.keyRebinds.layoutExtraKeys.size() == 1,
                "Expected adding a built-in key through Add Custom Bind to store one explicit custom layout key.");
            Expect(g_config.keyRebinds.layoutExtraKeys.front() == kBuiltInLayoutSourceVk,
                "Expected Add Custom Bind to store F1 as an explicit custom layout key.");

            const std::string customKeyRectId =
             std::string("inputs.keyboard_layout.custom_key.") + std::to_string(static_cast<unsigned>(kBuiltInLayoutSourceVk));
            (void)ExpectGuiInteractionRect(customKeyRectId.c_str(),
                            "Expected Add Custom Bind to render a duplicate custom key square for F1.");

            RequestGuiTestOpenKeyboardLayoutContext(kBuiltInLayoutSourceVk);
            RenderKeyboardInputsFrame(window);
            RequestGuiTestKeyboardLayoutBeginBind(GuiTestKeyboardLayoutBindTarget::FullOutputVk);
            RenderKeyboardInputsFrame(window);
            SubmitKeyboardBindingEvent('B');
            RenderKeyboardInputsFrame(window);

            Expect(g_config.keyRebinds.rebinds.size() == 1,
                "Expected configuring the built-in custom key square to create one rebind.");
            Expect(g_config.keyRebinds.rebinds.front().fromKey == kBuiltInLayoutSourceVk,
                "Expected the built-in custom key square to edit the F1 rebind.");

            RequestGuiTestKeyboardLayoutRemoveCustomKey(kBuiltInLayoutSourceVk);
            RenderKeyboardInputsFrame(window);

            Expect(g_config.keyRebinds.layoutExtraKeys.empty(),
                "Expected removing a built-in custom square to clear only the explicit custom layout key entry.");
            Expect(g_config.keyRebinds.rebinds.size() == 1,
                "Expected removing a built-in custom square to preserve the underlying rebind.");

            ResetGuiTestInteractionRects();
            RenderKeyboardInputsFrame(window);
            GuiTestInteractionRect removedKeyRect;
            Expect(!GetGuiTestInteractionRect(customKeyRectId.c_str(), removedKeyRect),
                "Expected the duplicate built-in custom key square to disappear after removal.");
        }

    void RunKeyRebindGuiKeyboardLayoutChangeCustomInputPickerTest(TestRunMode runMode = TestRunMode::Automated) {
        constexpr DWORD kOriginalCustomLayoutSourceVk = VK_F13;
        constexpr DWORD kRetargetedCustomLayoutSourceVk = 'G';
        constexpr DWORD kRetargetedCustomLayoutSourceScan = 0x22;

        DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
        if (SkipIfNoModernGuiTestGL(window)) { return; }
        PrepareRebindGuiCase("key_rebind_gui_keyboard_layout_change_custom_input_picker");

        RenderKeyboardInputsFrame(window);
        RenderKeyboardInputsFrame(window);
        RequestGuiTestOpenKeyboardLayout();
        RenderKeyboardInputsFrame(window);
        RequestGuiTestKeyboardLayoutBeginAddCustomBind();
        RenderKeyboardInputsFrame(window);
        SubmitKeyboardBindingEvent(kOriginalCustomLayoutSourceVk);
        RenderKeyboardInputsFrame(window);

        RequestGuiTestOpenKeyboardLayoutContext(kOriginalCustomLayoutSourceVk);
        RenderKeyboardInputsFrame(window);
        RequestGuiTestKeyboardLayoutBeginBind(GuiTestKeyboardLayoutBindTarget::FullOutputVk);
        RenderKeyboardInputsFrame(window);
        SubmitKeyboardBindingEvent('B');
        RenderKeyboardInputsFrame(window);

        Expect(g_config.keyRebinds.layoutExtraKeys.size() == 1,
            "Expected the custom input picker fixture to start with one explicit custom layout key.");
        Expect(g_config.keyRebinds.layoutExtraKeys.front() == kOriginalCustomLayoutSourceVk,
            "Expected the custom input picker fixture to start from F13.");
        Expect(g_config.keyRebinds.rebinds.size() == 1,
            "Expected the custom input picker fixture to start with one configured rebind.");

        RequestGuiTestKeyboardLayoutOpenCustomInputPicker();
        RenderKeyboardInputsFrame(window);
        RequestGuiTestKeyboardLayoutSelectCustomInputScan(kRetargetedCustomLayoutSourceScan);
        RenderKeyboardInputsFrame(window);

        Expect(g_config.keyRebinds.layoutExtraKeys.size() == 1,
            "Expected retargeting a custom key input to keep exactly one explicit custom layout key.");
        Expect(g_config.keyRebinds.layoutExtraKeys.front() == kRetargetedCustomLayoutSourceVk,
            "Expected retargeting a custom key input to store G as the new custom layout source key.");
        Expect(g_config.keyRebinds.rebinds.size() == 1,
            "Expected retargeting a custom key input to preserve the existing rebind entry.");
        Expect(g_config.keyRebinds.rebinds.front().fromKey == kRetargetedCustomLayoutSourceVk,
            "Expected retargeting a custom key input to migrate the rebind source to G.");
        Expect(g_config.keyRebinds.rebinds.front().toKey == 'B',
            "Expected retargeting a custom key input to preserve the configured output key.");

        ResetGuiTestInteractionRects();
        RenderKeyboardInputsFrame(window);

        GuiTestInteractionRect customKeyRect;
        const std::string oldCustomKeyRectId =
            std::string("inputs.keyboard_layout.custom_key.") + std::to_string(static_cast<unsigned>(kOriginalCustomLayoutSourceVk));
        const std::string newCustomKeyRectId =
            std::string("inputs.keyboard_layout.custom_key.") + std::to_string(static_cast<unsigned>(kRetargetedCustomLayoutSourceVk));
        Expect(!GetGuiTestInteractionRect(oldCustomKeyRectId.c_str(), customKeyRect),
            "Expected the original F13 custom key square to disappear after retargeting its input.");
        Expect(GetGuiTestInteractionRect(newCustomKeyRectId.c_str(), customKeyRect),
            "Expected the retargeted G custom key square to appear after changing the custom input key.");
    }

    void RunKeyRebindGuiKeyboardLayoutChangeCustomInputCaptureTest(TestRunMode runMode = TestRunMode::Automated) {
        constexpr DWORD kOriginalCustomLayoutSourceVk = VK_F13;
        constexpr DWORD kRetargetedCustomLayoutSourceVk = 'G';

        DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
        if (SkipIfNoModernGuiTestGL(window)) { return; }
        PrepareRebindGuiCase("key_rebind_gui_keyboard_layout_change_custom_input_capture");

        RenderKeyboardInputsFrame(window);
        RenderKeyboardInputsFrame(window);
        RequestGuiTestOpenKeyboardLayout();
        RenderKeyboardInputsFrame(window);
        RequestGuiTestKeyboardLayoutBeginAddCustomBind();
        RenderKeyboardInputsFrame(window);
        SubmitKeyboardBindingEvent(kOriginalCustomLayoutSourceVk);
        RenderKeyboardInputsFrame(window);

        RequestGuiTestOpenKeyboardLayoutContext(kOriginalCustomLayoutSourceVk);
        RenderKeyboardInputsFrame(window);
        RequestGuiTestKeyboardLayoutBeginBind(GuiTestKeyboardLayoutBindTarget::FullOutputVk);
        RenderKeyboardInputsFrame(window);
        SubmitKeyboardBindingEvent('B');
        RenderKeyboardInputsFrame(window);

        RequestGuiTestKeyboardLayoutBeginCustomInputCapture();
        RenderKeyboardInputsFrame(window);
        SubmitKeyboardBindingEvent(kRetargetedCustomLayoutSourceVk);
        RenderKeyboardInputsFrame(window);

        Expect(g_config.keyRebinds.layoutExtraKeys.size() == 1,
            "Expected live-capturing a custom key input to keep exactly one explicit custom layout key.");
        Expect(g_config.keyRebinds.layoutExtraKeys.front() == kRetargetedCustomLayoutSourceVk,
            "Expected live-capturing a custom key input to store G as the new custom layout source key.");
        Expect(g_config.keyRebinds.rebinds.size() == 1,
            "Expected live-capturing a custom key input to preserve the existing rebind entry.");
        Expect(g_config.keyRebinds.rebinds.front().fromKey == kRetargetedCustomLayoutSourceVk,
            "Expected live-capturing a custom key input to migrate the rebind source to G.");
        Expect(g_config.keyRebinds.rebinds.front().toKey == 'B',
            "Expected live-capturing a custom key input to preserve the configured output key.");

        ResetGuiTestInteractionRects();
        RenderKeyboardInputsFrame(window);

        GuiTestInteractionRect customKeyRect;
        const std::string oldCustomKeyRectId =
            std::string("inputs.keyboard_layout.custom_key.") + std::to_string(static_cast<unsigned>(kOriginalCustomLayoutSourceVk));
        const std::string newCustomKeyRectId =
            std::string("inputs.keyboard_layout.custom_key.") + std::to_string(static_cast<unsigned>(kRetargetedCustomLayoutSourceVk));
        Expect(!GetGuiTestInteractionRect(oldCustomKeyRectId.c_str(), customKeyRect),
            "Expected the original F13 custom key square to disappear after live-capturing a new input key.");
        Expect(GetGuiTestInteractionRect(newCustomKeyRectId.c_str(), customKeyRect),
            "Expected the retargeted G custom key square to appear after live-capturing a new input key.");
    }

    void RunKeyRebindGuiKeyboardLayoutFullBindScanPickerRuntimeTest(TestRunMode runMode = TestRunMode::Automated) {
        constexpr DWORD kNumpadEnterScan = 0xE01C;

        DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
        if (SkipIfNoModernGuiTestGL(window)) { return; }
        PrepareRebindGuiCase("key_rebind_gui_keyboard_layout_full_bind_scan_picker_runtime");

        OpenKeyboardLayoutContext(window, 'A');
        BindKeyboardLayoutTarget(window, false, VK_RETURN);
        OpenKeyboardLayoutScanPicker(window);
        SetKeyboardLayoutScanFilter(window, GuiTestKeyboardLayoutScanFilterGroup::Numpad);
        SelectKeyboardLayoutScan(window, kNumpadEnterScan);

        Expect(g_config.keyRebinds.rebinds.size() == 1, "Expected the full rebind scan picker flow to create exactly one key rebind.");
        const KeyRebind& rebind = g_config.keyRebinds.rebinds.front();
        Expect(rebind.fromKey == 'A', "Expected the full rebind scan picker flow to bind from the clicked A key.");
        Expect(rebind.toKey == VK_RETURN, "Expected the full rebind scan picker flow to bind Return as the trigger key.");
        Expect(rebind.useCustomOutput, "Expected the full rebind scan picker flow to enable custom output state.");
        Expect(rebind.customOutputVK == VK_RETURN, "Expected the full rebind scan picker flow to mirror Return as the output VK.");
        Expect(rebind.customOutputScanCode == kNumpadEnterScan,
            "Expected the full rebind scan picker flow to store the Numpad Enter scan code override.");

        g_showGui.store(false, std::memory_order_release);
        PublishConfigSnapshot();

        ScopedRebindMessageCapture capture(window.hwnd());
        ScopedKeyboardStateOverride keyboardState;
        keyboardState.SetKeyDown(VK_SHIFT, false);
        keyboardState.SetToggle(VK_CAPITAL, false);
        keyboardState.Apply();

        const InputHandlerResult keyDownResult = HandleKeyRebinding(window.hwnd(), WM_KEYDOWN, 'A', BuildTestKeyboardMessageLParam('A', true));
        Expect(keyDownResult.consumed, "Expected the GUI-created full rebind scan picker flow to consume WM_KEYDOWN.");
        Expect(capture.messages.size() == 1,
            "Expected the GUI-created full rebind scan picker flow to forward exactly one WM_KEYDOWN message.");
        ExpectCapturedMessage(capture, 0, WM_KEYDOWN, VK_RETURN, "GUI full rebind scan picker WM_KEYDOWN");
        ExpectCapturedScanCode(capture, 0, kNumpadEnterScan, "GUI full rebind scan picker WM_KEYDOWN");
    }

    void RunKeyRebindGuiKeyboardLayoutFullBindScanPickerCannotTypeTest(TestRunMode runMode = TestRunMode::Automated) {
        constexpr DWORD kHomeScan = 0xE047;

        DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
        if (SkipIfNoModernGuiTestGL(window)) { return; }
        PrepareRebindGuiCase("key_rebind_gui_keyboard_layout_full_bind_scan_picker_cannot_type");

        OpenKeyboardLayoutContext(window, 'A');
        OpenKeyboardLayoutScanPicker(window);
        SetKeyboardLayoutScanFilter(window, GuiTestKeyboardLayoutScanFilterGroup::Nav);
        SelectKeyboardLayoutScan(window, kHomeScan);

        Expect(g_config.keyRebinds.rebinds.size() == 1,
            "Expected the full rebind scan picker cannot-type flow to create exactly one key rebind.");
        const KeyRebind& rebind = g_config.keyRebinds.rebinds.front();
        Expect(rebind.fromKey == 'A',
            "Expected the full rebind scan picker cannot-type flow to bind from the clicked A key.");
        Expect(rebind.toKey == VK_HOME,
            "Expected the full rebind scan picker cannot-type flow to resolve Home as the trigger key.");
        Expect(rebind.useCustomOutput,
            "Expected the full rebind scan picker cannot-type flow to keep custom output state enabled.");
        Expect(rebind.customOutputVK == VK_HOME,
            "Expected the full rebind scan picker cannot-type flow to mirror Home as the editable full-output VK.");
        Expect(rebind.customOutputScanCode == kHomeScan,
            "Expected the full rebind scan picker cannot-type flow to store the Home scan code override.");

        ResetGuiTestInteractionRects();
        RenderKeyboardInputsFrame(window);

        const GuiTestKeyboardLayoutKeyLabels labels = ExpectKeyboardLayoutKeyLabels(
            'A', "Expected keyboard-layout labels after selecting Home from the full rebind scan picker.");
        Expect(labels.primaryText == "CT",
            "Expected selecting Home from the full rebind scan picker to render the compact cannot-type indicator.");

        auto osScanName = [](DWORD scanCodeWithFlags) -> std::string {
            LONG keyNameLParam = static_cast<LONG>((scanCodeWithFlags & 0xFF) << 16);
            if ((scanCodeWithFlags & 0xFF00) != 0) { keyNameLParam |= (1 << 24); }
            char keyName[64] = {};
            return GetKeyNameTextA(keyNameLParam, keyName, sizeof(keyName)) > 0 ? std::string(keyName) : std::string();
        };
        const std::string expectedHomeLabel = osScanName(kHomeScan);
        Expect(!expectedHomeLabel.empty() && expectedHomeLabel != osScanName(kHomeScan & 0xFF),
            "Expected the OS to name the extended Home scan code distinctly from Numpad 7 (the extended flag must be honored).");
        Expect(labels.secondaryText == expectedHomeLabel,
            "Expected selecting Home from the full rebind scan picker to render the OS Home label '" + expectedHomeLabel +
                "'. Actual='" + labels.secondaryText + "'");
        Expect(labels.shiftLayerText.empty(),
            "Expected selecting Home from the full rebind scan picker to avoid rendering a Shift-layer label.");

        g_showGui.store(false, std::memory_order_release);
        PublishConfigSnapshot();

        ScopedRebindMessageCapture capture(window.hwnd());
        ScopedKeyboardStateOverride keyboardState;
        keyboardState.SetKeyDown(VK_SHIFT, false);
        keyboardState.SetToggle(VK_CAPITAL, false);
        keyboardState.Apply();

        const LPARAM keyDownLParam = BuildTestKeyboardMessageLParam('A', true);
        const InputHandlerResult keyDownResult = HandleKeyRebinding(window.hwnd(), WM_KEYDOWN, 'A', keyDownLParam);
        Expect(keyDownResult.consumed,
            "Expected the full rebind scan picker cannot-type flow to consume WM_KEYDOWN.");
        Expect(capture.messages.size() == 1,
            "Expected the full rebind scan picker cannot-type flow to forward exactly one WM_KEYDOWN message.");
        ExpectCapturedMessage(capture, 0, WM_KEYDOWN, VK_HOME, "GUI full rebind scan picker cannot-type WM_KEYDOWN");
        ExpectCapturedScanCode(capture, 0, kHomeScan, "GUI full rebind scan picker cannot-type WM_KEYDOWN");

        capture.Clear();
        const InputHandlerResult charResult = HandleCharRebinding(window.hwnd(), WM_CHAR, static_cast<WPARAM>('a'), keyDownLParam);
        Expect(charResult.consumed,
            "Expected the full rebind scan picker cannot-type flow to consume WM_CHAR.");
        Expect(capture.messages.empty(),
            "Expected the full rebind scan picker cannot-type flow to avoid forwarding WM_CHAR.");
    }

    void RunKeyRebindGuiKeyboardLayoutScanPickerFilterTest(TestRunMode runMode = TestRunMode::Automated) {
        constexpr DWORD kNumpadEnterScan = 0xE01C;

        DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
        if (SkipIfNoModernGuiTestGL(window)) { return; }
        PrepareRebindGuiCase("key_rebind_gui_keyboard_layout_scan_picker_filter");

        OpenKeyboardLayoutContext(window, 'A');
        BindKeyboardLayoutTarget(window, true, VK_RETURN);
        OpenKeyboardLayoutScanPicker(window);

        SetKeyboardLayoutScanFilter(window, GuiTestKeyboardLayoutScanFilterGroup::Alpha);
        SelectKeyboardLayoutScan(window, kNumpadEnterScan);

        Expect(g_config.keyRebinds.rebinds.size() == 1, "Expected the split rebind scan picker filter flow to create exactly one key rebind.");
        const KeyRebind& blockedRebind = g_config.keyRebinds.rebinds.front();
        Expect(blockedRebind.toKey == VK_RETURN, "Expected the split rebind scan picker filter flow to keep Return as the trigger key.");
        Expect(blockedRebind.customOutputScanCode == 0,
            "Expected the Alpha filter to reject selecting the Numpad Enter scan code override.");
        Expect(!blockedRebind.useCustomOutput,
            "Expected the Alpha filter rejection to avoid enabling custom output state for the trigger scan override.");

        SetKeyboardLayoutScanFilter(window, GuiTestKeyboardLayoutScanFilterGroup::Numpad);
        SelectKeyboardLayoutScan(window, kNumpadEnterScan);

        const KeyRebind& selectedRebind = g_config.keyRebinds.rebinds.front();
        Expect(selectedRebind.customOutputScanCode == kNumpadEnterScan,
            "Expected the Numpad filter to allow selecting the Numpad Enter scan code override.");
        Expect(selectedRebind.useCustomOutput,
            "Expected selecting the Numpad Enter scan code override to enable custom output state.");
    }

    void RunKeyRebindGuiKeyboardLayoutScanPickerResetToDefaultTest(TestRunMode runMode = TestRunMode::Automated) {
        constexpr DWORD kNumpadEnterScan = 0xE01C;
        constexpr DWORD kDefaultEnterScan = 0x001C;

        DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
        if (SkipIfNoModernGuiTestGL(window)) { return; }
        PrepareRebindGuiCase("key_rebind_gui_keyboard_layout_scan_picker_reset_to_default");

        OpenKeyboardLayoutContext(window, 'A');
        BindKeyboardLayoutTarget(window, true, VK_RETURN);
        OpenKeyboardLayoutScanPicker(window);
        SetKeyboardLayoutScanFilter(window, GuiTestKeyboardLayoutScanFilterGroup::Numpad);
        SelectKeyboardLayoutScan(window, kNumpadEnterScan);

        Expect(g_config.keyRebinds.rebinds.size() == 1, "Expected the split rebind scan picker reset flow to create exactly one key rebind.");
        Expect(g_config.keyRebinds.rebinds.front().customOutputScanCode == kNumpadEnterScan,
            "Expected the split rebind scan picker reset flow to start with the Numpad Enter scan code override.");

        ResetKeyboardLayoutScanToDefault(window);

        const KeyRebind& rebind = g_config.keyRebinds.rebinds.front();
        Expect(rebind.fromKey == 'A', "Expected resetting the scan picker to preserve the source key.");
        Expect(rebind.toKey == VK_RETURN, "Expected resetting the scan picker to preserve the trigger key.");
        Expect(rebind.customOutputScanCode == 0, "Expected resetting the scan picker to clear the trigger scan override.");
        Expect(!rebind.useCustomOutput,
            "Expected resetting the scan picker to disable custom output state when no other overrides remain.");

        g_showGui.store(false, std::memory_order_release);
        PublishConfigSnapshot();

        ScopedRebindMessageCapture capture(window.hwnd());
        ScopedKeyboardStateOverride keyboardState;
        keyboardState.SetKeyDown(VK_SHIFT, false);
        keyboardState.SetToggle(VK_CAPITAL, false);
        keyboardState.Apply();

        const InputHandlerResult keyDownResult = HandleKeyRebinding(window.hwnd(), WM_KEYDOWN, 'A', BuildTestKeyboardMessageLParam('A', true));
        Expect(keyDownResult.consumed, "Expected the reset split rebind scan picker flow to consume WM_KEYDOWN.");
        Expect(capture.messages.size() == 1,
            "Expected the reset split rebind scan picker flow to forward exactly one WM_KEYDOWN message.");
        ExpectCapturedMessage(capture, 0, WM_KEYDOWN, VK_RETURN, "GUI split rebind scan picker reset WM_KEYDOWN");
        ExpectCapturedScanCode(capture, 0, kDefaultEnterScan, "GUI split rebind scan picker reset WM_KEYDOWN");
    }