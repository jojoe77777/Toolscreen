#include "gui_internal.h"

#include "common/i18n.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>

static constexpr float spinnerHoldDelay = 0.2f;
static constexpr float spinnerHoldInterval = 0.01f;

static bool ShouldForceSliderTextInputFromMouseShortcut(bool* shouldSpoofLeftClick) {
    ImGuiIO& io = ImGui::GetIO();
    const bool leftDoubleClick = io.MouseClickedCount[ImGuiMouseButton_Left] == 2;
    const bool rightClick = io.MouseClicked[ImGuiMouseButton_Right];
    if (!leftDoubleClick && !rightClick) { return false; }
    if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) { return false; }

    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const float width = ImGui::CalcItemWidth();
    const float height = ImGui::GetFrameHeight();
    const ImVec2 mouse = io.MousePos;

    const bool inSliderRect = mouse.x >= cursor.x && mouse.x <= (cursor.x + width) && mouse.y >= cursor.y && mouse.y <= (cursor.y + height);
    if (!inSliderRect) { return false; }

    if (shouldSpoofLeftClick) { *shouldSpoofLeftClick = rightClick && !io.MouseClicked[ImGuiMouseButton_Left]; }
    return true;
}

static bool SliderFloatDoubleClickInputImpl(const char* label, float* v, float v_min, float v_max, const char* format,
                                            ImGuiSliderFlags flags) {
    ImGuiIO& io = ImGui::GetIO();
    bool spoofLeftClick = false;
    const bool forceTextInput = ShouldForceSliderTextInputFromMouseShortcut(&spoofLeftClick);
    const bool prevCtrl = io.KeyCtrl;
    const bool prevMouseClickedLeft = io.MouseClicked[ImGuiMouseButton_Left];
    const bool prevMouseDownLeft = io.MouseDown[ImGuiMouseButton_Left];
    const float prevMouseDownDurationLeft = io.MouseDownDuration[ImGuiMouseButton_Left];
    if (forceTextInput) { io.KeyCtrl = true; }
    if (spoofLeftClick) {
        io.MouseClicked[ImGuiMouseButton_Left] = true;
        io.MouseDown[ImGuiMouseButton_Left] = true;
        io.MouseDownDuration[ImGuiMouseButton_Left] = 0.0f;
    }

    bool changed = ImGui::SliderScalar(label, ImGuiDataType_Float, v, &v_min, &v_max, format, flags);

    if (forceTextInput) { io.KeyCtrl = prevCtrl; }
    if (spoofLeftClick) {
        io.MouseClicked[ImGuiMouseButton_Left] = prevMouseClickedLeft;
        io.MouseDown[ImGuiMouseButton_Left] = prevMouseDownLeft;
        io.MouseDownDuration[ImGuiMouseButton_Left] = prevMouseDownDurationLeft;
    }
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip(trc("tooltip.right_click_edit")); }
    return changed;
}

static bool SliderIntDoubleClickInputImpl(const char* label, int* v, int v_min, int v_max, const char* format, ImGuiSliderFlags flags) {
    ImGuiIO& io = ImGui::GetIO();
    bool spoofLeftClick = false;
    const bool forceTextInput = ShouldForceSliderTextInputFromMouseShortcut(&spoofLeftClick);
    const bool prevCtrl = io.KeyCtrl;
    const bool prevMouseClickedLeft = io.MouseClicked[ImGuiMouseButton_Left];
    const bool prevMouseDownLeft = io.MouseDown[ImGuiMouseButton_Left];
    const float prevMouseDownDurationLeft = io.MouseDownDuration[ImGuiMouseButton_Left];
    if (forceTextInput) { io.KeyCtrl = true; }
    if (spoofLeftClick) {
        io.MouseClicked[ImGuiMouseButton_Left] = true;
        io.MouseDown[ImGuiMouseButton_Left] = true;
        io.MouseDownDuration[ImGuiMouseButton_Left] = 0.0f;
    }

    bool changed = ImGui::SliderScalar(label, ImGuiDataType_S32, v, &v_min, &v_max, format, flags);

    if (forceTextInput) { io.KeyCtrl = prevCtrl; }
    if (spoofLeftClick) {
        io.MouseClicked[ImGuiMouseButton_Left] = prevMouseClickedLeft;
        io.MouseDown[ImGuiMouseButton_Left] = prevMouseDownLeft;
        io.MouseDownDuration[ImGuiMouseButton_Left] = prevMouseDownDurationLeft;
    }
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip(trc("tooltip.right_click_edit")); }
    return changed;
}

namespace ImGui {
bool SliderFloatDoubleClickInput(const char* label, float* v, float v_min, float v_max, const char* format, ImGuiSliderFlags flags) {
    return ::SliderFloatDoubleClickInputImpl(label, v, v_min, v_max, format, flags);
}

bool SliderIntDoubleClickInput(const char* label, int* v, int v_min, int v_max, const char* format, ImGuiSliderFlags flags) {
    return ::SliderIntDoubleClickInputImpl(label, v, v_min, v_max, format, flags);
}
}

std::string GetKeyRepeatSliderFormat(int value, int autoValueMs, bool useSystemDefaultLabel) {
    if (value >= 0) {
        return "%d ms";
    }
    if (useSystemDefaultLabel) {
        return trc("label.system_default");
    }
    return std::string(trc("label.auto")) + " (" + std::to_string((std::max)(autoValueMs, 1)) + "ms)";
}

std::string GetKeyRepeatDelaySliderFormat(int value, int autoValueMs, bool useSystemDefaultLabel) {
    if (value >= 0) {
        return "%d ms";
    }

    if (useSystemDefaultLabel) {
        return trc("label.system_default");
    }

    return std::string(trc("label.auto")) + " (" + std::to_string((std::max)(autoValueMs, 1)) + "ms)";
}

