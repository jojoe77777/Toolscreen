            if (ImGui::BeginTabItem("Mouse")) {
                g_currentlyEditingMirror = "";
                g_imageDragMode.store(false);
                g_windowOverlayDragMode.store(false);

                ImGui::SeparatorText("Mouse Settings");

                ImGui::Text("Mouse Sensitivity:");
                ImGui::SetNextItemWidth(600);
                if (ImGui::SliderFloat("##mouseSensitivity", &g_config.mouseSensitivity, 0.001f, 10.0f, "%.3fx")) {
                    g_configIsDirty = true;
                }
                ImGui::SameLine();
                HelpMarker("Multiplies mouse movement for raw input events (mouselook).\n"
                           "1.0 = normal sensitivity, higher = faster, lower = slower.\n"
                           "Useful for adjusting mouse speed when using stretched resolutions.");

                ImGui::Text("Windows Mouse Speed:");
                ImGui::SetNextItemWidth(600);
                int windowsSpeedValue = g_config.windowsMouseSpeed;
                if (ImGui::SliderInt("##windowsMouseSpeed", &windowsSpeedValue, 0, 20, 
                                     windowsSpeedValue == 0 ? "Disabled" : "%d")) {
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

                                const CursorTextures::CursorData* cursorData =
                                    CursorTextures::LoadOrFindCursor(cursorPath, loadType, newSize);
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
