if (BeginSelectableSettingsTopTabItem(trc("tabs.inputs"))) {
    g_currentlyEditingMirror = "";
    g_imageDragMode.store(false);
    g_windowOverlayDragMode.store(false);

    if (ImGui::BeginTabBar("InputsSubTabs")) {
        if (ShouldRenderConfigInputsSubTab(ConfigInputsSubTabId::Mouse)) {
            if (BeginSelectableSettingsInputsSubTabItem(trc("inputs.mouse"))) {
                SliderCtrlClickTip();

                const bool showAllMouseSections = MatchesConfigInputsSubTabCategorySearch(ConfigInputsSubTabId::Mouse, s_configGuiSearchState.query);
                const bool showMouseSettingsSection = ShouldRenderConfigSearchSection(showAllMouseSections, {
                    trc("inputs.mouse_settings"),
                    trc("label.mouse_sensitivity"),
                    trc("label.windows_mouse_speed"),
                    trc("label.let_cursor_escape_window"),
                    trc("label.confine_cursor"),
                    "mouse settings",
                    "mouse sensitivity",
                    "confine cursor"
                });
                const bool showCursorConfigurationSection = ShouldRenderConfigSearchSection(showAllMouseSections, {
                    trc("inputs.cursor_configuration"),
                    trc("inputs.enable_custom_cursors"),
                    trc("button.open_cursor_folder"),
                    trc("inputs.cursor"),
                    trc("inputs.cursor_size"),
                    "cursor",
                    "custom cursor",
                    "crosshair"
                });
                const bool showCursorTrailSection = ShouldRenderConfigSearchSection(showAllMouseSections, {
                    trc("cursor_trail.section_title"),
                    trc("cursor_trail.enabled"),
                    trc("cursor_trail.only_on_my_screen"),
                    trc("cursor_trail.only_on_obs"),
                    trc("cursor_trail.lifetime_ms"),
                    trc("cursor_trail.stamp_spacing"),
                    trc("cursor_trail.color"),
                    trc("cursor_trail.opacity"),
                    trc("cursor_trail.blend_mode"),
                    trc("cursor_trail.sprite_path"),
                    trc("cursor_trail.use_gradient"),
                    "cursor trail",
                    "trail",
                    "mouse trail",
                    "gradient"
                });

                if (showMouseSettingsSection) {
                    ImGui::SeparatorText(trc("inputs.mouse_settings"));
                    RecordConfigSearchSectionInteractionRect("config.section.inputs.mouse.mouse_settings");

                    RawInputSensitivityNote();
                    ImGui::Text(trc("label.mouse_sensitivity"));
                    ImGui::SetNextItemWidth(600);
                    if (ImGui::SliderFloat("##mouseSensitivity", &g_config.mouseSensitivity, 0.001f, 10.0f, "%.3fx")) { g_configIsDirty = true; }
                    ImGui::SameLine();
                    HelpMarker(trc("tooltip.mouse_sensitivity"));

                    ImGui::Text(trc("label.windows_mouse_speed"));
                    ImGui::SetNextItemWidth(600);
                    int windowsSpeedValue = g_config.windowsMouseSpeed;
                    if (ImGui::SliderInt("##windowsMouseSpeed", &windowsSpeedValue, 0, 20, windowsSpeedValue == 0 ? trc("label.disabled") : "%d")) {
                        g_config.windowsMouseSpeed = windowsSpeedValue;
                        g_configIsDirty = true;
                    }
                    ImGui::SameLine();
                    HelpMarker(trc("tooltip.windows_mouse_speed"));

                    if (g_gameVersion < GameVersion(1, 13, 0)) {
                        if (ImGui::Checkbox(trc("label.let_cursor_escape_window"), &g_config.allowCursorEscape)) { g_configIsDirty = true; }
                        ImGui::SameLine();
                        HelpMarker(trc("tooltip.let_cursor_escape_window"));
                    }

                    if (ImGui::Checkbox(trc("label.confine_cursor"), &g_config.confineCursor)) {
                        g_configIsDirty = true;
                        if (g_config.confineCursor) {
                            ApplyConfineCursorToGameWindow();
                        } else {
                            ClipCursorDirect(NULL);
                        }
                    }
                    ImGui::SameLine();
                    HelpMarker(trc("tooltip.confine_cursor"));
                }
 
                if (showCursorConfigurationSection) {
                    ImGui::Spacing();
                    ImGui::SeparatorText(trc("inputs.cursor_configuration"));
                    RecordConfigSearchSectionInteractionRect("config.section.inputs.mouse.cursor_configuration");

                    if (ImGui::Checkbox(trc("inputs.enable_custom_cursors"), &g_config.cursors.enabled)) {
                        g_configIsDirty = true;
                        g_cursorsNeedReload = true;
                    }
                    ImGui::SameLine();
                    HelpMarker(trc("tooltip.cursor_change"));

                    if (ImGui::Button(trc("button.open_cursor_folder"))) {
                        if (g_toolscreenPath.empty()) {
                            Log("ERROR: Unable to open custom cursor folder because toolscreen path is empty.");
                        } else {
                            std::wstring cursorsPath = g_toolscreenPath + L"\\cursors";
                            std::error_code ec;
                            if (!std::filesystem::exists(cursorsPath, ec)) { std::filesystem::create_directories(cursorsPath, ec); }

                            HINSTANCE shellResult = ShellExecuteW(NULL, L"open", cursorsPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
                            if ((INT_PTR)shellResult <= 32) { Log("ERROR: Failed to open custom cursor folder."); }
                        }
                    }
                    ImGui::SameLine();
                    HelpMarker(trc("tooltip.open_cursor_folder"));

                    ImGui::Spacing();

                    if (g_config.cursors.enabled) {
                        ImGui::Text(trc("inputs.configure_cursors_for_different_game_states"));
                        ImGui::Spacing();

                        struct CursorOption {
                            std::string key;
                            std::string name;
                            std::string description;
                        };

                        static std::vector<CursorOption> availableCursors;
                        static bool cursorListInitialized = false;
                        static auto lastCursorListRefreshTime = std::chrono::steady_clock::now() - std::chrono::seconds(2);

                        auto now = std::chrono::steady_clock::now();
                        if (!cursorListInitialized || now - lastCursorListRefreshTime >= std::chrono::seconds(2)) {
                            CursorTextures::RefreshCursorDefinitions();

                            availableCursors.clear();

                            auto cursorNames = CursorTextures::GetAvailableCursorNames();
                            availableCursors.reserve(cursorNames.size());

                            for (const auto& cursorName : cursorNames) {
                                std::string displayName = cursorName;

                                if (!displayName.empty()) {
                                    displayName[0] = std::toupper(displayName[0]);
                                    for (auto& c : displayName) {
                                        if (c == '_' || c == '-') c = ' ';
                                    }
                                }

                                std::string description;
                                if (cursorName.find("Cross") != std::string::npos) {
                                    description = tr("inputs.crosshair_cursor");
                                } else if (cursorName.find("Arrow") != std::string::npos) {
                                    description = tr("inputs.arrow_pointer_cursor");
                                } else {
                                    description = tr("inputs.custom_cursor");
                                }

                                availableCursors.push_back({ cursorName, displayName, description });
                            }

                            cursorListInitialized = true;
                            lastCursorListRefreshTime = now;
                        }

                        struct CursorConfigUI {
                            const char* name;
                            CursorConfig* config;
                        };

                        CursorConfigUI cursors[] = { { trc("game_state.title"), &g_config.cursors.title },
                                                     { trc("game_state.wall"), &g_config.cursors.wall },
                                                     { trc("game_state.inworld_free"), &g_config.cursors.ingame } };

                        for (int i = 0; i < 3; ++i) {
                            auto& cursorUI = cursors[i];
                            auto& cursorConfig = *cursorUI.config;
                            ImGui::PushID(i);

                            ImGui::SeparatorText(cursorUI.name);

                            const char* currentCursorName = cursorConfig.cursorName.c_str();
                            std::string currentDescription = "";
                            for (const auto& option : availableCursors) {
                                if (cursorConfig.cursorName == option.key) {
                                    currentCursorName = option.name.c_str();
                                    currentDescription = option.description;
                                    break;
                                }
                            }

                            ImGui::Text(trc("inputs.cursor"));
                            ImGui::SameLine();
                            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.35f);
                            if (ImGui::BeginCombo("##cursor", currentCursorName)) {
                                for (const auto& option : availableCursors) {
                                    ImGui::PushID(option.key.c_str());

                                    bool is_selected = (cursorConfig.cursorName == option.key);

                                    if (ImGui::Selectable(option.name.c_str(), is_selected)) {
                                        cursorConfig.cursorName = option.key;
                                        g_configIsDirty = true;
                                        g_cursorsNeedReload = true;

                                        std::wstring cursorPath;
                                        UINT loadType = IMAGE_CURSOR;
                                        CursorTextures::GetCursorPathByName(option.key, cursorPath, loadType);

                                        const CursorTextures::CursorData* cursorData =
                                            CursorTextures::LoadOrFindCursor(cursorPath, loadType, cursorConfig.cursorSize);
                                        if (cursorData && cursorData->hCursor) { SetCursor(cursorData->hCursor); }
                                    }

                                    if (ImGui::IsItemHovered()) {
                                        ImGui::BeginTooltip();
                                        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.6f, 1.0f), "%s", option.name.c_str());
                                        ImGui::Separator();
                                        ImGui::TextUnformatted(option.description.c_str());
                                        ImGui::EndTooltip();
                                    }

                                    if (is_selected) { ImGui::SetItemDefaultFocus(); }

                                    ImGui::PopID();
                                }
                                ImGui::EndCombo();
                            }

                            if (!currentDescription.empty() && ImGui::IsItemHovered()) {
                                ImGui::BeginTooltip();
                                ImGui::TextUnformatted(currentDescription.c_str());
                                ImGui::EndTooltip();
                            }

                            ImGui::SameLine();
                            ImGui::Spacing();
                            ImGui::SameLine();
                            ImGui::Text(trc("inputs.cursor_size"));
                            ImGui::SameLine();

                            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.8f);
                            int sliderValue = cursorConfig.cursorSize;
                            if (ImGui::SliderInt("##cursorSize", &sliderValue, ConfigDefaults::CURSOR_MIN_SIZE,
                                                 ConfigDefaults::CURSOR_MAX_SIZE, "%d px", ImGuiSliderFlags_AlwaysClamp)) {
                                int newSize = sliderValue;
                                if (newSize != cursorConfig.cursorSize) {
                                    cursorConfig.cursorSize = newSize;
                                    g_configIsDirty = true;

                                    std::wstring cursorPath;
                                    UINT loadType = IMAGE_CURSOR;
                                    CursorTextures::GetCursorPathByName(cursorConfig.cursorName, cursorPath, loadType);

                                    const CursorTextures::CursorData* cursorData = CursorTextures::LoadOrFindCursor(cursorPath, loadType, newSize);
                                    if (cursorData && cursorData->hCursor) { SetCursor(cursorData->hCursor); }
                                }
                            }

                            ImGui::PopID();
                        }

                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();

                        if (ImGui::Button((tr("button.reset_defaults") + "##cursors").c_str())) { ImGui::OpenPopup(trc("inputs.reset_cursors_to_defaults")); }

                        if (ImGui::BeginPopupModal(trc("inputs.reset_cursors_to_defaults"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
                            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f), trc("label.warning"));
                            ImGui::Text(trc("inputs.reset_cursors_to_defaults_tip"));
                            ImGui::Text(trc("label.action_cannot_be_undone"));
                            ImGui::Separator();
                            if (ImGui::Button(trc("button.confirm_reset"), ImVec2(120, 0))) {
                                g_config.cursors = GetDefaultCursors();
                                g_configIsDirty = true;
                                g_cursorsNeedReload = true;
                                ImGui::CloseCurrentPopup();
                            }
                            ImGui::SetItemDefaultFocus();
                            ImGui::SameLine();
                            if (ImGui::Button(trc("label.cancel"), ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
                            ImGui::EndPopup();
                        }
                    }
                }

                if (showCursorTrailSection) {
                    ImGui::Spacing();
                    ImGui::SeparatorText(trc("cursor_trail.section_title"));
                    RecordConfigSearchSectionInteractionRect("config.section.inputs.mouse.cursor_trail");

                    auto& trail = g_config.cursorTrail;

                    if (ImGui::Checkbox(trc("cursor_trail.enabled"), &trail.enabled)) { g_configIsDirty = true; }
                    ImGui::SameLine();
                    HelpMarker(trc("cursor_trail.tooltip.enabled"));

                    if (trail.enabled) {
                        if (ImGui::Checkbox(trc("cursor_trail.only_on_my_screen"), &trail.onlyOnMyScreen)) {
                            if (trail.onlyOnMyScreen) trail.onlyOnObs = false;
                            g_configIsDirty = true;
                        }
                        ImGui::SameLine();
                        HelpMarker(trc("cursor_trail.tooltip.only_on_my_screen"));

                        if (ImGui::Checkbox(trc("cursor_trail.only_on_obs"), &trail.onlyOnObs)) {
                            if (trail.onlyOnObs) trail.onlyOnMyScreen = false;
                            g_configIsDirty = true;
                        }
                        ImGui::SameLine();
                        HelpMarker(trc("cursor_trail.tooltip.only_on_obs"));

                    
                        const float sliderW = 300.0f;

                        ImGui::Text(trc("cursor_trail.lifetime_ms"));
                        ImGui::SetNextItemWidth(sliderW);
                        if (ImGui::SliderInt("##cursor_trail_lifetime", &trail.lifetimeMs, 50, 500, "%d ms",
                                             ImGuiSliderFlags_AlwaysClamp)) { g_configIsDirty = true; }

                        ImGui::Text(trc("cursor_trail.opacity"));
                        ImGui::SetNextItemWidth(sliderW);
                        if (ImGui::SliderFloat("##cursor_trail_opacity", &trail.opacity, 0.0f, 1.0f, "%.2f",
                                               ImGuiSliderFlags_AlwaysClamp)) { g_configIsDirty = true; }

                        ImGui::Text(trc("cursor_trail.blend_mode"));
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(sliderW);
                        const char* blendModes[] = { "Alpha", "Additive" };
                        const char* blendModeLabels[] = { trc("cursor_trail.blend_mode.alpha"),
                                                          trc("cursor_trail.blend_mode.additive") };
                        int blendIdx = 0;
                        for (int i = 0; i < 2; ++i) {
                            if (trail.blendMode == blendModes[i]) { blendIdx = i; break; }
                        }
                        if (ImGui::BeginCombo("##cursor_trail_blend_mode", blendModeLabels[blendIdx])) {
                            for (int i = 0; i < 2; ++i) {
                                const bool selected = (blendIdx == i);
                                if (ImGui::Selectable(blendModeLabels[i], selected)) {
                                    trail.blendMode = blendModes[i];
                                    g_configIsDirty = true;
                                }
                                if (selected) { ImGui::SetItemDefaultFocus(); }
                            }
                            ImGui::EndCombo();
                        }
                        ImGui::SameLine();
                        HelpMarker(trc("cursor_trail.tooltip.blend_mode"));

                        if (ImGui::Checkbox(trc("cursor_trail.use_velocity_size"), &trail.useVelocitySize)) { g_configIsDirty = true; }
                        ImGui::SameLine();
                        HelpMarker(trc("cursor_trail.tooltip.use_velocity_size"));

                        if (trail.useVelocitySize) {
                            ImGui::Text(trc("cursor_trail.velocity_size_intensity"));
                            ImGui::SetNextItemWidth(sliderW);
                            if (ImGui::SliderFloat("##cursor_trail_velocity_intensity", &trail.velocitySizeIntensity, 0.0f, 1.0f, "%.2f",
                                                   ImGuiSliderFlags_AlwaysClamp)) { g_configIsDirty = true; }
                        }

                        ImGui::Text(trail.useGradient ? trc("cursor_trail.head_color") : trc("cursor_trail.color"));
                        float trailColor[3] = { trail.color.r, trail.color.g, trail.color.b };
                        if (ImGui::ColorEdit3("##cursor_trail_color", trailColor)) {
                            trail.color.r = trailColor[0];
                            trail.color.g = trailColor[1];
                            trail.color.b = trailColor[2];
                            g_configIsDirty = true;
                        }

                        if (ImGui::Checkbox(trc("cursor_trail.use_gradient"), &trail.useGradient)) { g_configIsDirty = true; }
                        ImGui::SameLine();
                        HelpMarker(trc("cursor_trail.tooltip.use_gradient"));

                        if (trail.useGradient) {
                            ImGui::Text(trc("cursor_trail.tail_color"));
                            float trailTailColor[3] = { trail.tailColor.r, trail.tailColor.g, trail.tailColor.b };
                            if (ImGui::ColorEdit3("##cursor_trail_tail_color", trailTailColor)) {
                                trail.tailColor.r = trailTailColor[0];
                                trail.tailColor.g = trailTailColor[1];
                                trail.tailColor.b = trailTailColor[2];
                                g_configIsDirty = true;
                            }
                        }

                        if (ImGui::CollapsingHeader(trc("cursor_trail.advanced"))) {
                            ImGui::Text(trc("cursor_trail.stamp_spacing"));
                            ImGui::SetNextItemWidth(sliderW);
                            if (ImGui::SliderInt("##cursor_trail_spacing", &trail.stampSpacingPx, 1, 64, "%d px",
                                                 ImGuiSliderFlags_AlwaysClamp)) { g_configIsDirty = true; }
                            ImGui::SameLine();
                            HelpMarker(trc("cursor_trail.tooltip.stamp_spacing"));

                            ImGui::Text(trc("cursor_trail.sprite_size"));
                            ImGui::SetNextItemWidth(sliderW);
                            if (ImGui::SliderInt("##cursor_trail_sprite_size", &trail.spriteSizePx, 4, 256, "%d px",
                                                 ImGuiSliderFlags_AlwaysClamp)) { g_configIsDirty = true; }

                            ImGui::Text(trc("cursor_trail.tail_size_scale"));
                            ImGui::SetNextItemWidth(sliderW);
                            if (ImGui::SliderFloat("##cursor_trail_tail_scale", &trail.tailSizeScale, 0.0f, 2.0f, "%.2fx",
                                                   ImGuiSliderFlags_AlwaysClamp)) { g_configIsDirty = true; }
                            ImGui::SameLine();
                            HelpMarker(trc("cursor_trail.tooltip.tail_size_scale"));

                            ImGui::Text(trc("cursor_trail.sprite_path"));
                            ImGui::SetNextItemWidth(sliderW);
                            if (ImGui::InputText("##cursor_trail_sprite_path", &trail.spritePath)) {
                                g_configIsDirty = true;
                            }
                            ImGui::SameLine();
                            if (ImGui::Button((tr("button.browse") + "##cursor_trail_browse").c_str())) {
                                ImagePickerResult result = OpenImagePickerAndValidate(
                                    g_minecraftHwnd.load(), g_toolscreenPath, g_toolscreenPath);
                                if (result.completed && result.success) {
                                    trail.spritePath = result.path;
                                    g_configIsDirty = true;
                                }
                            }
                            ImGui::SameLine();
                            if (ImGui::Button((tr("button.clear") + "##cursor_trail_clear").c_str())) {
                                if (!trail.spritePath.empty()) {
                                    trail.spritePath.clear();
                                    g_configIsDirty = true;
                                }
                            }
                            ImGui::SameLine();
                            HelpMarker(trc("cursor_trail.tooltip.sprite_path"));

                            static std::string s_lastValidatedPath;
                            static std::string s_lastValidationError;
                            if (trail.spritePath != s_lastValidatedPath) {
                                s_lastValidatedPath = trail.spritePath;
                                s_lastValidationError = trail.spritePath.empty()
                                    ? std::string{}
                                    : ValidateImageFile(trail.spritePath, g_toolscreenPath, 256);
                            }
                            if (!s_lastValidationError.empty()) {
                                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", s_lastValidationError.c_str());
                            }
                        }
                    }
                }

                ImGui::EndTabItem();
            }
        }

        if (ShouldRenderConfigInputsSubTab(ConfigInputsSubTabId::Keyboard)) {
            if (BeginSelectableSettingsInputsSubTabItem(trc("inputs.keyboard"))) {
                SliderCtrlClickTip();

                const bool showAllKeyboardSections = MatchesConfigInputsSubTabCategorySearch(ConfigInputsSubTabId::Keyboard, s_configGuiSearchState.query);
                const bool showKeyRepeatRateSection = ShouldRenderConfigSearchSection(showAllKeyboardSections, {
                    trc("inputs.key_repeat_rate"),
                    trc("inputs.key_repeat_start_delay"),
                    trc("inputs.key_repeat_delay"),
                    "key repeat",
                    "repeat delay"
                });
                const bool showKeyRebindingSection = ShouldRenderConfigSearchSection(showAllKeyboardSections, {
                    trc("inputs.key_rebinding"),
                    trc("inputs.enable_key_rebinding"),
                    trc("inputs.resolve_rebind_targets_for_hotkeys"),
                    trc("inputs.allow_system_alt_tab"),
                    trc("inputs.allow_system_alt_f4"),
                    trc("inputs.suppress_caps_lock_toggle"),
                    trc("inputs.rebind_indicator"),
                    trc("inputs.indicator_position"),
                    trc("inputs.rebind_toggle_hotkey"),
                    "key rebinding",
                    "rebind",
                    "toggle hotkey"
                });

                if (showKeyRepeatRateSection) {
                    ImGui::SeparatorText(trc("inputs.key_repeat_rate"));
                    RecordConfigSearchSectionInteractionRect("config.section.inputs.keyboard.key_repeat_rate");

                    constexpr bool useSystemKeyRepeat = true;

                    auto clampKeyRepeatStartDelayValue = [useSystemKeyRepeat](int value) {
                        if (value < 0) {
                            return -1;
                        }
                        if (value < 100) {
                            return 100;
                        }

                        value = (std::min)(value, 300);
                        if (useSystemKeyRepeat) {
                            return value;
                        }
                        return 100 + (((value - 100) + 2) / 5) * 5;
                    };

                    auto clampKeyRepeatDelayValue = [useSystemKeyRepeat](int value) {
                        if (value < 0) {
                            return -1;
                        }

                        value = (std::max)(value, 1);
                        return (std::min)(value, useSystemKeyRepeat ? 300 : 50);
                    };

                    ImGui::Text(trc("inputs.key_repeat_start_delay"));
                    ImGui::SetNextItemWidth(600);
                    int startDelayValue = clampKeyRepeatStartDelayValue(g_config.keyRepeatStartDelay);
                    constexpr int kStartDelayAutoSliderValue = 99;
                    int startDelaySliderValue = (startDelayValue < 0) ? kStartDelayAutoSliderValue : startDelayValue;
                    const std::string startDelayFormat =
                        GetKeyRepeatSliderFormat(startDelayValue, ConfigDefaults::CONFIG_KEY_REPEAT_AUTO_START_DELAY_MS, useSystemKeyRepeat);
                    if (ImGui::SliderIntDoubleClickInput("##keyRepeatStartDelay", &startDelaySliderValue, kStartDelayAutoSliderValue, 300,
                                                         startDelayFormat.c_str(),
                                                         ImGuiSliderFlags_AlwaysClamp)) {
                        startDelayValue = (startDelaySliderValue == kStartDelayAutoSliderValue) ? -1 : startDelaySliderValue;
                        startDelayValue = clampKeyRepeatStartDelayValue(startDelayValue);
                        g_config.keyRepeatStartDelay = startDelayValue;
                        g_configIsDirty = true;
                        ApplyKeyRepeatSettings();
                    }
                    RecordConfigSearchSectionInteractionRect("config.control.inputs.keyboard.key_repeat_start_delay");
                    ImGui::SameLine();
                    HelpMarker(trc("inputs.tooltip.key_repeat_start_delay"));

                    ImGui::Text(trc("inputs.key_repeat_delay"));
                    ImGui::SetNextItemWidth(600);
                    int repeatDelayValue = clampKeyRepeatDelayValue(g_config.keyRepeatDelay);
                    const std::string repeatDelayFormat =
                        GetKeyRepeatDelaySliderFormat(repeatDelayValue, ConfigDefaults::CONFIG_KEY_REPEAT_AUTO_DELAY_MS, useSystemKeyRepeat);
                    if (ImGui::SliderIntDoubleClickInput("##keyRepeatDelay", &repeatDelayValue, -1, useSystemKeyRepeat ? 300 : 50,
                                                         repeatDelayFormat.c_str(),
                                                         ImGuiSliderFlags_AlwaysClamp)) {
                        repeatDelayValue = clampKeyRepeatDelayValue(repeatDelayValue);
                        g_config.keyRepeatDelay = repeatDelayValue;
                        g_configIsDirty = true;
                        ApplyKeyRepeatSettings();
                    }
                    RecordConfigSearchSectionInteractionRect("config.control.inputs.keyboard.key_repeat_delay");
                    ImGui::SameLine();
                    HelpMarker(trc("inputs.tooltip.key_repeat_delay"));

                    ImGui::Spacing();
                }

                if (showKeyRebindingSection) {
                    ImGui::SeparatorText(trc("inputs.key_rebinding"));
                    RecordConfigSearchSectionInteractionRect("config.section.inputs.keyboard.key_rebinding");
                    ImGui::TextWrapped(trc("inputs.tooltip.key_rebinding"));
                    ImGui::Spacing();

                    if (ImGui::Checkbox(trc("inputs.enable_key_rebinding"), &g_config.keyRebinds.enabled)) {
                        if (!g_config.keyRebinds.enabled) {
                            ReleaseActiveLowLevelRebindKeys(g_minecraftHwnd.load(std::memory_order_relaxed));
                        }
                        g_configIsDirty = true;
                        std::lock_guard<std::mutex> hotkeyLock(g_hotkeyMainKeysMutex);
                        RebuildHotkeyMainKeys_Internal();
                    }
                    ImGui::SameLine();
                    HelpMarker(trc("inputs.tooltip.enable_key_rebinding"));
                    ImGui::SameLine();
                    {
                        const ImVec4 rebindActiveGreen = ImVec4(0.20f, 1.00f, 0.20f, 1.00f);
                        const ImVec4 rebindDisabledRed = ImVec4(1.00f, 0.20f, 0.20f, 1.00f);
                        ImGui::TextColored(g_config.keyRebinds.enabled ? rebindActiveGreen : rebindDisabledRed, "(%s)",
                                           g_config.keyRebinds.enabled ? trc("label.enabled") : trc("label.disabled"));
                    }

            if (ImGui::Checkbox(trc("inputs.resolve_rebind_targets_for_hotkeys"), &g_config.keyRebinds.resolveRebindTargetsForHotkeys)) {
                g_configIsDirty = true;
            }
            ImGui::SameLine();
            HelpMarker(trc("inputs.tooltip.resolve_rebind_targets"));

            if (ImGui::Checkbox(trc("inputs.allow_system_alt_tab"), &g_config.keyRebinds.allowSystemAltTab)) {
                g_configIsDirty = true;
            }
            ImGui::SameLine();
            HelpMarker(trc("inputs.tooltip.allow_system_alt_tab"));

            if (ImGui::Checkbox(trc("inputs.allow_system_alt_f4"), &g_config.keyRebinds.allowSystemAltF4)) {
                g_configIsDirty = true;
            }
            ImGui::SameLine();
            HelpMarker(trc("inputs.tooltip.allow_system_alt_f4"));

            if (ImGui::Checkbox(trc("inputs.suppress_caps_lock_toggle"), &g_config.keyRebinds.suppressCapsLockToggle)) {
                g_configIsDirty = true;
            }
            ImGui::SameLine();
            HelpMarker(trc("inputs.tooltip.suppress_caps_lock_toggle"));

            {
                const char* modeLabels[] = {
                    trc("label.off"), trc("inputs.indicator_when_active"),
                    trc("inputs.indicator_when_inactive"), trc("inputs.indicator_both_states")
                };
                ImGui::SetNextItemWidth(200);
                if (ImGui::Combo(trc("inputs.rebind_indicator"), &g_config.keyRebinds.indicatorMode, modeLabels, 4)) {
                    g_configIsDirty = true;
                }
                ImGui::SameLine();
                HelpMarker(trc("inputs.tooltip.show_rebind_indicator"));
            }

            if (g_config.keyRebinds.indicatorMode > 0) {
                const char* positionLabels[] = {
                    trc("label.top_left"), trc("label.top_right"),
                    trc("label.bottom_left"), trc("label.bottom_right")
                };
                ImGui::SetNextItemWidth(150);
                if (ImGui::Combo(trc("inputs.indicator_position"), &g_config.keyRebinds.indicatorPosition, positionLabels, 4)) {
                    g_configIsDirty = true;
                }

                auto indicatorImageField = [&](const char* label, std::string& path, const char* browseId, const char* resetId) {
                    std::string displayName = path.empty() ? tr("label.default") : std::filesystem::path(path).filename().string();
                    float btnW = ImGui::CalcTextSize(displayName.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;
                    btnW = (std::max)(btnW, 80.0f);
                    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, path.empty() ? 0.5f : 1.0f);
                    if (ImGui::Button((displayName + browseId).c_str(), ImVec2(btnW, 0))) {
                        ImagePickerResult result = OpenImagePickerAndValidate(g_minecraftHwnd.load(), g_toolscreenPath, g_toolscreenPath);
                        if (result.completed && result.success) {
                            path = result.path;
                            g_configIsDirty = true;
                            InvalidateRebindIndicatorTexture();
                        }
                    }
                    ImGui::PopStyleVar();
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", path.empty() ? trc("inputs.tooltip.default_indicator_image") : path.c_str());
                    }
                    ImGui::SameLine();
                    ImGui::Text("%s", label);
                    if (!path.empty()) {
                        ImGui::SameLine();
                        if (ImGui::SmallButton(resetId)) {
                            path.clear();
                            g_configIsDirty = true;
                            InvalidateRebindIndicatorTexture();
                        }
                    }
                };

                if (g_config.keyRebinds.indicatorMode == 1 || g_config.keyRebinds.indicatorMode == 3) {
                    indicatorImageField(trc("inputs.indicator_image_enabled"), g_config.keyRebinds.indicatorImageEnabled,
                                        "##rebind_ind_on", "X##rebind_img_on");
                }

                if (g_config.keyRebinds.indicatorMode == 2 || g_config.keyRebinds.indicatorMode == 3) {
                    indicatorImageField(trc("inputs.indicator_image_disabled"), g_config.keyRebinds.indicatorImageDisabled,
                                        "##rebind_ind_off", "X##rebind_img_off");
                }
            }

            ImGui::Spacing();
            ImGui::SeparatorText(trc("inputs.rebind_toggle_hotkey"));
            std::string rebindToggleHotkeyStr = GetKeyComboString(g_config.keyRebinds.toggleHotkey);
            const bool isBindingRebindToggle = (s_mainHotkeyToBind == -995);
            const char* rebindToggleHotkeyButtonLabel =
                isBindingRebindToggle ? trc("hotkeys.press_keys")
                                      : (rebindToggleHotkeyStr.empty() ? trc("hotkeys.click_to_bind") : rebindToggleHotkeyStr.c_str());
            if (ImGui::Button(rebindToggleHotkeyButtonLabel, ImVec2(150, 0))) {
                s_mainHotkeyToBind = -995;
                s_altHotkeyToBind = { -1, -1 };
                s_exclusionToBind = { -1, -1 };
                MarkHotkeyBindingActive();
            }
            ImGui::SameLine();
            HelpMarker(trc("inputs.tooltip.rebind_toggle_hotkey"));

            if (g_config.keyRebinds.enabled) {
                ImGui::Separator();
                ImGui::Spacing();

                auto getScanCodeWithExtendedFlag = [](DWORD vk) -> DWORD {
                    DWORD scan = MapVirtualKey(vk, MAPVK_VK_TO_VSC_EX);
                    if (scan == 0) { scan = MapVirtualKey(vk, MAPVK_VK_TO_VSC); }

                    if ((scan & 0xFF00) == 0) {
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
                            if ((scan & 0xFF) != 0) { scan |= 0xE000; }
                            break;
                        default:
                            break;
                        }
                    }

                    return scan;
                };

                static int s_rebindFromKeyToBind = -1;
                static int s_rebindOutputVKToBind = -1;
                static int s_rebindTextOverrideVKToBind = -1;
                static int s_rebindOutputScanToBind = -1;

                enum class LayoutBindTarget {
                    None,
                    FullOutputVk,
                    TypesVk,
                    TypesVkShift,
                    TriggersVk,
                };
                enum class LayoutDisableTarget {
                    All,
                    Types,
                    TypesVkShift,
                    Triggers,
                };
                static LayoutBindTarget s_layoutBindTarget = LayoutBindTarget::None;
                static int s_layoutBindIndex = -1;
                static uint64_t s_layoutBindLastSeq = 0;

                enum class LayoutUnicodeEditTarget {
                    None,
                    TypesBase,
                    TypesShift,
                };

                static int s_layoutUnicodeEditIndex = -1;
                static std::string s_layoutUnicodeEditText;
                static LayoutUnicodeEditTarget s_layoutUnicodeEditTarget = LayoutUnicodeEditTarget::None;

                static std::vector<std::string> s_rebindUnicodeTextEdit;

                auto isValidUnicodeScalar = [](uint32_t cp) -> bool {
                    if (cp == 0) return false;
                    if (cp > 0x10FFFFu) return false;
                    if (cp >= 0xD800u && cp <= 0xDFFFu) return false;
                    return true;
                };

                auto codepointToUtf8 = [&](uint32_t cp) -> std::string {
                    if (!isValidUnicodeScalar(cp)) return std::string();
                    std::wstring w;
                    if (cp <= 0xFFFFu) {
                        w.push_back((wchar_t)cp);
                    } else {
                        uint32_t v = cp - 0x10000u;
                        wchar_t high = (wchar_t)(0xD800u + (v >> 10));
                        wchar_t low = (wchar_t)(0xDC00u + (v & 0x3FFu));
                        w.push_back(high);
                        w.push_back(low);
                    }
                    return WideToUtf8(w);
                };

                auto formatCodepointUPlus = [&](uint32_t cp) -> std::string {
                    char buf[32] = {};
                    if (cp <= 0xFFFFu) {
                        sprintf_s(buf, "U+%04X", (unsigned)cp);
                    } else {
                        sprintf_s(buf, "U+%06X", (unsigned)cp);
                    }
                    return std::string(buf);
                };

                auto codepointToDisplay = [&](uint32_t cp) -> std::string {
                    if (!isValidUnicodeScalar(cp)) return std::string("[None]");
                    if (cp < 0x20u || cp == 0x7Fu) return formatCodepointUPlus(cp);
                    std::string s = codepointToUtf8(cp);
                    if (s.empty()) return formatCodepointUPlus(cp);
                    return s;
                };

                auto tryParseUnicodeInput = [&](const std::string& in, uint32_t& outCp) -> bool {
                    auto isSpace = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
                    std::string s = in;
                    while (!s.empty() && isSpace((unsigned char)s.front())) s.erase(s.begin());
                    while (!s.empty() && isSpace((unsigned char)s.back())) s.pop_back();
                    if (s.empty()) return false;

                    auto startsWithI = [&](const char* pfx) {
                        size_t n = std::char_traits<char>::length(pfx);
                        if (s.size() < n) return false;
                        for (size_t i = 0; i < n; ++i) {
                            char a = s[i];
                            char b = pfx[i];
                            if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
                            if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');
                            if (a != b) return false;
                        }
                        return true;
                    };

                    std::string hex;
                    if (startsWithI("U+")) hex = s.substr(2);
                    else if (startsWithI("\\\\u") || startsWithI("\\\\U")) hex = s.substr(2);
                    else if (startsWithI("0X")) hex = s.substr(2);

                    if (!hex.empty()) {
                        if (!hex.empty() && hex.front() == '{' && hex.back() == '}') hex = hex.substr(1, hex.size() - 2);
                        try {
                            size_t idx = 0;
                            unsigned long v = std::stoul(hex, &idx, 16);
                            if (idx == 0) return false;
                            if (!isValidUnicodeScalar((uint32_t)v)) return false;
                            outCp = (uint32_t)v;
                            return true;
                        } catch (...) {
                            return false;
                        }
                    }

                    std::wstring w = Utf8ToWide(s);
                    if (!w.empty()) {
                        uint32_t cp = 0;
                        if (w.size() >= 2 && w[0] >= 0xD800 && w[0] <= 0xDBFF && w[1] >= 0xDC00 && w[1] <= 0xDFFF) {
                            cp = 0x10000u + (((uint32_t)w[0] - 0xD800u) << 10) + ((uint32_t)w[1] - 0xDC00u);
                        } else {
                            cp = (uint32_t)w[0];
                        }
                        if (isValidUnicodeScalar(cp)) {
                            outCp = cp;
                            return true;
                        }
                    }

                    try {
                        size_t idx = 0;
                        unsigned long v = std::stoul(s, &idx, 16);
                        if (idx == 0) return false;
                        if (!isValidUnicodeScalar((uint32_t)v)) return false;
                        outCp = (uint32_t)v;
                        return true;
                    } catch (...) {
                        return false;
                    }
                };

                auto syncUnicodeEditBuffers = [&]() {
                    if (s_rebindUnicodeTextEdit.size() != g_config.keyRebinds.rebinds.size()) {
                        s_rebindUnicodeTextEdit.resize(g_config.keyRebinds.rebinds.size());
                    }
                };

                constexpr float kDefaultKeyboardLayoutScale = 1.35f;
                constexpr int kKeyboardLayoutCursorStateViewAny = 0;
                constexpr int kKeyboardLayoutCursorStateViewCursorFree = 1;
                constexpr int kKeyboardLayoutCursorStateViewCursorGrabbed = 2;
                static bool s_keyboardLayoutOpen = false;
                static float s_keyboardLayoutScale = kDefaultKeyboardLayoutScale;
                static int s_physicalLayout = 0;
                static int s_keyLabelLayout = 0;
                static int s_keyboardLayoutCursorStateView = kKeyboardLayoutCursorStateViewAny;
                static bool s_layoutAddCustomKeyPending = false;
                static uint64_t s_layoutAddCustomKeyLastSeq = 0;
                static bool s_layoutCustomInputCapturePending = false;
                static uint64_t s_layoutCustomInputCaptureLastSeq = 0;
                static DWORD s_layoutPendingCustomKeyRemovalVk = 0;
                static int s_layoutPendingCustomKeyRemovalRebindCount = 0;
                static bool s_layoutEscapeRequiresRelease = false;
                static bool s_layoutContextSplitMode = false;
                static bool s_layoutContextPopupWasOpenLastFrame = false;
                static bool s_keyboardLayoutWasOpenLastFrame = false;
                static ImVec2 s_lastKeyboardLayoutWindowSize = ImVec2(-1.0f, -1.0f);
                static float s_lastKeyboardLayoutGuiScaleFactor = -1.0f;
                static DWORD s_layoutContextVk = 0;
                static int s_layoutContextPreferredIndex = -1;
                static bool s_layoutContextOpenedFromCustomKey = false;
#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
                if (ConsumeGuiTestOpenKeyboardLayoutRequest()) {
                    s_keyboardLayoutOpen = true;
                }
#endif
                if (s_layoutEscapeRequiresRelease && !ImGui::IsKeyDown(ImGuiKey_Escape)) {
                    s_layoutEscapeRequiresRelease = false;
                }

                if (ImGui::Button(trc("inputs.open_keyboard_layout"))) { s_keyboardLayoutOpen = true; }
#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
                RecordGuiTestInteractionRect("inputs.open_keyboard_layout", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
#endif
                ImGui::SameLine();
                HelpMarker(trc("inputs.tooltip.open_keyboard_layout"));

                if (s_keyboardLayoutOpen) {
                    const ImGuiViewport* vp = ImGui::GetMainViewport();
                    const ImVec2 workSize = vp ? vp->WorkSize : ImVec2(1600.0f, 900.0f);
                    ImVec2 target = ImVec2(workSize.x * 0.92f, workSize.y * 0.88f);
                    if (!vp || workSize.x >= 1100.0f) target.x = (std::max)(target.x, 1100.0f);
                    if (!vp || workSize.y >= 680.0f) target.y = (std::max)(target.y, 680.0f);
                    if (vp) {
                        const float maxTargetW = (workSize.x > 32.0f) ? (workSize.x - 32.0f) : workSize.x;
                        const float maxTargetH = (workSize.y > 32.0f) ? (workSize.y - 32.0f) : workSize.y;
                        if (target.x > maxTargetW) target.x = maxTargetW;
                        if (target.y > maxTargetH) target.y = maxTargetH;
                    }
                    ImGui::SetNextWindowSize(target, ImGuiCond_Appearing);
                    const ImVec2 center = vp ? ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f)
                                              : ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f);
                    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
                }

                auto isScrollWheelVk = [](DWORD vk) -> bool {
                    return vk == VK_TOOLSCREEN_SCROLL_UP || vk == VK_TOOLSCREEN_SCROLL_DOWN;
                };

                auto canAcceptTypesVkCaptureFor = [&](const KeyRebind* rb, DWORD originalVk, DWORD capturedVk) -> bool {
                    if (capturedVk != 0) {
                        KeyRebind capturedCandidate;
                        capturedCandidate.enabled = true;
                        capturedCandidate.fromKey = capturedVk;
                        if (DoesKeyRebindTriggerCannotType(capturedCandidate)) return false;
                    }

                    KeyRebind candidate;
                    if (rb != nullptr) {
                        candidate = *rb;
                    }
                    if (candidate.fromKey == 0) {
                        candidate.fromKey = originalVk;
                        candidate.enabled = true;
                    }

                    return !DoesKeyRebindTriggerCannotType(candidate);
                };

                auto normalizeModifierVkForDisplay = [](DWORD vk, DWORD scanCodeWithFlags) -> DWORD {
                    const DWORD scanLow = (scanCodeWithFlags & 0xFF);
                    const bool isExtended = (scanCodeWithFlags & 0xFF00) != 0;

                    switch (vk) {
                    case VK_SHIFT:
                        if (scanLow == 0x36) return VK_RSHIFT;
                        if (scanLow == 0x2A) return VK_LSHIFT;
                        return vk;
                    case VK_CONTROL:
                        return isExtended ? VK_RCONTROL : VK_LCONTROL;
                    case VK_MENU:
                        return isExtended ? VK_RMENU : VK_LMENU;
                    default:
                        return vk;
                    }
                };

                auto usesSpecificModifierDisplayName = [](DWORD vk) -> bool {
                    return vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_LMENU ||
                           vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN;
                };

                auto scanCodeToDisplayName = [&](DWORD scan, DWORD fallbackVk) -> std::string {
                    const DWORD normalizedFallbackVk = normalizeModifierVkForDisplay(fallbackVk, scan);

                    if (fallbackVk == VK_LBUTTON || fallbackVk == VK_RBUTTON || fallbackVk == VK_MBUTTON || fallbackVk == VK_XBUTTON1 ||
                        fallbackVk == VK_XBUTTON2 || isScrollWheelVk(fallbackVk)) {
                        return VkToString(fallbackVk);
                    }

                    if (scan == 0) {
                        if (usesSpecificModifierDisplayName(normalizedFallbackVk)) {
                            return VkToString(normalizedFallbackVk);
                        }
                        return std::string(tr("inputs.keyboard_layout_unbound"));
                    }

                    const DWORD scanLow = (scan & 0xFF);
                    if (scanLow == 0x45) {
                        if (fallbackVk == VK_NUMLOCK) return VkToString(VK_NUMLOCK);
                        if (fallbackVk == VK_PAUSE) return VkToString(VK_PAUSE);
                    }

                    LONG keyNameLParam = static_cast<LONG>((scan & 0xFF) << 16);
                    if ((scan & 0xFF00) != 0) { keyNameLParam |= (1 << 24); }
                    if (usesSpecificModifierDisplayName(normalizedFallbackVk)) {
                        return VkToString(normalizedFallbackVk);
                    }
                    char keyName[64] = {};
                    if (GetKeyNameTextA(keyNameLParam, keyName, sizeof(keyName)) > 0) { return std::string(keyName); }

                    DWORD scanDisplayVK = MapVirtualKey(scan, MAPVK_VSC_TO_VK_EX);
                    scanDisplayVK = normalizeModifierVkForDisplay(scanDisplayVK, scan);
                    if (scanDisplayVK != 0) {
                        if (scanLow == 0x45 && (fallbackVk == VK_NUMLOCK || fallbackVk == VK_PAUSE) &&
                            (scanDisplayVK == VK_NUMLOCK || scanDisplayVK == VK_PAUSE) && scanDisplayVK != fallbackVk) {
                            scanDisplayVK = fallbackVk;
                        }
                        return VkToString(scanDisplayVK);
                    }
                    return std::string(tr("inputs.keyboard_layout_unknown"));
                };

                auto getKeyboardLayoutCursorStateViewIdFor = [&](int view) -> const char* {
                    switch (view) {
                    case kKeyboardLayoutCursorStateViewCursorFree:
                        return kKeyRebindCursorStateCursorFree;
                    case kKeyboardLayoutCursorStateViewCursorGrabbed:
                        return kKeyRebindCursorStateCursorGrabbed;
                    case kKeyboardLayoutCursorStateViewAny:
                    default:
                        return kKeyRebindCursorStateAny;
                    }
                };

                auto getKeyboardLayoutCursorStateViewId = [&]() -> const char* { return getKeyboardLayoutCursorStateViewIdFor(s_keyboardLayoutCursorStateView); };

                auto getKeyboardLayoutCursorStateViewLabel = [&](const char* cursorStateId) -> const char* {
                    if (cursorStateId == nullptr) {
                        return trc("label.default");
                    }
                    if (strcmp(cursorStateId, kKeyRebindCursorStateCursorFree) == 0) {
                        return trc("inputs.rebind_layout_cursor_free");
                    }
                    if (strcmp(cursorStateId, kKeyRebindCursorStateCursorGrabbed) == 0) {
                        return trc("inputs.rebind_layout_cursor_grabbed");
                    }
                    return trc("label.default");
                };

                auto getKeyboardLayoutCursorStateViewTooltip = [&](int view) -> const char* {
                    switch (view) {
                    case kKeyboardLayoutCursorStateViewCursorFree:
                        return trc("inputs.tooltip.rebind_layout_cursor_free_button");
                    case kKeyboardLayoutCursorStateViewCursorGrabbed:
                        return trc("inputs.tooltip.rebind_layout_cursor_grabbed_button");
                    case kKeyboardLayoutCursorStateViewAny:
                    default:
                        return trc("inputs.tooltip.rebind_layout_default_button");
                    }
                };

                auto drawKeyboardLayoutCursorStateButton = [&](const char* id, int view) {
                    const bool selected = s_keyboardLayoutCursorStateView == view;
                    const float buttonSize = ImGui::GetFrameHeight() * 1.08f;
                    const ImGuiStyle& style = ImGui::GetStyle();

                    ImGui::PushID(id);
                    ImGui::InvisibleButton("##cursorStateLayoutButton", ImVec2(buttonSize, buttonSize));
                    const bool hovered = ImGui::IsItemHovered();
                    const bool held = ImGui::IsItemActive();
                    if (ImGui::IsItemClicked()) {
                        s_keyboardLayoutCursorStateView = view;
                    }

                    const ImVec2 buttonMin = ImGui::GetItemRectMin();
                    const ImVec2 buttonMax = ImGui::GetItemRectMax();
                    const float width = buttonMax.x - buttonMin.x;
                    const float height = buttonMax.y - buttonMin.y;
                    const float rounding = 6.0f;
                    const float stroke = selected ? 2.0f : 1.6f;
                    ImDrawList* drawList = ImGui::GetWindowDrawList();

                    ImVec4 fillColor = style.Colors[ImGuiCol_FrameBg];
                    if (selected) {
                        fillColor = style.Colors[ImGuiCol_ButtonActive];
                    } else if (held) {
                        fillColor = style.Colors[ImGuiCol_ButtonActive];
                    } else if (hovered) {
                        fillColor = style.Colors[ImGuiCol_ButtonHovered];
                    }

                    ImVec4 borderColor = selected ? style.Colors[ImGuiCol_CheckMark]
                                                  : (hovered ? style.Colors[ImGuiCol_ButtonHovered] : style.Colors[ImGuiCol_Border]);
                    ImVec4 iconColor = selected ? style.Colors[ImGuiCol_Text]
                                                : (hovered ? style.Colors[ImGuiCol_Text] : style.Colors[ImGuiCol_TextDisabled]);

                    drawList->AddRectFilled(buttonMin, buttonMax, ImGui::ColorConvertFloat4ToU32(fillColor), rounding);
                    drawList->AddRect(buttonMin, buttonMax, ImGui::ColorConvertFloat4ToU32(borderColor), rounding, 0,
                                      selected ? 2.0f : 1.0f);

                    const float inset = width * 0.22f;
                    const float centerX = buttonMin.x + width * 0.5f;
                    const float centerY = buttonMin.y + height * 0.5f;
                    const ImU32 iconColorU32 = ImGui::ColorConvertFloat4ToU32(iconColor);

                    if (view == kKeyboardLayoutCursorStateViewAny) {
                        const float barHeight = height * 0.11f;
                        const float barGap = height * 0.11f;
                        const float left = buttonMin.x + inset;
                        const float right = buttonMax.x - inset;
                        for (int barIndex = 0; barIndex < 3; ++barIndex) {
                            const float y = buttonMin.y + height * 0.25f + barIndex * (barHeight + barGap);
                            drawList->AddRectFilled(ImVec2(left, y), ImVec2(right, y + barHeight), iconColorU32, barHeight * 0.45f);
                        }
                    } else if (view == kKeyboardLayoutCursorStateViewCursorFree) {
                        const CursorTextures::CursorData* cursorData = CursorTextures::LoadOrFindSystemCursor(IDC_ARROW);
                        if (cursorData && cursorData->texture != 0 && cursorData->bitmapWidth > 0 && cursorData->bitmapHeight > 0) {
                            const int contentLeft = cursorData->contentRight > cursorData->contentLeft ? cursorData->contentLeft : 0;
                            const int contentTop = cursorData->contentBottom > cursorData->contentTop ? cursorData->contentTop : 0;
                            const int contentRight = cursorData->contentRight > cursorData->contentLeft ? cursorData->contentRight
                                                                                                           : cursorData->bitmapWidth;
                            const int contentBottom = cursorData->contentBottom > cursorData->contentTop ? cursorData->contentBottom
                                                                                                           : cursorData->bitmapHeight;
                            const float contentWidth = static_cast<float>((std::max)(1, contentRight - contentLeft));
                            const float contentHeight = static_cast<float>((std::max)(1, contentBottom - contentTop));
                            const float maxImageWidth = width;
                            const float maxImageHeight = height * 0.8f;
                            const float imageScale = (std::min)(maxImageWidth / contentWidth, maxImageHeight / contentHeight);
                            const float drawWidth = floorf(contentWidth * imageScale);
                            const float drawHeight = floorf(contentHeight * imageScale);
                            const ImVec2 imageMin(floorf(buttonMin.x + (width - drawWidth) * 0.5f),
                                                  floorf(buttonMin.y + (height - drawHeight) * 0.5f));
                            const ImVec2 imageMax(imageMin.x + drawWidth, imageMin.y + drawHeight);
                            const ImVec2 uvMin(static_cast<float>(contentLeft) / static_cast<float>(cursorData->bitmapWidth),
                                               static_cast<float>(contentTop) / static_cast<float>(cursorData->bitmapHeight));
                            const ImVec2 uvMax(static_cast<float>(contentRight) / static_cast<float>(cursorData->bitmapWidth),
                                               static_cast<float>(contentBottom) / static_cast<float>(cursorData->bitmapHeight));
                            const float imageAlpha = selected ? 1.0f : (hovered ? 0.96f : 0.88f);
                            drawList->AddImage((ImTextureID)(intptr_t)cursorData->texture, imageMin, imageMax, uvMin, uvMax,
                                               ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, imageAlpha)));
                        } else {
                            const ImVec2 tip(buttonMin.x + width * 0.34f, buttonMin.y + height * 0.28f);
                            const ImVec2 tail(buttonMin.x + width * 0.62f, buttonMin.y + height * 0.68f);
                            drawList->AddLine(tip, tail, iconColorU32, stroke);
                            drawList->AddLine(tip, ImVec2(buttonMin.x + width * 0.34f, buttonMin.y + height * 0.58f), iconColorU32, stroke);
                            drawList->AddLine(tip, ImVec2(buttonMin.x + width * 0.54f, buttonMin.y + height * 0.42f), iconColorU32, stroke);
                        }
                    } else {
                        const float crossHalfWidth = width * 0.35f;
                        const float crossHalfHeight = height * 0.35f;
                        drawList->AddLine(ImVec2(centerX - crossHalfWidth, centerY), ImVec2(centerX + crossHalfWidth, centerY), iconColorU32,
                                          stroke);
                        drawList->AddLine(ImVec2(centerX, centerY - crossHalfHeight), ImVec2(centerX, centerY + crossHalfHeight), iconColorU32,
                                          stroke);
                    }

                    if (selected) {
                        drawList->AddLine(ImVec2(buttonMin.x + 4.0f, buttonMax.y - 2.0f), ImVec2(buttonMax.x - 4.0f, buttonMax.y - 2.0f),
                                          iconColorU32, 2.0f);
                    }

                    if (hovered) {
                        ImGui::SetTooltip("%s", getKeyboardLayoutCursorStateViewTooltip(view));
                    }

                    ImGui::PopID();
                };

                auto findBestRebindIndexForCursorState = [&](DWORD fromVk, const char* cursorStateId) -> int {
                    int first = -1;
                    int enabledAny = -1;
                    int enabledConfigured = -1;
                    int configuredAny = -1;

                    for (int ri = 0; ri < (int)g_config.keyRebinds.rebinds.size(); ++ri) {
                        const auto& r = g_config.keyRebinds.rebinds[ri];
                        if (r.fromKey != fromVk || r.cursorState != cursorStateId) continue;
                        if (first == -1) first = ri;

                        const bool configured = (r.fromKey != 0 && r.toKey != 0);
                        if (configured && configuredAny == -1) configuredAny = ri;
                        if (r.enabled && enabledAny == -1) enabledAny = ri;
                        if (r.enabled && configured) {
                            enabledConfigured = ri;
                            break;
                        }
                    }

                    if (enabledConfigured != -1) return enabledConfigured;
                    if (configuredAny != -1) return configuredAny;
                    if (enabledAny != -1) return enabledAny;
                    return first;
                };

                ImGui::SetNextWindowBgAlpha(1.0f);
                if (s_keyboardLayoutOpen) {
                    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(18, 19, 22, 255));
                    const bool keyboardLayoutWindowVisible =
                        ImGui::Begin(trc("inputs.keyboard_layout"), &s_keyboardLayoutOpen,
                                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoCollapse);
                    if (keyboardLayoutWindowVisible) {
                    MarkRebindBindingActive();
                    const bool keyboardLayoutOpenedThisFrame = ImGui::IsWindowAppearing() || !s_keyboardLayoutWasOpenLastFrame;
                    const ImVec2 keyboardLayoutWindowSize = ImGui::GetWindowSize();
                    const float keyboardLayoutGuiScaleFactor = ComputeGuiScaleFactorFromCachedWindowSize();
                    const bool keyboardLayoutWindowResized = fabsf(keyboardLayoutWindowSize.x - s_lastKeyboardLayoutWindowSize.x) > 0.5f ||
                                                           fabsf(keyboardLayoutWindowSize.y - s_lastKeyboardLayoutWindowSize.y) > 0.5f;
                    const bool keyboardLayoutGuiScaleChanged = s_lastKeyboardLayoutGuiScaleFactor < 0.0f ||
                                                               fabsf(keyboardLayoutGuiScaleFactor - s_lastKeyboardLayoutGuiScaleFactor) > 0.001f;
                    bool keyboardLayoutScaleChanged = false;
                    const bool escapePressedThisFrame = ImGui::IsKeyPressed(ImGuiKey_Escape);
                    bool layoutEscapeConsumedThisFrame = false;

                    const bool anyPopupOpenNow = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup);
                    const bool contextPopupOpenNow = ImGui::IsPopupOpen(trc("inputs.rebind_config")) || ImGui::IsPopupOpen(trc("inputs.triggers_custom")) ||
                                                     ImGui::IsPopupOpen(trc("inputs.custom_unicode"));
                    bool blockLayoutEscapeThisFrame = false;
                    if (escapePressedThisFrame && (anyPopupOpenNow || contextPopupOpenNow || s_layoutContextPopupWasOpenLastFrame)) {
                        s_layoutEscapeRequiresRelease = true;
                        blockLayoutEscapeThisFrame = true;
                        layoutEscapeConsumedThisFrame = true;
                    }

                    if (s_layoutEscapeRequiresRelease) {
                        blockLayoutEscapeThisFrame = true;
                        layoutEscapeConsumedThisFrame = true;
                    }

                    {
                        float scalePct = s_keyboardLayoutScale * 100.0f;
                        ImGui::TextUnformatted(trc("inputs.keyboard_layout_scale"));
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(220.0f);
                        if (ImGui::SliderFloat("##keyboardLayoutScalePct", &scalePct, 60.0f, 300.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
                            s_keyboardLayoutScale = scalePct / 100.0f;
                            keyboardLayoutScaleChanged = true;
                        }
                        ImGui::SameLine();
                        HelpMarker(trc("inputs.tooltip.keyboard_layout_scale"));

                        ImGui::SameLine();
                        ImGui::TextUnformatted(trc("inputs.physical_layout"));
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(100.0f);
                        {
                            const char* physicalLayoutItems[] = { trc("label.auto"), "ANSI", "ISO" };
                            ImGui::Combo("##physicalLayout", &s_physicalLayout, physicalLayoutItems, IM_ARRAYSIZE(physicalLayoutItems));
                        }
                        ImGui::SameLine();
                        HelpMarker(trc("inputs.tooltip.physical_layout"));

                        ImGui::SameLine();
                        ImGui::TextUnformatted(trc("inputs.key_labels"));
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(120.0f);
                        {
                            const char* keyLabelItems[] = { trc("label.system"), "QWERTY" };
                            ImGui::Combo("##keyLabelLayout", &s_keyLabelLayout, keyLabelItems, IM_ARRAYSIZE(keyLabelItems));
                        }
                        ImGui::SameLine();
                        HelpMarker(trc("inputs.tooltip.key_labels"));

#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
                        if (const int requestedCursorStateView = ConsumeGuiTestKeyboardLayoutCursorStateViewRequest();
                            requestedCursorStateView >= kKeyboardLayoutCursorStateViewAny &&
                            requestedCursorStateView <= kKeyboardLayoutCursorStateViewCursorGrabbed) {
                            s_keyboardLayoutCursorStateView = requestedCursorStateView;
                        }
#endif

                        ImGui::TextUnformatted(trc("inputs.rebind_layout_state"));
                        ImGui::SameLine();
                        drawKeyboardLayoutCursorStateButton("default", kKeyboardLayoutCursorStateViewAny);
                        ImGui::SameLine(0.0f, 6.0f);
                        drawKeyboardLayoutCursorStateButton("cursorFree", kKeyboardLayoutCursorStateViewCursorFree);
                        ImGui::SameLine(0.0f, 6.0f);
                        drawKeyboardLayoutCursorStateButton("cursorGrabbed", kKeyboardLayoutCursorStateViewCursorGrabbed);
                        ImGui::SameLine();
                        HelpMarker(trc("inputs.tooltip.rebind_layout_state"));
                        ImGui::SameLine();
                        ImGui::TextDisabled("%s", getKeyboardLayoutCursorStateViewLabel(getKeyboardLayoutCursorStateViewId()));

                        ImGui::TextDisabled(trc("inputs.keyboard_layout_state_note"));
                        ImGui::TextDisabled(trc("inputs.keyboard_layout_tip"));
                        ImGui::TextDisabled(trc("label.not_all_rebinds_supported"));

                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();
                    }

                    // Make the keyboard region scrollable (both axes) to fit on smaller windows.
                    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(14, 15, 18, 255));
                    ImGui::BeginChild("##keyboardLayoutChild", ImVec2(0, 0), true,
                                     ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_AlwaysVerticalScrollbar);

                    struct KeyCell {
                        DWORD vk;
                        const char* labelOverride;
                        float w;
                    };

                    auto Spacer = [](float wUnits) -> KeyCell { return KeyCell{ 0, nullptr, wUnits }; };
                    auto Key = [](DWORD vk, float wUnits = 1.0f, const char* overrideLabel = nullptr) -> KeyCell {
                        return KeyCell{ vk, overrideLabel, wUnits };
                    };

                    static HKL s_cachedHkl = nullptr;
                    static bool s_cachedUseISO = false;
                    static std::unordered_map<DWORD, DWORD> s_cachedVkSwap;
                    static int s_cachedPhysicalLayout = -1;
                    static int s_cachedKeyLabelLayout = -1;

                    HKL currentHkl = GetKeyboardLayout(0);
                    const bool layoutChanged = currentHkl != s_cachedHkl ||
                                               s_physicalLayout != s_cachedPhysicalLayout ||
                                               s_keyLabelLayout != s_cachedKeyLabelLayout;
                    if (layoutChanged) {
                        s_cachedHkl = currentHkl;
                        s_cachedPhysicalLayout = s_physicalLayout;
                        s_cachedKeyLabelLayout = s_keyLabelLayout;

                        if (s_physicalLayout == 1) s_cachedUseISO = false;
                        else if (s_physicalLayout == 2) s_cachedUseISO = true;
                        else {
                            WORD langId = LOWORD((DWORD_PTR)currentHkl) & 0xFF;
                            s_cachedUseISO = langId != LANG_ENGLISH;
                        }

                        s_cachedVkSwap.clear();
                        if (s_keyLabelLayout != 1) {
                            static const UINT remappableScans[] = {
                                0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,
                                0x1E,0x1F,0x20,0x21,0x22,0x23,0x24,0x25,0x26,
                                0x2C,0x2D,0x2E,0x2F,0x30,0x31,0x32,
                                0x29,0x0C,0x0D,0x1A,0x1B,0x2B,0x27,0x28,
                                0x33,0x34,0x35,0x56,
                            };
                            static const DWORD qwertyVks[] = {
                                'Q','W','E','R','T','Y','U','I','O','P',
                                'A','S','D','F','G','H','J','K','L',
                                'Z','X','C','V','B','N','M',
                                VK_OEM_3,VK_OEM_MINUS,VK_OEM_PLUS,VK_OEM_4,VK_OEM_6,VK_OEM_5,VK_OEM_1,VK_OEM_7,
                                VK_OEM_COMMA,VK_OEM_PERIOD,VK_OEM_2,VK_OEM_102,
                            };
                            for (int i = 0; i < sizeof(remappableScans)/sizeof(remappableScans[0]); ++i) {
                                DWORD mapped = (DWORD)MapVirtualKeyW(remappableScans[i], MAPVK_VSC_TO_VK);
                                if (mapped != 0 && mapped != qwertyVks[i]) {
                                    s_cachedVkSwap[qwertyVks[i]] = mapped;
                                }
                            }
                        }
                    }

                    static std::unordered_map<DWORD, std::string> s_cachedOemLabels;
                    if (layoutChanged) {
                        s_cachedOemLabels.clear();
                        if (s_keyLabelLayout != 1) {
                            static const DWORD oemVks[] = {
                                VK_OEM_1, VK_OEM_2, VK_OEM_3, VK_OEM_4, VK_OEM_5, VK_OEM_6, VK_OEM_7,
                                VK_OEM_PLUS, VK_OEM_MINUS, VK_OEM_COMMA, VK_OEM_PERIOD, VK_OEM_102,
                            };
                            for (DWORD vk : oemVks) {
                                DWORD resolvedVk = vk;
                                if (auto it = s_cachedVkSwap.find(vk); it != s_cachedVkSwap.end()) resolvedVk = it->second;
                                BYTE ks[256] = {};
                                wchar_t wbuf[4] = {};
                                UINT sc = MapVirtualKeyW(resolvedVk, MAPVK_VK_TO_VSC);
                                int ret = ToUnicodeEx(resolvedVk, sc, ks, wbuf, 4, 0, currentHkl);
                                if (ret < 0) { ToUnicodeEx(resolvedVk, sc, ks, wbuf, 4, 0, currentHkl); ret = 1; }
                                if (ret >= 1 && wbuf[0] >= 32) {
                                    char buf[8] = {};
                                    int len = WideCharToMultiByte(CP_UTF8, 0, wbuf, 1, buf, sizeof(buf) - 1, nullptr, nullptr);
                                    if (len > 0) s_cachedOemLabels[vk] = std::string(buf, len);
                                }
                            }
                        }
                    }

                    const bool useISO = s_cachedUseISO;
                    const auto& vkSwap = s_cachedVkSwap;
                    const auto& oemLabels = s_cachedOemLabels;

                    const std::vector<std::vector<KeyCell>> rows = {
                                                { Key(VK_ESCAPE), Spacer(1.0f), Key(VK_F1), Key(VK_F2), Key(VK_F3), Key(VK_F4), Spacer(0.5f), Key(VK_F5),
                          Key(VK_F6), Key(VK_F7), Key(VK_F8), Spacer(0.5f), Key(VK_F9), Key(VK_F10), Key(VK_F11), Key(VK_F12),
                                                    Spacer(0.5f), Key(VK_SNAPSHOT, 1.25f, "PRTSC"), Key(VK_SCROLL, 1.25f, "SCRLK"), Key(VK_PAUSE, 1.25f, "PAUSE") },

                        { Key(VK_OEM_3, 1.0f, "`") , Key('1'), Key('2'), Key('3'), Key('4'), Key('5'), Key('6'), Key('7'), Key('8'), Key('9'),
                          Key('0'), Key(VK_OEM_MINUS, 1.0f, "-"), Key(VK_OEM_PLUS, 1.0f, "="), Key(VK_BACK, 2.0f, "BACK"), Spacer(0.5f),
                                                    Key(VK_INSERT, 1.25f, "INS"), Key(VK_HOME, 1.25f, "HOME"), Key(VK_PRIOR, 1.25f, "PGUP"), Spacer(0.5f),
                                                    Key(VK_NUMLOCK, 1.25f, "NUMLK"), Key(VK_DIVIDE, 1.25f, "/"), Key(VK_MULTIPLY, 1.25f, "*"), Key(VK_SUBTRACT, 1.25f, "-") },

                        { Key(VK_TAB, 1.5f, "TAB"), Key('Q'), Key('W'), Key('E'), Key('R'), Key('T'), Key('Y'), Key('U'), Key('I'), Key('O'), Key('P'),
                          Key(VK_OEM_4, 1.0f, "["), Key(VK_OEM_6, 1.0f, "]"), Key(VK_OEM_5, 1.5f, "\\"), Spacer(0.5f),
                                                    Key(VK_DELETE, 1.25f, "DEL"), Key(VK_END, 1.25f, "END"), Key(VK_NEXT, 1.25f, "PGDN"), Spacer(0.5f),
                                                    Key(VK_NUMPAD7, 1.25f, "NUM7"), Key(VK_NUMPAD8, 1.25f, "NUM8"), Key(VK_NUMPAD9, 1.25f, "NUM9"), Key(VK_ADD, 1.25f, "+") },

                        { Key(VK_CAPITAL, 1.75f, "CAPS"), Key('A'), Key('S'), Key('D'), Key('F'), Key('G'), Key('H'), Key('J'), Key('K'), Key('L'),
                          Key(VK_OEM_1, 1.0f, ";"), Key(VK_OEM_7, 1.0f, "'"), Key(VK_RETURN, 2.25f, "ENTER"), Spacer(0.5f),
                                                                                                        Spacer(1.25f), Spacer(1.25f), Spacer(1.25f), Spacer(0.5f),
                                                                                                        Key(VK_NUMPAD4, 1.25f, "NUM4"), Key(VK_NUMPAD5, 1.25f, "NUM5"), Key(VK_NUMPAD6, 1.25f, "NUM6"),
                                                    Spacer(1.25f) },

                        useISO
                        ? std::vector<KeyCell>{ Key(VK_LSHIFT, 1.25f, "LSHIFT"), Key(VK_OEM_102, 1.0f), Key('Z'), Key('X'), Key('C'), Key('V'), Key('B'), Key('N'), Key('M'),
                          Key(VK_OEM_COMMA, 1.0f, ","), Key(VK_OEM_PERIOD, 1.0f, "."), Key(VK_OEM_2, 1.0f, "/"), Key(VK_RSHIFT, 2.75f, "RSHIFT"),
                                                    Spacer(0.5f), Spacer(1.25f), Key(VK_UP, 1.25f, "UP"), Spacer(1.25f), Spacer(0.5f),
                                                    Key(VK_NUMPAD1, 1.25f, "NUM1"), Key(VK_NUMPAD2, 1.25f, "NUM2"), Key(VK_NUMPAD3, 1.25f, "NUM3"), Key(VK_RETURN, 1.25f, "ENTER") }
                        : std::vector<KeyCell>{ Key(VK_LSHIFT, 2.25f, "LSHIFT"), Key('Z'), Key('X'), Key('C'), Key('V'), Key('B'), Key('N'), Key('M'),
                          Key(VK_OEM_COMMA, 1.0f, ","), Key(VK_OEM_PERIOD, 1.0f, "."), Key(VK_OEM_2, 1.0f, "/"), Key(VK_RSHIFT, 2.75f, "RSHIFT"),
                                                    Spacer(0.5f), Spacer(1.25f), Key(VK_UP, 1.25f, "UP"), Spacer(1.25f), Spacer(0.5f),
                                                    Key(VK_NUMPAD1, 1.25f, "NUM1"), Key(VK_NUMPAD2, 1.25f, "NUM2"), Key(VK_NUMPAD3, 1.25f, "NUM3"), Key(VK_RETURN, 1.25f, "ENTER") },

                                                { Key(VK_LCONTROL, 1.25f, "LCTRL"), Key(VK_LWIN, 1.25f, "LWIN"), Key(VK_LMENU, 1.25f, "LALT"),
                                                    Key(VK_SPACE, 6.25f, "SPACE"), Key(VK_RMENU, 1.25f, "RALT"), Key(VK_RWIN, 1.25f, "RWIN"), Key(VK_APPS, 1.25f, "APPS"),
                                                    Key(VK_RCONTROL, 1.25f, "RCTRL"), Spacer(0.5f), Key(VK_LEFT, 1.25f, "LEFT"), Key(VK_DOWN, 1.25f, "DOWN"), Key(VK_RIGHT, 1.25f, "RIGHT"),
                                                    Spacer(0.5f), Key(VK_NUMPAD0, 2.5f, "NUM0"), Key(VK_DECIMAL, 1.25f, "NUM."), Spacer(1.25f) },
                    };

                    auto containsLayoutSourceKey = [](const std::vector<DWORD>& keys, DWORD vk) {
                        return std::find(keys.begin(), keys.end(), vk) != keys.end();
                    };

                    std::vector<DWORD> builtInLayoutKeys;
                    builtInLayoutKeys.reserve(96);
                    auto appendBuiltInLayoutKey = [&](DWORD sourceVk) {
                        if (sourceVk == 0 || containsLayoutSourceKey(builtInLayoutKeys, sourceVk)) return;
                        builtInLayoutKeys.push_back(sourceVk);
                    };

                    for (const auto& row : rows) {
                        for (const KeyCell& cell : row) {
                            if (cell.vk == 0) continue;
                            DWORD displayVk = cell.vk;
                            if (auto it = vkSwap.find(cell.vk); it != vkSwap.end()) {
                                displayVk = it->second;
                            }
                            appendBuiltInLayoutKey(displayVk);
                        }
                    }
                    appendBuiltInLayoutKey(VK_LBUTTON);
                    appendBuiltInLayoutKey(VK_RBUTTON);
                    appendBuiltInLayoutKey(VK_MBUTTON);
                    appendBuiltInLayoutKey(VK_XBUTTON1);
                    appendBuiltInLayoutKey(VK_XBUTTON2);
                    appendBuiltInLayoutKey(VK_TOOLSCREEN_SCROLL_UP);
                    appendBuiltInLayoutKey(VK_TOOLSCREEN_SCROLL_DOWN);

                    auto collectCustomLayoutKeys = [&]() {
                        std::vector<DWORD> customLayoutKeys;
                        customLayoutKeys.reserve(g_config.keyRebinds.layoutExtraKeys.size() + g_config.keyRebinds.rebinds.size());

                        auto appendStoredCustomLayoutKey = [&](DWORD vk) {
                            if (vk == 0 || containsLayoutSourceKey(customLayoutKeys, vk)) return;
                            customLayoutKeys.push_back(vk);
                        };

                        auto appendFallbackCustomLayoutKey = [&](DWORD vk) {
                            if (vk == 0 || containsLayoutSourceKey(builtInLayoutKeys, vk) || containsLayoutSourceKey(customLayoutKeys, vk)) return;
                            customLayoutKeys.push_back(vk);
                        };

                        for (DWORD vk : g_config.keyRebinds.layoutExtraKeys) {
                            appendStoredCustomLayoutKey(vk);
                        }

                        for (const auto& rebind : g_config.keyRebinds.rebinds) {
                            appendFallbackCustomLayoutKey(rebind.fromKey);
                        }

                        return customLayoutKeys;
                    };

                    auto findRebindForKey = [&](DWORD fromVk) -> const KeyRebind* {
                        const int idx = findBestRebindIndexForCursorState(fromVk, getKeyboardLayoutCursorStateViewId());
                        if (idx < 0 || idx >= (int)g_config.keyRebinds.rebinds.size()) {
                            return nullptr;
                        }
                        return &g_config.keyRebinds.rebinds[idx];
                    };

                    auto adjustTrackedRebindIndicesAfterErase = [&](int eraseIdx) {
                        auto decIfAfter = [&](int& value) {
                            if (value == -1) return;
                            if (value == eraseIdx) {
                                value = -1;
                            } else if (value > eraseIdx) {
                                value -= 1;
                            }
                        };

                        decIfAfter(s_rebindFromKeyToBind);
                        decIfAfter(s_rebindOutputVKToBind);
                        decIfAfter(s_rebindTextOverrideVKToBind);
                        decIfAfter(s_rebindOutputScanToBind);
                        decIfAfter(s_layoutBindIndex);
                        decIfAfter(s_layoutContextPreferredIndex);
                        decIfAfter(s_layoutUnicodeEditIndex);

                        if (s_layoutBindIndex == -1) {
                            s_layoutBindTarget = LayoutBindTarget::None;
                        }
                        if (s_layoutUnicodeEditIndex == -1) {
                            s_layoutUnicodeEditTarget = LayoutUnicodeEditTarget::None;
                            s_layoutUnicodeEditText.clear();
                        }
                    };

                    auto countRebindsForSourceKey = [&](DWORD sourceVk) -> int {
                        int count = 0;
                        for (const auto& rebind : g_config.keyRebinds.rebinds) {
                            if (rebind.fromKey == sourceVk) {
                                ++count;
                            }
                        }
                        return count;
                    };

                    auto removeCustomLayoutKeyOnly = [&](DWORD sourceVk) {
                        bool removedLayoutKey = false;
                        auto layoutExtraKeyIt = std::remove(g_config.keyRebinds.layoutExtraKeys.begin(),
                                                            g_config.keyRebinds.layoutExtraKeys.end(), sourceVk);
                        if (layoutExtraKeyIt != g_config.keyRebinds.layoutExtraKeys.end()) {
                            g_config.keyRebinds.layoutExtraKeys.erase(layoutExtraKeyIt, g_config.keyRebinds.layoutExtraKeys.end());
                            removedLayoutKey = true;
                        }

                        if (removedLayoutKey) {
                            g_configIsDirty = true;
                        }

                        return removedLayoutKey;
                    };

                    auto removeCustomLayoutKeyAndRebinds = [&](DWORD sourceVk) {
                        bool removedLayoutKey = removeCustomLayoutKeyOnly(sourceVk);

                        int removedRebindCount = 0;
                        for (int rebindIndex = (int)g_config.keyRebinds.rebinds.size() - 1; rebindIndex >= 0; --rebindIndex) {
                            if (g_config.keyRebinds.rebinds[rebindIndex].fromKey != sourceVk) continue;
                            g_config.keyRebinds.rebinds.erase(g_config.keyRebinds.rebinds.begin() + rebindIndex);
                            adjustTrackedRebindIndicesAfterErase(rebindIndex);
                            ++removedRebindCount;
                        }

                        if (!removedLayoutKey && removedRebindCount == 0) {
                            return;
                        }

                        if (s_layoutContextVk == sourceVk) {
                            s_layoutContextVk = 0;
                            s_layoutContextPreferredIndex = -1;
                            s_layoutContextOpenedFromCustomKey = false;
                        }

                        if (s_layoutPendingCustomKeyRemovalVk == sourceVk) {
                            s_layoutPendingCustomKeyRemovalVk = 0;
                            s_layoutPendingCustomKeyRemovalRebindCount = 0;
                        }

                        syncUnicodeEditBuffers();
                        g_configIsDirty = true;
                        if (removedRebindCount > 0) {
                            std::lock_guard<std::mutex> hotkeyLock(g_hotkeyMainKeysMutex);
                            RebuildHotkeyMainKeys_Internal();
                        }
                    };

                    auto roundPx = [](float v) -> float { return (float)(int)(v + 0.5f); };

                    constexpr float kKeyboardScaleMult = 1.0f;
                    constexpr float kKeyHeightMul = 1.68f;
                    constexpr float kKeyUnitMul = 0.96f;
                    constexpr float kKeyGapMul = 1.08f;
                    constexpr float kKeyCapInsetXMul = 0.55f;
                    constexpr float kKeyCapInsetYMul = 0.45f;
                    constexpr float kKeyRoundingPx = 5.0f;

                    float keyboardScale = s_keyboardLayoutScale * kKeyboardScaleMult;

                    float keyH = roundPx(ImGui::GetFrameHeight() * kKeyHeightMul * keyboardScale);
                    float unit = roundPx(keyH * kKeyUnitMul);
                    unit = (float)(((int)(unit + 2.0f) / 4) * 4);
                    if (unit < 20.0f) unit = 20.0f;
                    float gap = roundPx(ImGui::GetStyle().ItemInnerSpacing.x * keyboardScale * kKeyGapMul);
                    if (gap < 1.0f) gap = 1.0f;
                    if (keyboardLayoutOpenedThisFrame || keyboardLayoutWindowResized || keyboardLayoutScaleChanged ||
                        keyboardLayoutGuiScaleChanged) {
                        RequestKeyboardLayoutFontRefresh(keyboardLayoutWindowSize, keyH, keyboardScale,
                                                         keyboardLayoutOpenedThisFrame || keyboardLayoutScaleChanged);
                        s_lastKeyboardLayoutWindowSize = keyboardLayoutWindowSize;
                    }
                    s_lastKeyboardLayoutGuiScaleFactor = keyboardLayoutGuiScaleFactor;
                    const float rounding = roundPx(kKeyRoundingPx * keyboardScale);
                    ImDrawList* dl = ImGui::GetWindowDrawList();

                    const float pitchX = unit;
                    const float keyPadX = roundPx(gap * kKeyCapInsetXMul);
                    const float keyPadY = roundPx(gap * kKeyCapInsetYMul);

                    float keyboardMaxRowW = 0.0f;
                    for (const auto& row : rows) {
                        float w = 0.0f;
                        for (size_t c = 0; c < row.size(); ++c) {
                            w += row[c].w * pitchX;
                        }
                        if (w > keyboardMaxRowW) keyboardMaxRowW = w;
                    }

                    if (keyboardLayoutOpenedThisFrame && keyboardMaxRowW > 0.0f) {
                        const float mousePanelExtra = unit * 4.5f;
                        const float totalLayoutW = keyboardMaxRowW + mousePanelExtra;
                        const float platePad = 10.0f * s_keyboardLayoutScale;
                        const float availW = ImGui::GetContentRegionAvail().x - platePad * 2.0f;
                        if (availW > 0.0f && totalLayoutW > 0.0f) {
                            float fitScale = s_keyboardLayoutScale;
                            if (totalLayoutW > availW) {
                                fitScale = s_keyboardLayoutScale * (availW / totalLayoutW);
                            }
                            fitScale = std::clamp(fitScale, 0.6f, 3.0f);
                            if (fabsf(fitScale - s_keyboardLayoutScale) > 0.01f) {
                                s_keyboardLayoutScale = fitScale;
                                keyboardLayoutScaleChanged = true;

                                const float ks2 = s_keyboardLayoutScale * kKeyboardScaleMult;
                                const float kH2 = roundPx(ImGui::GetFrameHeight() * kKeyHeightMul * ks2);
                                float u2 = roundPx(kH2 * kKeyUnitMul);
                                u2 = (float)(((int)(u2 + 2.0f) / 4) * 4);
                                if (u2 < 20.0f) u2 = 20.0f;
                                float g2 = roundPx(ImGui::GetStyle().ItemInnerSpacing.x * ks2 * kKeyGapMul);
                                if (g2 < 1.0f) g2 = 1.0f;
                                keyboardScale = ks2;
                                keyH = kH2;
                                unit = u2; gap = g2;
                                keyboardMaxRowW = 0.0f;
                                for (const auto& row : rows) {
                                    float w = 0.0f;
                                    for (size_t c = 0; c < row.size(); ++c) { w += row[c].w * u2; }
                                    if (w > keyboardMaxRowW) keyboardMaxRowW = w;
                                }
                            }
                        }
                    }

                    const ImVec2 layoutStart = ImGui::GetCursorPos();
                    const ImVec2 layoutStartScreen = ImGui::GetCursorScreenPos();
                    const float keyboardTotalH = (float)rows.size() * (keyH + gap);

                    {
                        const float platePad = 10.0f * keyboardScale;
                        const float plateRound = 10.0f * keyboardScale;
                        const ImVec2 plateMin = ImVec2(layoutStartScreen.x - platePad, layoutStartScreen.y - platePad);
                        const ImVec2 plateMax =
                            ImVec2(layoutStartScreen.x + keyboardMaxRowW + platePad, layoutStartScreen.y + keyboardTotalH - gap + platePad);

                        dl->AddRectFilled(ImVec2(plateMin.x + 5, plateMin.y + 6), ImVec2(plateMax.x + 5, plateMax.y + 6),
                                          IM_COL32(0, 0, 0, 130), plateRound);

                        const ImU32 plateTop = IM_COL32(35, 38, 46, 255);
                        const ImU32 plateBot = IM_COL32(18, 20, 26, 255);
                        dl->AddRectFilled(plateMin, plateMax, plateBot, plateRound);
                        const float plateSplit = 0.55f;
                        const ImVec2 plateMid = ImVec2(plateMax.x, plateMin.y + (plateMax.y - plateMin.y) * plateSplit);
                        dl->AddRectFilled(plateMin, plateMid, plateTop, plateRound, ImDrawFlags_RoundCornersTop);
                        dl->AddRect(plateMin, plateMax, IM_COL32(10, 10, 12, 255), plateRound);
                        dl->AddLine(ImVec2(plateMin.x + 6, plateMin.y + 6), ImVec2(plateMax.x - 6, plateMin.y + 6), IM_COL32(255, 255, 255, 25),
                                    1.0f);
                    }
                    bool openRemoveCustomKeyConfirmPopup = false;

                    const ImGuiID rebindPopupId = ImGui::GetID(trc("inputs.rebind_config"));
                    auto openRebindContextFor = [&](DWORD vk, bool openedFromCustomKey) {
                        s_layoutContextVk = vk;
                        s_layoutContextPreferredIndex = -1;
                        s_layoutContextOpenedFromCustomKey = openedFromCustomKey;
                        ImGui::OpenPopup(rebindPopupId);
                    };

                    auto normalizeMouseButtonLabel = [](std::string s) -> std::string {
                        if (s == "MOUSE1") return "MB1";
                        if (s == "MOUSE2") return "MB2";
                        if (s == "MOUSE3") return "MB3";
                        if (s == "MOUSE4") return "MB4";
                        if (s == "MOUSE5") return "MB5";
                        return s;
                    };

                    auto isModifierVk = [](DWORD vk) -> bool {
                        return vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_SHIFT || vk == VK_LSHIFT ||
                               vk == VK_RSHIFT || vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN;
                    };

                    auto isConsumeOnlyLayoutRebind = [](const KeyRebind* rb) -> bool {
                        return rb != nullptr && rb->enabled && rb->fromKey != 0 && rb->toKey == 0;
                    };

                    const char* layoutNoneLabel = trc("label.none");
                    const char* disableKeyLabel = trc("inputs.disable_key");

                    auto isTriggerOutputDisabled = [&](const KeyRebind* rb) -> bool {
                        return rb != nullptr && rb->enabled && rb->fromKey != 0 && rb->triggerOutputDisabled &&
                               !isConsumeOnlyLayoutRebind(rb);
                    };

                    auto isBaseTypesOutputDisabled = [&](const KeyRebind* rb) -> bool {
                        return rb != nullptr && rb->enabled && rb->fromKey != 0 && rb->baseOutputDisabled &&
                               !isConsumeOnlyLayoutRebind(rb);
                    };

                    auto isShiftTypesOutputDisabled = [&](const KeyRebind* rb) -> bool {
                        return rb != nullptr && rb->enabled && rb->fromKey != 0 && rb->shiftLayerEnabled &&
                               rb->shiftLayerOutputDisabled && !isConsumeOnlyLayoutRebind(rb);
                    };

                    auto isMouseButtonVk = [](DWORD vk) -> bool {
                        return vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
                    };

                    auto normalizeCapturedLayoutSourceVk = [&](DWORD vk, LPARAM lParam) -> DWORD {
                        if (isMouseButtonVk(vk) || isScrollWheelVk(vk)) return vk;

                        uint64_t rawLParam = static_cast<uint64_t>(lParam);
                        DWORD scanWithFlags = static_cast<DWORD>((rawLParam >> 16) & 0xFFu);
                        if ((rawLParam & (1ull << 24)) != 0) {
                            scanWithFlags |= 0xE000u;
                        }
                        return normalizeModifierVkForDisplay(vk, scanWithFlags);
                    };

                    auto getLayoutSourceButtonLabel = [&](DWORD vk) -> std::string {
                        if (isMouseButtonVk(vk) || isScrollWheelVk(vk)) {
                            return normalizeMouseButtonLabel(VkToString(vk));
                        }

                        const DWORD displayScan = getScanCodeWithExtendedFlag(vk);
                        return normalizeMouseButtonLabel(scanCodeToDisplayName(displayScan, vk));
                    };

                    auto getCompactLayoutSourceButtonLabel = [&](DWORD vk) -> std::string {
                        if (vk == VK_TOOLSCREEN_SCROLL_UP) return "UP";
                        if (vk == VK_TOOLSCREEN_SCROLL_DOWN) return "DN";
                        return getLayoutSourceButtonLabel(vk);
                    };

                    auto customLayoutSourceVkFromScan = [&](DWORD scan) -> DWORD {
                        DWORD vkFromScan = MapVirtualKey(scan, MAPVK_VSC_TO_VK_EX);
                        if ((scan & 0xFF00) == 0) {
                            switch (scan) {
                            case 0x47: vkFromScan = VK_NUMPAD7; break;
                            case 0x48: vkFromScan = VK_NUMPAD8; break;
                            case 0x49: vkFromScan = VK_NUMPAD9; break;
                            case 0x4B: vkFromScan = VK_NUMPAD4; break;
                            case 0x4C: vkFromScan = VK_NUMPAD5; break;
                            case 0x4D: vkFromScan = VK_NUMPAD6; break;
                            case 0x4F: vkFromScan = VK_NUMPAD1; break;
                            case 0x50: vkFromScan = VK_NUMPAD2; break;
                            case 0x51: vkFromScan = VK_NUMPAD3; break;
                            case 0x4A: vkFromScan = VK_SUBTRACT; break;
                            case 0x4E: vkFromScan = VK_ADD; break;
                            case 0x52: vkFromScan = VK_NUMPAD0; break;
                            case 0x53: vkFromScan = VK_DECIMAL; break;
                            default: break;
                            }
                        }
                        return normalizeModifierVkForDisplay(vkFromScan, scan);
                    };

                    auto canMoveCustomLayoutSourceKey = [&](DWORD sourceVk, DWORD newSourceVk) {
                        if (sourceVk == 0 || newSourceVk == 0) return false;
                        if (sourceVk == newSourceVk) return true;
                        if (containsLayoutSourceKey(g_config.keyRebinds.layoutExtraKeys, newSourceVk)) return false;
                        return countRebindsForSourceKey(newSourceVk) == 0;
                    };

                    auto moveCustomLayoutSourceKey = [&](DWORD sourceVk, DWORD newSourceVk) {
                        if (!canMoveCustomLayoutSourceKey(sourceVk, newSourceVk)) return false;
                        if (sourceVk == newSourceVk) return true;

                        bool changed = false;
                        bool movedStoredCustomKey = false;
                        for (DWORD& storedVk : g_config.keyRebinds.layoutExtraKeys) {
                            if (storedVk != sourceVk) continue;
                            storedVk = newSourceVk;
                            movedStoredCustomKey = true;
                            changed = true;
                        }

                        if (!movedStoredCustomKey && !containsLayoutSourceKey(g_config.keyRebinds.layoutExtraKeys, newSourceVk)) {
                            g_config.keyRebinds.layoutExtraKeys.push_back(newSourceVk);
                            changed = true;
                        }

                        int movedRebindCount = 0;
                        for (auto& rebind : g_config.keyRebinds.rebinds) {
                            if (rebind.fromKey != sourceVk) continue;
                            rebind.fromKey = newSourceVk;
                            ++movedRebindCount;
                            changed = true;
                        }

                        if (!changed) return false;

                        if (s_layoutContextVk == sourceVk) {
                            s_layoutContextVk = newSourceVk;
                            s_layoutContextPreferredIndex = -1;
                        }

                        if (s_layoutPendingCustomKeyRemovalVk == sourceVk) {
                            s_layoutPendingCustomKeyRemovalVk = newSourceVk;
                        }

                        s_layoutContextOpenedFromCustomKey = true;
                        syncUnicodeEditBuffers();
                        g_configIsDirty = true;
                        if (movedRebindCount > 0) {
                            std::lock_guard<std::mutex> hotkeyLock(g_hotkeyMainKeysMutex);
                            RebuildHotkeyMainKeys_Internal();
                        }
                        return true;
                    };

                    auto queueCustomLayoutKeyRemoval = [&](DWORD sourceVk) {
                        const bool builtInSourceKey = containsLayoutSourceKey(builtInLayoutKeys, sourceVk);
                        const bool isStoredCustomKey = containsLayoutSourceKey(g_config.keyRebinds.layoutExtraKeys, sourceVk);
                        const bool isFallbackOnlyCustomKey = !builtInSourceKey && countRebindsForSourceKey(sourceVk) > 0;
                        if (sourceVk == 0 || (!isStoredCustomKey && !isFallbackOnlyCustomKey)) {
                            return;
                        }

                        const int savedRebindCount = countRebindsForSourceKey(sourceVk);
                        if (builtInSourceKey) {
                            (void)removeCustomLayoutKeyOnly(sourceVk);
                            return;
                        }

                        if (savedRebindCount <= 0) {
                            removeCustomLayoutKeyAndRebinds(sourceVk);
                            return;
                        }

                        s_layoutPendingCustomKeyRemovalVk = sourceVk;
                        s_layoutPendingCustomKeyRemovalRebindCount = savedRebindCount;
                        openRemoveCustomKeyConfirmPopup = true;
                    };

                    auto isNonTypableTextVk = [&](DWORD vk) -> bool {
                        if (vk == 0) return false;
                        KeyRebind candidate;
                        candidate.enabled = true;
                        candidate.fromKey = vk;
                        return DoesKeyRebindTriggerCannotType(candidate);
                    };

                    auto hasShiftLayerOverride = [&](const KeyRebind* rb, DWORD originalVk) -> bool {
                        if (!rb || !rb->shiftLayerEnabled) return false;
                        if (isShiftTypesOutputDisabled(rb)) return true;
                        if (rb->shiftLayerOutputUnicode != 0) return true;
                        if (rb->shiftLayerOutputVK == 0) return false;
                        if (rb->useCustomOutput && rb->customOutputUnicode != 0) return true;

                        DWORD baseTextVk = (rb->useCustomOutput && rb->customOutputVK != 0) ? rb->customOutputVK : originalVk;
                        if (baseTextVk == 0) baseTextVk = originalVk;
                        if (rb->shiftLayerOutputVK != baseTextVk) return true;
                        // Keep explicit Shift-layer settings visible/configured even when VK matches base output.
                        return true;
                    };

                    auto resolveBaseTypesVkFor = [&](const KeyRebind* rb, DWORD originalVk) -> DWORD {
                        if (isBaseTypesOutputDisabled(rb)) return 0;

                        DWORD textVk = (rb && rb->useCustomOutput && rb->customOutputVK != 0) ? rb->customOutputVK : originalVk;
                        if (textVk == 0) textVk = originalVk;
                        return textVk;
                    };

                    auto resolveTypesVkFor = [&](const KeyRebind* rb, DWORD originalVk, bool useShiftLayer) -> DWORD {
                        DWORD textVk = resolveBaseTypesVkFor(rb, originalVk);
                        if (rb && useShiftLayer && rb->shiftLayerEnabled) {
                            if (isShiftTypesOutputDisabled(rb)) return 0;
                            if (rb->shiftLayerOutputVK != 0) {
                                textVk = rb->shiftLayerOutputVK;
                            }
                        }
                        return textVk;
                    };

                    auto resolveTriggerVkFor = [&](const KeyRebind* rb, DWORD originalVk) -> DWORD {
                        if (isTriggerOutputDisabled(rb)) return 0;
                        DWORD triggerVk = rb ? rb->toKey : originalVk;
                        if (triggerVk == 0) triggerVk = originalVk;
                        return triggerVk;
                    };

                    auto resolveTriggerScanFor = [&](const KeyRebind* rb, DWORD originalVk) -> DWORD {
                        const DWORD triggerVk = resolveTriggerVkFor(rb, originalVk);
                        if (triggerVk == 0) return 0;
                        DWORD displayScan = (rb && rb->useCustomOutput && rb->customOutputScanCode != 0) ? rb->customOutputScanCode
                                                                                                            : getScanCodeWithExtendedFlag(triggerVk);
                        if (displayScan != 0 && (displayScan & 0xFF00) == 0) {
                            DWORD derived = getScanCodeWithExtendedFlag(triggerVk);
                            if ((derived & 0xFF00) != 0 && ((derived & 0xFF) == (displayScan & 0xFF))) { displayScan = derived; }
                        }
                        return displayScan;
                    };

                    auto cannotTypeFor = [&](const KeyRebind* rb, DWORD originalVk) -> bool {
                        KeyRebind candidate;
                        if (rb != nullptr) {
                            candidate = *rb;
                        }
                        if (candidate.fromKey == 0) {
                            candidate.fromKey = originalVk;
                            candidate.enabled = true;
                        }
                        return DoesKeyRebindTriggerCannotType(candidate);
                    };

                    auto typesValueForDisplay = [&](const KeyRebind* rb, DWORD originalVk) -> std::string {
                        if (isConsumeOnlyLayoutRebind(rb)) return tr("label.none");
                        if (isBaseTypesOutputDisabled(rb)) return tr("label.none");
                        if (cannotTypeFor(rb, originalVk)) return tr("inputs.cannot_type");
                        if (rb && rb->useCustomOutput && rb->customOutputUnicode != 0) {
                            return codepointToDisplay((uint32_t)rb->customOutputUnicode);
                        }

                        DWORD textVk = resolveTypesVkFor(rb, originalVk, false);
                        if (isNonTypableTextVk(textVk)) return tr("inputs.cannot_type");
                        return normalizeMouseButtonLabel(VkToString(textVk));
                    };

                    auto typesShiftValueForDisplay = [&](const KeyRebind* rb, DWORD originalVk) -> std::string {
                        if (isConsumeOnlyLayoutRebind(rb)) return tr("label.none");
                        if (isShiftTypesOutputDisabled(rb)) return tr("label.none");
                        if (cannotTypeFor(rb, originalVk)) return tr("inputs.cannot_type");
                        if (!hasShiftLayerOverride(rb, originalVk)) return typesValueForDisplay(rb, originalVk);
                        if (rb->shiftLayerEnabled && rb->shiftLayerOutputUnicode != 0) {
                            return codepointToDisplay((uint32_t)rb->shiftLayerOutputUnicode);
                        }

                        DWORD textVk = resolveTypesVkFor(rb, originalVk, true);
                        if (isNonTypableTextVk(textVk)) return tr("inputs.cannot_type");
                        return normalizeMouseButtonLabel(VkToString(textVk));
                    };

                    const std::string cannotTypeText = tr("inputs.cannot_type");
                    auto keyboardLayoutIndicatorText = [&](const std::string& text) -> std::string {
                        return text == cannotTypeText ? std::string("CT") : text;
                    };

                    auto isConfiguredLayoutRebind = [&](const KeyRebind* rb, DWORD originalVk) -> bool {
                        if (!rb || rb->fromKey == 0) return false;
                        if (isConsumeOnlyLayoutRebind(rb)) return true;
                        if (isTriggerOutputDisabled(rb)) return true;
                        if (isBaseTypesOutputDisabled(rb)) return true;
                        if (rb->toKey != 0 && rb->toKey != originalVk) return true;
                        if (rb->customOutputScanCode != 0) return true;
                        if (rb->customOutputUnicode != 0) return true;
                        if (rb->customOutputVK != 0 && rb->customOutputVK != originalVk) return true;
                        if (rb->baseOutputShifted) return true;
                        if (rb->shiftLayerUsesCapsLock) return true;
                        if (hasShiftLayerOverride(rb, originalVk)) return true;
                        return false;
                    };

                    extern std::atomic<bool> g_isGameFocused;
                    const bool keyboardLayoutGameFocused = g_isGameFocused.load(std::memory_order_acquire);

                    auto drawKeyCell = [&](DWORD vk, const char* label, const ImVec2& pMin, const ImVec2& pMax, const KeyRebind* rb) {
                        const bool hovered = ImGui::IsItemHovered();
                        const bool physDown = keyboardLayoutGameFocused && !isScrollWheelVk(vk) && (GetAsyncKeyState(vk) & 0x8000) != 0;
                        const bool active = ImGui::IsItemActive() || physDown;

                        struct KeyTheme {
                            ImU32 top;
                            ImU32 bottom;
                            ImU32 border;
                            ImU32 text;
                        };

                        auto clamp255 = [](int v) -> int { return v < 0 ? 0 : (v > 255 ? 255 : v); };
                        auto adjust = [&](ImU32 c, int add) -> ImU32 {
                            int r = (int)(c & 0xFF);
                            int g = (int)((c >> 8) & 0xFF);
                            int b = (int)((c >> 16) & 0xFF);
                            int a = (int)((c >> 24) & 0xFF);
                            r = clamp255(r + add);
                            g = clamp255(g + add);
                            b = clamp255(b + add);
                            return (ImU32)(r | (g << 8) | (b << 16) | (a << 24));
                        };

                        auto themeForVk = [&](DWORD v) -> KeyTheme {
                            const bool isMouse = (v == VK_LBUTTON || v == VK_RBUTTON || v == VK_MBUTTON || v == VK_XBUTTON1 || v == VK_XBUTTON2);
                            const bool isFn = (v == VK_ESCAPE || (v >= VK_F1 && v <= VK_F24) || v == VK_SNAPSHOT || v == VK_SCROLL || v == VK_PAUSE);
                            const bool isMod = (v == VK_SHIFT || v == VK_LSHIFT || v == VK_RSHIFT || v == VK_CONTROL || v == VK_LCONTROL ||
                                                v == VK_RCONTROL || v == VK_MENU || v == VK_LMENU || v == VK_RMENU || v == VK_LWIN || v == VK_RWIN ||
                                                v == VK_TAB || v == VK_CAPITAL || v == VK_BACK || v == VK_RETURN || v == VK_SPACE || v == VK_APPS);
                            const bool isNav = (v == VK_INSERT || v == VK_DELETE || v == VK_HOME || v == VK_END || v == VK_PRIOR || v == VK_NEXT ||
                                                v == VK_UP || v == VK_DOWN || v == VK_LEFT || v == VK_RIGHT);
                            const bool isNum = (v >= VK_NUMPAD0 && v <= VK_NUMPAD9) || v == VK_ADD || v == VK_SUBTRACT || v == VK_MULTIPLY ||
                                                v == VK_DIVIDE || v == VK_DECIMAL || v == VK_NUMLOCK;

                            KeyTheme t{ IM_COL32(80, 86, 98, 255), IM_COL32(45, 49, 60, 255), IM_COL32(18, 18, 20, 255),
                                        IM_COL32(245, 245, 245, 255) };
                            if (isMod) {
                                t.top = IM_COL32(72, 78, 92, 255);
                                t.bottom = IM_COL32(38, 42, 52, 255);
                            }
                            if (isFn) {
                                t.top = IM_COL32(92, 80, 74, 255);
                                t.bottom = IM_COL32(52, 42, 38, 255);
                            }
                            if (isNav) {
                                t.top = IM_COL32(78, 86, 104, 255);
                                t.bottom = IM_COL32(40, 45, 56, 255);
                            }
                            if (isNum) {
                                t.top = IM_COL32(72, 88, 90, 255);
                                t.bottom = IM_COL32(36, 46, 48, 255);
                            }
                            if (isMouse) {
                                t.top = IM_COL32(88, 90, 108, 255);
                                t.bottom = IM_COL32(44, 46, 60, 255);
                            }
                            return t;
                        };

                        KeyTheme theme = themeForVk(vk);
                        theme.bottom = adjust(theme.bottom, 10);
                        if (hovered) {
                            theme.top = adjust(theme.top, 12);
                            theme.bottom = adjust(theme.bottom, 10);
                        }
                        if (active) {
                            theme.top = adjust(theme.top, -8);
                            theme.bottom = adjust(theme.bottom, -16);
                        }

                        const ImVec2 size = ImVec2(pMax.x - pMin.x, pMax.y - pMin.y);
                        const float shadow = 2.0f * keyboardScale;
                        const ImVec2 pressOff = active ? ImVec2(0.0f, 1.2f * keyboardScale) : ImVec2(0, 0);

                        dl->AddRectFilled(ImVec2(pMin.x + shadow, pMin.y + shadow), ImVec2(pMax.x + shadow, pMax.y + shadow),
                                          IM_COL32(0, 0, 0, 90), rounding);

                        const ImVec2 kMin = ImVec2(pMin.x + pressOff.x, pMin.y + pressOff.y);
                        const ImVec2 kMax = ImVec2(pMax.x + pressOff.x, pMax.y + pressOff.y);

                        dl->AddRectFilled(kMin, kMax, theme.bottom, rounding);
                        const float split = 0.70f;
                        const ImVec2 kMid = ImVec2(kMax.x, kMin.y + (kMax.y - kMin.y) * split);
                        dl->AddRectFilled(kMin, kMid, theme.top, rounding, ImDrawFlags_RoundCornersTop);

                        dl->AddLine(ImVec2(kMin.x + 2, kMin.y + 1), ImVec2(kMax.x - 2, kMin.y + 1), IM_COL32(255, 255, 255, 18), 1.0f);

                        dl->AddRect(kMin, kMax, theme.border, rounding, 0, 1.0f);

                        const bool disabledOutput = isConsumeOnlyLayoutRebind(rb);
                        const bool hasConfigured = isConfiguredLayoutRebind(rb, vk);
                        if (hasConfigured) {
                            const bool warningState = disabledOutput || isTriggerOutputDisabled(rb) ||
                                                      isBaseTypesOutputDisabled(rb) || isShiftTypesOutputDisabled(rb) || !rb->enabled;
                            const ImU32 outline = warningState ? IM_COL32(255, 170, 0, 255) : IM_COL32(0, 220, 110, 255);
                            dl->AddRect(kMin, kMax, outline, rounding, 0, 3.0f);
                            const ImU32 tint = warningState ? IM_COL32(255, 165, 0, 35) : IM_COL32(0, 190, 95, 30);
                            dl->AddRectFilled(kMin, kMax, tint, rounding);
                        }

                        const float padX = 6.0f * keyboardScale;
                        const float padY = 4.0f * keyboardScale;

                        const bool isNoOp = [&]() -> bool {
                            if (!hasConfigured) return false;
                            if (disabledOutput) return false;
                            if (isTriggerOutputDisabled(rb) || isBaseTypesOutputDisabled(rb) || isShiftTypesOutputDisabled(rb)) return false;
                            if (rb->toKey != vk) return false;
                            if (rb->customOutputVK != 0 && rb->customOutputVK != rb->toKey) return false;
                            if (rb->customOutputUnicode != 0) return false;
                            if (rb->customOutputScanCode != 0) return false;
                            if (rb->baseOutputShifted) return false;
                            if (hasShiftLayerOverride(rb, vk)) return false;
                            return true;
                        }();
                        const bool showRebindInfo = hasConfigured && !isNoOp;
                        DWORD triggerVK = (showRebindInfo && !disabledOutput) ? resolveTriggerVkFor(rb, vk) : vk;
                        DWORD outScan = (showRebindInfo && !disabledOutput) ? resolveTriggerScanFor(rb, vk) : 0;
                        const std::string primaryText = disabledOutput
                                                            ? std::string(layoutNoneLabel)
                                                            : (showRebindInfo ? keyboardLayoutIndicatorText(typesValueForDisplay(rb, vk))
                                                                              : std::string(label));
                        const std::string secondaryText = (showRebindInfo && !disabledOutput)
                                                              ? ((triggerVK == 0) ? std::string(layoutNoneLabel)
                                                                                  : normalizeMouseButtonLabel(scanCodeToDisplayName(outScan, triggerVK)))
                                                              : std::string();
                        const bool showShiftLayerText = showRebindInfo && !disabledOutput && hasShiftLayerOverride(rb, vk);
                        const std::string shiftLayerText = showShiftLayerText
                                                               ? keyboardLayoutIndicatorText(normalizeMouseButtonLabel(typesShiftValueForDisplay(rb, vk)))
                                                               : std::string();
                        const bool showSecondaryText = showRebindInfo && !secondaryText.empty();

#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
                        RecordGuiTestKeyboardLayoutKeyLabels(vk, primaryText, secondaryText, shiftLayerText);
#endif

                        ImFont* fLabel = g_keyboardLayoutPrimaryFont ? g_keyboardLayoutPrimaryFont : ImGui::GetFont();
                        ImFont* fSecondary = g_keyboardLayoutSecondaryFont ? g_keyboardLayoutSecondaryFont : fLabel;
                        auto snapPxText = [](float v) -> float { return floorf(v + 0.5f); };
                        auto snapFontSize = [](float v) -> float {
                            float s = floorf(v + 0.5f);
                            if (s < 8.0f) s = 8.0f;
                            return s;
                        };
                        auto fitTextToWidth = [&](ImFont* font, float desiredFontSize, const std::string& text, float availWidth,
                                                  float minFontSize) {
                            float fontSize = desiredFontSize;
                            ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text.c_str());

                            if (availWidth > 1.0f && textSize.x > availWidth) {
                                const float widthScale = availWidth / (textSize.x + 0.001f);
                                fontSize = desiredFontSize * widthScale;
                                if (fontSize < minFontSize) fontSize = minFontSize;
                                textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text.c_str());

                                if (textSize.x > availWidth && fontSize > minFontSize) {
                                    const float exactScale = availWidth / (textSize.x + 0.001f);
                                    float refinedFontSize = fontSize * exactScale;
                                    if (refinedFontSize < minFontSize) refinedFontSize = minFontSize;
                                    if (refinedFontSize < fontSize) {
                                        fontSize = refinedFontSize;
                                        textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text.c_str());
                                    }
                                }
                            }

                            return std::pair<float, ImVec2>(fontSize, textSize);
                        };
                        auto centerTextXWithin = [&](float minX, float maxX, float textWidth) {
                            const float availWidth = maxX - minX;
                            float x = snapPxText(minX + (availWidth - textWidth) * 0.5f);
                            const float maxStartX = maxX - textWidth;
                            if (maxStartX >= minX) {
                                if (x < minX) x = snapPxText(minX);
                                if (x > maxStartX) x = snapPxText(maxStartX);
                            }
                            return x;
                        };

                        const float textAvailW = size.x - padX * 2.0f;
                        const float textAvailH = size.y - padY * 2.0f;
                        const float textMinX = kMin.x + padX;
                        const float textMaxX = kMax.x - padX;
                        float layoutTextBoost = 0.85f + 0.35f * s_keyboardLayoutScale;
                        if (layoutTextBoost < 0.85f) layoutTextBoost = 0.85f;
                        if (layoutTextBoost > 1.85f) layoutTextBoost = 1.85f;

                        float shiftLayerFs = 0.0f;
                        ImVec2 shiftLayerSz = ImVec2(0.0f, 0.0f);
                        float shiftLayerBodyOffset = 0.0f;
                        float shiftLayerBodyMinX = textMinX;
                        if (showShiftLayerText) {
                            shiftLayerFs = snapFontSize(fSecondary->LegacySize * (0.70f + 0.10f * s_keyboardLayoutScale));
                            shiftLayerSz = fSecondary->CalcTextSizeA(shiftLayerFs, FLT_MAX, 0.0f, shiftLayerText.c_str());
                            const float shiftAvailW = textAvailW * (showSecondaryText ? 0.42f : 0.50f);
                            if (shiftAvailW > 6.0f) {
                                const auto fittedShift = fitTextToWidth(fSecondary, shiftLayerFs, shiftLayerText, shiftAvailW, 6.0f);
                                shiftLayerFs = fittedShift.first;
                                shiftLayerSz = fittedShift.second;
                            }

                            const float bodyOffsetCap = textAvailH * (showSecondaryText ? 0.10f : 0.06f);
                            if (bodyOffsetCap > 0.0f && shiftLayerSz.y > 0.0f) {
                                shiftLayerBodyOffset = snapPxText(shiftLayerSz.y * (showSecondaryText ? 0.28f : 0.18f));
                                if (shiftLayerBodyOffset > bodyOffsetCap) shiftLayerBodyOffset = snapPxText(bodyOffsetCap);
                            }

                            const float shiftTextGapX = snapPxText((showSecondaryText ? 4.0f : 5.0f) * keyboardScale);
                            const float candidateBodyMinX = snapPxText(textMinX + shiftLayerSz.x + shiftTextGapX);
                            const float maxBodyMinX = textMaxX - 6.0f;
                            if (candidateBodyMinX < maxBodyMinX) shiftLayerBodyMinX = candidateBodyMinX;
                        }

                        const float bodyTextAvailH = (shiftLayerBodyOffset < textAvailH) ? (textAvailH - shiftLayerBodyOffset) : textAvailH;
                        const float bodyTextTopY = kMin.y + padY + ((shiftLayerBodyOffset < textAvailH) ? shiftLayerBodyOffset : 0.0f);
                        const float bodyTextMinX = showShiftLayerText ? shiftLayerBodyMinX : textMinX;
                        const float bodyTextAvailW = textMaxX - bodyTextMinX;

                        float labelFontSize = fLabel->LegacySize * layoutTextBoost;
                        ImVec2 labelSz = fLabel->CalcTextSizeA(labelFontSize, FLT_MAX, 0.0f, primaryText.c_str());

                        if (bodyTextAvailW > 8.0f) {
                            const auto fittedPrimary = fitTextToWidth(fLabel, snapFontSize(labelFontSize), primaryText, bodyTextAvailW, 6.0f);
                            labelFontSize = fittedPrimary.first;
                            labelSz = fittedPrimary.second;
                        }

                        const ImU32 shiftCol = (rb && !rb->enabled) ? IM_COL32(255, 220, 170, 215) : IM_COL32(220, 238, 255, 220);
                        const ImU32 infoCol = (rb && !rb->enabled) ? IM_COL32(255, 220, 170, 235) : IM_COL32(245, 245, 245, 235);

                        if (showShiftLayerText && showSecondaryText) {
                            ImFont* fTopRow = fLabel;
                            const float topRowBoost = 1.12f;
                            const float pairTopPadY = snapPxText((padY * 0.40f) - (0.5f * keyboardScale));
                            const float pairBottomPadY = snapPxText(padY * 0.55f);
                            const float pairTextAvailH = size.y - pairTopPadY - pairBottomPadY;

                            float topRowFs = snapFontSize(fTopRow->LegacySize * layoutTextBoost * topRowBoost);
                            ImVec2 shiftSz = ImVec2(0.0f, 0.0f);
                            ImVec2 primarySz = ImVec2(0.0f, 0.0f);
                            auto recalcTopPair = [&]() {
                                shiftSz = fTopRow->CalcTextSizeA(topRowFs, FLT_MAX, 0.0f, shiftLayerText.c_str());
                                primarySz = fTopRow->CalcTextSizeA(topRowFs, FLT_MAX, 0.0f, primaryText.c_str());
                            };
                            recalcTopPair();

                            float secondaryFs = snapFontSize(fSecondary->LegacySize * layoutTextBoost);
                            ImVec2 secondarySz = fSecondary->CalcTextSizeA(secondaryFs, FLT_MAX, 0.0f, secondaryText.c_str());
                            if (textAvailW > 8.0f) {
                                const auto fittedSecondary =
                                    fitTextToWidth(fSecondary, secondaryFs, secondaryText, textAvailW, 6.0f);
                                secondaryFs = fittedSecondary.first;
                                secondarySz = fittedSecondary.second;
                            }

                            float topGapX = snapPxText(2.0f * keyboardScale);
                            if (topGapX < 1.0f) topGapX = 1.0f;

                            auto fitTopPair = [&]() {
                                float combinedW = shiftSz.x + topGapX + primarySz.x;
                                if (textAvailW > 8.0f && combinedW > textAvailW) {
                                    float fit = textAvailW / (combinedW + 0.001f);
                                    if (fit < 1.0f) {
                                        topRowFs = (topRowFs * fit < 6.0f) ? 6.0f : topRowFs * fit;
                                        recalcTopPair();
                                        combinedW = shiftSz.x + topGapX + primarySz.x;
                                    }

                                    if (combinedW > textAvailW) {
                                        const float widestTopLabel = (shiftSz.x > primarySz.x) ? shiftSz.x : primarySz.x;
                                        if (widestTopLabel > 0.0f) {
                                            const float maxWidthFit = (textAvailW - topGapX) / (widestTopLabel + 0.001f);
                                            if (maxWidthFit < 1.0f) {
                                                topRowFs = (topRowFs * maxWidthFit < 6.0f) ? 6.0f : topRowFs * maxWidthFit;
                                                recalcTopPair();
                                            }
                                        }
                                    }
                                }
                            };

                            fitTopPair();

                            float lineGap = snapPxText(1.0f * keyboardScale);
                            if (lineGap < 1.0f) lineGap = 1.0f;

                            float topRowH = (shiftSz.y > primarySz.y) ? shiftSz.y : primarySz.y;
                            float totalH = topRowH + lineGap + secondarySz.y;
                            if (pairTextAvailH > 0.0f && totalH > pairTextAvailH) {
                                const float fit = pairTextAvailH / (totalH + 0.001f);
                                if (fit < 1.0f) {
                                    topRowFs = (topRowFs * fit < 6.0f) ? 6.0f : topRowFs * fit;
                                    secondaryFs = (secondaryFs * fit < 6.0f) ? 6.0f : secondaryFs * fit;

                                    recalcTopPair();
                                    if (textAvailW > 8.0f) {
                                        const auto fittedSecondary =
                                            fitTextToWidth(fSecondary, secondaryFs, secondaryText, textAvailW, 6.0f);
                                        secondaryFs = fittedSecondary.first;
                                        secondarySz = fittedSecondary.second;
                                    } else {
                                        secondarySz = fSecondary->CalcTextSizeA(secondaryFs, FLT_MAX, 0.0f, secondaryText.c_str());
                                    }

                                    fitTopPair();
                                    topRowH = (shiftSz.y > primarySz.y) ? shiftSz.y : primarySz.y;
                                    totalH = topRowH + lineGap + secondarySz.y;
                                }
                            }

                            const float combinedTopW = shiftSz.x + topGapX + primarySz.x;
                            float topY = snapPxText(kMin.y + pairTopPadY);
                            float secondaryY = snapPxText(kMax.y - pairBottomPadY - secondarySz.y);
                            if (secondaryY < topY + topRowH + lineGap) {
                                float startY = snapPxText(kMin.y + pairTopPadY + (pairTextAvailH - totalH) * 0.5f);
                                if (startY < kMin.y + pairTopPadY) startY = snapPxText(kMin.y + pairTopPadY);
                                topY = startY;
                                secondaryY = snapPxText(topY + topRowH + lineGap);
                            }

                            const float pairStartX = centerTextXWithin(textMinX, textMaxX, combinedTopW);
                            const float shiftX = pairStartX;
                            const float primaryX = snapPxText(shiftX + shiftSz.x + topGapX);
                            const float shiftY = snapPxText(topY + (topRowH - shiftSz.y) * 0.5f);
                            const float primaryY = snapPxText(topY + (topRowH - primarySz.y) * 0.5f);
                            const float secondaryX = centerTextXWithin(textMinX, textMaxX, secondarySz.x);

                            dl->AddText(fTopRow, topRowFs, ImVec2(shiftX, shiftY), shiftCol, shiftLayerText.c_str());
                            dl->AddText(fTopRow, topRowFs, ImVec2(primaryX, primaryY), theme.text, primaryText.c_str());
                            dl->AddText(fSecondary, secondaryFs, ImVec2(secondaryX, secondaryY), infoCol, secondaryText.c_str());
                        } else if (!showRebindInfo || !showSecondaryText) {
                            if (showShiftLayerText) {
                                float shiftX = snapPxText(textMinX);
                                const float shiftMaxX = textMaxX - shiftLayerSz.x;
                                if (shiftMaxX >= textMinX && shiftX > shiftMaxX) shiftX = snapPxText(shiftMaxX);

                                float shiftY = snapPxText(kMin.y + padY - 1.0f * keyboardScale);
                                const float shiftMaxY = kMax.y - padY - shiftLayerSz.y;
                                if (shiftMaxY >= kMin.y + padY && shiftY > shiftMaxY) shiftY = snapPxText(shiftMaxY);

                                dl->AddText(fSecondary, shiftLayerFs, ImVec2(shiftX, shiftY), shiftCol, shiftLayerText.c_str());
                            }

                            const float maxByHeight = bodyTextAvailH * 0.98f;
                            if (maxByHeight > 0.0f && labelFontSize > maxByHeight) {
                                labelFontSize = maxByHeight;
                                labelSz = fLabel->CalcTextSizeA(labelFontSize, FLT_MAX, 0.0f, primaryText.c_str());
                                if (bodyTextAvailW > 8.0f) {
                                    const auto fittedPrimary = fitTextToWidth(fLabel, labelFontSize, primaryText, bodyTextAvailW, 6.0f);
                                    labelFontSize = fittedPrimary.first;
                                    labelSz = fittedPrimary.second;
                                }
                            }

                            float x = centerTextXWithin(bodyTextMinX, textMaxX, labelSz.x);
                            float y = snapPxText(bodyTextTopY + (bodyTextAvailH - labelSz.y) * 0.5f);
                            if (showShiftLayerText) {
                                const float shiftAlignedY = snapPxText(kMin.y + padY - 1.0f * keyboardScale + (shiftLayerSz.y - labelSz.y) * 0.5f);
                                const float shiftMaxY = kMax.y - padY - labelSz.y;
                                y = shiftAlignedY;
                                if (shiftMaxY >= bodyTextTopY && y > shiftMaxY) y = snapPxText(shiftMaxY);
                            }
                            if (y < bodyTextTopY) y = snapPxText(bodyTextTopY);
                            dl->AddText(fLabel, labelFontSize, ImVec2(x, y), theme.text, primaryText.c_str());
                        } else {
                            ImFont* f = fLabel;

                            float primaryFs = labelFontSize;
                            ImVec2 primarySz = f->CalcTextSizeA(primaryFs, FLT_MAX, 0.0f, primaryText.c_str());

                            float secondaryFs = fSecondary->LegacySize * layoutTextBoost;
                            ImVec2 secondarySz = fSecondary->CalcTextSizeA(secondaryFs, FLT_MAX, 0.0f, secondaryText.c_str());
                            if (bodyTextAvailW > 8.0f) {
                                const auto fittedSecondary =
                                    fitTextToWidth(fSecondary, snapFontSize(secondaryFs), secondaryText, bodyTextAvailW, 6.0f);
                                secondaryFs = fittedSecondary.first;
                                secondarySz = fittedSecondary.second;
                            }

                            float lineGap = snapPxText(1.0f * keyboardScale);
                            if (lineGap < 1.0f) lineGap = 1.0f;

                            float totalH = primarySz.y + lineGap + secondarySz.y;
                            if (bodyTextAvailH > 0.0f && totalH > bodyTextAvailH) {
                                float fit = bodyTextAvailH / (totalH + 0.001f);
                                if (fit < 1.0f) {
                                    primaryFs *= fit;
                                    secondaryFs *= fit;
                                    if (bodyTextAvailW > 8.0f) {
                                        const auto fittedPrimary = fitTextToWidth(f, primaryFs, primaryText, bodyTextAvailW, 6.0f);
                                        primaryFs = fittedPrimary.first;
                                        primarySz = fittedPrimary.second;

                                        const auto fittedSecondary = fitTextToWidth(fSecondary, secondaryFs, secondaryText, bodyTextAvailW, 6.0f);
                                        secondaryFs = fittedSecondary.first;
                                        secondarySz = fittedSecondary.second;
                                    } else {
                                        primarySz = f->CalcTextSizeA(primaryFs, FLT_MAX, 0.0f, primaryText.c_str());
                                        secondarySz = fSecondary->CalcTextSizeA(secondaryFs, FLT_MAX, 0.0f, secondaryText.c_str());
                                    }
                                    totalH = primarySz.y + lineGap + secondarySz.y;
                                }
                            }

                            float startY = snapPxText(bodyTextTopY + (bodyTextAvailH - totalH) * 0.5f);
                            if (startY < bodyTextTopY) startY = snapPxText(bodyTextTopY);

                            float x1 = centerTextXWithin(bodyTextMinX, textMaxX, primarySz.x);
                            dl->AddText(f, primaryFs, ImVec2(x1, startY), theme.text, primaryText.c_str());

                            float y2 = snapPxText(startY + primarySz.y + lineGap);
                            float x2 = centerTextXWithin(bodyTextMinX, textMaxX, secondarySz.x);

                            dl->AddText(fSecondary, secondaryFs, ImVec2(x2, y2), infoCol, secondaryText.c_str());
                        }

                        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                            openRebindContextFor(vk, false);
                        }
                    };

