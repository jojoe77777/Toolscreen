if (BeginSelectableSettingsNestedTabItem(trc("ninjabrain.title"))) {
    g_currentlyEditingMirror = "";
    g_imageDragMode.store(false);
    g_windowOverlayDragMode.store(false);

    SliderCtrlClickTip();

    {
        auto& nb = g_config.ninjabrainOverlay;
        bool changed = false;

        if (nb.layoutStyle != "compact") {
            nb.layoutStyle = "compact";
            changed = true;
        }

        std::vector<NinjabrainPresetDefinition> ninjabrainPresets = GetEmbeddedNinjabrainPresets();
        std::stable_sort(ninjabrainPresets.begin(), ninjabrainPresets.end(),
                         [](const NinjabrainPresetDefinition& left, const NinjabrainPresetDefinition& right) {
                             auto presetRank = [](const std::string& presetId) {
                                 if (presetId == "compact") return 0;
                                 if (presetId == "ninjabrainbot") return 1;
                                 return 100;
                             };

                             const int leftRank = presetRank(left.id);
                             const int rightRank = presetRank(right.id);
                             if (leftRank != rightRank) {
                                 return leftRank < rightRank;
                             }

                             if (left.translationKey != right.translationKey) {
                                 return left.translationKey < right.translationKey;
                             }

                             return left.id < right.id;
                         });
        static std::string s_pendingNinjabrainPresetId;
        bool openPresetConfirm = false;

        auto applyNinjabrainPreset = [&](const NinjabrainPresetDefinition& presetDefinition) {
            const bool enabled = nb.enabled;
            const int x = nb.x;
            const int y = nb.y;
            const std::string relativeTo = nb.relativeTo;
            const std::string customFontPath = nb.customFontPath;
            const std::string apiBaseUrl = nb.apiBaseUrl;
            const std::vector<std::string> allowedModes = nb.allowedModes;
            const bool onlyOnMyScreen = nb.onlyOnMyScreen;
            const bool onlyOnObs = nb.onlyOnObs;
            NinjabrainOverlayConfig preset = presetDefinition.overlay;

            if (presetDefinition.preserveCurrentPlacement) {
                preset.enabled = enabled;
                preset.x = x;
                preset.y = y;
                preset.relativeTo = relativeTo;
                preset.customFontPath = customFontPath;
                preset.apiBaseUrl = apiBaseUrl;
                preset.allowedModes = allowedModes;
                preset.onlyOnMyScreen = onlyOnMyScreen;
                preset.onlyOnObs = onlyOnObs;
            }
            nb = std::move(preset);
            g_eyeZoomFontNeedsReload.store(true);
        };
        auto findNinjabrainPresetById = [&](const std::string& presetId) -> const NinjabrainPresetDefinition* {
            const auto presetIt = std::find_if(ninjabrainPresets.begin(), ninjabrainPresets.end(),
                                               [&](const NinjabrainPresetDefinition& presetDefinition) {
                                                   return presetDefinition.id == presetId;
                                               });
            return (presetIt != ninjabrainPresets.end()) ? &(*presetIt) : nullptr;
        };

        const std::vector<FontPickerOption> ninjabrainFontOptions = BuildFontPickerOptions();
        auto applyNinjabrainFontChange = [&]() {
            changed = true;
            g_eyeZoomFontNeedsReload.store(true);
        };

        {
            const bool wasEnabled = nb.enabled;
            changed |= ImGui::Checkbox((std::string(trc("ninjabrain.enable")) + "##nb").c_str(), &nb.enabled);
            if (nb.enabled != wasEnabled) {
                PublishConfigSnapshot();
                if (nb.enabled) {
                    StartNinjabrainClient();
                } else {
                    StopNinjabrainClientAsync();
                }
            }
        }

        if (nb.enabled) {
        ImGui::Spacing();

        const NinjabrainApiStatus apiStatus = GetNinjabrainClientStatus();
        ImVec4 statusColor = ImVec4(0.65f, 0.65f, 0.65f, 1.0f);
        const char* statusKey = "ninjabrain.status_stopped";
        switch (apiStatus.connectionState) {
        case NinjabrainApiConnectionState::Connected:
            statusColor = ImVec4(0.35f, 0.85f, 0.35f, 1.0f);
            statusKey = "ninjabrain.status_connected";
            break;
        case NinjabrainApiConnectionState::Connecting:
            statusColor = ImVec4(1.0f, 0.8f, 0.35f, 1.0f);
            statusKey = "ninjabrain.status_connecting";
            break;
        case NinjabrainApiConnectionState::Offline:
            statusColor = ImVec4(1.0f, 0.45f, 0.45f, 1.0f);
            statusKey = "ninjabrain.status_offline";
            break;
        case NinjabrainApiConnectionState::Stopped:
            break;
        }

        constexpr ImGuiTreeNodeFlags kNinjabrainSectionFlags = ImGuiTreeNodeFlags_SpanAvailWidth;
        constexpr ImGuiTreeNodeFlags kNinjabrainOpenSectionFlags = kNinjabrainSectionFlags | ImGuiTreeNodeFlags_DefaultOpen;
        constexpr float kNinjabrainLabelColumnWidth = 210.0f;
        constexpr float kNinjabrainWideLabelColumnWidth = 240.0f;
        constexpr float kNinjabrainCompactSliderWidth = 180.0f;
        constexpr float kNinjabrainMoveButtonWidth = 60.0f;

        ImGui::SeparatorText(trc("ninjabrain.api"));
        ImGui::TextDisabled("%s", trc("label.status"));
        ImGui::SameLine();
        ImGui::TextColored(statusColor, "%s", trc(statusKey));
        if (!apiStatus.apiBaseUrl.empty()) {
            ImGui::TextDisabled("%s", apiStatus.apiBaseUrl.c_str());
        }
        if (apiStatus.connectionState == NinjabrainApiConnectionState::Connecting && !apiStatus.apiBaseUrl.empty()) {
            ImGui::TextWrapped("%s", trc("ninjabrain.status_connecting_detail", apiStatus.apiBaseUrl));
        } else if (apiStatus.connectionState == NinjabrainApiConnectionState::Offline &&
                   !apiStatus.apiBaseUrl.empty() && !apiStatus.error.empty()) {
            ImGui::TextWrapped("%s", trc("ninjabrain.status_offline_detail", apiStatus.apiBaseUrl, apiStatus.error));
        }
        if (apiStatus.connectionState != NinjabrainApiConnectionState::Connected) {
            if (ImGui::Button(trc("ninjabrain.retry_button"))) {
                PublishConfigSnapshot();
                RestartNinjabrainClientAsync();
            }
            ImGui::SameLine();
            ImGui::TextWrapped("%s", trc("ninjabrain.enable_api_hint"));
        }
        ImGui::Spacing();

        {
            std::string preview = nb.allowedModes.empty() ? trc("ninjabrain.all_modes") : "";
            if (!nb.allowedModes.empty()) {
                for (size_t mi = 0; mi < nb.allowedModes.size(); ++mi) {
                    if (mi > 0) preview += ", ";
                    preview += nb.allowedModes[mi];
                }
                if (preview.size() > 40) preview = preview.substr(0, 37) + "...";
            }
            bool modesNodeOpen = ImGui::TreeNodeEx("##nbModesNode", ImGuiTreeNodeFlags_SpanAvailWidth, "Show in modes: %s", preview.c_str());
            if (modesNodeOpen) {
                bool allModes = nb.allowedModes.empty();
                if (ImGui::Checkbox((std::string(trc("ninjabrain.all_modes")) + "##nbAllModes").c_str(), &allModes)) {
                    nb.allowedModes.clear(); changed = true;
                }
                ImGui::Separator();
                for (auto& mode : g_config.modes) {
                    bool inList = false;
                    for (auto& m : nb.allowedModes) if (m == mode.id) { inList = true; break; }
                    std::string cbLabel = mode.id + "##nbMode_" + mode.id;
                    if (ImGui::Checkbox(cbLabel.c_str(), &inList)) {
                        if (inList) nb.allowedModes.push_back(mode.id);
                        else        nb.allowedModes.erase(std::remove(nb.allowedModes.begin(), nb.allowedModes.end(), mode.id), nb.allowedModes.end());
                        changed = true;
                    }
                }
                ImGui::TreePop();
            }
        }
        ImGui::Spacing();

        ImGui::SeparatorText(trc("ninjabrain.presets"));
        ImGui::TextDisabled("%s", trc("ninjabrain.presets_hint"));
        for (size_t presetIndex = 0; presetIndex < ninjabrainPresets.size(); ++presetIndex) {
            const NinjabrainPresetDefinition& presetDefinition = ninjabrainPresets[presetIndex];
            if (presetIndex > 0) {
                ImGui::SameLine();
            }
            const std::string presetButtonLabel =
                std::string(trc(presetDefinition.translationKey.c_str())) + "##nbPreset_" + presetDefinition.id;
            if (ImGui::Button(presetButtonLabel.c_str())) {
                s_pendingNinjabrainPresetId = presetDefinition.id;
                openPresetConfirm = true;
            }
        }
        if (openPresetConfirm) {
            ImGui::OpenPopup(trc("ninjabrain.preset_confirm_title"));
        }
        const float ninjabrainModalMinWidth =
            (std::clamp)(ImGui::GetWindowSize().x * 0.4f, 360.0f, 520.0f);
        SetNextSettingsModalCentered();
        ImGui::SetNextWindowSizeConstraints(ImVec2(ninjabrainModalMinWidth, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
        if (ImGui::BeginPopupModal(trc("ninjabrain.preset_confirm_title"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            const NinjabrainPresetDefinition* pendingPreset = findNinjabrainPresetById(s_pendingNinjabrainPresetId);
            if (pendingPreset == nullptr) {
                s_pendingNinjabrainPresetId.clear();
                ImGui::CloseCurrentPopup();
            } else {
                const std::string confirmMessage =
                    tr("ninjabrain.preset_confirm_message", trc(pendingPreset->translationKey.c_str()));
                ImGui::TextWrapped("%s", confirmMessage.c_str());
                ImGui::Separator();
                if (ImGui::Button(trc("button.apply"), ImVec2(120, 0))) {
                    applyNinjabrainPreset(*pendingPreset);
                    s_pendingNinjabrainPresetId.clear();
                    changed = true;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button(trc("button.cancel"), ImVec2(120, 0))) {
                    s_pendingNinjabrainPresetId.clear();
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }
        ImGui::Spacing();

        ImGui::SeparatorText(trc("ninjabrain.rendering"));
        float overlayOpacityPercent = std::clamp(nb.overlayOpacity, 0.0f, 1.0f) * 100.0f;
        ImGui::SetNextItemWidth(kNinjabrainCompactSliderWidth);
        if (ImGui::SliderFloat((std::string(trc("ninjabrain.opacity")) + "##nb").c_str(), &overlayOpacityPercent, 0.0f, 100.0f, "%.0f%%")) {
            nb.overlayOpacity = overlayOpacityPercent / 100.0f;
            changed = true;
        }
        {
            float bgOpacityPercent = (nb.bgEnabled ? std::clamp(nb.bgOpacity, 0.0f, 1.0f) : 0.0f) * 100.0f;
            ImGui::SetNextItemWidth(kNinjabrainCompactSliderWidth);
            if (ImGui::SliderFloat((std::string(trc("ninjabrain.bg_opacity")) + "##nb").c_str(), &bgOpacityPercent, 0.0f, 100.0f, "%.0f%%")) {
                nb.bgOpacity = bgOpacityPercent / 100.0f;
                nb.bgEnabled = nb.bgOpacity > 0.0f;
                changed = true;
            }
        }
        if (ImGui::Checkbox((std::string(trc("ninjabrain.only_on_my_screen")) + "##nb").c_str(), &nb.onlyOnMyScreen)) {
            if (nb.onlyOnMyScreen) nb.onlyOnObs = false;
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", trc("ninjabrain.tooltip_only_on_my_screen"));
        if (ImGui::Checkbox((std::string(trc("ninjabrain.only_on_obs")) + "##nb").c_str(), &nb.onlyOnObs)) {
            if (nb.onlyOnObs) nb.onlyOnMyScreen = false;
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", trc("ninjabrain.tooltip_only_on_obs"));
        if (ImGui::Checkbox((std::string(trc("ninjabrain.hide_if_stale")) + "##nb").c_str(), &nb.hideIfStale)) {
            changed = true;
        }
        if (nb.hideIfStale) {
            ImGui::Text("%s", trc("ninjabrain.hide_if_stale_delay"));
            ImGui::SameLine();
            if (Spinner("##nbHideIfStaleDelay", &nb.hideIfStaleDelaySeconds, 1, 1, 3600, 80.0f, 1.0f)) {
                changed = true;
            }
        }

        ImGui::Columns(2, "nb_render_cols", false);
        ImGui::SetColumnWidth(0, kNinjabrainLabelColumnWidth);

        ImGui::Text("%s", trc("ninjabrain.pos_x"));
        ImGui::NextColumn();
        if (Spinner("##nb_x", &nb.x)) changed = true;
        ImGui::NextColumn();

        ImGui::Text("%s", trc("ninjabrain.pos_y"));
        ImGui::NextColumn();
        if (Spinner("##nb_y", &nb.y)) changed = true;
        ImGui::NextColumn();

        ImGui::Text("%s", trc("ninjabrain.font"));
        ImGui::NextColumn();
        const bool usingCustomFont = RenderFontPickerCombo("##nbFontChoice", 250.0f, ninjabrainFontOptions, nb.customFontPath,
                                                           s_ninjabrainFontPickerState, applyNinjabrainFontChange);

        ImGui::NextColumn();

        if (usingCustomFont) {
            ImGui::Text("%s", trc("ninjabrain.custom_font_path"));
            ImGui::NextColumn();
            RenderCustomFontPathEditor("##nbCustomFont", "##nbCustomFont", 250.0f, ninjabrainFontOptions, nb.customFontPath,
                                       s_ninjabrainFontPickerState, "Select Font for Ninjabrain Overlay",
                                       applyNinjabrainFontChange, "ninjabrain.custom_font_hint");
            ImGui::NextColumn();
        }

        ImGui::Text("%s", trc("ninjabrain.scale"));
        ImGui::NextColumn();
        float scalePercent = nb.overlayScale * 100.0f;
        ImGui::SetNextItemWidth(250);
        if (ImGui::SliderFloat("##nbScale", &scalePercent, 5.0f, 100.0f, "%.0f%%")) {
            nb.overlayScale = scalePercent / 100.0f;
            changed = true;
            g_eyeZoomFontNeedsReload.store(true);
        }
        ImGui::NextColumn();

        ImGui::Text("%s", trc("ninjabrain.relative_to"));
        ImGui::NextColumn();
        const char* current_rel_to = getFriendlyName(nb.relativeTo, ninjabrainRelativeToOptions);
        if (strcmp(current_rel_to, "Unknown") == 0) {
            if (nb.relativeTo == "topLeftViewport") {
                current_rel_to = getFriendlyName("topLeftScreen", ninjabrainRelativeToOptions);
            } else if (nb.relativeTo == "topRightViewport") {
                current_rel_to = getFriendlyName("topRightScreen", ninjabrainRelativeToOptions);
            } else if (nb.relativeTo == "bottomLeftViewport") {
                current_rel_to = getFriendlyName("bottomLeftScreen", ninjabrainRelativeToOptions);
            } else if (nb.relativeTo == "bottomRightViewport") {
                current_rel_to = getFriendlyName("bottomRightScreen", ninjabrainRelativeToOptions);
            } else if (nb.relativeTo == "centerViewport") {
                current_rel_to = getFriendlyName("centerScreen", ninjabrainRelativeToOptions);
            }
        }
        ImGui::SetNextItemWidth(210);
        if (ImGui::BeginCombo("##nb_rel_to", current_rel_to)) {
            for (const auto& option : ninjabrainRelativeToOptions) {
                if (ImGui::Selectable(option.second, nb.relativeTo == option.first)) {
                    nb.relativeTo = option.first; changed = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::NextColumn();

        ImGui::Columns(1);
        ImGui::Spacing();

        auto renderNinjabrainAppearanceSection = [&]() {
            ImGui::SeparatorText(trc("ninjabrain.appearance"));
            ImGui::Columns(2, "nb_appearance_render_cols", false);
            ImGui::SetColumnWidth(0, kNinjabrainLabelColumnWidth);

            ImGui::Text("%s", trc("ninjabrain.predictions"));
            ImGui::NextColumn();
            if (Spinner("##nbShownPreds", &nb.shownPredictions, 1, 1, 5)) changed = true;
            ImGui::NextColumn();

            ImGui::Text("%s", trc("ninjabrain.eye_throw_rows"));
            ImGui::NextColumn();
            if (Spinner("##nbEyeThrowRowsAppearance", &nb.eyeThrowRows, 1, 1, static_cast<int>(kNinjabrainThrowLimit))) changed = true;
            ImGui::NextColumn();

            ImGui::Text("%s", trc("ninjabrain.coords_display"));
            ImGui::NextColumn();
            {
                const char* currentCoordsDisplay = (nb.coordsDisplay == "chunk")
                    ? trc("ninjabrain.coords_display_chunk")
                    : trc("ninjabrain.coords_display_block");
                ImGui::SetNextItemWidth(180.0f);
                if (ImGui::BeginCombo("##nbCoordsDisplay", currentCoordsDisplay)) {
                    const bool coordsAreChunk = (nb.coordsDisplay == "chunk");
                    if (ImGui::Selectable(trc("ninjabrain.coords_display_block"), !coordsAreChunk)) {
                        nb.coordsDisplay = "block";
                        for (auto& col : nb.columns) {
                            if (col.id == "coords" && (col.header.empty() || col.header == "Chunk")) {
                                col.header = "Location";
                                break;
                            }
                        }
                        changed = true;
                    }
                    if (ImGui::Selectable(trc("ninjabrain.coords_display_chunk"), coordsAreChunk)) {
                        nb.coordsDisplay = "chunk";
                        for (auto& col : nb.columns) {
                            if (col.id == "coords" && (col.header.empty() || col.header == "Location")) {
                                col.header = "Chunk";
                                break;
                            }
                        }
                        changed = true;
                    }
                    ImGui::EndCombo();
                }
            }
            ImGui::NextColumn();

            ImGui::Text("%s", trc("ninjabrain.outline_width"));
            ImGui::NextColumn();
            if (Spinner("##nbOutline", &nb.outlineWidth, 1, 0, 5)) changed = true;
            ImGui::NextColumn();

            ImGui::Text("%s", trc("ninjabrain.text_outline_color"));
            ImGui::NextColumn();
            ImGui::SetNextItemWidth(250);
            if (ImGui::ColorEdit4("##nbOutlineColor", &nb.outlineColor.r,
                                  ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf)) changed = true;
            ImGui::NextColumn();

            ImGui::Columns(1);

            if (ImGui::Checkbox((std::string(trc("ninjabrain.always_show")) + "##nb").c_str(), &nb.alwaysShow)) changed = true;
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", trc("ninjabrain.tooltip_always_show"));
            }
            if (ImGui::Checkbox((std::string(trc("ninjabrain.show_direction_to_stronghold")) + "##nb").c_str(), &nb.showDirectionToStronghold)) changed = true;
            if (ImGui::Checkbox((std::string(trc("ninjabrain.show_boat_state_top_bar")) + "##nb").c_str(), &nb.showBoatStateInTopBar)) changed = true;

            if (nb.showBoatStateInTopBar) {
                ImGui::Columns(2, "nb_boat_header_cols", false);
                ImGui::SetColumnWidth(0, kNinjabrainLabelColumnWidth);

                ImGui::Text("%s", trc("ninjabrain.boat_state_size"));
                ImGui::NextColumn();
                ImGui::SetNextItemWidth(250);
                if (ImGui::SliderFloat("##nbBoatStateSize", &nb.boatStateSize, 8.0f, 96.0f, "%.0f px")) changed = true;
                ImGui::NextColumn();

                ImGui::Text("%s", trc("ninjabrain.boat_state_margin_right"));
                ImGui::NextColumn();
                ImGui::SetNextItemWidth(250);
                if (ImGui::SliderFloat("##nbBoatStateMarginRight", &nb.boatStateMarginRight, 0.0f, 96.0f, "%.0f px")) changed = true;
                ImGui::NextColumn();

                ImGui::Columns(1);
            }

            ImGui::SeparatorText(trc("ninjabrain.background"));
            ImGui::Columns(2, "nb_appearance_background_cols", false);
            ImGui::SetColumnWidth(0, kNinjabrainLabelColumnWidth);

            ImGui::Text("%s", trc("ninjabrain.border_width"));
            ImGui::NextColumn();
            if (Spinner("##nbBorderWidth", &nb.borderWidth, 1, 0, 8)) changed = true;
            ImGui::NextColumn();

            ImGui::Text("%s", trc("ninjabrain.border_radius"));
            ImGui::NextColumn();
            ImGui::SetNextItemWidth(250);
            if (ImGui::SliderFloat("##nbBorderRadius", &nb.borderRadius, 0.0f, 32.0f, "%.0f px")) changed = true;
            ImGui::NextColumn();

            ImGui::Text("%s", trc("ninjabrain.corner_radius"));
            ImGui::NextColumn();
            ImGui::SetNextItemWidth(250);
            if (ImGui::SliderFloat("##nbCornerRadius", &nb.cornerRadius, 0.0f, 32.0f, "%.0f px")) changed = true;
            ImGui::NextColumn();

            ImGui::Columns(1);

            if (ImGui::CollapsingHeader(trc("ninjabrain.colors"), kNinjabrainSectionFlags)) {
                if (ImGui::ColorEdit4((std::string(trc("ninjabrain.bg_color")) + "##nb").c_str(), &nb.bgColor.r, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf)) changed = true;
                if (ImGui::ColorEdit4((std::string(trc("ninjabrain.header_fill")) + "##nb").c_str(), &nb.headerFillColor.r, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf)) changed = true;
                if (ImGui::ColorEdit4((std::string(trc("ninjabrain.throws_background")) + "##nb").c_str(), &nb.throwsBackgroundColor.r, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf)) changed = true;
                if (ImGui::ColorEdit4((std::string(trc("ninjabrain.border_color")) + "##nb").c_str(), &nb.borderColor.r, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf)) changed = true;
                if (ImGui::ColorEdit4((std::string(trc("ninjabrain.divider_color")) + "##nb").c_str(), &nb.dividerColor.r, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf)) changed = true;
                if (ImGui::ColorEdit4((std::string(trc("ninjabrain.header_divider_color")) + "##nb").c_str(), &nb.headerDividerColor.r, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf)) changed = true;
                if (ImGui::ColorEdit4((std::string(trc("ninjabrain.color_headers")) + "##nb").c_str(), &nb.textColor.r, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf)) changed = true;
                if (ImGui::ColorEdit4((std::string(trc("ninjabrain.color_data")) + "##nb").c_str(), &nb.dataColor.r, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf)) changed = true;
                if (ImGui::ColorEdit4((std::string(trc("ninjabrain.throws_text_color")) + "##nb").c_str(), &nb.throwsTextColor.r, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf)) changed = true;
                if (ImGui::ColorEdit4((std::string(trc("ninjabrain.divine_text_color")) + "##nb").c_str(), &nb.divineTextColor.r, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf)) changed = true;
                if (ImGui::ColorEdit4((std::string(trc("ninjabrain.coord_positive_color")) + "##nb").c_str(), &nb.coordPositiveColor.r, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf)) changed = true;
                if (ImGui::ColorEdit4((std::string(trc("ninjabrain.coord_negative_color")) + "##nb").c_str(), &nb.coordNegativeColor.r, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf)) changed = true;
                if (ImGui::ColorEdit4((std::string(trc("ninjabrain.subpixel_positive_color")) + "##nb").c_str(), &nb.subpixelPositiveColor.r, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf)) changed = true;
                if (ImGui::ColorEdit4((std::string(trc("ninjabrain.subpixel_negative_color")) + "##nb").c_str(), &nb.subpixelNegativeColor.r, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf)) changed = true;
                if (ImGui::ColorEdit4((std::string(trc("ninjabrain.certainty_color")) + "##nb").c_str(), &nb.certaintyColor.r, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf)) changed = true;
                if (ImGui::ColorEdit4((std::string(trc("ninjabrain.certainty_mid_color")) + "##nb").c_str(), &nb.certaintyMidColor.r, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf)) changed = true;
                if (ImGui::ColorEdit4((std::string(trc("ninjabrain.certainty_low_color")) + "##nb").c_str(), &nb.certaintyLowColor.r, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf)) changed = true;
            }

            if (ImGui::CollapsingHeader(trc("ninjabrain.columns"), kNinjabrainSectionFlags)) {
                if (ImGui::BeginTable("##nbCols", 5,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
                {
                    ImGui::TableSetupColumn("#",      ImGuiTableColumnFlags_WidthFixed,   22.f);
                    ImGui::TableSetupColumn("Show",   ImGuiTableColumnFlags_WidthFixed,   44.f);
                    ImGui::TableSetupColumn("Header", ImGuiTableColumnFlags_WidthFixed, 148.f);
                    ImGui::TableSetupColumn(trc("label.width"), ImGuiTableColumnFlags_WidthFixed, 112.f);
                    ImGui::TableSetupColumn("Order",  ImGuiTableColumnFlags_WidthFixed,  128.f);
                    ImGui::TableHeadersRow();

                    int moveFrom = -1, moveTo = -1;
                    for (int ci = 0; ci < (int)nb.columns.size(); ci++) {
                        auto& col = nb.columns[ci];
                        ImGui::TableNextRow();
                        ImGui::PushID(ci);

                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextDisabled("%d", ci + 1);

                        ImGui::TableSetColumnIndex(1);
                        bool show = col.show;
                        if (ImGui::Checkbox("##show", &show)) { col.show = show; changed = true; }

                        ImGui::TableSetColumnIndex(2);
                        char buf[32]; strncpy_s(buf, col.header.c_str(), sizeof(buf) - 1);
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::InputText("##hdr", buf, sizeof(buf))) { col.header = buf; changed = true; }

                        ImGui::TableSetColumnIndex(3);
                        ImGui::BeginDisabled(!nb.staticColumnWidths);
                        int staticWidth = col.staticWidth;
                        if (Spinner("##width", &staticWidth, 1, 0, 2000, 40.0f, 1.0f)) {
                            col.staticWidth = staticWidth;
                            changed = true;
                        }
                        ImGui::EndDisabled();

                        ImGui::TableSetColumnIndex(4);
                        if (ci == 0) ImGui::BeginDisabled();
                        if (ImGui::Button("Up##col", ImVec2(kNinjabrainMoveButtonWidth, 0.0f))) { moveFrom = ci; moveTo = ci - 1; changed = true; }
                        if (ci == 0) ImGui::EndDisabled();
                        ImGui::SameLine(0.0f, 4.0f);
                        if (ci == (int)nb.columns.size() - 1) ImGui::BeginDisabled();
                        if (ImGui::Button("Down##col", ImVec2(kNinjabrainMoveButtonWidth, 0.0f))) { moveFrom = ci; moveTo = ci + 1; changed = true; }
                        if (ci == (int)nb.columns.size() - 1) ImGui::EndDisabled();

                        ImGui::PopID();
                    }
                    if (moveFrom >= 0 && moveTo >= 0 && moveTo < (int)nb.columns.size())
                        std::swap(nb.columns[moveFrom], nb.columns[moveTo]);

                    ImGui::EndTable();
                }
            }
        };

        renderNinjabrainAppearanceSection();

        if (ImGui::CollapsingHeader(trc("ninjabrain.layout"), kNinjabrainOpenSectionFlags)) {
            if (ImGui::Checkbox((std::string(trc("ninjabrain.show_throw_details")) + "##nbLayout").c_str(), &nb.showThrowDetails)) changed = true;
            if (ImGui::Checkbox((std::string(trc("ninjabrain.static_column_widths")) + "##nbLayout").c_str(), &nb.staticColumnWidths)) changed = true;

            ImGui::Columns(2, "nb_layout_cols", false);
            ImGui::SetColumnWidth(0, kNinjabrainLabelColumnWidth);

            ImGui::Text("%s", trc("ninjabrain.section_layout_mode"));
            ImGui::NextColumn();
            {
                const bool manualSectionLayout = nb.sectionLayoutMode == "manual";
                const char* layoutModePreview = manualSectionLayout
                    ? trc("ninjabrain.section_layout_manual")
                    : trc("ninjabrain.section_layout_flow");
                ImGui::SetNextItemWidth(250);
                if (ImGui::BeginCombo("##nbSectionLayoutMode", layoutModePreview)) {
                    if (ImGui::Selectable(trc("ninjabrain.section_layout_flow"), !manualSectionLayout)) {
                        nb.sectionLayoutMode = "flow";
                        changed = true;
                    }
                    if (ImGui::Selectable(trc("ninjabrain.section_layout_manual"), manualSectionLayout)) {
                        nb.sectionLayoutMode = "manual";
                        changed = true;
                    }
                    ImGui::EndCombo();
                }
            }
            ImGui::NextColumn();

            ImGui::Text("%s", trc("ninjabrain.side_padding"));
            ImGui::NextColumn();
            ImGui::SetNextItemWidth(250);
            if (ImGui::SliderFloat("##nbSidePadding", &nb.sidePadding, 0.0f, 300.0f, "%.0f px")) changed = true;
            ImGui::NextColumn();

            ImGui::Text("%s", trc("ninjabrain.content_padding_top"));
            ImGui::NextColumn();
            ImGui::SetNextItemWidth(250);
            if (ImGui::SliderFloat("##nbContentPaddingTop", &nb.contentPaddingTop, 0.0f, 300.0f, "%.0f px")) changed = true;
            ImGui::NextColumn();

            ImGui::Text("%s", trc("ninjabrain.content_padding_bottom"));
            ImGui::NextColumn();
            ImGui::SetNextItemWidth(250);
            if (ImGui::SliderFloat("##nbContentPaddingBottom", &nb.contentPaddingBottom, 0.0f, 300.0f, "%.0f px")) changed = true;
            ImGui::NextColumn();

            ImGui::Columns(1);
        }

        if (ImGui::CollapsingHeader(trc("ninjabrain.section_results"), kNinjabrainSectionFlags)) {
            ImGui::Columns(2, "nb_results_layout_cols", false);
            ImGui::SetColumnWidth(0, kNinjabrainWideLabelColumnWidth);

            ImGui::Text("%s", trc("ninjabrain.results_margin_left"));
            ImGui::NextColumn();
            ImGui::SetNextItemWidth(250);
            if (ImGui::SliderFloat("##nbResultsMarginLeft", &nb.resultsMarginLeft, 0.0f, 300.0f, "%.0f px")) changed = true;
            ImGui::NextColumn();

            ImGui::Text("%s", trc("ninjabrain.results_margin_right"));
            ImGui::NextColumn();
            ImGui::SetNextItemWidth(250);
            if (ImGui::SliderFloat("##nbResultsMarginRight", &nb.resultsMarginRight, 0.0f, 300.0f, "%.0f px")) changed = true;
            ImGui::NextColumn();

            ImGui::Text("%s", trc("ninjabrain.results_margin_top"));
            ImGui::NextColumn();
            ImGui::SetNextItemWidth(250);
            if (ImGui::SliderFloat("##nbResultsMarginTop", &nb.resultsMarginTop, 0.0f, 300.0f, "%.0f px")) changed = true;
            ImGui::NextColumn();

            ImGui::Text("%s", trc("ninjabrain.results_margin_bottom"));
            ImGui::NextColumn();
            ImGui::SetNextItemWidth(250);
            if (ImGui::SliderFloat("##nbResultsMarginBottom", &nb.resultsMarginBottom, 0.0f, 300.0f, "%.0f px")) changed = true;
            ImGui::NextColumn();

            ImGui::Text("%s", trc("ninjabrain.results_header_padding_y"));
            ImGui::NextColumn();
            ImGui::SetNextItemWidth(250);
            if (ImGui::SliderFloat("##nbResultsHeaderPaddingY", &nb.resultsHeaderPaddingY, 0.0f, 32.0f, "%.0f px")) changed = true;
            ImGui::NextColumn();

            ImGui::Text("%s", trc("ninjabrain.row_spacing"));
            ImGui::NextColumn();
            ImGui::SetNextItemWidth(250);
            if (ImGui::SliderFloat("##nbRowSpacing", &nb.rowSpacing, 0.0f, 30.0f, "%.0f px")) changed = true;
            ImGui::NextColumn();

            ImGui::Text("%s", trc("ninjabrain.results_column_gap"));
            ImGui::NextColumn();
            ImGui::SetNextItemWidth(250);
            if (ImGui::SliderFloat("##nbResultsColumnGap", &nb.resultsColumnGap, 0.0f, 80.0f, "%.0f px")) changed = true;
            ImGui::NextColumn();

            ImGui::Columns(1);
        }

        if (nb.sectionLayoutMode == "manual" && ImGui::CollapsingHeader(trc("ninjabrain.manual_sections"), kNinjabrainSectionFlags)) {
            auto renderSectionAnchorCombo = [&](const char* id, std::string& anchor) {
                const char* preview = trc("position.top_left");
                if (anchor == "topRight") {
                    preview = trc("position.top_right");
                } else if (anchor == "bottomLeft") {
                    preview = trc("position.bottom_left");
                } else if (anchor == "bottomRight") {
                    preview = trc("position.bottom_right");
                }

                if (ImGui::BeginCombo(id, preview)) {
                    if (ImGui::Selectable(trc("position.top_left"), anchor == "topLeft")) {
                        anchor = "topLeft";
                        changed = true;
                    }
                    if (ImGui::Selectable(trc("position.top_right"), anchor == "topRight")) {
                        anchor = "topRight";
                        changed = true;
                    }
                    if (ImGui::Selectable(trc("position.bottom_left"), anchor == "bottomLeft")) {
                        anchor = "bottomLeft";
                        changed = true;
                    }
                    if (ImGui::Selectable(trc("position.bottom_right"), anchor == "bottomRight")) {
                        anchor = "bottomRight";
                        changed = true;
                    }
                    ImGui::EndCombo();
                }
            };
            if (ImGui::BeginTable("##nbManualSections", 5,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn(trc("label.section"), ImGuiTableColumnFlags_WidthStretch, 1.6f);
                ImGui::TableSetupColumn(trc("ninjabrain.manual_section_anchor"), ImGuiTableColumnFlags_WidthStretch, 1.5f);
                ImGui::TableSetupColumn(trc("ninjabrain.manual_section_offset_x"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn(trc("ninjabrain.manual_section_offset_y"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn(trc("ninjabrain.manual_section_draw_order"), ImGuiTableColumnFlags_WidthStretch, 0.9f);
                ImGui::TableHeadersRow();

                auto renderManualSectionRow = [&](const char* label, const char* idSuffix, std::string& anchor,
                                                  float& offsetX, float& offsetY, int& drawOrder) {
                    ImGui::TableNextRow();
                    ImGui::PushID(idSuffix);

                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(label);

                    ImGui::TableSetColumnIndex(1);
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    renderSectionAnchorCombo("##anchor", anchor);

                    ImGui::TableSetColumnIndex(2);
                    if (SpinnerFloat("##offsetX", &offsetX, 1.0f, 0.0f, 1000.0f, "%.0f")) changed = true;

                    ImGui::TableSetColumnIndex(3);
                    if (SpinnerFloat("##offsetY", &offsetY, 1.0f, 0.0f, 1000.0f, "%.0f")) changed = true;

                    ImGui::TableSetColumnIndex(4);
                    if (Spinner("##drawOrder", &drawOrder, 1, 0, 32, 48.0f, 1.0f)) changed = true;

                    ImGui::PopID();
                };

                renderManualSectionRow(trc("ninjabrain.section_results"), "results", nb.resultsAnchor,
                                       nb.resultsOffsetX, nb.resultsOffsetY, nb.resultsDrawOrder);
                renderManualSectionRow(trc("ninjabrain.section_information_messages"), "info", nb.informationMessagesAnchor,
                                       nb.informationMessagesOffsetX, nb.informationMessagesOffsetY, nb.informationMessagesDrawOrder);
                renderManualSectionRow(trc("ninjabrain.section_throws"), "throws", nb.throwsAnchor,
                                       nb.throwsOffsetX, nb.throwsOffsetY, nb.throwsDrawOrder);
                renderManualSectionRow(trc("ninjabrain.section_failed_result"), "failure", nb.failureAnchor,
                                       nb.failureOffsetX, nb.failureOffsetY, nb.failureDrawOrder);
                renderManualSectionRow(trc("ninjabrain.section_blind_result"), "blind", nb.blindAnchor,
                                       nb.blindOffsetX, nb.blindOffsetY, nb.blindDrawOrder);

                ImGui::EndTable();
            }
        }

        if (ImGui::CollapsingHeader(trc("ninjabrain.information_messages"), kNinjabrainSectionFlags)) {
            ImGui::Columns(2, "nb_info_message_cols", false);
            ImGui::SetColumnWidth(0, kNinjabrainLabelColumnWidth);

            ImGui::BeginDisabled(nb.sectionLayoutMode == "manual");
            ImGui::Text("%s", trc("ninjabrain.info_messages_placement"));
            ImGui::NextColumn();
            {
                const char* placementPreview = trc("ninjabrain.info_messages_middle");
                if (nb.informationMessagesPlacement == "top") {
                    placementPreview = trc("ninjabrain.info_messages_top");
                } else if (nb.informationMessagesPlacement == "bottom") {
                    placementPreview = trc("ninjabrain.info_messages_bottom");
                }

                ImGui::SetNextItemWidth(250);
                if (ImGui::BeginCombo("##nbInfoMessagesPlacement", placementPreview)) {
                    const bool isTop = nb.informationMessagesPlacement == "top";
                    const bool isMiddle = nb.informationMessagesPlacement == "middle";
                    const bool isBottom = nb.informationMessagesPlacement == "bottom";
                    if (ImGui::Selectable(trc("ninjabrain.info_messages_top"), isTop)) {
                        nb.informationMessagesPlacement = "top";
                        changed = true;
                    }
                    if (ImGui::Selectable(trc("ninjabrain.info_messages_middle"), isMiddle)) {
                        nb.informationMessagesPlacement = "middle";
                        changed = true;
                    }
                    if (ImGui::Selectable(trc("ninjabrain.info_messages_bottom"), isBottom)) {
                        nb.informationMessagesPlacement = "bottom";
                        changed = true;
                    }
                    ImGui::EndCombo();
                }
            }
            ImGui::NextColumn();
            ImGui::EndDisabled();

            ImGui::Text("%s", trc("ninjabrain.info_messages_font_size"));
            ImGui::NextColumn();
            {
                const float baseFontSize = (std::max)(1.0f, kNinjabrainOverlayBaseFontSize * ((nb.overlayScale > 0.01f) ? nb.overlayScale : 1.0f));
                float infoFontSize = baseFontSize * nb.informationMessagesFontScale;
                ImGui::SetNextItemWidth(250);
                if (ImGui::SliderFloat("##nbInfoMessagesFontSize", &infoFontSize, 12.0f, 160.0f, "%.0f px")) {
                    nb.informationMessagesFontScale = infoFontSize / baseFontSize;
                    changed = true;
                }
            }
            ImGui::NextColumn();

            ImGui::Text("%s", trc("ninjabrain.info_messages_icon_size"));
            ImGui::NextColumn();
            {
                const float baseFontSize = (std::max)(1.0f, kNinjabrainOverlayBaseFontSize * ((nb.overlayScale > 0.01f) ? nb.overlayScale : 1.0f));
                const float baseIconSize = (std::max)(1.0f, baseFontSize * nb.informationMessagesFontScale * 1.2f * 0.82f);
                float infoIconSize = baseIconSize * nb.informationMessagesIconScale;
                ImGui::SetNextItemWidth(250);
                if (ImGui::SliderFloat("##nbInfoMessagesIconSize", &infoIconSize, 8.0f, 160.0f, "%.0f px")) {
                    nb.informationMessagesIconScale = infoIconSize / baseIconSize;
                    changed = true;
                }
            }
            ImGui::NextColumn();

            ImGui::Text("%s", trc("ninjabrain.info_messages_margin_left"));
            ImGui::NextColumn();
            ImGui::SetNextItemWidth(250);
            if (ImGui::SliderFloat("##nbInfoMessagesMarginLeft", &nb.informationMessagesMarginLeft, 0.0f, 300.0f, "%.0f px")) changed = true;
            ImGui::NextColumn();

            ImGui::Text("%s", trc("ninjabrain.info_messages_margin_right"));
            ImGui::NextColumn();
            ImGui::SetNextItemWidth(250);
            if (ImGui::SliderFloat("##nbInfoMessagesMarginRight", &nb.informationMessagesMarginRight, 0.0f, 300.0f, "%.0f px")) changed = true;
            ImGui::NextColumn();

            ImGui::Text("%s", trc("ninjabrain.info_messages_margin_top"));
            ImGui::NextColumn();
            ImGui::SetNextItemWidth(250);
            if (ImGui::SliderFloat("##nbInfoMessagesMarginTop", &nb.informationMessagesMarginTop, 0.0f, 300.0f, "%.0f px")) changed = true;
            ImGui::NextColumn();

            ImGui::Text("%s", trc("ninjabrain.info_messages_margin_bottom"));
            ImGui::NextColumn();
            ImGui::SetNextItemWidth(250);
            if (ImGui::SliderFloat("##nbInfoMessagesMarginBottom", &nb.informationMessagesMarginBottom, 0.0f, 300.0f, "%.0f px")) changed = true;
            ImGui::NextColumn();

            ImGui::Text("%s", trc("ninjabrain.info_messages_icon_text_margin"));
            ImGui::NextColumn();
            ImGui::SetNextItemWidth(250);
            if (ImGui::SliderFloat("##nbInfoMessagesIconTextMargin", &nb.informationMessagesIconTextMargin, 0.0f, 48.0f, "%.0f px")) changed = true;
            ImGui::NextColumn();

            ImGui::Columns(1);
        }

        if (ImGui::CollapsingHeader(trc("ninjabrain.throws_layout"), kNinjabrainSectionFlags)) {
            ImGui::Columns(2, "nb_throws_layout_cols", false);
            ImGui::SetColumnWidth(0, kNinjabrainWideLabelColumnWidth);

            ImGui::Text("%s", trc("ninjabrain.throws_margin_left"));
            ImGui::NextColumn();
            ImGui::SetNextItemWidth(250);
            if (ImGui::SliderFloat("##nbThrowsMarginLeft", &nb.throwsMarginLeft, 0.0f, 300.0f, "%.0f px")) changed = true;
            ImGui::NextColumn();

            ImGui::Text("%s", trc("ninjabrain.throws_margin_right"));
            ImGui::NextColumn();
            ImGui::SetNextItemWidth(250);
            if (ImGui::SliderFloat("##nbThrowsMarginRight", &nb.throwsMarginRight, 0.0f, 300.0f, "%.0f px")) changed = true;
            ImGui::NextColumn();

            ImGui::Text("%s", trc("ninjabrain.throws_margin_top"));
            ImGui::NextColumn();
            ImGui::SetNextItemWidth(250);
            if (ImGui::SliderFloat("##nbThrowsMarginTop", &nb.throwsMarginTop, 0.0f, 300.0f, "%.0f px")) changed = true;
            ImGui::NextColumn();

            ImGui::Text("%s", trc("ninjabrain.throws_margin_bottom"));
            ImGui::NextColumn();
            ImGui::SetNextItemWidth(250);
            if (ImGui::SliderFloat("##nbThrowsMarginBottom", &nb.throwsMarginBottom, 0.0f, 300.0f, "%.0f px")) changed = true;
            ImGui::NextColumn();

            ImGui::Text("%s", trc("ninjabrain.throws_header_padding_y"));
            ImGui::NextColumn();
            ImGui::SetNextItemWidth(250);
            if (ImGui::SliderFloat("##nbThrowsHeaderPaddingY", &nb.throwsHeaderPaddingY, 0.0f, 32.0f, "%.0f px")) changed = true;
            ImGui::NextColumn();

            ImGui::Text("%s", trc("ninjabrain.throws_row_padding_y"));
            ImGui::NextColumn();
            ImGui::SetNextItemWidth(250);
            if (ImGui::SliderFloat("##nbThrowsRowPaddingY", &nb.throwsRowPaddingY, 0.0f, 32.0f, "%.0f px")) changed = true;
            ImGui::NextColumn();

            ImGui::Columns(1);
        }

        if (ImGui::CollapsingHeader(trc("ninjabrain.failure_layout"), kNinjabrainSectionFlags)) {
            ImGui::Columns(2, "nb_failure_layout_cols", false);
            ImGui::SetColumnWidth(0, kNinjabrainLabelColumnWidth);

            ImGui::Text("%s", trc("ninjabrain.failure_margin_left"));
            ImGui::NextColumn();
            ImGui::SetNextItemWidth(250);
            if (ImGui::SliderFloat("##nbFailureMarginLeft", &nb.failureMarginLeft, 0.0f, 300.0f, "%.0f px")) changed = true;
            ImGui::NextColumn();

            ImGui::Text("%s", trc("ninjabrain.failure_margin_right"));
            ImGui::NextColumn();
            ImGui::SetNextItemWidth(250);
            if (ImGui::SliderFloat("##nbFailureMarginRight", &nb.failureMarginRight, 0.0f, 300.0f, "%.0f px")) changed = true;
            ImGui::NextColumn();

            ImGui::Text("%s", trc("ninjabrain.failure_margin_top"));
            ImGui::NextColumn();
            ImGui::SetNextItemWidth(250);
            if (ImGui::SliderFloat("##nbFailureMarginTop", &nb.failureMarginTop, 0.0f, 300.0f, "%.0f px")) changed = true;
            ImGui::NextColumn();

            ImGui::Text("%s", trc("ninjabrain.failure_margin_bottom"));
            ImGui::NextColumn();
            ImGui::SetNextItemWidth(250);
            if (ImGui::SliderFloat("##nbFailureMarginBottom", &nb.failureMarginBottom, 0.0f, 300.0f, "%.0f px")) changed = true;
            ImGui::NextColumn();

            ImGui::Text("%s", trc("ninjabrain.failure_line_gap"));
            ImGui::NextColumn();
            ImGui::SetNextItemWidth(250);
            if (ImGui::SliderFloat("##nbFailureLineGap", &nb.failureLineGap, 0.0f, 64.0f, "%.0f px")) changed = true;
            ImGui::NextColumn();

            ImGui::Columns(1);
        }

        if (ImGui::CollapsingHeader(trc("ninjabrain.blind_layout"), kNinjabrainSectionFlags)) {
            ImGui::Columns(2, "nb_blind_layout_cols", false);
            ImGui::SetColumnWidth(0, kNinjabrainLabelColumnWidth);

            ImGui::Text("%s", trc("ninjabrain.blind_margin_left"));
            ImGui::NextColumn();
            ImGui::SetNextItemWidth(250);
            if (ImGui::SliderFloat("##nbBlindMarginLeft", &nb.blindMarginLeft, 0.0f, 300.0f, "%.0f px")) changed = true;
            ImGui::NextColumn();

            ImGui::Text("%s", trc("ninjabrain.blind_margin_right"));
            ImGui::NextColumn();
            ImGui::SetNextItemWidth(250);
            if (ImGui::SliderFloat("##nbBlindMarginRight", &nb.blindMarginRight, 0.0f, 300.0f, "%.0f px")) changed = true;
            ImGui::NextColumn();

            ImGui::Text("%s", trc("ninjabrain.blind_margin_top"));
            ImGui::NextColumn();
            ImGui::SetNextItemWidth(250);
            if (ImGui::SliderFloat("##nbBlindMarginTop", &nb.blindMarginTop, 0.0f, 300.0f, "%.0f px")) changed = true;
            ImGui::NextColumn();

            ImGui::Text("%s", trc("ninjabrain.blind_margin_bottom"));
            ImGui::NextColumn();
            ImGui::SetNextItemWidth(250);
            if (ImGui::SliderFloat("##nbBlindMarginBottom", &nb.blindMarginBottom, 0.0f, 300.0f, "%.0f px")) changed = true;
            ImGui::NextColumn();

            ImGui::Text("%s", trc("ninjabrain.blind_line_gap"));
            ImGui::NextColumn();
            ImGui::SetNextItemWidth(250);
            if (ImGui::SliderFloat("##nbBlindLineGap", &nb.blindLineGap, 0.0f, 64.0f, "%.0f px")) changed = true;
            ImGui::NextColumn();

            ImGui::Columns(1);
        }

        } // nb.enabled

        ImGui::Spacing();
        if (ImGui::Button(trc("ninjabrain.reset_button"))) {
            ImGui::OpenPopup(trc("ninjabrain.reset_title"));
        }
        const float ninjabrainResetModalMinWidth =
            (std::clamp)(ImGui::GetWindowSize().x * 0.4f, 360.0f, 520.0f);
        SetNextSettingsModalCentered();
        ImGui::SetNextWindowSizeConstraints(ImVec2(ninjabrainResetModalMinWidth, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
        if (ImGui::BeginPopupModal(trc("ninjabrain.reset_title"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("%s", trc("ninjabrain.reset_confirm"));
            ImGui::Separator();
            if (ImGui::Button(trc("button.confirm_reset"), ImVec2(120, 0))) {
                Config defaultConfig;
                if (LoadEmbeddedDefaultConfig(defaultConfig)) {
                    nb = std::move(defaultConfig.ninjabrainOverlay);
                } else {
                    nb = NinjabrainOverlayConfig{};
                }
                changed = true;
                s_pendingNinjabrainPresetId.clear();
                s_ninjabrainFontPickerState = {};
                g_eyeZoomFontNeedsReload.store(true);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(trc("button.cancel"), ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }

        if (changed) g_configIsDirty = true;
    }

    ImGui::EndTabItem();
}