void RenderTransitionSettingsHorizontalNoBackground(ModeConfig& mode, const std::string& idSuffix) {
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(10, 5));

    if (ImGui::BeginTable(("TransitionTableNoBg" + idSuffix).c_str(), 1, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Game", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();

        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.3f, 0.5f, 0.7f, 0.8f));
        ImGui::Text(trc("transition.viewport_animation"));
        ImGui::PopStyleColor();
        ImGui::Separator();

        ImGui::Text(trc("transition.type"));
        ImGui::SetNextItemWidth(-FLT_MIN);
        const char* gameOptions[] = { trc("transition.cut"), trc("transition.bounce") };
        int gameType = (mode.gameTransition == GameTransitionType::Cut) ? 0 : 1;
        if (ImGui::Combo(("##GameTrans" + idSuffix).c_str(), &gameType, gameOptions, IM_ARRAYSIZE(gameOptions))) {
            mode.gameTransition = (gameType == 0) ? GameTransitionType::Cut : GameTransitionType::Bounce;
            g_configIsDirty = true;
        }

        if (mode.gameTransition == GameTransitionType::Bounce) {
            ImGui::Spacing();
            ImGui::Text(trc("transition.duration"));
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (Spinner(("##GameDur" + idSuffix).c_str(), &mode.transitionDurationMs, 10, 50, 5000)) { g_configIsDirty = true; }
            ImGui::SameLine();
            ImGui::TextDisabled(trc("transition.ms"));

            ImGui::Spacing();
            ImGui::Text(trc("transition.ease_in"));
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::SliderFloatDoubleClickInput(("##EaseIn" + idSuffix).c_str(), &mode.easeInPower, 1.0f, 6.0f, "%.1f")) {
                g_configIsDirty = true;
            }

            ImGui::Text(trc("transition.ease_out"));
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::SliderFloatDoubleClickInput(("##EaseOut" + idSuffix).c_str(), &mode.easeOutPower, 1.0f, 6.0f, "%.1f")) {
                g_configIsDirty = true;
            }

            ImGui::Spacing();
            ImGui::Text(trc("transition.bounces"));
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (Spinner(("##BounceCount" + idSuffix).c_str(), &mode.bounceCount, 1, 0, 10)) { g_configIsDirty = true; }

            if (mode.bounceCount > 0) {
                ImGui::Text(trc("transition.intensity"));
                ImGui::SetNextItemWidth(-FLT_MIN);
                float displayIntensity = mode.bounceIntensity * 100.0f;
                if (ImGui::SliderFloatDoubleClickInput(("##BounceInt" + idSuffix).c_str(), &displayIntensity, 0.0f, 5.0f, "%.2f")) {
                    mode.bounceIntensity = displayIntensity / 100.0f;
                    g_configIsDirty = true;
                }

                ImGui::Text(trc("transition.bounce_ms"));
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (Spinner(("##BounceDur" + idSuffix).c_str(), &mode.bounceDurationMs, 10, 20, 500)) { g_configIsDirty = true; }
            }

            ImGui::Spacing();
            ImGui::Separator();
            if (ImGui::Checkbox((tr("transition.relative_stretching") + "##" + idSuffix).c_str(), &mode.relativeStretching)) {
                g_configIsDirty = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(trc("transition.tooltip.relative_stretching"));
            }
        }

        ImGui::EndTable();
    }

    ImGui::PopStyleVar();

    ImGui::TextDisabled(trc("transition.note.fullscreen_background"));
}

void RenderTransitionSettingsHorizontal(ModeConfig& mode, const std::string& idSuffix) {
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(10, 5));

    if (ImGui::BeginTable(("TransitionTable" + idSuffix).c_str(), 1, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Game", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();

        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.3f, 0.5f, 0.7f, 0.8f));
        ImGui::Text(trc("transition.viewport_animation"));
        ImGui::PopStyleColor();
        ImGui::Separator();

        ImGui::Text(trc("transition.type"));
        ImGui::SetNextItemWidth(-FLT_MIN);
        const char* gameOptions[] = { trc("transition.cut"), trc("transition.bounce") };
        int gameType = (mode.gameTransition == GameTransitionType::Cut) ? 0 : 1;
        if (ImGui::Combo(("##GameTrans" + idSuffix).c_str(), &gameType, gameOptions, IM_ARRAYSIZE(gameOptions))) {
            mode.gameTransition = (gameType == 0) ? GameTransitionType::Cut : GameTransitionType::Bounce;
            g_configIsDirty = true;
        }

        if (mode.gameTransition == GameTransitionType::Bounce) {
            ImGui::Spacing();
            ImGui::Text(trc("transition.duration"));
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (Spinner(("##GameDur" + idSuffix).c_str(), &mode.transitionDurationMs, 10, 50, 5000)) { g_configIsDirty = true; }
            ImGui::SameLine();
            ImGui::TextDisabled(trc("transition.ms"));

            ImGui::Spacing();
            ImGui::Text(trc("transition.ease_in"));
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::SliderFloatDoubleClickInput(("##EaseIn" + idSuffix).c_str(), &mode.easeInPower, 1.0f, 6.0f, "%.1f")) {
                g_configIsDirty = true;
            }

            ImGui::Text(trc("transition.ease_out"));
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::SliderFloatDoubleClickInput(("##EaseOut" + idSuffix).c_str(), &mode.easeOutPower, 1.0f, 6.0f, "%.1f")) {
                g_configIsDirty = true;
            }

            ImGui::Spacing();
            ImGui::Text(trc("transition.bounces"));
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (Spinner(("##BounceCount" + idSuffix).c_str(), &mode.bounceCount, 1, 0, 10)) { g_configIsDirty = true; }

            if (mode.bounceCount > 0) {
                ImGui::Text(trc("transition.intensity"));
                ImGui::SetNextItemWidth(-FLT_MIN);
                float displayIntensity = mode.bounceIntensity * 100.0f;
                if (ImGui::SliderFloatDoubleClickInput(("##BounceInt" + idSuffix).c_str(), &displayIntensity, 0.0f, 5.0f, "%.2f")) {
                    mode.bounceIntensity = displayIntensity / 100.0f;
                    g_configIsDirty = true;
                }

                ImGui::Text(trc("transition.bounce_ms"));
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (Spinner(("##BounceDur" + idSuffix).c_str(), &mode.bounceDurationMs, 10, 20, 500)) { g_configIsDirty = true; }
            }

            ImGui::Spacing();
            ImGui::Separator();
            if (ImGui::Checkbox((tr("transition.relative_stretching") + "##" + idSuffix).c_str(), &mode.relativeStretching)) {
                g_configIsDirty = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(trc("transition.tooltip.relative_stretching"));
            }

            if (ImGui::Checkbox((tr("transition.skip_x_animation") + "##" + idSuffix).c_str(), &mode.skipAnimateX)) { g_configIsDirty = true; }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(trc("transition.tooltip.skip_x_animation"));
            }

            if (ImGui::Checkbox((tr("transition.skip_y_animation") + "##" + idSuffix).c_str(), &mode.skipAnimateY)) { g_configIsDirty = true; }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(trc("transition.tooltip.skip_y_animation"));
            }
        }

        ImGui::EndTable();
    }

    ImGui::PopStyleVar();

    ImGui::Spacing();
    if (ImGui::Button((tr("transition.preview_transition") + "##" + idSuffix).c_str())) {
        std::lock_guard<std::mutex> pendingLock(g_pendingModeSwitchMutex);
        g_pendingModeSwitch.pending = true;
        g_pendingModeSwitch.isPreview = true;
        g_pendingModeSwitch.previewFromModeId = g_config.defaultMode;
        g_pendingModeSwitch.modeId = mode.id;
        g_pendingModeSwitch.source = "Preview button";
        Log("[GUI] Queued transition preview: {} -> {}", g_config.defaultMode, mode.id);
    }
    ImGui::SameLine();
    HelpMarker(("Preview the transition by switching from your default mode (" + g_config.defaultMode + ") to this mode.").c_str());
}