#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
                    if (const DWORD requestedContextVk = ConsumeGuiTestOpenKeyboardLayoutContextRequest(); requestedContextVk != 0) {
                        const bool requestOpenedFromCustomKey =
                            containsLayoutSourceKey(g_config.keyRebinds.layoutExtraKeys, requestedContextVk) ||
                            !containsLayoutSourceKey(builtInLayoutKeys, requestedContextVk);
                        openRebindContextFor(requestedContextVk, requestOpenedFromCustomKey);
                    }
#endif

                    bool haveNumpadPlusAnchor = false;
                    ImVec2 numpadPlusAnchorMin = ImVec2(0, 0);
                    ImVec2 numpadPlusAnchorMax = ImVec2(0, 0);
                    bool haveNumpadEnterAnchor = false;
                    ImVec2 numpadEnterAnchorMin = ImVec2(0, 0);
                    ImVec2 numpadEnterAnchorMax = ImVec2(0, 0);

                    auto snapPx = [](float v) -> float { return (float)(int)(v + 0.5f); };

                    for (size_t rowIdx = 0; rowIdx < rows.size(); ++rowIdx) {
                        float xCursor = layoutStart.x;
                        float yCursor = layoutStart.y + (float)rowIdx * (keyH + gap);

                        for (size_t colIdx = 0; colIdx < rows[rowIdx].size(); ++colIdx) {
                            const KeyCell& kc = rows[rowIdx][colIdx];
                            const float keyW = kc.w * pitchX;

                            ImGui::SetCursorPos(ImVec2(snapPx(xCursor), snapPx(yCursor)));
                            if (kc.vk == 0) {
                                ImGui::Dummy(ImVec2(keyW, keyH));
                                xCursor += keyW;
                                xCursor = snapPx(xCursor);
                                continue;
                            }

                            if (kc.vk == VK_ADD && rowIdx == 2) {
                                const ImVec2 aMin = ImGui::GetCursorScreenPos();
                                ImGui::Dummy(ImVec2(keyW, keyH));
                                const ImVec2 aMax = ImVec2(aMin.x + keyW, aMin.y + keyH);
                                haveNumpadPlusAnchor = true;
                                numpadPlusAnchorMin = aMin;
                                numpadPlusAnchorMax = aMax;
                                xCursor += keyW;
                                xCursor = snapPx(xCursor);
                                continue;
                            }

                            if (kc.vk == VK_RETURN && rowIdx == 4 && kc.w < 1.6f) {
                                const ImVec2 aMin = ImGui::GetCursorScreenPos();
                                ImGui::Dummy(ImVec2(keyW, keyH));
                                const ImVec2 aMax = ImVec2(aMin.x + keyW, aMin.y + keyH);
                                haveNumpadEnterAnchor = true;
                                numpadEnterAnchorMin = aMin;
                                numpadEnterAnchorMax = aMax;
                                xCursor += keyW;
                                xCursor = snapPx(xCursor);
                                continue;
                            }

                            ImGui::PushID((int)(rowIdx * 1000 + colIdx));
                            ImGui::InvisibleButton("##key", ImVec2(keyW, keyH),
                                                  ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);

                            const ImVec2 pMin = ImGui::GetItemRectMin();
                            const ImVec2 pMax = ImGui::GetItemRectMax();
#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
                            RecordGuiTestKeyboardLayoutKeyRect(kc.vk, pMin, pMax);
#endif

                            DWORD displayVk = kc.vk;
                            if (auto it = vkSwap.find(kc.vk); it != vkSwap.end()) {
                                displayVk = it->second;
                            }
                            const char* displayLabel = (displayVk != kc.vk) ? nullptr : kc.labelOverride;

                            const KeyRebind* rb = findRebindForKey(displayVk);
                            auto oemIt = oemLabels.find(kc.vk);
                            const char* oemLabel = oemIt != oemLabels.end() ? oemIt->second.c_str() : nullptr;
                            std::string keyName = oemLabel ? std::string(oemLabel) :
                                                  (displayLabel ? std::string(displayLabel) : VkToString(displayVk));
                            ImVec2 capMin = ImVec2(pMin.x + keyPadX, pMin.y + keyPadY);
                            ImVec2 capMax = ImVec2(pMax.x - keyPadX, pMax.y - keyPadY);
                            if (capMax.x <= capMin.x + 2.0f) { capMin.x = pMin.x; capMax.x = pMax.x; }
                            if (capMax.y <= capMin.y + 2.0f) { capMin.y = pMin.y; capMax.y = pMax.y; }
                            drawKeyCell(displayVk, keyName.c_str(), capMin, capMax, rb);

                            if (ImGui::IsItemHovered()) {
                                ImGui::BeginTooltip();
                                if (isConfiguredLayoutRebind(rb, displayVk)) {
                                    if (isConsumeOnlyLayoutRebind(rb)) {
                                        ImGui::TextUnformatted(layoutNoneLabel);
                                    } else {
                                    DWORD triggerVkTip = resolveTriggerVkFor(rb, displayVk);
                                    DWORD triggerScanTip = resolveTriggerScanFor(rb, displayVk);
                                    const std::string typesTip = typesValueForDisplay(rb, displayVk);
                                    const std::string triggersTip = (triggerVkTip == 0)
                                                                        ? std::string(layoutNoneLabel)
                                                                        : normalizeMouseButtonLabel(scanCodeToDisplayName(triggerScanTip, triggerVkTip));
                                    const std::string typesText = tr("inputs.types_format", typesTip.c_str());
                                    ImGui::TextUnformatted(typesText.c_str());
                                    if (hasShiftLayerOverride(rb, displayVk)) {
                                        const std::string typesShiftTip = typesShiftValueForDisplay(rb, displayVk);
                                        const std::string typesShiftText = tr("inputs.types_shift_format", typesShiftTip.c_str());
                                        ImGui::TextUnformatted(typesShiftText.c_str());
                                    }
                                    const std::string triggersText = tr("inputs.triggers_format", triggersTip.c_str());
                                    ImGui::TextUnformatted(triggersText.c_str());
                                    }
                                } else {
                                    ImGui::Text("%s (%u)", (oemLabel ? oemLabel : VkToString(displayVk).c_str()), (unsigned)displayVk);
                                    ImGui::TextUnformatted(trc("inputs.tooltip.right_click_to_configure"));
                                }
                                ImGui::EndTooltip();
                            }

                            ImGui::PopID();
                            xCursor += keyW;
                            xCursor = snapPx(xCursor);
                        }

                    }

                    auto drawTallKey = [&](DWORD vk, const char* label, const ImVec2& anchorMin, const ImVec2& anchorMax) {
                        const float w = anchorMax.x - anchorMin.x;
                        const float h = keyH * 2.0f + gap;

                        ImGui::PushID((int)vk);
                        ImGui::SetCursorScreenPos(anchorMin);
                        ImGui::InvisibleButton("##tall", ImVec2(w, h),
                                              ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
                        const ImVec2 pMin = ImGui::GetItemRectMin();
                        const ImVec2 pMax = ImVec2(pMin.x + w, pMin.y + h);
#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
                        RecordGuiTestKeyboardLayoutKeyRect(vk, pMin, pMax);
#endif

                        const KeyRebind* rb = findRebindForKey(vk);
                        ImVec2 capMin = ImVec2(pMin.x + keyPadX, pMin.y + keyPadY);
                        ImVec2 capMax = ImVec2(pMax.x - keyPadX, pMax.y - keyPadY);
                        if (capMax.x <= capMin.x + 2.0f) { capMin.x = pMin.x; capMax.x = pMax.x; }
                        if (capMax.y <= capMin.y + 2.0f) { capMin.y = pMin.y; capMax.y = pMax.y; }
                        drawKeyCell(vk, label, capMin, capMax, rb);

                        if (ImGui::IsItemHovered()) {
                            ImGui::BeginTooltip();
                            if (isConfiguredLayoutRebind(rb, vk)) {
                                if (isConsumeOnlyLayoutRebind(rb)) {
                                    ImGui::TextUnformatted(layoutNoneLabel);
                                } else {
                                DWORD triggerVkTip = resolveTriggerVkFor(rb, vk);
                                DWORD triggerScanTip = resolveTriggerScanFor(rb, vk);
                                const std::string typesTip = typesValueForDisplay(rb, vk);
                                const std::string triggersTip = (triggerVkTip == 0)
                                                                    ? std::string(layoutNoneLabel)
                                                                    : normalizeMouseButtonLabel(scanCodeToDisplayName(triggerScanTip, triggerVkTip));
                                const std::string typesText = tr("inputs.types_format", typesTip.c_str());
                                ImGui::TextUnformatted(typesText.c_str());
                                if (hasShiftLayerOverride(rb, vk)) {
                                    const std::string typesShiftTip = typesShiftValueForDisplay(rb, vk);
                                    const std::string typesShiftText = tr("inputs.types_shift_format", typesShiftTip.c_str());
                                    ImGui::TextUnformatted(typesShiftText.c_str());
                                }
                                const std::string triggersText = tr("inputs.triggers_format", triggersTip.c_str());
                                ImGui::TextUnformatted(triggersText.c_str());
                                }
                            } else {
                                ImGui::Text("%s (%u)", VkToString(vk).c_str(), (unsigned)vk);
                                ImGui::TextUnformatted(trc("inputs.tooltip.right_click_to_configure"));
                            }
                            ImGui::EndTooltip();
                        }

                        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                            openRebindContextFor(vk, false);
                        }

                        ImGui::PopID();
                    };

                    if (haveNumpadPlusAnchor) {
                        drawTallKey(VK_ADD, "+", numpadPlusAnchorMin, numpadPlusAnchorMax);
                    }
                    if (haveNumpadEnterAnchor) {
                        drawTallKey(VK_RETURN, "ENTER", numpadEnterAnchorMin, numpadEnterAnchorMax);
                    }

                    const float mousePanelX = layoutStart.x + keyboardMaxRowW + unit * 0.9f;
                    const float mousePanelY = layoutStart.y;
                    ImGui::SetCursorPos(ImVec2(mousePanelX, mousePanelY));

                    const float mouseHeaderH = 0.0f;

                    float mouseDiagramTotalH = mouseHeaderH;

                    {
                        const float mouseW = unit * 3.6f;
                        const float mouseH = (keyboardTotalH - mouseHeaderH - gap);
                        mouseDiagramTotalH = mouseHeaderH + mouseH;
                        const float pad = 6.0f * keyboardScale;

                        ImGui::SetCursorPos(ImVec2(mousePanelX, mousePanelY + mouseHeaderH));
                        ImGui::Dummy(ImVec2(mouseW, mouseH));
                        const ImVec2 bodyMin = ImGui::GetItemRectMin();
                        const ImVec2 bodyMax = ImGui::GetItemRectMax();

                        const float bodyR = (mouseW < mouseH ? mouseW : mouseH) * 0.45f;
                        dl->AddRectFilled(bodyMin, bodyMax, IM_COL32(24, 26, 33, 255), bodyR);
                        dl->AddRect(bodyMin, bodyMax, IM_COL32(10, 10, 12, 255), bodyR, 0, 1.5f);

                        const ImVec2 innerMin = ImVec2(bodyMin.x + pad, bodyMin.y + pad);
                        const ImVec2 innerMax = ImVec2(bodyMax.x - pad, bodyMax.y - pad);
                        const float midX = (innerMin.x + innerMax.x) * 0.5f;
                        const float topH = (innerMax.y - innerMin.y) * 0.52f;
                        const float splitY = innerMin.y + topH;

                        auto drawMouseSegment = [&](DWORD vk, const char* label, const ImVec2& segMin, const ImVec2& segMax, float segR, ImDrawFlags segFlags) {
                            const bool hovered = ImGui::IsItemHovered();
                            const bool active = ImGui::IsItemActive();

                            struct KeyTheme {
                                ImU32 top;
                                ImU32 bottom;
                                ImU32 border;
                                ImU32 text;
                            };
                            auto clamp255 = [](int v) -> int { return v < 0 ? 0 : (v > 255 ? 255 : v); };
                            auto adjust = [&](ImU32 c, int add) -> ImU32 {
                                int r = (int)(c & 0xFF);
                                int g = (int)((c >> 8) & 0xFF);
                                int b = (int)((c >> 16) & 0xFF);
                                int a = (int)((c >> 24) & 0xFF);
                                r = clamp255(r + add);
                                g = clamp255(g + add);
                                b = clamp255(b + add);
                                return (ImU32)(r | (g << 8) | (b << 16) | (a << 24));
                            };
                            KeyTheme theme{ IM_COL32(88, 90, 108, 255), IM_COL32(44, 46, 60, 255), IM_COL32(18, 18, 20, 255),
                                            IM_COL32(245, 245, 245, 255) };
                            theme.bottom = adjust(theme.bottom, 10);
                            if (hovered) {
                                theme.top = adjust(theme.top, 12);
                                theme.bottom = adjust(theme.bottom, 10);
                            }
                            if (active) {
                                theme.top = adjust(theme.top, -8);
                                theme.bottom = adjust(theme.bottom, -16);
                            }

                            dl->AddRectFilled(segMin, segMax, theme.bottom, segR, segFlags);
                            const ImVec2 segMid = ImVec2(segMax.x, segMin.y + (segMax.y - segMin.y) * 0.72f);
                            ImDrawFlags topFlags = segFlags;
                            if (segFlags == ImDrawFlags_RoundCornersAll) {
                                topFlags = ImDrawFlags_RoundCornersTop;
                            } else {
                                topFlags &= (ImDrawFlags_RoundCornersTopLeft | ImDrawFlags_RoundCornersTopRight);
                            }
                            dl->AddRectFilled(segMin, segMid, theme.top, segR, topFlags);
                            dl->AddRect(segMin, segMax, theme.border, segR, segFlags, 1.0f);

                            const KeyRebind* rb = findRebindForKey(vk);
#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
                            RecordGuiTestKeyboardLayoutKeyLabels(vk, label, std::string(), std::string());
#endif
                            if (isConfiguredLayoutRebind(rb, vk)) {
                                const bool warningState = isConsumeOnlyLayoutRebind(rb) || isTriggerOutputDisabled(rb) ||
                                                          isBaseTypesOutputDisabled(rb) || isShiftTypesOutputDisabled(rb) || !rb->enabled;
                                const ImU32 outline = warningState ? IM_COL32(255, 170, 0, 255) : IM_COL32(0, 220, 110, 255);
                                dl->AddRect(segMin, segMax, outline, segR, segFlags, 3.0f);
                            }

                            ImFont* f = ImGui::GetFont();
                            const float fs = ImGui::GetFontSize() * 0.85f;
                            const ImVec2 tsz = f->CalcTextSizeA(fs, FLT_MAX, 0.0f, label);
                            const ImVec2 tpos = ImVec2(segMin.x + (segMax.x - segMin.x - tsz.x) * 0.5f, segMin.y + (segMax.y - segMin.y - tsz.y) * 0.5f);
                            dl->AddText(f, fs, tpos, theme.text, label);

                            if (ImGui::IsItemHovered()) {
                                ImGui::BeginTooltip();
                                if (isConfiguredLayoutRebind(rb, vk)) {
                                    if (isConsumeOnlyLayoutRebind(rb)) {
                                        ImGui::TextUnformatted(layoutNoneLabel);
                                    } else {
                                    DWORD triggerVkTip = resolveTriggerVkFor(rb, vk);
                                    DWORD triggerScanTip = resolveTriggerScanFor(rb, vk);
                                    const std::string typesTip = typesValueForDisplay(rb, vk);
                                    const std::string triggersTip = (triggerVkTip == 0)
                                                                        ? std::string(layoutNoneLabel)
                                                                        : normalizeMouseButtonLabel(scanCodeToDisplayName(triggerScanTip, triggerVkTip));
                                    ImGui::Text("Types: %s", typesTip.c_str());
                                    ImGui::Text("Triggers: %s", triggersTip.c_str());
                                    }
                                } else {
                                    ImGui::Text("Input: %s (%u)", VkToString(vk).c_str(), (unsigned)vk);
                                    ImGui::TextUnformatted("Right-click to configure rebinds.");
                                }
                                ImGui::EndTooltip();
                            }
                            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                                openRebindContextFor(vk, false);
                            }
                        };

                        const float wheelW = (innerMax.x - innerMin.x) * 0.16f;
                        const float wheelH = topH * 0.55f;
                        const ImVec2 wheelMin = ImVec2(midX - wheelW * 0.5f, innerMin.y + topH * 0.18f);
                        const ImVec2 wheelMax = ImVec2(midX + wheelW * 0.5f, wheelMin.y + wheelH);
                        const float wheelSideGap = 2.0f * keyboardScale;

                        const ImVec2 leftMin = innerMin;
                        const ImVec2 leftMax = ImVec2(wheelMin.x - wheelSideGap, splitY);
                        const ImVec2 rightMin = ImVec2(wheelMax.x + wheelSideGap, innerMin.y);
                        const ImVec2 rightMax = ImVec2(innerMax.x, splitY);

                        const float sideW = (innerMax.x - innerMin.x) * 0.32f;
                        const float sideH = (innerMax.y - innerMin.y) * 0.12f;
                        const float sideX0 = innerMin.x + (innerMax.x - innerMin.x) * 0.07f;
                        const float sideY0 = innerMin.y + topH + (innerMax.y - innerMin.y - topH) * 0.26f;
                        const float sideGap = sideH * 0.35f;
                        const ImVec2 side1Min = ImVec2(sideX0, sideY0);
                        const ImVec2 side1Max = ImVec2(sideX0 + sideW, sideY0 + sideH);
                        const ImVec2 side2Min = ImVec2(sideX0, sideY0 + sideH + sideGap);
                        const ImVec2 side2Max = ImVec2(sideX0 + sideW, sideY0 + sideH + sideGap + sideH);
                        const float scrollButtonGap = 3.0f * keyboardScale;
                        float scrollButtonW = wheelW - 4.0f * keyboardScale;
                        if (scrollButtonW < 20.0f * keyboardScale) scrollButtonW = 20.0f * keyboardScale;
                        const float scrollButtonHAvailTop = (wheelMin.y - innerMin.y) - scrollButtonGap;
                        const float scrollButtonHAvailBottom = (splitY - wheelMax.y) - scrollButtonGap;
                        float scrollButtonH = 10.0f * keyboardScale;
                        if (scrollButtonH > scrollButtonHAvailTop) scrollButtonH = scrollButtonHAvailTop;
                        if (scrollButtonH > scrollButtonHAvailBottom) scrollButtonH = scrollButtonHAvailBottom;
                        if (scrollButtonH < 8.0f * keyboardScale) scrollButtonH = 8.0f * keyboardScale;
                        const ImVec2 scrollUpMin =
                            ImVec2(midX - scrollButtonW * 0.5f, wheelMin.y - scrollButtonGap - scrollButtonH);
                        const ImVec2 scrollUpMax = ImVec2(midX + scrollButtonW * 0.5f, wheelMin.y - scrollButtonGap);
                        const ImVec2 scrollDownMin = ImVec2(midX - scrollButtonW * 0.5f, wheelMax.y + scrollButtonGap);
                        const ImVec2 scrollDownMax = ImVec2(midX + scrollButtonW * 0.5f, wheelMax.y + scrollButtonGap + scrollButtonH);

                        dl->AddLine(ImVec2(midX, innerMin.y + 2), ImVec2(midX, splitY - 2), IM_COL32(10, 10, 12, 255), 1.0f);
                        dl->AddLine(ImVec2(innerMin.x + 2, splitY), ImVec2(innerMax.x - 2, splitY), IM_COL32(10, 10, 12, 255), 1.0f);

                        ImGui::SetCursorScreenPos(leftMin);
                        ImGui::InvisibleButton("##mouse_left", ImVec2(leftMax.x - leftMin.x, leftMax.y - leftMin.y),
                                              ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
                        RecordGuiTestKeyboardLayoutKeyRect(VK_LBUTTON, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
#endif
                        drawMouseSegment(VK_LBUTTON, "MB1", leftMin, leftMax, bodyR * 0.75f, ImDrawFlags_RoundCornersTopLeft);

                        ImGui::SetCursorScreenPos(rightMin);
                        ImGui::InvisibleButton("##mouse_right", ImVec2(rightMax.x - rightMin.x, rightMax.y - rightMin.y),
                                              ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
                        RecordGuiTestKeyboardLayoutKeyRect(VK_RBUTTON, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
#endif
                        drawMouseSegment(VK_RBUTTON, "MB2", rightMin, rightMax, bodyR * 0.75f, ImDrawFlags_RoundCornersTopRight);

                        ImGui::SetCursorScreenPos(wheelMin);
                        ImGui::InvisibleButton("##mouse_mid", ImVec2(wheelMax.x - wheelMin.x, wheelMax.y - wheelMin.y),
                                              ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
                        RecordGuiTestKeyboardLayoutKeyRect(VK_MBUTTON, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
#endif
                        drawMouseSegment(VK_MBUTTON, "MB3", wheelMin, wheelMax, 6.0f * keyboardScale, ImDrawFlags_RoundCornersAll);

                        ImGui::SetCursorScreenPos(side1Min);
                        ImGui::InvisibleButton("##mouse_x1", ImVec2(side1Max.x - side1Min.x, side1Max.y - side1Min.y),
                                              ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
                        RecordGuiTestKeyboardLayoutKeyRect(VK_XBUTTON2, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
#endif
                        drawMouseSegment(VK_XBUTTON2, "MB5", side1Min, side1Max, 6.0f * keyboardScale, ImDrawFlags_RoundCornersAll);

                        ImGui::SetCursorScreenPos(side2Min);
                        ImGui::InvisibleButton("##mouse_x2", ImVec2(side2Max.x - side2Min.x, side2Max.y - side2Min.y),
                                              ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
                        RecordGuiTestKeyboardLayoutKeyRect(VK_XBUTTON1, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
#endif
                        drawMouseSegment(VK_XBUTTON1, "MB4", side2Min, side2Max, 6.0f * keyboardScale, ImDrawFlags_RoundCornersAll);

                        ImGui::SetCursorScreenPos(scrollUpMin);
                        ImGui::InvisibleButton("##mouse_scroll_up", ImVec2(scrollUpMax.x - scrollUpMin.x, scrollUpMax.y - scrollUpMin.y),
                                              ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
                        RecordGuiTestKeyboardLayoutKeyRect(VK_TOOLSCREEN_SCROLL_UP, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
#endif
                        drawMouseSegment(VK_TOOLSCREEN_SCROLL_UP, "UP", scrollUpMin, scrollUpMax, 6.0f * keyboardScale,
                                         ImDrawFlags_RoundCornersAll);

                        ImGui::SetCursorScreenPos(scrollDownMin);
                        ImGui::InvisibleButton("##mouse_scroll_down",
                                              ImVec2(scrollDownMax.x - scrollDownMin.x, scrollDownMax.y - scrollDownMin.y),
                                              ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
                        RecordGuiTestKeyboardLayoutKeyRect(VK_TOOLSCREEN_SCROLL_DOWN, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
#endif
                        drawMouseSegment(VK_TOOLSCREEN_SCROLL_DOWN, "DN", scrollDownMin, scrollDownMax, 6.0f * keyboardScale,
                                         ImDrawFlags_RoundCornersAll);
                    }

                    const float popupUiScale = std::clamp(s_keyboardLayoutScale / kDefaultKeyboardLayoutScale, 0.85f, 2.0f);
                    ImGui::SetNextWindowPos(ImGui::GetMousePos(), ImGuiCond_Appearing);
                    ImGui::SetNextWindowSizeConstraints(ImVec2(520.0f * popupUiScale, 0.0f), ImVec2(760.0f * popupUiScale, FLT_MAX));
                    if (ImGui::BeginPopup(trc("inputs.rebind_config"))) {
                        // Also block global ESC-to-close-GUI while editing inside this popup.
                        MarkRebindBindingActive();
                        ImGui::SetWindowFontScale(popupUiScale);
                        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * popupUiScale, 4.0f * popupUiScale));
                        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(7.0f * popupUiScale, 6.0f * popupUiScale));
                        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(4.0f * popupUiScale, 3.0f * popupUiScale));

                        const bool nestedLayoutPopupOpen = ImGui::IsPopupOpen(trc("inputs.triggers_custom")) ||
                                                           ImGui::IsPopupOpen(trc("inputs.custom_key_input_picker")) ||
                                                           ImGui::IsPopupOpen(trc("inputs.custom_unicode"));
                        if (ImGui::IsKeyPressed(ImGuiKey_Escape) && !nestedLayoutPopupOpen) {
                            if (s_layoutCustomInputCapturePending) {
                                s_layoutEscapeRequiresRelease = true;
                                layoutEscapeConsumedThisFrame = true;
                                s_layoutCustomInputCapturePending = false;
                            } else {
                            s_layoutEscapeRequiresRelease = true;
                            layoutEscapeConsumedThisFrame = true;
                            s_layoutBindTarget = LayoutBindTarget::None;
                            s_layoutBindIndex = -1;
                            s_layoutUnicodeEditIndex = -1;
                            s_layoutUnicodeEditTarget = LayoutUnicodeEditTarget::None;
                            s_layoutUnicodeEditText.clear();
                            ImGui::CloseCurrentPopup();
                            }
                        }

                        syncUnicodeEditBuffers();

                        auto isNoOpRebindForKey = [&](const KeyRebind& r, DWORD originalVk) -> bool {
                            if (r.fromKey != originalVk) return false;
                            if (r.triggerOutputDisabled) return false;
                            if (r.baseOutputDisabled) return false;
                            if (r.shiftLayerOutputDisabled) return false;
                            if (r.toKey != originalVk) return false;
                            if (r.customOutputVK != 0 && r.customOutputVK != r.toKey) return false;
                            if (r.customOutputUnicode != 0) return false;
                            if (r.customOutputScanCode != 0) return false;
                            if (r.baseOutputShifted) return false;
                            if (r.shiftLayerUsesCapsLock) return false;
                            if (hasShiftLayerOverride(&r, originalVk)) return false;
                            return true;
                        };

                        auto eraseRebindIndex = [&](int eraseIdx) {
                            if (eraseIdx < 0 || eraseIdx >= (int)g_config.keyRebinds.rebinds.size()) return;
                            g_config.keyRebinds.rebinds.erase(g_config.keyRebinds.rebinds.begin() + eraseIdx);
                            g_configIsDirty = true;
                            std::lock_guard<std::mutex> hotkeyLock(g_hotkeyMainKeysMutex);
                            RebuildHotkeyMainKeys_Internal();

                            auto decIfAfter = [&](int& v) {
                                if (v == -1) return;
                                if (v == eraseIdx) {
                                    v = -1;
                                } else if (v > eraseIdx) {
                                    v -= 1;
                                }
                            };
                            decIfAfter(s_rebindFromKeyToBind);
                            decIfAfter(s_rebindOutputVKToBind);
                            decIfAfter(s_rebindTextOverrideVKToBind);
                            decIfAfter(s_rebindOutputScanToBind);
                            decIfAfter(s_layoutBindIndex);
                            decIfAfter(s_layoutContextPreferredIndex);
                            decIfAfter(s_layoutUnicodeEditIndex);

                            syncUnicodeEditBuffers();
                        };

                        auto eraseRebindsForKeyAndCursorState = [&](DWORD fromVk, const char* cursorStateId) {
                            for (int eraseIdx = (int)g_config.keyRebinds.rebinds.size() - 1; eraseIdx >= 0; --eraseIdx) {
                                const auto& rebind = g_config.keyRebinds.rebinds[eraseIdx];
                                if (rebind.fromKey != fromVk || rebind.cursorState != cursorStateId) continue;
                                eraseRebindIndex(eraseIdx);
                            }
                        };

                        auto findBestRebindIndexForKey = [&](DWORD fromVk) -> int {
                            return findBestRebindIndexForCursorState(fromVk, getKeyboardLayoutCursorStateViewId());
                        };

                        // Do not create a rebind on right-click.
                        int idx = s_layoutContextPreferredIndex;
                        const char* currentCursorStateId = getKeyboardLayoutCursorStateViewId();
                        if (idx < 0 || idx >= (int)g_config.keyRebinds.rebinds.size() || g_config.keyRebinds.rebinds[idx].fromKey != s_layoutContextVk ||
                            g_config.keyRebinds.rebinds[idx].cursorState != currentCursorStateId) {
                            idx = findBestRebindIndexForKey(s_layoutContextVk);
                            s_layoutContextPreferredIndex = idx;
                        }

                        auto createRebindForKeyIfMissing = [&](DWORD fromVk) -> int {
                            const char* cursorStateId = getKeyboardLayoutCursorStateViewId();
                            int e = findBestRebindIndexForCursorState(fromVk, cursorStateId);
                            if (e >= 0) return e;

                            KeyRebind r;
                            if (strcmp(cursorStateId, kKeyRebindCursorStateAny) != 0) {
                                const int anyIdx = findBestRebindIndexForCursorState(fromVk, kKeyRebindCursorStateAny);
                                if (anyIdx >= 0 && anyIdx < (int)g_config.keyRebinds.rebinds.size()) {
                                    r = g_config.keyRebinds.rebinds[anyIdx];
                                }
                            }
                            if (r.fromKey == 0) {
                                r.fromKey = fromVk;
                                r.toKey = fromVk;
                                r.enabled = true;
                            }
                            r.fromKey = fromVk;
                            r.cursorState = cursorStateId;
                            g_config.keyRebinds.rebinds.push_back(r);
                            g_configIsDirty = true;
                            std::lock_guard<std::mutex> hotkeyLock(g_hotkeyMainKeysMutex);
                            RebuildHotkeyMainKeys_Internal();
                            syncUnicodeEditBuffers();
                            return (int)g_config.keyRebinds.rebinds.size() - 1;
                        };

                        auto typesValueFor = [&](const KeyRebind* rb, DWORD originalVk) -> std::string {
                            return typesValueForDisplay(rb, originalVk);
                        };

                        auto typesShiftValueFor = [&](const KeyRebind* rb, DWORD originalVk) -> std::string {
                            const std::string shiftValue = typesShiftValueForDisplay(rb, originalVk);
                            if (!hasShiftLayerOverride(rb, originalVk)) {
                                return tr("inputs.types_shift_inherits_format", shiftValue.c_str());
                            }
                            return shiftValue;
                        };

                        auto triggersValueFor = [&](const KeyRebind* rb, DWORD originalVk) -> std::string {
                            if (isConsumeOnlyLayoutRebind(rb)) return tr("label.disabled");
                            if (isConsumeOnlyLayoutRebind(rb)) return tr("label.none");
                            if (isTriggerOutputDisabled(rb)) return tr("label.none");
                            DWORD triggerVk = resolveTriggerVkFor(rb, originalVk);
                            if (triggerVk == 0) return tr("label.none");
                            DWORD displayScan = resolveTriggerScanFor(rb, originalVk);
                            return scanCodeToDisplayName(displayScan, triggerVk);
                        };

                        auto syncCustomOutputState = [&](KeyRebind& r) {
                            r.useCustomOutput = (r.customOutputVK != 0) || (r.customOutputUnicode != 0) || (r.customOutputScanCode != 0);
                        };

                        auto clearBaseTypesOverrideIfDefault = [&](KeyRebind& r, DWORD originalVk) {
                            if (r.baseOutputDisabled) {
                                syncCustomOutputState(r);
                                return;
                            }
                            if (r.customOutputUnicode == 0 && r.customOutputVK == originalVk && !r.baseOutputShifted) {
                                r.customOutputVK = 0;
                            }
                            syncCustomOutputState(r);
                        };

                        auto clearShiftLayerOverrideIfDefault = [&](KeyRebind& r, DWORD originalVk) {
                            if (!r.shiftLayerEnabled) {
                                r.shiftLayerUsesCapsLock = false;
                                r.shiftLayerOutputDisabled = false;
                                r.shiftLayerOutputVK = 0;
                                r.shiftLayerOutputUnicode = 0;
                                r.shiftLayerOutputShifted = false;
                                return;
                            }

                            if (r.shiftLayerOutputDisabled) {
                                r.shiftLayerOutputVK = 0;
                                r.shiftLayerOutputUnicode = 0;
                                r.shiftLayerOutputShifted = false;
                                return;
                            }

                            if (r.shiftLayerOutputUnicode != 0) {
                                if (r.shiftLayerOutputVK == 0) {
                                    r.shiftLayerOutputVK = resolveTypesVkFor(&r, originalVk, false);
                                }
                                return;
                            }

                            const DWORD baseTypesVk = resolveTypesVkFor(&r, originalVk, false);
                            if (r.shiftLayerOutputVK == 0) {
                                r.shiftLayerOutputVK = baseTypesVk;
                            }
                        };

                        auto resetLayoutRebindToDefault = [&](KeyRebind& r, bool enabled) {
                            r.toKey = r.fromKey;
                            r.triggerOutputDisabled = false;
                            r.baseOutputDisabled = false;
                            r.customOutputVK = 0;
                            r.customOutputUnicode = 0;
                            r.customOutputScanCode = 0;
                            r.baseOutputShifted = false;
                            r.shiftLayerEnabled = false;
                            r.shiftLayerUsesCapsLock = false;
                            r.shiftLayerOutputDisabled = false;
                            r.shiftLayerOutputVK = 0;
                            r.shiftLayerOutputUnicode = 0;
                            r.shiftLayerOutputShifted = false;
                            r.useCustomOutput = false;
                            r.enabled = enabled;
                        };

                        auto setFullRebindState = [&](KeyRebind& r, DWORD originalVk) {
                            DWORD unifiedVk = resolveTriggerVkFor(&r, originalVk);
                            if (unifiedVk == 0) unifiedVk = originalVk;

                            r.toKey = unifiedVk;
                            r.triggerOutputDisabled = false;
                            r.baseOutputDisabled = false;
                            r.customOutputUnicode = 0;
                            r.baseOutputShifted = false;
                            r.shiftLayerEnabled = false;
                            r.shiftLayerUsesCapsLock = false;
                            r.shiftLayerOutputDisabled = false;
                            r.shiftLayerOutputVK = 0;
                            r.shiftLayerOutputUnicode = 0;
                            r.shiftLayerOutputShifted = false;
                            r.customOutputVK = unifiedVk;
                            clearBaseTypesOverrideIfDefault(r, originalVk);
                        };

                        auto isSplitRebindUiMode = [&](const KeyRebind* rb, DWORD originalVk) -> bool {
                            if (!rb) return false;
                            if (rb->triggerOutputDisabled) return true;
                            if (rb->baseOutputDisabled) return true;
                            if (rb->shiftLayerOutputDisabled) return true;
                            if (rb->customOutputUnicode != 0) return true;
                            if (rb->shiftLayerOutputUnicode != 0) return true;
                            if (rb->baseOutputShifted) return true;
                            if (rb->shiftLayerUsesCapsLock) return true;
                            if (hasShiftLayerOverride(rb, originalVk)) return true;
                            return resolveTypesVkFor(rb, originalVk, false) != resolveTriggerVkFor(rb, originalVk);
                        };

                        struct KnownScanCode { DWORD scan; std::string name; int group; };
                        enum ScanGroup { SG_Alpha=0, SG_Digit, SG_Function, SG_Nav, SG_Numpad, SG_Modifier, SG_Other, SG_Raw, SG_COUNT };

                        static std::vector<KnownScanCode> s_knownScanCodes;
                        static bool s_knownScanCodesBuilt = false;
                        if (!s_knownScanCodesBuilt) {
                            struct ScanMenuEntry {
                                DWORD scan = 0;
                                std::string name;
                                std::string sortName;
                                int group = 0;
                                int order = 0;
                            };

                            s_knownScanCodes.clear();
                            s_knownScanCodes.reserve(0x1FE);
                            std::vector<ScanMenuEntry> entries;
                            entries.reserve(0x1FE);

                            auto toUpperAscii = [](std::string s) -> std::string {
                                for (char& ch : s) {
                                    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
                                }
                                return s;
                            };

                            auto navOrderForVk = [](DWORD vk) -> int {
                                switch (vk) {
                                case VK_INSERT: return 0;
                                case VK_HOME: return 1;
                                case VK_PRIOR: return 2;
                                case VK_DELETE: return 3;
                                case VK_END: return 4;
                                case VK_NEXT: return 5;
                                case VK_UP: return 6;
                                case VK_LEFT: return 7;
                                case VK_DOWN: return 8;
                                case VK_RIGHT: return 9;
                                case VK_SNAPSHOT: return 10;
                                case VK_SCROLL: return 11;
                                case VK_PAUSE: return 12;
                                default: return 1000 + (int)vk;
                                }
                            };

                            auto numpadOrderFor = [](DWORD vk, DWORD scan) -> int {
                                switch (vk) {
                                case VK_NUMLOCK: return 0;
                                case VK_DIVIDE: return 1;
                                case VK_MULTIPLY: return 2;
                                case VK_SUBTRACT: return 3;
                                case VK_NUMPAD7: return 4;
                                case VK_NUMPAD8: return 5;
                                case VK_NUMPAD9: return 6;
                                case VK_ADD: return 7;
                                case VK_NUMPAD4: return 8;
                                case VK_NUMPAD5: return 9;
                                case VK_NUMPAD6: return 10;
                                case VK_NUMPAD1: return 11;
                                case VK_NUMPAD2: return 12;
                                case VK_NUMPAD3: return 13;
                                case VK_RETURN: return ((scan & 0xFF00) != 0) ? 14 : 1000;
                                case VK_NUMPAD0: return 15;
                                case VK_DECIMAL: return 16;
                                default: return 1000 + (int)vk;
                                }
                            };

                            auto modifierOrderForVk = [](DWORD vk) -> int {
                                switch (vk) {
                                case VK_ESCAPE: return 0;
                                case VK_TAB: return 1;
                                case VK_CAPITAL: return 2;
                                case VK_SHIFT:
                                case VK_LSHIFT:
                                case VK_RSHIFT:
                                    return 3;
                                case VK_CONTROL:
                                case VK_LCONTROL:
                                case VK_RCONTROL:
                                    return 4;
                                case VK_MENU:
                                case VK_LMENU:
                                case VK_RMENU:
                                    return 5;
                                case VK_LWIN:
                                case VK_RWIN:
                                    return 6;
                                case VK_SPACE: return 7;
                                case VK_RETURN: return 8;
                                case VK_BACK: return 9;
                                case VK_APPS: return 10;
                                default: return 1000 + (int)vk;
                                }
                            };


                            auto classifyGroupOrder = [&](DWORD scan, DWORD vk) -> std::pair<int, int> {
                                if (vk >= 'A' && vk <= 'Z') {
                                    return { SG_Alpha, (int)(vk - 'A') };
                                }
                                if (vk >= '0' && vk <= '9') {
                                    return { SG_Digit, (int)(vk - '0') };
                                }
                                if (vk >= VK_F1 && vk <= VK_F24) {
                                    return { SG_Function, (int)(vk - VK_F1) };
                                }

                                switch (vk) {
                                case VK_INSERT:
                                case VK_HOME:
                                case VK_PRIOR:
                                case VK_DELETE:
                                case VK_END:
                                case VK_NEXT:
                                case VK_UP:
                                case VK_LEFT:
                                case VK_DOWN:
                                case VK_RIGHT:
                                case VK_SNAPSHOT:
                                case VK_SCROLL:
                                case VK_PAUSE:
                                    return { SG_Nav, navOrderForVk(vk) };
                                default:
                                    break;
                                }

                                const bool isNumpad =
                                    (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) || vk == VK_NUMLOCK || vk == VK_DIVIDE ||
                                    vk == VK_MULTIPLY || vk == VK_SUBTRACT || vk == VK_ADD || vk == VK_DECIMAL ||
                                    (vk == VK_RETURN && ((scan & 0xFF00) != 0));
                                if (isNumpad) {
                                    return { SG_Numpad, numpadOrderFor(vk, scan) };
                                }

                                switch (vk) {
                                case VK_ESCAPE:
                                case VK_TAB:
                                case VK_CAPITAL:
                                case VK_SHIFT:
                                case VK_LSHIFT:
                                case VK_RSHIFT:
                                case VK_CONTROL:
                                case VK_LCONTROL:
                                case VK_RCONTROL:
                                case VK_MENU:
                                case VK_LMENU:
                                case VK_RMENU:
                                case VK_LWIN:
                                case VK_RWIN:
                                case VK_SPACE:
                                case VK_RETURN:
                                case VK_BACK:
                                case VK_APPS:
                                    return { SG_Modifier, modifierOrderForVk(vk) };
                                default:
                                    break;
                                }

                                if (vk == 0) {
                                    return { SG_Raw, (int)scan };
                                }

                                return { SG_Other, (int)vk };
                            };

                            auto numpadVkForNonExtendedScan = [](DWORD scan) -> DWORD {
                                switch (scan) {
                                case 0x47: return VK_NUMPAD7;
                                case 0x48: return VK_NUMPAD8;
                                case 0x49: return VK_NUMPAD9;
                                case 0x4B: return VK_NUMPAD4;
                                case 0x4C: return VK_NUMPAD5;
                                case 0x4D: return VK_NUMPAD6;
                                case 0x4F: return VK_NUMPAD1;
                                case 0x50: return VK_NUMPAD2;
                                case 0x51: return VK_NUMPAD3;
                                case 0x4A: return VK_SUBTRACT;
                                case 0x4E: return VK_ADD;
                                case 0x52: return VK_NUMPAD0;
                                case 0x53: return VK_DECIMAL;
                                default: return 0;
                                }
                            };

                            auto appendScan = [&](DWORD scan) {
                                DWORD vkFromScan = MapVirtualKey(scan, MAPVK_VSC_TO_VK_EX);
                                if ((scan & 0xFF00) == 0) {
                                    DWORD npVk = numpadVkForNonExtendedScan(scan);
                                    if (npVk != 0) vkFromScan = npVk;
                                }
                                vkFromScan = normalizeModifierVkForDisplay(vkFromScan, scan);
                                std::string name = scanCodeToDisplayName(scan, vkFromScan);
                                const auto [group, order] = classifyGroupOrder(scan, vkFromScan);
                                ScanMenuEntry e;
                                e.scan = scan;
                                e.name = name;
                                e.sortName = toUpperAscii(name);
                                e.group = group;
                                e.order = order;
                                entries.push_back(std::move(e));
                            };

                            for (DWORD low = 1; low <= 0xFF; ++low) {
                                appendScan(low);
                            }

                            for (DWORD low = 1; low <= 0xFF; ++low) {
                                appendScan(0xE000 | low);
                            }

                            std::sort(entries.begin(), entries.end(), [](const ScanMenuEntry& a, const ScanMenuEntry& b) {
                                if (a.group != b.group) return a.group < b.group;
                                if (a.order != b.order) return a.order < b.order;
                                if (a.sortName != b.sortName) return a.sortName < b.sortName;
                                return a.scan < b.scan;
                            });

                            for (const auto& e : entries) {
                                s_knownScanCodes.push_back({ e.scan, e.name, e.group });
                            }

                            s_knownScanCodesBuilt = true;
                        }

                        auto formatScanHex = [](DWORD scan) -> std::string {
                            auto hex2 = [](unsigned v) -> std::string {
                                static const char* kHex = "0123456789ABCDEF";
                                std::string s;
                                s.push_back(kHex[(v >> 4) & 0xF]);
                                s.push_back(kHex[v & 0xF]);
                                return s;
                            };

                            const unsigned low = (unsigned)(scan & 0xFF);
                            if ((scan & 0xFF00) != 0) {
                                return std::string("E0 ") + hex2(low);
                            }
                            return hex2(low);
                        };

                        if (s_layoutBindTarget != LayoutBindTarget::None && s_layoutBindIndex >= 0 &&
                            s_layoutBindIndex < (int)g_config.keyRebinds.rebinds.size()) {
                            MarkRebindBindingActive();

                            DWORD capturedVk = 0;
                            LPARAM capturedLParam = 0;
                            bool capturedIsMouse = false;
                            if (ConsumeBindingInputEventSince(s_layoutBindLastSeq, capturedVk, capturedLParam, capturedIsMouse)) {
                                if (capturedVk == VK_ESCAPE) {
                                    auto& r = g_config.keyRebinds.rebinds[s_layoutBindIndex];
                                    int maybeErase = isNoOpRebindForKey(r, r.fromKey) ? s_layoutBindIndex : -1;
                                    s_layoutBindTarget = LayoutBindTarget::None;
                                    s_layoutBindIndex = -1;
                                    if (maybeErase != -1) {
                                        eraseRebindIndex(maybeErase);
                                        if (s_layoutContextPreferredIndex == maybeErase) s_layoutContextPreferredIndex = -1;
                                    }
                                } else {
                                    auto& r = g_config.keyRebinds.rebinds[s_layoutBindIndex];
                                    bool captureAccepted = true;
                                    if (s_layoutBindTarget == LayoutBindTarget::FullOutputVk) {
                                        r.customOutputScanCode = 0;
                                        r.toKey = capturedVk;
                                        setFullRebindState(r, r.fromKey);
                                        if (s_layoutBindIndex >= 0 && s_layoutBindIndex < (int)s_rebindUnicodeTextEdit.size()) {
                                            s_rebindUnicodeTextEdit[s_layoutBindIndex].clear();
                                        }
                                        g_configIsDirty = true;
                                    } else if (s_layoutBindTarget == LayoutBindTarget::TypesVk) {
                                        if (!canAcceptTypesVkCaptureFor(&r, r.fromKey, capturedVk)) {
                                            captureAccepted = false;
                                        } else {
                                            r.baseOutputDisabled = false;
                                            r.useCustomOutput = true;
                                            r.customOutputVK = capturedVk;
                                            r.customOutputUnicode = 0;
                                            if (s_layoutBindIndex >= 0 && s_layoutBindIndex < (int)s_rebindUnicodeTextEdit.size()) {
                                                s_rebindUnicodeTextEdit[s_layoutBindIndex].clear();
                                            }

                                            clearBaseTypesOverrideIfDefault(r, r.fromKey);
                                            g_configIsDirty = true;
                                        }
                                    } else if (s_layoutBindTarget == LayoutBindTarget::TypesVkShift) {
                                        if (!canAcceptTypesVkCaptureFor(&r, r.fromKey, capturedVk)) {
                                            captureAccepted = false;
                                        } else {
                                            r.shiftLayerEnabled = true;
                                            r.shiftLayerOutputDisabled = false;
                                            r.shiftLayerOutputVK = capturedVk;
                                            r.shiftLayerOutputUnicode = 0;

                                            clearShiftLayerOverrideIfDefault(r, r.fromKey);
                                            g_configIsDirty = true;
                                        }
                                    } else if (s_layoutBindTarget == LayoutBindTarget::TriggersVk) {
                                        r.toKey = capturedVk;
                                        r.triggerOutputDisabled = false;

                                        if (!s_layoutContextSplitMode) {
                                            r.customOutputScanCode = 0;
                                            setFullRebindState(r, r.fromKey);
                                        }
                                        g_configIsDirty = true;
                                    }

                                    if (captureAccepted) {
                                        if (isNoOpRebindForKey(r, r.fromKey)) {
                                            int eraseIdx = s_layoutBindIndex;
                                            s_layoutBindTarget = LayoutBindTarget::None;
                                            s_layoutBindIndex = -1;
                                            eraseRebindIndex(eraseIdx);
                                        } else {
                                            s_layoutBindTarget = LayoutBindTarget::None;
                                            s_layoutBindIndex = -1;
                                            (void)capturedLParam;
                                            (void)capturedIsMouse;
                                        }
                                    }
                                }
                            }
                        }

                        if (s_layoutUnicodeEditIndex != -1) {
                            MarkRebindBindingActive();
                            ImGui::OpenPopup(trc("inputs.custom_unicode"));
                        }

                        if (ImGui::BeginPopupModal(trc("inputs.custom_unicode"), NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
                            MarkRebindBindingActive();
                            ImGui::SetWindowFontScale(popupUiScale);
                            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                                s_layoutEscapeRequiresRelease = true;
                                layoutEscapeConsumedThisFrame = true;
                                s_layoutUnicodeEditIndex = -1;
                                s_layoutUnicodeEditTarget = LayoutUnicodeEditTarget::None;
                                s_layoutUnicodeEditText.clear();
                                ImGui::CloseCurrentPopup();
                            }
                            ImGui::TextUnformatted(trc("inputs.tooltip.enter_unicode_or_codepoint"));
                            ImGui::TextDisabled(trc("inputs.tooltip.unicode_examples"));
                            ImGui::Separator();
                            ImGui::SetNextItemWidth(260.0f * popupUiScale);
                            ImGui::InputTextWithHint("##unicode", trc("inputs.tooltip.unicode_hint"), &s_layoutUnicodeEditText);
                            ImGui::Spacing();

                            const bool canApply = true;
                            if (ImGui::Button(trc("button.apply"), ImVec2(120.0f * popupUiScale, 0)) && canApply) {
                                if (s_layoutUnicodeEditIndex >= 0 && s_layoutUnicodeEditIndex < (int)g_config.keyRebinds.rebinds.size()) {
                                    auto& r = g_config.keyRebinds.rebinds[s_layoutUnicodeEditIndex];
                                    if (s_layoutUnicodeEditTarget == LayoutUnicodeEditTarget::TypesShift) {
                                        r.shiftLayerEnabled = true;
                                        r.shiftLayerOutputDisabled = false;
                                        if (r.shiftLayerOutputVK == 0) {
                                            r.shiftLayerOutputVK = resolveTypesVkFor(&r, r.fromKey, false);
                                        }
                                        if (s_layoutUnicodeEditText.empty()) {
                                            r.shiftLayerOutputUnicode = 0;
                                            clearShiftLayerOverrideIfDefault(r, r.fromKey);
                                            g_configIsDirty = true;
                                        } else {
                                            uint32_t cp = 0;
                                            if (tryParseUnicodeInput(s_layoutUnicodeEditText, cp)) {
                                                r.shiftLayerOutputUnicode = (DWORD)cp;
                                                g_configIsDirty = true;
                                            }
                                        }
                                    } else {
                                        r.baseOutputDisabled = false;
                                        if (s_layoutUnicodeEditText.empty()) {
                                            r.customOutputUnicode = 0;
                                            if (r.customOutputVK == 0 && r.customOutputScanCode == 0) r.useCustomOutput = false;
                                            g_configIsDirty = true;
                                        } else {
                                            uint32_t cp = 0;
                                            if (tryParseUnicodeInput(s_layoutUnicodeEditText, cp)) {
                                                r.useCustomOutput = true;
                                                r.customOutputUnicode = (DWORD)cp;
                                                g_configIsDirty = true;
                                            }
                                        }
                                    }

                                    if (isNoOpRebindForKey(r, r.fromKey)) {
                                        int eraseIdx = s_layoutUnicodeEditIndex;
                                        s_layoutUnicodeEditIndex = -1;
                                        s_layoutUnicodeEditTarget = LayoutUnicodeEditTarget::None;
                                        s_layoutUnicodeEditText.clear();
                                        ImGui::CloseCurrentPopup();
                                        eraseRebindIndex(eraseIdx);
                                    }
                                }

                                s_layoutUnicodeEditIndex = -1;
                                s_layoutUnicodeEditTarget = LayoutUnicodeEditTarget::None;
                                s_layoutUnicodeEditText.clear();
                                ImGui::CloseCurrentPopup();
                            }
                            ImGui::SameLine();
                            if (ImGui::Button(trc("label.cancel"), ImVec2(120.0f * popupUiScale, 0))) {
                                if (s_layoutUnicodeEditIndex >= 0 && s_layoutUnicodeEditIndex < (int)g_config.keyRebinds.rebinds.size()) {
                                    auto& r = g_config.keyRebinds.rebinds[s_layoutUnicodeEditIndex];
                                    int maybeErase = isNoOpRebindForKey(r, r.fromKey) ? s_layoutUnicodeEditIndex : -1;
                                    if (maybeErase != -1) {
                                        eraseRebindIndex(maybeErase);
                                        if (s_layoutContextPreferredIndex == maybeErase) s_layoutContextPreferredIndex = -1;
                                    }
                                }
                                s_layoutUnicodeEditIndex = -1;
                                s_layoutUnicodeEditTarget = LayoutUnicodeEditTarget::None;
                                s_layoutUnicodeEditText.clear();
                                ImGui::CloseCurrentPopup();
                            }
                            ImGui::EndPopup();
                        }

                        KeyRebind* rbPtr = (idx >= 0 && idx < (int)g_config.keyRebinds.rebinds.size()) ? &g_config.keyRebinds.rebinds[idx] : nullptr;
                        if (ImGui::IsWindowAppearing()) {
                            s_layoutContextSplitMode = isSplitRebindUiMode(rbPtr, s_layoutContextVk);
                        }

                        ImGui::TextDisabled("%s %s", trc("inputs.rebind_applies_when"),
                                            getKeyboardLayoutCursorStateViewLabel(getKeyboardLayoutCursorStateViewId()));
                        ImGui::Separator();

                        const float bindButtonW = 132.0f * popupUiScale;
                        const float auxButtonW = 82.0f * popupUiScale;
                        const float labelColumnW = 142.0f * popupUiScale;
                        const float popupInlineGap = 6.0f * popupUiScale;
                        bool openTriggersCustomPopup = false;
                        ImVec2 triggersCustomPopupAnchor = ImVec2(0.0f, 0.0f);
                        const std::string typesValue = typesValueFor(rbPtr, s_layoutContextVk);
                        const std::string typesShiftValue = typesShiftValueFor(rbPtr, s_layoutContextVk);
                        const std::string triggersValue = triggersValueFor(rbPtr, s_layoutContextVk);
                        const bool disableTypeControls = cannotTypeFor(rbPtr, s_layoutContextVk);
                        const bool layoutContextIsScrollWheel = isScrollWheelVk(s_layoutContextVk);
                        const bool layoutContextKeyIsCustom = containsLayoutSourceKey(g_config.keyRebinds.layoutExtraKeys, s_layoutContextVk) ||
                                                              (!containsLayoutSourceKey(builtInLayoutKeys, s_layoutContextVk) &&
                                                               countRebindsForSourceKey(s_layoutContextVk) > 0);
                        const bool allowCustomLayoutInputPicker = layoutContextKeyIsCustom && s_layoutContextOpenedFromCustomKey;
                        bool openCustomInputPickerPopup = false;
                        ImVec2 customInputPickerPopupAnchor = ImVec2(0.0f, 0.0f);
                        auto beginCustomInputCapture = [&]() {
                            s_layoutCustomInputCapturePending = true;
                            s_layoutCustomInputCaptureLastSeq = GetLatestBindingInputSequence();
                            MarkRebindBindingActive();
                        };

                        auto getScrollWheelPopupLabel = [&](DWORD vk) -> const char* {
                            return (vk == VK_TOOLSCREEN_SCROLL_DOWN) ? trc("inputs.scroll_down_label") : trc("inputs.scroll_up_label");
                        };

                        auto setLayoutDisabledState = [&](LayoutDisableTarget target, bool disabled) {
                            if (target == LayoutDisableTarget::All) {
                                eraseRebindsForKeyAndCursorState(s_layoutContextVk, currentCursorStateId);
                                s_layoutContextPreferredIndex = -1;
                                idx = -1;

                                if (disabled) {
                                    idx = createRebindForKeyIfMissing(s_layoutContextVk);
                                    s_layoutContextPreferredIndex = idx;
                                    if (idx >= 0 && idx < (int)g_config.keyRebinds.rebinds.size()) {
                                        auto& r = g_config.keyRebinds.rebinds[idx];
                                        resetLayoutRebindToDefault(r, true);
                                        r.toKey = 0;
                                        g_configIsDirty = true;
                                        std::lock_guard<std::mutex> hotkeyLock(g_hotkeyMainKeysMutex);
                                        RebuildHotkeyMainKeys_Internal();
                                    }
                                }

                                rbPtr = (idx >= 0 && idx < (int)g_config.keyRebinds.rebinds.size()) ? &g_config.keyRebinds.rebinds[idx] : nullptr;
                                return;
                            }

                            if (!disabled && (idx < 0 || idx >= (int)g_config.keyRebinds.rebinds.size())) {
                                rbPtr = nullptr;
                                return;
                            }

                            idx = createRebindForKeyIfMissing(s_layoutContextVk);
                            s_layoutContextPreferredIndex = idx;
                            if (idx < 0 || idx >= (int)g_config.keyRebinds.rebinds.size()) {
                                rbPtr = nullptr;
                                return;
                            }

                            auto& r = g_config.keyRebinds.rebinds[idx];
                            switch (target) {
                            case LayoutDisableTarget::Types:
                                r.baseOutputDisabled = disabled;
                                if (disabled) {
                                    r.customOutputVK = 0;
                                    r.customOutputUnicode = 0;
                                    r.baseOutputShifted = false;
                                }
                                clearBaseTypesOverrideIfDefault(r, r.fromKey);
                                break;
                            case LayoutDisableTarget::TypesVkShift:
                                if (disabled) {
                                    r.shiftLayerEnabled = true;
                                    r.shiftLayerOutputDisabled = true;
                                    r.shiftLayerOutputVK = 0;
                                    r.shiftLayerOutputUnicode = 0;
                                    r.shiftLayerOutputShifted = false;
                                } else {
                                    r.shiftLayerOutputDisabled = false;
                                    if (!r.shiftLayerUsesCapsLock && r.shiftLayerOutputVK == 0 && r.shiftLayerOutputUnicode == 0 &&
                                        !r.shiftLayerOutputShifted) {
                                        r.shiftLayerEnabled = false;
                                    }
                                }
                                clearShiftLayerOverrideIfDefault(r, r.fromKey);
                                break;
                            case LayoutDisableTarget::Triggers:
                                r.triggerOutputDisabled = disabled;
                                if (disabled) {
                                    r.toKey = r.fromKey;
                                    r.customOutputScanCode = 0;
                                } else if (r.toKey == 0) {
                                    r.toKey = r.fromKey;
                                }
                                break;
                            case LayoutDisableTarget::All:
                                break;
                            }

                            g_configIsDirty = true;
                            if (isNoOpRebindForKey(r, r.fromKey)) {
                                eraseRebindIndex(idx);
                                s_layoutContextPreferredIndex = -1;
                                idx = -1;
                            }

                            rbPtr = (idx >= 0 && idx < (int)g_config.keyRebinds.rebinds.size()) ? &g_config.keyRebinds.rebinds[idx] : nullptr;
                        };

                        if (allowCustomLayoutInputPicker && s_layoutCustomInputCapturePending) {
                            MarkRebindBindingActive();

                            DWORD capturedVk = 0;
                            LPARAM capturedLParam = 0;
                            bool capturedIsMouse = false;
                            if (ConsumeBindingInputEventSince(s_layoutCustomInputCaptureLastSeq, capturedVk, capturedLParam, capturedIsMouse)) {
                                (void)capturedIsMouse;
                                if (capturedVk == VK_ESCAPE) {
                                    s_layoutCustomInputCapturePending = false;
                                } else {
                                    const DWORD sourceVk = normalizeCapturedLayoutSourceVk(capturedVk, capturedLParam);
                                    s_layoutCustomInputCapturePending = false;
                                    (void)moveCustomLayoutSourceKey(s_layoutContextVk, sourceVk);
                                }
                            }
                        }

#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
                        auto recordPopupInteractionRect = [&](const char* id, const ImVec2& fallbackMin, const ImVec2& fallbackMax) {
                            ImVec2 min = ImGui::GetItemRectMin();
                            ImVec2 max = ImGui::GetItemRectMax();
                            if (max.x <= min.x || max.y <= min.y) {
                                min = fallbackMin;
                                max = fallbackMax;
                            }
                            RecordGuiTestInteractionRect(id, min, max);
                        };

                        auto popupButtonFallbackMax = [&](const ImVec2& min, float width) {
                            return ImVec2(min.x + width, min.y + ImGui::GetFrameHeight());
                        };

                        auto popupToggleFallbackMax = [&](const char* label, const ImVec2& min) {
                            const ImGuiStyle& style = ImGui::GetStyle();
                            const float boxSize = ImGui::GetFrameHeight();
                            const float textWidth = ImGui::CalcTextSize(label).x;
                            return ImVec2(min.x + boxSize + style.ItemInnerSpacing.x + textWidth, min.y + boxSize);
                        };

                        if (const int requestedSplitMode = ConsumeGuiTestKeyboardLayoutSplitModeRequest(); requestedSplitMode != -1) {
                            const bool splitMode = requestedSplitMode != 0;
                            s_layoutContextSplitMode = splitMode;
                            if (!splitMode && idx >= 0 && idx < (int)g_config.keyRebinds.rebinds.size()) {
                                auto& r = g_config.keyRebinds.rebinds[idx];
                                setFullRebindState(r, r.fromKey);
                                g_configIsDirty = true;

                                if (isNoOpRebindForKey(r, r.fromKey)) {
                                    eraseRebindIndex(idx);
                                    s_layoutContextPreferredIndex = -1;
                                    idx = -1;
                                }
                            }
                            rbPtr = (idx >= 0 && idx < (int)g_config.keyRebinds.rebinds.size()) ? &g_config.keyRebinds.rebinds[idx] : nullptr;
                        }

                        GuiTestKeyboardLayoutDisableTarget requestedDisableTarget = GuiTestKeyboardLayoutDisableTarget::None;
                        bool requestedDisableState = false;
                        if (ConsumeGuiTestKeyboardLayoutDisableTargetRequest(requestedDisableTarget, requestedDisableState)) {
                            if (layoutContextIsScrollWheel) {
                                if (requestedDisableTarget == GuiTestKeyboardLayoutDisableTarget::All) {
                                    setLayoutDisabledState(LayoutDisableTarget::All, requestedDisableState);
                                }
                            } else {
                                switch (requestedDisableTarget) {
                                case GuiTestKeyboardLayoutDisableTarget::All:
                                    setLayoutDisabledState(LayoutDisableTarget::All, requestedDisableState);
                                    break;
                                case GuiTestKeyboardLayoutDisableTarget::Types:
                                    setLayoutDisabledState(LayoutDisableTarget::Types, requestedDisableState);
                                    break;
                                case GuiTestKeyboardLayoutDisableTarget::TypesVkShift:
                                    setLayoutDisabledState(LayoutDisableTarget::TypesVkShift, requestedDisableState);
                                    break;
                                case GuiTestKeyboardLayoutDisableTarget::Triggers:
                                    setLayoutDisabledState(LayoutDisableTarget::Triggers, requestedDisableState);
                                    break;
                                default:
                                    break;
                                }
                            }
                        } else if (const int requestedOutputDisabled = ConsumeGuiTestKeyboardLayoutOutputDisabledRequest();
                                   !layoutContextIsScrollWheel && requestedOutputDisabled != -1) {
                            setLayoutDisabledState(LayoutDisableTarget::All, requestedOutputDisabled != 0);
                        }

                        if (const int requestedScrollWheelEnabled = ConsumeGuiTestKeyboardLayoutScrollWheelEnabledRequest();
                            layoutContextIsScrollWheel && requestedScrollWheelEnabled != -1) {
                            setLayoutDisabledState(LayoutDisableTarget::All, requestedScrollWheelEnabled == 0);
                        }

                        if (const GuiTestKeyboardLayoutBindTarget requestedBindTarget = ConsumeGuiTestKeyboardLayoutBindTargetRequest();
                            requestedBindTarget != GuiTestKeyboardLayoutBindTarget::None) {
                            idx = createRebindForKeyIfMissing(s_layoutContextVk);
                            s_layoutContextPreferredIndex = idx;
                            if (idx >= 0) {
                                auto& r = g_config.keyRebinds.rebinds[idx];
                                switch (requestedBindTarget) {
                                case GuiTestKeyboardLayoutBindTarget::FullOutputVk:
                                    s_layoutBindTarget = LayoutBindTarget::FullOutputVk;
                                    break;
                                case GuiTestKeyboardLayoutBindTarget::TypesVk:
                                    s_layoutBindTarget = LayoutBindTarget::TypesVk;
                                    break;
                                case GuiTestKeyboardLayoutBindTarget::TypesVkShift:
                                    r.shiftLayerEnabled = true;
                                    s_layoutBindTarget = LayoutBindTarget::TypesVkShift;
                                    break;
                                case GuiTestKeyboardLayoutBindTarget::TriggersVk:
                                    s_layoutBindTarget = LayoutBindTarget::TriggersVk;
                                    break;
                                default:
                                    break;
                                }
                                s_layoutBindIndex = idx;
                                s_layoutBindLastSeq = GetLatestBindingInputSequence();
                                MarkRebindBindingActive();
                            }
                        }

                        if (const int requestedShiftUppercase = ConsumeGuiTestKeyboardLayoutShiftUppercaseRequest(); requestedShiftUppercase != -1) {
                            idx = createRebindForKeyIfMissing(s_layoutContextVk);
                            s_layoutContextPreferredIndex = idx;
                            if (idx >= 0) {
                                auto& r = g_config.keyRebinds.rebinds[idx];
                                r.shiftLayerEnabled = true;
                                r.shiftLayerOutputDisabled = false;
                                if (r.shiftLayerOutputVK == 0) {
                                    r.shiftLayerOutputVK = resolveTypesVkFor(&r, r.fromKey, false);
                                }
                                r.shiftLayerOutputShifted = requestedShiftUppercase != 0;
                                clearShiftLayerOverrideIfDefault(r, r.fromKey);
                                g_configIsDirty = true;

                                if (isNoOpRebindForKey(r, r.fromKey)) {
                                    eraseRebindIndex(idx);
                                    s_layoutContextPreferredIndex = -1;
                                    idx = -1;
                                }
                            }
                        }

                        if (const int requestedShiftCapsLock = ConsumeGuiTestKeyboardLayoutShiftCapsLockRequest(); requestedShiftCapsLock != -1) {
                            idx = createRebindForKeyIfMissing(s_layoutContextVk);
                            s_layoutContextPreferredIndex = idx;
                            if (idx >= 0) {
                                auto& r = g_config.keyRebinds.rebinds[idx];
                                r.shiftLayerEnabled = true;
                                if (r.shiftLayerOutputVK == 0) {
                                    r.shiftLayerOutputVK = resolveTypesVkFor(&r, r.fromKey, false);
                                }
                                r.shiftLayerUsesCapsLock = requestedShiftCapsLock != 0;
                                clearShiftLayerOverrideIfDefault(r, r.fromKey);
                                g_configIsDirty = true;

                                if (isNoOpRebindForKey(r, r.fromKey)) {
                                    eraseRebindIndex(idx);
                                    s_layoutContextPreferredIndex = -1;
                                    idx = -1;
                                }
                            }
                        }

                        if (ConsumeGuiTestKeyboardLayoutOpenScanPickerRequest()) {
                            openTriggersCustomPopup = true;
                            triggersCustomPopupAnchor = ImVec2(ImGui::GetCursorScreenPos().x, ImGui::GetCursorScreenPos().y + ImGui::GetFrameHeight());
                        }
#endif

                        if (layoutContextIsScrollWheel) {
                            const bool scrollWheelDisabled = isConsumeOnlyLayoutRebind(rbPtr);
                            const std::string scrollWheelLabel = getScrollWheelPopupLabel(s_layoutContextVk);
                            const ImVec2 scrollEnabledRectMin = ImGui::GetCursorScreenPos();
                            if (ImGui::RadioButton(scrollWheelLabel.c_str(), !scrollWheelDisabled)) {
                                setLayoutDisabledState(LayoutDisableTarget::All, false);
                            }
#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
                            recordPopupInteractionRect("inputs.keyboard_layout.popup.scroll_enabled", scrollEnabledRectMin,
                                                       popupToggleFallbackMax(scrollWheelLabel.c_str(), scrollEnabledRectMin));
#endif
                            const ImVec2 scrollDisabledRectMin = ImGui::GetCursorScreenPos();
                            if (ImGui::RadioButton(disableKeyLabel, scrollWheelDisabled)) {
                                setLayoutDisabledState(LayoutDisableTarget::All, true);
                            }
#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
                            recordPopupInteractionRect("inputs.keyboard_layout.popup.scroll_disabled", scrollDisabledRectMin,
                                                       popupToggleFallbackMax(disableKeyLabel, scrollDisabledRectMin));
#endif
                        } else {
                            const bool outputDisabled = isConsumeOnlyLayoutRebind(rbPtr);
                            const ImVec2 fullRebindRectMin = ImGui::GetCursorScreenPos();
                            if (ImGui::RadioButton(trc("inputs.full_rebind_label"), !s_layoutContextSplitMode && !outputDisabled)) {
                                s_layoutContextSplitMode = false;
                                if (idx >= 0 && idx < (int)g_config.keyRebinds.rebinds.size()) {
                                    auto& r = g_config.keyRebinds.rebinds[idx];
                                    setFullRebindState(r, r.fromKey);
                                    g_configIsDirty = true;

                                    if (isNoOpRebindForKey(r, r.fromKey)) {
                                        eraseRebindIndex(idx);
                                        s_layoutContextPreferredIndex = -1;
                                        idx = -1;
                                    }
                                }
                            }
#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
                            recordPopupInteractionRect("inputs.keyboard_layout.popup.full_rebind", fullRebindRectMin,
                                                       popupToggleFallbackMax(trc("inputs.full_rebind_label"), fullRebindRectMin));
#endif
                            const ImVec2 splitRebindRectMin = ImGui::GetCursorScreenPos();
                            if (ImGui::RadioButton(trc("inputs.split_rebind_label"), s_layoutContextSplitMode && !outputDisabled)) {
                                s_layoutContextSplitMode = true;
                                if (outputDisabled && idx >= 0 && idx < (int)g_config.keyRebinds.rebinds.size()) {
                                    eraseRebindIndex(idx);
                                    s_layoutContextPreferredIndex = -1;
                                    idx = -1;
                                }
                            }
#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
                            recordPopupInteractionRect("inputs.keyboard_layout.popup.split_rebind", splitRebindRectMin,
                                                       popupToggleFallbackMax(trc("inputs.split_rebind_label"), splitRebindRectMin));
#endif
                            const ImVec2 disabledOutputRectMin = ImGui::GetCursorScreenPos();
                            if (ImGui::RadioButton(disableKeyLabel, outputDisabled)) {
                                setLayoutDisabledState(LayoutDisableTarget::All, true);
                            }
#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
                            recordPopupInteractionRect("inputs.keyboard_layout.popup.output_disabled", disabledOutputRectMin,
                                                       popupToggleFallbackMax(disableKeyLabel, disabledOutputRectMin));
#endif

                            rbPtr = (idx >= 0 && idx < (int)g_config.keyRebinds.rebinds.size()) ? &g_config.keyRebinds.rebinds[idx] : nullptr;
                            if (!outputDisabled) {
                                ImGui::Dummy(ImVec2(0.0f, 4.0f * popupUiScale));
                                ImGui::Separator();

                                auto drawPopupRowLabel = [&](const char* tooltip, const char* label) {
                                const float rowBaseY = ImGui::GetCursorPosY();
                                const float markerYOffset = 2.0f * popupUiScale;
                                const float labelYOffset = -1.0f * popupUiScale;
                                ImGui::SetCursorPosY(rowBaseY + markerYOffset);
                                HelpMarker(tooltip);
                                ImGui::SameLine(0.0f, popupInlineGap);
                                ImGui::SetCursorPosY(rowBaseY + labelYOffset);
                                ImGui::AlignTextToFramePadding();
                                ImGui::TextUnformatted(label);
                            };

                            if (ImGui::BeginTable("##rebind_config_rows", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoPadOuterX)) {
                            ImGui::TableSetupColumn("##rebind_label", ImGuiTableColumnFlags_WidthFixed, labelColumnW);
                            ImGui::TableSetupColumn("##rebind_value", ImGuiTableColumnFlags_WidthStretch);

                            if (allowCustomLayoutInputPicker) {
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);
                                drawPopupRowLabel(trc("inputs.tooltip.custom_key_input"), trc("inputs.input_label"));
                                ImGui::TableSetColumnIndex(1);
                                {
                                    const std::string customInputValue = s_layoutCustomInputCapturePending
                                        ? std::string(trc("hotkeys.press_keys"))
                                        : getLayoutSourceButtonLabel(s_layoutContextVk);
                                    const ImVec2 customInputRectMin = ImGui::GetCursorScreenPos();
                                    if (ImGui::Button((customInputValue + "##custom_input_source").c_str(), ImVec2(bindButtonW, 0))) {
                                        beginCustomInputCapture();
                                    }
#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
                                    recordPopupInteractionRect("inputs.keyboard_layout.popup.custom_input", customInputRectMin,
                                                               popupButtonFallbackMax(customInputRectMin, bindButtonW));
                                    if (ConsumeGuiTestKeyboardLayoutBeginCustomInputCaptureRequest()) {
                                        beginCustomInputCapture();
                                    }
#endif
                                    ImGui::SameLine(0.0f, popupInlineGap);
                                    const ImVec2 customInputPickRectMin = ImGui::GetCursorScreenPos();
                                    if (ImGui::Button((tr("label.pick") + "##custom_input_pick").c_str(), ImVec2(auxButtonW, 0))) {
                                        s_layoutCustomInputCapturePending = false;
                                        openCustomInputPickerPopup = true;
                                        customInputPickerPopupAnchor =
                                            ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + 4.0f * popupUiScale);
                                    }
#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
                                    recordPopupInteractionRect("inputs.keyboard_layout.popup.custom_input_pick", customInputPickRectMin,
                                                               popupButtonFallbackMax(customInputPickRectMin, auxButtonW));
                                    if (ConsumeGuiTestKeyboardLayoutOpenCustomInputPickerRequest()) {
                                        openCustomInputPickerPopup = true;
                                        customInputPickerPopupAnchor =
                                            ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + 4.0f * popupUiScale);
                                    }
#endif
                                }
                            }

                            if (!s_layoutContextSplitMode) {
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);
                                drawPopupRowLabel(trc("inputs.tooltip.rebind_output"), trc("inputs.output_label"));
                                ImGui::TableSetColumnIndex(1);
                                {
                                    std::string label = (s_layoutBindTarget == LayoutBindTarget::FullOutputVk)
                                        ? std::string(trc("hotkeys.press_keys"))
                                        : triggersValue;
                                    const ImVec2 outputRectMin = ImGui::GetCursorScreenPos();
                                    if (ImGui::Button((label + "##output_full").c_str(), ImVec2(bindButtonW, 0))) {
                                        idx = createRebindForKeyIfMissing(s_layoutContextVk);
                                        s_layoutContextPreferredIndex = idx;
                                        if (idx >= 0) {
                                            s_layoutBindTarget = LayoutBindTarget::FullOutputVk;
                                            s_layoutBindIndex = idx;
                                            s_layoutBindLastSeq = GetLatestBindingInputSequence();
                                            MarkRebindBindingActive();
                                        }
                                    }
#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
                                    recordPopupInteractionRect("inputs.keyboard_layout.popup.output", outputRectMin,
                                                               popupButtonFallbackMax(outputRectMin, bindButtonW));
#endif
                                    ImGui::SameLine(0.0f, popupInlineGap);
                                    if (ImGui::Button((tr("label.pick") + "##output_scan_pick").c_str(), ImVec2(auxButtonW, 0))) {
                                        openTriggersCustomPopup = true;
                                        triggersCustomPopupAnchor =
                                            ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + 4.0f * popupUiScale);
                                    }
                                }
                            } else {
                                const bool typesDisabled = isBaseTypesOutputDisabled(rbPtr);
                                const bool shiftTypesDisabled = isShiftTypesOutputDisabled(rbPtr);
                                const bool triggersDisabled = isTriggerOutputDisabled(rbPtr);

                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);
                                drawPopupRowLabel(trc("inputs.tooltip.rebind_types"), trc("inputs.types_label"));
                                ImGui::TableSetColumnIndex(1);
                                {
                                    ImGui::BeginDisabled(disableTypeControls);
                                    std::string label = (s_layoutBindTarget == LayoutBindTarget::TypesVk)
                                        ? std::string(trc("hotkeys.press_keys"))
                                        : typesValue;
                                    const ImVec2 typesRectMin = ImGui::GetCursorScreenPos();
                                    if (ImGui::Button((label + "##types").c_str(), ImVec2(bindButtonW, 0))) {
                                        idx = createRebindForKeyIfMissing(s_layoutContextVk);
                                        s_layoutContextPreferredIndex = idx;
                                        if (idx >= 0) {
                                            s_layoutBindTarget = LayoutBindTarget::TypesVk;
                                            s_layoutBindIndex = idx;
                                            s_layoutBindLastSeq = GetLatestBindingInputSequence();
                                            MarkRebindBindingActive();
                                        }
                                    }
#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
                                    recordPopupInteractionRect("inputs.keyboard_layout.popup.types", typesRectMin,
                                                               popupButtonFallbackMax(typesRectMin, bindButtonW));
#endif
                                    ImGui::SameLine(0.0f, popupInlineGap);
                                    if (ImGui::Button((tr("label.custom") + "##types_custom").c_str(), ImVec2(auxButtonW, 0))) {
                                        idx = createRebindForKeyIfMissing(s_layoutContextVk);
                                        s_layoutContextPreferredIndex = idx;
                                        if (idx >= 0) {
                                            g_config.keyRebinds.rebinds[idx].baseOutputDisabled = false;
                                            s_layoutUnicodeEditIndex = idx;
                                            s_layoutUnicodeEditTarget = LayoutUnicodeEditTarget::TypesBase;
                                            const auto& r = g_config.keyRebinds.rebinds[idx];
                                            s_layoutUnicodeEditText =
                                                (r.customOutputUnicode != 0) ? formatCodepointUPlus((uint32_t)r.customOutputUnicode) : std::string();
                                            MarkRebindBindingActive();
                                        }
                                    }
                                    ImGui::SameLine(0.0f, popupInlineGap);
                                    ImGui::BeginDisabled(typesDisabled);
                                    ImGui::AlignTextToFramePadding();
                                    bool baseOutputShifted = rbPtr ? rbPtr->baseOutputShifted : false;
                                    const ImVec2 typesUppercaseRectMin = ImGui::GetCursorScreenPos();
                                    if (ImGui::Checkbox((std::string(trc("inputs.uppercase_label")) + "##types_uppercase").c_str(), &baseOutputShifted)) {
                                        idx = createRebindForKeyIfMissing(s_layoutContextVk);
                                        s_layoutContextPreferredIndex = idx;
                                        if (idx >= 0) {
                                            auto& r = g_config.keyRebinds.rebinds[idx];
                                            r.baseOutputDisabled = false;
                                            r.baseOutputShifted = baseOutputShifted;
                                            clearBaseTypesOverrideIfDefault(r, r.fromKey);
                                            g_configIsDirty = true;

                                            if (isNoOpRebindForKey(r, r.fromKey)) {
                                                eraseRebindIndex(idx);
                                                s_layoutContextPreferredIndex = -1;
                                                idx = -1;
                                            }
                                        }
                                    }
                                    ImGui::EndDisabled();
#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
                                    recordPopupInteractionRect("inputs.keyboard_layout.popup.types_uppercase", typesUppercaseRectMin,
                                                               popupToggleFallbackMax(trc("inputs.uppercase_label"), typesUppercaseRectMin));
#endif
                                    ImGui::SameLine(0.0f, popupInlineGap);
                                    ImGui::AlignTextToFramePadding();
                                    bool typesDisabledToggle = typesDisabled;
                                    const ImVec2 typesDisabledRectMin = ImGui::GetCursorScreenPos();
                                    if (ImGui::Checkbox((std::string(disableKeyLabel) + "##types_disabled").c_str(), &typesDisabledToggle)) {
                                        setLayoutDisabledState(LayoutDisableTarget::Types, typesDisabledToggle);
                                    }
#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
                                    recordPopupInteractionRect("inputs.keyboard_layout.popup.types_disabled", typesDisabledRectMin,
                                                               popupToggleFallbackMax(disableKeyLabel, typesDisabledRectMin));
#endif
                                    ImGui::EndDisabled();
                                }

                                rbPtr = (idx >= 0 && idx < (int)g_config.keyRebinds.rebinds.size()) ? &g_config.keyRebinds.rebinds[idx] : nullptr;

                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);
                                drawPopupRowLabel(trc("inputs.tooltip.rebind_types_shift"), trc("inputs.types_shift_label"));
                                ImGui::TableSetColumnIndex(1);
                                {
                                    ImGui::BeginDisabled(disableTypeControls);
                                    std::string label = (s_layoutBindTarget == LayoutBindTarget::TypesVkShift)
                                        ? std::string(trc("hotkeys.press_keys"))
                                        : typesShiftValue;
                                    const ImVec2 typesShiftRectMin = ImGui::GetCursorScreenPos();
                                    if (ImGui::Button((label + "##types_shift").c_str(), ImVec2(bindButtonW, 0))) {
                                        idx = createRebindForKeyIfMissing(s_layoutContextVk);
                                        s_layoutContextPreferredIndex = idx;
                                        if (idx >= 0) {
                                            auto& r = g_config.keyRebinds.rebinds[idx];
                                            r.shiftLayerEnabled = true;
                                            s_layoutBindTarget = LayoutBindTarget::TypesVkShift;
                                            s_layoutBindIndex = idx;
                                            s_layoutBindLastSeq = GetLatestBindingInputSequence();
                                            MarkRebindBindingActive();
                                        }
                                    }
#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
                                    recordPopupInteractionRect("inputs.keyboard_layout.popup.types_shift", typesShiftRectMin,
                                                               popupButtonFallbackMax(typesShiftRectMin, bindButtonW));
#endif
                                    ImGui::SameLine(0.0f, popupInlineGap);
                                    if (ImGui::Button((tr("label.custom") + "##types_shift_custom").c_str(), ImVec2(auxButtonW, 0))) {
                                        idx = createRebindForKeyIfMissing(s_layoutContextVk);
                                        s_layoutContextPreferredIndex = idx;
                                        if (idx >= 0) {
                                            auto& r = g_config.keyRebinds.rebinds[idx];
                                            r.shiftLayerEnabled = true;
                                            r.shiftLayerOutputDisabled = false;
                                            s_layoutUnicodeEditIndex = idx;
                                            s_layoutUnicodeEditTarget = LayoutUnicodeEditTarget::TypesShift;
                                            s_layoutUnicodeEditText =
                                                (r.shiftLayerOutputUnicode != 0)
                                                    ? formatCodepointUPlus((uint32_t)r.shiftLayerOutputUnicode)
                                                    : std::string();
                                            MarkRebindBindingActive();
                                        }
                                    }
                                    ImGui::SameLine(0.0f, popupInlineGap);
                                    ImGui::BeginDisabled(shiftTypesDisabled);
                                    ImGui::AlignTextToFramePadding();
                                    bool shiftLayerOutputShifted = rbPtr ? rbPtr->shiftLayerOutputShifted : false;
                                    const ImVec2 typesShiftUppercaseRectMin = ImGui::GetCursorScreenPos();
                                    if (ImGui::Checkbox((std::string(trc("inputs.uppercase_label")) + "##types_shift_uppercase").c_str(),
                                                        &shiftLayerOutputShifted)) {
                                        idx = createRebindForKeyIfMissing(s_layoutContextVk);
                                        s_layoutContextPreferredIndex = idx;
                                        if (idx >= 0) {
                                            auto& r = g_config.keyRebinds.rebinds[idx];
                                            r.shiftLayerEnabled = true;
                                            r.shiftLayerOutputDisabled = false;
                                            if (r.shiftLayerOutputVK == 0) {
                                                r.shiftLayerOutputVK = resolveTypesVkFor(&r, r.fromKey, false);
                                            }
                                            r.shiftLayerOutputShifted = shiftLayerOutputShifted;
                                            clearShiftLayerOverrideIfDefault(r, r.fromKey);
                                            g_configIsDirty = true;

                                            if (isNoOpRebindForKey(r, r.fromKey)) {
                                                eraseRebindIndex(idx);
                                                s_layoutContextPreferredIndex = -1;
                                                idx = -1;
                                            }
                                        }
                                    }
                                    ImGui::EndDisabled();
#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
                                    recordPopupInteractionRect("inputs.keyboard_layout.popup.types_shift_uppercase", typesShiftUppercaseRectMin,
                                                               popupToggleFallbackMax(trc("inputs.uppercase_label"), typesShiftUppercaseRectMin));
#endif
                                    ImGui::SameLine(0.0f, popupInlineGap);
                                    ImGui::AlignTextToFramePadding();
                                    bool shiftTypesDisabledToggle = shiftTypesDisabled;
                                    const ImVec2 typesShiftDisabledRectMin = ImGui::GetCursorScreenPos();
                                    if (ImGui::Checkbox((std::string(disableKeyLabel) + "##types_shift_disabled").c_str(),
                                                        &shiftTypesDisabledToggle)) {
                                        setLayoutDisabledState(LayoutDisableTarget::TypesVkShift, shiftTypesDisabledToggle);
                                    }
#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
                                    recordPopupInteractionRect("inputs.keyboard_layout.popup.types_shift_disabled", typesShiftDisabledRectMin,
                                                               popupToggleFallbackMax(disableKeyLabel, typesShiftDisabledRectMin));
#endif
                                    ImGui::EndDisabled();
                                }

                                rbPtr = (idx >= 0 && idx < (int)g_config.keyRebinds.rebinds.size()) ? &g_config.keyRebinds.rebinds[idx] : nullptr;

                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);
                                drawPopupRowLabel(trc("inputs.tooltip.rebind_types_shift_caps_lock"), trc("inputs.shift_layer_label"));
                                ImGui::TableSetColumnIndex(1);
                                {
                                    ImGui::BeginDisabled(disableTypeControls);
                                    bool shiftLayerUsesCapsLock = rbPtr ? rbPtr->shiftLayerUsesCapsLock : false;
                                    const ImVec2 shiftCapsLockRectMin = ImGui::GetCursorScreenPos();
                                    if (ImGui::Checkbox(trc("inputs.shift_layer_caps_lock_label"), &shiftLayerUsesCapsLock)) {
                                        idx = createRebindForKeyIfMissing(s_layoutContextVk);
                                        s_layoutContextPreferredIndex = idx;
                                        if (idx >= 0) {
                                            auto& r = g_config.keyRebinds.rebinds[idx];
                                            r.shiftLayerEnabled = true;
                                            if (r.shiftLayerOutputVK == 0) {
                                                r.shiftLayerOutputVK = resolveTypesVkFor(&r, r.fromKey, false);
                                            }
                                            r.shiftLayerUsesCapsLock = shiftLayerUsesCapsLock;
                                            clearShiftLayerOverrideIfDefault(r, r.fromKey);
                                            g_configIsDirty = true;

                                            if (isNoOpRebindForKey(r, r.fromKey)) {
                                                eraseRebindIndex(idx);
                                                s_layoutContextPreferredIndex = -1;
                                                idx = -1;
                                            }
                                        }
                                    }
#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
                                    recordPopupInteractionRect("inputs.keyboard_layout.popup.shift_caps_lock", shiftCapsLockRectMin,
                                                               popupToggleFallbackMax(trc("inputs.shift_layer_caps_lock_label"), shiftCapsLockRectMin));
#endif
                                    ImGui::EndDisabled();
                                }

                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);
                                drawPopupRowLabel(trc("inputs.tooltip.rebind_triggers"), trc("inputs.triggers_label"));
                                ImGui::TableSetColumnIndex(1);
                                {
                                    std::string label = (s_layoutBindTarget == LayoutBindTarget::TriggersVk)
                                        ? std::string(trc("hotkeys.press_keys"))
                                        : triggersValue;
                                    const ImVec2 triggersRectMin = ImGui::GetCursorScreenPos();
                                    if (ImGui::Button((label + "##triggers").c_str(), ImVec2(bindButtonW, 0))) {
                                        idx = createRebindForKeyIfMissing(s_layoutContextVk);
                                        s_layoutContextPreferredIndex = idx;
                                        if (idx >= 0) {
                                            g_config.keyRebinds.rebinds[idx].triggerOutputDisabled = false;
                                            s_layoutBindTarget = LayoutBindTarget::TriggersVk;
                                            s_layoutBindIndex = idx;
                                            s_layoutBindLastSeq = GetLatestBindingInputSequence();
                                            MarkRebindBindingActive();
                                        }
                                    }
#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
                                    recordPopupInteractionRect("inputs.keyboard_layout.popup.triggers", triggersRectMin,
                                                               popupButtonFallbackMax(triggersRectMin, bindButtonW));
#endif
                                    ImGui::SameLine(0.0f, popupInlineGap);
                                    if (ImGui::Button((tr("label.pick") + "##triggers_scan_pick").c_str(), ImVec2(auxButtonW, 0))) {
                                        if (idx >= 0 && idx < (int)g_config.keyRebinds.rebinds.size()) {
                                            g_config.keyRebinds.rebinds[idx].triggerOutputDisabled = false;
                                        }
                                        openTriggersCustomPopup = true;
                                        triggersCustomPopupAnchor =
                                            ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + 4.0f * popupUiScale);
                                    }
                                    ImGui::SameLine(0.0f, popupInlineGap);
                                    ImGui::AlignTextToFramePadding();
                                    bool triggersDisabledToggle = triggersDisabled;
                                    const ImVec2 triggersDisabledRectMin = ImGui::GetCursorScreenPos();
                                    if (ImGui::Checkbox((std::string(disableKeyLabel) + "##triggers_disabled").c_str(),
                                                        &triggersDisabledToggle)) {
                                        setLayoutDisabledState(LayoutDisableTarget::Triggers, triggersDisabledToggle);
                                    }
#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
                                    recordPopupInteractionRect("inputs.keyboard_layout.popup.triggers_disabled", triggersDisabledRectMin,
                                                               popupToggleFallbackMax(disableKeyLabel, triggersDisabledRectMin));
#endif
                                }
                            }

                                ImGui::EndTable();
                            }
                            }
                        }

                        if (openCustomInputPickerPopup) {
                            ImGui::SetNextWindowPos(customInputPickerPopupAnchor, ImGuiCond_Appearing);
                            ImGui::OpenPopup(trc("inputs.custom_key_input_picker"));
                        }
                        ImGui::SetNextWindowSizeConstraints(ImVec2(340.0f * popupUiScale, 220.0f * popupUiScale),
                                                            ImVec2(520.0f * popupUiScale, 460.0f * popupUiScale));

                        if (ImGui::BeginPopup(trc("inputs.custom_key_input_picker"))) {
                                MarkRebindBindingActive();
                                ImGui::SetWindowFontScale(popupUiScale);
                                if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                                    s_layoutEscapeRequiresRelease = true;
                                    layoutEscapeConsumedThisFrame = true;
                                    ImGui::CloseCurrentPopup();
                                }

                                const std::string currentSourceText = tr("inputs.current_format", getLayoutSourceButtonLabel(s_layoutContextVk).c_str());
                                ImGui::TextUnformatted(currentSourceText.c_str());
                                ImGui::Separator();

                                static int s_customInputFilterGroup = -1;
                                if (ImGui::IsWindowAppearing()) s_customInputFilterGroup = -1;

                                const char* groupLabels[SG_COUNT + 1] = {
                                    trc("inputs.scan_group_all"), trc("inputs.scan_group_alpha"), trc("inputs.scan_group_digit"),
                                    trc("inputs.scan_group_function"), trc("inputs.scan_group_nav"), trc("inputs.scan_group_numpad"),
                                    trc("inputs.scan_group_modifier"), trc("inputs.scan_group_other")
                                };
                                for (int g = -1; g < SG_Raw; ++g) {
                                    if (g > -1) ImGui::SameLine();
                                    bool active = (s_customInputFilterGroup == g);
                                    if (active) {
                                        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                                    }
                                    if (ImGui::Button(groupLabels[g + 1])) { s_customInputFilterGroup = g; }
                                    if (active) { ImGui::PopStyleColor(2); }
                                }
                                ImGui::Spacing();

                                auto tryApplyCustomInputSource = [&](DWORD requestedVk) -> bool {
                                    if (!moveCustomLayoutSourceKey(s_layoutContextVk, requestedVk)) return false;
                                    ImGui::CloseCurrentPopup();
                                    return true;
                                };

