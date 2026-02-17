if (ImGui::BeginTabItem("Inputs")) {
    g_currentlyEditingMirror = "";
    g_imageDragMode.store(false);
    g_windowOverlayDragMode.store(false);

    // Sub-tabs for Mouse and Keyboard
    if (ImGui::BeginTabBar("InputsSubTabs")) {
        // =====================================================================
        // MOUSE SUB-TAB
        // =====================================================================
        if (ImGui::BeginTabItem("Mouse")) {
            ImGui::SeparatorText("Mouse Settings");

            ImGui::Text("Mouse Sensitivity:");
            ImGui::SetNextItemWidth(600);
            if (ImGui::SliderFloat("##mouseSensitivity", &g_config.mouseSensitivity, 0.001f, 10.0f, "%.3fx")) { g_configIsDirty = true; }
            ImGui::SameLine();
            HelpMarker("Multiplies mouse movement for raw input events (mouselook).\n"
                       "1.0 = normal sensitivity, higher = faster, lower = slower.\n"
                       "Useful for adjusting mouse speed when using stretched resolutions.");

            ImGui::Text("Windows Mouse Speed:");
            ImGui::SetNextItemWidth(600);
            int windowsSpeedValue = g_config.windowsMouseSpeed;
            if (ImGui::SliderInt("##windowsMouseSpeed", &windowsSpeedValue, 0, 20, windowsSpeedValue == 0 ? "Disabled" : "%d")) {
                g_config.windowsMouseSpeed = windowsSpeedValue;
                g_configIsDirty = true;
            }
            ImGui::SameLine();
            HelpMarker("Temporarily overrides Windows mouse speed setting while game is running.\n"
                       "0 = Disabled (use system setting)\n"
                       "1-20 = Override Windows mouse speed (10 = default Windows speed)\n"
                       "Affects cursor movement in game menus. Original setting is restored on exit.");

            if (g_gameVersion < GameVersion(1, 13, 0)) {
                if (ImGui::Checkbox("Let Cursor Escape Window", &g_config.allowCursorEscape)) { g_configIsDirty = true; }
                ImGui::SameLine();
                HelpMarker("For pre 1.13, prevents the cursor being locked to the game window when in fullscreen");
            }

            ImGui::Spacing();
            ImGui::SeparatorText("Cursor Configuration");

            if (ImGui::Checkbox("Enable Custom Cursors", &g_config.cursors.enabled)) {
                g_configIsDirty = true;
                // Schedule cursor reload (will happen outside GUI rendering to avoid deadlock)
                g_cursorsNeedReload = true;
            }
            ImGui::SameLine();
            HelpMarker("When enabled, the mouse cursor will change based on the current game state.");

            ImGui::Spacing();

            if (g_config.cursors.enabled) {
                ImGui::Text("Configure cursors for different game states:");
                ImGui::Spacing();

                // Available cursor options
                struct CursorOption {
                    std::string key;
                    std::string name;
                    std::string description;
                };

                // Build cursor list dynamically from detected cursors
                static std::vector<CursorOption> availableCursors;
                static bool cursorListInitialized = false;

                if (!cursorListInitialized) {
                    // Initialize cursor definitions first
                    CursorTextures::InitializeCursorDefinitions();

                    // Get all available cursor names
                    auto cursorNames = CursorTextures::GetAvailableCursorNames();

                    // Build display list from available cursors
                    for (const auto& cursorName : cursorNames) {
                        std::string displayName = cursorName;

                        // Create user-friendly name (capitalize first letter, replace underscores/hyphens with spaces)
                        if (!displayName.empty()) {
                            displayName[0] = std::toupper(displayName[0]);
                            for (auto& c : displayName) {
                                if (c == '_' || c == '-') c = ' ';
                            }
                        }

                        std::string description;
                        // Determine description based on cursor name
                        if (cursorName.find("Cross") != std::string::npos) {
                            description = "Crosshair cursor";
                        } else if (cursorName.find("Arrow") != std::string::npos) {
                            description = "Arrow pointer cursor";
                        } else {
                            description = "Custom cursor";
                        }

                        availableCursors.push_back({ cursorName, displayName, description });
                    }

                    cursorListInitialized = true;
                }

                // Fixed 3 cursor configurations
                struct CursorConfigUI {
                    const char* name;
                    CursorConfig* config;
                };

                CursorConfigUI cursors[] = { { "Title Screen", &g_config.cursors.title },
                                             { "Wall", &g_config.cursors.wall },
                                             { "In World", &g_config.cursors.ingame } };

                for (int i = 0; i < 3; ++i) {
                    auto& cursorUI = cursors[i];
                    auto& cursorConfig = *cursorUI.config;
                    ImGui::PushID(i);

                    ImGui::SeparatorText(cursorUI.name);

                    // Find current cursor display name
                    const char* currentCursorName = cursorConfig.cursorName.c_str();
                    std::string currentDescription = "";
                    for (const auto& option : availableCursors) {
                        if (cursorConfig.cursorName == option.key) {
                            currentCursorName = option.name.c_str();
                            currentDescription = option.description;
                            break;
                        }
                    }

                    // Cursor dropdown
                    ImGui::Text("Cursor:");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.35f);
                    if (ImGui::BeginCombo("##cursor", currentCursorName)) {
                        // Display each cursor option with name and description
                        for (const auto& option : availableCursors) {
                            ImGui::PushID(option.key.c_str());

                            bool is_selected = (cursorConfig.cursorName == option.key);

                            if (ImGui::Selectable(option.name.c_str(), is_selected)) {
                                cursorConfig.cursorName = option.key;
                                g_configIsDirty = true;
                                // Schedule cursor reload (will happen outside GUI rendering to avoid deadlock)
                                g_cursorsNeedReload = true;

                                // Apply cursor immediately via SetCursor (loads on-demand if needed)
                                std::wstring cursorPath;
                                UINT loadType = IMAGE_CURSOR;
                                CursorTextures::GetCursorPathByName(option.key, cursorPath, loadType);

                                const CursorTextures::CursorData* cursorData =
                                    CursorTextures::LoadOrFindCursor(cursorPath, loadType, cursorConfig.cursorSize);
                                if (cursorData && cursorData->hCursor) { SetCursor(cursorData->hCursor); }
                            }

                            // Show description on hover
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

                    // Show current cursor description on hover
                    if (!currentDescription.empty() && ImGui::IsItemHovered()) {
                        ImGui::BeginTooltip();
                        ImGui::TextUnformatted(currentDescription.c_str());
                        ImGui::EndTooltip();
                    }

                    // Cursor size slider on the same line
                    ImGui::SameLine();
                    ImGui::Spacing();
                    ImGui::SameLine();
                    ImGui::Text("Size:");
                    ImGui::SameLine();

                    // Slider takes remaining width
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.8f); // Leave space for help marker
                    int sliderValue = cursorConfig.cursorSize;
                    if (ImGui::SliderInt("##cursorSize", &sliderValue, 8, 144, "%d px", ImGuiSliderFlags_AlwaysClamp)) {
                        int newSize = sliderValue;
                        if (newSize != cursorConfig.cursorSize) {
                            cursorConfig.cursorSize = newSize;
                            g_configIsDirty = true;

                            // Apply cursor immediately via SetCursor (loads on-demand if needed)
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

                if (ImGui::Button("Reset to Defaults##cursors")) { ImGui::OpenPopup("Reset Cursors to Defaults?"); }

                if (ImGui::BeginPopupModal("Reset Cursors to Defaults?", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
                    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f), "WARNING:");
                    ImGui::Text("This will reset all cursor settings to their default values.");
                    ImGui::Text("This action cannot be undone.");
                    ImGui::Separator();
                    if (ImGui::Button("Confirm Reset", ImVec2(120, 0))) {
                        g_config.cursors = GetDefaultCursors();
                        g_configIsDirty = true;
                        g_cursorsNeedReload = true;
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SetItemDefaultFocus();
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel", ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
                    ImGui::EndPopup();
                }
            }

            ImGui::EndTabItem();
        }

        // =====================================================================
        // KEYBOARD SUB-TAB
        // =====================================================================
        if (ImGui::BeginTabItem("Keyboard")) {
            // --- Key Repeat Rate Settings ---
            ImGui::SeparatorText("Key Repeat Rate");

            ImGui::Text("Key Repeat Start Delay:");
            ImGui::SetNextItemWidth(600);
            int startDelayValue = g_config.keyRepeatStartDelay;
            if (ImGui::SliderInt("##keyRepeatStartDelay", &startDelayValue, 0, 500, startDelayValue == 0 ? "Default" : "%d ms")) {
                g_config.keyRepeatStartDelay = startDelayValue;
                g_configIsDirty = true;
                ApplyKeyRepeatSettings();
            }
            ImGui::SameLine();
            HelpMarker("Delay before a held key starts repeating.\n"
                       "0 = Use Windows default, 1-500ms = custom delay.\n"
                       "Only applied while the game window is focused.");

            ImGui::Text("Key Repeat Delay:");
            ImGui::SetNextItemWidth(600);
            int repeatDelayValue = g_config.keyRepeatDelay;
            if (ImGui::SliderInt("##keyRepeatDelay", &repeatDelayValue, 0, 500, repeatDelayValue == 0 ? "Default" : "%d ms")) {
                g_config.keyRepeatDelay = repeatDelayValue;
                g_configIsDirty = true;
                ApplyKeyRepeatSettings();
            }
            ImGui::SameLine();
            HelpMarker("Time between repeated key presses while held.\n"
                       "0 = Use Windows default, 1-500ms = custom delay.\n"
                       "Only applied while the game window is focused.");

            ImGui::Spacing();

            // --- Key Rebinding Section ---
            ImGui::SeparatorText("Key Rebinding");
            ImGui::TextWrapped("Intercept keyboard inputs and remap them before they reach the game.");
            ImGui::Spacing();

            // Master toggle
            if (ImGui::Checkbox("Enable Key Rebinding", &g_config.keyRebinds.enabled)) {
                g_configIsDirty = true;
                std::lock_guard<std::mutex> hotkeyLock(g_hotkeyMainKeysMutex);
                RebuildHotkeyMainKeys_Internal();
            }
            ImGui::SameLine();
            HelpMarker("When enabled, configured key rebinds will intercept keyboard input and send the remapped key to the game instead.");

            if (g_config.keyRebinds.enabled) {
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // Rebind list
                static int s_rebindFromKeyToBind = -1;    // Index of rebind being bound (from key)
                static int s_rebindOutputVKToBind = -1;   // Index of rebind being bound (output VK code)
                static int s_rebindOutputScanToBind = -1; // Index of rebind being bound (output scan code)

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

                // Rebind binding popup (for from key)
                bool is_rebind_from_binding = (s_rebindFromKeyToBind != -1);
                if (is_rebind_from_binding) { MarkRebindBindingActive(); }
                if (is_rebind_from_binding) { ImGui::OpenPopup("Bind From Key"); }

                if (ImGui::BeginPopupModal("Bind From Key", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
                    ImGui::Text("Press a key to bind as INPUT.");
                    ImGui::Text("Press ESC to cancel.");
                    ImGui::Separator();

                    static uint64_t s_lastBindingInputSeqInputs1 = 0;
                    if (ImGui::IsWindowAppearing()) { s_lastBindingInputSeqInputs1 = GetLatestBindingInputSequence(); }

                    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
                        s_rebindFromKeyToBind = -1;
                        ImGui::CloseCurrentPopup();
                    }

                    DWORD capturedVk = 0;
                    LPARAM capturedLParam = 0;
                    bool capturedIsMouse = false;
                    if (ConsumeBindingInputEventSince(s_lastBindingInputSeqInputs1, capturedVk, capturedLParam, capturedIsMouse)) {
                        // Allow binding modifier keys (L/R Ctrl/Shift/Alt) for key rebinding.
                        // Only disallow Escape (cancel) and Windows keys.
                        if (capturedVk != VK_ESCAPE && capturedVk != VK_LWIN && capturedVk != VK_RWIN) {
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

                // Output VK binding popup
                bool is_vk_binding = (s_rebindOutputVKToBind != -1);
                if (is_vk_binding) { MarkRebindBindingActive(); }
                if (is_vk_binding) { ImGui::OpenPopup("Bind Output VK"); }

                if (ImGui::BeginPopupModal("Bind Output VK", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
                    ImGui::Text("Press a key to set OUTPUT Virtual Key Code.");
                    ImGui::Text("Press ESC to cancel.");
                    ImGui::Separator();

                    static uint64_t s_lastBindingInputSeqInputs2 = 0;
                    if (ImGui::IsWindowAppearing()) { s_lastBindingInputSeqInputs2 = GetLatestBindingInputSequence(); }

                    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
                        s_rebindOutputVKToBind = -1;
                        ImGui::CloseCurrentPopup();
                    }

                    DWORD capturedVk = 0;
                    LPARAM capturedLParam = 0;
                    bool capturedIsMouse = false;
                    if (ConsumeBindingInputEventSince(s_lastBindingInputSeqInputs2, capturedVk, capturedLParam, capturedIsMouse)) {
                        // Allow modifier keys here as well (useful when the desired output is a modifier).
                        if (capturedVk != VK_ESCAPE && capturedVk != VK_LWIN && capturedVk != VK_RWIN) {
                            if (s_rebindOutputVKToBind >= 0 && s_rebindOutputVKToBind < (int)g_config.keyRebinds.rebinds.size()) {
                                auto& rebind = g_config.keyRebinds.rebinds[s_rebindOutputVKToBind];
                                rebind.toKey = capturedVk;
                                if (rebind.useCustomOutput) { rebind.customOutputVK = capturedVk; }
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

                // Output Scan Code binding popup
                bool is_scan_binding = (s_rebindOutputScanToBind != -1);
                if (is_scan_binding) { MarkRebindBindingActive(); }
                if (is_scan_binding) { ImGui::OpenPopup("Bind Output Scan"); }

                if (ImGui::BeginPopupModal("Bind Output Scan", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
                    ImGui::Text("Press a key to set OUTPUT Scan Code.");
                    ImGui::Text("Press ESC to cancel.");
                    ImGui::Separator();

                    static uint64_t s_lastBindingInputSeqInputs3 = 0;
                    if (ImGui::IsWindowAppearing()) { s_lastBindingInputSeqInputs3 = GetLatestBindingInputSequence(); }

                    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
                        s_rebindOutputScanToBind = -1;
                        ImGui::CloseCurrentPopup();
                    }

                    DWORD capturedVk = 0;
                    LPARAM capturedLParam = 0;
                    bool capturedIsMouse = false;
                    if (ConsumeBindingInputEventSince(s_lastBindingInputSeqInputs3, capturedVk, capturedLParam, capturedIsMouse)) {
                        // Allow modifier keys when capturing scan codes too.
                        if (capturedVk != VK_ESCAPE && capturedVk != VK_LWIN && capturedVk != VK_RWIN) {
                            if (s_rebindOutputScanToBind >= 0 && s_rebindOutputScanToBind < (int)g_config.keyRebinds.rebinds.size()) {
                                auto& rebind = g_config.keyRebinds.rebinds[s_rebindOutputScanToBind];

                                if (capturedVk == VK_LBUTTON || capturedVk == VK_RBUTTON || capturedVk == VK_MBUTTON ||
                                    capturedVk == VK_XBUTTON1 || capturedVk == VK_XBUTTON2) {
                                    if (!rebind.useCustomOutput) { rebind.customOutputVK = rebind.toKey; }
                                    rebind.customOutputScanCode = 0;
                                    rebind.useCustomOutput = true;
                                } else {
                                    UINT scanCode = static_cast<UINT>((capturedLParam >> 16) & 0xFF);
                                    if ((capturedLParam & (1LL << 24)) != 0) { scanCode |= 0xE000; }
                                    if ((capturedLParam & (1LL << 24)) == 0 && scanCode == 0) {
                                        scanCode = getScanCodeWithExtendedFlag(capturedVk);
                                    }

                                    if ((scanCode & 0xFF00) == 0) { scanCode = getScanCodeWithExtendedFlag(capturedVk); }

                                    rebind.customOutputScanCode = scanCode;
                                    if (!rebind.useCustomOutput) { rebind.customOutputVK = rebind.toKey; }
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

                int rebind_to_remove = -1;
                for (size_t i = 0; i < g_config.keyRebinds.rebinds.size(); ++i) {
                    auto& rebind = g_config.keyRebinds.rebinds[i];
                    ImGui::PushID((int)i);

                    // Delete button
                    if (ImGui::Button("X", ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()))) { rebind_to_remove = (int)i; }
                    ImGui::SameLine();

                    // Enable checkbox
                    if (ImGui::Checkbox("##enabled", &rebind.enabled)) {
                        g_configIsDirty = true;
                        std::lock_guard<std::mutex> hotkeyLock(g_hotkeyMainKeysMutex);
                        RebuildHotkeyMainKeys_Internal();
                    }
                    ImGui::SameLine();

                    // --- INPUT KEY ---
                    ImGui::Text("Input:");
                    ImGui::SameLine();
                    std::string fromKeyStr = VkToString(rebind.fromKey);
                    std::string fromLabel = (s_rebindFromKeyToBind == (int)i) ? "[Press key...]##from" : (fromKeyStr + "##from");
                    if (ImGui::Button(fromLabel.c_str(), ImVec2(100, 0))) {
                        s_rebindFromKeyToBind = (int)i;
                        s_rebindOutputVKToBind = -1;
                        s_rebindOutputScanToBind = -1;
                    }
                    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Click to bind the key to intercept"); }

                    ImGui::SameLine();
                    ImGui::Text("->");
                    ImGui::SameLine();

                    // --- OUTPUT VK CODE ---
                    ImGui::Text("Text:");
                    ImGui::SameLine();
                    DWORD displayVK = rebind.useCustomOutput ? rebind.customOutputVK : rebind.toKey;
                    std::string vkKeyStr = VkToString(displayVK);
                    std::string vkLabel =
                        (s_rebindOutputVKToBind == (int)i) ? "[Press key...]##vk" : (vkKeyStr + " (" + std::to_string(displayVK) + ")##vk");
                    if (ImGui::Button(vkLabel.c_str(), ImVec2(120, 0))) {
                        s_rebindOutputVKToBind = (int)i;
                        s_rebindFromKeyToBind = -1;
                        s_rebindOutputScanToBind = -1;
                    }
                    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Click to bind which character is typed in chat/recipe book"); }

                    ImGui::SameLine();

                    // --- OUTPUT SCAN CODE ---
                    ImGui::Text("Game Keybind:");
                    ImGui::SameLine();
                    // Get the scan code to display - use custom if set, otherwise derive from toKey
                    DWORD displayScan = rebind.useCustomOutput ? rebind.customOutputScanCode : getScanCodeWithExtendedFlag(rebind.toKey);
                    if (rebind.useCustomOutput && displayScan != 0 && (displayScan & 0xFF00) == 0) {
                        DWORD derived = getScanCodeWithExtendedFlag(rebind.customOutputVK != 0 ? rebind.customOutputVK : rebind.toKey);
                        if ((derived & 0xFF00) != 0 && ((derived & 0xFF) == (displayScan & 0xFF))) { displayScan = derived; }
                    }

                    std::string scanKeyStr;
                    if (displayScan != 0) {
                        DWORD scanDisplayVK = MapVirtualKey(displayScan, MAPVK_VSC_TO_VK_EX);
                        if (scanDisplayVK != 0) {
                            scanKeyStr = VkToString(scanDisplayVK);
                        } else {
                            LONG keyNameLParam = static_cast<LONG>((displayScan & 0xFF) << 16);
                            if ((displayScan & 0xFF00) != 0) { keyNameLParam |= (1 << 24); } // extended key bit

                            char keyName[64] = {};
                            if (GetKeyNameTextA(keyNameLParam, keyName, sizeof(keyName)) > 0) { scanKeyStr = keyName; }
                        }
                    }

                    if (scanKeyStr.empty()) { scanKeyStr = "[Unbound]"; }

                    std::string scanLabel = (s_rebindOutputScanToBind == (int)i) ? "[Press key...]##scan" : (scanKeyStr + "##scan");
                    if (ImGui::Button(scanLabel.c_str(), ImVec2(100, 0))) {
                        s_rebindOutputScanToBind = (int)i;
                        s_rebindFromKeyToBind = -1;
                        s_rebindOutputVKToBind = -1;
                    }
                    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Click to bind which game keybind is triggered"); }

                    ImGui::PopID();
                }

                // Remove rebind if marked
                if (rebind_to_remove >= 0 && rebind_to_remove < (int)g_config.keyRebinds.rebinds.size()) {
                    g_config.keyRebinds.rebinds.erase(g_config.keyRebinds.rebinds.begin() + rebind_to_remove);
                    g_configIsDirty = true;
                    std::lock_guard<std::mutex> hotkeyLock(g_hotkeyMainKeysMutex);
                    RebuildHotkeyMainKeys_Internal();
                }

                ImGui::Spacing();
                if (ImGui::Button("Add Rebind")) {
                    g_config.keyRebinds.rebinds.push_back(KeyRebind{});
                    g_configIsDirty = true;
                    std::lock_guard<std::mutex> hotkeyLock(g_hotkeyMainKeysMutex);
                    RebuildHotkeyMainKeys_Internal();
                }
            }

            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::EndTabItem();
}