static std::string& ToUpper(std::string& s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
    return s;
}

std::string VkToString(DWORD vk) {
    if (vk == 0) return "[None]";

    static std::map<DWORD, std::string> specialKeys = { { VK_LBUTTON, "MOUSE1" },
                                                        { VK_RBUTTON, "MOUSE2" },
                                                        { VK_MBUTTON, "MOUSE3" },
                                                        { VK_XBUTTON1, "MOUSE4" },
                                                        { VK_XBUTTON2, "MOUSE5" },
                                                        { VK_TOOLSCREEN_SCROLL_UP, "SCROLL UP" },
                                                        { VK_TOOLSCREEN_SCROLL_DOWN, "SCROLL DOWN" },
                                                        { VK_SHIFT, "SHIFT" },
                                                        { VK_LSHIFT, "LSHIFT" },
                                                        { VK_RSHIFT, "RSHIFT" },
                                                        { VK_CONTROL, "CTRL" },
                                                        { VK_LCONTROL, "LCTRL" },
                                                        { VK_RCONTROL, "RCTRL" },
                                                        { VK_MENU, "ALT" },
                                                        { VK_LMENU, "LALT" },
                                                        { VK_RMENU, "RALT" },
                                                        { VK_LWIN, "LWIN" },
                                                        { VK_RWIN, "RWIN" },
                                                        { VK_F1, "F1" },
                                                        { VK_F2, "F2" },
                                                        { VK_F3, "F3" },
                                                        { VK_F4, "F4" },
                                                        { VK_F5, "F5" },
                                                        { VK_F6, "F6" },
                                                        { VK_F7, "F7" },
                                                        { VK_F8, "F8" },
                                                        { VK_F9, "F9" },
                                                        { VK_F10, "F10" },
                                                        { VK_F11, "F11" },
                                                        { VK_F12, "F12" },
                                                        { VK_F13, "F13" },
                                                        { VK_F14, "F14" },
                                                        { VK_F15, "F15" },
                                                        { VK_F16, "F16" },
                                                        { VK_F17, "F17" },
                                                        { VK_F18, "F18" },
                                                        { VK_F19, "F19" },
                                                        { VK_F20, "F20" },
                                                        { VK_F21, "F21" },
                                                        { VK_F22, "F22" },
                                                        { VK_F23, "F23" },
                                                        { VK_F24, "F24" },
                                                        { VK_BACK, "BACKSPACE" },
                                                        { VK_TAB, "TAB" },
                                                        { VK_RETURN, "ENTER" },
                                                        { VK_CAPITAL, "CAPS LOCK" },
                                                        { VK_ESCAPE, "ESC" },
                                                        { VK_SPACE, "SPACE" },
                                                        { VK_PRIOR, "PAGE UP" },
                                                        { VK_NEXT, "PAGE DOWN" },
                                                        { VK_END, "END" },
                                                        { VK_HOME, "HOME" },
                                                        { VK_LEFT, "LEFT" },
                                                        { VK_UP, "UP" },
                                                        { VK_RIGHT, "RIGHT" },
                                                        { VK_DOWN, "DOWN" },
                                                        { VK_INSERT, "INSERT" },
                                                        { VK_DELETE, "DELETE" },
                                                        { VK_NUMLOCK, "NUM LOCK" },
                                                        { VK_PAUSE, "PAUSE" },
                                                        { VK_NUMPAD0, "NUM 0" },
                                                        { VK_NUMPAD1, "NUM 1" },
                                                        { VK_NUMPAD2, "NUM 2" },
                                                        { VK_NUMPAD3, "NUM 3" },
                                                        { VK_NUMPAD4, "NUM 4" },
                                                        { VK_NUMPAD5, "NUM 5" },
                                                        { VK_NUMPAD6, "NUM 6" },
                                                        { VK_NUMPAD7, "NUM 7" },
                                                        { VK_NUMPAD8, "NUM 8" },
                                                        { VK_NUMPAD9, "NUM 9" },
                                                        { VK_MULTIPLY, "NUM *" },
                                                        { VK_ADD, "NUM +" },
                                                        { VK_SEPARATOR, "NUM SEP" },
                                                        { VK_SUBTRACT, "NUM -" },
                                                        { VK_DECIMAL, "NUM ." },
                                                        { VK_DIVIDE, "NUM /" },
                                                        { VK_OEM_1, ";" },
                                                        { VK_OEM_PLUS, "=" },
                                                        { VK_OEM_COMMA, "," },
                                                        { VK_OEM_MINUS, "-" },
                                                        { VK_OEM_PERIOD, "." },
                                                        { VK_OEM_2, "/" },
                                                        { VK_OEM_3, "`" },
                                                        { VK_OEM_4, "[" },
                                                        { VK_OEM_5, "\\" },
                                                        { VK_OEM_6, "]" },
                                                        { VK_OEM_7, "'" } };
    if (specialKeys.count(vk)) { return specialKeys.at(vk); }

    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { return std::string(1, (char)vk); }

    char keyName[128];
    if (GetKeyNameTextA(MapVirtualKeyA(vk, MAPVK_VK_TO_VSC) << 16, keyName, sizeof(keyName)) != 0) {
        std::string str(keyName);
        return ToUpper(str);
    }

    std::stringstream ss;
    ss << "0x" << std::hex << std::uppercase << vk;
    return ss.str();
}