#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
                                if (const DWORD requestedScan = ConsumeGuiTestKeyboardLayoutSelectCustomInputScanRequest(); requestedScan != 0) {
                                    (void)tryApplyCustomInputSource(customLayoutSourceVkFromScan(requestedScan));
                                }
#endif

                                ImGui::BeginChild("##custom_input_source_list", ImVec2(0.0f, 260.0f * popupUiScale), true);
                                std::unordered_set<DWORD> shownCustomInputVks;
                                auto appendCustomInputOption = [&](DWORD optionVk, const std::string& optionLabel, int optionGroup) {
                                    if (optionVk == 0) return;
                                    if (s_customInputFilterGroup >= 0 && optionGroup != s_customInputFilterGroup) return;
                                    if (!shownCustomInputVks.insert(optionVk).second) return;
                                    if (optionVk != s_layoutContextVk && !canMoveCustomLayoutSourceKey(s_layoutContextVk, optionVk)) return;

                                    const bool selected = optionVk == s_layoutContextVk;
                                    const std::string itemLabel = optionLabel + "##custom_input_option_" + std::to_string((unsigned)optionVk);
                                    if (ImGui::Selectable(itemLabel.c_str(), selected)) {
                                        (void)tryApplyCustomInputSource(optionVk);
                                    }
                                };

                                for (const auto& it : s_knownScanCodes) {
                                    if (it.group == SG_Raw) continue;
                                    const DWORD optionVk = customLayoutSourceVkFromScan(it.scan);
                                    if (optionVk == 0) continue;
                                    appendCustomInputOption(optionVk, getLayoutSourceButtonLabel(optionVk), it.group);
                                }

                                appendCustomInputOption(VK_LBUTTON, getLayoutSourceButtonLabel(VK_LBUTTON), SG_Other);
                                appendCustomInputOption(VK_RBUTTON, getLayoutSourceButtonLabel(VK_RBUTTON), SG_Other);
                                appendCustomInputOption(VK_MBUTTON, getLayoutSourceButtonLabel(VK_MBUTTON), SG_Other);
                                appendCustomInputOption(VK_XBUTTON1, getLayoutSourceButtonLabel(VK_XBUTTON1), SG_Other);
                                appendCustomInputOption(VK_XBUTTON2, getLayoutSourceButtonLabel(VK_XBUTTON2), SG_Other);
                                appendCustomInputOption(VK_TOOLSCREEN_SCROLL_UP, getLayoutSourceButtonLabel(VK_TOOLSCREEN_SCROLL_UP), SG_Other);
                                appendCustomInputOption(VK_TOOLSCREEN_SCROLL_DOWN, getLayoutSourceButtonLabel(VK_TOOLSCREEN_SCROLL_DOWN), SG_Other);
                                ImGui::EndChild();

                                ImGui::EndPopup();
                        }

                        idx = (idx >= 0) ? idx : findBestRebindIndexForKey(s_layoutContextVk);
                        KeyRebind* r = (idx >= 0 && idx < (int)g_config.keyRebinds.rebinds.size()) ? &g_config.keyRebinds.rebinds[idx] : nullptr;

                        if (openTriggersCustomPopup) {
                            ImGui::SetNextWindowPos(triggersCustomPopupAnchor, ImGuiCond_Appearing);
                            ImGui::OpenPopup(trc("inputs.triggers_custom"));
                        }
                        ImGui::SetNextWindowSizeConstraints(ImVec2(340.0f * popupUiScale, 220.0f * popupUiScale),
                                                            ImVec2(520.0f * popupUiScale, 460.0f * popupUiScale));

                        if (ImGui::BeginPopup(trc("inputs.triggers_custom"))) {
                                MarkRebindBindingActive();
                                ImGui::SetWindowFontScale(popupUiScale);
                                if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                                    s_layoutEscapeRequiresRelease = true;
                                    layoutEscapeConsumedThisFrame = true;
                                    ImGui::CloseCurrentPopup();
                                }

                                DWORD curTriggerVk = r ? r->toKey : s_layoutContextVk;
                                if (curTriggerVk == 0) curTriggerVk = s_layoutContextVk;
                                DWORD curScan = (r && r->useCustomOutput && r->customOutputScanCode != 0) ? r->customOutputScanCode
                                                                                                          : getScanCodeWithExtendedFlag(curTriggerVk);
                                std::string preview = scanCodeToDisplayName(curScan, curTriggerVk);
                                const DWORD defaultScan = getScanCodeWithExtendedFlag(curTriggerVk);
                                const std::string defaultPreview = scanCodeToDisplayName(defaultScan, curTriggerVk);

                                const std::string currentPreviewText = tr("inputs.current_format", preview.c_str());
                                ImGui::TextUnformatted(currentPreviewText.c_str());
                                ImGui::Separator();

                                static int s_scanFilterGroup = -1; // -1 = All
                                if (ImGui::IsWindowAppearing()) s_scanFilterGroup = -1;
