if (ImGui::BeginTabItem("Modes")) {
    g_currentlyEditingMirror = "";
    int mode_to_remove = -1;

    g_imageDragMode.store(false);
    g_windowOverlayDragMode.store(false);

    // Check if resolution changing is supported
    bool resolutionSupported = IsResolutionChangeSupported(g_gameVersion);
    if (!resolutionSupported) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.7f, 0.0f, 1.0f)); // Orange/yellow warning color
        ImGui::TextWrapped("WARNING: Resolution changing is not supported for Minecraft version %d.%d.%d (requires 1.13+). "
                           "Mode dimension editing and switching are disabled.",
                           g_gameVersion.valid ? g_gameVersion.major : 0, g_gameVersion.valid ? g_gameVersion.minor : 0,
                           g_gameVersion.valid ? g_gameVersion.patch : 0);
        ImGui::TextWrapped("Other features (overlays, images, cursors) remain functional.");
        ImGui::PopStyleColor();
        ImGui::Separator();
    }

    // Check if raw input is disabled (50+ WM_MOUSEMOVE events without raw input)
    if (g_wmMouseMoveCount.load() > 50) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.5f, 1.0f)); // Red warning color
        ImGui::TextWrapped("WARNING: You have Raw Input disabled. Please enable it in Options -> Controls -> Mouse Settings.");
        ImGui::PopStyleColor();
        ImGui::Separator();
    }

    // --- DEFAULT MODES SECTION ---
    ImGui::SeparatorText("Default Modes");

    // --- FULLSCREEN MODE ---
    for (size_t i = 0; i < g_config.modes.size(); ++i) {
        auto& mode = g_config.modes[i];
        if (EqualsIgnoreCase(mode.id, "Fullscreen")) {
            ImGui::PushID((int)i);

            bool node_open = ImGui::TreeNodeEx("##mode_node", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", mode.id.c_str());
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::GetFrameHeight());

            // Don't show delete button for predefined modes
            ImGui::Dummy(ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));

            if (node_open) {

                if (!resolutionSupported) { ImGui::BeginDisabled(); }

                ImGui::Columns(2, "dims", false);
                ImGui::Text("Width");
                ImGui::NextColumn();
                // Use temporary variable for spinner, then defer the change to logic thread
                int tempWidth = mode.width;
                if (Spinner("##Width", &tempWidth, 1, 1)) {
                    // Queue the dimension change for logic thread to apply
                    std::lock_guard<std::mutex> lock(g_pendingDimensionChangeMutex);
                    g_pendingDimensionChange.pending = true;
                    g_pendingDimensionChange.modeId = mode.id;
                    g_pendingDimensionChange.newWidth = tempWidth;
                    g_pendingDimensionChange.newHeight = 0; // Unchanged
                    g_pendingDimensionChange.sendWmSize = (g_currentModeId == mode.id);
                }
                ImGui::NextColumn();
                ImGui::Text("Height");
                ImGui::NextColumn();
                int tempHeight = mode.height;
                if (Spinner("##Height", &tempHeight, 1, 1)) {
                    // Queue the dimension change for logic thread to apply
                    std::lock_guard<std::mutex> lock(g_pendingDimensionChangeMutex);
                    g_pendingDimensionChange.pending = true;
                    g_pendingDimensionChange.modeId = mode.id;
                    g_pendingDimensionChange.newWidth = 0; // Unchanged
                    g_pendingDimensionChange.newHeight = tempHeight;
                    g_pendingDimensionChange.sendWmSize = (g_currentModeId == mode.id);
                }
                ImGui::Columns(1);

                if (ImGui::Button("Switch to this Mode")) {
                    // Defer mode switch to avoid deadlock (g_configMutex is held during GUI rendering)
                    std::lock_guard<std::mutex> pendingLock(g_pendingModeSwitchMutex);
                    g_pendingModeSwitch.pending = true;
                    g_pendingModeSwitch.modeId = mode.id;
                    g_pendingModeSwitch.source = "GUI mode list";
                    Log("[GUI] Deferred mode switch to: " + mode.id);
                }
                // Force stretch mode for fullscreen
                mode.stretch.enabled = true;
                mode.stretch.x = 0;
                mode.stretch.y = 0;
                mode.stretch.width = GetCachedScreenWidth();
                mode.stretch.height = GetCachedScreenHeight();

                // --- TRANSITION SETTINGS ---
                ImGui::Separator();
                if (ImGui::TreeNode("Transition Settings")) {
                    RenderTransitionSettingsHorizontalNoBackground(mode, "Fullscreen");
                    ImGui::TreePop();
                }

                // --- BORDER SETTINGS ---
                if (ImGui::TreeNode("Border Settings")) {
                    if (ImGui::Checkbox("Enable Border", &mode.border.enabled)) { g_configIsDirty = true; }
                    ImGui::SameLine();
                    HelpMarker("Draw a border around the game viewport. Border appears outside the game area.");

                    if (mode.border.enabled) {
                        ImGui::Text("Color:");
                        ImVec4 borderCol = ImVec4(mode.border.color.r, mode.border.color.g, mode.border.color.b, 1.0f);
                        if (ImGui::ColorEdit3("##BorderColor", (float*)&borderCol, ImGuiColorEditFlags_NoInputs)) {
                            mode.border.color = { borderCol.x, borderCol.y, borderCol.z };
                            g_configIsDirty = true;
                        }

                        ImGui::Text("Width:");
                        ImGui::SetNextItemWidth(100);
                        if (Spinner("##BorderWidth", &mode.border.width, 1, 1, 50)) { g_configIsDirty = true; }
                        ImGui::SameLine();
                        ImGui::TextDisabled("px");

                        ImGui::Text("Corner Radius:");
                        ImGui::SetNextItemWidth(100);
                        if (Spinner("##BorderRadius", &mode.border.radius, 1, 0, 100)) { g_configIsDirty = true; }
                        ImGui::SameLine();
                        ImGui::TextDisabled("px");
                    }
                    ImGui::TreePop();
                }
                ImGui::Separator();

                if (ImGui::TreeNode("Mirrors")) {
                    int mirror_idx_to_remove = -1;
                    for (size_t k = 0; k < mode.mirrorIds.size(); ++k) {
                        ImGui::PushID(static_cast<int>(k));
                        std::string del_mirror_label = "X##del_mirror_from_mode_" + std::to_string(k);
                        if (ImGui::Button(del_mirror_label.c_str())) { mirror_idx_to_remove = (int)k; }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(mode.mirrorIds[k].c_str());
                        ImGui::PopID();
                    }
                    if (mirror_idx_to_remove != -1) {
                        mode.mirrorIds.erase(mode.mirrorIds.begin() + mirror_idx_to_remove);
                        g_configIsDirty = true;
                    }
                    if (ImGui::BeginCombo("Add Mirror##add_mirror_to_mode", "[Select Mirror]")) {
                        for (const auto& mirrorConf : g_config.mirrors) {
                            if (std::find(mode.mirrorIds.begin(), mode.mirrorIds.end(), mirrorConf.name) == mode.mirrorIds.end()) {
                                if (ImGui::Selectable(mirrorConf.name.c_str())) {
                                    mode.mirrorIds.push_back(mirrorConf.name);
                                    g_configIsDirty = true;
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode("Mirror Groups")) {
                    int group_idx_to_remove = -1;
                    for (size_t k = 0; k < mode.mirrorGroupIds.size(); ++k) {
                        ImGui::PushID(static_cast<int>(k));
                        std::string del_group_label = "X##del_mirror_group_from_mode_" + std::to_string(k);
                        if (ImGui::Button(del_group_label.c_str())) { group_idx_to_remove = (int)k; }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(mode.mirrorGroupIds[k].c_str());
                        ImGui::PopID();
                    }
                    if (group_idx_to_remove != -1) {
                        mode.mirrorGroupIds.erase(mode.mirrorGroupIds.begin() + group_idx_to_remove);
                        g_configIsDirty = true;
                    }
                    if (ImGui::BeginCombo("Add Mirror Group##add_mirror_group_to_mode", "[Select Group]")) {
                        for (const auto& groupConf : g_config.mirrorGroups) {
                            if (std::find(mode.mirrorGroupIds.begin(), mode.mirrorGroupIds.end(), groupConf.name) ==
                                mode.mirrorGroupIds.end()) {
                                if (ImGui::Selectable(groupConf.name.c_str())) {
                                    mode.mirrorGroupIds.push_back(groupConf.name);
                                    g_configIsDirty = true;
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode("Images")) {
                    int image_idx_to_remove = -1;
                    for (size_t k = 0; k < mode.imageIds.size(); ++k) {
                        ImGui::PushID(static_cast<int>(k));
                        std::string del_img_label = "X##del_img_from_mode_" + std::to_string(k);
                        if (ImGui::Button(del_img_label.c_str())) { image_idx_to_remove = (int)k; }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(mode.imageIds[k].c_str());
                        ImGui::PopID();
                    }
                    if (image_idx_to_remove != -1) {
                        mode.imageIds.erase(mode.imageIds.begin() + image_idx_to_remove);
                        g_configIsDirty = true;
                    }
                    if (ImGui::BeginCombo("Add Image##add_image_to_mode", "[Select Image]")) {
                        for (const auto& imgConf : g_config.images) {
                            if (std::find(mode.imageIds.begin(), mode.imageIds.end(), imgConf.name) == mode.imageIds.end()) {
                                if (ImGui::Selectable(imgConf.name.c_str())) {
                                    mode.imageIds.push_back(imgConf.name);
                                    g_configIsDirty = true;
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode("Window Overlays")) {
                    int windowOverlay_idx_to_remove = -1;
                    for (size_t k = 0; k < mode.windowOverlayIds.size(); ++k) {
                        ImGui::PushID(static_cast<int>(k));
                        std::string del_overlay_label = "X##del_overlay_from_mode_" + std::to_string(k);
                        if (ImGui::Button(del_overlay_label.c_str())) { windowOverlay_idx_to_remove = (int)k; }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(mode.windowOverlayIds[k].c_str());
                        ImGui::PopID();
                    }
                    if (windowOverlay_idx_to_remove != -1) {
                        mode.windowOverlayIds.erase(mode.windowOverlayIds.begin() + windowOverlay_idx_to_remove);
                        g_configIsDirty = true;
                    }
                    if (ImGui::BeginCombo("Add Window Overlay##add_overlay_to_mode", "[Select Window Overlay]")) {
                        for (const auto& overlayConf : g_config.windowOverlays) {
                            if (std::find(mode.windowOverlayIds.begin(), mode.windowOverlayIds.end(), overlayConf.name) ==
                                mode.windowOverlayIds.end()) {
                                if (ImGui::Selectable(overlayConf.name.c_str())) {
                                    mode.windowOverlayIds.push_back(overlayConf.name);
                                    g_configIsDirty = true;
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TreePop();
                }


                // --- SENSITIVITY OVERRIDE ---
                if (ImGui::TreeNode("Sensitivity Override##Fullscreen")) {
                    if (ImGui::Checkbox("Override Sensitivity", &mode.sensitivityOverrideEnabled)) { g_configIsDirty = true; }
                    HelpMarker("When enabled, this mode uses its own mouse sensitivity instead of the global setting.");

                    if (mode.sensitivityOverrideEnabled) {
                        if (ImGui::Checkbox("Separate X/Y", &mode.separateXYSensitivity)) {
                            g_configIsDirty = true;
                            // Initialize X/Y values from combined sensitivity if just enabled
                            if (mode.separateXYSensitivity) {
                                mode.modeSensitivityX = mode.modeSensitivity;
                                mode.modeSensitivityY = mode.modeSensitivity;
                            }
                        }
                        ImGui::SameLine();
                        HelpMarker("Use different sensitivity values for horizontal (X) and vertical (Y) mouse movement.");

                        if (mode.separateXYSensitivity) {
                            ImGui::Text("X Sensitivity:");
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##FullscreenSensitivityX", &mode.modeSensitivityX, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                            ImGui::Text("Y Sensitivity:");
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##FullscreenSensitivityY", &mode.modeSensitivityY, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                        } else {
                            ImGui::Text("Sensitivity:");
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##FullscreenSensitivity", &mode.modeSensitivity, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                            ImGui::SameLine();
                            HelpMarker("Mouse sensitivity for this mode (1.0 = normal)");
                        }
                    }
                    ImGui::TreePop();
                }

                if (!resolutionSupported) { ImGui::EndDisabled(); }

                ImGui::TreePop();
            }
            ImGui::PopID();
            break;
        }
    }

    // --- EYEZOOM MODE SECTION ---
    for (size_t i = 0; i < g_config.modes.size(); ++i) {
        auto& mode = g_config.modes[i];
        if (EqualsIgnoreCase(mode.id, "EyeZoom")) {
            ImGui::PushID((int)i + 10000); // Use unique ID offset

            bool node_open = ImGui::TreeNodeEx("##mode_node", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", mode.id.c_str());
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::GetFrameHeight());

            // Don't show delete button for predefined modes
            ImGui::Dummy(ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));

            if (node_open) {
                if (!resolutionSupported) { ImGui::BeginDisabled(); }

                ImGui::Columns(2, "mode_config_cols", false);
                ImGui::SetColumnWidth(0, 150);

                ImGui::Text("Game Width");
                ImGui::NextColumn();
                int tempWidth2 = mode.width;
                if (Spinner("##ModeWidth", &tempWidth2, 1, 1, screenWidth)) {
                    std::lock_guard<std::mutex> lock(g_pendingDimensionChangeMutex);
                    g_pendingDimensionChange.pending = true;
                    g_pendingDimensionChange.modeId = mode.id;
                    g_pendingDimensionChange.newWidth = tempWidth2;
                    g_pendingDimensionChange.newHeight = 0; // Unchanged
                    g_pendingDimensionChange.sendWmSize = (g_currentModeId == mode.id);
                }
                ImGui::NextColumn();
                ImGui::Text("Game Height");
                ImGui::NextColumn();
                int tempHeight2 = mode.height;
                if (Spinner("##ModeHeight", &tempHeight2, 1, 1, 16384)) {
                    std::lock_guard<std::mutex> lock(g_pendingDimensionChangeMutex);
                    g_pendingDimensionChange.pending = true;
                    g_pendingDimensionChange.modeId = mode.id;
                    g_pendingDimensionChange.newWidth = 0; // Unchanged
                    g_pendingDimensionChange.newHeight = tempHeight2;
                    g_pendingDimensionChange.sendWmSize = (g_currentModeId == mode.id);
                }
                ImGui::Columns(1);

                if (ImGui::Button("Switch to this Mode")) {
                    // Defer mode switch to avoid deadlock (g_configMutex is held during GUI rendering)
                    std::lock_guard<std::mutex> pendingLock(g_pendingModeSwitchMutex);
                    g_pendingModeSwitch.pending = true;
                    g_pendingModeSwitch.modeId = mode.id;
                    g_pendingModeSwitch.source = "GUI EyeZoom mode";
                    Log("[GUI] Deferred mode switch to: " + mode.id);
                }

                if (!resolutionSupported) { ImGui::EndDisabled(); }

                if (g_currentModeId == mode.id) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(Current)");
                }

                // --- EYEZOOM SETTINGS (shown directly at top, not in collapsible TreeNode) ---
                ImGui::Separator();
                ImGui::Text("EyeZoom Settings");

                ImGui::Text("Clone Settings (Source)");
                ImGui::Columns(2, "eyezoom_clone_cols", false);
                ImGui::SetColumnWidth(0, 150);
                ImGui::Text("Clone Width");
                ImGui::NextColumn();
                // Clone Width must be even - step by 2, enforce even values
                // Max value is the mode's game width
                int maxCloneWidth = mode.width;
                if (Spinner("##EyeZoomCloneWidth", &g_config.eyezoom.cloneWidth, 2, 2, maxCloneWidth)) {
                    // Ensure value stays even
                    if (g_config.eyezoom.cloneWidth % 2 != 0) { g_config.eyezoom.cloneWidth = (g_config.eyezoom.cloneWidth / 2) * 2; }
                    // Clamp overlay width to the new clone width
                    int maxOverlay = g_config.eyezoom.cloneWidth / 2;
                    if (g_config.eyezoom.overlayWidth > maxOverlay) g_config.eyezoom.overlayWidth = maxOverlay;
                    g_configIsDirty = true;
                }
                ImGui::NextColumn();
                ImGui::Text("Clone Height");
                ImGui::NextColumn();
                // Max value is the mode's game height
                int maxCloneHeight = mode.height;
                if (Spinner("##EyeZoomCloneHeight", &g_config.eyezoom.cloneHeight, 10, 1, maxCloneHeight)) g_configIsDirty = true;
                ImGui::NextColumn();
                ImGui::Text("Overlay Width (per side)");
                ImGui::NextColumn();
                {
                    int maxOverlay = g_config.eyezoom.cloneWidth / 2;
                    if (Spinner("##EyeZoomOverlayWidth", &g_config.eyezoom.overlayWidth, 1, 0, maxOverlay)) g_configIsDirty = true;
                }
                ImGui::SameLine();
                HelpMarker("How many colored overlay boxes + numbers to draw on EACH side of the center line.\n"
                           "cloneWidth controls how wide the clone samples; overlayWidth only controls how much of the numbered overlay is drawn.");
                ImGui::Columns(1);

                ImGui::Separator();
                ImGui::Text("Margin Settings (Output)");
                ImGui::Columns(2, "eyezoom_margin_cols", false);
                ImGui::SetColumnWidth(0, 150);
                ImGui::Text("Horizontal Margin");
                ImGui::NextColumn();
                // Calculate max margin based on EyeZoom mode's TARGET viewport position (not current animated position)
                // This prevents the margin from being clamped during mode transitions
                // EyeZoom mode centers a narrow viewport: finalX = (screenWidth - modeWidth) / 2
                int eyezoomModeWidth = mode.width; // Use the EyeZoom mode's configured width
                int eyezoomTargetFinalX = (screenWidth - eyezoomModeWidth) / 2;
                // Max margin calculation: eyezoomTargetFinalX - (2 * margin) >= 0.2 * stretchWidth
                // margin <= (eyezoomTargetFinalX - 0.2 * stretchWidth) / 2
                int maxHMargin = (int)((eyezoomTargetFinalX - 0.2f * g_config.eyezoom.stretchWidth) / 2.0f);
                if (maxHMargin < 0) maxHMargin = 0;
                if (Spinner("##EyeZoomHorizontalMargin", &g_config.eyezoom.horizontalMargin, 10, 0, maxHMargin)) g_configIsDirty = true;
                ImGui::NextColumn();
                ImGui::Text("Vertical Margin");
                ImGui::NextColumn();
                // Calculate max vertical margin: ensure zoom output height stays at least 20% of screen height
                int monitorHeight = GetCachedScreenHeight();
                int maxVMargin = (int)((monitorHeight - 0.2f * monitorHeight) / 2.0f);
                if (maxVMargin < 0) maxVMargin = 0;
                if (Spinner("##EyeZoomVerticalMargin", &g_config.eyezoom.verticalMargin, 10, 0, maxVMargin)) g_configIsDirty = true;
                ImGui::Columns(1);

                ImGui::Separator();
                ImGui::Text("Color Settings");
                // Grid Color 1 with opacity
                {
                    ImVec4 col1 = ImVec4(g_config.eyezoom.gridColor1.r, g_config.eyezoom.gridColor1.g, g_config.eyezoom.gridColor1.b,
                                         g_config.eyezoom.gridColor1Opacity);
                    if (ImGui::ColorEdit4("Grid Color 1", (float*)&col1, ImGuiColorEditFlags_AlphaBar)) {
                        g_config.eyezoom.gridColor1 = { col1.x, col1.y, col1.z };
                        g_config.eyezoom.gridColor1Opacity = col1.w;
                        g_configIsDirty = true;
                    }
                }
                // Grid Color 2 with opacity
                {
                    ImVec4 col2 = ImVec4(g_config.eyezoom.gridColor2.r, g_config.eyezoom.gridColor2.g, g_config.eyezoom.gridColor2.b,
                                         g_config.eyezoom.gridColor2Opacity);
                    if (ImGui::ColorEdit4("Grid Color 2", (float*)&col2, ImGuiColorEditFlags_AlphaBar)) {
                        g_config.eyezoom.gridColor2 = { col2.x, col2.y, col2.z };
                        g_config.eyezoom.gridColor2Opacity = col2.w;
                        g_configIsDirty = true;
                    }
                }
                // Center Line Color with opacity
                {
                    ImVec4 col3 = ImVec4(g_config.eyezoom.centerLineColor.r, g_config.eyezoom.centerLineColor.g,
                                         g_config.eyezoom.centerLineColor.b, g_config.eyezoom.centerLineColorOpacity);
                    if (ImGui::ColorEdit4("Center Line Color", (float*)&col3, ImGuiColorEditFlags_AlphaBar)) {
                        g_config.eyezoom.centerLineColor = { col3.x, col3.y, col3.z };
                        g_config.eyezoom.centerLineColorOpacity = col3.w;
                        g_configIsDirty = true;
                    }
                }
                // Text Color with opacity
                {
                    ImVec4 col4 = ImVec4(g_config.eyezoom.textColor.r, g_config.eyezoom.textColor.g, g_config.eyezoom.textColor.b,
                                         g_config.eyezoom.textColorOpacity);
                    if (ImGui::ColorEdit4("Text Color", (float*)&col4, ImGuiColorEditFlags_AlphaBar)) {
                        g_config.eyezoom.textColor = { col4.x, col4.y, col4.z };
                        g_config.eyezoom.textColorOpacity = col4.w;
                        g_configIsDirty = true;
                    }
                }

                ImGui::Separator();
                ImGui::Text("Text Settings");
                ImGui::SetNextItemWidth(250);
                if (ImGui::SliderInt("Text Font Size (px)", &g_config.eyezoom.textFontSize, 8, 80)) {
                    g_configIsDirty = true;
                    // Apply font size immediately
                    SetOverlayTextFontSize(g_config.eyezoom.textFontSize);
                }

                ImGui::Text("Text Font:");
                ImGui::SetNextItemWidth(300);
                if (ImGui::InputText("##EyeZoomTextFont", &g_config.eyezoom.textFontPath)) {
                    g_configIsDirty = true;
                    g_eyeZoomFontNeedsReload.store(true);
                }
                ImGui::SameLine();
                if (ImGui::Button("Browse...##EyeZoomFont")) {
                    OPENFILENAMEA ofn = {};
                    char szFile[MAX_PATH] = {};

                    // Pre-fill with current path if it exists
                    if (!g_config.eyezoom.textFontPath.empty()) { strncpy_s(szFile, g_config.eyezoom.textFontPath.c_str(), MAX_PATH - 1); }

                    ofn.lStructSize = sizeof(ofn);
                    ofn.hwndOwner = g_minecraftHwnd.load();
                    ofn.lpstrFile = szFile;
                    ofn.nMaxFile = sizeof(szFile);
                    ofn.lpstrFilter = "Font Files (*.ttf;*.otf)\0*.ttf;*.otf\0All Files (*.*)\0*.*\0";
                    ofn.nFilterIndex = 1;
                    ofn.lpstrTitle = "Select Font for EyeZoom Text";
                    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
                    ofn.lpstrInitialDir = "C:\\Windows\\Fonts";

                    if (GetOpenFileNameA(&ofn)) {
                        g_config.eyezoom.textFontPath = szFile;
                        g_configIsDirty = true;
                        g_eyeZoomFontNeedsReload.store(true);
                    }
                }
                ImGui::SameLine();
                HelpMarker("Custom font for EyeZoom overlay text. Leave empty to use the global font. Supports TTF and OTF files.");

                if (ImGui::Checkbox("Link Rectangle to Font Size", &g_config.eyezoom.linkRectToFont)) {
                    g_configIsDirty = true;
                    // If linking is enabled, sync the rectangle height to font size
                    if (g_config.eyezoom.linkRectToFont) {
                        g_config.eyezoom.rectHeight = static_cast<int>(g_config.eyezoom.textFontSize * 1.2f);
                    }
                }

                // Only show override slider when linking is disabled
                if (!g_config.eyezoom.linkRectToFont) {
                    ImGui::SetNextItemWidth(250);
                    if (ImGui::SliderInt("Override Rectangle Height (px)", &g_config.eyezoom.rectHeight, 8, 120)) {
                        g_configIsDirty = true;
                    }
                }

                // --- BACKGROUND SECTION ---
                if (ImGui::TreeNode("Background")) {
                    if (ImGui::RadioButton("Color", mode.background.selectedMode == "color")) {
                        if (mode.background.selectedMode != "color") {
                            mode.background.selectedMode = "color";
                            g_configIsDirty = true;
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton("Gradient", mode.background.selectedMode == "gradient")) {
                        if (mode.background.selectedMode != "gradient") {
                            mode.background.selectedMode = "gradient";
                            // Initialize default stops if empty
                            if (mode.background.gradientStops.size() < 2) {
                                mode.background.gradientStops.clear();
                                mode.background.gradientStops.push_back({ { 0.0f, 0.0f, 0.0f }, 0.0f });
                                mode.background.gradientStops.push_back({ { 1.0f, 1.0f, 1.0f }, 1.0f });
                            }
                            g_configIsDirty = true;
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton("Image", mode.background.selectedMode == "image")) {
                        if (mode.background.selectedMode != "image") {
                            mode.background.selectedMode = "image";
                            g_configIsDirty = true;
                            // Load existing background image if path is set
                            if (!mode.background.image.empty()) {
                                g_allImagesLoaded = false;
                                g_pendingImageLoad = true;
                                LoadImageAsync(DecodedImageData::Type::Background, mode.id, mode.background.image, g_toolscreenPath);
                            }
                        }
                    }

                    if (mode.background.selectedMode == "color") {
                        if (ImGui::ColorEdit3("##bgColor", &mode.background.color.r)) { g_configIsDirty = true; }
                    } else if (mode.background.selectedMode == "gradient") {
                        // Angle slider
                        ImGui::SetNextItemWidth(200);
                        if (ImGui::SliderFloat("Angle##bgGradAngle", &mode.background.gradientAngle, 0.0f, 360.0f, "%.0f deg")) {
                            g_configIsDirty = true;
                        }

                        // Color stops
                        ImGui::Text("Color Stops:");
                        int stopToRemove = -1;
                        for (size_t i = 0; i < mode.background.gradientStops.size(); i++) {
                            ImGui::PushID(static_cast<int>(i));
                            auto& stop = mode.background.gradientStops[i];

                            // Color picker
                            if (ImGui::ColorEdit3("##StopColor", &stop.color.r, ImGuiColorEditFlags_NoInputs)) { g_configIsDirty = true; }
                            ImGui::SameLine();

                            // Position slider (0-100%)
                            float pos = stop.position * 100.0f;
                            ImGui::SetNextItemWidth(100);
                            if (ImGui::SliderFloat("##StopPos", &pos, 0.0f, 100.0f, "%.0f%%")) {
                                stop.position = pos / 100.0f;
                                g_configIsDirty = true;
                            }

                            // Remove button (only if more than 2 stops)
                            if (mode.background.gradientStops.size() > 2) {
                                ImGui::SameLine();
                                if (ImGui::Button("X##RemoveStop")) { stopToRemove = static_cast<int>(i); }
                            }

                            ImGui::PopID();
                        }
                        if (stopToRemove >= 0) {
                            mode.background.gradientStops.erase(mode.background.gradientStops.begin() + stopToRemove);
                            g_configIsDirty = true;
                        }

                        // Add stop button (max 8 stops)
                        if (mode.background.gradientStops.size() < 8) {
                            if (ImGui::Button("+ Add Color Stop##bgGrad")) {
                                // Add at midpoint with gray color
                                GradientColorStop newStop;
                                newStop.position = 0.5f;
                                newStop.color = { 0.5f, 0.5f, 0.5f };
                                mode.background.gradientStops.push_back(newStop);
                                // Sort by position
                                std::sort(mode.background.gradientStops.begin(), mode.background.gradientStops.end(),
                                          [](const GradientColorStop& a, const GradientColorStop& b) { return a.position < b.position; });
                                g_configIsDirty = true;
                            }
                        }

                        // Animation controls
                        ImGui::Separator();
                        ImGui::Text("Animation:");
                        const char* animTypeNames[] = { "None", "Rotate", "Slide", "Wave", "Spiral", "Fade" };
                        int currentAnimType = static_cast<int>(mode.background.gradientAnimation);
                        ImGui::SetNextItemWidth(120);
                        if (ImGui::Combo("Type##GradAnim", &currentAnimType, animTypeNames, IM_ARRAYSIZE(animTypeNames))) {
                            mode.background.gradientAnimation = static_cast<GradientAnimationType>(currentAnimType);
                            g_configIsDirty = true;
                        }
                        if (mode.background.gradientAnimation != GradientAnimationType::None) {
                            ImGui::SetNextItemWidth(150);
                            if (ImGui::SliderFloat("Speed##GradAnimSpeed", &mode.background.gradientAnimationSpeed, 0.1f, 5.0f, "%.1fx")) {
                                g_configIsDirty = true;
                            }
                            /*
                            if (ImGui::Checkbox("Color Fade##GradColorFade", &mode.background.gradientColorFade)) {
                                g_configIsDirty = true;
                            }
                            if (ImGui::IsItemHovered()) {
                                ImGui::SetTooltip("Smoothly cycle colors through gradient stops");
                            }*/
                        }
                    } else if (mode.background.selectedMode == "image") {
                        if (ImGui::InputText("Path", &mode.background.image)) {
                            ClearImageError("eyezoom_bg");
                            g_configIsDirty = true;
                            g_allImagesLoaded = false;
                            g_pendingImageLoad = true;
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Browse...##eyezoom_bg")) {
                            // Use the validated image picker
                            ImagePickerResult result =
                                OpenImagePickerAndValidate(g_minecraftHwnd.load(), g_toolscreenPath, g_toolscreenPath);

                            if (result.completed) {
                                if (result.success) {
                                    mode.background.image = result.path;
                                    ClearImageError("eyezoom_bg");
                                    g_allImagesLoaded = false;
                                    g_pendingImageLoad = true;
                                    g_configIsDirty = true;
                                } else if (!result.error.empty()) {
                                    SetImageError("eyezoom_bg", result.error);
                                }
                            }
                        }

                        // Show error message if any
                        std::string bgError = GetImageError("eyezoom_bg");
                        if (!bgError.empty()) { ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", bgError.c_str()); }
                    }
                    ImGui::TreePop();
                }

                // --- BORDER SETTINGS ---
                if (ImGui::TreeNode("Border Settings##EyeZoom")) {
                    if (ImGui::Checkbox("Enable Border##EyeZoom", &mode.border.enabled)) { g_configIsDirty = true; }
                    ImGui::SameLine();
                    HelpMarker("Draw a border around the game viewport. Border appears outside the game area.");

                    if (mode.border.enabled) {
                        ImGui::Text("Color:");
                        ImVec4 borderCol = ImVec4(mode.border.color.r, mode.border.color.g, mode.border.color.b, 1.0f);
                        if (ImGui::ColorEdit3("##BorderColorEyeZoom", (float*)&borderCol, ImGuiColorEditFlags_NoInputs)) {
                            mode.border.color = { borderCol.x, borderCol.y, borderCol.z };
                            g_configIsDirty = true;
                        }

                        ImGui::Text("Width:");
                        ImGui::SetNextItemWidth(100);
                        if (Spinner("##BorderWidthEyeZoom", &mode.border.width, 1, 1, 50)) { g_configIsDirty = true; }
                        ImGui::SameLine();
                        ImGui::TextDisabled("px");

                        ImGui::Text("Corner Radius:");
                        ImGui::SetNextItemWidth(100);
                        if (Spinner("##BorderRadiusEyeZoom", &mode.border.radius, 1, 0, 100)) { g_configIsDirty = true; }
                        ImGui::SameLine();
                        ImGui::TextDisabled("px");
                    }
                    ImGui::TreePop();
                }

                // --- MIRRORS SECTION ---
                if (ImGui::TreeNode("Mirrors")) {
                    int mirror_idx_to_remove = -1;
                    for (size_t k = 0; k < mode.mirrorIds.size(); ++k) {
                        ImGui::PushID(static_cast<int>(k));
                        std::string del_mirror_label = "X##del_mirror_from_mode_" + std::to_string(k);
                        if (ImGui::Button(del_mirror_label.c_str())) { mirror_idx_to_remove = (int)k; }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(mode.mirrorIds[k].c_str());
                        ImGui::PopID();
                    }
                    if (mirror_idx_to_remove != -1) {
                        mode.mirrorIds.erase(mode.mirrorIds.begin() + mirror_idx_to_remove);
                        g_configIsDirty = true;
                    }
                    if (ImGui::BeginCombo("Add Mirror##add_mirror_to_mode", "[Select Mirror]")) {
                        for (const auto& mirrorConf : g_config.mirrors) {
                            if (std::find(mode.mirrorIds.begin(), mode.mirrorIds.end(), mirrorConf.name) == mode.mirrorIds.end()) {
                                if (ImGui::Selectable(mirrorConf.name.c_str())) {
                                    mode.mirrorIds.push_back(mirrorConf.name);
                                    g_configIsDirty = true;
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TreePop();
                }

                // --- MIRROR GROUPS SECTION ---
                if (ImGui::TreeNode("Mirror Groups##EyeZoom")) {
                    int group_idx_to_remove = -1;
                    for (size_t k = 0; k < mode.mirrorGroupIds.size(); ++k) {
                        ImGui::PushID(static_cast<int>(k));
                        std::string del_group_label = "X##del_mirror_group_from_eyezoom_" + std::to_string(k);
                        if (ImGui::Button(del_group_label.c_str())) { group_idx_to_remove = (int)k; }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(mode.mirrorGroupIds[k].c_str());
                        ImGui::PopID();
                    }
                    if (group_idx_to_remove != -1) {
                        mode.mirrorGroupIds.erase(mode.mirrorGroupIds.begin() + group_idx_to_remove);
                        g_configIsDirty = true;
                    }
                    if (ImGui::BeginCombo("Add Mirror Group##add_mirror_group_to_eyezoom", "[Select Group]")) {
                        for (const auto& groupConf : g_config.mirrorGroups) {
                            if (std::find(mode.mirrorGroupIds.begin(), mode.mirrorGroupIds.end(), groupConf.name) ==
                                mode.mirrorGroupIds.end()) {
                                if (ImGui::Selectable(groupConf.name.c_str())) {
                                    mode.mirrorGroupIds.push_back(groupConf.name);
                                    g_configIsDirty = true;
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TreePop();
                }

                // --- IMAGES SECTION ---
                if (ImGui::TreeNode("Images")) {
                    int image_idx_to_remove = -1;
                    for (size_t k = 0; k < mode.imageIds.size(); ++k) {
                        ImGui::PushID(static_cast<int>(k));
                        std::string del_img_label = "X##del_img_from_mode_" + std::to_string(k);
                        if (ImGui::Button(del_img_label.c_str())) { image_idx_to_remove = (int)k; }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(mode.imageIds[k].c_str());
                        ImGui::PopID();
                    }
                    if (image_idx_to_remove != -1) {
                        mode.imageIds.erase(mode.imageIds.begin() + image_idx_to_remove);
                        g_configIsDirty = true;
                    }
                    if (ImGui::BeginCombo("Add Image##add_image_to_mode", "[Select Image]")) {
                        for (const auto& imgConf : g_config.images) {
                            if (std::find(mode.imageIds.begin(), mode.imageIds.end(), imgConf.name) == mode.imageIds.end()) {
                                if (ImGui::Selectable(imgConf.name.c_str())) {
                                    mode.imageIds.push_back(imgConf.name);
                                    g_configIsDirty = true;
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TreePop();
                }

                // --- WINDOW OVERLAYS SECTION ---
                if (ImGui::TreeNode("Window Overlays")) {
                    int overlay_idx_to_remove = -1;
                    for (size_t k = 0; k < mode.windowOverlayIds.size(); ++k) {
                        ImGui::PushID(static_cast<int>(k));
                        std::string del_overlay_label = "X##del_overlay_from_mode_" + std::to_string(k);
                        if (ImGui::Button(del_overlay_label.c_str())) { overlay_idx_to_remove = (int)k; }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(mode.windowOverlayIds[k].c_str());
                        ImGui::PopID();
                    }
                    if (overlay_idx_to_remove != -1) {
                        mode.windowOverlayIds.erase(mode.windowOverlayIds.begin() + overlay_idx_to_remove);
                        g_configIsDirty = true;
                    }
                    if (ImGui::BeginCombo("Add Overlay##add_overlay_to_mode", "[Select Overlay]")) {
                        for (const auto& overlayConf : g_config.windowOverlays) {
                            if (std::find(mode.windowOverlayIds.begin(), mode.windowOverlayIds.end(), overlayConf.name) ==
                                mode.windowOverlayIds.end()) {
                                if (ImGui::Selectable(overlayConf.name.c_str())) {
                                    mode.windowOverlayIds.push_back(overlayConf.name);
                                    g_configIsDirty = true;
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TreePop();
                }

                // --- TRANSITION SETTINGS ---
                ImGui::Separator();
                if (ImGui::TreeNode("Transition Settings##EyeZoom")) {
                    RenderTransitionSettingsHorizontal(mode, "EyeZoom");

                    // Slide Zoom In option
                    ImGui::Separator();
                    if (ImGui::Checkbox("Slide Zoom In", &g_config.eyezoom.slideZoomIn)) { g_configIsDirty = true; }
                    ImGui::SameLine();
                    HelpMarker("When enabled, the zoom overlay slides in from the left instead of growing with the viewport. Both reach "
                               "their targets at the same time.");

                    if (ImGui::Checkbox("Slide Mirrors In", &g_config.eyezoom.slideMirrorsIn)) { g_configIsDirty = true; }
                    ImGui::SameLine();
                    HelpMarker("When enabled, mirrors slide in from the screen edge they are closest to (left or right) instead of "
                               "appearing instantly during transitions.");

                    ImGui::TreePop();
                }


                // --- SENSITIVITY OVERRIDE ---
                if (ImGui::TreeNode("Sensitivity Override##EyeZoom")) {
                    if (ImGui::Checkbox("Override Sensitivity", &mode.sensitivityOverrideEnabled)) { g_configIsDirty = true; }
                    HelpMarker("When enabled, this mode uses its own mouse sensitivity instead of the global setting.");

                    if (mode.sensitivityOverrideEnabled) {
                        if (ImGui::Checkbox("Separate X/Y##EyeZoom", &mode.separateXYSensitivity)) {
                            g_configIsDirty = true;
                            if (mode.separateXYSensitivity) {
                                mode.modeSensitivityX = mode.modeSensitivity;
                                mode.modeSensitivityY = mode.modeSensitivity;
                            }
                        }
                        ImGui::SameLine();
                        HelpMarker("Use different sensitivity values for horizontal (X) and vertical (Y) mouse movement.");

                        if (mode.separateXYSensitivity) {
                            ImGui::Text("X Sensitivity:");
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##EyeZoomSensitivityX", &mode.modeSensitivityX, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                            ImGui::Text("Y Sensitivity:");
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##EyeZoomSensitivityY", &mode.modeSensitivityY, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                        } else {
                            ImGui::Text("Sensitivity:");
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##EyeZoomSensitivity", &mode.modeSensitivity, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                            ImGui::SameLine();
                            HelpMarker("Mouse sensitivity for this mode (1.0 = normal)");
                        }
                    }
                    ImGui::TreePop();
                }

                ImGui::TreePop();
            }
            ImGui::PopID();
            break;
        }
    }

    // --- THIN MODE SECTION ---
    for (size_t i = 0; i < g_config.modes.size(); ++i) {
        auto& mode = g_config.modes[i];
        if (EqualsIgnoreCase(mode.id, "Thin")) {
            ImGui::PushID((int)i + 20000); // Use unique ID offset

            bool node_open = ImGui::TreeNodeEx("##mode_node", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", mode.id.c_str());
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::GetFrameHeight());
            ImGui::Dummy(ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));

            if (node_open) {
                if (!resolutionSupported) { ImGui::BeginDisabled(); }


                ImGui::Columns(2, "thin_dims", false);

                // Normal mode: editable spinners
                ImGui::Text("Width");
                ImGui::NextColumn();
                int tempWidth3 = mode.width;
                if (Spinner("##Width", &tempWidth3, 1, 1, screenWidth)) {
                    std::lock_guard<std::mutex> lock(g_pendingDimensionChangeMutex);
                    g_pendingDimensionChange.pending = true;
                    g_pendingDimensionChange.modeId = mode.id;
                    g_pendingDimensionChange.newWidth = tempWidth3;
                    g_pendingDimensionChange.newHeight = 0; // Unchanged
                    g_pendingDimensionChange.sendWmSize = (g_currentModeId == mode.id);
                }
                ImGui::NextColumn();
                ImGui::Text("Height");
                ImGui::NextColumn();
                int tempHeight3 = mode.height;
                if (Spinner("##Height", &tempHeight3, 1, 1, screenHeight)) {
                    std::lock_guard<std::mutex> lock(g_pendingDimensionChangeMutex);
                    g_pendingDimensionChange.pending = true;
                    g_pendingDimensionChange.modeId = mode.id;
                    g_pendingDimensionChange.newWidth = 0; // Unchanged
                    g_pendingDimensionChange.newHeight = tempHeight3;
                    g_pendingDimensionChange.sendWmSize = (g_currentModeId == mode.id);
                }
                ImGui::Columns(1);

                if (ImGui::Button("Switch to this Mode##Thin")) {
                    std::lock_guard<std::mutex> pendingLock(g_pendingModeSwitchMutex);
                    g_pendingModeSwitch.pending = true;
                    g_pendingModeSwitch.modeId = mode.id;
                    g_pendingModeSwitch.source = "GUI Thin mode";
                }

                if (!resolutionSupported) { ImGui::EndDisabled(); }

                if (g_currentModeId == mode.id) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(Current)");
                }

                // Transition Settings
                ImGui::Separator();
                if (ImGui::TreeNode("Transition Settings##Thin")) {
                    RenderTransitionSettingsHorizontal(mode, "Thin");
                    if (ImGui::Checkbox("Slide Mirrors In##Thin", &mode.slideMirrorsIn)) { g_configIsDirty = true; }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Mirrors slide in from the screen edges instead of appearing instantly");
                    }
                    ImGui::TreePop();
                }

                // Background
                if (ImGui::TreeNode("Background##Thin")) {
                    if (ImGui::RadioButton("Color##Thin", mode.background.selectedMode == "color")) {
                        mode.background.selectedMode = "color";
                        g_configIsDirty = true;
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton("Gradient##Thin", mode.background.selectedMode == "gradient")) {
                        if (mode.background.selectedMode != "gradient") {
                            mode.background.selectedMode = "gradient";
                            if (mode.background.gradientStops.size() < 2) {
                                mode.background.gradientStops.clear();
                                mode.background.gradientStops.push_back({ { 0.0f, 0.0f, 0.0f }, 0.0f });
                                mode.background.gradientStops.push_back({ { 1.0f, 1.0f, 1.0f }, 1.0f });
                            }
                            g_configIsDirty = true;
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton("Image##Thin", mode.background.selectedMode == "image")) {
                        if (mode.background.selectedMode != "image") {
                            mode.background.selectedMode = "image";
                            g_configIsDirty = true;
                            // Load existing background image if path is set
                            if (!mode.background.image.empty()) {
                                g_allImagesLoaded = false;
                                g_pendingImageLoad = true;
                                LoadImageAsync(DecodedImageData::Type::Background, mode.id, mode.background.image, g_toolscreenPath);
                            }
                        }
                    }
                    if (mode.background.selectedMode == "color") {
                        if (ImGui::ColorEdit3("##bgColorThin", &mode.background.color.r)) { g_configIsDirty = true; }
                    } else if (mode.background.selectedMode == "gradient") {
                        ImGui::SetNextItemWidth(200);
                        if (ImGui::SliderFloat("Angle##bgGradAngleThin", &mode.background.gradientAngle, 0.0f, 360.0f, "%.0f deg")) {
                            g_configIsDirty = true;
                        }
                        ImGui::Text("Color Stops:");
                        int stopToRemove = -1;
                        for (size_t i = 0; i < mode.background.gradientStops.size(); i++) {
                            ImGui::PushID(static_cast<int>(i));
                            auto& stop = mode.background.gradientStops[i];
                            if (ImGui::ColorEdit3("##StopColor", &stop.color.r, ImGuiColorEditFlags_NoInputs)) { g_configIsDirty = true; }
                            ImGui::SameLine();
                            float pos = stop.position * 100.0f;
                            ImGui::SetNextItemWidth(100);
                            if (ImGui::SliderFloat("##StopPos", &pos, 0.0f, 100.0f, "%.0f%%")) {
                                stop.position = pos / 100.0f;
                                g_configIsDirty = true;
                            }
                            if (mode.background.gradientStops.size() > 2) {
                                ImGui::SameLine();
                                if (ImGui::Button("X##RemoveStop")) { stopToRemove = static_cast<int>(i); }
                            }
                            ImGui::PopID();
                        }
                        if (stopToRemove >= 0) {
                            mode.background.gradientStops.erase(mode.background.gradientStops.begin() + stopToRemove);
                            g_configIsDirty = true;
                        }
                        if (mode.background.gradientStops.size() < 8) {
                            if (ImGui::Button("+ Add Color Stop##bgGradThin")) {
                                GradientColorStop newStop;
                                newStop.position = 0.5f;
                                newStop.color = { 0.5f, 0.5f, 0.5f };
                                mode.background.gradientStops.push_back(newStop);
                                std::sort(mode.background.gradientStops.begin(), mode.background.gradientStops.end(),
                                          [](const GradientColorStop& a, const GradientColorStop& b) { return a.position < b.position; });
                                g_configIsDirty = true;
                            }
                        }

                        // Animation controls
                        ImGui::Separator();
                        ImGui::Text("Animation:");
                        const char* animTypeNamesThin[] = { "None", "Rotate", "Slide", "Wave", "Spiral", "Fade" };
                        int currentAnimTypeThin = static_cast<int>(mode.background.gradientAnimation);
                        ImGui::SetNextItemWidth(120);
                        if (ImGui::Combo("Type##GradAnimThin", &currentAnimTypeThin, animTypeNamesThin, IM_ARRAYSIZE(animTypeNamesThin))) {
                            mode.background.gradientAnimation = static_cast<GradientAnimationType>(currentAnimTypeThin);
                            g_configIsDirty = true;
                        }
                        if (mode.background.gradientAnimation != GradientAnimationType::None) {
                            ImGui::SetNextItemWidth(150);
                            if (ImGui::SliderFloat("Speed##GradAnimSpeedThin", &mode.background.gradientAnimationSpeed, 0.1f, 5.0f, "%.1fx")) {
                                g_configIsDirty = true;
                            }
                            /*
                            if (ImGui::Checkbox("Color Fade##GradColorFadeThin", &mode.background.gradientColorFade)) {
                                g_configIsDirty = true;
                            }
                            if (ImGui::IsItemHovered()) {
                                ImGui::SetTooltip("Smoothly cycle colors through gradient stops");
                            }*/
                        }
                    } else if (mode.background.selectedMode == "image") {
                        std::string thinErrorKey = "mode_bg_thin";
                        if (ImGui::InputText("Path##Thin", &mode.background.image)) {
                            ClearImageError(thinErrorKey);
                            g_configIsDirty = true;
                            g_allImagesLoaded = false;
                            g_pendingImageLoad = true;
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Browse...##thin_bg")) {
                            ImagePickerResult result =
                                OpenImagePickerAndValidate(g_minecraftHwnd.load(), g_toolscreenPath, g_toolscreenPath);
                            if (result.completed) {
                                if (result.success) {
                                    mode.background.image = result.path;
                                    ClearImageError(thinErrorKey);
                                    g_allImagesLoaded = false;
                                    g_pendingImageLoad = true;
                                    g_configIsDirty = true;
                                } else if (!result.error.empty()) {
                                    SetImageError(thinErrorKey, result.error);
                                }
                            }
                        }
                        std::string thinBgError = GetImageError(thinErrorKey);
                        if (!thinBgError.empty()) { ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", thinBgError.c_str()); }
                    }
                    ImGui::TreePop();
                }

                // Border Settings
                if (ImGui::TreeNode("Border Settings##Thin")) {
                    if (ImGui::Checkbox("Enable Border##Thin", &mode.border.enabled)) { g_configIsDirty = true; }
                    if (mode.border.enabled) {
                        ImGui::Text("Color:");
                        ImVec4 borderCol = ImVec4(mode.border.color.r, mode.border.color.g, mode.border.color.b, 1.0f);
                        if (ImGui::ColorEdit3("##BorderColorThin", (float*)&borderCol, ImGuiColorEditFlags_NoInputs)) {
                            mode.border.color = { borderCol.x, borderCol.y, borderCol.z };
                            g_configIsDirty = true;
                        }
                        ImGui::Text("Width:");
                        if (Spinner("##BorderWidthThin", &mode.border.width, 1, 1, 50)) { g_configIsDirty = true; }
                    }
                    ImGui::TreePop();
                }

                // Mirrors
                if (ImGui::TreeNode("Mirrors##Thin")) {
                    int mirror_idx_to_remove = -1;
                    for (size_t k = 0; k < mode.mirrorIds.size(); ++k) {
                        ImGui::PushID(static_cast<int>(k));
                        if (ImGui::Button("X##del_mirror")) { mirror_idx_to_remove = (int)k; }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(mode.mirrorIds[k].c_str());
                        ImGui::PopID();
                    }
                    if (mirror_idx_to_remove != -1) {
                        mode.mirrorIds.erase(mode.mirrorIds.begin() + mirror_idx_to_remove);
                        g_configIsDirty = true;
                    }
                    if (ImGui::BeginCombo("Add Mirror##Thin", "[Select Mirror]")) {
                        for (const auto& mirrorConf : g_config.mirrors) {
                            if (std::find(mode.mirrorIds.begin(), mode.mirrorIds.end(), mirrorConf.name) == mode.mirrorIds.end()) {
                                if (ImGui::Selectable(mirrorConf.name.c_str())) {
                                    mode.mirrorIds.push_back(mirrorConf.name);
                                    g_configIsDirty = true;
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TreePop();
                }

                // Mirror Groups
                if (ImGui::TreeNode("Mirror Groups##Thin")) {
                    int group_idx_to_remove = -1;
                    for (size_t k = 0; k < mode.mirrorGroupIds.size(); ++k) {
                        ImGui::PushID(static_cast<int>(k));
                        std::string del_group_label = "X##del_mirror_group_from_thin_" + std::to_string(k);
                        if (ImGui::Button(del_group_label.c_str())) { group_idx_to_remove = (int)k; }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(mode.mirrorGroupIds[k].c_str());
                        ImGui::PopID();
                    }
                    if (group_idx_to_remove != -1) {
                        mode.mirrorGroupIds.erase(mode.mirrorGroupIds.begin() + group_idx_to_remove);
                        g_configIsDirty = true;
                    }
                    if (ImGui::BeginCombo("Add Mirror Group##add_mirror_group_to_thin", "[Select Group]")) {
                        for (const auto& groupConf : g_config.mirrorGroups) {
                            if (std::find(mode.mirrorGroupIds.begin(), mode.mirrorGroupIds.end(), groupConf.name) ==
                                mode.mirrorGroupIds.end()) {
                                if (ImGui::Selectable(groupConf.name.c_str())) {
                                    mode.mirrorGroupIds.push_back(groupConf.name);
                                    g_configIsDirty = true;
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TreePop();
                }

                // Images
                if (ImGui::TreeNode("Images##Thin")) {
                    int image_idx_to_remove = -1;
                    for (size_t k = 0; k < mode.imageIds.size(); ++k) {
                        ImGui::PushID(static_cast<int>(k));
                        if (ImGui::Button("X##del_img")) { image_idx_to_remove = (int)k; }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(mode.imageIds[k].c_str());
                        ImGui::PopID();
                    }
                    if (image_idx_to_remove != -1) {
                        mode.imageIds.erase(mode.imageIds.begin() + image_idx_to_remove);
                        g_configIsDirty = true;
                    }
                    if (ImGui::BeginCombo("Add Image##Thin", "[Select Image]")) {
                        for (const auto& imgConf : g_config.images) {
                            if (std::find(mode.imageIds.begin(), mode.imageIds.end(), imgConf.name) == mode.imageIds.end()) {
                                if (ImGui::Selectable(imgConf.name.c_str())) {
                                    mode.imageIds.push_back(imgConf.name);
                                    g_configIsDirty = true;
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TreePop();
                }

                // --- SENSITIVITY OVERRIDE ---
                if (ImGui::TreeNode("Sensitivity Override##Thin")) {
                    if (ImGui::Checkbox("Override Sensitivity##Thin", &mode.sensitivityOverrideEnabled)) { g_configIsDirty = true; }
                    HelpMarker("When enabled, this mode uses its own mouse sensitivity instead of the global setting.");

                    if (mode.sensitivityOverrideEnabled) {
                        if (ImGui::Checkbox("Separate X/Y##Thin", &mode.separateXYSensitivity)) {
                            g_configIsDirty = true;
                            if (mode.separateXYSensitivity) {
                                mode.modeSensitivityX = mode.modeSensitivity;
                                mode.modeSensitivityY = mode.modeSensitivity;
                            }
                        }
                        ImGui::SameLine();
                        HelpMarker("Use different sensitivity values for horizontal (X) and vertical (Y) mouse movement.");

                        if (mode.separateXYSensitivity) {
                            ImGui::Text("X Sensitivity:");
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##ThinSensitivityX", &mode.modeSensitivityX, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                            ImGui::Text("Y Sensitivity:");
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##ThinSensitivityY", &mode.modeSensitivityY, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                        } else {
                            ImGui::Text("Sensitivity:");
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##ThinSensitivity", &mode.modeSensitivity, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                            ImGui::SameLine();
                            HelpMarker("Mouse sensitivity for this mode (1.0 = normal)");
                        }
                    }
                    ImGui::TreePop();
                }

                ImGui::TreePop();
            }
            ImGui::PopID();
            break;
        }
    }

    // --- WIDE MODE SECTION ---
    for (size_t i = 0; i < g_config.modes.size(); ++i) {
        auto& mode = g_config.modes[i];
        if (EqualsIgnoreCase(mode.id, "Wide")) {
            ImGui::PushID((int)i + 30000); // Use unique ID offset

            bool node_open = ImGui::TreeNodeEx("##mode_node", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", mode.id.c_str());
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::GetFrameHeight());
            ImGui::Dummy(ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));

            if (node_open) {
                if (!resolutionSupported) { ImGui::BeginDisabled(); }

                ImGui::Columns(2, "wide_dims", false);

                // Absolute pixel spinners mode
                ImGui::Text("Width");
                ImGui::NextColumn();
                int tempWidth4 = mode.width;
                if (Spinner("##Width", &tempWidth4, 1, 1, screenWidth)) {
                    std::lock_guard<std::mutex> lock(g_pendingDimensionChangeMutex);
                    g_pendingDimensionChange.pending = true;
                    g_pendingDimensionChange.modeId = mode.id;
                    g_pendingDimensionChange.newWidth = tempWidth4;
                    g_pendingDimensionChange.newHeight = 0; // Unchanged
                    g_pendingDimensionChange.sendWmSize = (g_currentModeId == mode.id);
                }
                ImGui::NextColumn();
                ImGui::Text("Height");
                ImGui::NextColumn();
                int tempHeight4 = mode.height;
                if (Spinner("##Height", &tempHeight4, 1, 1, screenHeight)) {
                    std::lock_guard<std::mutex> lock(g_pendingDimensionChangeMutex);
                    g_pendingDimensionChange.pending = true;
                    g_pendingDimensionChange.modeId = mode.id;
                    g_pendingDimensionChange.newWidth = 0; // Unchanged
                    g_pendingDimensionChange.newHeight = tempHeight4;
                    g_pendingDimensionChange.sendWmSize = (g_currentModeId == mode.id);
                }
                ImGui::Columns(1);

                if (ImGui::Button("Switch to this Mode##Wide")) {
                    std::lock_guard<std::mutex> pendingLock(g_pendingModeSwitchMutex);
                    g_pendingModeSwitch.pending = true;
                    g_pendingModeSwitch.modeId = mode.id;
                    g_pendingModeSwitch.source = "GUI Wide mode";
                }

                if (!resolutionSupported) { ImGui::EndDisabled(); }

                if (g_currentModeId == mode.id) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(Current)");
                }

                // Transition Settings
                ImGui::Separator();
                if (ImGui::TreeNode("Transition Settings##Wide")) {
                    RenderTransitionSettingsHorizontal(mode, "Wide");
                    if (ImGui::Checkbox("Slide Mirrors In##Wide", &mode.slideMirrorsIn)) { g_configIsDirty = true; }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Mirrors slide in from the screen edges instead of appearing instantly");
                    }
                    ImGui::TreePop();
                }

                // Background
                if (ImGui::TreeNode("Background##Wide")) {
                    if (ImGui::RadioButton("Color##Wide", mode.background.selectedMode == "color")) {
                        mode.background.selectedMode = "color";
                        g_configIsDirty = true;
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton("Gradient##Wide", mode.background.selectedMode == "gradient")) {
                        if (mode.background.selectedMode != "gradient") {
                            mode.background.selectedMode = "gradient";
                            if (mode.background.gradientStops.size() < 2) {
                                mode.background.gradientStops.clear();
                                mode.background.gradientStops.push_back({ { 0.0f, 0.0f, 0.0f }, 0.0f });
                                mode.background.gradientStops.push_back({ { 1.0f, 1.0f, 1.0f }, 1.0f });
                            }
                            g_configIsDirty = true;
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton("Image##Wide", mode.background.selectedMode == "image")) {
                        if (mode.background.selectedMode != "image") {
                            mode.background.selectedMode = "image";
                            g_configIsDirty = true;
                            // Load existing background image if path is set
                            if (!mode.background.image.empty()) {
                                g_allImagesLoaded = false;
                                g_pendingImageLoad = true;
                                LoadImageAsync(DecodedImageData::Type::Background, mode.id, mode.background.image, g_toolscreenPath);
                            }
                        }
                    }
                    if (mode.background.selectedMode == "color") {
                        if (ImGui::ColorEdit3("##bgColorWide", &mode.background.color.r)) { g_configIsDirty = true; }
                    } else if (mode.background.selectedMode == "gradient") {
                        ImGui::SetNextItemWidth(200);
                        if (ImGui::SliderFloat("Angle##bgGradAngleWide", &mode.background.gradientAngle, 0.0f, 360.0f, "%.0f deg")) {
                            g_configIsDirty = true;
                        }
                        ImGui::Text("Color Stops:");
                        int stopToRemove = -1;
                        for (size_t i = 0; i < mode.background.gradientStops.size(); i++) {
                            ImGui::PushID(static_cast<int>(i));
                            auto& stop = mode.background.gradientStops[i];
                            if (ImGui::ColorEdit3("##StopColor", &stop.color.r, ImGuiColorEditFlags_NoInputs)) { g_configIsDirty = true; }
                            ImGui::SameLine();
                            float pos = stop.position * 100.0f;
                            ImGui::SetNextItemWidth(100);
                            if (ImGui::SliderFloat("##StopPos", &pos, 0.0f, 100.0f, "%.0f%%")) {
                                stop.position = pos / 100.0f;
                                g_configIsDirty = true;
                            }
                            if (mode.background.gradientStops.size() > 2) {
                                ImGui::SameLine();
                                if (ImGui::Button("X##RemoveStop")) { stopToRemove = static_cast<int>(i); }
                            }
                            ImGui::PopID();
                        }
                        if (stopToRemove >= 0) {
                            mode.background.gradientStops.erase(mode.background.gradientStops.begin() + stopToRemove);
                            g_configIsDirty = true;
                        }
                        if (mode.background.gradientStops.size() < 8) {
                            if (ImGui::Button("+ Add Color Stop##bgGradWide")) {
                                GradientColorStop newStop;
                                newStop.position = 0.5f;
                                newStop.color = { 0.5f, 0.5f, 0.5f };
                                mode.background.gradientStops.push_back(newStop);
                                std::sort(mode.background.gradientStops.begin(), mode.background.gradientStops.end(),
                                          [](const GradientColorStop& a, const GradientColorStop& b) { return a.position < b.position; });
                                g_configIsDirty = true;
                            }
                        }

                        // Animation controls
                        ImGui::Separator();
                        ImGui::Text("Animation:");
                        const char* animTypeNamesWide[] = { "None", "Rotate", "Slide", "Wave", "Spiral", "Fade" };
                        int currentAnimTypeWide = static_cast<int>(mode.background.gradientAnimation);
                        ImGui::SetNextItemWidth(120);
                        if (ImGui::Combo("Type##GradAnimWide", &currentAnimTypeWide, animTypeNamesWide, IM_ARRAYSIZE(animTypeNamesWide))) {
                            mode.background.gradientAnimation = static_cast<GradientAnimationType>(currentAnimTypeWide);
                            g_configIsDirty = true;
                        }
                        if (mode.background.gradientAnimation != GradientAnimationType::None) {
                            ImGui::SetNextItemWidth(150);
                            if (ImGui::SliderFloat("Speed##GradAnimSpeedWide", &mode.background.gradientAnimationSpeed, 0.1f, 5.0f, "%.1fx")) {
                                g_configIsDirty = true;
                            }
                            /*
                            if (ImGui::Checkbox("Color Fade##GradColorFadeWide", &mode.background.gradientColorFade)) {
                                g_configIsDirty = true;
                            }
                            if (ImGui::IsItemHovered()) {
                                ImGui::SetTooltip("Smoothly cycle colors through gradient stops");
                            }*/
                        }
                    } else if (mode.background.selectedMode == "image") {
                        std::string wideErrorKey = "mode_bg_wide";
                        if (ImGui::InputText("Path##Wide", &mode.background.image)) {
                            ClearImageError(wideErrorKey);
                            g_configIsDirty = true;
                            g_allImagesLoaded = false;
                            g_pendingImageLoad = true;
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Browse...##wide_bg")) {
                            ImagePickerResult result =
                                OpenImagePickerAndValidate(g_minecraftHwnd.load(), g_toolscreenPath, g_toolscreenPath);
                            if (result.completed) {
                                if (result.success) {
                                    mode.background.image = result.path;
                                    ClearImageError(wideErrorKey);
                                    g_allImagesLoaded = false;
                                    g_pendingImageLoad = true;
                                    g_configIsDirty = true;
                                } else if (!result.error.empty()) {
                                    SetImageError(wideErrorKey, result.error);
                                }
                            }
                        }
                        std::string wideBgError = GetImageError(wideErrorKey);
                        if (!wideBgError.empty()) { ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", wideBgError.c_str()); }
                    }
                    ImGui::TreePop();
                }

                // Border Settings
                if (ImGui::TreeNode("Border Settings##Wide")) {
                    if (ImGui::Checkbox("Enable Border##Wide", &mode.border.enabled)) { g_configIsDirty = true; }
                    if (mode.border.enabled) {
                        ImGui::Text("Color:");
                        ImVec4 borderCol = ImVec4(mode.border.color.r, mode.border.color.g, mode.border.color.b, 1.0f);
                        if (ImGui::ColorEdit3("##BorderColorWide", (float*)&borderCol, ImGuiColorEditFlags_NoInputs)) {
                            mode.border.color = { borderCol.x, borderCol.y, borderCol.z };
                            g_configIsDirty = true;
                        }
                        ImGui::Text("Width:");
                        if (Spinner("##BorderWidthWide", &mode.border.width, 1, 1, 50)) { g_configIsDirty = true; }
                    }
                    ImGui::TreePop();
                }

                // Mirrors
                if (ImGui::TreeNode("Mirrors##Wide")) {
                    int mirror_idx_to_remove = -1;
                    for (size_t k = 0; k < mode.mirrorIds.size(); ++k) {
                        ImGui::PushID(static_cast<int>(k));
                        if (ImGui::Button("X##del_mirror")) { mirror_idx_to_remove = (int)k; }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(mode.mirrorIds[k].c_str());
                        ImGui::PopID();
                    }
                    if (mirror_idx_to_remove != -1) {
                        mode.mirrorIds.erase(mode.mirrorIds.begin() + mirror_idx_to_remove);
                        g_configIsDirty = true;
                    }
                    if (ImGui::BeginCombo("Add Mirror##Wide", "[Select Mirror]")) {
                        for (const auto& mirrorConf : g_config.mirrors) {
                            if (std::find(mode.mirrorIds.begin(), mode.mirrorIds.end(), mirrorConf.name) == mode.mirrorIds.end()) {
                                if (ImGui::Selectable(mirrorConf.name.c_str())) {
                                    mode.mirrorIds.push_back(mirrorConf.name);
                                    g_configIsDirty = true;
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TreePop();
                }

                // Mirror Groups
                if (ImGui::TreeNode("Mirror Groups##Wide")) {
                    int group_idx_to_remove = -1;
                    for (size_t k = 0; k < mode.mirrorGroupIds.size(); ++k) {
                        ImGui::PushID(static_cast<int>(k));
                        std::string del_group_label = "X##del_mirror_group_from_wide_" + std::to_string(k);
                        if (ImGui::Button(del_group_label.c_str())) { group_idx_to_remove = (int)k; }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(mode.mirrorGroupIds[k].c_str());
                        ImGui::PopID();
                    }
                    if (group_idx_to_remove != -1) {
                        mode.mirrorGroupIds.erase(mode.mirrorGroupIds.begin() + group_idx_to_remove);
                        g_configIsDirty = true;
                    }
                    if (ImGui::BeginCombo("Add Mirror Group##add_mirror_group_to_wide", "[Select Group]")) {
                        for (const auto& groupConf : g_config.mirrorGroups) {
                            if (std::find(mode.mirrorGroupIds.begin(), mode.mirrorGroupIds.end(), groupConf.name) ==
                                mode.mirrorGroupIds.end()) {
                                if (ImGui::Selectable(groupConf.name.c_str())) {
                                    mode.mirrorGroupIds.push_back(groupConf.name);
                                    g_configIsDirty = true;
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TreePop();
                }

                // Images
                if (ImGui::TreeNode("Images##Wide")) {
                    int image_idx_to_remove = -1;
                    for (size_t k = 0; k < mode.imageIds.size(); ++k) {
                        ImGui::PushID(static_cast<int>(k));
                        if (ImGui::Button("X##del_img")) { image_idx_to_remove = (int)k; }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(mode.imageIds[k].c_str());
                        ImGui::PopID();
                    }
                    if (image_idx_to_remove != -1) {
                        mode.imageIds.erase(mode.imageIds.begin() + image_idx_to_remove);
                        g_configIsDirty = true;
                    }
                    if (ImGui::BeginCombo("Add Image##Wide", "[Select Image]")) {
                        for (const auto& imgConf : g_config.images) {
                            if (std::find(mode.imageIds.begin(), mode.imageIds.end(), imgConf.name) == mode.imageIds.end()) {
                                if (ImGui::Selectable(imgConf.name.c_str())) {
                                    mode.imageIds.push_back(imgConf.name);
                                    g_configIsDirty = true;
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TreePop();
                }

                // --- SENSITIVITY OVERRIDE ---
                if (ImGui::TreeNode("Sensitivity Override##Wide")) {
                    if (ImGui::Checkbox("Override Sensitivity##Wide", &mode.sensitivityOverrideEnabled)) { g_configIsDirty = true; }
                    HelpMarker("When enabled, this mode uses its own mouse sensitivity instead of the global setting.");

                    if (mode.sensitivityOverrideEnabled) {
                        if (ImGui::Checkbox("Separate X/Y##Wide", &mode.separateXYSensitivity)) {
                            g_configIsDirty = true;
                            if (mode.separateXYSensitivity) {
                                mode.modeSensitivityX = mode.modeSensitivity;
                                mode.modeSensitivityY = mode.modeSensitivity;
                            }
                        }
                        ImGui::SameLine();
                        HelpMarker("Use different sensitivity values for horizontal (X) and vertical (Y) mouse movement.");

                        if (mode.separateXYSensitivity) {
                            ImGui::Text("X Sensitivity:");
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##WideSensitivityX", &mode.modeSensitivityX, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                            ImGui::Text("Y Sensitivity:");
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##WideSensitivityY", &mode.modeSensitivityY, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                        } else {
                            ImGui::Text("Sensitivity:");
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##WideSensitivity", &mode.modeSensitivity, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                            ImGui::SameLine();
                            HelpMarker("Mouse sensitivity for this mode (1.0 = normal)");
                        }
                    }
                    ImGui::TreePop();
                }

                ImGui::TreePop();
            }
            ImGui::PopID();
            break;
        }
    }

    // --- CUSTOM MODES SECTION ---
    ImGui::SeparatorText("Custom Modes");

    for (size_t i = 0; i < g_config.modes.size(); ++i) {
        auto& mode = g_config.modes[i];
        // Skip Fullscreen and EyeZoom since they're already shown above
        if (!IsHardcodedMode(mode.id)) {
            ImGui::PushID((int)i);

            if (!resolutionSupported) { ImGui::BeginDisabled(); }

            // X button on the left
            std::string delete_button_label = "X##delete_mode_" + std::to_string(i);
            if (ImGui::Button(delete_button_label.c_str(), ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()))) {
                std::string popup_id = "Delete Mode?##" + std::to_string(i);
                ImGui::OpenPopup(popup_id.c_str());
            }

            if (!resolutionSupported) { ImGui::EndDisabled(); }

            // Popup modal outside of node_open block so it can be displayed even when collapsed
            std::string popup_id = "Delete Mode?##" + std::to_string(i);
            if (ImGui::BeginPopupModal(popup_id.c_str(), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("Are you sure you want to delete mode '%s'?\nThis cannot be undone.", mode.id.c_str());
                ImGui::Separator();
                if (ImGui::Button("OK", ImVec2(120, 0))) {
                    mode_to_remove = (int)i;
                    g_configIsDirty = true;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SetItemDefaultFocus();
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
                ImGui::EndPopup();
            }

            ImGui::SameLine();
            bool node_open = ImGui::TreeNodeEx("##mode_node", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", mode.id.c_str());

            if (node_open) {
                ImGui::Text("Name");
                ImGui::SetNextItemWidth(250);

                // Check for duplicate names and reserved names
                bool hasDuplicate = HasDuplicateModeName(mode.id, i);
                bool isReservedName = IsHardcodedMode(mode.id);
                bool hasError = hasDuplicate || isReservedName;

                if (hasError) {
                    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.6f, 0.2f, 0.2f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.7f, 0.3f, 0.3f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.8f, 0.3f, 0.3f, 1.0f));
                }

                std::string oldModeId = mode.id;
                if (ImGui::InputText("##Name", &mode.id)) {
                    // Check if the new name is valid (not duplicate and not reserved)
                    bool newIsReserved = IsHardcodedMode(mode.id);
                    if (!HasDuplicateModeName(mode.id, i) && !newIsReserved) {
                        g_configIsDirty = true;
                    } else {
                        // Revert the change if it creates a duplicate or uses a reserved name
                        mode.id = oldModeId;
                    }
                }

                if (hasError) { ImGui::PopStyleColor(3); }

                if (hasDuplicate) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Name already exists!");
                } else if (isReservedName) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Name is reserved!");
                }

                if (!resolutionSupported) { ImGui::BeginDisabled(); }

                ImGui::Columns(2, "dims", false);

                // Absolute pixel spinners mode
                ImGui::Text("Width");
                ImGui::NextColumn();
                int tempWidth5 = mode.width;
                if (Spinner("##Width", &tempWidth5, 1, 1)) {
                    std::lock_guard<std::mutex> lock(g_pendingDimensionChangeMutex);
                    g_pendingDimensionChange.pending = true;
                    g_pendingDimensionChange.modeId = mode.id;
                    g_pendingDimensionChange.newWidth = tempWidth5;
                    g_pendingDimensionChange.newHeight = 0; // Unchanged
                    g_pendingDimensionChange.sendWmSize = (g_currentModeId == mode.id);
                }
                ImGui::NextColumn();
                ImGui::Text("Height");
                ImGui::NextColumn();
                int tempHeight5 = mode.height;
                if (Spinner("##Height", &tempHeight5, 1, 1)) {
                    std::lock_guard<std::mutex> lock(g_pendingDimensionChangeMutex);
                    g_pendingDimensionChange.pending = true;
                    g_pendingDimensionChange.modeId = mode.id;
                    g_pendingDimensionChange.newWidth = 0; // Unchanged
                    g_pendingDimensionChange.newHeight = tempHeight5;
                    g_pendingDimensionChange.sendWmSize = (g_currentModeId == mode.id);
                }
                ImGui::Columns(1);

                if (ImGui::Button("Switch to this Mode")) {
                    // Defer mode switch to avoid deadlock (g_configMutex is held during GUI rendering)
                    std::lock_guard<std::mutex> pendingLock(g_pendingModeSwitchMutex);
                    g_pendingModeSwitch.pending = true;
                    g_pendingModeSwitch.modeId = mode.id;
                    g_pendingModeSwitch.source = "GUI mode detail";
                    Log("[GUI] Deferred mode switch to: " + mode.id);
                }

                // --- TRANSITION SETTINGS ---
                ImGui::Separator();
                if (ImGui::TreeNode("Transition Settings##CustomMode")) {
                    RenderTransitionSettingsHorizontal(mode, "CustomMode");
                    if (ImGui::Checkbox("Slide Mirrors In##CustomMode", &mode.slideMirrorsIn)) { g_configIsDirty = true; }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Mirrors slide in from the screen edges instead of appearing instantly");
                    }
                    ImGui::TreePop();
                }
                ImGui::Separator();
                ;

                // --- BORDER SETTINGS ---
                if (ImGui::TreeNode("Border Settings##CustomMode")) {
                    if (ImGui::Checkbox("Enable Border##CustomMode", &mode.border.enabled)) { g_configIsDirty = true; }
                    ImGui::SameLine();
                    HelpMarker("Draw a border around the game viewport. Border appears outside the game area.");

                    if (mode.border.enabled) {
                        ImGui::Text("Color:");
                        ImVec4 borderCol = ImVec4(mode.border.color.r, mode.border.color.g, mode.border.color.b, 1.0f);
                        if (ImGui::ColorEdit3("##BorderColorCustom", (float*)&borderCol, ImGuiColorEditFlags_NoInputs)) {
                            mode.border.color = { borderCol.x, borderCol.y, borderCol.z };
                            g_configIsDirty = true;
                        }

                        ImGui::Text("Width:");
                        ImGui::SetNextItemWidth(100);
                        if (Spinner("##BorderWidthCustom", &mode.border.width, 1, 1, 50)) { g_configIsDirty = true; }
                        ImGui::SameLine();
                        ImGui::TextDisabled("px");

                        ImGui::Text("Corner Radius:");
                        ImGui::SetNextItemWidth(100);
                        if (Spinner("##BorderRadiusCustom", &mode.border.radius, 1, 0, 100)) { g_configIsDirty = true; }
                        ImGui::SameLine();
                        ImGui::TextDisabled("px");
                    }
                    ImGui::TreePop();
                }
                ImGui::Separator();

                if (ImGui::TreeNode("Background")) {
                    if (ImGui::RadioButton("Color", mode.background.selectedMode == "color")) {
                        if (mode.background.selectedMode != "color") {
                            mode.background.selectedMode = "color";
                            g_configIsDirty = true;
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton("Gradient", mode.background.selectedMode == "gradient")) {
                        if (mode.background.selectedMode != "gradient") {
                            mode.background.selectedMode = "gradient";
                            if (mode.background.gradientStops.size() < 2) {
                                mode.background.gradientStops.clear();
                                mode.background.gradientStops.push_back({ { 0.0f, 0.0f, 0.0f }, 0.0f });
                                mode.background.gradientStops.push_back({ { 1.0f, 1.0f, 1.0f }, 1.0f });
                            }
                            g_configIsDirty = true;
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton("Image", mode.background.selectedMode == "image")) {
                        if (mode.background.selectedMode != "image") {
                            mode.background.selectedMode = "image";
                            g_configIsDirty = true;
                            // Load existing background image if path is set
                            if (!mode.background.image.empty()) {
                                g_allImagesLoaded = false;
                                g_pendingImageLoad = true;
                                LoadImageAsync(DecodedImageData::Type::Background, mode.id, mode.background.image, g_toolscreenPath);
                            }
                        }
                    }

                    if (mode.background.selectedMode == "color") {
                        if (ImGui::ColorEdit3("##bgColor", &mode.background.color.r)) { g_configIsDirty = true; }
                    } else if (mode.background.selectedMode == "gradient") {
                        std::string gradId = "##bgGrad_" + mode.id;
                        ImGui::SetNextItemWidth(200);
                        if (ImGui::SliderFloat(("Angle" + gradId).c_str(), &mode.background.gradientAngle, 0.0f, 360.0f, "%.0f deg")) {
                            g_configIsDirty = true;
                        }
                        ImGui::Text("Color Stops:");
                        int stopToRemove = -1;
                        for (size_t i = 0; i < mode.background.gradientStops.size(); i++) {
                            ImGui::PushID(static_cast<int>(i));
                            auto& stop = mode.background.gradientStops[i];
                            if (ImGui::ColorEdit3("##StopColor", &stop.color.r, ImGuiColorEditFlags_NoInputs)) { g_configIsDirty = true; }
                            ImGui::SameLine();
                            float pos = stop.position * 100.0f;
                            ImGui::SetNextItemWidth(100);
                            if (ImGui::SliderFloat("##StopPos", &pos, 0.0f, 100.0f, "%.0f%%")) {
                                stop.position = pos / 100.0f;
                                g_configIsDirty = true;
                            }
                            if (mode.background.gradientStops.size() > 2) {
                                ImGui::SameLine();
                                if (ImGui::Button("X##RemoveStop")) { stopToRemove = static_cast<int>(i); }
                            }
                            ImGui::PopID();
                        }
                        if (stopToRemove >= 0) {
                            mode.background.gradientStops.erase(mode.background.gradientStops.begin() + stopToRemove);
                            g_configIsDirty = true;
                        }
                        if (mode.background.gradientStops.size() < 8) {
                            if (ImGui::Button(("+ Add Color Stop" + gradId).c_str())) {
                                GradientColorStop newStop;
                                newStop.position = 0.5f;
                                newStop.color = { 0.5f, 0.5f, 0.5f };
                                mode.background.gradientStops.push_back(newStop);
                                std::sort(mode.background.gradientStops.begin(), mode.background.gradientStops.end(),
                                          [](const GradientColorStop& a, const GradientColorStop& b) { return a.position < b.position; });
                                g_configIsDirty = true;
                            }
                        }

                        // Animation controls
                        ImGui::Separator();
                        ImGui::Text("Animation:");
                        const char* animTypeNamesCustom[] = { "None", "Rotate", "Slide", "Wave", "Spiral", "Fade" };
                        int currentAnimTypeCustom = static_cast<int>(mode.background.gradientAnimation);
                        ImGui::SetNextItemWidth(120);
                        if (ImGui::Combo(("Type" + gradId).c_str(), &currentAnimTypeCustom, animTypeNamesCustom, IM_ARRAYSIZE(animTypeNamesCustom))) {
                            mode.background.gradientAnimation = static_cast<GradientAnimationType>(currentAnimTypeCustom);
                            g_configIsDirty = true;
                        }
                        if (mode.background.gradientAnimation != GradientAnimationType::None) {
                            ImGui::SetNextItemWidth(150);
                            if (ImGui::SliderFloat(("Speed" + gradId).c_str(), &mode.background.gradientAnimationSpeed, 0.1f, 5.0f, "%.1fx")) {
                                g_configIsDirty = true;
                            }
                            /*
                            if (ImGui::Checkbox(("Color Fade" + gradId).c_str(), &mode.background.gradientColorFade)) {
                                g_configIsDirty = true;
                            }
                            if (ImGui::IsItemHovered()) {
                                ImGui::SetTooltip("Smoothly cycle colors through gradient stops");
                            }*/
                        }
                    } else if (mode.background.selectedMode == "image") {
                        std::string modeErrorKey = "mode_bg_" + mode.id;
                        if (ImGui::InputText("Path", &mode.background.image)) {
                            ClearImageError(modeErrorKey);
                            g_configIsDirty = true;
                            g_allImagesLoaded = false;
                            g_pendingImageLoad = true;
                        }
                        ImGui::SameLine();
                        if (ImGui::Button(("Browse...##mode_bg_" + mode.id).c_str())) {
                            // Use the validated image picker
                            ImagePickerResult result =
                                OpenImagePickerAndValidate(g_minecraftHwnd.load(), g_toolscreenPath, g_toolscreenPath);

                            if (result.completed) {
                                if (result.success) {
                                    mode.background.image = result.path;
                                    ClearImageError(modeErrorKey);
                                    g_allImagesLoaded = false;
                                    g_pendingImageLoad = true;
                                    g_configIsDirty = true;
                                } else if (!result.error.empty()) {
                                    SetImageError(modeErrorKey, result.error);
                                }
                            }
                        }

                        // Show error message if any
                        std::string bgError = GetImageError(modeErrorKey);
                        if (!bgError.empty()) { ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", bgError.c_str()); }
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode("Mirrors")) {
                    int mirror_idx_to_remove = -1;
                    for (size_t k = 0; k < mode.mirrorIds.size(); ++k) {
                        ImGui::PushID(static_cast<int>(k));
                        std::string del_mirror_label = "X##del_mirror_from_mode_" + std::to_string(k);
                        if (ImGui::Button(del_mirror_label.c_str())) { mirror_idx_to_remove = (int)k; }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(mode.mirrorIds[k].c_str());
                        ImGui::PopID();
                    }
                    if (mirror_idx_to_remove != -1) {
                        mode.mirrorIds.erase(mode.mirrorIds.begin() + mirror_idx_to_remove);
                        g_configIsDirty = true;
                    }
                    if (ImGui::BeginCombo("Add Mirror##add_mirror_to_mode", "[Select Mirror]")) {
                        for (const auto& mirrorConf : g_config.mirrors) {
                            if (std::find(mode.mirrorIds.begin(), mode.mirrorIds.end(), mirrorConf.name) == mode.mirrorIds.end()) {
                                if (ImGui::Selectable(mirrorConf.name.c_str())) {
                                    mode.mirrorIds.push_back(mirrorConf.name);
                                    g_configIsDirty = true;
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode("Mirror Groups##Custom")) {
                    int group_idx_to_remove = -1;
                    for (size_t k = 0; k < mode.mirrorGroupIds.size(); ++k) {
                        ImGui::PushID(static_cast<int>(k));
                        std::string del_group_label = "X##del_mirror_group_from_custom_" + std::to_string(k);
                        if (ImGui::Button(del_group_label.c_str())) { group_idx_to_remove = (int)k; }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(mode.mirrorGroupIds[k].c_str());
                        ImGui::PopID();
                    }
                    if (group_idx_to_remove != -1) {
                        mode.mirrorGroupIds.erase(mode.mirrorGroupIds.begin() + group_idx_to_remove);
                        g_configIsDirty = true;
                    }
                    if (ImGui::BeginCombo("Add Mirror Group##add_mirror_group_to_custom", "[Select Group]")) {
                        for (const auto& groupConf : g_config.mirrorGroups) {
                            if (std::find(mode.mirrorGroupIds.begin(), mode.mirrorGroupIds.end(), groupConf.name) ==
                                mode.mirrorGroupIds.end()) {
                                if (ImGui::Selectable(groupConf.name.c_str())) {
                                    mode.mirrorGroupIds.push_back(groupConf.name);
                                    g_configIsDirty = true;
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode("Images")) {
                    int image_idx_to_remove = -1;
                    for (size_t k = 0; k < mode.imageIds.size(); ++k) {
                        ImGui::PushID(static_cast<int>(k));
                        std::string del_img_label = "X##del_img_from_mode_" + std::to_string(k);
                        if (ImGui::Button(del_img_label.c_str())) { image_idx_to_remove = (int)k; }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(mode.imageIds[k].c_str());
                        ImGui::PopID();
                    }
                    if (image_idx_to_remove != -1) {
                        mode.imageIds.erase(mode.imageIds.begin() + image_idx_to_remove);
                        g_configIsDirty = true;
                    }
                    if (ImGui::BeginCombo("Add Image##add_image_to_mode", "[Select Image]")) {
                        for (const auto& imgConf : g_config.images) {
                            if (std::find(mode.imageIds.begin(), mode.imageIds.end(), imgConf.name) == mode.imageIds.end()) {
                                if (ImGui::Selectable(imgConf.name.c_str())) {
                                    mode.imageIds.push_back(imgConf.name);
                                    g_configIsDirty = true;
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode("Window Overlays")) {
                    int windowOverlay_idx_to_remove = -1;
                    for (size_t k = 0; k < mode.windowOverlayIds.size(); ++k) {
                        ImGui::PushID(static_cast<int>(k));
                        std::string del_overlay_label = "X##del_overlay_from_mode_" + std::to_string(k);
                        if (ImGui::Button(del_overlay_label.c_str())) { windowOverlay_idx_to_remove = (int)k; }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(mode.windowOverlayIds[k].c_str());
                        ImGui::PopID();
                    }
                    if (windowOverlay_idx_to_remove != -1) {
                        mode.windowOverlayIds.erase(mode.windowOverlayIds.begin() + windowOverlay_idx_to_remove);
                        g_configIsDirty = true;
                    }
                    if (ImGui::BeginCombo("Add Window Overlay##add_overlay_to_mode2", "[Select Window Overlay]")) {
                        for (const auto& overlayConf : g_config.windowOverlays) {
                            if (std::find(mode.windowOverlayIds.begin(), mode.windowOverlayIds.end(), overlayConf.name) ==
                                mode.windowOverlayIds.end()) {
                                if (ImGui::Selectable(overlayConf.name.c_str())) {
                                    mode.windowOverlayIds.push_back(overlayConf.name);
                                    g_configIsDirty = true;
                                }
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode("Stretch Properties")) {
                    if (ImGui::Checkbox("Enable Stretch", &mode.stretch.enabled)) g_configIsDirty = true;
                    ImGui::Columns(2, "stretch_cols", false);
                    ImGui::SetColumnWidth(0, 150);
                    ImGui::Text("X Position");
                    ImGui::NextColumn();
                    if (Spinner("##StretchX", &mode.stretch.x)) g_configIsDirty = true;
                    ImGui::SameLine();
                    if (ImGui::Button("Center H")) {
                        mode.stretch.x = (GetCachedScreenWidth() - mode.stretch.width) / 2;
                        g_configIsDirty = true;
                    }
                    ImGui::NextColumn();
                    ImGui::Text("Width");
                    ImGui::NextColumn();
                    if (Spinner("##StretchW", &mode.stretch.width, 1, 1)) g_configIsDirty = true;
                    ImGui::NextColumn();
                    ImGui::Text("Y Position");
                    ImGui::NextColumn();
                    if (Spinner("##StretchY", &mode.stretch.y)) g_configIsDirty = true;
                    ImGui::SameLine();
                    if (ImGui::Button("Center V")) {
                        mode.stretch.y = (GetCachedScreenHeight() - mode.stretch.height) / 2;
                        g_configIsDirty = true;
                    }
                    ImGui::NextColumn();
                    ImGui::Text("Height");
                    ImGui::NextColumn();
                    if (Spinner("##StretchH", &mode.stretch.height, 1, 1)) g_configIsDirty = true;
                    ImGui::Columns(1);
                    ImGui::TreePop();
                }

                // --- EXPRESSIONS SECTION ---
                if (ImGui::TreeNode("Expressions")) {
                    ImGui::TextWrapped("Use expressions for dynamic dimensions based on screen size.");
                    ImGui::TextDisabled("Variables: screenWidth, screenHeight");
                    ImGui::TextDisabled("Functions: min(), max(), floor(), ceil(), round(), abs()");
                    ImGui::Separator();

                    int screenW = GetCachedScreenWidth();
                    int screenH = GetCachedScreenHeight();

                    // Mode Width Expression
                    ImGui::Text("Mode Width:");
                    ImGui::SetNextItemWidth(250);
                    if (ImGui::InputText("##ModeWidthExpr", &mode.widthExpr)) {
                        g_configIsDirty = true;
                        if (!mode.widthExpr.empty()) {
                            int val = EvaluateExpression(mode.widthExpr, screenW, screenH, mode.width);
                            if (val > 0) mode.width = val;
                        }
                    }
                    if (!mode.widthExpr.empty()) {
                        std::string err;
                        if (!ValidateExpression(mode.widthExpr, err)) {
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Invalid");
                            if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", err.c_str()); }
                        } else {
                            ImGui::SameLine();
                            ImGui::TextDisabled("= %d", mode.width);
                        }
                    }

                    // Mode Height Expression
                    ImGui::Text("Mode Height:");
                    ImGui::SetNextItemWidth(250);
                    if (ImGui::InputText("##ModeHeightExpr", &mode.heightExpr)) {
                        g_configIsDirty = true;
                        if (!mode.heightExpr.empty()) {
                            int val = EvaluateExpression(mode.heightExpr, screenW, screenH, mode.height);
                            if (val > 0) mode.height = val;
                        }
                    }
                    if (!mode.heightExpr.empty()) {
                        std::string err;
                        if (!ValidateExpression(mode.heightExpr, err)) {
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Invalid");
                            if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", err.c_str()); }
                        } else {
                            ImGui::SameLine();
                            ImGui::TextDisabled("= %d", mode.height);
                        }
                    }

                    ImGui::Separator();
                    ImGui::Text("Stretch Expressions:");

                    // Stretch Width Expression
                    ImGui::Text("Stretch Width:");
                    ImGui::SetNextItemWidth(250);
                    if (ImGui::InputText("##StretchWidthExpr", &mode.stretch.widthExpr)) {
                        g_configIsDirty = true;
                        if (!mode.stretch.widthExpr.empty()) {
                            int val = EvaluateExpression(mode.stretch.widthExpr, screenW, screenH, mode.stretch.width);
                            if (val >= 0) mode.stretch.width = val;
                        }
                    }
                    if (!mode.stretch.widthExpr.empty()) {
                        std::string err;
                        if (!ValidateExpression(mode.stretch.widthExpr, err)) {
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Invalid");
                            if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", err.c_str()); }
                        } else {
                            ImGui::SameLine();
                            ImGui::TextDisabled("= %d", mode.stretch.width);
                        }
                    }

                    // Stretch Height Expression
                    ImGui::Text("Stretch Height:");
                    ImGui::SetNextItemWidth(250);
                    if (ImGui::InputText("##StretchHeightExpr", &mode.stretch.heightExpr)) {
                        g_configIsDirty = true;
                        if (!mode.stretch.heightExpr.empty()) {
                            int val = EvaluateExpression(mode.stretch.heightExpr, screenW, screenH, mode.stretch.height);
                            if (val >= 0) mode.stretch.height = val;
                        }
                    }
                    if (!mode.stretch.heightExpr.empty()) {
                        std::string err;
                        if (!ValidateExpression(mode.stretch.heightExpr, err)) {
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Invalid");
                            if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", err.c_str()); }
                        } else {
                            ImGui::SameLine();
                            ImGui::TextDisabled("= %d", mode.stretch.height);
                        }
                    }

                    // Stretch X Expression
                    ImGui::Text("Stretch X Position:");
                    ImGui::SetNextItemWidth(250);
                    if (ImGui::InputText("##StretchXExpr", &mode.stretch.xExpr)) {
                        g_configIsDirty = true;
                        if (!mode.stretch.xExpr.empty()) {
                            mode.stretch.x = EvaluateExpression(mode.stretch.xExpr, screenW, screenH, mode.stretch.x);
                        }
                    }
                    if (!mode.stretch.xExpr.empty()) {
                        std::string err;
                        if (!ValidateExpression(mode.stretch.xExpr, err)) {
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Invalid");
                            if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", err.c_str()); }
                        } else {
                            ImGui::SameLine();
                            ImGui::TextDisabled("= %d", mode.stretch.x);
                        }
                    }

                    // Stretch Y Expression
                    ImGui::Text("Stretch Y Position:");
                    ImGui::SetNextItemWidth(250);
                    if (ImGui::InputText("##StretchYExpr", &mode.stretch.yExpr)) {
                        g_configIsDirty = true;
                        if (!mode.stretch.yExpr.empty()) {
                            mode.stretch.y = EvaluateExpression(mode.stretch.yExpr, screenW, screenH, mode.stretch.y);
                        }
                    }
                    if (!mode.stretch.yExpr.empty()) {
                        std::string err;
                        if (!ValidateExpression(mode.stretch.yExpr, err)) {
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Invalid");
                            if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", err.c_str()); }
                        } else {
                            ImGui::SameLine();
                            ImGui::TextDisabled("= %d", mode.stretch.y);
                        }
                    }

                    ImGui::TreePop();
                }

                if (ImGui::TreeNode("Sensitivity Override")) {
                    if (ImGui::Checkbox("Override Sensitivity", &mode.sensitivityOverrideEnabled)) { g_configIsDirty = true; }
                    HelpMarker("When enabled, this mode uses its own mouse sensitivity instead of the global setting.");

                    if (mode.sensitivityOverrideEnabled) {
                        if (ImGui::Checkbox("Separate X/Y##Custom", &mode.separateXYSensitivity)) {
                            g_configIsDirty = true;
                            if (mode.separateXYSensitivity) {
                                mode.modeSensitivityX = mode.modeSensitivity;
                                mode.modeSensitivityY = mode.modeSensitivity;
                            }
                        }
                        ImGui::SameLine();
                        HelpMarker("Use different sensitivity values for horizontal (X) and vertical (Y) mouse movement.");

                        if (mode.separateXYSensitivity) {
                            ImGui::Text("X Sensitivity:");
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##ModeSensitivityX", &mode.modeSensitivityX, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                            ImGui::Text("Y Sensitivity:");
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##ModeSensitivityY", &mode.modeSensitivityY, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                        } else {
                            ImGui::Text("Sensitivity:");
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##ModeSensitivity", &mode.modeSensitivity, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                            ImGui::SameLine();
                            HelpMarker("Mouse sensitivity for this mode (1.0 = normal)");
                        }
                    }
                    ImGui::TreePop();
                }

                if (!resolutionSupported) { ImGui::EndDisabled(); }

                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }
    // Prevent deletion of hardcoded modes (Fullscreen and EyeZoom)
    if (mode_to_remove != -1) {
        auto& modeToDelete = g_config.modes[mode_to_remove];
        if (!IsHardcodedMode(modeToDelete.id)) {
            // If the mode being deleted is the current active mode, switch to Fullscreen first
            // to prevent crashes from being in a non-existent mode
            std::string currentMode;
            {
                std::lock_guard<std::mutex> modeLock(g_modeIdMutex);
                currentMode = g_currentModeId;
            }
            if (EqualsIgnoreCase(currentMode, modeToDelete.id)) {
                // Queue an instant switch to Fullscreen (deferred to avoid deadlock)
                std::lock_guard<std::mutex> pendingLock(g_pendingModeSwitchMutex);
                g_pendingModeSwitch.pending = true;
                g_pendingModeSwitch.modeId = "Fullscreen";
                g_pendingModeSwitch.source = "Mode deleted";
                g_pendingModeSwitch.isPreview = false;
                g_pendingModeSwitch.forceInstant = true;
                Log("[GUI] Mode '" + modeToDelete.id + "' was active and is being deleted - switching to Fullscreen");
            }
            g_config.modes.erase(g_config.modes.begin() + mode_to_remove);
            g_configIsDirty = true;
        }
    }

    ImGui::Separator();

    if (!resolutionSupported) { ImGui::BeginDisabled(); }

    if (ImGui::Button("Add New Mode")) {
        ModeConfig newMode;
        newMode.id = "New Mode " + std::to_string(g_config.modes.size() + 1);
        newMode.width = GetCachedScreenWidth();
        newMode.height = GetCachedScreenHeight();
        newMode.stretch.width = 300;
        newMode.stretch.height = GetCachedScreenHeight();
        g_config.modes.push_back(newMode);
        g_configIsDirty = true;
    }

    ImGui::SameLine();
    if (ImGui::Button("Reset to Defaults##modes")) { ImGui::OpenPopup("Reset Modes to Defaults?"); }

    if (!resolutionSupported) { ImGui::EndDisabled(); }

    if (ImGui::BeginPopupModal("Reset Modes to Defaults?", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f), "WARNING:");
        ImGui::Text("This will delete ALL user-created modes and restore the default modes.");
        ImGui::Text("This action cannot be undone.");
        ImGui::Separator();
        if (ImGui::Button("Confirm Reset", ImVec2(120, 0))) {
            g_config.modes = GetDefaultModes();
            g_config.eyezoom = GetDefaultEyeZoomConfig();

            // After resetting, apply dynamic sizing so percentage/expression defaults behave correctly.
            // (Example: Wide mode uses height = 0.25, which must be converted to pixels.)
            int screenW = GetCachedScreenWidth();
            int screenH = GetCachedScreenHeight();
            if (screenW < 1) screenW = 1;
            if (screenH < 1) screenH = 1;

            for (auto& mode : g_config.modes) {
                bool widthIsRelative = mode.widthExpr.empty() && mode.relativeWidth >= 0.0f && mode.relativeWidth <= 1.0f;
                bool heightIsRelative = mode.heightExpr.empty() && mode.relativeHeight >= 0.0f && mode.relativeHeight <= 1.0f;

                if (widthIsRelative) {
                    int w = static_cast<int>(mode.relativeWidth * screenW);
                    if (w < 1) w = 1;
                    mode.width = w;
                }
                if (heightIsRelative) {
                    int h = static_cast<int>(mode.relativeHeight * screenH);
                    if (h < 1) h = 1;
                    mode.height = h;
                }
            }

            // Evaluate expression-based dimensions (uses cached screen size).
            RecalculateExpressionDimensions();

            g_configIsDirty = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    ImGui::EndTabItem();
}