DWORD StringToVk(const std::string& keyStr) {
    std::string cleanKey = keyStr;

    cleanKey.erase(0, cleanKey.find_first_not_of(" \t\r\n"));
    cleanKey.erase(cleanKey.find_last_not_of(" \t\r\n") + 1);

    ToUpper(cleanKey);

    if (cleanKey.empty()) return 0;

    static std::map<std::string, DWORD> keyMap = {
        {"MOUSE1", VK_LBUTTON}, {"LBUTTON", VK_LBUTTON}, {"LEFTMOUSE", VK_LBUTTON},
        {"MOUSE2", VK_RBUTTON}, {"RBUTTON", VK_RBUTTON}, {"RIGHTMOUSE", VK_RBUTTON},
        {"MOUSE3", VK_MBUTTON}, {"MBUTTON", VK_MBUTTON}, {"MIDDLEMOUSE", VK_MBUTTON},
        {"MOUSE4", VK_XBUTTON1}, {"XBUTTON1", VK_XBUTTON1}, {"MOUSE BUTTON 4", VK_XBUTTON1}, {"MOUSEBUTTON4", VK_XBUTTON1},
        {"MOUSE5", VK_XBUTTON2}, {"XBUTTON2", VK_XBUTTON2}, {"MOUSE BUTTON 5", VK_XBUTTON2}, {"MOUSEBUTTON5", VK_XBUTTON2},
        {"SCROLL UP", VK_TOOLSCREEN_SCROLL_UP}, {"SCROLLUP", VK_TOOLSCREEN_SCROLL_UP}, {"WHEEL UP", VK_TOOLSCREEN_SCROLL_UP},
        {"WHEELUP", VK_TOOLSCREEN_SCROLL_UP}, {"MOUSE WHEEL UP", VK_TOOLSCREEN_SCROLL_UP},
        {"MOUSEWHEELUP", VK_TOOLSCREEN_SCROLL_UP}, {"SCROLL DOWN", VK_TOOLSCREEN_SCROLL_DOWN},
        {"SCROLLDOWN", VK_TOOLSCREEN_SCROLL_DOWN}, {"WHEEL DOWN", VK_TOOLSCREEN_SCROLL_DOWN},
        {"WHEELDOWN", VK_TOOLSCREEN_SCROLL_DOWN}, {"MOUSE WHEEL DOWN", VK_TOOLSCREEN_SCROLL_DOWN},
        {"MOUSEWHEELDOWN", VK_TOOLSCREEN_SCROLL_DOWN},

        {"SHIFT", VK_SHIFT}, {"LSHIFT", VK_LSHIFT}, {"RSHIFT", VK_RSHIFT},
        {"CTRL", VK_CONTROL}, {"CONTROL", VK_CONTROL}, {"LCTRL", VK_LCONTROL}, {"RCTRL", VK_RCONTROL},
        {"LCONTROL", VK_LCONTROL}, {"RCONTROL", VK_RCONTROL},
        {"ALT", VK_MENU}, {"MENU", VK_MENU}, {"LALT", VK_LMENU}, {"RALT", VK_RMENU}, {"LMENU", VK_LMENU}, {"RMENU", VK_RMENU},
        {"WIN", VK_LWIN}, {"WINDOWS", VK_LWIN}, {"LWIN", VK_LWIN}, {"RWIN", VK_RWIN}, {"WINKEY", VK_LWIN}, {"WINDOWSKEY", VK_LWIN},

        {"F1", VK_F1}, {"F2", VK_F2}, {"F3", VK_F3}, {"F4", VK_F4}, {"F5", VK_F5}, {"F6", VK_F6},
        {"F7", VK_F7}, {"F8", VK_F8}, {"F9", VK_F9}, {"F10", VK_F10}, {"F11", VK_F11}, {"F12", VK_F12},
        {"F13", VK_F13}, {"F14", VK_F14}, {"F15", VK_F15}, {"F16", VK_F16}, {"F17", VK_F17}, {"F18", VK_F18},
        {"F19", VK_F19}, {"F20", VK_F20}, {"F21", VK_F21}, {"F22", VK_F22}, {"F23", VK_F23}, {"F24", VK_F24},

        {"BACKSPACE", VK_BACK}, {"BACK", VK_BACK}, {"BKSP", VK_BACK},
        {"TAB", VK_TAB}, {"TABULATOR", VK_TAB},
        {"ENTER", VK_RETURN}, {"RETURN", VK_RETURN}, {"CR", VK_RETURN},
        {"CAPS LOCK", VK_CAPITAL}, {"CAPSLOCK", VK_CAPITAL}, {"CAPS", VK_CAPITAL}, {"CAPITAL", VK_CAPITAL},
        {"ESCAPE", VK_ESCAPE}, {"ESC", VK_ESCAPE},
        {"SPACE", VK_SPACE}, {"SPACEBAR", VK_SPACE}, {"SPC", VK_SPACE},

        {"PAGE UP", VK_PRIOR}, {"PAGEUP", VK_PRIOR}, {"PGUP", VK_PRIOR}, {"PRIOR", VK_PRIOR},
        {"PAGE DOWN", VK_NEXT}, {"PAGEDOWN", VK_NEXT}, {"PGDN", VK_NEXT}, {"NEXT", VK_NEXT},
        {"END", VK_END}, {"HOME", VK_HOME},
        {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT}, {"UP", VK_UP}, {"DOWN", VK_DOWN},
        {"ARROW LEFT", VK_LEFT}, {"ARROWLEFT", VK_LEFT}, {"LEFT ARROW", VK_LEFT}, {"LEFTARROW", VK_LEFT},
        {"ARROW RIGHT", VK_RIGHT}, {"ARROWRIGHT", VK_RIGHT}, {"RIGHT ARROW", VK_RIGHT}, {"RIGHTARROW", VK_RIGHT},
        {"ARROW UP", VK_UP}, {"ARROWUP", VK_UP}, {"UP ARROW", VK_UP}, {"UPARROW", VK_UP},
        {"ARROW DOWN", VK_DOWN}, {"ARROWDOWN", VK_DOWN}, {"DOWN ARROW", VK_DOWN}, {"DOWNARROW", VK_DOWN},
        {"INSERT", VK_INSERT}, {"INS", VK_INSERT},
        {"DELETE", VK_DELETE}, {"DEL", VK_DELETE},

        {"NUMPAD 0", VK_NUMPAD0}, {"NUMPAD0", VK_NUMPAD0}, {"NUM 0", VK_NUMPAD0}, {"NUM0", VK_NUMPAD0},
        {"NUMPAD 1", VK_NUMPAD1}, {"NUMPAD1", VK_NUMPAD1}, {"NUM 1", VK_NUMPAD1}, {"NUM1", VK_NUMPAD1},
        {"NUMPAD 2", VK_NUMPAD2}, {"NUMPAD2", VK_NUMPAD2}, {"NUM 2", VK_NUMPAD2}, {"NUM2", VK_NUMPAD2},
        {"NUMPAD 3", VK_NUMPAD3}, {"NUMPAD3", VK_NUMPAD3}, {"NUM 3", VK_NUMPAD3}, {"NUM3", VK_NUMPAD3},
        {"NUMPAD 4", VK_NUMPAD4}, {"NUMPAD4", VK_NUMPAD4}, {"NUM 4", VK_NUMPAD4}, {"NUM4", VK_NUMPAD4},
        {"NUMPAD 5", VK_NUMPAD5}, {"NUMPAD5", VK_NUMPAD5}, {"NUM 5", VK_NUMPAD5}, {"NUM5", VK_NUMPAD5},
        {"NUMPAD 6", VK_NUMPAD6}, {"NUMPAD6", VK_NUMPAD6}, {"NUM 6", VK_NUMPAD6}, {"NUM6", VK_NUMPAD6},
        {"NUMPAD 7", VK_NUMPAD7}, {"NUMPAD7", VK_NUMPAD7}, {"NUM 7", VK_NUMPAD7}, {"NUM7", VK_NUMPAD7},
        {"NUMPAD 8", VK_NUMPAD8}, {"NUMPAD8", VK_NUMPAD8}, {"NUM 8", VK_NUMPAD8}, {"NUM8", VK_NUMPAD8},
        {"NUMPAD 9", VK_NUMPAD9}, {"NUMPAD9", VK_NUMPAD9}, {"NUM 9", VK_NUMPAD9}, {"NUM9", VK_NUMPAD9},
        {"NUMPAD *", VK_MULTIPLY}, {"NUMPAD*", VK_MULTIPLY}, {"NUM *", VK_MULTIPLY}, {"NUM*", VK_MULTIPLY},
        {"NUMPAD +", VK_ADD}, {"NUMPAD+", VK_ADD}, {"NUM +", VK_ADD}, {"NUM+", VK_ADD},
        {"NUMPAD -", VK_SUBTRACT}, {"NUMPAD-", VK_SUBTRACT}, {"NUM -", VK_SUBTRACT}, {"NUM-", VK_SUBTRACT},
        {"NUMPAD .", VK_DECIMAL}, {"NUMPAD.", VK_DECIMAL}, {"NUM .", VK_DECIMAL}, {"NUM.", VK_DECIMAL},
        {"NUMPAD /", VK_DIVIDE}, {"NUMPAD/", VK_DIVIDE}, {"NUM /", VK_DIVIDE}, {"NUM/", VK_DIVIDE},
        {"NUMPAD SEP", VK_SEPARATOR}, {"NUMPADSEP", VK_SEPARATOR}, {"NUM SEP", VK_SEPARATOR}, {"NUMSEP", VK_SEPARATOR},

        {";", VK_OEM_1}, {"SEMICOLON", VK_OEM_1},
        {"=", VK_OEM_PLUS}, {"EQUALS", VK_OEM_PLUS}, {"PLUS", VK_OEM_PLUS},
        {",", VK_OEM_COMMA}, {"COMMA", VK_OEM_COMMA},
        {"-", VK_OEM_MINUS}, {"MINUS", VK_OEM_MINUS}, {"DASH", VK_OEM_MINUS}, {"HYPHEN", VK_OEM_MINUS},
        {".", VK_OEM_PERIOD}, {"PERIOD", VK_OEM_PERIOD}, {"DOT", VK_OEM_PERIOD},
        {"/", VK_OEM_2}, {"SLASH", VK_OEM_2}, {"FORWARDSLASH", VK_OEM_2},
        {"`", VK_OEM_3}, {"GRAVE", VK_OEM_3}, {"BACKTICK", VK_OEM_3}, {"TILDE", VK_OEM_3},
        {"[", VK_OEM_4}, {"LEFTBRACKET", VK_OEM_4}, {"OPENBRACKET", VK_OEM_4},
        {"\\", VK_OEM_5}, {"BACKSLASH", VK_OEM_5},
        {"]", VK_OEM_6}, {"RIGHTBRACKET", VK_OEM_6}, {"CLOSEBRACKET", VK_OEM_6},
        {"'", VK_OEM_7}, {"QUOTE", VK_OEM_7}, {"APOSTROPHE", VK_OEM_7}, {"SINGLEQUOTE", VK_OEM_7},

        {"SCROLL LOCK", VK_SCROLL}, {"SCROLLLOCK", VK_SCROLL}, {"SCROLL", VK_SCROLL},
        {"NUM LOCK", VK_NUMLOCK}, {"NUMLOCK", VK_NUMLOCK},

        {"PRINT SCREEN", VK_SNAPSHOT}, {"PRINTSCREEN", VK_SNAPSHOT}, {"PRTSC", VK_SNAPSHOT}, {"SNAPSHOT", VK_SNAPSHOT},
        {"PAUSE", VK_PAUSE}, {"BREAK", VK_PAUSE}, {"PAUSE BREAK", VK_PAUSE}, {"PAUSEBREAK", VK_PAUSE},
        {"APPS", VK_APPS}, {"APPLICATION", VK_APPS}, {"CONTEXT", VK_APPS}, {"CONTEXTMENU", VK_APPS}
    };

    if (keyMap.count(cleanKey)) { return keyMap[cleanKey]; }

    if (cleanKey.length() == 1) {
        char c = cleanKey[0];
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) { return (DWORD)c; }
        return VkKeyScanA(c) & 0xFF;
    }

    if (cleanKey.rfind("0X", 0) == 0 && cleanKey.length() > 2) {
        try {
            return std::stoul(cleanKey.substr(2), nullptr, 16);
        } catch (...) { return 0; }
    }

    if (std::all_of(cleanKey.begin(), cleanKey.end(), ::isdigit)) {
        try {
            DWORD val = std::stoul(cleanKey);
            return (val > 0 && val < 256) ? val : 0;
        } catch (...) { return 0; }
    }

    return 0;
}