#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
                                if (const int requestedScanFilterGroup = ConsumeGuiTestKeyboardLayoutScanFilterRequest();
                                    requestedScanFilterGroup >= -1 && requestedScanFilterGroup < SG_Raw) {
                                    s_scanFilterGroup = requestedScanFilterGroup;
                                }
#endif

                                auto clearTriggerCustomScan = [&]() {
                                    if (idx < 0 || idx >= (int)g_config.keyRebinds.rebinds.size()) return;
                                    auto& rr = g_config.keyRebinds.rebinds[idx];
                                    rr.customOutputScanCode = 0;
                                    if (rr.customOutputVK == 0 && rr.customOutputUnicode == 0) rr.useCustomOutput = false;
                                    g_configIsDirty = true;
                                    r = &rr;
                                };

                                auto tryApplyTriggerCustomScan = [&](DWORD requestedScan) -> bool {
                                    const auto found = std::find_if(s_knownScanCodes.begin(), s_knownScanCodes.end(),
                                                                    [&](const KnownScanCode& known) { return known.scan == requestedScan; });
                                    if (found == s_knownScanCodes.end()) return false;
                                    if (found->group == SG_Raw) return false;
                                    if (s_scanFilterGroup >= 0 && found->group != s_scanFilterGroup) return false;

                                    const DWORD requestedVk = customLayoutSourceVkFromScan(requestedScan);
                                    if (requestedVk == 0) return false;

                                    idx = createRebindForKeyIfMissing(s_layoutContextVk);
                                    s_layoutContextPreferredIndex = idx;
                                    if (idx < 0) return false;

                                    auto& rr = g_config.keyRebinds.rebinds[idx];
                                    rr.toKey = requestedVk;
                                    rr.triggerOutputDisabled = false;
                                    rr.useCustomOutput = true;
                                    rr.customOutputScanCode = requestedScan;
                                    if (!s_layoutContextSplitMode) {
                                        setFullRebindState(rr, rr.fromKey);
                                        if (idx >= 0 && idx < (int)s_rebindUnicodeTextEdit.size()) {
                                            s_rebindUnicodeTextEdit[idx].clear();
                                        }
                                    }
                                    g_configIsDirty = true;
                                    r = &rr;
                                    return true;
                                };

                                const bool isDefaultScan = !(r && r->useCustomOutput && r->customOutputScanCode != 0);
                                if (ImGui::Selectable(tr("inputs.scan_reset_default_format", defaultPreview.c_str()).c_str(), isDefaultScan)) {
                                    clearTriggerCustomScan();
                                }
