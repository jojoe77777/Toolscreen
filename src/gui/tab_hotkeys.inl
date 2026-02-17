// Only show Hotkeys tab if resolution changing is supported (1.13+)
if (IsResolutionChangeSupported(g_gameVersion)) {
    if (ImGui::BeginTabItem("Hotkeys")) {
        g_currentlyEditingMirror = ""; // Disable image drag mode in other tabs
        g_imageDragMode.store(false);
        g_windowOverlayDragMode.store(false);

        if (!g_isStateOutputAvailable.load(std::memory_order_acquire)) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "Warning: wpstateout.txt not found.");
            ImGui::TextWrapped("State-based hotkey restrictions are currently disabled, so hotkeys will trigger regardless of required "
                               "game states. Install the State Output mod to enable these conditions.");
            ImGui::Separator();
        }

        // GUI Hotkey Section
        ImGui::SeparatorText("GUI Hotkey");
        ImGui::PushID("gui_hotkey");
        std::string guiKeyStr = GetKeyComboString(g_config.guiHotkey);
        std::string guiNode_label = "Open/Close GUI: " + (guiKeyStr.empty() ? "[None]" : guiKeyStr);

        bool guiNode_open = ImGui::TreeNodeEx("##gui_hotkey_node", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", guiNode_label.c_str());
        if (guiNode_open) {
            const char* gui_button_label =
                (s_mainHotkeyToBind == -999) ? "[Press Keys...]" : (guiKeyStr.empty() ? "[None]" : guiKeyStr.c_str());
            if (ImGui::Button(gui_button_label)) {
                s_mainHotkeyToBind = -999; // Special ID for GUI hotkey
                s_altHotkeyToBind = { -1, -1 };
                s_exclusionToBind = { -1, -1 };
            }
            ImGui::TreePop();
        }
        ImGui::PopID();

        ImGui::SeparatorText("Mode Hotkeys");
        int hotkey_to_remove = -1;
        for (size_t i = 0; i < g_config.hotkeys.size(); ++i) {
            auto& hotkey = g_config.hotkeys[i];
            ImGui::PushID((int)i);
            std::string keyStr = GetKeyComboString(hotkey.keys);
            std::string node_label = "Hotkey: " + (keyStr.empty() ? "[None]" : keyStr);

            // X button on the left
            if (ImGui::Button(("X##del_hotkey_" + std::to_string(i)).c_str(), ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()))) {
                ImGui::OpenPopup(("Delete Hotkey?##" + std::to_string(i)).c_str());
            }

            // Popup modal outside of node_open block so it can be displayed even when collapsed
            if (ImGui::BeginPopupModal(("Delete Hotkey?##" + std::to_string(i)).c_str(), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("Are you sure you want to delete this hotkey?");
                ImGui::Separator();
                if (ImGui::Button("OK")) {
                    hotkey_to_remove = (int)i;
                    g_configIsDirty = true;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel")) { ImGui::CloseCurrentPopup(); }
                ImGui::EndPopup();
            }

            ImGui::SameLine();
            bool node_open = ImGui::TreeNodeEx("##hotkey_node", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", node_label.c_str());

            if (node_open) {
                const char* button_label = (s_mainHotkeyToBind == i) ? "[Press Keys...]" : (keyStr.empty() ? "[None]" : keyStr.c_str());
                if (ImGui::Button(button_label)) {
                    s_mainHotkeyToBind = (int)i;
                    s_altHotkeyToBind = { -1, -1 };
                    s_exclusionToBind = { -1, -1 };
                }

                ImGui::SeparatorText("Target Mode");
                ImGui::SetNextItemWidth(150);
                const char* modeDisplay = hotkey.secondaryMode.empty() ? "[None]" : hotkey.secondaryMode.c_str();
                if (ImGui::BeginCombo("Mode", modeDisplay)) {
                    // Add "[None]" option to clear mode
                    if (ImGui::Selectable("[None]", hotkey.secondaryMode.empty())) {
                        hotkey.secondaryMode = "";
                        SetHotkeySecondaryMode(i, "");
                        g_configIsDirty = true;
                    }
                    for (const auto& mode : g_config.modes) {
                        // Don't allow selecting Fullscreen as target (it's the implicit toggle-back mode)
                        bool is_fullscreen = EqualsIgnoreCase(mode.id, "Fullscreen");
                        if (is_fullscreen) { ImGui::BeginDisabled(); }
                        if (ImGui::Selectable(mode.id.c_str(), false, is_fullscreen ? ImGuiSelectableFlags_Disabled : 0)) {
                            hotkey.secondaryMode = mode.id;
                            SetHotkeySecondaryMode(i, mode.id); // Update runtime state immediately
                            g_configIsDirty = true;
                        }
                        if (is_fullscreen) {
                            ImGui::EndDisabled();
                            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                                ImGui::SetTooltip("Fullscreen is the implicit toggle-back mode");
                            }
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(?)");
                if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Pressing this hotkey toggles between Fullscreen and this mode"); }

                ImGui::SeparatorText("Alternative Secondary Modes");
                int alt_to_remove = -1;
                for (size_t j = 0; j < hotkey.altSecondaryModes.size(); ++j) {
                    auto& alt = hotkey.altSecondaryModes[j];
                    ImGui::PushID(static_cast<int>(j));

                    if (ImGui::Button("X")) { alt_to_remove = (int)j; }
                    ImGui::SameLine();

                    std::string altKeyStr = GetKeyComboString(alt.keys);
                    bool is_binding_this = (s_altHotkeyToBind.hotkey_idx == i && s_altHotkeyToBind.alt_idx == j);
                    const char* alt_button_label = is_binding_this ? "[...]" : (altKeyStr.empty() ? "[None]" : altKeyStr.c_str());
                    if (ImGui::Button(alt_button_label, ImVec2(100, 0))) {
                        s_altHotkeyToBind = { (int)i, (int)j };
                        s_mainHotkeyToBind = -1;
                        s_exclusionToBind = { -1, -1 };
                    }
                    ImGui::SameLine();

                    ImGui::SetNextItemWidth(150);
                    const char* altModeDisplay = alt.mode.empty() ? "[None]" : alt.mode.c_str();
                    if (ImGui::BeginCombo("Mode", altModeDisplay)) {
                        // Add "[None]" option to clear alternative mode
                        if (ImGui::Selectable("[None]", alt.mode.empty())) {
                            alt.mode = "";
                            g_configIsDirty = true;
                        }
                        for (const auto& mode : g_config.modes) {
                            // Don't allow selecting Fullscreen as target
                            bool is_fullscreen = EqualsIgnoreCase(mode.id, "Fullscreen");
                            if (is_fullscreen) { ImGui::BeginDisabled(); }
                            if (ImGui::Selectable(mode.id.c_str(), false, is_fullscreen ? ImGuiSelectableFlags_Disabled : 0)) {
                                alt.mode = mode.id;
                                g_configIsDirty = true;
                            }
                            if (is_fullscreen) {
                                ImGui::EndDisabled();
                                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                                    ImGui::SetTooltip("Fullscreen is the implicit toggle-back mode");
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::PopID();
                }
                if (alt_to_remove != -1) {
                    hotkey.altSecondaryModes.erase(hotkey.altSecondaryModes.begin() + alt_to_remove);
                    g_configIsDirty = true;
                }
                if (ImGui::Button("Add Alternative Mode")) {
                    hotkey.altSecondaryModes.push_back(AltSecondaryMode{});
                    g_configIsDirty = true;
                }

                ImGui::Separator();
                ImGui::Columns(2, "debounce_col", false);
                ImGui::SetColumnWidth(0, 150);
                ImGui::Text("Debounce (ms)");
                ImGui::NextColumn();
                if (Spinner("##debounce", &hotkey.debounce, 1, 0)) g_configIsDirty = true;
                ImGui::Columns(1);

                if (ImGui::Checkbox("Trigger on Release", &hotkey.triggerOnRelease)) { g_configIsDirty = true; }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("When checked, the hotkey triggers when the key is released instead of pressed");
                }

                if (ImGui::Checkbox("Block key from game", &hotkey.blockKeyFromGame)) { g_configIsDirty = true; }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("When enabled, the key event that matches this hotkey is consumed and will NOT be forwarded to the game.\n"
                                     "The hotkey will still trigger normally.");
                }

                if (ImGui::Checkbox("Allow exit to Fullscreen regardless of game state",
                                   &hotkey.allowExitToFullscreenRegardlessOfGameState)) {
                    g_configIsDirty = true;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("When enabled, toggling BACK to Fullscreen is allowed even if required game states are not met.\n"
                                     "Toggling INTO the target mode still requires the configured game state.");
                }

                if (ImGui::TreeNode("Required Game States")) {
                    // Check if "Any" state is active (empty gameState array)
                    bool isAnySelected = hotkey.conditions.gameState.empty();

                    // "Any" option at the top
                    if (ImGui::Checkbox("Any", &isAnySelected)) {
                        if (isAnySelected) {
                            // Clear all game states to represent "Any"
                            hotkey.conditions.gameState.clear();
                        } else {
                            // When unticking "Any", select common states
                            hotkey.conditions.gameState.clear();
                            hotkey.conditions.gameState.push_back("wall");
                            hotkey.conditions.gameState.push_back("inworld,cursor_free");
                            hotkey.conditions.gameState.push_back("inworld,cursor_grabbed");
                            hotkey.conditions.gameState.push_back("title");
                        }
                        g_configIsDirty = true;
                    }

                    // Disable other options when "Any" is selected
                    if (isAnySelected) { ImGui::BeginDisabled(); }

                    for (const char* state : guiGameStates) {
                        auto it = std::find(hotkey.conditions.gameState.begin(), hotkey.conditions.gameState.end(), state);
                        bool is_selected = (it != hotkey.conditions.gameState.end());

                        // Special handling for "generating" - check for both "generating" and "waiting"
                        if (strcmp(state, "generating") == 0) {
                            auto waitingIt = std::find(hotkey.conditions.gameState.begin(), hotkey.conditions.gameState.end(), "waiting");
                            is_selected = is_selected || (waitingIt != hotkey.conditions.gameState.end());
                        }

                        const char* friendlyName = getGameStateFriendlyName(state);
                        if (ImGui::Checkbox(friendlyName, &is_selected)) {
                            if (strcmp(state, "generating") == 0) {
                                // Special handling for "World Generation" - affects both "generating" and
                                // "waiting"
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
                                    // IMPORTANT: std::vector::erase invalidates iterators at/after the erase point.
                                    // Erase by value (erase-remove) to avoid using invalidated iterators.
                                    auto& gs = hotkey.conditions.gameState;
                                    gs.erase(std::remove(gs.begin(), gs.end(), "waiting"), gs.end());
                                    gs.erase(std::remove(gs.begin(), gs.end(), "generating"), gs.end());
                                }
                            } else {
                                // Normal handling for other states
                                if (is_selected) {
                                    hotkey.conditions.gameState.push_back(state);
                                } else {
                                    hotkey.conditions.gameState.erase(it);
                                }
                            }
                            g_configIsDirty = true;
                        }
                    }

                    if (isAnySelected) { ImGui::EndDisabled(); }

                    ImGui::TreePop();
                }

                if (ImGui::TreeNode("Exclusion Keys")) {
                    int exclusion_to_remove = -1;
                    auto& exclusions = hotkey.conditions.exclusions;
                    for (size_t j = 0; j < exclusions.size(); ++j) {
                        ImGui::PushID((int)j);
                        bool is_binding_this = (s_exclusionToBind.hotkey_idx == i && s_exclusionToBind.exclusion_idx == j);
                        std::string ex_key_str = is_binding_this ? "[...]" : VkToString(exclusions[j]);
                        const char* ex_button_label = ex_key_str.c_str();

                        if (ImGui::Button(ex_button_label, ImVec2(100, 0))) {
                            if (!is_binding_this) {
                                s_exclusionToBind = { (int)i, (int)j };
                                s_mainHotkeyToBind = -1;
                                s_altHotkeyToBind = { -1, -1 };
                            } else {
                                s_exclusionToBind = { -1, -1 };
                            }
                        }
                        ImGui::SameLine();
                        if (ImGui::Button(("x##del_ex_" + std::to_string(j)).c_str(),
                                          ImVec2(ImGui::GetItemRectSize().y, ImGui::GetItemRectSize().y))) {
                            exclusion_to_remove = (int)j;
                        }
                        ImGui::PopID();
                    }
                    if (exclusion_to_remove != -1) {
                        exclusions.erase(exclusions.begin() + exclusion_to_remove);
                        g_configIsDirty = true;
                    }
                    if (ImGui::Button("+ Add Exclusion")) {
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
            ResetAllHotkeySecondaryModes(); // Sync secondary mode state after removal
            // DEADLOCK FIX: Use internal version since g_configMutex is already held
            std::lock_guard<std::mutex> hotkeyLock(g_hotkeyMainKeysMutex);
            RebuildHotkeyMainKeys_Internal();
        }
        ImGui::Separator();
        if (ImGui::Button("Add New Hotkey")) {
            try {
                HotkeyConfig newHotkey;
                newHotkey.keys = std::vector<DWORD>(); // Explicitly initialize empty vector
                newHotkey.mainMode = g_config.defaultMode.empty() ? "Fullscreen" : g_config.defaultMode;
                newHotkey.secondaryMode = "";                                  // Initialize to empty
                newHotkey.altSecondaryModes = std::vector<AltSecondaryMode>(); // Explicitly initialize empty vector
                newHotkey.conditions = HotkeyConditions();                     // Initialize conditions
                newHotkey.debounce = 100;
                g_config.hotkeys.push_back(std::move(newHotkey));        // Use move semantics
                ResizeHotkeySecondaryModes(g_config.hotkeys.size());     // Sync runtime state
                SetHotkeySecondaryMode(g_config.hotkeys.size() - 1, ""); // Init new entry
                std::lock_guard<std::mutex> hotkeyLock(g_hotkeyMainKeysMutex);
                RebuildHotkeyMainKeys_Internal();
                g_configIsDirty = true;
            } catch (const std::exception& e) { Log(std::string("ERROR: Failed to add new hotkey: ") + e.what()); }
        }

        ImGui::SameLine();
        if (ImGui::Button("Reset to Defaults##hotkeys")) { ImGui::OpenPopup("Reset Hotkeys to Defaults?"); }

        if (ImGui::BeginPopupModal("Reset Hotkeys to Defaults?", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f), "WARNING:");
            ImGui::Text("This will delete ALL custom hotkeys and restore the default hotkeys.");
            ImGui::Text("This action cannot be undone.");
            ImGui::Separator();
            if (ImGui::Button("Confirm Reset", ImVec2(120, 0))) {
                g_config.hotkeys = GetDefaultHotkeys();
                ResetAllHotkeySecondaryModes(); // Sync secondary mode state after reset
                // DEADLOCK FIX: Use internal version since g_configMutex is already held
                std::lock_guard<std::mutex> hotkeyLock(g_hotkeyMainKeysMutex);
                RebuildHotkeyMainKeys_Internal();
                g_configIsDirty = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetItemDefaultFocus();
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }

        // ============================================================================
        // SENSITIVITY HOTKEYS SECTION
        // ============================================================================
        ImGui::SeparatorText("Sensitivity Hotkeys");
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Sensitivity hotkeys temporarily override mouse sensitivity.\n"
                              "The override persists until the next mode change.");
        }

        int sens_hotkey_to_remove = -1;
        for (size_t i = 0; i < g_config.sensitivityHotkeys.size(); ++i) {
            auto& sensHotkey = g_config.sensitivityHotkeys[i];
            ImGui::PushID(("sens_hotkey_" + std::to_string(i)).c_str());

            std::string sensKeyStr = GetKeyComboString(sensHotkey.keys);
            std::string sensNodeLabel = "Sensitivity: " + (sensKeyStr.empty() ? "[None]" : sensKeyStr) + " -> " +
                                        std::to_string(sensHotkey.sensitivity).substr(0, 4) + "x";

            // X button on the left
            if (ImGui::Button(("X##del_sens_" + std::to_string(i)).c_str(), ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()))) {
                ImGui::OpenPopup(("Delete Sensitivity Hotkey?##" + std::to_string(i)).c_str());
            }

            // Popup modal
            if (ImGui::BeginPopupModal(("Delete Sensitivity Hotkey?##" + std::to_string(i)).c_str(), NULL,
                                       ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("Are you sure you want to delete this sensitivity hotkey?");
                ImGui::Separator();
                if (ImGui::Button("OK")) {
                    sens_hotkey_to_remove = (int)i;
                    g_configIsDirty = true;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel")) { ImGui::CloseCurrentPopup(); }
                ImGui::EndPopup();
            }

            ImGui::SameLine();
            bool sensNodeOpen = ImGui::TreeNodeEx("##sens_hotkey_node", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", sensNodeLabel.c_str());

            if (sensNodeOpen) {
                // Key binding button
                const char* sensButtonLabel =
                    (s_sensHotkeyToBind == (int)i) ? "[Press Keys...]" : (sensKeyStr.empty() ? "[None]" : sensKeyStr.c_str());
                if (ImGui::Button(sensButtonLabel)) {
                    s_sensHotkeyToBind = (int)i;
                    s_mainHotkeyToBind = -1;
                    s_altHotkeyToBind = { -1, -1 };
                    s_exclusionToBind = { -1, -1 };
                }
                ImGui::SameLine();
                HelpMarker("Click to bind a key combination for this sensitivity override.");

                // Sensitivity value
                ImGui::SeparatorText("Sensitivity");
                if (ImGui::Checkbox("Separate X/Y##sens", &sensHotkey.separateXY)) {
                    if (!sensHotkey.separateXY) {
                        // When disabling, sync X/Y to unified value
                        sensHotkey.sensitivityX = sensHotkey.sensitivity;
                        sensHotkey.sensitivityY = sensHotkey.sensitivity;
                    }
                    g_configIsDirty = true;
                }
                ImGui::SameLine();
                HelpMarker("Enable to set different sensitivity values for X and Y axes.");

                if (sensHotkey.separateXY) {
                    ImGui::Text("X Sensitivity:");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(150);
                    if (ImGui::SliderFloat("##sensX", &sensHotkey.sensitivityX, 0.001f, 10.0f, "%.3fx")) { g_configIsDirty = true; }

                    ImGui::Text("Y Sensitivity:");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(150);
                    if (ImGui::SliderFloat("##sensY", &sensHotkey.sensitivityY, 0.001f, 10.0f, "%.3fx")) { g_configIsDirty = true; }
                } else {
                    ImGui::Text("Sensitivity:");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(150);
                    if (ImGui::SliderFloat("##sens", &sensHotkey.sensitivity, 0.001f, 10.0f, "%.3fx")) { g_configIsDirty = true; }
                }

                if (ImGui::TreeNode("Required Game States##sens")) {
                    // Check if "Any" state is active (empty gameState array)
                    bool isAnySelected = sensHotkey.conditions.gameState.empty();

                    // "Any" option at the top
                    if (ImGui::Checkbox("Any##sens", &isAnySelected)) {
                        if (isAnySelected) {
                            // Clear all game states to represent "Any"
                            sensHotkey.conditions.gameState.clear();
                        } else {
                            // When unticking "Any", select common states
                            sensHotkey.conditions.gameState.clear();
                            sensHotkey.conditions.gameState.push_back("wall");
                            sensHotkey.conditions.gameState.push_back("inworld,cursor_free");
                            sensHotkey.conditions.gameState.push_back("inworld,cursor_grabbed");
                            sensHotkey.conditions.gameState.push_back("title");
                        }
                        g_configIsDirty = true;
                    }

                    // Disable other options when "Any" is selected
                    if (isAnySelected) { ImGui::BeginDisabled(); }

                    for (const char* state : guiGameStates) {
                        auto it = std::find(sensHotkey.conditions.gameState.begin(), sensHotkey.conditions.gameState.end(), state);
                        bool is_selected = (it != sensHotkey.conditions.gameState.end());

                        // Special handling for "generating" - check for both "generating" and "waiting"
                        if (strcmp(state, "generating") == 0) {
                            auto waitingIt =
                                std::find(sensHotkey.conditions.gameState.begin(), sensHotkey.conditions.gameState.end(), "waiting");
                            is_selected = is_selected || (waitingIt != sensHotkey.conditions.gameState.end());
                        }

                        const char* friendlyName = getGameStateFriendlyName(state);
                        if (ImGui::Checkbox((std::string(friendlyName) + "##sens").c_str(), &is_selected)) {
                            if (strcmp(state, "generating") == 0) {
                                // Special handling for "World Generation" - affects both "generating" and "waiting"
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
                                    // IMPORTANT: std::vector::erase invalidates iterators at/after the erase point.
                                    // Erase by value (erase-remove) to avoid using invalidated iterators.
                                    auto& gs = sensHotkey.conditions.gameState;
                                    gs.erase(std::remove(gs.begin(), gs.end(), "waiting"), gs.end());
                                    gs.erase(std::remove(gs.begin(), gs.end(), "generating"), gs.end());
                                }
                            } else {
                                // Normal handling for other states
                                if (is_selected) {
                                    sensHotkey.conditions.gameState.push_back(state);
                                } else {
                                    sensHotkey.conditions.gameState.erase(it);
                                }
                            }
                            g_configIsDirty = true;
                        }
                    }

                    if (isAnySelected) { ImGui::EndDisabled(); }

                    ImGui::TreePop();
                }

                // Toggle mode
                if (ImGui::Checkbox("Toggle##sens", &sensHotkey.toggle)) { g_configIsDirty = true; }
                ImGui::SameLine();
                HelpMarker(
                    "When enabled, pressing the hotkey again resets sensitivity back to normal (mode override or global sensitivity).");

                // Debounce
                ImGui::Text("Debounce:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(100);
                if (ImGui::InputInt("ms##sens_debounce", &sensHotkey.debounce)) {
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

        if (ImGui::Button("Add Sensitivity Hotkey")) {
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
            } catch (const std::exception& e) { Log(std::string("ERROR: Failed to add sensitivity hotkey: ") + e.what()); }
        }

        ImGui::EndTabItem();
    }
}