std::vector<DWORD> ParseHotkeyString(const std::string& hotkeyStr) {
    std::vector<DWORD> keys;

    if (hotkeyStr.empty()) return keys;

    std::string cleanStr = hotkeyStr;

    cleanStr.erase(0, cleanStr.find_first_not_of(" \t\r\n"));
    cleanStr.erase(cleanStr.find_last_not_of(" \t\r\n") + 1);

    if (cleanStr.empty()) return keys;

    std::vector<char> separators = { '+', '-', '_', ',', '|', '&' };
    char usedSeparator = '\0';

    for (char sep : separators) {
        if (cleanStr.find(sep) != std::string::npos) {
            usedSeparator = sep;
            if (sep == '+') break;
        }
    }

    std::vector<std::string> keyParts;
    if (usedSeparator != '\0') {
        std::stringstream ss(cleanStr);
        std::string keyPart;
        while (std::getline(ss, keyPart, usedSeparator)) { keyParts.push_back(keyPart); }
    } else {
        if (cleanStr.find(' ') != std::string::npos) {
            std::stringstream ss(cleanStr);
            std::string word;
            std::string current_key;

            while (ss >> word) {
                if (!current_key.empty()) {
                    DWORD testVk = StringToVk(current_key);
                    if (testVk != 0) {
                        keyParts.push_back(current_key);
                        current_key = word;
                    } else {
                        current_key += " " + word;
                    }
                } else {
                    current_key = word;
                }
            }

            if (!current_key.empty()) { keyParts.push_back(current_key); }
        } else {
            keyParts.push_back(cleanStr);
        }
    }

    for (const std::string& keyPart : keyParts) {
        DWORD vk = StringToVk(keyPart);
        if (vk != 0) { keys.push_back(vk); }
    }

    return keys;
}