#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
                                if (ConsumeGuiTestKeyboardLayoutResetScanToDefaultRequest()) {
                                    clearTriggerCustomScan();
                                }
#endif
                                ImGui::Separator();

                                const char* groupLabels[SG_COUNT + 1] = {
                                    trc("inputs.scan_group_all"), trc("inputs.scan_group_alpha"), trc("inputs.scan_group_digit"),
                                    trc("inputs.scan_group_function"), trc("inputs.scan_group_nav"), trc("inputs.scan_group_numpad"),
                                    trc("inputs.scan_group_modifier"), trc("inputs.scan_group_other")
                                };
                                for (int g = -1; g < SG_Raw; ++g) {
                                    if (g > -1) ImGui::SameLine();
                                    bool active = (s_scanFilterGroup == g);
                                    if (active) {
                                        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                                    }
                                    if (ImGui::Button(groupLabels[g + 1])) { s_scanFilterGroup = g; }
                                    if (active) { ImGui::PopStyleColor(2); }
                                }
                                ImGui::Spacing();

#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
                                if (const DWORD requestedScan = ConsumeGuiTestKeyboardLayoutSelectScanRequest(); requestedScan != 0) {
                                    (void)tryApplyTriggerCustomScan(requestedScan);
                                }
#endif

                                ImGui::BeginChild("##triggers_custom_list", ImVec2(0.0f, 260.0f * popupUiScale), true);
                                for (const auto& it : s_knownScanCodes) {
                                    if (it.group == SG_Raw) continue;
                                    if (s_scanFilterGroup >= 0 && it.group != s_scanFilterGroup) continue;
                                    const DWORD scan = it.scan;
                                    const std::string& name = it.name;
                                    const std::string itemLabel = name + "  (" + formatScanHex(scan) + ")##scan_" + std::to_string((unsigned)scan);

                                    const bool selected = (r && r->useCustomOutput && r->customOutputScanCode == scan);
                                    if (ImGui::Selectable(itemLabel.c_str(), selected)) {
                                        (void)tryApplyTriggerCustomScan(scan);
                                    }
                                }
                                ImGui::EndChild();

                                ImGui::EndPopup();
                        }

                        if (!layoutContextIsScrollWheel && cannotTypeFor(rbPtr, s_layoutContextVk)) {
                            ImGui::TextDisabled(trc("inputs.cannot_type"));
                        }

                        if (!layoutContextIsScrollWheel) {
                            ImGui::Spacing();
                        }

                        if (!layoutContextIsScrollWheel &&
                            ImGui::Button((tr("label.reset") + "##layout_reset").c_str(), ImVec2(102.0f * popupUiScale, 0))) {
                            if (idx >= 0 && idx < (int)g_config.keyRebinds.rebinds.size()) {
                                auto& r = g_config.keyRebinds.rebinds[idx];
                                resetLayoutRebindToDefault(r, true);
                                g_configIsDirty = true;

                                if (idx >= 0 && idx < (int)s_rebindUnicodeTextEdit.size()) s_rebindUnicodeTextEdit[idx].clear();

                                if (isNoOpRebindForKey(r, r.fromKey)) {
                                    eraseRebindIndex(idx);
                                    s_layoutContextPreferredIndex = -1;
                                } else {
                                    std::lock_guard<std::mutex> hotkeyLock(g_hotkeyMainKeysMutex);
                                    RebuildHotkeyMainKeys_Internal();
                                }
                            }
                        }

                        if (!layoutContextIsScrollWheel && layoutContextKeyIsCustom) {
                            ImGui::SameLine();
                            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.36f, 0.14f, 0.14f, 0.90f));
                            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.50f, 0.16f, 0.16f, 0.95f));
                            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.28f, 0.10f, 0.10f, 1.00f));
                            if (ImGui::Button((tr("inputs.remove_custom_bind") + "##layout_remove_custom_key").c_str(),
                                              ImVec2(194.0f * popupUiScale, 0))) {
                                queueCustomLayoutKeyRemoval(s_layoutContextVk);
                                ImGui::CloseCurrentPopup();
                            }
                            if (ImGui::IsItemHovered()) {
                                ImGui::SetTooltip("%s", trc("inputs.tooltip.remove_custom_bind"));
                            }
                            ImGui::PopStyleColor(3);
                        }

                        ImGui::PopStyleVar(3);
                        ImGui::EndPopup();
                    }

                    const float totalH = (keyboardTotalH > mouseDiagramTotalH) ? keyboardTotalH : mouseDiagramTotalH;

                    auto hasCustomLayoutSourceKey = [&](DWORD vk) {
                        if (containsLayoutSourceKey(g_config.keyRebinds.layoutExtraKeys, vk)) return true;
                        for (const auto& rebind : g_config.keyRebinds.rebinds) {
                            if (rebind.fromKey == vk && !containsLayoutSourceKey(builtInLayoutKeys, vk)) {
                                return true;
                            }
                        }
                        return false;
                    };

                    if (s_layoutAddCustomKeyPending) {
                        MarkRebindBindingActive();

                        DWORD capturedVk = 0;
                        LPARAM capturedLParam = 0;
                        bool capturedIsMouse = false;
                        if (ConsumeBindingInputEventSince(s_layoutAddCustomKeyLastSeq, capturedVk, capturedLParam, capturedIsMouse)) {
                            (void)capturedIsMouse;
                            const DWORD sourceVk = normalizeCapturedLayoutSourceVk(capturedVk, capturedLParam);
                            const bool existingCustomSourceKey = hasCustomLayoutSourceKey(sourceVk);

                            s_layoutAddCustomKeyPending = false;
                            if (sourceVk != 0 && !existingCustomSourceKey) {
                                g_config.keyRebinds.layoutExtraKeys.push_back(sourceVk);
                                g_configIsDirty = true;
                            }

                            if (sourceVk != 0) {
                                openRebindContextFor(sourceVk, true);
                            }
                        }
                    }

                    const std::vector<DWORD> customLayoutKeys = collectCustomLayoutKeys();

                    ImGui::SetCursorPos(ImVec2(layoutStart.x, layoutStart.y + totalH + gap));

                    ImGui::Spacing();
                    ImGui::SeparatorText(trc("inputs.custom_layout_keys"));
                    auto beginAddCustomBindCapture = [&]() {
                        s_layoutAddCustomKeyPending = true;
                        s_layoutAddCustomKeyLastSeq = GetLatestBindingInputSequence();
                        MarkRebindBindingActive();
                    };
