            if (ImGui::BeginTabItem(trc("tabs.mouse"))) {
                g_currentlyEditingMirror = "";

                ImGui::SeparatorText(trc("inputs.mouse_settings"));

                ImGui::Text(trc("label.mouse_sensitivity"));
                ImGui::SetNextItemWidth(600);
                if (ImGui::SliderFloat("##mouseSensitivity", &g_config.mouseSensitivity, 0.001f, 10.0f, "%.3fx")) {
                    g_configIsDirty = true;
                }
                ImGui::SameLine();
                HelpMarker(trc("tooltip.mouse_sensitivity"));

                ImGui::Text(trc("label.windows_mouse_speed"));
                ImGui::SetNextItemWidth(600);
                int windowsSpeedValue = g_config.windowsMouseSpeed;
                if (ImGui::SliderInt("##windowsMouseSpeed", &windowsSpeedValue, 0, 20, 
                                     windowsSpeedValue == 0 ? trc("label.disabled") : "%d")) {
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

                ImGui::Spacing();
                ImGui::SeparatorText(trc("inputs.cursor_configuration"));

                if (ImGui::Checkbox(trc("inputs.enable_custom_cursors"), &g_config.cursors.enabled)) {
                    g_configIsDirty = true;
                    // Schedule cursor reload (will happen outside GUI rendering to avoid deadlock)
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
                                displayName[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(displayName[0])));
                                for (auto& c : displayName) {
                                    if (c == '_' || c == '-') c = ' ';
                                }
                            }

                            std::string description;
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
                        lastCursorListRefreshTime = now;
                    }

                    struct CursorConfigUI {
                        const char* name;
                        CursorConfig* config;
                    };

                    CursorConfigUI cursors[] = { { trc("game_state.title"), &g_config.cursors.title },
                                                 { trc("game_state.wall"), &g_config.cursors.wall },
                                                 { trc("inputs.in_world"), &g_config.cursors.ingame } };

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
                                    // Schedule cursor reload (will happen outside GUI rendering to avoid deadlock)
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
                        if (ImGui::Button(trc("button.cancel"), ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
                        ImGui::EndPopup();
                    }
                }

                ImGui::EndTabItem();
            }