ImGuiKey VkToImGuiKey(int vk) {
    switch (vk) {
    case VK_TAB:
        return ImGuiKey_Tab;
    case VK_LEFT:
        return ImGuiKey_LeftArrow;
    case VK_RIGHT:
        return ImGuiKey_RightArrow;
    case VK_UP:
        return ImGuiKey_UpArrow;
    case VK_DOWN:
        return ImGuiKey_DownArrow;
    case VK_PRIOR:
        return ImGuiKey_PageUp;
    case VK_NEXT:
        return ImGuiKey_PageDown;
    case VK_HOME:
        return ImGuiKey_Home;
    case VK_END:
        return ImGuiKey_End;
    case VK_INSERT:
        return ImGuiKey_Insert;
    case VK_DELETE:
        return ImGuiKey_Delete;
    case VK_BACK:
        return ImGuiKey_Backspace;
    case VK_SPACE:
        return ImGuiKey_Space;
    case VK_RETURN:
        return ImGuiKey_Enter;
    case VK_ESCAPE:
        return ImGuiKey_Escape;
    case VK_OEM_7:
        return ImGuiKey_Apostrophe;
    case VK_OEM_COMMA:
        return ImGuiKey_Comma;
    case VK_OEM_MINUS:
        return ImGuiKey_Minus;
    case VK_OEM_PERIOD:
        return ImGuiKey_Period;
    case VK_OEM_2:
        return ImGuiKey_Slash;
    case VK_OEM_1:
        return ImGuiKey_Semicolon;
    case VK_OEM_PLUS:
        return ImGuiKey_Equal;
    case VK_OEM_4:
        return ImGuiKey_LeftBracket;
    case VK_OEM_5:
        return ImGuiKey_Backslash;
    case VK_OEM_6:
        return ImGuiKey_RightBracket;
    case VK_OEM_3:
        return ImGuiKey_GraveAccent;
    case VK_CAPITAL:
        return ImGuiKey_CapsLock;
    case VK_SCROLL:
        return ImGuiKey_ScrollLock;
    case VK_NUMLOCK:
        return ImGuiKey_NumLock;
    case VK_SNAPSHOT:
        return ImGuiKey_PrintScreen;
    case VK_PAUSE:
        return ImGuiKey_Pause;
    case VK_NUMPAD0:
        return ImGuiKey_Keypad0;
    case VK_NUMPAD1:
        return ImGuiKey_Keypad1;
    case VK_NUMPAD2:
        return ImGuiKey_Keypad2;
    case VK_NUMPAD3:
        return ImGuiKey_Keypad3;
    case VK_NUMPAD4:
        return ImGuiKey_Keypad4;
    case VK_NUMPAD5:
        return ImGuiKey_Keypad5;
    case VK_NUMPAD6:
        return ImGuiKey_Keypad6;
    case VK_NUMPAD7:
        return ImGuiKey_Keypad7;
    case VK_NUMPAD8:
        return ImGuiKey_Keypad8;
    case VK_NUMPAD9:
        return ImGuiKey_Keypad9;
    case VK_DECIMAL:
        return ImGuiKey_KeypadDecimal;
    case VK_DIVIDE:
        return ImGuiKey_KeypadDivide;
    case VK_MULTIPLY:
        return ImGuiKey_KeypadMultiply;
    case VK_SUBTRACT:
        return ImGuiKey_KeypadSubtract;
    case VK_ADD:
        return ImGuiKey_KeypadAdd;
    case VK_LSHIFT:
        return ImGuiKey_LeftShift;
    case VK_LCONTROL:
        return ImGuiKey_LeftCtrl;
    case VK_LMENU:
        return ImGuiKey_LeftAlt;
    case VK_LWIN:
        return ImGuiKey_LeftSuper;
    case VK_RSHIFT:
        return ImGuiKey_RightShift;
    case VK_RCONTROL:
        return ImGuiKey_RightCtrl;
    case VK_RMENU:
        return ImGuiKey_RightAlt;
    case VK_RWIN:
        return ImGuiKey_RightSuper;
    case VK_APPS:
        return ImGuiKey_Menu;
    case VK_F1:
        return ImGuiKey_F1;
    case VK_F2:
        return ImGuiKey_F2;
    case VK_F3:
        return ImGuiKey_F3;
    case VK_F4:
        return ImGuiKey_F4;
    case VK_F5:
        return ImGuiKey_F5;
    case VK_F6:
        return ImGuiKey_F6;
    case VK_F7:
        return ImGuiKey_F7;
    case VK_F8:
        return ImGuiKey_F8;
    case VK_F9:
        return ImGuiKey_F9;
    case VK_F10:
        return ImGuiKey_F10;
    case VK_F11:
        return ImGuiKey_F11;
    case VK_F12:
        return ImGuiKey_F12;
    }
    if (vk >= '0' && vk <= '9') return (ImGuiKey)(ImGuiKey_0 + (vk - '0'));
    if (vk >= 'A' && vk <= 'Z') return (ImGuiKey)(ImGuiKey_A + (vk - 'A'));
    return ImGuiKey_None;
}