#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
                    if (ConsumeGuiTestKeyboardLayoutBeginAddCustomBindRequest()) {
                        beginAddCustomBindCapture();
                    }
#endif
                    const char* addCustomBindLabel = s_layoutAddCustomKeyPending ? trc("hotkeys.press_keys") : trc("inputs.add_custom_bind");
                    if (ImGui::Button(addCustomBindLabel, ImVec2(190.0f * popupUiScale, 0.0f))) {
                        beginAddCustomBindCapture();
                    }
#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
                    RecordGuiTestInteractionRect("inputs.keyboard_layout_add_custom_bind", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
#endif
                    ImGui::SameLine();
                    HelpMarker(trc("inputs.tooltip.add_custom_bind"));
                    if (s_layoutAddCustomKeyPending) {
                        ImGui::SameLine();
                        if (ImGui::Button((tr("label.cancel") + "##layoutAddCustomBindCancel").c_str())) {
                            s_layoutAddCustomKeyPending = false;
                        }
                    }

                    if (!customLayoutKeys.empty()) {
                        float customKeyX = layoutStart.x;
                        float customKeyY = ImGui::GetCursorPosY() + gap * 0.5f;
                        const float customKeyWrapWidth =
                            (std::max)(keyboardMaxRowW + unit * 4.5f, ImGui::GetContentRegionAvail().x);
                        const float customKeyWrapX = layoutStart.x + customKeyWrapWidth;
                        float customKeyBottomY = customKeyY;

#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
                        if (const DWORD requestedRemoveCustomKey = ConsumeGuiTestKeyboardLayoutRemoveCustomKeyRequest(); requestedRemoveCustomKey != 0) {
                            queueCustomLayoutKeyRemoval(requestedRemoveCustomKey);
                        }
#endif

                        for (DWORD vk : customLayoutKeys) {
                            const std::string keyLabel = getCompactLayoutSourceButtonLabel(vk);
                            const float keyW = unit;
                            const float customKeyGroupW = keyW;

                            if (customKeyX > layoutStart.x && customKeyX + customKeyGroupW > customKeyWrapX) {
                                customKeyX = layoutStart.x;
                                customKeyY += keyH + gap;
                            }

                            ImGui::SetCursorPos(ImVec2(customKeyX, customKeyY));
                            ImGui::PushID(static_cast<int>(vk));
                            ImGui::InvisibleButton("##customLayoutKey", ImVec2(keyW, keyH),
                                                  ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);

                            const ImVec2 pMin = ImGui::GetItemRectMin();
                            const ImVec2 pMax = ImGui::GetItemRectMax();
#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
                            RecordGuiTestInteractionRect(std::string("inputs.keyboard_layout.custom_key.") + std::to_string(static_cast<unsigned>(vk)),
                                                         pMin, pMax);
#endif

                            const KeyRebind* rb = findRebindForKey(vk);
                            ImVec2 capMin = ImVec2(pMin.x + keyPadX, pMin.y + keyPadY);
                            ImVec2 capMax = ImVec2(pMax.x - keyPadX, pMax.y - keyPadY);
                            if (capMax.x <= capMin.x + 2.0f) {
                                capMin.x = pMin.x;
                                capMax.x = pMax.x;
                            }
                            if (capMax.y <= capMin.y + 2.0f) {
                                capMin.y = pMin.y;
                                capMax.y = pMax.y;
                            }
                            drawKeyCell(vk, keyLabel.c_str(), capMin, capMax, rb);

                            if (ImGui::IsItemHovered()) {
                                ImGui::BeginTooltip();
                                if (isConfiguredLayoutRebind(rb, vk)) {
                                    DWORD triggerVkTip = resolveTriggerVkFor(rb, vk);
                                    DWORD triggerScanTip = resolveTriggerScanFor(rb, vk);
                                    const std::string typesTip = typesValueForDisplay(rb, vk);
                                    const std::string triggersTip = (triggerVkTip == 0)
                                                                        ? std::string(trc("label.disabled"))
                                                                        : normalizeMouseButtonLabel(scanCodeToDisplayName(triggerScanTip, triggerVkTip));
                                    const std::string typesText = tr("inputs.types_format", typesTip.c_str());
                                    ImGui::TextUnformatted(typesText.c_str());
                                    if (hasShiftLayerOverride(rb, vk)) {
                                        const std::string typesShiftTip = typesShiftValueForDisplay(rb, vk);
                                        const std::string typesShiftText = tr("inputs.types_shift_format", typesShiftTip.c_str());
                                        ImGui::TextUnformatted(typesShiftText.c_str());
                                    }
                                    const std::string triggersText = tr("inputs.triggers_format", triggersTip.c_str());
                                    ImGui::TextUnformatted(triggersText.c_str());
                                } else {
                                    ImGui::Text("%s (%u)", keyLabel.c_str(), static_cast<unsigned>(vk));
                                    ImGui::TextUnformatted(trc("inputs.tooltip.custom_layout_key"));
                                }
                                ImGui::EndTooltip();
                            }

                            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                                openRebindContextFor(vk, true);
                            }

                            ImGui::PopID();
                            customKeyBottomY = (std::max)(customKeyBottomY, customKeyY + keyH);
                            customKeyX += customKeyGroupW + gap;
                        }

                        ImGui::SetCursorPos(ImVec2(layoutStart.x, customKeyBottomY + gap));
                    } else {
                        ImGui::Spacing();
                    }

                    if (openRemoveCustomKeyConfirmPopup) {
                        ImGui::OpenPopup(trc("inputs.remove_custom_bind_confirm"));
                    }

                    if (s_layoutPendingCustomKeyRemovalVk != 0) {
                        MarkRebindBindingActive();
                    }

                    if (ImGui::BeginPopupModal(trc("inputs.remove_custom_bind_confirm"), NULL,
                                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
                        MarkRebindBindingActive();
                        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                            s_layoutEscapeRequiresRelease = true;
                            layoutEscapeConsumedThisFrame = true;
                            s_layoutPendingCustomKeyRemovalVk = 0;
                            s_layoutPendingCustomKeyRemovalRebindCount = 0;
                            ImGui::CloseCurrentPopup();
                        }

#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
                        const bool confirmRemovalRequested = ConsumeGuiTestKeyboardLayoutConfirmRemoveCustomKeyRequest();
#else
                        const bool confirmRemovalRequested = false;
#endif

                        const std::string pendingRemovalLabel =
                            (s_layoutPendingCustomKeyRemovalVk != 0) ? getLayoutSourceButtonLabel(s_layoutPendingCustomKeyRemovalVk)
                                                                      : std::string();
                        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.20f, 1.0f), "%s", trc("label.warning"));
                        ImGui::TextUnformatted(trc("inputs.remove_custom_bind_description"));
                        if (!pendingRemovalLabel.empty()) {
                            ImGui::Text("%s", pendingRemovalLabel.c_str());
                        }
                        if (s_layoutPendingCustomKeyRemovalRebindCount > 0) {
                            const std::string warningText =
                                tr("inputs.remove_custom_bind_rebinds_warning", s_layoutPendingCustomKeyRemovalRebindCount);
                            ImGui::TextWrapped("%s", warningText.c_str());
                        }
                        ImGui::TextUnformatted(trc("label.action_cannot_be_undone"));
                        ImGui::Separator();

                        const bool confirmClicked =
                            ImGui::Button(trc("inputs.remove_custom_bind"), ImVec2(190.0f * popupUiScale, 0.0f));
