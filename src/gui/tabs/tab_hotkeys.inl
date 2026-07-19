if (IsResolutionChangeSupported(g_gameVersion)) {
    if (BeginSelectableSettingsTopTabItem(trc("tabs.hotkeys"))) {
        g_currentlyEditingMirror = "";

        {
            const GameStateSourceKind stateSource = g_activeGameStateSource.load(std::memory_order_acquire);
            ImGui::Spacing();
            if (stateSource == GameStateSourceKind::None) {
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "%s", trc("hotkeys.state_source.warning"));
                ImGui::TextWrapped("%s", trc("hotkeys.state_source.warning_tooltip"));
            } else {
                const char* sourceName = (stateSource == GameStateSourceKind::Hermes) ? trc("hotkeys.state_source.hermes")
                                                                                      : trc("hotkeys.state_source.stateoutput");
                ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1.0f), "%s %s", trc("hotkeys.state_source.active"), sourceName);
            }
            ImGui::Separator();
        }

        SliderCtrlClickTip();

        ImGui::SeparatorText(trc("hotkeys.gui_hotkey"));
        ImGui::PushID("gui_hotkey");
        std::string guiKeyStr = GetKeyComboString(g_config.guiHotkey);
        std::string guiNode_label = tr("hotkeys.gui_hotkey_open_close") + (guiKeyStr.empty() ? trc("hotkeys.none") : guiKeyStr);

        bool guiNode_open = ImGui::TreeNodeEx("##gui_hotkey_node", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", guiNode_label.c_str());
        if (guiNode_open) {
            const char* gui_button_label =
                (s_mainHotkeyToBind == -999) ? trc("hotkeys.press_keys") : (guiKeyStr.empty() ? trc("hotkeys.none") : guiKeyStr.c_str());
            if (ImGui::Button(gui_button_label)) {
                s_mainHotkeyToBind = -999;
                s_altHotkeyToBind = { -1, -1 };
                s_exclusionToBind = { -1, -1 };
                MarkHotkeyBindingActive();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();

        ImGui::SeparatorText(trc("hotkeys.window_hotkeys"));
        ImGui::PushID("borderless_hotkey");
        std::string borderlessKeyStr = GetKeyComboString(g_config.borderlessHotkey);
        std::string borderlessNodeLabel =
            tr("hotkeys.toggle_borderless_prefix") + (borderlessKeyStr.empty() ? trc("hotkeys.none") : borderlessKeyStr);

        bool borderlessNodeOpen =
            ImGui::TreeNodeEx("##borderless_hotkey_node", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", borderlessNodeLabel.c_str());
        if (borderlessNodeOpen) {
            const bool isBindingBorderless = (s_mainHotkeyToBind == -998);
            const char* borderlessButtonLabel =
                isBindingBorderless ? trc("hotkeys.press_keys") : (borderlessKeyStr.empty() ? trc("hotkeys.none") : borderlessKeyStr.c_str());
            if (ImGui::Button(borderlessButtonLabel)) {
                s_mainHotkeyToBind = -998;
                s_altHotkeyToBind = { -1, -1 };
                s_exclusionToBind = { -1, -1 };
                MarkHotkeyBindingActive();
            }
            ImGui::SameLine();
            ImGui::TextDisabled(trc("label.question_mark"));
            if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(trc("tooltip.toggle_borderless"));
                }
            ImGui::TreePop();
        }
        ImGui::PopID();

        ImGui::PushID("overlay_visibility_hotkeys");
        {
            std::string imgOverlayKeyStr = GetKeyComboString(g_config.imageOverlaysHotkey);
            std::string imgOverlayNodeLabel =
                tr("hotkeys.toggle_image_overlays_prefix") + (imgOverlayKeyStr.empty() ? trc("hotkeys.none") : imgOverlayKeyStr);

            const bool imgOverlaysVisible = g_imageOverlaysVisible.load(std::memory_order_acquire);
            const ImVec4 visibleGreen = ImVec4(0.20f, 1.00f, 0.20f, 1.00f);
            const ImVec4 hiddenRed = ImVec4(1.00f, 0.20f, 0.20f, 1.00f);

            bool imgOverlayNodeOpen =
                ImGui::TreeNodeEx("##image_overlay_toggle_node", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", imgOverlayNodeLabel.c_str());

            ImGui::SameLine();
            ImGui::TextDisabled("%s", trc("label.status"));
            ImGui::SameLine();
            ImGui::TextColored(imgOverlaysVisible ? visibleGreen : hiddenRed, "%s", imgOverlaysVisible ? trc("label.shown") : trc("label.hidden"));
            if (imgOverlayNodeOpen) {
                const bool isBindingImgOverlay = (s_mainHotkeyToBind == -997);
                const char* imgOverlayButtonLabel =
                    isBindingImgOverlay ? trc("hotkeys.press_keys") : (imgOverlayKeyStr.empty() ? trc("hotkeys.none") : imgOverlayKeyStr.c_str());
                if (ImGui::Button(imgOverlayButtonLabel)) {
                    s_mainHotkeyToBind = -997;
                    s_altHotkeyToBind = { -1, -1 };
                    s_exclusionToBind = { -1, -1 };
                    MarkHotkeyBindingActive();
                }
                ImGui::SameLine();
                ImGui::TextDisabled(trc("label.question_mark"));
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(trc("tooltip.toggle_image_overlays.advanced"));
                }
                ImGui::TreePop();
            }

            std::string winOverlayKeyStr = GetKeyComboString(g_config.windowOverlaysHotkey);
            std::string winOverlayNodeLabel =
                tr("hotkeys.toggle_window_overlays_prefix") + (winOverlayKeyStr.empty() ? trc("hotkeys.none") : winOverlayKeyStr);

            const bool winOverlaysVisible = g_windowOverlaysVisible.load(std::memory_order_acquire);

            bool winOverlayNodeOpen =
                ImGui::TreeNodeEx("##window_overlay_toggle_node", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", winOverlayNodeLabel.c_str());

            ImGui::SameLine();
            ImGui::TextDisabled("%s", trc("label.status"));
            ImGui::SameLine();
            ImGui::TextColored(winOverlaysVisible ? visibleGreen : hiddenRed, "%s", winOverlaysVisible ? trc("label.shown") : trc("label.hidden"));
            if (winOverlayNodeOpen) {
                const bool isBindingWinOverlay = (s_mainHotkeyToBind == -996);
                const char* winOverlayButtonLabel =
                    isBindingWinOverlay ? trc("hotkeys.press_keys") : (winOverlayKeyStr.empty() ? trc("hotkeys.none") : winOverlayKeyStr.c_str());
                if (ImGui::Button(winOverlayButtonLabel)) {
                    s_mainHotkeyToBind = -996;
                    s_altHotkeyToBind = { -1, -1 };
                    s_exclusionToBind = { -1, -1 };
                    MarkHotkeyBindingActive();
                }
                ImGui::SameLine();
                ImGui::TextDisabled(trc("label.question_mark"));
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(trc("tooltip.toggle_window_overlay.advanced"));
                }
                ImGui::TreePop();
            }

            std::string ninjabrainOverlayKeyStr = GetKeyComboString(g_config.ninjabrainOverlayHotkey);
            std::string ninjabrainOverlayNodeLabel =
                tr("hotkeys.toggle_ninjabrain_overlay_prefix") +
                (ninjabrainOverlayKeyStr.empty() ? trc("hotkeys.none") : ninjabrainOverlayKeyStr);

            const bool ninjabrainOverlayVisible = g_ninjabrainOverlayVisible.load(std::memory_order_acquire);

            bool ninjabrainOverlayNodeOpen =
                ImGui::TreeNodeEx("##ninjabrain_overlay_toggle_node", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", ninjabrainOverlayNodeLabel.c_str());

            ImGui::SameLine();
            ImGui::TextDisabled("%s", trc("label.status"));
            ImGui::SameLine();
            ImGui::TextColored(ninjabrainOverlayVisible ? visibleGreen : hiddenRed, "%s",
                               ninjabrainOverlayVisible ? trc("label.shown") : trc("label.hidden"));
            if (ninjabrainOverlayNodeOpen) {
                const bool isBindingNinjabrainOverlay = (s_mainHotkeyToBind == -994);
                const char* ninjabrainOverlayButtonLabel =
                    isBindingNinjabrainOverlay ? trc("hotkeys.press_keys")
                                               : (ninjabrainOverlayKeyStr.empty() ? trc("hotkeys.none") : ninjabrainOverlayKeyStr.c_str());
                if (ImGui::Button(ninjabrainOverlayButtonLabel)) {
                    s_mainHotkeyToBind = -994;
                    s_altHotkeyToBind = { -1, -1 };
                    s_exclusionToBind = { -1, -1 };
                    MarkHotkeyBindingActive();
                }
                ImGui::SameLine();
                ImGui::TextDisabled(trc("label.question_mark"));
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(trc("tooltip.toggle_ninjabrain_overlay.advanced"));
                }
                ImGui::TreePop();
            }
        }
        ImGui::PopID();

        ImGui::SeparatorText(trc("hotkeys.mode_hotkeys"));
        int hotkey_to_remove = -1;
        for (size_t i = 0; i < g_config.hotkeys.size(); ++i) {
            auto& hotkey = g_config.hotkeys[i];
            ImGui::PushID((int)i);
            std::string keyStr = GetKeyComboString(hotkey.keys);
            std::string node_label = tr("hotkeys.hotkey_prefix") + (keyStr.empty() ? trc("hotkeys.none") : keyStr);

            if (ImGui::Button((tr("hotkeys.button_x") + "##del_hotkey_" + std::to_string(i)).c_str(), ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()))) {
                ImGui::OpenPopup((tr("hotkeys.delete_hotkey") + "##" + std::to_string(i)).c_str());
            }

            // Popup modal outside of node_open block so it can be displayed even when collapsed
            if (ImGui::BeginPopupModal((tr("hotkeys.delete_hotkey") + "##" + std::to_string(i)).c_str(), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("%s", trc("hotkeys.delete_hotkey.confirm"));
                ImGui::Separator();
                if (ImGui::Button(trc("button.ok"))) {
                    hotkey_to_remove = (int)i;
                    g_configIsDirty = true;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button(trc("button.cancel"))) { ImGui::CloseCurrentPopup(); }
                ImGui::EndPopup();
            }

            ImGui::SameLine();
            bool node_open = ImGui::TreeNodeEx("##hotkey_node", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", node_label.c_str());

            if (node_open) {
                const char* button_label = (s_mainHotkeyToBind == i) ? trc("hotkeys.press_keys") : (keyStr.empty() ? trc("hotkeys.none") : keyStr.c_str());
                if (ImGui::Button(button_label)) {
                    s_mainHotkeyToBind = (int)i;
                    s_altHotkeyToBind = { -1, -1 };
                    s_exclusionToBind = { -1, -1 };
                    MarkHotkeyBindingActive();
                }

                ImGui::SeparatorText(trc("hotkeys.target_mode"));
                ImGui::SetNextItemWidth(150);
                const char* modeDisplay = hotkey.secondaryMode.empty() ? trc("hotkeys.none") : hotkey.secondaryMode.c_str();
                if (ImGui::BeginCombo(trc("hotkeys.modes"), modeDisplay)) {
                    if (ImGui::Selectable(trc("hotkeys.none"), hotkey.secondaryMode.empty())) {
                        hotkey.secondaryMode = "";
                        SetHotkeySecondaryMode(i, "");
                        g_configIsDirty = true;
                    }
                    for (const auto& mode : g_config.modes) {
                        bool is_default_mode = EqualsIgnoreCase(mode.id, g_config.defaultMode);
                        if (is_default_mode) { ImGui::BeginDisabled(); }
                        if (ImGui::Selectable(mode.id.c_str(), false, is_default_mode ? ImGuiSelectableFlags_Disabled : 0)) {
                            hotkey.secondaryMode = mode.id;
                            SetHotkeySecondaryMode(i, mode.id);
                            g_configIsDirty = true;
                        }
                        if (is_default_mode) {
                            ImGui::EndDisabled();
                            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                                ImGui::SetTooltip(trc("hotkeys.tooltip.default_mode_toggle_back"), g_config.defaultMode.c_str());
                            }
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                ImGui::TextDisabled(trc("label.question_mark"));
                if (ImGui::IsItemHovered()) { ImGui::SetTooltip(trc("hotkeys.tooltip.mode_hotkey"), g_config.defaultMode.c_str()); }

                ImGui::SeparatorText(trc("hotkeys.alt_secondary_modes"));
                int alt_to_remove = -1;
                for (size_t j = 0; j < hotkey.altSecondaryModes.size(); ++j) {
                    auto& alt = hotkey.altSecondaryModes[j];
                    ImGui::PushID(static_cast<int>(j));

                    if (ImGui::Button(trc("hotkeys.button_x"))) { alt_to_remove = (int)j; }
                    ImGui::SameLine();

                    std::string altKeyStr = GetKeyComboString(alt.keys);
                    bool is_binding_this = (s_altHotkeyToBind.hotkey_idx == i && s_altHotkeyToBind.alt_idx == j);
                    const char* alt_button_label = is_binding_this ? trc("hotkeys.press_keys_ellipsis") : (altKeyStr.empty() ? trc("hotkeys.none") : altKeyStr.c_str());
                    if (ImGui::Button(alt_button_label, ImVec2(100, 0))) {
                        s_altHotkeyToBind = { (int)i, (int)j };
                        s_mainHotkeyToBind = -1;
                        s_exclusionToBind = { -1, -1 };
                        MarkHotkeyBindingActive();
                    }
                    ImGui::SameLine();

                    ImGui::SetNextItemWidth(150);
                    const char* altModeDisplay = alt.mode.empty() ? trc("hotkeys.none") : alt.mode.c_str();
                    if (ImGui::BeginCombo(trc("hotkeys.modes"), altModeDisplay)) {
                        if (ImGui::Selectable(trc("hotkeys.none"), alt.mode.empty())) {
                            alt.mode = "";
                            g_configIsDirty = true;
                        }
                        for (const auto& mode : g_config.modes) {
                            bool is_default_mode = EqualsIgnoreCase(mode.id, g_config.defaultMode);
                            if (is_default_mode) { ImGui::BeginDisabled(); }
                            if (ImGui::Selectable(mode.id.c_str(), false, is_default_mode ? ImGuiSelectableFlags_Disabled : 0)) {
                                alt.mode = mode.id;
                                g_configIsDirty = true;
                            }
                            if (is_default_mode) {
                                ImGui::EndDisabled();
                                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                                    ImGui::SetTooltip(trc("hotkeys.tooltip.default_mode_toggle_back"), g_config.defaultMode.c_str());
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::PopID();
                }
                if (alt_to_remove != -1) {
                    hotkey.altSecondaryModes.erase(hotkey.altSecondaryModes.begin() + alt_to_remove);
                    SetHotkeySecondaryMode(i, hotkey.secondaryMode);
                    g_configIsDirty = true;
                }
                if (ImGui::Button(trc("hotkeys.add_alt_mode"))) {
                    hotkey.altSecondaryModes.push_back(AltSecondaryMode{});
                    g_configIsDirty = true;
                }

                ImGui::Separator();
                ImGui::Columns(2, "debounce_col", false);
                ImGui::SetColumnWidth(0, 150);
                ImGui::Text("%s", trc("hotkeys.debounce_ms"));
                ImGui::NextColumn();
                if (Spinner("##debounce", &hotkey.debounce, 1, 0)) g_configIsDirty = true;
                ImGui::Columns(1);

                if (hotkey.triggerOnHold) { ImGui::BeginDisabled(); }
                if (ImGui::Checkbox(trc("hotkeys.trigger_on_release"), &hotkey.triggerOnRelease)) {
                    g_configIsDirty = true;
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip(hotkey.triggerOnHold
                        ? "Disabled while \"Trigger only when holding\" is active"
                        : trc("hotkeys.tooltip.trigger_on_release"));
                }
                if (hotkey.triggerOnHold) { ImGui::EndDisabled(); }

                if (ImGui::Checkbox(trc("hotkeys.trigger_only_when_holding"), &hotkey.triggerOnHold)) {
                    if (hotkey.triggerOnHold) { hotkey.triggerOnRelease = false; }
                    g_configIsDirty = true;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", trc("hotkeys.tooltip.trigger_only_when_holding"));
                }

                if (ImGui::Checkbox(trc("hotkeys.block_key_from_game"), &hotkey.blockKeyFromGame)) { g_configIsDirty = true; }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", trc("hotkeys.tooltip.block_key_from_game"));
                }

                if (ImGui::Checkbox(trc("hotkeys.allow_exit_to_default_mode_regardless_of_game_state"),
                                   &hotkey.allowExitToFullscreenRegardlessOfGameState)) {
                    g_configIsDirty = true;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(trc("hotkeys.tooltip.allow_exit_to_default_mode_regardless_of_game_state"), g_config.defaultMode.c_str());
                }

                if (ImGui::TreeNode(trc("hotkeys.required_game_states"))) {
                    bool isAnySelected = hotkey.conditions.gameState.empty();

                    bool anyToggle = isAnySelected;
                    if (ImGui::Checkbox(trc("hotkeys.any"), &anyToggle) && anyToggle) {
                        hotkey.conditions.gameState.clear();
                        g_configIsDirty = true;
                    }

                    const bool stateSourceUnavailable = !g_isStateOutputAvailable.load(std::memory_order_acquire);

                    for (const char* state : guiGameStates) {
                        auto it = std::find(hotkey.conditions.gameState.begin(), hotkey.conditions.gameState.end(), state);
                        bool is_selected = (it != hotkey.conditions.gameState.end());

                        if (strcmp(state, "generating") == 0) {
                            auto waitingIt = std::find(hotkey.conditions.gameState.begin(), hotkey.conditions.gameState.end(), "waiting");
                            is_selected = is_selected || (waitingIt != hotkey.conditions.gameState.end());
                        }

                        const bool disableThisState =
                            stateSourceUnavailable && strcmp(state, "any,cursor_free") != 0 && strcmp(state, "any,cursor_grabbed") != 0;
                        if (disableThisState) { ImGui::BeginDisabled(); }

                        const char* friendlyName = getGameStateFriendlyName(state);
                        if (ImGui::Checkbox(friendlyName, &is_selected)) {
                            if (strcmp(state, "generating") == 0) {
                                if (is_selected) {
                                    auto generateIt = std::find(hotkey.conditions.gameState.begin(), hotkey.conditions.gameState.end(),
                                                               "generating");
                                    auto waitingIt = std::find(hotkey.conditions.gameState.begin(), hotkey.conditions.gameState.end(),
                                                              "waiting");

                                    if (generateIt == hotkey.conditions.gameState.end()) {
                                        hotkey.conditions.gameState.push_back("generating");
                                    }
                                    if (waitingIt == hotkey.conditions.gameState.end()) {
                                        hotkey.conditions.gameState.push_back("waiting");
                                    }
                                } else {
                                    auto& gs = hotkey.conditions.gameState;
                                    gs.erase(std::remove(gs.begin(), gs.end(), "waiting"), gs.end());
                                    gs.erase(std::remove(gs.begin(), gs.end(), "generating"), gs.end());
                                }
                            } else {
                                if (is_selected) {
                                    hotkey.conditions.gameState.push_back(state);
                                } else {
                                    hotkey.conditions.gameState.erase(it);
                                }
                            }
                            g_configIsDirty = true;
                        }

                        if (disableThisState) { ImGui::EndDisabled(); }
                        if (disableThisState && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                            ImGui::SetTooltip("%s", trc("hotkeys.state_source.warning_tooltip"));
                        }
                    }

                    ImGui::TreePop();
                }

                if (ImGui::TreeNode(trc("hotkeys.exclusion_keys"))) {
                    int exclusion_to_remove = -1;
                    auto& exclusions = hotkey.conditions.exclusions;
                    for (size_t j = 0; j < exclusions.size(); ++j) {
                        ImGui::PushID((int)j);
                        bool is_binding_this = (s_exclusionToBind.hotkey_idx == i && s_exclusionToBind.exclusion_idx == j);
                        std::string ex_key_str = is_binding_this ? trc("hotkeys.press_keys_ellipsis") : VkToString(exclusions[j]);
                        const char* ex_button_label = ex_key_str.c_str();

                        if (ImGui::Button(ex_button_label, ImVec2(100, 0))) {
                            if (!is_binding_this) {
                                s_exclusionToBind = { (int)i, (int)j };
                                s_mainHotkeyToBind = -1;
                                s_altHotkeyToBind = { -1, -1 };
                                MarkHotkeyBindingActive();
                            } else {
                                s_exclusionToBind = { -1, -1 };
                            }
                        }
                        ImGui::SameLine();
                        if (ImGui::Button((tr("hotkeys.delete_x") + "##del_ex_" + std::to_string(j)).c_str(),
                                          ImVec2(ImGui::GetItemRectSize().y, ImGui::GetItemRectSize().y))) {
                            exclusion_to_remove = (int)j;
                        }
                        ImGui::PopID();
                    }
                    if (exclusion_to_remove != -1) {
                        exclusions.erase(exclusions.begin() + exclusion_to_remove);
                        g_configIsDirty = true;
                    }
                    if (ImGui::Button(trc("hotkeys.add_exclusion"))) {
                        exclusions.push_back(0);
                        g_configIsDirty = true;
                    }
                    ImGui::TreePop();
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }

        if (hotkey_to_remove != -1) {
            g_config.hotkeys.erase(g_config.hotkeys.begin() + hotkey_to_remove);
            ResetAllHotkeySecondaryModes();
            // DEADLOCK FIX: Use internal version since g_configMutex is already held
            std::lock_guard<std::mutex> hotkeyLock(g_hotkeyMainKeysMutex);
            RebuildHotkeyMainKeys_Internal();
        }
        ImGui::Separator();
        if (ImGui::Button(trc("hotkeys.add_new_hotkey"))) {
            try {
                HotkeyConfig newHotkey;
                newHotkey.keys = std::vector<DWORD>();
                newHotkey.mainMode = g_config.defaultMode.empty() ? "Fullscreen" : g_config.defaultMode;
                newHotkey.secondaryMode = "";
                newHotkey.altSecondaryModes = std::vector<AltSecondaryMode>();
                newHotkey.conditions = HotkeyConditions();
                newHotkey.debounce = 100;
                g_config.hotkeys.push_back(std::move(newHotkey));
                ResizeHotkeySecondaryModes(g_config.hotkeys.size());
                SetHotkeySecondaryMode(g_config.hotkeys.size() - 1, "");
                std::lock_guard<std::mutex> hotkeyLock(g_hotkeyMainKeysMutex);
                RebuildHotkeyMainKeys_Internal();
                g_configIsDirty = true;
            } catch (const std::exception& e) { Log("ERROR: Failed to add new hotkey: {}", e.what()); }
        }

        ImGui::SameLine();
        if (ImGui::Button((tr("hotkeys.reset_to_defaults") + "##hotkeys").c_str())) { ImGui::OpenPopup(trc("hotkeys.reset_to_defaults_popup")); }

        if (ImGui::BeginPopupModal(trc("hotkeys.reset_to_defaults_popup"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f), "%s", trc("label.warning"));
            ImGui::Text("%s", trc("hotkeys.reset_warning"));
            ImGui::Text("%s", trc("label.action_cannot_be_undone"));
            ImGui::Separator();
            if (ImGui::Button(trc("button.confirm_reset"), ImVec2(120, 0))) {
                g_config.hotkeys = GetDefaultHotkeys();
                ResetAllHotkeySecondaryModes();
                // DEADLOCK FIX: Use internal version since g_configMutex is already held
                std::lock_guard<std::mutex> hotkeyLock(g_hotkeyMainKeysMutex);
                RebuildHotkeyMainKeys_Internal();
                g_configIsDirty = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetItemDefaultFocus();
            ImGui::SameLine();
            if (ImGui::Button(trc("button.cancel"), ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }

        ImGui::SeparatorText(trc("hotkeys.sensitivity_hotkeys"));
        ImGui::TextDisabled(trc("label.question_mark"));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", trc("hotkeys.tooltip.sensitivity_hotkeys"));
        }

        int sens_hotkey_to_remove = -1;
        for (size_t i = 0; i < g_config.sensitivityHotkeys.size(); ++i) {
            auto& sensHotkey = g_config.sensitivityHotkeys[i];
            ImGui::PushID(("sens_hotkey_" + std::to_string(i)).c_str());

            std::string sensKeyStr = GetKeyComboString(sensHotkey.keys);
            std::string sensNodeLabel = tr("hotkeys.sensitivity_prefix") + (sensKeyStr.empty() ? trc("hotkeys.none") : sensKeyStr) + " -> " +
                                        std::to_string(sensHotkey.sensitivity).substr(0, 4) + "x";

            if (ImGui::Button((tr("hotkeys.button_x") + "##del_sens_" + std::to_string(i)).c_str(), ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()))) {
                ImGui::OpenPopup((tr("hotkeys.delete_sens_hotkey") + "##" + std::to_string(i)).c_str());
            }

            if (ImGui::BeginPopupModal((tr("hotkeys.delete_sens_hotkey") + "##" + std::to_string(i)).c_str(), NULL,
                                       ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("%s", trc("hotkeys.delete_sens_hotkey_confirm"));
                ImGui::Separator();
                if (ImGui::Button(trc("button.ok"))) {
                    sens_hotkey_to_remove = (int)i;
                    g_configIsDirty = true;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button(trc("button.cancel"))) { ImGui::CloseCurrentPopup(); }
                ImGui::EndPopup();
            }

            ImGui::SameLine();
            bool sensNodeOpen = ImGui::TreeNodeEx("##sens_hotkey_node", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", sensNodeLabel.c_str());

            if (sensNodeOpen) {
                const char* sensButtonLabel =
                    (s_sensHotkeyToBind == (int)i) ? trc("hotkeys.press_keys") : (sensKeyStr.empty() ? trc("hotkeys.none") : sensKeyStr.c_str());
                if (ImGui::Button(sensButtonLabel)) {
                    s_sensHotkeyToBind = (int)i;
                    s_mainHotkeyToBind = -1;
                    s_altHotkeyToBind = { -1, -1 };
                    s_exclusionToBind = { -1, -1 };
                    MarkHotkeyBindingActive();
                }
                ImGui::SameLine();
                HelpMarker(trc("hotkeys.sens.tooltip.bind_hotkey"));

                ImGui::SeparatorText(trc("hotkeys.sensitivity_section"));
                if (ImGui::Checkbox((tr("mirrors.separate_x_y") + "##sens").c_str(), &sensHotkey.separateXY)) {
                    if (!sensHotkey.separateXY) {
                        sensHotkey.sensitivityX = sensHotkey.sensitivity;
                        sensHotkey.sensitivityY = sensHotkey.sensitivity;
                    }
                    g_configIsDirty = true;
                }
                ImGui::SameLine();
                HelpMarker(trc("hotkeys.sens.tooltip.separate_xy"));

                if (sensHotkey.separateXY) {
                    RawInputSensitivityNote();
                    ImGui::Text("%s", trc("hotkeys.sensitivity_x"));
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(150);
                    if (ImGui::SliderFloat("##sensX", &sensHotkey.sensitivityX, 0.001f, 10.0f, trc("hotkeys.sensitivity_format"))) { g_configIsDirty = true; }

                    RawInputSensitivityNote();
                    ImGui::Text("%s", trc("hotkeys.sensitivity_y"));
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(150);
                    if (ImGui::SliderFloat("##sensY", &sensHotkey.sensitivityY, 0.001f, 10.0f, trc("hotkeys.sensitivity_format"))) { g_configIsDirty = true; }
                } else {
                    RawInputSensitivityNote();
                    ImGui::Text("%s:", trc("hotkeys.sensitivity_section"));
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(150);
                    if (ImGui::SliderFloat("##sens", &sensHotkey.sensitivity, 0.001f, 10.0f, trc("hotkeys.sensitivity_format"))) { g_configIsDirty = true; }
                }

                if (ImGui::TreeNode((tr("hotkeys.required_game_states") + "##sens").c_str())) {
                    bool isAnySelected = sensHotkey.conditions.gameState.empty();

                    bool anyToggle = isAnySelected;
                    if (ImGui::Checkbox((tr("hotkeys.any") + "##sens").c_str(), &anyToggle) && anyToggle) {
                        sensHotkey.conditions.gameState.clear();
                        g_configIsDirty = true;
                    }

                    const bool stateSourceUnavailable = !g_isStateOutputAvailable.load(std::memory_order_acquire);

                    for (const char* state : guiGameStates) {
                        auto it = std::find(sensHotkey.conditions.gameState.begin(), sensHotkey.conditions.gameState.end(), state);
                        bool is_selected = (it != sensHotkey.conditions.gameState.end());

                        if (strcmp(state, "generating") == 0) {
                            auto waitingIt =
                                std::find(sensHotkey.conditions.gameState.begin(), sensHotkey.conditions.gameState.end(), "waiting");
                            is_selected = is_selected || (waitingIt != sensHotkey.conditions.gameState.end());
                        }

                        const bool disableThisState =
                            stateSourceUnavailable && strcmp(state, "any,cursor_free") != 0 && strcmp(state, "any,cursor_grabbed") != 0;
                        if (disableThisState) { ImGui::BeginDisabled(); }

                        const char* friendlyName = getGameStateFriendlyName(state);
                        if (ImGui::Checkbox((std::string(friendlyName) + "##sens").c_str(), &is_selected)) {
                            if (strcmp(state, "generating") == 0) {
                                if (is_selected) {
                                    auto generateIt = std::find(sensHotkey.conditions.gameState.begin(), sensHotkey.conditions.gameState.end(),
                                                               "generating");
                                    auto waitingIt = std::find(sensHotkey.conditions.gameState.begin(), sensHotkey.conditions.gameState.end(),
                                                              "waiting");

                                    if (generateIt == sensHotkey.conditions.gameState.end()) {
                                        sensHotkey.conditions.gameState.push_back("generating");
                                    }
                                    if (waitingIt == sensHotkey.conditions.gameState.end()) {
                                        sensHotkey.conditions.gameState.push_back("waiting");
                                    }
                                } else {
                                    auto& gs = sensHotkey.conditions.gameState;
                                    gs.erase(std::remove(gs.begin(), gs.end(), "waiting"), gs.end());
                                    gs.erase(std::remove(gs.begin(), gs.end(), "generating"), gs.end());
                                }
                            } else {
                                if (is_selected) {
                                    sensHotkey.conditions.gameState.push_back(state);
                                } else {
                                    sensHotkey.conditions.gameState.erase(it);
                                }
                            }
                            g_configIsDirty = true;
                        }

                        if (disableThisState) { ImGui::EndDisabled(); }
                        if (disableThisState && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                            ImGui::SetTooltip("%s", trc("hotkeys.state_source.warning_tooltip"));
                        }
                    }

                    ImGui::TreePop();
                }

                if (ImGui::Checkbox((tr("hotkeys.toggle") + "##sens").c_str(), &sensHotkey.toggle)) { g_configIsDirty = true; }
                ImGui::SameLine();
                HelpMarker(
                    trc("hotkeys.sens.tooltip.toggle"));

                ImGui::Text("%s", trc("hotkeys.debounce"));
                ImGui::SameLine();
                ImGui::SetNextItemWidth(100);
                if (ImGui::InputInt((tr("hotkeys.sensitivity_ms") + "##sens_debounce").c_str(), &sensHotkey.debounce)) {
                    sensHotkey.debounce = (std::max)(0, (std::min)(1000, sensHotkey.debounce));
                    g_configIsDirty = true;
                }

                ImGui::TreePop();
            }

            ImGui::PopID();
        }

        if (sens_hotkey_to_remove != -1) {
            g_config.sensitivityHotkeys.erase(g_config.sensitivityHotkeys.begin() + sens_hotkey_to_remove);
            std::lock_guard<std::mutex> hotkeyLock(g_hotkeyMainKeysMutex);
            RebuildHotkeyMainKeys_Internal();
        }

        if (ImGui::Button(trc("hotkeys.add_sensitivity_hotkey"))) {
            try {
                SensitivityHotkeyConfig newSensHotkey;
                newSensHotkey.keys = std::vector<DWORD>();
                newSensHotkey.sensitivity = 1.0f;
                newSensHotkey.separateXY = false;
                newSensHotkey.sensitivityX = 1.0f;
                newSensHotkey.sensitivityY = 1.0f;
                newSensHotkey.debounce = 100;
                g_config.sensitivityHotkeys.push_back(std::move(newSensHotkey));
                std::lock_guard<std::mutex> hotkeyLock(g_hotkeyMainKeysMutex);
                RebuildHotkeyMainKeys_Internal();
                g_configIsDirty = true;
            } catch (const std::exception& e) { Log("ERROR: Failed to add sensitivity hotkey: {}", e.what()); }
        }

        ImGui::EndTabItem();
    }
}


