if (BeginSelectableSettingsNestedTabItem(trc("tabs.window_overlays"))) {
    g_currentlyEditingMirror = "";

    std::string scrollToWindowOverlayThisFrame;
    if (!g_scrollToWindowOverlayName.empty()) {
        g_selectedWindowOverlayName = g_scrollToWindowOverlayName;
        scrollToWindowOverlayThisFrame = g_scrollToWindowOverlayName;
        g_scrollToWindowOverlayName.clear();
    }

    SliderCtrlClickTip();
    const std::string g_currentModeId = GetPublishedCurrentModeId();

    int windowOverlay_to_remove = -1;
    for (size_t i = 0; i < g_config.windowOverlays.size(); ++i) {
        auto& overlay = g_config.windowOverlays[i];
        ImGui::PushID((int)i);

        std::string delete_overlay_label = "X##delete_overlay_" + std::to_string(i);
        if (ImGui::Button(delete_overlay_label.c_str(), ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()))) {
            std::string overlay_popup_id = (tr("window.overlays_delete") + "##" + std::to_string(i));
            ImGui::OpenPopup(overlay_popup_id.c_str());
        }

        std::string overlay_popup_id = (tr("window.overlays_delete") + "##" + std::to_string(i));
        if (ImGui::BeginPopupModal(overlay_popup_id.c_str(), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped("%s", trc("window.tooltip_delete", overlay.name));
            ImGui::Separator();
            if (ImGui::Button(trc("button.ok"), ImVec2(120, 0))) {
                windowOverlay_to_remove = (int)i;
                g_configIsDirty = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetItemDefaultFocus();
            ImGui::SameLine();
            if (ImGui::Button(trc("button.cancel"), ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }

        ImGui::SameLine();

        std::string oldOverlayName = overlay.name;

        const bool isScrollTarget = (!scrollToWindowOverlayThisFrame.empty() && overlay.name == scrollToWindowOverlayThisFrame);
        if (isScrollTarget) { ImGui::SetNextItemOpen(true, ImGuiCond_Always); }
        bool node_open = ImGui::TreeNodeEx("##overlay_node", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", overlay.name.c_str());
        if (isScrollTarget) { ImGui::SetScrollHereY(0.2f); }

        if (node_open) {

            bool hasDuplicate = HasDuplicateWindowOverlayName(overlay.name, i);
            if (hasDuplicate) {
                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.6f, 0.2f, 0.2f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.7f, 0.3f, 0.3f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.8f, 0.3f, 0.3f, 1.0f));
            }

            if (ImGui::InputText(trc("window.overlays_name"), &overlay.name)) {
                if (!HasDuplicateWindowOverlayName(overlay.name, i)) {
                    g_configIsDirty = true;
                    if (oldOverlayName != overlay.name) {
                        for (auto& mode : g_config.modes) {
                            RenameModeSource(mode, ModeSourceType::WindowOverlay, oldOverlayName, overlay.name);
                        }
                        // Render and capture look overlays up by name; re-key the cache entry so it stays visible.
                        {
                            std::lock_guard<std::mutex> cacheLock(g_windowOverlayCacheMutex);
                            auto it = g_windowOverlayCache.find(oldOverlayName);
                            if (it != g_windowOverlayCache.end() && g_windowOverlayCache.find(overlay.name) == g_windowOverlayCache.end()) {
                                auto node = g_windowOverlayCache.extract(it);
                                node.key() = overlay.name;
                                g_windowOverlayCache.insert(std::move(node));
                            }
                        }
                    }
                } else {
                    overlay.name = oldOverlayName;
                }
            }

            if (hasDuplicate) { ImGui::PopStyleColor(3); }

            if (hasDuplicate) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), trc("images.name_duplicate"));
            }

            // Dropdown for selecting from currently open windows
            ImGui::Separator();
            ImGui::Text(trc("window.overlays_select_window"));

            // Get cached window list from background thread (non-blocking)
            auto s_cachedWindows = GetCachedWindowList();

            ImGui::PushID(("window_dropdown_" + std::to_string(i)).c_str());

            std::string dropdownPreview = trc("window.overlays_choose_window");
            if (!overlay.windowTitle.empty()) {
                WindowInfo currentInfo;
                currentInfo.title = overlay.windowTitle;
                currentInfo.className = overlay.windowClass;
                currentInfo.executableName = overlay.executableName;
                dropdownPreview = currentInfo.GetDisplayName();
                if (dropdownPreview.length() > 60) { dropdownPreview = dropdownPreview.substr(0, 57) + "..."; }
            }

            if (ImGui::BeginCombo("##WindowSelector", dropdownPreview.c_str())) {
                if (s_cachedWindows.empty()) {
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), trc("window.overlays_not_found"));
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), trc("window.overlays_thread_exit"));
                } else {
                    for (const auto& windowInfo : s_cachedWindows) {
                        bool isSelected = (overlay.windowTitle == windowInfo.title && overlay.windowClass == windowInfo.className &&
                                           overlay.executableName == windowInfo.executableName);

                        std::string displayText = windowInfo.GetDisplayName();
                        if (!IsWindowInfoValid(windowInfo)) {
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
                        } else {
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
                        }

                        if (ImGui::Selectable(displayText.c_str(), isSelected)) {
                            if (IsWindowInfoValid(windowInfo)) {
                                overlay.windowTitle = windowInfo.title;
                                overlay.windowClass = windowInfo.className;
                                overlay.executableName = windowInfo.executableName;
                                g_configIsDirty = true;
                                // Queue deferred reload to avoid blocking GUI thread
                                QueueOverlayReload(overlay.name, overlay);
                            }
                        }
                        ImGui::PopStyleColor();
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::PopID();

            ImGui::Text(trc("window.overlays_match_priority"));
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(trc("window.tooltip_match_priority"));
            }

            const char* priorityOptions[] = { trc("window.overlays_match_priority_title"), trc("window.overlays_match_priority_title_executable") };
            const char* priorityValues[] = { "title", "title_executable" };

            int currentPriorityIdx = 0;
            for (int idx = 0; idx < 2; idx++) {
                if (overlay.windowMatchPriority == priorityValues[idx]) {
                    currentPriorityIdx = idx;
                    break;
                }
            }

            ImGui::PushItemWidth(300.0f);
            if (ImGui::Combo("##MatchPriority", &currentPriorityIdx, priorityOptions, 2)) {
                overlay.windowMatchPriority = priorityValues[currentPriorityIdx];
                g_configIsDirty = true;
                // Queue deferred reload to avoid blocking GUI thread
                QueueOverlayReload(overlay.name, overlay);
            }
            ImGui::PopItemWidth();
            ImGui::SeparatorText(trc("window.overlays_rendering"));
            if (ImGui::SliderFloat(trc("label.opacity"), &overlay.opacity, 0.0f, 1.0f)) g_configIsDirty = true;
            if (ImGui::Checkbox(trc("window.overlays_pixelated_scaling"), &overlay.pixelatedScaling)) g_configIsDirty = true;
            if (ImGui::Checkbox(trc("window.overlays_only_on_my_screen"), &overlay.onlyOnMyScreen)) g_configIsDirty = true;
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(trc("window.tooltip.only_on_my_screen"));
            }

            ImGui::Columns(2, "overlay_render", false);
            ImGui::SetColumnWidth(0, 120);
            ImGui::Text(trc("label.x"));
            ImGui::NextColumn();
            if (Spinner("##overlay_x", &overlay.x)) g_configIsDirty = true;
            ImGui::NextColumn();
            ImGui::Text(trc("label.y"));
            ImGui::NextColumn();
            if (Spinner("##overlay_y", &overlay.y)) g_configIsDirty = true;
            ImGui::NextColumn();
            ImGui::Text(trc("label.scale"));
            ImGui::NextColumn();
            if (ImGui::Checkbox((tr("mirrors.separate_x_y") + "##overlay_sep_scale").c_str(), &overlay.separateScale)) {
                g_configIsDirty = true;
                if (overlay.separateScale) { overlay.scaleX = overlay.scale; overlay.scaleY = overlay.scale; }
                else { overlay.scale = 0.5f * (overlay.scaleX + overlay.scaleY); }
            }
            ImGui::SameLine();
            if (!overlay.separateScale) {
                float scalePercent = overlay.scale * 100.0f;
                ImGui::SetNextItemWidth(180);
                if (ImGui::SliderFloat("##overlay_scale", &scalePercent, 10.0f, 200.0f, "%.0f%%")) {
                    overlay.scale = scalePercent / 100.0f;
                    g_configIsDirty = true;
                }
            } else {
                float scaleXPercent = overlay.scaleX * 100.0f;
                float scaleYPercent = overlay.scaleY * 100.0f;
                ImGui::SetNextItemWidth(90);
                if (ImGui::SliderFloat("X##overlay_scaleX", &scaleXPercent, 10.0f, 200.0f, "%.0f%%")) { overlay.scaleX = scaleXPercent / 100.0f; g_configIsDirty = true; }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(90);
                if (ImGui::SliderFloat("Y##overlay_scaleY", &scaleYPercent, 10.0f, 200.0f, "%.0f%%")) { overlay.scaleY = scaleYPercent / 100.0f; g_configIsDirty = true; }
            }
            ImGui::NextColumn();
            ImGui::Text(trc("label.relative_to"));
            ImGui::NextColumn();
            const char* current_rel_to = getFriendlyName(overlay.relativeTo, imageRelativeToOptions);
            ImGui::SetNextItemWidth(150);
            if (ImGui::BeginCombo("##overlay_rel_to", current_rel_to)) {
                for (const auto& option : imageRelativeToOptions) {
                    if (ImGui::Selectable(option.second, overlay.relativeTo == option.first)) {
                        overlay.relativeTo = option.first;
                        g_configIsDirty = true;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::Columns(1);

            ImGui::SeparatorText(trc("window.overlays_cropping"));
            ImGui::Columns(2, "overlay_crop", false);
            ImGui::SetColumnWidth(0, 120);
            ImGui::Text(trc("window.overlays_crop_top"));
            ImGui::NextColumn();
            if (Spinner("##overlay_crop_t", &overlay.crop_top, 1, 0)) g_configIsDirty = true;
            ImGui::NextColumn();
            ImGui::Text(overlay.cropToHeight ? trc("label.crop_to_height") : trc("window.overlays_crop_bottom"));
            ImGui::NextColumn();
            if (Spinner("##overlay_crop_b", &overlay.crop_bottom, 1, 0)) g_configIsDirty = true;
            ImGui::SameLine();
            if (ImGui::Checkbox(trc("label.crop_to_target_h"), &overlay.cropToHeight)) g_configIsDirty = true;
            ImGui::SameLine(); HelpMarker(trc("label.tooltip.crop_to"));
            ImGui::NextColumn();
            ImGui::Text(trc("window.overlays_crop_left"));
            ImGui::NextColumn();
            if (Spinner("##overlay_crop_l", &overlay.crop_left, 1, 0)) g_configIsDirty = true;
            ImGui::NextColumn();
            ImGui::Text(overlay.cropToWidth ? trc("label.crop_to_width") : trc("window.overlays_crop_right"));
            ImGui::NextColumn();
            if (Spinner("##overlay_crop_r", &overlay.crop_right, 1, 0)) g_configIsDirty = true;
            ImGui::SameLine();
            if (ImGui::Checkbox(trc("label.crop_to_target_w"), &overlay.cropToWidth)) g_configIsDirty = true;
            ImGui::SameLine(); HelpMarker(trc("label.tooltip.crop_to"));
            ImGui::NextColumn();
            ImGui::Columns(1);

            ImGui::SeparatorText(trc("window.overlays_capture_settings"));
            ImGui::Columns(2, "overlay_capture", false);
            ImGui::SetColumnWidth(0, 150);
            ImGui::Text(trc("label.fps"));
            ImGui::NextColumn();
            ImGui::SetNextItemWidth(250);
            if (ImGui::SliderInt("##fps", &overlay.fps, 1, 60, "%d fps")) {
                g_configIsDirty = true;
                UpdateWindowOverlayFPS(overlay.name, overlay.fps);
            }
            ImGui::NextColumn();

            ImGui::Text(trc("window.overlays_search_interval"));
            ImGui::NextColumn();
            float searchIntervalSeconds = overlay.searchInterval / 1000.0f;
            ImGui::SetNextItemWidth(250);
            if (ImGui::SliderFloat("##searchInterval", &searchIntervalSeconds, 0.5f, 5.0f, "%.1f s")) {
                overlay.searchInterval = static_cast<int>(searchIntervalSeconds * 1000.0f);
                g_configIsDirty = true;
                UpdateWindowOverlaySearchInterval(overlay.name, overlay.searchInterval);
            }
            ImGui::NextColumn();
            ImGui::Columns(1);

            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(trc("tooltip.capture_search_interval"));
            }

            ImGui::Text(trc("window.overlays_capture_method"));
            const char* captureMethods[] = { "Windows 10+", "BitBlt" };
            int currentMethodIdx = 0;
            for (int i = 0; i < 2; i++) {
                if (overlay.captureMethod == captureMethods[i]) {
                    currentMethodIdx = i;
                    break;
                }
            }
            ImGui::PushItemWidth(150.0f);
            if (ImGui::Combo("##captureMethod", &currentMethodIdx, captureMethods, 2)) {
                overlay.captureMethod = captureMethods[currentMethodIdx];
                g_configIsDirty = true;
                // Queue deferred reload to avoid blocking GUI thread
                QueueOverlayReload(overlay.name, overlay);
            }
            ImGui::PopItemWidth();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(trc("tooltip.capture_method"));
            }

            if (ImGui::Checkbox(trc("window.overlays_force_update"), &overlay.forceUpdate)) g_configIsDirty = true;
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(trc("window.overlays_tooltip_force_update"));
            }

            ImGui::SeparatorText(trc("window.overlays_interaction"));
            if (ImGui::Checkbox(trc("window.overlays_enable_interaction"), &overlay.enableInteraction)) g_configIsDirty = true;
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(trc("tooltip.capture_interaction"));
            }

            ImGui::SeparatorText(trc("window.overlays_background"));
            if (ImGui::Checkbox(trc("window.overlays_enable_background"), &overlay.background.enabled)) g_configIsDirty = true;
            ImGui::BeginDisabled(!overlay.background.enabled);
            if (ImGui::ColorEdit3(trc("window.overlays_bg_color"), &overlay.background.color.r)) g_configIsDirty = true;
            if (ImGui::SliderFloat(trc("window.overlays_bg_opacity"), &overlay.background.opacity, 0.0f, 1.0f)) g_configIsDirty = true;
            ImGui::EndDisabled();

            ImGui::SeparatorText(trc("window.overlays_color_keying"));
            if (ImGui::Checkbox(trc("window.overlays_enable_color_keying"), &overlay.enableColorKey)) g_configIsDirty = true;
            ImGui::BeginDisabled(!overlay.enableColorKey);

            int colorKeyToRemove = -1;
            for (size_t k = 0; k < overlay.colorKeys.size(); k++) {
                ImGui::PushID(static_cast<int>(k));
                auto& ck = overlay.colorKeys[k];

                ImGui::Text("Key %zu:", k + 1);
                ImGui::SameLine();
                ImGui::PushItemWidth(150.0f);
                if (ImGui::ColorEdit3("##color", &ck.color.r, ImGuiColorEditFlags_NoLabel)) g_configIsDirty = true;
                ImGui::PopItemWidth();
                ImGui::SameLine();
                ImGui::PushItemWidth(80.0f);
                if (ImGui::SliderFloat("##sens", &ck.sensitivity, 0.001f, 1.0f, "%.3f")) g_configIsDirty = true;
                ImGui::PopItemWidth();
                ImGui::SameLine();
                if (ImGui::Button("X##remove")) { colorKeyToRemove = static_cast<int>(k); }
                ImGui::PopID();
            }

            if (colorKeyToRemove >= 0) {
                overlay.colorKeys.erase(overlay.colorKeys.begin() + colorKeyToRemove);
                g_configIsDirty = true;
            }

            ImGui::BeginDisabled(overlay.colorKeys.size() >= ConfigDefaults::MAX_COLOR_KEYS);
            if (ImGui::Button(trc("window.overlays_add_color_key"))) {
                ColorKeyConfig newKey;
                newKey.color = { 0.0f, 0.0f, 0.0f };
                newKey.sensitivity = 0.05f;
                overlay.colorKeys.push_back(newKey);
                g_configIsDirty = true;
            }
            ImGui::EndDisabled();

            ImGui::EndDisabled();

            ImGui::SeparatorText(trc("window.overlays_border"));
            if (ImGui::Checkbox((tr("window.overlays_enable_border") + "##WindowOverlay").c_str(), &overlay.border.enabled)) { g_configIsDirty = true; }
            ImGui::SameLine();
            HelpMarker(trc("window.overlays_tooltip_border"));

            if (overlay.border.enabled) {
                ImGui::Text(trc("images.border_color"));
                ImVec4 borderCol = ImVec4(overlay.border.color.r, overlay.border.color.g, overlay.border.color.b, 1.0f);
                if (ImGui::ColorEdit3("##BorderColorWindowOverlay", (float*)&borderCol, ImGuiColorEditFlags_NoInputs)) {
                    overlay.border.color = { borderCol.x, borderCol.y, borderCol.z };
                    g_configIsDirty = true;
                }

                ImGui::Text(trc("images.border_width"));
                ImGui::SetNextItemWidth(100);
                if (Spinner("##BorderWidthWindowOverlay", &overlay.border.width, 1, 1, 50)) { g_configIsDirty = true; }
                ImGui::SameLine();
                ImGui::TextDisabled(trc("label.px"));

                ImGui::Text(trc("images.border_radius"));
                ImGui::SetNextItemWidth(100);
                if (Spinner("##BorderRadiusWindowOverlay", &overlay.border.radius, 1, 0, 100)) { g_configIsDirty = true; }
                ImGui::SameLine();
                ImGui::TextDisabled(trc("label.px"));
            }

            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    if (windowOverlay_to_remove >= 0) {
        std::string deletedOverlayName = g_config.windowOverlays[windowOverlay_to_remove].name;
        RemoveWindowOverlayFromCache(deletedOverlayName);
        g_config.windowOverlays.erase(g_config.windowOverlays.begin() + windowOverlay_to_remove);
        for (auto& mode : g_config.modes) {
            RemoveAllModeSources(mode, ModeSourceType::WindowOverlay, deletedOverlayName);
        }
        g_configIsDirty = true;
    }
    ImGui::Separator();
    if (ImGui::Button(trc("button.add_overlay"))) {
        WindowOverlayConfig newOverlay;
        newOverlay.name = tr("window.overlays_new_window_overlay") + " " + std::to_string(g_config.windowOverlays.size() + 1);
        newOverlay.relativeTo = "centerViewport";
        g_config.windowOverlays.push_back(newOverlay);
        g_configIsDirty = true;

        if (!g_currentModeId.empty()) {
            for (auto& mode : g_config.modes) {
                if (mode.id == g_currentModeId) {
                    AddModeSource(mode, ModeSourceType::WindowOverlay, newOverlay.name);
                    break;
                }
            }
        }
    }

    ImGui::SameLine();
    if (ImGui::Button((tr("button.reset_defaults") + "##windowoverlays").c_str())) { ImGui::OpenPopup(trc("window.overlays_reset_to_defaults")); }

    if (ImGui::BeginPopupModal(trc("window.overlays_reset_to_defaults"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f), trc("label.warning"));
        ImGui::Text(trc("window.overlays_warning_reset_to_defaults"));
        ImGui::Text(trc("label.action_cannot_be_undone"));
        ImGui::Separator();
        if (ImGui::Button(trc("button.confirm_reset"), ImVec2(120, 0))) {
            for (const auto& overlay : g_config.windowOverlays) { RemoveWindowOverlayFromCache(overlay.name); }
            g_config.windowOverlays = GetDefaultWindowOverlays();
            g_configIsDirty = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (ImGui::Button(trc("button.cancel"), ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    ImGui::EndTabItem();
}
