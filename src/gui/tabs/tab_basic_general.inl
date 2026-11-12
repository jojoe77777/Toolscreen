if (BeginSelectableSettingsTopTabItem(trc("tabs.general"))) {
    g_currentlyEditingMirror = "";

    SliderCtrlClickTip();
    const std::string g_currentModeId = GetPublishedCurrentModeId();

    ImGui::SeparatorText(trc("label.window"));
    {
        HWND hwnd = g_minecraftHwnd.load(std::memory_order_relaxed);
        const bool canToggleBorderless = (hwnd != NULL && IsWindow(hwnd));

        if (!canToggleBorderless) { ImGui::BeginDisabled(); }
        if (ImGui::Button(trc("general.go_borderless"), ImVec2(150, 0))) {
            ToggleBorderlessWindowedFullscreen(hwnd);
        }
        if (!canToggleBorderless) { ImGui::EndDisabled(); }

        ImGui::SameLine();
        HelpMarker(trc("general.tooltip.go_borderless"));
    }

    auto RenderInlineHotkeyBinding = [&](const std::string& targetModeId, const char* label) {
        int hotkeyIdx = -1;
        for (size_t i = 0; i < g_config.hotkeys.size(); ++i) {
            if (EqualsIgnoreCase(g_config.hotkeys[i].mainMode, "Fullscreen") &&
                EqualsIgnoreCase(g_config.hotkeys[i].secondaryMode, targetModeId)) {
                hotkeyIdx = static_cast<int>(i);
                break;
            }
        }

        ImGui::SameLine();
        ImGui::Text(trc("label.hotkey"));
        ImGui::SameLine();

        if (hotkeyIdx != -1) {
            std::string keyStr = GetKeyComboString(g_config.hotkeys[hotkeyIdx].keys);
            bool isBinding = (s_mainHotkeyToBind == hotkeyIdx);
            const char* buttonLabel = isBinding ? "[Press Keys...]" : (keyStr.empty() ? "[Click to Bind]" : keyStr.c_str());

            ImGui::PushID(label);
            if (ImGui::Button(buttonLabel, ImVec2(120, 0))) {
                s_mainHotkeyToBind = hotkeyIdx;
                s_altHotkeyToBind = { -1, -1 };
                s_exclusionToBind = { -1, -1 };
                MarkHotkeyBindingActive();
            }
            ImGui::PopID();
        } else {
            ImGui::TextDisabled("[No hotkey]");
        }
    };

    auto EnsureModeExists = [&](const std::string& modeId, int width, int height) {
        for (const auto& mode : g_config.modes) {
            if (EqualsIgnoreCase(mode.id, modeId)) return;
        }
        ModeConfig newMode;
        newMode.id = modeId;
        newMode.width = width;
        newMode.height = height;
        newMode.background.selectedMode = "color";
        newMode.background.color = { 0.0f, 0.0f, 0.0f };
        g_config.modes.push_back(newMode);
        g_configIsDirty = true;
    };

    auto EnsureHotkeyForMode = [&](const std::string& targetModeId) {
        for (const auto& hotkey : g_config.hotkeys) {
            if (EqualsIgnoreCase(hotkey.mainMode, "Fullscreen") && EqualsIgnoreCase(hotkey.secondaryMode, targetModeId)) {
                return;
            }
        }
        HotkeyConfig newHotkey;
        newHotkey.keys = std::vector<DWORD>();
        newHotkey.mainMode = "Fullscreen";
        newHotkey.secondaryMode = targetModeId;
        newHotkey.debounce = 100;
        g_config.hotkeys.push_back(newHotkey);
        ResizeHotkeySecondaryModes(g_config.hotkeys.size());
        SetHotkeySecondaryMode(g_config.hotkeys.size() - 1, targetModeId);
        std::lock_guard<std::mutex> hotkeyLock(g_hotkeyMainKeysMutex);
        RebuildHotkeyMainKeys_Internal();
        g_configIsDirty = true;
    };

    auto RemoveModeAndHotkey = [&](const std::string& modeId) {
        for (auto it = g_config.modes.begin(); it != g_config.modes.end(); ++it) {
            if (EqualsIgnoreCase(it->id, modeId)) {
                g_config.modes.erase(it);
                break;
            }
        }
        g_config.hotkeys.erase(std::remove_if(g_config.hotkeys.begin(), g_config.hotkeys.end(),
                                              [&](const HotkeyConfig& h) { return EqualsIgnoreCase(h.secondaryMode, modeId); }),
                               g_config.hotkeys.end());
        RemoveInvalidHotkeyModeReferences(g_config);
        ResetAllHotkeySecondaryModes();
        std::lock_guard<std::mutex> hotkeyLock(g_hotkeyMainKeysMutex);
        RebuildHotkeyMainKeys_Internal();
        g_configIsDirty = true;

        if (EqualsIgnoreCase(g_config.defaultMode, modeId)) {
            g_config.defaultMode = "Fullscreen";
        }

        if (EqualsIgnoreCase(g_currentModeId, modeId)) {
            std::string fallbackMode = (EqualsIgnoreCase(g_config.defaultMode, modeId) || g_config.defaultMode.empty())
                                           ? "Fullscreen" : g_config.defaultMode;
            std::lock_guard<std::mutex> pendingLock(g_pendingModeSwitchMutex);
            g_pendingModeSwitch.pending = true;
            g_pendingModeSwitch.modeId = fallbackMode;
            g_pendingModeSwitch.source = "Basic mode disabled";
            g_pendingModeSwitch.forceInstant = true;
        }
    };

    auto ModeExists = [&](const std::string& modeId) -> bool {
        for (const auto& mode : g_config.modes) {
            if (EqualsIgnoreCase(mode.id, modeId)) return true;
        }
        return false;
    };

    ImGui::SeparatorText(trc("label.modes"));

    auto HasHotkeyBound = [&](const std::string& modeId) -> bool {
        for (const auto& hotkey : g_config.hotkeys) {
            if (EqualsIgnoreCase(hotkey.mainMode, "Fullscreen") && EqualsIgnoreCase(hotkey.secondaryMode, modeId)) {
                return !hotkey.keys.empty();
            }
        }
        return false;
    };

    auto RenderModeHotkeyBinding = [&](const std::string& targetModeId, const char* label) {
        int hotkeyIdx = -1;
        for (size_t i = 0; i < g_config.hotkeys.size(); ++i) {
            if (EqualsIgnoreCase(g_config.hotkeys[i].mainMode, "Fullscreen") &&
                EqualsIgnoreCase(g_config.hotkeys[i].secondaryMode, targetModeId)) {
                hotkeyIdx = static_cast<int>(i);
                break;
            }
        }

        if (hotkeyIdx == -1) return;

        std::string keyStr = GetKeyComboString(g_config.hotkeys[hotkeyIdx].keys);
        bool isBinding = (s_mainHotkeyToBind == hotkeyIdx);
        const char* buttonLabel = isBinding ? "[Press Keys...]" : (keyStr.empty() ? "[Click to Bind]" : keyStr.c_str());

        ImGui::PushID(label);
        const ImGuiStyle& hotkeyStyle = ImGui::GetStyle();
        ImGui::PushStyleColor(ImGuiCol_Button, hotkeyStyle.Colors[ImGuiCol_FrameBg]);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hotkeyStyle.Colors[ImGuiCol_FrameBgHovered]);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, hotkeyStyle.Colors[ImGuiCol_FrameBgActive]);
        float columnWidth = ImGui::GetContentRegionAvail().x;
        if (ImGui::Button(buttonLabel, ImVec2(columnWidth, 0))) {
            s_mainHotkeyToBind = hotkeyIdx;
            s_altHotkeyToBind = { -1, -1 };
            s_exclusionToBind = { -1, -1 };
            MarkHotkeyBindingActive();
        }
        ImGui::PopStyleColor(3);
        ImGui::PopID();
    };

    auto GetModeConfig = [&](const std::string& modeId) -> ModeConfig* {
        for (auto& mode : g_config.modes) {
            if (EqualsIgnoreCase(mode.id, modeId)) return &mode;
        }
        return nullptr;
    };

    enum class EyeZoomInlineKind {
        None,
        LabelsOnly,
        ControlsOnly,
        CopiedFromEyeZoom,
    };

    auto SyncPreemptiveFromEyeZoom = [&]() {
        ModeConfig* eyezoomMode = GetModeConfig("EyeZoom");
        ModeConfig* preemptiveMode = GetModeConfig("Preemptive");
        if (!eyezoomMode || !preemptiveMode) { return false; }

        bool changed = false;
        if (preemptiveMode->relativeWidth >= 0.0f || preemptiveMode->relativeHeight >= 0.0f || preemptiveMode->useRelativeSize) {
            preemptiveMode->relativeWidth = -1.0f;
            preemptiveMode->relativeHeight = -1.0f;
            preemptiveMode->useRelativeSize = false;
            changed = true;
        }

        if (preemptiveMode->width != eyezoomMode->width) {
            preemptiveMode->width = eyezoomMode->width;
            changed = true;
        }
        const int syncedManualWidth = (eyezoomMode->manualWidth > 0) ? eyezoomMode->manualWidth : eyezoomMode->width;
        if (preemptiveMode->manualWidth != syncedManualWidth) {
            preemptiveMode->manualWidth = syncedManualWidth;
            changed = true;
        }

        if (preemptiveMode->height != eyezoomMode->height) {
            preemptiveMode->height = eyezoomMode->height;
            changed = true;
        }
        const int syncedManualHeight = (eyezoomMode->manualHeight > 0) ? eyezoomMode->manualHeight : eyezoomMode->height;
        if (preemptiveMode->manualHeight != syncedManualHeight) {
            preemptiveMode->manualHeight = syncedManualHeight;
            changed = true;
        }

        return changed;
    };

    if (SyncPreemptiveFromEyeZoom()) { g_configIsDirty = true; }

    auto RenderModeTableRow = [&](const std::string& modeId, const char* label, const char* hotkeyLabel, int defaultWidth,
                                  int defaultHeight, int maxWidth, int maxHeight,
                                  EyeZoomInlineKind eyezoomInline = EyeZoomInlineKind::None, bool readOnlyDimensions = false) {
        ModeConfig* modeConfig = GetModeConfig(modeId);
        const bool relativeOnlySizing = EqualsIgnoreCase(modeId, "Fullscreen") || EqualsIgnoreCase(modeId, "Thin") || EqualsIgnoreCase(modeId, "Wide");
        const int safeMaxWidth = (std::max)(1, maxWidth);
        const int safeMaxHeight = (std::max)(1, maxHeight);

        EnsureHotkeyForMode(modeId);

        ImGui::TableNextRow();

        ImGui::TableNextColumn();
        ImGui::Text("%s", label);

        if (readOnlyDimensions) { ImGui::BeginDisabled(); }

        ImGui::TableNextColumn();
        if (modeConfig) {
            ImGui::PushID((std::string(label) + "_width").c_str());
            const float widthColumnAvail = ImGui::GetContentRegionAvail().x;
            if (relativeOnlySizing) {
                float minWidthPct = 1.0f;
                if (EqualsIgnoreCase(modeId, "Thin")) {
                    minWidthPct = (std::min)(100.0f, (330.0f / static_cast<float>(safeMaxWidth)) * 100.0f);
                }

                float widthPct = ((modeConfig->relativeWidth >= 0.0f && modeConfig->relativeWidth <= 1.0f)
                                      ? modeConfig->relativeWidth
                                      : (static_cast<float>(modeConfig->width) / static_cast<float>(safeMaxWidth))) *
                                 100.0f;
                widthPct = (std::max)(minWidthPct, (std::min)(100.0f, widthPct));

                ImGui::SetNextItemWidth(widthColumnAvail);
                if (ImGui::SliderFloat("##w_pct", &widthPct, minWidthPct, 100.0f, "%.1f%%")) {
                    modeConfig->useRelativeSize = true;
                    modeConfig->relativeWidth = widthPct / 100.0f;

                    int computedWidth = static_cast<int>(modeConfig->relativeWidth * static_cast<float>(safeMaxWidth));
                    if (EqualsIgnoreCase(modeId, "Thin")) {
                        computedWidth = (std::max)(330, computedWidth);
                    }
                    modeConfig->width = (std::max)(1, computedWidth);
                    if (EqualsIgnoreCase(modeId, "Fullscreen")) {
                        modeConfig->stretch.enabled = true;
                        modeConfig->stretch.x = 0;
                        modeConfig->stretch.y = 0;
                        modeConfig->stretch.width = safeMaxWidth;
                        modeConfig->stretch.height = safeMaxHeight;
                        if (g_currentModeId == modeId) {
                            HWND hwnd = g_minecraftHwnd.load();
                            if (hwnd) {
                                RetargetActiveModeTransition(*modeConfig);
                                PublishGuiConfigSnapshot();
                                RequestWindowClientResize(hwnd, modeConfig->width, modeConfig->height, "gui:basic_fullscreen_width_slider");
                            }
                        }
                    }
                    g_configIsDirty = true;
                }
            } else {
                const float spinnerInputWidth = (std::max)(1.0f, widthColumnAvail - (ImGui::GetFrameHeight() * 2.0f) - 6.0f);
                if (Spinner("##w", &modeConfig->width, 10, 1, maxWidth, spinnerInputWidth, 3)) {
                    if (EqualsIgnoreCase(modeId, "EyeZoom")) {
                        modeConfig->manualWidth = modeConfig->width;
                        if (SyncPreemptiveFromEyeZoom()) { g_configIsDirty = true; }
                    }
                    modeConfig->relativeWidth = -1.0f;
                    g_configIsDirty = true;
                }
            }
            ImGui::PopID();
        }

        ImGui::TableNextColumn();
        if (modeConfig) {
            ImGui::PushID((std::string(label) + "_height").c_str());
            const float heightColumnAvail = ImGui::GetContentRegionAvail().x;
            if (relativeOnlySizing) {
                float heightPct = ((modeConfig->relativeHeight >= 0.0f && modeConfig->relativeHeight <= 1.0f)
                                       ? modeConfig->relativeHeight
                                       : (static_cast<float>(modeConfig->height) / static_cast<float>(safeMaxHeight))) *
                                  100.0f;
                heightPct = (std::max)(1.0f, (std::min)(100.0f, heightPct));

                ImGui::SetNextItemWidth(heightColumnAvail);
                if (ImGui::SliderFloat("##h_pct", &heightPct, 1.0f, 100.0f, "%.1f%%")) {
                    modeConfig->useRelativeSize = true;
                    modeConfig->relativeHeight = heightPct / 100.0f;
                    modeConfig->height = (std::max)(1, static_cast<int>(modeConfig->relativeHeight * static_cast<float>(safeMaxHeight)));
                    if (EqualsIgnoreCase(modeId, "Fullscreen")) {
                        modeConfig->stretch.enabled = true;
                        modeConfig->stretch.x = 0;
                        modeConfig->stretch.y = 0;
                        modeConfig->stretch.width = safeMaxWidth;
                        modeConfig->stretch.height = safeMaxHeight;
                        if (g_currentModeId == modeId) {
                            HWND hwnd = g_minecraftHwnd.load();
                            if (hwnd) {
                                RetargetActiveModeTransition(*modeConfig);
                                PublishGuiConfigSnapshot();
                                RequestWindowClientResize(hwnd, modeConfig->width, modeConfig->height, "gui:basic_fullscreen_height_slider");
                            }
                        }
                    }
                    g_configIsDirty = true;
                }
            } else {
                const float spinnerInputWidth = (std::max)(1.0f, heightColumnAvail - (ImGui::GetFrameHeight() * 2.0f) - 6.0f);
                if (Spinner("##h", &modeConfig->height, 10, 1, maxHeight, spinnerInputWidth, 3)) {
                    if (EqualsIgnoreCase(modeId, "EyeZoom")) {
                        modeConfig->manualHeight = modeConfig->height;
                        if (SyncPreemptiveFromEyeZoom()) { g_configIsDirty = true; }
                    }
                    modeConfig->relativeHeight = -1.0f;
                    g_configIsDirty = true;
                }
            }
            ImGui::PopID();
        }

        if (readOnlyDimensions) { ImGui::EndDisabled(); }

        ImGui::TableNextColumn();
        RenderModeHotkeyBinding(modeId, hotkeyLabel);

        ImGui::TableNextColumn();
        if (eyezoomInline != EyeZoomInlineKind::None) {
            ImGui::PushID((std::string("eyezoom_inline_settings_") + modeId).c_str());

            if (eyezoomInline == EyeZoomInlineKind::CopiedFromEyeZoom) {
                ImGui::PushTextWrapPos(ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x);
                ImGui::TextDisabled("%s", trc("modes.preemptive.copied_from_eyezoom"));
                ImGui::PopTextWrapPos();
                ImGui::PopID();
                return;
            }

            if (ImGui::BeginTable("##eyezoom_inline_tbl", 2, ImGuiTableFlags_SizingStretchSame)) {
                ImGui::TableNextRow();

                if (eyezoomInline == EyeZoomInlineKind::LabelsOnly) {
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(trc("eyezoom.clone_width"));
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(trc("eyezoom.overlay_pixels"));
                    ImGui::SameLine();
                    HelpMarker(trc("eyezoom.tooltip"));
                } else if (eyezoomInline == EyeZoomInlineKind::ControlsOnly) {
                    ImGui::TableSetColumnIndex(0);
                        int maxCloneWidth = (modeConfig ? modeConfig->width : maxWidth);
                    if (maxCloneWidth < 2) maxCloneWidth = 2;
                        if (maxCloneWidth < g_config.eyezoom.cloneWidth) maxCloneWidth = g_config.eyezoom.cloneWidth;
                    if (SpinnerDeferredTextInput("##EyeZoomCloneWidth", &g_config.eyezoom.cloneWidth, 2, 2, maxCloneWidth, 64, 3)) {
                        if (g_config.eyezoom.cloneWidth % 2 != 0) { g_config.eyezoom.cloneWidth = (g_config.eyezoom.cloneWidth / 2) * 2; }
                        int maxOverlay = g_config.eyezoom.cloneWidth / 2;
                        if (g_config.eyezoom.overlayWidth > maxOverlay) g_config.eyezoom.overlayWidth = maxOverlay;
                        g_configIsDirty = true;
                    }

                    ImGui::TableSetColumnIndex(1);
                    {
                        int maxOverlay = g_config.eyezoom.cloneWidth / 2;
                        if (SpinnerDeferredTextInput("##EyeZoomOverlayWidth", &g_config.eyezoom.overlayWidth, 1, 0, maxOverlay, 64, 3)) g_configIsDirty = true;
                    }
                }

                ImGui::EndTable();
            }

            ImGui::PopID();
        }
    };

    if (ImGui::BeginTable("ModeTable", 5, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Mode", ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn("Width", ImGuiTableColumnFlags_WidthFixed, 120);
        ImGui::TableSetupColumn("Height", ImGuiTableColumnFlags_WidthFixed, 120);
        ImGui::TableSetupColumn("Hotkey", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableSetupColumn("EyeZoom Settings", ImGuiTableColumnFlags_WidthFixed, 240);

        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        const char* headers[] = { trc("general.mode_table.mode"), trc("general.mode_table.width"), trc("general.mode_table.height"), trc("general.mode_table.hotkey"), trc("general.mode_table.eyezoom_settings") };
        for (int i = 0; i < 5; i++) {
            ImGui::TableSetColumnIndex(i);
            float columnWidth = ImGui::GetColumnWidth();
            float textWidth = ImGui::CalcTextSize(headers[i]).x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (columnWidth - textWidth) * 0.5f);
            ImGui::TableHeader(headers[i]);
        }

        int monitorWidth = GetCachedWindowWidth();
        int monitorHeight = GetCachedWindowHeight();

        RenderModeTableRow("Fullscreen", "Fullscreen", "fullscreen_hotkey", monitorWidth, monitorHeight, monitorWidth, monitorHeight,
                           EyeZoomInlineKind::None, false);

        RenderModeTableRow("Thin", "Thin", "thin_hotkey", 400, monitorHeight, monitorWidth, monitorHeight, EyeZoomInlineKind::None);

        RenderModeTableRow("Wide", "Wide", "wide_hotkey", monitorWidth, 400, monitorWidth, monitorHeight, EyeZoomInlineKind::LabelsOnly);

        RenderModeTableRow("EyeZoom", "EyeZoom", "eyezoom_hotkey", 384, 16384, monitorWidth, 16384, EyeZoomInlineKind::ControlsOnly);

        RenderModeTableRow("Preemptive", "Preemptive", "preemptive_hotkey", 384, 16384, monitorWidth, 16384,
                           EyeZoomInlineKind::CopiedFromEyeZoom, true);

        ImGui::EndTable();
    }

    ImGui::SeparatorText(trc("label.sensitivity"));

    RawInputSensitivityNote();
    ImGui::Text(trc("general.global_sensitivity"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    if (ImGui::SliderFloat("##globalSensBasic", &g_config.mouseSensitivity, 0.001f, 10.0f, "%.3fx")) { g_configIsDirty = true; }
    ImGui::SameLine();
    HelpMarker(trc("general.sens.tooltip.global_sensitivity"));

    {
        ModeConfig* eyezoomMode = GetModeConfig("EyeZoom");
        if (eyezoomMode) {
            RawInputSensitivityNote();
            ImGui::Text(trc("general.eyezoom_sensitivity"));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(200);
            if (ImGui::SliderFloat("##eyezoomSensBasic", &eyezoomMode->modeSensitivity, 0.001f, 10.0f, "%.3fx")) {
                if (eyezoomMode->modeSensitivity < 0.001f) eyezoomMode->modeSensitivity = 0.001f;
                eyezoomMode->sensitivityOverrideEnabled = true;
                g_configIsDirty = true;
            }
            ImGui::SameLine();
            HelpMarker(trc("general.sens.tooltip.eyezoom_sensitivity"));
        }
    }

    {
        ModeConfig* preemptiveMode = GetModeConfig("Preemptive");
        if (preemptiveMode) {
            RawInputSensitivityNote();
            ImGui::Text(trc("general.preemptive_sensitivity"));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(200);
            if (ImGui::SliderFloat("##preemptiveSensBasic", &preemptiveMode->modeSensitivity, 0.001f, 10.0f, "%.3fx")) {
                if (preemptiveMode->modeSensitivity < 0.001f) preemptiveMode->modeSensitivity = 0.001f;
                preemptiveMode->sensitivityOverrideEnabled = true;
                g_configIsDirty = true;
            }
            ImGui::SameLine();
            HelpMarker(trc("general.sens.tooltip.preemptive_sensitivity"));
        }
    }

    ImGui::Separator();
    ImGui::SeparatorText(trc("label.overlays"));

    {
        auto FindNinjabrainBotImage = [&]() -> ImageConfig* {
            for (auto& img : g_config.images) {
                if (EqualsIgnoreCase(img.name, "Ninjabrain Bot")) { return &img; }
            }
            return nullptr;
        };

        auto CreateNinjabrainBotImage = [&]() {
            WCHAR tempPath[MAX_PATH];
            if (GetTempPathW(MAX_PATH, tempPath) > 0) {
                std::wstring nbImagePath = std::wstring(tempPath) + L"nb-overlay.png";
                ImageConfig ninjabrainBot;
                ninjabrainBot.name = "Ninjabrain Bot";
                ninjabrainBot.path = WideToUtf8(nbImagePath);
                ninjabrainBot.x = 0;
                ninjabrainBot.y = 0;
                ninjabrainBot.scale = 1.2f;
                ninjabrainBot.relativeTo = "topLeftScreen";
                ninjabrainBot.opacity = 1.0f;
                ninjabrainBot.colorKey = { 55 / 255.0f, 60 / 255.0f, 66 / 255.0f };
                ninjabrainBot.enableColorKey = true;
                ninjabrainBot.colorKeySensitivity = 0.05f;
                ninjabrainBot.background = { true, { 0.0f, 0.0f, 0.0f }, 0.5f };
                g_config.images.push_back(ninjabrainBot);
                g_allImagesLoaded = false;
                g_pendingImageLoad = true;
            }
        };

        auto ModeHasNinjabrain = [&](const std::string& modeId) -> bool {
            ModeConfig* mode = GetModeConfig(modeId);
            return mode && ModeHasSource(*mode, ModeSourceType::Image, "Ninjabrain Bot");
        };

        auto AddNinjabrainToMode = [&](const std::string& modeId) {
            ModeConfig* mode = GetModeConfig(modeId);
            if (mode && AddModeSource(*mode, ModeSourceType::Image, "Ninjabrain Bot")) { g_configIsDirty = true; }
        };

        auto RemoveNinjabrainFromMode = [&](const std::string& modeId) {
            ModeConfig* mode = GetModeConfig(modeId);
            if (mode && RemoveAllModeSources(*mode, ModeSourceType::Image, "Ninjabrain Bot") > 0) { g_configIsDirty = true; }
        };

        bool ninjabrainImageEnabled = ModeHasNinjabrain("Fullscreen") || ModeHasNinjabrain("EyeZoom") || ModeHasNinjabrain("Preemptive") ||
                                     ModeHasNinjabrain("Thin") || ModeHasNinjabrain("Wide");

        if (ImGui::Checkbox(trc("general.ninjabrainbot_overlay_image"), &ninjabrainImageEnabled)) {
            if (ninjabrainImageEnabled) {
                if (!FindNinjabrainBotImage()) { CreateNinjabrainBotImage(); }
                AddNinjabrainToMode("Fullscreen");
                AddNinjabrainToMode("EyeZoom");
                AddNinjabrainToMode("Preemptive");
                AddNinjabrainToMode("Thin");
                AddNinjabrainToMode("Wide");
            } else {
                RemoveNinjabrainFromMode("Fullscreen");
                RemoveNinjabrainFromMode("EyeZoom");
                RemoveNinjabrainFromMode("Preemptive");
                RemoveNinjabrainFromMode("Thin");
                RemoveNinjabrainFromMode("Wide");
            }
            g_configIsDirty = true;
        }

        bool ninjabrainApiEnabled = g_config.ninjabrainOverlay.enabled;
        if (ImGui::Checkbox(trc("general.ninjabrainbot_overlay_api"), &ninjabrainApiEnabled)) {
            g_config.ninjabrainOverlay.enabled = ninjabrainApiEnabled;
            g_configIsDirty = true;
        }
    }

    ImGui::SeparatorText(trc("general.mirrors"));
    ImGui::TextDisabled(trc("general.tooltip.mirrors"));

    auto RenderMirrorAssignments = [&](const std::string& modeId, const char* label) {
        ModeConfig* modeConfig = GetModeConfig(modeId);
        if (!modeConfig) return;

        ImGui::PushID(label);
        if (ImGui::TreeNode(label)) {
            int item_idx_to_remove = -1;
            bool remove_is_group = false;
            std::vector<std::string> mirrorIds;
            std::vector<std::string> mirrorGroupIds;
            mirrorIds.reserve(modeConfig->sources.size());
            mirrorGroupIds.reserve(modeConfig->sources.size());

            for (const auto& source : modeConfig->sources) {
                if (source.type == ModeSourceType::Mirror) {
                    mirrorIds.push_back(source.id);
                } else if (source.type == ModeSourceType::MirrorGroup) {
                    mirrorGroupIds.push_back(source.id);
                }
            }

            for (size_t k = 0; k < mirrorIds.size(); ++k) {
                ImGui::PushID(static_cast<int>(k));
                if (ImGui::Button("X", ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()))) {
                    item_idx_to_remove = static_cast<int>(k);
                    remove_is_group = false;
                }
                ImGui::SameLine();
                ImGui::TextUnformatted(mirrorIds[k].c_str());
                ImGui::PopID();
            }

            for (size_t k = 0; k < mirrorGroupIds.size(); ++k) {
                ImGui::PushID(static_cast<int>(k) + 10000);
                if (ImGui::Button("X", ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()))) {
                    item_idx_to_remove = static_cast<int>(k);
                    remove_is_group = true;
                }
                ImGui::SameLine();
                ImGui::Text("%s %s", trc("general.group"), mirrorGroupIds[k].c_str());
                ImGui::PopID();
            }

            if (item_idx_to_remove != -1) {
                if (remove_is_group) {
                    if (RemoveModeSource(*modeConfig, ModeSourceType::MirrorGroup, mirrorGroupIds[item_idx_to_remove])) {
                        g_configIsDirty = true;
                    }
                } else if (RemoveModeSource(*modeConfig, ModeSourceType::Mirror, mirrorIds[item_idx_to_remove])) {
                    g_configIsDirty = true;
                }
            }

            if (ImGui::BeginCombo("##AddMirrorOrGroup", trc("general.add_mirror_or_group"))) {
                for (const auto& mirrorConf : g_config.mirrors) {
                    if (!ModeHasSource(*modeConfig, ModeSourceType::Mirror, mirrorConf.name) &&
                        ImGui::Selectable(mirrorConf.name.c_str())) {
                        if (AddModeSource(*modeConfig, ModeSourceType::Mirror, mirrorConf.name)) { g_configIsDirty = true; }
                    }
                }
                if (!g_config.mirrors.empty() && !g_config.mirrorGroups.empty()) { ImGui::Separator(); }
                for (const auto& groupConf : g_config.mirrorGroups) {
                    if (!ModeHasSource(*modeConfig, ModeSourceType::MirrorGroup, groupConf.name)) {
                        std::string displayName = std::string(trc("general.group")) + " " + groupConf.name;
                        if (ImGui::Selectable(displayName.c_str())) {
                            if (AddModeSource(*modeConfig, ModeSourceType::MirrorGroup, groupConf.name)) { g_configIsDirty = true; }
                        }
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::TreePop();
        }
        ImGui::PopID();
    };

    RenderMirrorAssignments("Fullscreen", "Fullscreen");
    RenderMirrorAssignments("Thin", "Thin");
    RenderMirrorAssignments("Wide", "Wide");
    RenderMirrorAssignments("EyeZoom", "EyeZoom");
    RenderMirrorAssignments("Preemptive", "Preemptive");

    ImGui::EndTabItem();
}