bool HasDuplicateModeName(const std::string& name, size_t currentIndex) {
    for (size_t i = 0; i < g_config.modes.size(); i++) {
        if (i != currentIndex && g_config.modes[i].id == name) { return true; }
    }
    return false;
}

bool HasDuplicateMirrorName(const std::string& name, size_t currentIndex) {
    for (size_t i = 0; i < g_config.mirrors.size(); i++) {
        if (i != currentIndex && g_config.mirrors[i].name == name) { return true; }
    }
    return false;
}

bool HasDuplicateMirrorGroupName(const std::string& name, size_t currentIndex) {
    for (size_t i = 0; i < g_config.mirrorGroups.size(); i++) {
        if (i != currentIndex && g_config.mirrorGroups[i].name == name) { return true; }
    }
    return false;
}

bool HasDuplicateImageName(const std::string& name, size_t currentIndex) {
    for (size_t i = 0; i < g_config.images.size(); i++) {
        if (i != currentIndex && g_config.images[i].name == name) { return true; }
    }
    return false;
}

bool HasDuplicateWindowOverlayName(const std::string& name, size_t currentIndex) {
    for (size_t i = 0; i < g_config.windowOverlays.size(); i++) {
        if (i != currentIndex && g_config.windowOverlays[i].name == name) { return true; }
    }
    return false;
}

bool HasDuplicateBrowserOverlayName(const std::string& name, size_t currentIndex) {
    for (size_t i = 0; i < g_config.browserOverlays.size(); i++) {
        if (i != currentIndex && g_config.browserOverlays[i].name == name) { return true; }
    }
    return false;
}

bool HasDuplicateEyeZoomOverlayName(const std::string& name, size_t currentIndex) {
    for (size_t i = 0; i < g_config.eyezoom.overlays.size(); i++) {
        if (i != currentIndex && g_config.eyezoom.overlays[i].name == name) { return true; }
    }
    return false;
}

bool Spinner(const char* id_label, int* v, int step, int min_val, int max_val, float inputWidth, float margin) {
    ImGui::PushID(id_label);
    bool value_changed = false;
    float button_size = ImGui::GetFrameHeight();

    ImGuiStorage* storage = ImGui::GetStateStorage();
    ImGuiID minus_id = ImGui::GetID("-btn");
    ImGuiID plus_id = ImGui::GetID("+btn");

    if (ImGui::Button("-", { button_size, button_size })) {
        *v -= step;
        value_changed = true;
    }
    if (ImGui::IsItemActive()) {
        float hold_time = storage->GetFloat(minus_id, 0.0f);
        hold_time += ImGui::GetIO().DeltaTime;
        storage->SetFloat(minus_id, hold_time);

        if (hold_time > spinnerHoldDelay) {
            int repeat_count = (int)((hold_time - spinnerHoldDelay) / spinnerHoldInterval);
            int last_repeat_count = storage->GetInt(ImGui::GetID("-cnt"), 0);
            if (repeat_count > last_repeat_count) {
                *v -= step;
                value_changed = true;
                storage->SetInt(ImGui::GetID("-cnt"), repeat_count);
            }
        }
    } else {
        storage->SetFloat(minus_id, 0.0f);
        storage->SetInt(ImGui::GetID("-cnt"), 0);
    }

    ImGui::SameLine(0, margin);
    ImGui::SetNextItemWidth(inputWidth);
    if (ImGui::InputInt("##value", v, 0, 0)) { value_changed = true; }
    ImGui::SameLine(0, margin);

    if (ImGui::Button("+", { button_size, button_size })) {
        *v += step;
        value_changed = true;
    }
    if (ImGui::IsItemActive()) {
        float hold_time = storage->GetFloat(plus_id, 0.0f);
        hold_time += ImGui::GetIO().DeltaTime;
        storage->SetFloat(plus_id, hold_time);

        if (hold_time > spinnerHoldDelay) {
            int repeat_count = (int)((hold_time - spinnerHoldDelay) / spinnerHoldInterval);
            int last_repeat_count = storage->GetInt(ImGui::GetID("+cnt"), 0);
            if (repeat_count > last_repeat_count) {
                *v += step;
                value_changed = true;
                storage->SetInt(ImGui::GetID("+cnt"), repeat_count);
            }
        }
    } else {
        storage->SetFloat(plus_id, 0.0f);
        storage->SetInt(ImGui::GetID("+cnt"), 0);
    }

    int clamped_v = *v;
    if (clamped_v < min_val) clamped_v = min_val;
    if (clamped_v > max_val) clamped_v = max_val;
    if (*v != clamped_v) {
        *v = clamped_v;
        value_changed = true;
    }

    ImGui::PopID();
    return value_changed;
}

