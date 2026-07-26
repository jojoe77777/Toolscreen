if (BeginSelectableSettingsTopTabItem(trc("tabs.other"))) {
    g_currentlyEditingMirror = "";

    SliderCtrlClickTip();

    ImGui::SeparatorText(trc("hotkeys.gui_hotkey"));
    ImGui::PushID("basic_gui_hotkey");
    std::string guiKeyStr = GetKeyComboString(g_config.guiHotkey);

    ImGui::Text(trc("hotkeys.gui_hotkey_open_close"));
    ImGui::SameLine();

    bool isBindingGui = (s_mainHotkeyToBind == -999);
    const char* guiButtonLabel = isBindingGui ? trc("hotkeys.press_keys") : (guiKeyStr.empty() ? trc("hotkeys.click_to_bind") : guiKeyStr.c_str());
    if (ImGui::Button(guiButtonLabel, ImVec2(150, 0))) {
        s_mainHotkeyToBind = -999;
        s_altHotkeyToBind = { -1, -1 };
        s_exclusionToBind = { -1, -1 };
            MarkHotkeyBindingActive();
    }
    ImGui::PopID();

    ImGui::SeparatorText(trc("label.overlay_visibility_hotkeys"));

    ImGui::PushID("basic_image_overlay_toggle_hotkey");
    {
        const bool imgOverlaysVisible = g_imageOverlaysVisible.load(std::memory_order_acquire);
        const ImVec4 visibleGreen = ImVec4(0.20f, 1.00f, 0.20f, 1.00f);
        const ImVec4 hiddenRed = ImVec4(1.00f, 0.20f, 0.20f, 1.00f);

        std::string imgKeyStr = GetKeyComboString(g_config.imageOverlaysHotkey);
        ImGui::Text(trc("label.toggle_image_overlays"));
        ImGui::SameLine();
        const bool isBindingImg = (s_mainHotkeyToBind == -997);
        const char* imgBtnLabel = isBindingImg ? trc("hotkeys.press_keys") : (imgKeyStr.empty() ? trc("hotkeys.click_to_bind") : imgKeyStr.c_str());
        if (ImGui::Button(imgBtnLabel, ImVec2(150, 0))) {
            s_mainHotkeyToBind = -997;
            s_altHotkeyToBind = { -1, -1 };
            s_exclusionToBind = { -1, -1 };
                MarkHotkeyBindingActive();
        }
        ImGui::SameLine();
        ImGui::TextDisabled(trc("label.question_mark"));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(trc("tooltip.toggle_image_overlays.basic"));
        }

        ImGui::SameLine();
        ImGui::TextDisabled(trc("label.status"));
        ImGui::SameLine();
        ImGui::TextColored(imgOverlaysVisible ? visibleGreen : hiddenRed, "%s", imgOverlaysVisible ? trc("label.shown") : trc("label.hidden"));
    }
    ImGui::PopID();

    ImGui::PushID("basic_window_overlay_toggle_hotkey");
    {
        const bool winOverlaysVisible = g_windowOverlaysVisible.load(std::memory_order_acquire);
        const ImVec4 visibleGreen = ImVec4(0.20f, 1.00f, 0.20f, 1.00f);
        const ImVec4 hiddenRed = ImVec4(1.00f, 0.20f, 0.20f, 1.00f);

        std::string winKeyStr = GetKeyComboString(g_config.windowOverlaysHotkey);
        ImGui::Text(trc("label.toggle_window_overlays"));
        ImGui::SameLine();
        const bool isBindingWin = (s_mainHotkeyToBind == -996);
        const char* winBtnLabel = isBindingWin ? trc("hotkeys.press_keys") : (winKeyStr.empty() ? trc("hotkeys.click_to_bind") : winKeyStr.c_str());
        if (ImGui::Button(winBtnLabel, ImVec2(150, 0))) {
            s_mainHotkeyToBind = -996;
            s_altHotkeyToBind = { -1, -1 };
            s_exclusionToBind = { -1, -1 };
                MarkHotkeyBindingActive();
        }
        ImGui::SameLine();
        ImGui::TextDisabled(trc("label.question_mark"));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(trc("tooltip.toggle_window_overlays.basic"));
        }

        ImGui::SameLine();
        ImGui::TextDisabled(trc("label.status"));
        ImGui::SameLine();
        ImGui::TextColored(winOverlaysVisible ? visibleGreen : hiddenRed, "%s", winOverlaysVisible ? trc("label.shown") : trc("label.hidden"));
    }
    ImGui::PopID();

    ImGui::PushID("basic_ninjabrain_overlay_toggle_hotkey");
    {
        const bool ninjabrainOverlayVisible = g_ninjabrainOverlayVisible.load(std::memory_order_acquire);
        const ImVec4 visibleGreen = ImVec4(0.20f, 1.00f, 0.20f, 1.00f);
        const ImVec4 hiddenRed = ImVec4(1.00f, 0.20f, 0.20f, 1.00f);

        std::string ninjabrainKeyStr = GetKeyComboString(g_config.ninjabrainOverlayHotkey);
        ImGui::Text(trc("label.toggle_ninjabrain_overlay"));
        ImGui::SameLine();
        const bool isBindingNinjabrain = (s_mainHotkeyToBind == -994);
        const char* ninjabrainBtnLabel =
            isBindingNinjabrain ? trc("hotkeys.press_keys") : (ninjabrainKeyStr.empty() ? trc("hotkeys.click_to_bind") : ninjabrainKeyStr.c_str());
        if (ImGui::Button(ninjabrainBtnLabel, ImVec2(150, 0))) {
            s_mainHotkeyToBind = -994;
            s_altHotkeyToBind = { -1, -1 };
            s_exclusionToBind = { -1, -1 };
                MarkHotkeyBindingActive();
        }
        ImGui::SameLine();
        ImGui::TextDisabled(trc("label.question_mark"));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(trc("tooltip.toggle_ninjabrain_overlay.basic"));
        }

        ImGui::SameLine();
        ImGui::TextDisabled(trc("label.status"));
        ImGui::SameLine();
        ImGui::TextColored(ninjabrainOverlayVisible ? visibleGreen : hiddenRed, "%s",
                           ninjabrainOverlayVisible ? trc("label.shown") : trc("label.hidden"));
    }
    ImGui::PopID();

    ImGui::SeparatorText(trc("hotkeys.window_hotkeys"));
    ImGui::PushID("basic_borderless_toggle");
    HWND hwnd = g_minecraftHwnd.load(std::memory_order_relaxed);
    const bool canToggleBorderless = (hwnd != NULL && IsWindow(hwnd));

    ImGui::Text(trc("label.toggle_borderless"));
    ImGui::SameLine();

    if (!canToggleBorderless) { ImGui::BeginDisabled(); }
    if (ImGui::Button(trc("general.go_borderless"), ImVec2(150, 0))) {
        ToggleBorderlessWindowedFullscreen(hwnd);
    }
    if (!canToggleBorderless) { ImGui::EndDisabled(); }
    ImGui::SameLine();
    HelpMarker(trc("tooltip.toggle_borderless"));
    ImGui::PopID();

    {
        ImGui::PushID("basic_auto_borderless");
        if (ImGui::Checkbox(trc("settings.auto_borderless"), &g_config.autoBorderless)) { g_configIsDirty = true; }
        ImGui::SameLine();
        HelpMarker(trc("tooltip.auto_borderless"));
        ImGui::PopID();
    }

    ImGui::SeparatorText(trc("label.display_settings"));

    if (ImGui::Checkbox(trc("label.hide_animations_in_game"), &g_config.hideAnimationsInGame)) { g_configIsDirty = true; }
    ImGui::SameLine();
    HelpMarker(trc("tooltip.hide_animations_in_game"));

    ImGui::PushID("startup_indicator");
    ImGui::SeparatorText(trc("startup_indicator.section"));

    if (g_config.startupIndicatorMode == 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f), trc("label.warning"));
        ImGui::TextWrapped("%s", trc("startup_indicator.warning"));
    }

    std::string startupHotkey = GetKeyComboString(g_config.guiHotkey);
    if (startupHotkey.empty()) startupHotkey = trc("startup_indicator.hotkey_unset");
    ImGui::TextUnformatted(trc("startup_indicator.current_hotkey", startupHotkey));

    ImGui::Spacing();

    int startupMode = g_config.startupIndicatorMode;
    bool startupModeChanged = false;
    startupModeChanged |= ImGui::RadioButton(trc("startup_indicator.none"), &startupMode, 0);
    ImGui::SameLine(); HelpMarker(trc("startup_indicator.none_help"));
    startupModeChanged |= ImGui::RadioButton(trc("startup_indicator.shortcut_reminder"), &startupMode, 1);
    ImGui::SameLine(); HelpMarker(trc("startup_indicator.shortcut_reminder_help"));
    startupModeChanged |= ImGui::RadioButton(trc("startup_indicator.custom"), &startupMode, 2);
    ImGui::SameLine(); HelpMarker(trc("startup_indicator.custom_help"));

    if (startupModeChanged) {
        g_config.startupIndicatorMode = startupMode;
        g_configIsDirty = true;
    }

    if (g_config.startupIndicatorMode == 2) {
        ImGui::Indent();
        ImGui::TextUnformatted(trc("startup_indicator.image_path"));

        if (ImGui::Button(trc("button.browse"))) {
            ImagePickerResult result = OpenImagePickerAndValidate(g_minecraftHwnd.load(), g_toolscreenPath, g_toolscreenPath);
            if (result.completed && result.success) {
                if (DetectVisualMediaKindFromPath(result.path) == VisualMediaKind::VideoMpeg1) {
                    SetImageError("startup_indicator", trc("startup_indicator.error.video_unsupported"));
                } else {
                    g_config.startupIndicatorImagePath = result.path;
                    ClearImageError("startup_indicator");
                    g_configIsDirty = true;
                    InvalidateStartupIndicatorTexture();
                }
            } else if (result.completed && !result.success && !result.error.empty()) {
                SetImageError("startup_indicator", result.error);
            }
        }

        ImGui::SameLine();
        if (RenderMaskedPathInput("##startup_ind_path", g_config.startupIndicatorImagePath)) {
            g_configIsDirty = true;
            InvalidateStartupIndicatorTexture();
            const std::string& p = g_config.startupIndicatorImagePath;
            if (p.empty()) {
                ClearImageError("startup_indicator");
            } else {
                std::string err = ValidateImageFile(p, g_toolscreenPath);
                if (err.empty()) ClearImageError("startup_indicator");
                else SetImageError("startup_indicator", err);
            }
        }

        if (g_config.startupIndicatorImagePath.empty()) {
            ImGui::TextDisabled("%s", trc("startup_indicator.custom_no_image"));
        }
        std::string startupImgErr = GetImageError("startup_indicator");
        if (!startupImgErr.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", startupImgErr.c_str());
        }
        ImGui::Unindent();
    }
    ImGui::PopID();

    ImGui::SeparatorText(trc("label.font"));

    const std::vector<FontPickerOption> mainGuiFontOptions = BuildFontPickerOptions();
    auto applyMainGuiFontChange = []() {
        g_configIsDirty = true;
        RequestDynamicGuiFontRefresh(true);
    };

    ImGui::Text(trc("label.font"));
    const bool usingCustomMainGuiFont = RenderFontPickerCombo("##MainGuiFontChoice", 300.0f, mainGuiFontOptions, g_config.fontPath,
                                                              s_mainGuiFontPickerState, applyMainGuiFontChange);
    ImGui::SameLine();
    HelpMarker(trc("tooltip.font"));

    if (usingCustomMainGuiFont) {
        ImGui::Text(trc("label.font_path"));
        RenderCustomFontPathEditor("##FontPath", "##MainGuiFont", 300.0f, mainGuiFontOptions, g_config.fontPath,
                                   s_mainGuiFontPickerState, "Select Main GUI Font", applyMainGuiFontChange);
    }

    ImGui::Text(trc("label.scale"));
    ImGui::SetNextItemWidth(160);
    if (ImGui::SliderFloat("##GuiFontScale", &g_config.appearance.guiFontScale, 0.75f, 2.0f, "%.2fx")) {
        g_config.appearance.guiFontScale = std::clamp(g_config.appearance.guiFontScale, 0.75f, 2.0f);
        g_configIsDirty = true;
        RequestDynamicGuiFontRefresh(true);
    }
    ImGui::SameLine();
    HelpMarker(trc("tooltip.gui_font_scale"));

    ImGui::EndTabItem();
}