#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
                        RecordGuiTestInteractionRect("inputs.keyboard_layout.remove_custom_key_confirm", ImGui::GetItemRectMin(),
                                                     ImGui::GetItemRectMax());
#endif
                        if (confirmClicked || confirmRemovalRequested) {
                            removeCustomLayoutKeyAndRebinds(s_layoutPendingCustomKeyRemovalVk);
                            s_layoutPendingCustomKeyRemovalVk = 0;
                            s_layoutPendingCustomKeyRemovalRebindCount = 0;
                            ImGui::CloseCurrentPopup();
                        }
                        ImGui::SameLine();
                        if (ImGui::Button(trc("label.cancel"), ImVec2(120.0f * popupUiScale, 0.0f))) {
                            s_layoutPendingCustomKeyRemovalVk = 0;
                            s_layoutPendingCustomKeyRemovalRebindCount = 0;
                            ImGui::CloseCurrentPopup();
                        }

                        ImGui::EndPopup();
                    }

                    {
                        ImGui::Spacing();
                        ImGui::SeparatorText(trc("inputs.rebinds"));
                        auto isConsumeOnlySummaryRebind = [&](const KeyRebind& r) -> bool {
                            return r.enabled && r.fromKey != 0 && r.toKey == 0;
                        };
                        auto isNoOp = [&](const KeyRebind& r) -> bool {
                            if (r.fromKey == 0) return true;
                            if (isConsumeOnlySummaryRebind(r)) return false;
                            if (r.triggerOutputDisabled) return false;
                            if (r.baseOutputDisabled) return false;
                            if (r.shiftLayerOutputDisabled) return false;
                            if (r.toKey == 0) return true;
                            if (r.toKey != r.fromKey) return false;
                            if (r.customOutputVK != 0 && r.customOutputVK != r.toKey) return false;
                            if (r.customOutputUnicode != 0) return false;
                            if (r.customOutputScanCode != 0) return false;
                            if (r.baseOutputShifted) return false;
                            if (hasShiftLayerOverride(&r, r.fromKey)) return false;
                            return true;
                        };
                        auto renderRebindSummaryLine = [&](const KeyRebind& r) {
                            std::string fromStr = VkToString(r.fromKey);
                            if (isConsumeOnlySummaryRebind(r)) {
                                ImGui::Text("%s -> %s", fromStr.c_str(), layoutNoneLabel);
                                return;
                            }

                            std::string typesStr = typesValueForDisplay(&r, r.fromKey);
                            if (hasShiftLayerOverride(&r, r.fromKey)) {
                                const std::string shiftTypesStr = typesShiftValueForDisplay(&r, r.fromKey);
                                typesStr += "/" + shiftTypesStr;
                            }

                            DWORD triggerVk = resolveTriggerVkFor(&r, r.fromKey);
                            std::string triggersStr = (triggerVk == 0) ? std::string(layoutNoneLabel)
                                                                        : scanCodeToDisplayName(resolveTriggerScanFor(&r, r.fromKey), triggerVk);
                            ImGui::Text("%s -> %s & %s", fromStr.c_str(), typesStr.c_str(), triggersStr.c_str());
                        };

                        auto renderRebindSummarySection = [&](const char* cursorStateId, const char* label) -> bool {
                            bool sectionShown = false;
                            for (const auto& r : g_config.keyRebinds.rebinds) {
                                if (r.fromKey == 0) continue;
                                if (r.toKey == 0 && !isConsumeOnlySummaryRebind(r)) continue;
                                if (isNoOp(r)) continue;
                                if (NormalizeKeyRebindCursorStateId(r.cursorState) != cursorStateId) continue;

                                if (!sectionShown) {
                                    ImGui::TextDisabled("%s", label);
                                    ImGui::Indent();
                                    sectionShown = true;
                                }

                                renderRebindSummaryLine(r);
                            }

                            if (sectionShown) {
                                ImGui::Unindent();
                            }

                            return sectionShown;
                        };

                        bool anyShown = false;
                        const struct {
                            const char* cursorStateId;
                            const char* label;
                        } summarySections[] = {
                            { kKeyRebindCursorStateAny, trc("label.default") },
                            { kKeyRebindCursorStateCursorGrabbed, trc("inputs.rebind_layout_cursor_grabbed") },
                            { kKeyRebindCursorStateCursorFree, trc("inputs.rebind_layout_cursor_free") },
                        };

                        for (const auto& section : summarySections) {
                            if (!renderRebindSummarySection(section.cursorStateId, section.label)) {
                                continue;
                            }

                            anyShown = true;
                            ImGui::Spacing();
                        }

                        if (!anyShown) {
                            ImGui::TextDisabled(trc("inputs.no_active_rebinds"));
                        }
                    }

                    ImGui::EndChild();
                    ImGui::PopStyleColor();

                    const bool anyRebindBindUiActiveAfter = (s_rebindFromKeyToBind != -1) || (s_rebindOutputVKToBind != -1) ||
                                                            (s_rebindTextOverrideVKToBind != -1) || (s_rebindOutputScanToBind != -1) ||
                                                            s_layoutAddCustomKeyPending ||
                                                            s_layoutCustomInputCapturePending ||
                                                            (s_layoutPendingCustomKeyRemovalVk != 0) ||
                                                            (s_layoutBindTarget != LayoutBindTarget::None) || (s_layoutUnicodeEditIndex != -1) ||
                                                            ImGui::IsPopupOpen(trc("inputs.rebind_config")) || ImGui::IsPopupOpen(trc("inputs.triggers_custom")) ||
                                                            ImGui::IsPopupOpen(trc("inputs.custom_key_input_picker")) ||
                                                            ImGui::IsPopupOpen(trc("inputs.custom_unicode")) ||
                                                            ImGui::IsPopupOpen(trc("inputs.remove_custom_bind_confirm"));
                    if (!blockLayoutEscapeThisFrame && !layoutEscapeConsumedThisFrame && escapePressedThisFrame && !anyRebindBindUiActiveAfter) {
                        s_keyboardLayoutOpen = false;
                    }

                    s_layoutContextPopupWasOpenLastFrame = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup);
                    s_keyboardLayoutWasOpenLastFrame = s_keyboardLayoutOpen;
                    } else {
                        s_layoutContextPopupWasOpenLastFrame = false;
                        s_keyboardLayoutWasOpenLastFrame = false;
                        s_layoutAddCustomKeyPending = false;
                        s_layoutCustomInputCapturePending = false;
                        s_layoutPendingCustomKeyRemovalVk = 0;
                        s_layoutPendingCustomKeyRemovalRebindCount = 0;
                        s_layoutContextOpenedFromCustomKey = false;
                        s_lastKeyboardLayoutWindowSize = ImVec2(-1.0f, -1.0f);
                        s_lastKeyboardLayoutGuiScaleFactor = -1.0f;
                    }
                    ImGui::End();
                    ImGui::PopStyleColor();
                } else {
                    s_layoutContextPopupWasOpenLastFrame = false;
                    s_keyboardLayoutWasOpenLastFrame = false;
                    s_layoutAddCustomKeyPending = false;
                    s_layoutCustomInputCapturePending = false;
                    s_layoutPendingCustomKeyRemovalVk = 0;
                    s_layoutPendingCustomKeyRemovalRebindCount = 0;
                    s_layoutContextOpenedFromCustomKey = false;
                    s_lastKeyboardLayoutWindowSize = ImVec2(-1.0f, -1.0f);
                    s_lastKeyboardLayoutGuiScaleFactor = -1.0f;
                }

                bool is_rebind_from_binding = (s_rebindFromKeyToBind != -1);
                if (is_rebind_from_binding) { MarkRebindBindingActive(); }
                if (is_rebind_from_binding) { ImGui::OpenPopup(trc("inputs.bind_from_key")); }

                if (ImGui::BeginPopupModal(trc("inputs.bind_from_key"), NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
                    ImGui::Text(trc("inputs.bind_from_key.tooltip.prompt"));
                    ImGui::Text(trc("inputs.bind_from_key.tooltip.cancel"));
                    ImGui::Separator();

                    static uint64_t s_lastBindingInputSeqInputs1 = 0;
                    if (ImGui::IsWindowAppearing()) { s_lastBindingInputSeqInputs1 = GetLatestBindingInputSequence(); }

                    DWORD capturedVk = 0;
                    LPARAM capturedLParam = 0;
                    bool capturedIsMouse = false;
                    if (ConsumeBindingInputEventSince(s_lastBindingInputSeqInputs1, capturedVk, capturedLParam, capturedIsMouse)) {
                        if (capturedVk == VK_ESCAPE) {
                            s_rebindFromKeyToBind = -1;
                            ImGui::CloseCurrentPopup();
                        } else {
                            if (s_rebindFromKeyToBind != -1 && s_rebindFromKeyToBind < (int)g_config.keyRebinds.rebinds.size()) {
                                g_config.keyRebinds.rebinds[s_rebindFromKeyToBind].fromKey = capturedVk;
                                g_configIsDirty = true;
                                std::lock_guard<std::mutex> hotkeyLock(g_hotkeyMainKeysMutex);
                                RebuildHotkeyMainKeys_Internal();
                                (void)capturedLParam;
                                (void)capturedIsMouse;
                            }
                            s_rebindFromKeyToBind = -1;
                            ImGui::CloseCurrentPopup();
                        }
                    }

                    ImGui::EndPopup();
                }

                bool is_vk_binding = (s_rebindOutputVKToBind != -1);
                if (is_vk_binding) { MarkRebindBindingActive(); }
                if (is_vk_binding) { ImGui::OpenPopup(trc("inputs.bind_output_vk")); }

                if (ImGui::BeginPopupModal(trc("inputs.bind_output_vk"), NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
                    ImGui::Text(trc("inputs.bind_output_vk.tooltip.prompt"));
                    ImGui::Text(trc("inputs.bind_output_vk.tooltip.cancel"));
                    ImGui::Separator();

                    static uint64_t s_lastBindingInputSeqInputs2 = 0;
                    if (ImGui::IsWindowAppearing()) { s_lastBindingInputSeqInputs2 = GetLatestBindingInputSequence(); }

                    DWORD capturedVk = 0;
                    LPARAM capturedLParam = 0;
                    bool capturedIsMouse = false;
                    if (ConsumeBindingInputEventSince(s_lastBindingInputSeqInputs2, capturedVk, capturedLParam, capturedIsMouse)) {
                        if (capturedVk == VK_ESCAPE) {
                            s_rebindOutputVKToBind = -1;
                            ImGui::CloseCurrentPopup();
                        } else {
                            if (s_rebindOutputVKToBind >= 0 && s_rebindOutputVKToBind < (int)g_config.keyRebinds.rebinds.size()) {
                                auto& rebind = g_config.keyRebinds.rebinds[s_rebindOutputVKToBind];
                                rebind.toKey = capturedVk;
                                // Do not touch custom text override here.
                                g_configIsDirty = true;
                                (void)capturedLParam;
                                (void)capturedIsMouse;
                            }
                            s_rebindOutputVKToBind = -1;
                            ImGui::CloseCurrentPopup();
                        }
                    }

                    ImGui::EndPopup();
                }

#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
                if (const int requestedTextOverrideBind = ConsumeGuiTestOpenRebindTextOverrideBindRequest();
                    requestedTextOverrideBind >= 0 && requestedTextOverrideBind < (int)g_config.keyRebinds.rebinds.size()) {
                    s_rebindTextOverrideVKToBind = requestedTextOverrideBind;
                }
#endif

                bool is_text_vk_binding = (s_rebindTextOverrideVKToBind != -1);
                if (is_text_vk_binding) { MarkRebindBindingActive(); }
                if (is_text_vk_binding) { ImGui::OpenPopup(trc("inputs.bind_text_override_vk")); }

                if (ImGui::BeginPopupModal(trc("inputs.bind_text_override_vk"), NULL,
                                           ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
                    ImGui::Text(trc("inputs.bind_text_override_vk.tooltip.prompt"));
                    ImGui::Text(trc("inputs.bind_text_override_vk.tooltip.cancel"));
                    ImGui::Separator();

                    static uint64_t s_lastBindingInputSeqInputs2b = 0;
                    if (ImGui::IsWindowAppearing()) { s_lastBindingInputSeqInputs2b = GetLatestBindingInputSequence(); }

                    DWORD capturedVk = 0;
                    LPARAM capturedLParam = 0;
                    bool capturedIsMouse = false;
                    if (ConsumeBindingInputEventSince(s_lastBindingInputSeqInputs2b, capturedVk, capturedLParam, capturedIsMouse)) {
                        if (capturedVk == VK_ESCAPE) {
                            s_rebindTextOverrideVKToBind = -1;
                            ImGui::CloseCurrentPopup();
                        } else {
                            bool closePopup = true;
                            if (s_rebindTextOverrideVKToBind >= 0 &&
                                s_rebindTextOverrideVKToBind < (int)g_config.keyRebinds.rebinds.size()) {
                                auto& rebind = g_config.keyRebinds.rebinds[s_rebindTextOverrideVKToBind];
                                if (canAcceptTypesVkCaptureFor(&rebind, rebind.fromKey, capturedVk)) {
                                    rebind.useCustomOutput = true;
                                    rebind.customOutputVK = capturedVk;
                                    g_configIsDirty = true;
                                } else {
                                    closePopup = false;
                                }
                                (void)capturedLParam;
                                (void)capturedIsMouse;
                            }
                            if (closePopup) {
                                s_rebindTextOverrideVKToBind = -1;
                                ImGui::CloseCurrentPopup();
                            }
                        }
                    }

                    ImGui::EndPopup();
                }

                bool is_scan_binding = (s_rebindOutputScanToBind != -1);
                if (is_scan_binding) { MarkRebindBindingActive(); }
                if (is_scan_binding) { ImGui::OpenPopup(trc("inputs.bind_output_scan")); }

                if (ImGui::BeginPopupModal(trc("inputs.bind_output_scan"), NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
                    ImGui::Text(trc("inputs.bind_output_scan.tooltip.prompt"));
                    ImGui::Text(trc("inputs.bind_output_scan.tooltip.cancel"));
                    ImGui::Separator();

                    static uint64_t s_lastBindingInputSeqInputs3 = 0;
                    if (ImGui::IsWindowAppearing()) { s_lastBindingInputSeqInputs3 = GetLatestBindingInputSequence(); }

                    DWORD capturedVk = 0;
                    LPARAM capturedLParam = 0;
                    bool capturedIsMouse = false;
                    if (ConsumeBindingInputEventSince(s_lastBindingInputSeqInputs3, capturedVk, capturedLParam, capturedIsMouse)) {
                        if (capturedVk == VK_ESCAPE) {
                            s_rebindOutputScanToBind = -1;
                            ImGui::CloseCurrentPopup();
                        } else {
                            if (s_rebindOutputScanToBind >= 0 && s_rebindOutputScanToBind < (int)g_config.keyRebinds.rebinds.size()) {
                                auto& rebind = g_config.keyRebinds.rebinds[s_rebindOutputScanToBind];

                                if (capturedVk == VK_LBUTTON || capturedVk == VK_RBUTTON || capturedVk == VK_MBUTTON ||
                                    capturedVk == VK_XBUTTON1 || capturedVk == VK_XBUTTON2) {
                                    rebind.toKey = capturedVk;
                                    rebind.customOutputScanCode = 0;
                                } else {
                                    UINT scanCode = static_cast<UINT>((capturedLParam >> 16) & 0xFF);
                                    if ((capturedLParam & (1LL << 24)) != 0) { scanCode |= 0xE000; }
                                    if ((capturedLParam & (1LL << 24)) == 0 && scanCode == 0) {
                                        scanCode = getScanCodeWithExtendedFlag(capturedVk);
                                    }

                                    if ((scanCode & 0xFF00) == 0) { scanCode = getScanCodeWithExtendedFlag(capturedVk); }

                                    rebind.customOutputScanCode = scanCode;
                                    rebind.useCustomOutput = true;

                                    Log("[Rebind][GameKeybind] capturedVk=" + std::to_string(capturedVk) +
                                        " capturedLParam=" + std::to_string(static_cast<long long>(capturedLParam)) +
                                        " storedScan=" + std::to_string(scanCode) + " ext=" + std::string((scanCode & 0xFF00) ? "1" : "0"));
                                }

                                g_configIsDirty = true;
                                (void)capturedIsMouse;
                            }
                            s_rebindOutputScanToBind = -1;
                            ImGui::CloseCurrentPopup();
                        }
                    }

                    ImGui::EndPopup();
                }

                ImGui::Spacing();
                ImGui::TextDisabled(trc("inputs.tooltip.configure_key_rebinds"));
            }

                }

                ImGui::EndTabItem();
            }
        }

        ImGui::EndTabBar();
    }

    ImGui::EndTabItem();
}