bool SpinnerDeferredTextInput(const char* id_label, int* v, int step, int min_val, int max_val, float inputWidth, float margin) {
    ImGui::PushID(id_label);
    bool value_changed = false;
    float button_size = ImGui::GetFrameHeight();

    ImGuiStorage* storage = ImGui::GetStateStorage();
    ImGuiID minus_id = ImGui::GetID("-btn");
    ImGuiID plus_id = ImGui::GetID("+btn");
    ImGuiID pending_id = ImGui::GetID("pending_value");
    ImGuiID editing_id = ImGui::GetID("pending_editing");

    const auto clamp_value = [&](int value) {
        if (value < min_val) value = min_val;
        if (value > max_val) value = max_val;
        return value;
    };

    auto sync_pending_from_committed = [&]() {
        storage->SetInt(pending_id, *v);
        storage->SetInt(editing_id, 0);
    };

    if (storage->GetInt(editing_id, 0) == 0) { storage->SetInt(pending_id, *v); }

    if (ImGui::Button("-", { button_size, button_size })) {
        *v = clamp_value(*v - step);
        value_changed = true;
        sync_pending_from_committed();
    }
    if (ImGui::IsItemActive()) {
        float hold_time = storage->GetFloat(minus_id, 0.0f);
        hold_time += ImGui::GetIO().DeltaTime;
        storage->SetFloat(minus_id, hold_time);

        if (hold_time > spinnerHoldDelay) {
            int repeat_count = (int)((hold_time - spinnerHoldDelay) / spinnerHoldInterval);
            int last_repeat_count = storage->GetInt(ImGui::GetID("-cnt"), 0);
            if (repeat_count > last_repeat_count) {
                *v = clamp_value(*v - step);
                value_changed = true;
                sync_pending_from_committed();
                storage->SetInt(ImGui::GetID("-cnt"), repeat_count);
            }
        }
    } else {
        storage->SetFloat(minus_id, 0.0f);
        storage->SetInt(ImGui::GetID("-cnt"), 0);
    }

    ImGui::SameLine(0, margin);
    ImGui::SetNextItemWidth(inputWidth);
    int pending_value = storage->GetInt(pending_id, *v);
    ImGui::InputInt("##value", &pending_value, 0, 0);
    const bool input_active = ImGui::IsItemActive();
    const bool input_committed = ImGui::IsItemDeactivatedAfterEdit();

    if (input_committed) {
        pending_value = clamp_value(pending_value);
        if (*v != pending_value) {
            *v = pending_value;
            value_changed = true;
        }
        sync_pending_from_committed();
    } else if (input_active) {
        storage->SetInt(pending_id, pending_value);
        storage->SetInt(editing_id, 1);
    } else {
        sync_pending_from_committed();
    }

    ImGui::SameLine(0, margin);

    if (ImGui::Button("+", { button_size, button_size })) {
        *v = clamp_value(*v + step);
        value_changed = true;
        sync_pending_from_committed();
    }
    if (ImGui::IsItemActive()) {
        float hold_time = storage->GetFloat(plus_id, 0.0f);
        hold_time += ImGui::GetIO().DeltaTime;
        storage->SetFloat(plus_id, hold_time);

        if (hold_time > spinnerHoldDelay) {
            int repeat_count = (int)((hold_time - spinnerHoldDelay) / spinnerHoldInterval);
            int last_repeat_count = storage->GetInt(ImGui::GetID("+cnt"), 0);
            if (repeat_count > last_repeat_count) {
                *v = clamp_value(*v + step);
                value_changed = true;
                sync_pending_from_committed();
                storage->SetInt(ImGui::GetID("+cnt"), repeat_count);
            }
        }
    } else {
        storage->SetFloat(plus_id, 0.0f);
        storage->SetInt(ImGui::GetID("+cnt"), 0);
    }

    ImGui::PopID();
    return value_changed;
}

bool SpinnerFloat(const char* id_label, float* v, float step, float min_val, float max_val, const char* format) {
    ImGui::PushID(id_label);
    bool value_changed = false;
    float button_size = ImGui::GetFrameHeight();

    ImGuiStorage* storage = ImGui::GetStateStorage();
    ImGuiID minus_id = ImGui::GetID("-btn");
    ImGuiID plus_id = ImGui::GetID("+btn");

    if (ImGui::Button("-", { button_size, button_size })) {
        *v -= step;
        value_changed = true;
    }
    if (ImGui::IsItemActive()) {
        float hold_time = storage->GetFloat(minus_id, 0.0f);
        hold_time += ImGui::GetIO().DeltaTime;
        storage->SetFloat(minus_id, hold_time);

        if (hold_time > spinnerHoldDelay) {
            int repeat_count = (int)((hold_time - spinnerHoldDelay) / spinnerHoldInterval);
            int last_repeat_count = storage->GetInt(ImGui::GetID("-cnt"), 0);
            if (repeat_count > last_repeat_count) {
                *v -= step;
                value_changed = true;
                storage->SetInt(ImGui::GetID("-cnt"), repeat_count);
            }
        }
    } else {
        storage->SetFloat(minus_id, 0.0f);
        storage->SetInt(ImGui::GetID("-cnt"), 0);
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    if (ImGui::InputFloat("##value", v, 0.0f, 0.0f, format)) { value_changed = true; }
    ImGui::SameLine();

    if (ImGui::Button("+", { button_size, button_size })) {
        *v += step;
        value_changed = true;
    }
    if (ImGui::IsItemActive()) {
        float hold_time = storage->GetFloat(plus_id, 0.0f);
        hold_time += ImGui::GetIO().DeltaTime;
        storage->SetFloat(plus_id, hold_time);

        if (hold_time > spinnerHoldDelay) {
            int repeat_count = (int)((hold_time - spinnerHoldDelay) / spinnerHoldInterval);
            int last_repeat_count = storage->GetInt(ImGui::GetID("+cnt"), 0);
            if (repeat_count > last_repeat_count) {
                *v += step;
                value_changed = true;
                storage->SetInt(ImGui::GetID("+cnt"), repeat_count);
            }
        }
    } else {
        storage->SetFloat(plus_id, 0.0f);
        storage->SetInt(ImGui::GetID("+cnt"), 0);
    }

    float clamped_v = *v;
    if (clamped_v < min_val) clamped_v = min_val;
    if (clamped_v > max_val) clamped_v = max_val;
    if (*v != clamped_v) {
        *v = clamped_v;
        value_changed = true;
    }

    ImGui::PopID();
    return value_changed;
}
