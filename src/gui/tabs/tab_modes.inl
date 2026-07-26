if (BeginSelectableSettingsTopTabItem(trc("tabs.modes"))) {
    g_currentlyEditingMirror = "";
    int mode_to_remove = -1;
    static std::string pendingDefaultModeId;

    bool resolutionSupported = IsResolutionChangeSupported(g_gameVersion);
    if (!resolutionSupported) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.7f, 0.0f, 1.0f));
        ImGui::TextWrapped("%s", trc("modes.warning.resolution_change", g_gameVersion.valid ? g_gameVersion.major : 0,
                                       g_gameVersion.valid ? g_gameVersion.minor : 0,
                                       g_gameVersion.valid ? g_gameVersion.patch : 0));
        ImGui::TextWrapped("%s", trc("modes.features_remain_functional"));
        ImGui::PopStyleColor();
        ImGui::Separator();
    }

    if (g_wmMouseMoveCount.load() > 50) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.5f, 1.0f));
        ImGui::TextWrapped("%s", trc("modes.warning.raw_input"));
        ImGui::PopStyleColor();
        ImGui::Separator();
    }

    SliderCtrlClickTip();
    const std::string g_currentModeId = GetPublishedCurrentModeId();

    auto renderBackgroundImageOptions = [&](ModeConfig& mode, const std::string& idSuffix) {
        if (mode.background.image.empty()) {
            ImGui::TextDisabled("%s", trc("modes.image_fit.no_image"));
            return;
        }
        const char* fitLabels[] = { trc("modes.image_fit.fill"), trc("modes.image_fit.fit"), trc("modes.image_fit.stretch"),
                                    trc("modes.image_fit.center"), trc("modes.image_fit.tile") };
        const char* fitValues[] = { "fill", "fit", "stretch", "center", "tile" };

        int currentFit = 0;
        for (int fitIndex = 0; fitIndex < IM_ARRAYSIZE(fitValues); ++fitIndex) {
            if (mode.background.imageFit == fitValues[fitIndex]) {
                currentFit = fitIndex;
                break;
            }
        }

        ImGui::SetNextItemWidth(160);
        if (ImGui::Combo((tr("modes.image_fit") + "##" + idSuffix).c_str(), &currentFit, fitLabels, IM_ARRAYSIZE(fitLabels))) {
            mode.background.imageFit = fitValues[currentFit];
            g_configIsDirty = true;
        }
        ImGui::SameLine();
        HelpMarker(trc("modes.tooltip.image_fit"));

        const BackgroundImageFit fit = ParseBackgroundImageFit(mode.background.imageFit);

        if (fit == BackgroundImageFit::Center) {
            float scalePercent = mode.background.imageCenterScale * 100.0f;
            ImGui::SetNextItemWidth(160);
            if (ImGui::SliderFloat((tr("modes.image_scale") + "##" + idSuffix).c_str(), &scalePercent, kBackgroundImageScaleMin * 100.0f,
                                   kBackgroundImageScaleMax * 100.0f, "%.0f%%")) {
                mode.background.imageCenterScale = scalePercent / 100.0f;
                g_configIsDirty = true;
            }
        } else if (fit == BackgroundImageFit::Tile) {
            float tilePercent = mode.background.imageTileScale * 100.0f;
            ImGui::SetNextItemWidth(160);
            if (ImGui::SliderFloat((tr("modes.image_tile_size") + "##" + idSuffix).c_str(), &tilePercent, kBackgroundImageScaleMin * 100.0f,
                                   kBackgroundImageScaleMax * 100.0f, "%.0f%%")) {
                mode.background.imageTileScale = tilePercent / 100.0f;
                g_configIsDirty = true;
            }
            ImGui::SetNextItemWidth(160);
            if (ImGui::SliderInt((tr("modes.image_tile_spacing") + "##" + idSuffix).c_str(), &mode.background.imageTileSpacing, 0,
                                 kBackgroundImageSpacingMax, "%dpx")) {
                g_configIsDirty = true;
            }
        }

        if (BackgroundImageFitShowsBackdrop(fit)) {
            ImGui::SetNextItemWidth(160);
            if (ImGui::ColorEdit3((tr("modes.background_color") + "##" + idSuffix).c_str(), &mode.background.color.r)) {
                g_configIsDirty = true;
            }
        }
    };

    auto applyManualModeDimensionChange = [&](ModeConfig& mode, bool updateWidth, int requestedValue, int minValue,
                                              const char* resizeSource) {
        int clampedValue = (std::max)(minValue, requestedValue);
        if (updateWidth) {
            if (EqualsIgnoreCase(mode.id, "Thin")) {
                clampedValue = (std::max)(330, clampedValue);
            }
            mode.width = clampedValue;
            mode.manualWidth = mode.width;
            mode.relativeWidth = -1.0f;
            mode.widthExpr.clear();
        } else {
            mode.height = clampedValue;
            mode.manualHeight = mode.height;
            mode.relativeHeight = -1.0f;
            mode.heightExpr.clear();
        }

        const bool hasRelativeWidth = (mode.relativeWidth >= 0.0f && mode.relativeWidth <= 1.0f);
        const bool hasRelativeHeight = (mode.relativeHeight >= 0.0f && mode.relativeHeight <= 1.0f);
        if (!hasRelativeWidth && !hasRelativeHeight) { mode.useRelativeSize = false; }

        if (EqualsIgnoreCase(mode.id, "EyeZoom")) {
            g_config.eyezoom.windowWidth = mode.width;
            g_config.eyezoom.windowHeight = mode.height;
            if (SyncPreemptiveModeFromEyeZoom(g_config)) { g_configIsDirty = true; }
        }

        if (EqualsIgnoreCase(mode.id, "Fullscreen")) {
            int currentClientW = GetCachedWindowWidth();
            int currentClientH = GetCachedWindowHeight();
            if (currentClientW < 1) currentClientW = 1;
            if (currentClientH < 1) currentClientH = 1;
            mode.stretch.enabled = true;
            mode.stretch.x = 0;
            mode.stretch.y = 0;
            mode.stretch.width = currentClientW;
            mode.stretch.height = currentClientH;
        }

        g_configIsDirty = true;

        const bool shouldResizeCurrentMode = (g_currentModeId == mode.id);
        const bool shouldResizePreemptiveEyeZoom = EqualsIgnoreCase(g_currentModeId, "Preemptive") && EqualsIgnoreCase(mode.id, "EyeZoom");
        if (!shouldResizeCurrentMode && !shouldResizePreemptiveEyeZoom) { return; }

        HWND hwnd = g_minecraftHwnd.load(std::memory_order_relaxed);
        if (!hwnd) { return; }

        if (shouldResizeCurrentMode) {
            RetargetActiveModeTransition(mode);
            PublishGuiConfigSnapshot();
            RequestWindowClientResize(hwnd, mode.width, mode.height, resizeSource);
            return;
        }

        if (shouldResizePreemptiveEyeZoom) {
            for (const auto& syncedMode : g_config.modes) {
                if (EqualsIgnoreCase(syncedMode.id, "Preemptive")) {
                    RetargetActiveModeTransition(syncedMode);
                    PublishGuiConfigSnapshot();
                    RequestWindowClientResize(hwnd, syncedMode.width, syncedMode.height, "gui:preemptive_eyezoom_sync");
                    break;
                }
            }
        }
    };

    auto getModeSourceTypeLabel = [&](ModeSourceType type) -> const char* {
        switch (type) {
        case ModeSourceType::Mirror:
            return trc("modes.mirrors");
        case ModeSourceType::MirrorGroup:
            return trc("modes.mirror_groups");
        case ModeSourceType::Image:
            return trc("modes.images");
        case ModeSourceType::WindowOverlay:
            return trc("modes.window_overlays");
        case ModeSourceType::BrowserOverlay:
            return trc("modes.browser_overlays");
        }

        return "Unknown";
    };

    auto getModeSourceTypeTint = [&](ModeSourceType type) {
        switch (type) {
        case ModeSourceType::Mirror:
            return ImVec4(0.38f, 0.68f, 1.00f, 1.00f);
        case ModeSourceType::MirrorGroup:
            return ImVec4(0.30f, 0.82f, 0.69f, 1.00f);
        case ModeSourceType::Image:
            return ImVec4(0.98f, 0.70f, 0.24f, 1.00f);
        case ModeSourceType::WindowOverlay:
            return ImVec4(0.82f, 0.49f, 0.97f, 1.00f);
        case ModeSourceType::BrowserOverlay:
            return ImVec4(0.95f, 0.42f, 0.52f, 1.00f);
        }

        return ImVec4(0.65f, 0.65f, 0.65f, 1.00f);
    };

    auto drawModeSourceTypeIcon = [&](ImDrawList* drawList, const ImVec2& min, const ImVec2& max, ModeSourceType type) {
        const ImVec4 tint = getModeSourceTypeTint(type);
        const ImU32 bgColor = ImGui::ColorConvertFloat4ToU32(ImVec4(tint.x, tint.y, tint.z, 0.20f));
        const ImU32 borderColor = ImGui::ColorConvertFloat4ToU32(ImVec4(tint.x, tint.y, tint.z, 0.90f));
        const ImU32 detailColor = ImGui::GetColorU32(ImGuiCol_Text);
        const float width = max.x - min.x;
        const float height = max.y - min.y;
        const float left = min.x + width * 0.22f;
        const float right = max.x - width * 0.22f;
        const float top = min.y + height * 0.22f;
        const float bottom = max.y - height * 0.22f;
        const float centerX = (min.x + max.x) * 0.5f;
        const float centerY = (min.y + max.y) * 0.5f;
        const float rounding = 5.0f;

        drawList->AddRectFilled(min, max, bgColor, rounding);
        drawList->AddRect(min, max, borderColor, rounding, 0, 1.2f);

        switch (type) {
        case ModeSourceType::Mirror:
            drawList->AddRect(ImVec2(left, top), ImVec2(right, bottom), detailColor, 3.0f, 0, 1.2f);
            drawList->AddLine(ImVec2(centerX, top + 1.5f), ImVec2(centerX, bottom - 1.5f), detailColor, 1.2f);
            drawList->AddLine(ImVec2(left + 1.5f, bottom - 2.5f), ImVec2(right - 1.5f, top + 2.5f), detailColor, 1.0f);
            break;
        case ModeSourceType::MirrorGroup:
            drawList->AddRect(ImVec2(left + 2.0f, top + 1.0f), ImVec2(right, bottom - 1.0f), detailColor, 2.5f, 0, 1.0f);
            drawList->AddRect(ImVec2(left - 1.0f, top + 3.0f), ImVec2(right - 3.0f, bottom + 1.0f), detailColor, 2.5f, 0, 1.0f);
            drawList->AddLine(ImVec2(centerX + 1.0f, top + 2.5f), ImVec2(centerX + 1.0f, bottom - 1.5f), detailColor, 1.0f);
            break;
        case ModeSourceType::Image:
            drawList->AddRect(ImVec2(left, top), ImVec2(right, bottom), detailColor, 2.5f, 0, 1.0f);
            drawList->AddCircleFilled(ImVec2(right - width * 0.14f, top + height * 0.16f), width * 0.07f, detailColor);
            drawList->AddTriangleFilled(ImVec2(left + width * 0.06f, bottom - height * 0.10f), ImVec2(centerX - width * 0.03f, top + height * 0.28f),
                                        ImVec2(centerX + width * 0.10f, bottom - height * 0.10f), detailColor);
            drawList->AddTriangleFilled(ImVec2(centerX - width * 0.03f, bottom - height * 0.10f), ImVec2(right - width * 0.16f, top + height * 0.40f),
                                        ImVec2(right - width * 0.04f, bottom - height * 0.10f), detailColor);
            break;
        case ModeSourceType::WindowOverlay:
            drawList->AddRect(ImVec2(left, top), ImVec2(right, bottom), detailColor, 2.5f, 0, 1.0f);
            drawList->AddLine(ImVec2(left, top + height * 0.18f), ImVec2(right, top + height * 0.18f), detailColor, 1.0f);
            drawList->AddLine(ImVec2(left + width * 0.20f, top + height * 0.28f), ImVec2(left + width * 0.20f, bottom - height * 0.10f), detailColor,
                              1.0f);
            drawList->AddLine(ImVec2(left + width * 0.30f, centerY), ImVec2(right - width * 0.10f, centerY), detailColor, 1.0f);
            break;
        case ModeSourceType::BrowserOverlay:
            drawList->AddRect(ImVec2(left, top), ImVec2(right, bottom), detailColor, 2.5f, 0, 1.0f);
            drawList->AddLine(ImVec2(left, top + height * 0.18f), ImVec2(right, top + height * 0.18f), detailColor, 1.0f);
            drawList->AddCircleFilled(ImVec2(left + width * 0.12f, top + height * 0.10f), width * 0.04f, detailColor);
            drawList->AddCircleFilled(ImVec2(left + width * 0.22f, top + height * 0.10f), width * 0.04f, detailColor);
            drawList->AddCircleFilled(ImVec2(left + width * 0.32f, top + height * 0.10f), width * 0.04f, detailColor);
            drawList->AddLine(ImVec2(left + width * 0.12f, centerY), ImVec2(right - width * 0.12f, centerY), detailColor, 1.0f);
            drawList->AddLine(ImVec2(left + width * 0.12f, centerY + height * 0.16f), ImVec2(right - width * 0.28f, centerY + height * 0.16f), detailColor,
                              1.0f);
            break;
        }
    };

    auto drawModeSourceTypeIconAt = [&](const ImVec2& min, float iconSize, ModeSourceType type) {
        drawModeSourceTypeIcon(ImGui::GetWindowDrawList(), min, ImVec2(min.x + iconSize, min.y + iconSize), type);
    };

    auto renderModeSourceTypeIcon = [&](const ModeSourceRef& source, const char* id) {
        const float iconSize = ImGui::GetFrameHeight();
        const ImVec2 iconMin = ImGui::GetCursorScreenPos();
        const ImVec2 iconMax(iconMin.x + iconSize, iconMin.y + iconSize);

        ImGui::InvisibleButton(id, ImVec2(iconSize, iconSize));
        drawModeSourceTypeIcon(ImGui::GetWindowDrawList(), iconMin, iconMax, source.type);

        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", getModeSourceTypeLabel(source.type)); }
    };

    auto renderModeSourceDragHandle = [&](const char* id, float width, float height) {
        const ImVec2 handleMin = ImGui::GetCursorScreenPos();
        const ImVec2 handleSize(width, height);
        ImGui::InvisibleButton(id, handleSize);

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImU32 dotColor = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        const float radius = 1.4f;
        const float columnGap = width * 0.42f;
        const float firstX = handleMin.x + width * 0.28f;
        const float secondX = firstX + columnGap;
        const float topY = handleMin.y + height * 0.26f;
        const float rowGap = height * 0.24f;
        for (int row = 0; row < 3; ++row) {
            const float y = topY + rowGap * row;
            drawList->AddCircleFilled(ImVec2(firstX, y), radius, dotColor);
            drawList->AddCircleFilled(ImVec2(secondX, y), radius, dotColor);
        }
    };

    auto renderModeSourceAssignments = [&](ModeConfig& mode, const std::string& idSuffix) {
        if (ImGui::TreeNode((std::string(trc("modes.sources")) + "##" + idSuffix).c_str())) {
            const ImGuiStyle& style = ImGui::GetStyle();
            const float rowHeight = ImGui::GetFrameHeight() + style.FramePadding.y * 1.5f;
            const float dragHandleWidth = (std::max)(10.0f, ImGui::GetFrameHeight() * 0.58f);
            const float listPaddingY = style.WindowPadding.y;
            const float listHeight = (std::min)(rowHeight * (mode.sources.empty() ? 1.8f : static_cast<float>(mode.sources.size()) + 0.8f),
                                                rowHeight * 6.25f);
            int removeIndex = -1;
            int moveFromIndex = -1;
            size_t moveInsertIndex = 0;
            bool hasPreviewInsertLine = false;
            float previewInsertLineY = 0.0f;
            ImVec4 previewInsertTint = ImVec4(0.45f, 0.72f, 1.00f, 1.00f);

            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.07f, 0.09f, 0.13f, 0.70f));
            const std::string listChildId = "##sources_list_" + idSuffix;
            if (ImGui::BeginChild(listChildId.c_str(), ImVec2(0.0f, listHeight), true)) {
                if (mode.sources.empty()) {
                    ImGui::Dummy(ImVec2(0.0f, rowHeight * 0.20f));
                    ImGui::TextDisabled(trc("modes.sources_drag_hint"));
                }

                for (size_t k = 0; k < mode.sources.size(); ++k) {
                    const ModeSourceRef& source = mode.sources[k];

                    ImGui::PushID(("mode_source_" + idSuffix + "_" + std::to_string(k)).c_str());

                    ImGui::Selectable("##source_row", false, ImGuiSelectableFlags_AllowOverlap,
                                      ImVec2(ImGui::GetContentRegionAvail().x, rowHeight));

                    const ImVec2 rowMin = ImGui::GetItemRectMin();
                    const ImVec2 rowMax = ImGui::GetItemRectMax();
                    const bool rowHovered = ImGui::IsItemHovered();
                    const bool rowActive = ImGui::IsItemActive();
                    const ImVec4 typeTint = getModeSourceTypeTint(source.type);
                    const ImVec4 rowFill = rowActive ? ImVec4(typeTint.x, typeTint.y, typeTint.z, 0.26f)
                                                     : rowHovered ? ImVec4(typeTint.x, typeTint.y, typeTint.z, 0.18f)
                                                                  : ImVec4(0.11f, 0.13f, 0.18f, 0.92f);
                    const ImVec4 rowBorder = rowHovered ? ImVec4(typeTint.x, typeTint.y, typeTint.z, 0.70f)
                                                        : ImVec4(typeTint.x, typeTint.y, typeTint.z, 0.38f);
                    ImDrawList* drawList = ImGui::GetWindowDrawList();
                    drawList->AddRectFilled(rowMin, rowMax, ImGui::ColorConvertFloat4ToU32(rowFill), 6.0f);
                    drawList->AddRect(rowMin, rowMax, ImGui::ColorConvertFloat4ToU32(rowBorder), 6.0f, 0, 1.0f);
                    drawList->AddRectFilled(ImVec2(rowMin.x + 1.0f, rowMin.y + 2.0f), ImVec2(rowMin.x + 5.0f, rowMax.y - 2.0f),
                                            ImGui::ColorConvertFloat4ToU32(ImVec4(typeTint.x, typeTint.y, typeTint.z, 0.95f)), 3.0f);

                    if (ImGui::BeginDragDropTarget()) {
                        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
                                "MODE_SOURCE_ROW", ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
                            if (payload->DataSize == sizeof(int)) {
                                const bool insertAfter = ImGui::GetMousePos().y > (rowMin.y + rowMax.y) * 0.5f;
                                const int payloadIndex = *static_cast<const int*>(payload->Data);
                                hasPreviewInsertLine = true;
                                previewInsertLineY = insertAfter ? rowMax.y : rowMin.y;
                                previewInsertTint = getModeSourceTypeTint(source.type);

                                if (payload->Delivery) {
                                    moveFromIndex = payloadIndex;
                                    moveInsertIndex = k + (insertAfter ? 1u : 0u);
                                }
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }

                    const float contentY = rowMin.y + (rowHeight - ImGui::GetFrameHeight()) * 0.5f;
                    const float removeButtonWidth = ImGui::GetFrameHeight();
                    const float removeButtonX = rowMax.x - style.FramePadding.x - removeButtonWidth;
                    float cursorX = rowMin.x + style.FramePadding.x + 8.0f;

                    ImGui::SetCursorScreenPos(ImVec2(cursorX, contentY));
                    renderModeSourceDragHandle("##drag_handle", dragHandleWidth, ImGui::GetFrameHeight());
                    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                        const int draggedIndex = static_cast<int>(k);
                        ImGui::SetDragDropPayload("MODE_SOURCE_ROW", &draggedIndex, sizeof(draggedIndex));

                        const float previewY = ImGui::GetCursorScreenPos().y;
                        renderModeSourceTypeIcon(source, "##drag_preview_icon");
                        ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
                        ImGui::SetCursorScreenPos(
                            ImVec2(ImGui::GetCursorScreenPos().x, previewY + (ImGui::GetFrameHeight() - ImGui::GetTextLineHeight()) * 0.5f));
                        ImGui::TextUnformatted(source.id.c_str());
                        ImGui::EndDragDropSource();
                    }
                    cursorX += dragHandleWidth + style.ItemInnerSpacing.x;

                    ImGui::SetCursorScreenPos(ImVec2(cursorX, contentY));
                    renderModeSourceTypeIcon(source, "##type_icon");
                    cursorX += ImGui::GetFrameHeight() + style.ItemInnerSpacing.x;

                    ImGui::SetCursorScreenPos(ImVec2(removeButtonX, contentY));
                    if (ImGui::Button("X##remove", ImVec2(removeButtonWidth, 0.0f))) { removeIndex = static_cast<int>(k); }

                    const float textMinX = cursorX;
                    const float textMaxX = removeButtonX - style.ItemInnerSpacing.x;
                    if (textMaxX > textMinX) {
                        const ImVec2 textPos(textMinX, rowMin.y + (rowHeight - ImGui::GetTextLineHeight()) * 0.5f);
                        drawList->PushClipRect(ImVec2(textMinX, rowMin.y), ImVec2(textMaxX, rowMax.y), true);
                        drawList->AddText(textPos, ImGui::GetColorU32(ImGuiCol_Text), source.id.c_str());
                        drawList->PopClipRect();
                    }

                    ImGui::PopID();
                }

                if (hasPreviewInsertLine) {
                    const ImVec2 lineStart(ImGui::GetWindowPos().x + style.WindowPadding.x, previewInsertLineY);
                    const ImVec2 lineEnd(ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x - style.WindowPadding.x, previewInsertLineY);
                    const ImU32 lineColor = ImGui::ColorConvertFloat4ToU32(ImVec4(previewInsertTint.x, previewInsertTint.y, previewInsertTint.z, 1.00f));
                    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(lineStart.x, previewInsertLineY - 4.0f),
                                                              ImVec2(lineStart.x + 10.0f, previewInsertLineY + 4.0f), lineColor, 3.0f);
                    ImGui::GetWindowDrawList()->AddLine(ImVec2(lineStart.x + 14.0f, lineStart.y), lineEnd, lineColor, 2.0f);
                    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(lineStart.x + 14.0f, lineStart.y), 3.0f, lineColor);
                }
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();

            ImGui::Dummy(ImVec2(0.0f, listPaddingY * 0.35f));

            if (removeIndex >= 0) {
                mode.sources.erase(mode.sources.begin() + removeIndex);
                g_configIsDirty = true;
            } else if (moveFromIndex >= 0 && static_cast<size_t>(moveFromIndex) < mode.sources.size()) {
                size_t normalizedInsertIndex = moveInsertIndex;
                if (normalizedInsertIndex > mode.sources.size()) { normalizedInsertIndex = mode.sources.size(); }

                size_t fromIndex = static_cast<size_t>(moveFromIndex);
                if (fromIndex < normalizedInsertIndex) { --normalizedInsertIndex; }

                if (fromIndex != normalizedInsertIndex) {
                    ModeSourceRef movedSource = mode.sources[fromIndex];
                    mode.sources.erase(mode.sources.begin() + static_cast<std::ptrdiff_t>(fromIndex));
                    mode.sources.insert(mode.sources.begin() + static_cast<std::ptrdiff_t>(normalizedInsertIndex), std::move(movedSource));
                    g_configIsDirty = true;
                }
            }

            if (ImGui::BeginCombo((std::string(trc("modes.add_source")) + "##" + idSuffix).c_str(), trc("modes.add_source"))) {
                auto renderSourceOption = [&](ModeSourceType type, const std::string& id) {
                    ImGui::PushID((std::to_string(static_cast<int>(type)) + "_" + id).c_str());
                    const bool alreadyAdded = ModeHasSource(mode, type, id);
                    const float optionHeight = ImGui::GetFrameHeight() + style.FramePadding.y;
                    if (alreadyAdded) { ImGui::BeginDisabled(); }
                    const bool selected = ImGui::Selectable("##source_option", false, 0, ImVec2(0.0f, optionHeight));
                    if (alreadyAdded) { ImGui::EndDisabled(); }
                    const ImVec2 optionMin = ImGui::GetItemRectMin();
                    const ImVec2 optionMax = ImGui::GetItemRectMax();
                    const bool optionHovered = ImGui::IsItemHovered();
                    const ImVec4 tint = getModeSourceTypeTint(type);
                    ImDrawList* drawList = ImGui::GetWindowDrawList();
                    if (optionHovered) {
                        drawList->AddRectFilled(optionMin, optionMax,
                                                ImGui::ColorConvertFloat4ToU32(ImVec4(tint.x, tint.y, tint.z, 0.16f)), 5.0f);
                    }

                    const float iconSize = ImGui::GetFrameHeight() - 1.0f;
                    const ImVec2 iconMin(optionMin.x + style.FramePadding.x, optionMin.y + (optionHeight - iconSize) * 0.5f);
                    drawModeSourceTypeIconAt(iconMin, iconSize, type);

                    const ImVec2 textPos(iconMin.x + iconSize + style.ItemInnerSpacing.x,
                                         optionMin.y + (optionHeight - ImGui::GetTextLineHeight()) * 0.5f);
                    const ImU32 textColor = alreadyAdded ? ImGui::GetColorU32(ImGuiCol_TextDisabled) : ImGui::GetColorU32(ImGuiCol_Text);
                    drawList->AddText(textPos, textColor, id.c_str());

                    if (alreadyAdded) {
                        ImGui::PopID();
                        return;
                    }

                    if (selected) {
                        if (AddModeSource(mode, type, id)) { g_configIsDirty = true; }
                    }

                    ImGui::PopID();
                };

                if (!g_config.mirrors.empty()) {
                    ImGui::SeparatorText(trc("modes.mirrors"));
                    for (const auto& mirrorConf : g_config.mirrors) { renderSourceOption(ModeSourceType::Mirror, mirrorConf.name); }
                }
                if (!g_config.mirrorGroups.empty()) {
                    ImGui::SeparatorText(trc("modes.mirror_groups"));
                    for (const auto& groupConf : g_config.mirrorGroups) { renderSourceOption(ModeSourceType::MirrorGroup, groupConf.name); }
                }
                if (!g_config.images.empty()) {
                    ImGui::SeparatorText(trc("modes.images"));
                    for (const auto& imgConf : g_config.images) { renderSourceOption(ModeSourceType::Image, imgConf.name); }
                }
                if (!g_config.windowOverlays.empty()) {
                    ImGui::SeparatorText(trc("modes.window_overlays"));
                    for (const auto& overlayConf : g_config.windowOverlays) {
                        renderSourceOption(ModeSourceType::WindowOverlay, overlayConf.name);
                    }
                }
                if (!g_config.browserOverlays.empty()) {
                    ImGui::SeparatorText(trc("modes.browser_overlays"));
                    for (const auto& overlayConf : g_config.browserOverlays) {
                        renderSourceOption(ModeSourceType::BrowserOverlay, overlayConf.name);
                    }
                }

                ImGui::EndCombo();
            }

            ImGui::TreePop();
        }
    };

    ImGui::SeparatorText(trc("modes.status.default_modes"));

    for (size_t i = 0; i < g_config.modes.size(); ++i) {
        auto& mode = g_config.modes[i];
        if (EqualsIgnoreCase(mode.id, "Fullscreen")) {
            ImGui::PushID((int)i);

            bool node_open = ImGui::TreeNodeEx("##mode_node", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", mode.id.c_str());
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::GetFrameHeight());

            ImGui::Dummy(ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));

            if (node_open) {

                if (!resolutionSupported) { ImGui::BeginDisabled(); }

                int modeScreenW = GetCachedWindowWidth();
                int modeScreenH = GetCachedWindowHeight();
                if (modeScreenW < 1) modeScreenW = 1;
                if (modeScreenH < 1) modeScreenH = 1;

                bool useManualPixelSize = !mode.useRelativeSize;
                if (ImGui::Checkbox((tr("modes.label.manual_pixel_size") + "##Fullscreen").c_str(), &useManualPixelSize)) {
                    mode.useRelativeSize = !useManualPixelSize;
                    mode.widthExpr.clear();
                    mode.heightExpr.clear();
                    if (mode.useRelativeSize) {
                        mode.relativeWidth = (std::max)(0.01f, (std::min)(1.0f, static_cast<float>(mode.width) / static_cast<float>(modeScreenW)));
                        mode.relativeHeight =
                            (std::max)(0.01f, (std::min)(1.0f, static_cast<float>(mode.height) / static_cast<float>(modeScreenH)));
                    } else {
                        mode.relativeWidth = -1.0f;
                        mode.relativeHeight = -1.0f;
                    }
                    g_configIsDirty = true;
                }

                ImGui::Columns(2, "dims", false);
                ImGui::Text(trc("label.width"));
                ImGui::NextColumn();
                if (mode.useRelativeSize) {
                    int displayedWidth = ResolveModeDisplayWidth(mode, modeScreenW, modeScreenH);
                    float widthPct = ((mode.relativeWidth >= 0.0f && mode.relativeWidth <= 1.0f)
                                          ? mode.relativeWidth
                                          : (static_cast<float>(displayedWidth) / static_cast<float>(modeScreenW))) *
                                     100.0f;
                    widthPct = (std::max)(1.0f, (std::min)(100.0f, widthPct));
                    if (ImGui::SliderFloat("##WidthPercent", &widthPct, 1.0f, 100.0f, "%.1f%%")) {
                        mode.useRelativeSize = true;
                        mode.relativeWidth = widthPct / 100.0f;
                        mode.width = ResolveModeDisplayWidth(mode, modeScreenW, modeScreenH);
                        displayedWidth = mode.width;
                        g_configIsDirty = true;
                        if (g_currentModeId == mode.id) {
                            HWND hwnd = g_minecraftHwnd.load();
                            if (hwnd) {
                                RetargetActiveModeTransition(mode);
                                PublishGuiConfigSnapshot();
                                RequestWindowClientResize(hwnd, mode.width, mode.height, "gui:fullscreen_width_slider");
                            }
                        }
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%d px)", displayedWidth);
                } else {
                    int tempWidth = mode.width;
                    if (Spinner("##Width", &tempWidth, 1, 1)) {
                        applyManualModeDimensionChange(mode, true, tempWidth, 1, "gui:fullscreen_width_spinner");
                    }
                }
                ImGui::NextColumn();
                ImGui::Text(trc("label.height"));
                ImGui::NextColumn();
                if (mode.useRelativeSize) {
                    int displayedHeight = ResolveModeDisplayHeight(mode, modeScreenW, modeScreenH);
                    float heightPct = ((mode.relativeHeight >= 0.0f && mode.relativeHeight <= 1.0f)
                                           ? mode.relativeHeight
                                           : (static_cast<float>(displayedHeight) / static_cast<float>(modeScreenH))) *
                                      100.0f;
                    heightPct = (std::max)(1.0f, (std::min)(100.0f, heightPct));
                    if (ImGui::SliderFloat("##HeightPercent", &heightPct, 1.0f, 100.0f, "%.1f%%")) {
                        mode.useRelativeSize = true;
                        mode.relativeHeight = heightPct / 100.0f;
                        mode.height = ResolveModeDisplayHeight(mode, modeScreenW, modeScreenH);
                        displayedHeight = mode.height;
                        g_configIsDirty = true;
                        if (g_currentModeId == mode.id) {
                            HWND hwnd = g_minecraftHwnd.load();
                            if (hwnd) {
                                RetargetActiveModeTransition(mode);
                                PublishGuiConfigSnapshot();
                                RequestWindowClientResize(hwnd, mode.width, mode.height, "gui:fullscreen_height_slider");
                            }
                        }
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%d px)", displayedHeight);
                } else {
                    int tempHeight = mode.height;
                    if (Spinner("##Height", &tempHeight, 1, 1)) {
                        applyManualModeDimensionChange(mode, false, tempHeight, 1, "gui:fullscreen_height_spinner");
                    }
                }
                ImGui::Columns(1);

                if (ImGui::Button(trc("modes.switch_to_this_mode"))) {
                    // Defer mode switch to avoid deadlock (g_configMutex is held during GUI rendering)
                    std::lock_guard<std::mutex> pendingLock(g_pendingModeSwitchMutex);
                    g_pendingModeSwitch.pending = true;
                    g_pendingModeSwitch.modeId = mode.id;
                    g_pendingModeSwitch.source = "GUI mode list";
                    Log("[GUI] Deferred mode switch to: " + mode.id);
                }
                mode.stretch.enabled = true;
                mode.stretch.x = 0;
                mode.stretch.y = 0;
                mode.stretch.width = GetCachedWindowWidth();
                mode.stretch.height = GetCachedWindowHeight();

                ImGui::Separator();
                if (ImGui::TreeNode(trc("modes.transition_settings"))) {
                    RenderTransitionSettingsHorizontalNoBackground(mode, "Fullscreen");
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode(trc("modes.border_settings"))) {
                    if (ImGui::Checkbox(trc("modes.enable_border"), &mode.border.enabled)) { g_configIsDirty = true; }
                    ImGui::SameLine();
                    HelpMarker(trc("modes.tooltip.enable_border"));

                    if (mode.border.enabled) {
                        ImGui::Text(trc("images.border_color"));
                        ImVec4 borderCol = ImVec4(mode.border.color.r, mode.border.color.g, mode.border.color.b, 1.0f);
                        if (ImGui::ColorEdit3("##BorderColor", (float*)&borderCol, ImGuiColorEditFlags_NoInputs)) {
                            mode.border.color = { borderCol.x, borderCol.y, borderCol.z };
                            g_configIsDirty = true;
                        }

                        ImGui::Text(trc("images.border_width"));
                        ImGui::SetNextItemWidth(100);
                        if (Spinner("##BorderWidth", &mode.border.width, 1, 1, 50)) { g_configIsDirty = true; }
                        ImGui::SameLine();
                        ImGui::TextDisabled(trc("label.px"));

                        ImGui::Text(trc("images.border_radius"));
                        ImGui::SetNextItemWidth(100);
                        if (Spinner("##BorderRadius", &mode.border.radius, 1, 0, 100)) { g_configIsDirty = true; }
                        ImGui::SameLine();
                        ImGui::TextDisabled(trc("label.px"));
                    }
                    ImGui::TreePop();
                }
                ImGui::Separator();

                renderModeSourceAssignments(mode, mode.id);


                if (ImGui::TreeNode((tr("modes.sensitivity_override") + "##Fullscreen").c_str())) {
                    if (ImGui::Checkbox(trc("modes.override_sensitivity"), &mode.sensitivityOverrideEnabled)) { g_configIsDirty = true; }
                    HelpMarker(trc("modes.tooltip.override_sensitivity"));

                    if (mode.sensitivityOverrideEnabled) {
                        if (ImGui::Checkbox(trc("modes.separate_xy_sensitivity"), &mode.separateXYSensitivity)) {
                            g_configIsDirty = true;
                            if (mode.separateXYSensitivity) {
                                mode.modeSensitivityX = mode.modeSensitivity;
                                mode.modeSensitivityY = mode.modeSensitivity;
                            }
                        }
                        ImGui::SameLine();
                        HelpMarker(trc("modes.tooltip.separate_xy_sensitivity"));

                        if (mode.separateXYSensitivity) {
                            RawInputSensitivityNote();
                            ImGui::Text(trc("modes.sensitivity_x"));
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##FullscreenSensitivityX", &mode.modeSensitivityX, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                            RawInputSensitivityNote();
                            ImGui::Text(trc("modes.sensitivity_y"));
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##FullscreenSensitivityY", &mode.modeSensitivityY, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                        } else {
                            RawInputSensitivityNote();
                            ImGui::Text(trc("modes.sensitivity"));
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##FullscreenSensitivity", &mode.modeSensitivity, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                            ImGui::SameLine();
                            HelpMarker(trc("modes.tooltip.sensitivity"));
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

    for (size_t i = 0; i < g_config.modes.size(); ++i) {
        auto& mode = g_config.modes[i];
        if (EqualsIgnoreCase(mode.id, "EyeZoom")) {
            ImGui::PushID((int)i + 10000);

            bool node_open = ImGui::TreeNodeEx("##mode_node", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", mode.id.c_str());
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::GetFrameHeight());

            ImGui::Dummy(ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));

            if (node_open) {
                if (!resolutionSupported) { ImGui::BeginDisabled(); }

                ImGui::Columns(2, "mode_config_cols", false);
                ImGui::SetColumnWidth(0, 150);

                ImGui::Text(trc("modes.game_width"));
                ImGui::NextColumn();
                int tempWidth2 = mode.width;
                if (Spinner("##ModeWidth", &tempWidth2, 1, 1, screenWidth)) {
                    applyManualModeDimensionChange(mode, true, tempWidth2, 1, "gui:eyezoom_width_spinner");
                }
                ImGui::NextColumn();
                ImGui::Text(trc("modes.game_height"));
                ImGui::NextColumn();
                int tempHeight2 = mode.height;
                if (Spinner("##ModeHeight", &tempHeight2, 1, 1, 16384)) {
                    applyManualModeDimensionChange(mode, false, tempHeight2, 1, "gui:eyezoom_height_spinner");
                }
                ImGui::Columns(1);

                if (ImGui::Button(trc("modes.switch_to_this_mode"))) {
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
                    ImGui::TextDisabled(trc("label.current"));
                }

                ImGui::Separator();
                ImGui::Text(trc("modes.eyezoom.settings"));

                ImGui::Text(trc("modes.eyezoom.clone_settings_source"));
                ImGui::Columns(2, "eyezoom_clone_cols", false);
                ImGui::SetColumnWidth(0, 150);
                ImGui::Text(trc("modes.eyezoom.clone_width"));
                ImGui::NextColumn();
                // Clone Width must be even - step by 2, enforce even values
                    int maxCloneWidth = mode.width;
                    if (maxCloneWidth < g_config.eyezoom.cloneWidth) maxCloneWidth = g_config.eyezoom.cloneWidth;
                if (SpinnerDeferredTextInput("##EyeZoomCloneWidth", &g_config.eyezoom.cloneWidth, 2, 2, maxCloneWidth)) {
                    if (g_config.eyezoom.cloneWidth % 2 != 0) { g_config.eyezoom.cloneWidth = (g_config.eyezoom.cloneWidth / 2) * 2; }
                    int maxOverlay = g_config.eyezoom.cloneWidth / 2;
                    if (g_config.eyezoom.overlayWidth > maxOverlay) g_config.eyezoom.overlayWidth = maxOverlay;
                    g_configIsDirty = true;
                }
                ImGui::NextColumn();
                ImGui::Text(trc("modes.eyezoom.clone_height"));
                ImGui::NextColumn();
                    int maxCloneHeight = mode.height;
                    if (maxCloneHeight < g_config.eyezoom.cloneHeight) maxCloneHeight = g_config.eyezoom.cloneHeight;
                if (SpinnerDeferredTextInput("##EyeZoomCloneHeight", &g_config.eyezoom.cloneHeight, 10, 1, maxCloneHeight)) g_configIsDirty = true;
                ImGui::Columns(1);

                ImGui::Separator();
                ImGui::Text(trc("label.overlay"));

                // -- Default overlay (built-in numbered boxes) --
                {
                    if (ImGui::RadioButton("##ezov_radio_default", g_config.eyezoom.activeOverlayIndex == -1)) {
                        g_config.eyezoom.activeOverlayIndex = -1;
                        g_configIsDirty = true;
                    }
                    ImGui::SameLine();
                    bool defaultNodeOpen = ImGui::TreeNodeEx("##ezoverlay_default_node", ImGuiTreeNodeFlags_SpanAvailWidth, "Default");
                    if (defaultNodeOpen) {
                        int maxOverlay = g_config.eyezoom.cloneWidth / 2;
                        ImGui::Text(trc("modes.eyezoom.overlay_pixels"));
                        ImGui::SameLine();
                        if (SpinnerDeferredTextInput("##EyeZoomOverlayWidth", &g_config.eyezoom.overlayWidth, 1, 0, maxOverlay)) g_configIsDirty = true;
                        ImGui::SameLine();
                        HelpMarker(trc("modes.eyezoom.tooltip.overlay_pixels"));
                        ImGui::TreePop();
                    }
                }

                // -- Custom overlays (user-added images) --
                int ezoverlay_to_remove = -1;
                for (size_t ovi = 0; ovi < g_config.eyezoom.overlays.size(); ++ovi) {
                    auto& ov = g_config.eyezoom.overlays[ovi];
                    ImGui::PushID(static_cast<int>(ovi));

                    std::string deleteLabel = "X##delete_ezoverlay_" + std::to_string(ovi);
                    if (ImGui::Button(deleteLabel.c_str(), ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()))) {
                        std::string popupId = (tr("modes.eyezoom.delete_overlay") + "##ezov_" + std::to_string(ovi));
                    ImGui::OpenPopup(popupId.c_str());
                }

                    {
                        std::string popupId = (tr("modes.eyezoom.delete_overlay") + "##ezov_" + std::to_string(ovi));
                        if (ImGui::BeginPopupModal(popupId.c_str(), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
                            ImGui::TextWrapped("%s", trc("modes.eyezoom.delete_overlay_confirm", ov.name));
                            ImGui::Separator();
                            if (ImGui::Button(trc("button.ok"), ImVec2(120, 0))) {
                                ezoverlay_to_remove = (int)ovi;
                                g_configIsDirty = true;
                                ImGui::CloseCurrentPopup();
                            }
                            ImGui::SameLine();
                            if (ImGui::Button(trc("button.cancel"), ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
                            ImGui::EndPopup();
                        }
                    }

                    ImGui::SameLine();
                    if (ImGui::RadioButton("##ezov_radio", g_config.eyezoom.activeOverlayIndex == (int)ovi)) {
                        g_config.eyezoom.activeOverlayIndex = (int)ovi;
                        g_configIsDirty = true;
                    }
                    ImGui::SameLine();

                    std::string oldName = ov.name;
                    bool nodeOpen = ImGui::TreeNodeEx("##ezoverlay_node", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", ov.name.c_str());

                    if (nodeOpen) {
                        bool hasDuplicate = HasDuplicateEyeZoomOverlayName(ov.name, ovi);
                        if (hasDuplicate) {
                            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.6f, 0.2f, 0.2f, 1.0f));
                            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.7f, 0.3f, 0.3f, 1.0f));
                            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.8f, 0.3f, 0.3f, 1.0f));
                        }

                        if (ImGui::InputText("Name##ezov", &ov.name)) {
                            if (!HasDuplicateEyeZoomOverlayName(ov.name, ovi)) {
                                g_configIsDirty = true;
                                if (!ov.path.empty()) {
                                    LoadImageAsync(DecodedImageData::UserImage, "ezoverlay_" + ov.name, ov.path, g_toolscreenPath);
                                }
                            } else {
                                ov.name = oldName;
                            }
                        }

                        if (hasDuplicate) { ImGui::PopStyleColor(3); }
                        if (hasDuplicate) {
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), trc("images.name_duplicate"));
                        }

                        if (RenderMaskedPathInput((tr("label.path") + "##ezov").c_str(), ov.path)) {
                            g_configIsDirty = true;
                        }
                        if (ImGui::IsItemDeactivatedAfterEdit() && !ov.path.empty()) {
                            LoadImageAsync(DecodedImageData::UserImage, "ezoverlay_" + ov.name, ov.path, g_toolscreenPath);
                        }

                        ImGui::SameLine();
                        if (ImGui::Button((tr("button.browse") + "##ezov_" + std::to_string(ovi)).c_str())) {
                            ImagePickerResult result = OpenImagePickerAndValidate(g_minecraftHwnd.load(), g_toolscreenPath, g_toolscreenPath);
                            if (result.completed) {
                                if (result.success) {
                                    ov.path = result.path;
                                    g_configIsDirty = true;
                                    LoadImageAsync(DecodedImageData::UserImage, "ezoverlay_" + ov.name, ov.path, g_toolscreenPath);
                                }
                            }
                        }

                        // File-found badge
                        if (!ov.path.empty()) {
                            std::wstring ovWpath = Utf8ToWide(ov.path);
                            std::wstring ovFinalPath = (PathIsRelativeW(ovWpath.c_str()) && !g_toolscreenPath.empty())
                                ? (g_toolscreenPath + L"\\" + ovWpath) : ovWpath;
                            if (std::filesystem::exists(ovFinalPath)) {
                                ImGui::SameLine();
                                ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), trc("label.file_exists"));
                                if (ImGui::IsItemHovered()) ImGui::SetTooltip(trc("label.file_exists"));
                            } else {
                                ImGui::SameLine();
                                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "(!)");
                                if (ImGui::IsItemHovered()) ImGui::SetTooltip(trc("error.file_not_exists"));
                            }
                        }

                        ImGui::Text(trc("label.display_mode"));
                        ImGui::SameLine();
                        int dispMode = static_cast<int>(ov.displayMode);
                        if (ImGui::RadioButton((tr("label.manual") + "##ezov").c_str(), &dispMode, 0)) { ov.displayMode = EyeZoomOverlayDisplayMode::Manual; g_configIsDirty = true; }
                        ImGui::SameLine();
                        if (ImGui::RadioButton((tr("label.fit") + "##ezov").c_str(), &dispMode, 1)) { ov.displayMode = EyeZoomOverlayDisplayMode::Fit; g_configIsDirty = true; }
                        ImGui::SameLine();
                        if (ImGui::RadioButton((tr("label.stretch") + "##ezov").c_str(), &dispMode, 2)) { ov.displayMode = EyeZoomOverlayDisplayMode::Stretch; g_configIsDirty = true; }

                        if (ov.displayMode == EyeZoomOverlayDisplayMode::Manual) {
                            if (ImGui::SliderInt((tr("label.width") + "##ezov_mw").c_str(), &ov.manualWidth, 1, 4096)) g_configIsDirty = true;
                            if (ImGui::SliderInt((tr("label.height") + "##ezov_mh").c_str(), &ov.manualHeight, 1, 4096)) g_configIsDirty = true;
                            if (ImGui::Checkbox(trc("modes.eyezoom.clip_to_zoom_area"), &ov.clipToZoomArea)) { g_configIsDirty = true; }
                            ImGui::SameLine();
                            HelpMarker(trc("modes.eyezoom.tooltip.clip_to_zoom_area"));
                        }

                        if (ImGui::SliderFloat((tr("label.opacity") + "##ezov").c_str(), &ov.opacity, 0.0f, 1.0f)) g_configIsDirty = true;

                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                }

                if (ezoverlay_to_remove != -1) {
                    g_config.eyezoom.overlays.erase(g_config.eyezoom.overlays.begin() + ezoverlay_to_remove);
                    if (g_config.eyezoom.activeOverlayIndex == ezoverlay_to_remove) {
                        g_config.eyezoom.activeOverlayIndex = -1; // fall back to Default
                    } else if (g_config.eyezoom.activeOverlayIndex > ezoverlay_to_remove) {
                        g_config.eyezoom.activeOverlayIndex--;
                    }
                    DiscardUnusedUserImageCaches();
                    g_configIsDirty = true;
                }

                if (ImGui::Button(trc("modes.eyezoom.add_overlay"))) {
                    EyeZoomOverlayConfig newOv;
                    newOv.name = tr("modes.eyezoom.overlay") + " " + std::to_string(g_config.eyezoom.overlays.size() + 1);
                    g_config.eyezoom.overlays.push_back(newOv);
                    g_configIsDirty = true;
                }

                ImGui::Separator();
                ImGui::Text(trc("label.placement"));
                if (ImGui::Checkbox(trc("modes.eyezoom.use_custom_position"), &g_config.eyezoom.useCustomSizePosition)) { g_configIsDirty = true; }
                ImGui::SameLine();
                HelpMarker(trc("modes.eyezoom.tooltip.use_custom_position"));

                int monitorHeight = GetCachedWindowHeight();
                if (monitorHeight < 1) monitorHeight = 1;
                int monitorWidth = screenWidth;
                if (monitorWidth < 1) monitorWidth = 1;

                if (!g_config.eyezoom.useCustomSizePosition) {
                    int eyezoomModeWidth = mode.width;
                    int eyezoomTargetFinalX = (monitorWidth - eyezoomModeWidth) / 2;
                    if (eyezoomTargetFinalX < 1) eyezoomTargetFinalX = 1;

                    int autoHorizontalMargin = eyezoomTargetFinalX / 10;
                    int autoZoomAreaWidth = eyezoomTargetFinalX - (2 * autoHorizontalMargin);
                    if (autoZoomAreaWidth < 1) autoZoomAreaWidth = 1;

                    int autoVerticalMargin = monitorHeight / 8;
                    int autoZoomAreaHeight = monitorHeight - (2 * autoVerticalMargin);
                    if (autoZoomAreaHeight < 1) autoZoomAreaHeight = 1;

                    int autoPosY = (monitorHeight - autoZoomAreaHeight) / 2;
                    const std::string autoLayoutText =
                        tr("label.auto_layout_format", autoHorizontalMargin, autoPosY, autoZoomAreaWidth, autoZoomAreaHeight);
                    ImGui::TextDisabled("%s", autoLayoutText.c_str());
                }

                ImGui::BeginDisabled(!g_config.eyezoom.useCustomSizePosition);

                ImGui::Separator();
                ImGui::Text(trc("modes.eyezoom.zoom_area_output"));
                const float eyeZoomAreaLabelWidth =
                    (std::max)(150.0f,
                               (std::max)(ImGui::CalcTextSize(trc("modes.eyezoom.zoom_area_width")).x,
                                          ImGui::CalcTextSize(trc("modes.eyezoom.zoom_area_height")).x) +
                                   ImGui::GetStyle().ItemSpacing.x + 24.0f);
                ImGui::Columns(2, "eyezoom_area_cols", false);
                ImGui::SetColumnWidth(0, eyeZoomAreaLabelWidth);
                ImGui::Text(trc("modes.eyezoom.zoom_area_width"));
                ImGui::NextColumn();
                constexpr int kEyeZoomMaxCustomDimension = 16384;
                constexpr int kEyeZoomMaxCustomPosition = 16384;
                int maxZoomAreaWidth = (std::max)(kEyeZoomMaxCustomDimension, g_config.eyezoom.zoomAreaWidth);
                int maxZoomAreaHeight = (std::max)(kEyeZoomMaxCustomDimension, g_config.eyezoom.zoomAreaHeight);
                if (SpinnerDeferredTextInput("##EyeZoomAreaWidth", &g_config.eyezoom.zoomAreaWidth, 10, 1, maxZoomAreaWidth)) g_configIsDirty = true;
                ImGui::NextColumn();
                ImGui::Text(trc("modes.eyezoom.zoom_area_height"));
                ImGui::NextColumn();
                if (SpinnerDeferredTextInput("##EyeZoomAreaHeight", &g_config.eyezoom.zoomAreaHeight, 10, 1, maxZoomAreaHeight)) g_configIsDirty = true;
                ImGui::Columns(1);

                ImGui::Separator();
                HelpMarker("Set the EyeZoom output rectangle size, then place it anywhere on screen using X/Y.");

                int maxPosX = (std::max)(kEyeZoomMaxCustomPosition, g_config.eyezoom.positionX);
                int maxPosY = (std::max)(kEyeZoomMaxCustomPosition, g_config.eyezoom.positionY);

                ImGui::Columns(2, "eyezoom_position_cols", false);
                ImGui::SetColumnWidth(0, 150);
                ImGui::Text(trc("label.position_x"));
                ImGui::NextColumn();
                if (SpinnerDeferredTextInput("##EyeZoomPositionX", &g_config.eyezoom.positionX, 10, 0, maxPosX)) g_configIsDirty = true;
                ImGui::NextColumn();
                ImGui::Text(trc("label.position_y"));
                ImGui::NextColumn();
                if (SpinnerDeferredTextInput("##EyeZoomPositionY", &g_config.eyezoom.positionY, 10, 0, maxPosY)) g_configIsDirty = true;
                ImGui::Columns(1);

                ImGui::EndDisabled();

                ImGui::Separator();
                ImGui::Text(trc("modes.eyezoom.color_settings"));
                {
                    ImVec4 col1 = ImVec4(g_config.eyezoom.gridColor1.r, g_config.eyezoom.gridColor1.g, g_config.eyezoom.gridColor1.b,
                                         g_config.eyezoom.gridColor1Opacity);
                    if (ImGui::ColorEdit4(trc("modes.eyezoom.grid_color_1"), (float*)&col1, ImGuiColorEditFlags_AlphaBar)) {
                        g_config.eyezoom.gridColor1 = { col1.x, col1.y, col1.z };
                        g_config.eyezoom.gridColor1Opacity = col1.w;
                        g_configIsDirty = true;
                    }
                }
                {
                    ImVec4 col2 = ImVec4(g_config.eyezoom.gridColor2.r, g_config.eyezoom.gridColor2.g, g_config.eyezoom.gridColor2.b,
                                         g_config.eyezoom.gridColor2Opacity);
                    if (ImGui::ColorEdit4(trc("modes.eyezoom.grid_color_2"), (float*)&col2, ImGuiColorEditFlags_AlphaBar)) {
                        g_config.eyezoom.gridColor2 = { col2.x, col2.y, col2.z };
                        g_config.eyezoom.gridColor2Opacity = col2.w;
                        g_configIsDirty = true;
                    }
                }
                {
                    ImVec4 col3 = ImVec4(g_config.eyezoom.centerLineColor.r, g_config.eyezoom.centerLineColor.g,
                                         g_config.eyezoom.centerLineColor.b, g_config.eyezoom.centerLineColorOpacity);
                    if (ImGui::ColorEdit4(trc("modes.eyezoom.center_line_color"), (float*)&col3, ImGuiColorEditFlags_AlphaBar)) {
                        g_config.eyezoom.centerLineColor = { col3.x, col3.y, col3.z };
                        g_config.eyezoom.centerLineColorOpacity = col3.w;
                        g_configIsDirty = true;
                    }
                }
                {
                    ImVec4 col4 = ImVec4(g_config.eyezoom.textColor.r, g_config.eyezoom.textColor.g, g_config.eyezoom.textColor.b,
                                         g_config.eyezoom.textColorOpacity);
                    if (ImGui::ColorEdit4(trc("modes.eyezoom.text_color"), (float*)&col4, ImGuiColorEditFlags_AlphaBar)) {
                        g_config.eyezoom.textColor = { col4.x, col4.y, col4.z };
                        g_config.eyezoom.textColorOpacity = col4.w;
                        g_configIsDirty = true;
                    }
                }

                ImGui::Separator();
                ImGui::Text(trc("modes.eyezoom.text_settings"));

                const char* eyeZoomFontSizeModePreview = trc("modes.eyezoom.auto_font_size");
                switch (g_config.eyezoom.fontSizeMode) {
                    case EyeZoomFontSizeMode::PerSquareAuto:
                        eyeZoomFontSizeModePreview = trc("modes.eyezoom.per_square_auto_font_size");
                        break;
                    case EyeZoomFontSizeMode::Manual:
                        eyeZoomFontSizeModePreview = trc("modes.eyezoom.manual_font_size");
                        break;
                    case EyeZoomFontSizeMode::Auto:
                    default:
                        break;
                }

                if (ImGui::BeginCombo(trc("modes.eyezoom.auto_font_size"), eyeZoomFontSizeModePreview)) {
                    const struct EyeZoomFontSizeModeOption {
                        EyeZoomFontSizeMode mode;
                        const char* labelKey;
                    } options[] = {
                        { EyeZoomFontSizeMode::Auto, "modes.eyezoom.auto_font_size" },
                        { EyeZoomFontSizeMode::PerSquareAuto, "modes.eyezoom.per_square_auto_font_size" },
                        { EyeZoomFontSizeMode::Manual, "modes.eyezoom.manual_font_size" },
                    };

                    for (const auto& option : options) {
                        const bool isSelected = g_config.eyezoom.fontSizeMode == option.mode;
                        if (ImGui::Selectable(trc(option.labelKey), isSelected)) {
                            g_config.eyezoom.fontSizeMode = option.mode;
                            g_configIsDirty = true;
                        }
                        if (isSelected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }

                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                HelpMarker(trc("modes.eyezoom.tooltip.auto_font_size"));

                if (g_config.eyezoom.fontSizeMode == EyeZoomFontSizeMode::Manual) {
                    ImGui::SetNextItemWidth(250);
                    if (ImGui::SliderInt(trc("modes.eyezoom.text_font_size"), &g_config.eyezoom.textFontSize, 1, 96)) {
                        g_configIsDirty = true;
                        SetOverlayTextFontSize(g_config.eyezoom.textFontSize);
                    }
                } else {
                    ImGui::TextDisabled(trc("modes.eyezoom.tooltip.font_size_clamped"));
                    if (g_config.eyezoom.linkRectToFont) {
                        ImGui::TextDisabled(trc("modes.eyezoom.tooltip.link_rect_to_font"));
                    }
                }

                const std::vector<FontPickerOption> eyeZoomFontOptions = BuildFontPickerOptions({ { "", "font.preset.use_main_gui" } });
                auto applyEyeZoomFontChange = []() {
                    g_configIsDirty = true;
                    g_eyeZoomFontNeedsReload.store(true);
                };

                ImGui::Text(trc("modes.eyezoom.text_font"));
                const bool usingCustomEyeZoomFont = RenderFontPickerCombo("##EyeZoomTextFontChoice", 300.0f, eyeZoomFontOptions,
                                                                          g_config.eyezoom.textFontPath,
                                                                          s_eyeZoomFontPickerState, applyEyeZoomFontChange);
                ImGui::SameLine();
                HelpMarker(trc("modes.eyezoom.tooltip.custom_font"));

                if (usingCustomEyeZoomFont) {
                    ImGui::Text(trc("label.font_path"));
                    RenderCustomFontPathEditor("##EyeZoomTextFont", "##EyeZoomFont", 300.0f, eyeZoomFontOptions,
                                               g_config.eyezoom.textFontPath, s_eyeZoomFontPickerState,
                                               "Select Font for EyeZoom Text", applyEyeZoomFontChange);
                }

                if (ImGui::Checkbox(trc("modes.eyezoom.link_rect_to_font"), &g_config.eyezoom.linkRectToFont)) {
                    g_configIsDirty = true;
                    if (g_config.eyezoom.linkRectToFont) {
                        g_config.eyezoom.rectHeight = static_cast<int>(g_config.eyezoom.textFontSize * 1.2f);
                    }
                }

                if (!g_config.eyezoom.linkRectToFont) {
                    ImGui::SetNextItemWidth(250);
                    if (ImGui::SliderInt(trc("modes.eyezoom.override_rect_height"), &g_config.eyezoom.rectHeight, 8, 120)) {
                        g_configIsDirty = true;
                    }
                }

                if (ImGui::TreeNode(trc("modes.background"))) {
                    if (ImGui::RadioButton(trc("modes.color"), mode.background.selectedMode == "color")) {
                        if (mode.background.selectedMode != "color") {
                            mode.background.selectedMode = "color";
                            g_configIsDirty = true;
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton(trc("modes.gradient"), mode.background.selectedMode == "gradient")) {
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
                    if (ImGui::RadioButton(trc("label.image"), mode.background.selectedMode == "image")) {
                        if (mode.background.selectedMode != "image") {
                            mode.background.selectedMode = "image";
                            g_configIsDirty = true;
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
                        ImGui::SetNextItemWidth(200);
                        if (ImGui::SliderFloat("Angle##bgGradAngle", &mode.background.gradientAngle, 0.0f, 360.0f, "%.0f deg")) {
                            g_configIsDirty = true;
                        }

                        ImGui::Text(trc("modes.color_stops"));
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
                            if (ImGui::Button((tr("modes.gradient_add_color_stop") + "##bgGrad").c_str())) {
                                GradientColorStop newStop;
                                newStop.position = 0.5f;
                                newStop.color = { 0.5f, 0.5f, 0.5f };
                                mode.background.gradientStops.push_back(newStop);
                                std::sort(mode.background.gradientStops.begin(), mode.background.gradientStops.end(),
                                          [](const GradientColorStop& a, const GradientColorStop& b) { return a.position < b.position; });
                                g_configIsDirty = true;
                            }
                        }

                        ImGui::Separator();
                        ImGui::Text(trc("modes.gradient_animation"));
                        const char* animTypeNames[] = { trc("modes.gradient_animation_none"), trc("modes.gradient_animation_rotate"), trc("modes.gradient_animation_slide"), trc("modes.gradient_animation_wave"), trc("modes.gradient_animation_spiral"), trc("modes.gradient_animation_fade") };
                        int currentAnimType = static_cast<int>(mode.background.gradientAnimation);
                        ImGui::SetNextItemWidth(120);
                        if (ImGui::Combo((std::string(tr("modes.gradient_animation_type")) + "##GradAnim").c_str(), &currentAnimType, animTypeNames, IM_ARRAYSIZE(animTypeNames))) {
                            mode.background.gradientAnimation = static_cast<GradientAnimationType>(currentAnimType);
                            g_configIsDirty = true;
                        }
                        if (mode.background.gradientAnimation != GradientAnimationType::None) {
                            ImGui::SetNextItemWidth(150);
                            if (ImGui::SliderFloat((tr("modes.gradient_animation_speed") + "##GradAnimSpeed").c_str(), &mode.background.gradientAnimationSpeed, 0.1f, 5.0f, "%.1fx")) {
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
                        if (RenderMaskedPathInput(tr("modes.bg_image_path").c_str(), mode.background.image)) {
                            ClearImageError("eyezoom_bg");
                            g_configIsDirty = true;
                            g_allImagesLoaded = false;
                            g_pendingImageLoad = true;
                        }
                        ImGui::SameLine();
                        if (ImGui::Button((tr("button.browse") + "##eyezoom_bg").c_str())) {
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

                        renderBackgroundImageOptions(mode, "eyezoom_bg_fit");

                        std::string bgError = GetImageError("eyezoom_bg");
                        if (!bgError.empty()) { ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", bgError.c_str()); }
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode((tr("modes.border_settings") + "##EyeZoom").c_str())) {
                    if (ImGui::Checkbox((tr("modes.enable_border") + "##EyeZoom").c_str(), &mode.border.enabled)) { g_configIsDirty = true; }
                    ImGui::SameLine();
                    HelpMarker(trc("modes.tooltip.enable_border"));

                    if (mode.border.enabled) {
                        ImGui::Text(trc("images.border_color"));
                        ImVec4 borderCol = ImVec4(mode.border.color.r, mode.border.color.g, mode.border.color.b, 1.0f);
                        if (ImGui::ColorEdit3("##BorderColorEyeZoom", (float*)&borderCol, ImGuiColorEditFlags_NoInputs)) {
                            mode.border.color = { borderCol.x, borderCol.y, borderCol.z };
                            g_configIsDirty = true;
                        }

                        ImGui::Text(trc("images.border_width"));
                        ImGui::SetNextItemWidth(100);
                        if (Spinner("##BorderWidthEyeZoom", &mode.border.width, 1, 1, 50)) { g_configIsDirty = true; }
                        ImGui::SameLine();
                        ImGui::TextDisabled(trc("label.px"));

                        ImGui::Text(trc("images.border_radius"));
                        ImGui::SetNextItemWidth(100);
                        if (Spinner("##BorderRadiusEyeZoom", &mode.border.radius, 1, 0, 100)) { g_configIsDirty = true; }
                        ImGui::SameLine();
                        ImGui::TextDisabled(trc("label.px"));
                    }
                    ImGui::TreePop();
                }

                renderModeSourceAssignments(mode, mode.id);

                ImGui::Separator();
                if (ImGui::TreeNode((tr("modes.transition_settings") + "##EyeZoom").c_str())) {
                    RenderTransitionSettingsHorizontal(mode, "EyeZoom");

                    ImGui::Separator();
                    if (ImGui::Checkbox(trc("modes.eyezoom.slide_zoom_in"), &g_config.eyezoom.slideZoomIn)) { g_configIsDirty = true; }
                    ImGui::SameLine();
                    HelpMarker(trc("modes.eyezoom.tooltip.slide_zoom_in"));

                    if (ImGui::Checkbox(trc("modes.slide_mirrors_in"), &g_config.eyezoom.slideMirrorsIn)) { g_configIsDirty = true; }
                    ImGui::SameLine();
                    HelpMarker(trc("modes.tooltip.slide_mirrors_in"));

                    ImGui::TreePop();
                }


                if (ImGui::TreeNode((tr("modes.sensitivity_override") + "##EyeZoom").c_str())) {
                    if (ImGui::Checkbox(trc("modes.override_sensitivity"), &mode.sensitivityOverrideEnabled)) { g_configIsDirty = true; }
                    HelpMarker(trc("modes.tooltip.override_sensitivity"));

                    if (mode.sensitivityOverrideEnabled) {
                        if (ImGui::Checkbox((tr("modes.separate_xy_sensitivity") + "##EyeZoom").c_str(), &mode.separateXYSensitivity)) {
                            g_configIsDirty = true;
                            if (mode.separateXYSensitivity) {
                                mode.modeSensitivityX = mode.modeSensitivity;
                                mode.modeSensitivityY = mode.modeSensitivity;
                            }
                        }
                        ImGui::SameLine();
                        HelpMarker(trc("modes.tooltip.separate_xy_sensitivity"));

                        if (mode.separateXYSensitivity) {
                            RawInputSensitivityNote();
                            ImGui::Text(trc("modes.sensitivity_x"));
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##EyeZoomSensitivityX", &mode.modeSensitivityX, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                            RawInputSensitivityNote();
                            ImGui::Text(trc("modes.sensitivity_y"));
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##EyeZoomSensitivityY", &mode.modeSensitivityY, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                        } else {
                            RawInputSensitivityNote();
                            ImGui::Text(trc("modes.sensitivity"));
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##EyeZoomSensitivity", &mode.modeSensitivity, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                            ImGui::SameLine();
                            HelpMarker(trc("modes.tooltip.sensitivity"));
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

    for (size_t i = 0; i < g_config.modes.size(); ++i) {
        auto& mode = g_config.modes[i];
        if (EqualsIgnoreCase(mode.id, "Preemptive")) {
            ImGui::PushID((int)i + 15000);

            bool node_open = ImGui::TreeNodeEx("##mode_node", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", mode.id.c_str());
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::GetFrameHeight());

            ImGui::Dummy(ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));

            if (node_open) {
                if (!resolutionSupported) { ImGui::BeginDisabled(); }

                const ModeConfig* eyezoomMode = nullptr;
                for (const auto& m2 : g_config.modes) {
                    if (EqualsIgnoreCase(m2.id, "EyeZoom")) {
                        eyezoomMode = &m2;
                        break;
                    }
                }

                int copiedW = eyezoomMode ? eyezoomMode->width : mode.width;
                int copiedH = eyezoomMode ? eyezoomMode->height : mode.height;

                ImGui::Columns(2, "preemptive_dims", false);
                ImGui::SetColumnWidth(0, 150);

                ImGui::Text(trc("modes.game_width"));
                ImGui::NextColumn();
                {
                    ImGui::BeginDisabled();
                    int tempW = copiedW;
                    (void)Spinner("##PreemptiveModeWidth", &tempW, 1, 1, screenWidth);
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::TextDisabled(trc("modes.preemptive.copied_from_eyezoom"));
                }

                ImGui::NextColumn();
                ImGui::Text(trc("modes.game_height"));
                ImGui::NextColumn();
                {
                    ImGui::BeginDisabled();
                    int tempH = copiedH;
                    (void)Spinner("##PreemptiveModeHeight", &tempH, 1, 1, 16384);
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::TextDisabled(trc("modes.preemptive.copied_from_eyezoom"));
                }
                ImGui::Columns(1);

                if (!eyezoomMode) {
                    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f), trc("modes.preemptive.eyezoom_not_found"));
                }

                if (ImGui::Button((tr("modes.switch_to_this_mode") + "##Preemptive").c_str())) {
                    std::lock_guard<std::mutex> pendingLock(g_pendingModeSwitchMutex);
                    g_pendingModeSwitch.pending = true;
                    g_pendingModeSwitch.modeId = mode.id;
                    g_pendingModeSwitch.source = "GUI Preemptive mode";
                }

                if (!resolutionSupported) { ImGui::EndDisabled(); }

                if (g_currentModeId == mode.id) {
                    ImGui::SameLine();
                    ImGui::TextDisabled(trc("label.current"));
                }

                if (ImGui::TreeNode((tr("modes.background") + "##Preemptive").c_str())) {
                    if (ImGui::RadioButton((tr("modes.color") + "##Preemptive").c_str(), mode.background.selectedMode == "color")) {
                        if (mode.background.selectedMode != "color") {
                            mode.background.selectedMode = "color";
                            g_configIsDirty = true;
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton((tr("modes.gradient") + "##Preemptive").c_str(), mode.background.selectedMode == "gradient")) {
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
                    if (ImGui::RadioButton((tr("label.image") + "##Preemptive").c_str(), mode.background.selectedMode == "image")) {
                        if (mode.background.selectedMode != "image") {
                            mode.background.selectedMode = "image";
                            g_configIsDirty = true;
                            if (!mode.background.image.empty()) {
                                g_allImagesLoaded = false;
                                g_pendingImageLoad = true;
                                LoadImageAsync(DecodedImageData::Type::Background, mode.id, mode.background.image, g_toolscreenPath);
                            }
                        }
                    }

                    if (mode.background.selectedMode == "color") {
                        if (ImGui::ColorEdit3("##bgColorPreemptive", &mode.background.color.r)) { g_configIsDirty = true; }
                    } else if (mode.background.selectedMode == "gradient") {
                        ImGui::SetNextItemWidth(200);
                        if (ImGui::SliderFloat((tr("modes.gradient_angle") + "##bgGradAnglePreemptive").c_str(), &mode.background.gradientAngle, 0.0f, 360.0f, "%.0f deg")) {
                            g_configIsDirty = true;
                        }

                        ImGui::Text(trc("modes.color_stops"));
                        int stopToRemove = -1;
                        for (size_t i2 = 0; i2 < mode.background.gradientStops.size(); i2++) {
                            ImGui::PushID(static_cast<int>(i2));
                            auto& stop = mode.background.gradientStops[i2];

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
                                if (ImGui::Button("X##RemoveStop")) { stopToRemove = static_cast<int>(i2); }
                            }

                            ImGui::PopID();
                        }
                        if (stopToRemove >= 0) {
                            mode.background.gradientStops.erase(mode.background.gradientStops.begin() + stopToRemove);
                            g_configIsDirty = true;
                        }

                        if (mode.background.gradientStops.size() < 8) {
                            if (ImGui::Button((tr("modes.gradient_add_color_stop") + "##bgGradPreemptive").c_str())) {
                                GradientColorStop newStop;
                                newStop.position = 0.5f;
                                newStop.color = { 0.5f, 0.5f, 0.5f };
                                mode.background.gradientStops.push_back(newStop);
                                std::sort(mode.background.gradientStops.begin(), mode.background.gradientStops.end(),
                                          [](const GradientColorStop& a, const GradientColorStop& b) { return a.position < b.position; });
                                g_configIsDirty = true;
                            }
                        }

                        ImGui::Separator();
                        ImGui::Text(trc("modes.gradient_animation"));
                        const char* animTypeNames[] = { trc("modes.gradient_animation_none"), trc("modes.gradient_animation_rotate"), trc("modes.gradient_animation_slide"), trc("modes.gradient_animation_wave"), trc("modes.gradient_animation_spiral"), trc("modes.gradient_animation_fade") };
                        int currentAnimType = static_cast<int>(mode.background.gradientAnimation);
                        ImGui::SetNextItemWidth(120);
                        if (ImGui::Combo((tr("modes.gradient_animation_type") + "##GradAnimPreemptive").c_str(), &currentAnimType, animTypeNames, IM_ARRAYSIZE(animTypeNames))) {
                            mode.background.gradientAnimation = static_cast<GradientAnimationType>(currentAnimType);
                            g_configIsDirty = true;
                        }
                        if (mode.background.gradientAnimation != GradientAnimationType::None) {
                            ImGui::SetNextItemWidth(150);
                            if (ImGui::SliderFloat((tr("modes.gradient_animation_speed") + "##GradAnimSpeedPreemptive").c_str(), &mode.background.gradientAnimationSpeed, 0.1f, 5.0f,
                                                   "%.1fx")) {
                                g_configIsDirty = true;
                            }
                        }
                    } else if (mode.background.selectedMode == "image") {
                        if (RenderMaskedPathInput((tr("modes.bg_image_path") + "##preemptive_bg").c_str(), mode.background.image)) {
                            ClearImageError("preemptive_bg");
                            g_configIsDirty = true;
                            g_allImagesLoaded = false;
                            g_pendingImageLoad = true;
                        }
                        ImGui::SameLine();
                        if (ImGui::Button((tr("button.browse") + "##preemptive_bg").c_str())) {
                            ImagePickerResult result =
                                OpenImagePickerAndValidate(g_minecraftHwnd.load(), g_toolscreenPath, g_toolscreenPath);

                            if (result.completed) {
                                if (result.success) {
                                    mode.background.image = result.path;
                                    ClearImageError("preemptive_bg");
                                    g_allImagesLoaded = false;
                                    g_pendingImageLoad = true;
                                    g_configIsDirty = true;
                                } else if (!result.error.empty()) {
                                    SetImageError("preemptive_bg", result.error);
                                }
                            }
                        }

                        renderBackgroundImageOptions(mode, "preemptive_bg_fit");

                        std::string bgError = GetImageError("preemptive_bg");
                        if (!bgError.empty()) { ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", bgError.c_str()); }
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode((tr("modes.border_settings") + "##Preemptive").c_str())) {
                    if (ImGui::Checkbox((tr("modes.enable_border") + "##Preemptive").c_str(), &mode.border.enabled)) { g_configIsDirty = true; }
                    ImGui::SameLine();
                    HelpMarker(trc("modes.tooltip.enable_border"));

                    if (mode.border.enabled) {
                        ImGui::Text(trc("images.border_color"));
                        ImVec4 borderCol = ImVec4(mode.border.color.r, mode.border.color.g, mode.border.color.b, 1.0f);
                        if (ImGui::ColorEdit3("##BorderColorPreemptive", (float*)&borderCol, ImGuiColorEditFlags_NoInputs)) {
                            mode.border.color = { borderCol.x, borderCol.y, borderCol.z };
                            g_configIsDirty = true;
                        }

                        ImGui::Text(trc("modes.border_width"));
                        ImGui::SetNextItemWidth(100);
                        if (Spinner("##BorderWidthPreemptive", &mode.border.width, 1, 1, 50)) { g_configIsDirty = true; }
                        ImGui::SameLine();
                        ImGui::TextDisabled(trc("label.px"));

                        ImGui::Text(trc("images.border_radius"));
                        ImGui::SetNextItemWidth(100);
                        if (Spinner("##BorderRadiusPreemptive", &mode.border.radius, 1, 0, 100)) { g_configIsDirty = true; }
                        ImGui::SameLine();
                        ImGui::TextDisabled(trc("label.px"));
                    }
                    ImGui::TreePop();
                }

                renderModeSourceAssignments(mode, mode.id);
                ImGui::Separator();
                if (ImGui::TreeNode((tr("modes.transition_settings") + "##Preemptive").c_str())) {
                    RenderTransitionSettingsHorizontal(mode, "Preemptive");
                    if (ImGui::Checkbox((tr("modes.slide_mirrors_in") + "##Preemptive").c_str(), &mode.slideMirrorsIn)) { g_configIsDirty = true; }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(trc("modes.tooltip.slide_mirrors_in"));
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode((tr("modes.sensitivity_override") + "##Preemptive").c_str())) {
                    if (ImGui::Checkbox((tr("modes.override_sensitivity") + "##Preemptive").c_str(), &mode.sensitivityOverrideEnabled)) { g_configIsDirty = true; }
                    HelpMarker(trc("modes.tooltip.override_sensitivity"));

                    if (mode.sensitivityOverrideEnabled) {
                        if (ImGui::Checkbox((tr("modes.separate_xy_sensitivity") + "##Preemptive").c_str(), &mode.separateXYSensitivity)) {
                            g_configIsDirty = true;
                            if (mode.separateXYSensitivity) {
                                mode.modeSensitivityX = mode.modeSensitivity;
                                mode.modeSensitivityY = mode.modeSensitivity;
                            }
                        }
                        ImGui::SameLine();
                        HelpMarker(trc("modes.tooltip.separate_xy_sensitivity"));

                        if (mode.separateXYSensitivity) {
                            RawInputSensitivityNote();
                            ImGui::Text(trc("modes.sensitivity_x"));
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##PreemptiveSensitivityX", &mode.modeSensitivityX, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                            RawInputSensitivityNote();
                            ImGui::Text(trc("modes.sensitivity_y"));
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##PreemptiveSensitivityY", &mode.modeSensitivityY, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                        } else {
                            RawInputSensitivityNote();
                            ImGui::Text(trc("modes.sensitivity"));
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##PreemptiveSensitivity", &mode.modeSensitivity, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                            ImGui::SameLine();
                            HelpMarker(trc("modes.tooltip.sensitivity"));
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

    for (size_t i = 0; i < g_config.modes.size(); ++i) {
        auto& mode = g_config.modes[i];
        if (EqualsIgnoreCase(mode.id, "Thin")) {
            ImGui::PushID((int)i + 20000);

            bool node_open = ImGui::TreeNodeEx("##mode_node", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", mode.id.c_str());
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::GetFrameHeight());
            ImGui::Dummy(ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));

            if (node_open) {
                if (!resolutionSupported) { ImGui::BeginDisabled(); }


                bool useManualPixelSizeThin = !mode.useRelativeSize;
                if (ImGui::Checkbox((tr("modes.label.manual_pixel_size") + "##Thin").c_str(), &useManualPixelSizeThin)) {
                    mode.useRelativeSize = !useManualPixelSizeThin;
                    mode.widthExpr.clear();
                    mode.heightExpr.clear();
                    if (mode.useRelativeSize) {
                        mode.relativeWidth = (std::max)(0.01f, (std::min)(1.0f, static_cast<float>(mode.width) / static_cast<float>(screenWidth)));
                        mode.relativeHeight =
                            (std::max)(0.01f, (std::min)(1.0f, static_cast<float>(mode.height) / static_cast<float>(screenHeight)));
                        int thinMinWidthPx = 330;
                        float thinMinWidthPct = (std::min)(100.0f, (static_cast<float>(thinMinWidthPx) / static_cast<float>(screenWidth)) * 100.0f);
                        float currentWidthPct = mode.relativeWidth * 100.0f;
                        if (currentWidthPct < thinMinWidthPct) {
                            mode.relativeWidth = thinMinWidthPct / 100.0f;
                            mode.width = (std::max)(thinMinWidthPx,
                                                    static_cast<int>(mode.relativeWidth * static_cast<float>(screenWidth)));
                        }
                    } else {
                        if (mode.width < 330) { mode.width = 330; }
                        mode.relativeWidth = -1.0f;
                        mode.relativeHeight = -1.0f;
                    }
                    g_configIsDirty = true;
                }

                ImGui::Columns(2, "thin_dims", false);

                ImGui::Text(trc("label.width"));
                ImGui::NextColumn();
                if (mode.useRelativeSize) {
                    int thinMinWidthPx = 330;
                    float thinMinWidthPct = (std::min)(100.0f, (static_cast<float>(thinMinWidthPx) / static_cast<float>(screenWidth)) * 100.0f);
                    float widthPct = ((mode.relativeWidth >= 0.0f && mode.relativeWidth <= 1.0f)
                                          ? mode.relativeWidth
                                          : (static_cast<float>(mode.width) / static_cast<float>(screenWidth))) *
                                     100.0f;
                    widthPct = (std::max)(thinMinWidthPct, (std::min)(100.0f, widthPct));
                    if (ImGui::SliderFloat("##ThinWidthPercent", &widthPct, thinMinWidthPct, 100.0f, "%.1f%%")) {
                        mode.useRelativeSize = true;
                        mode.relativeWidth = widthPct / 100.0f;
                        mode.width = (std::max)(thinMinWidthPx, static_cast<int>(mode.relativeWidth * static_cast<float>(screenWidth)));
                        g_configIsDirty = true;
                        if (g_currentModeId == mode.id) {
                            HWND hwnd = g_minecraftHwnd.load();
                            if (hwnd) {
                                RetargetActiveModeTransition(mode);
                                PublishGuiConfigSnapshot();
                                RequestWindowClientResize(hwnd, mode.width, mode.height, "gui:thin_width_slider");
                            }
                        }
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%d px)", mode.width);
                } else {
                    int tempWidth3 = mode.width;
                    if (Spinner("##Width", &tempWidth3, 1, 330, screenWidth)) {
                        applyManualModeDimensionChange(mode, true, tempWidth3, 330, "gui:thin_width_spinner");
                    }
                }
                ImGui::NextColumn();
                ImGui::Text(trc("label.height"));
                ImGui::NextColumn();
                if (mode.useRelativeSize) {
                    float heightPct = ((mode.relativeHeight >= 0.0f && mode.relativeHeight <= 1.0f)
                                           ? mode.relativeHeight
                                           : (static_cast<float>(mode.height) / static_cast<float>(screenHeight))) *
                                      100.0f;
                    heightPct = (std::max)(1.0f, (std::min)(100.0f, heightPct));
                    if (ImGui::SliderFloat("##ThinHeightPercent", &heightPct, 1.0f, 100.0f, "%.1f%%")) {
                        mode.useRelativeSize = true;
                        mode.relativeHeight = heightPct / 100.0f;
                        mode.height = (std::max)(1, static_cast<int>(mode.relativeHeight * static_cast<float>(screenHeight)));
                        g_configIsDirty = true;
                        if (g_currentModeId == mode.id) {
                            HWND hwnd = g_minecraftHwnd.load();
                            if (hwnd) {
                                RetargetActiveModeTransition(mode);
                                PublishGuiConfigSnapshot();
                                RequestWindowClientResize(hwnd, mode.width, mode.height, "gui:thin_height_slider");
                            }
                        }
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%d px)", mode.height);
                } else {
                    int tempHeight3 = mode.height;
                    if (Spinner("##Height", &tempHeight3, 1, 1, screenHeight)) {
                        applyManualModeDimensionChange(mode, false, tempHeight3, 1, "gui:thin_height_spinner");
                    }
                }
                ImGui::Columns(1);

                if (ImGui::Button((tr("modes.switch_to_this_mode") + "##Thin").c_str())) {
                    std::lock_guard<std::mutex> pendingLock(g_pendingModeSwitchMutex);
                    g_pendingModeSwitch.pending = true;
                    g_pendingModeSwitch.modeId = mode.id;
                    g_pendingModeSwitch.source = "GUI Thin mode";
                }

                if (!resolutionSupported) { ImGui::EndDisabled(); }

                if (g_currentModeId == mode.id) {
                    ImGui::SameLine();
                    ImGui::TextDisabled(trc("label.current"));
                }

                ImGui::Separator();
                if (ImGui::TreeNode(trc("modes.transition_settings"))) {
                    RenderTransitionSettingsHorizontal(mode, "Thin");
                    if (ImGui::Checkbox(trc("modes.slide_mirrors_in"), &mode.slideMirrorsIn)) { g_configIsDirty = true; }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Mirrors slide in from the screen edges instead of appearing instantly");
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode(trc("modes.background"))) {
                    if (ImGui::RadioButton(trc("modes.color"), mode.background.selectedMode == "color")) {
                        mode.background.selectedMode = "color";
                        g_configIsDirty = true;
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton(trc("modes.gradient"), mode.background.selectedMode == "gradient")) {
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
                    if (ImGui::RadioButton(trc("modes.image"), mode.background.selectedMode == "image")) {
                        if (mode.background.selectedMode != "image") {
                            mode.background.selectedMode = "image";
                            g_configIsDirty = true;
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
                        if (ImGui::SliderFloat((tr("modes.gradient_angle") + "##bgGradAngleThin").c_str(), &mode.background.gradientAngle, 0.0f, 360.0f, "%.0f deg")) {
                            g_configIsDirty = true;
                        }
                        ImGui::Text(trc("modes.color_stops"));
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
                            if (ImGui::Button(trc("modes.gradient_add_color_stop"))) {
                                GradientColorStop newStop;
                                newStop.position = 0.5f;
                                newStop.color = { 0.5f, 0.5f, 0.5f };
                                mode.background.gradientStops.push_back(newStop);
                                std::sort(mode.background.gradientStops.begin(), mode.background.gradientStops.end(),
                                          [](const GradientColorStop& a, const GradientColorStop& b) { return a.position < b.position; });
                                g_configIsDirty = true;
                            }
                        }

                        ImGui::Separator();
                        ImGui::Text(trc("modes.gradient_animation"));
                        const char* animTypeNamesThin[] = { trc("modes.gradient_animation_none"), trc("modes.gradient_animation_rotate"),
                                                           trc("modes.gradient_animation_slide"), trc("modes.gradient_animation_wave"),
                                                           trc("modes.gradient_animation_spiral"), trc("modes.gradient_animation_fade") };
                        int currentAnimTypeThin = static_cast<int>(mode.background.gradientAnimation);
                        ImGui::SetNextItemWidth(120);
                        if (ImGui::Combo((tr("modes.gradient_animation_type") + "##GradAnimThin").c_str(), &currentAnimTypeThin, animTypeNamesThin, IM_ARRAYSIZE(animTypeNamesThin))) {
                            mode.background.gradientAnimation = static_cast<GradientAnimationType>(currentAnimTypeThin);
                            g_configIsDirty = true;
                        }
                        if (mode.background.gradientAnimation != GradientAnimationType::None) {
                            ImGui::SetNextItemWidth(150);
                            if (ImGui::SliderFloat((tr("modes.gradient_animation_speed") + "##GradAnimSpeedThin").c_str(), &mode.background.gradientAnimationSpeed, 0.1f, 5.0f, "%.1fx")) {
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
                        if (RenderMaskedPathInput((tr("modes.bg_image_path") + "##Thin").c_str(), mode.background.image)) {
                            ClearImageError(thinErrorKey);
                            g_configIsDirty = true;
                            g_allImagesLoaded = false;
                            g_pendingImageLoad = true;
                        }
                        ImGui::SameLine();
                        if (ImGui::Button((tr("button.browse") + "##thin_bg").c_str())) {
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
                        renderBackgroundImageOptions(mode, "thin_bg_fit");
                        std::string thinBgError = GetImageError(thinErrorKey);
                        if (!thinBgError.empty()) { ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", thinBgError.c_str()); }
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode(trc("modes.border_settings"))) {
                    if (ImGui::Checkbox(trc("modes.enable_border"), &mode.border.enabled)) { g_configIsDirty = true; }
                    if (mode.border.enabled) {
                        ImGui::Text(trc("modes.color"));
                        ImVec4 borderCol = ImVec4(mode.border.color.r, mode.border.color.g, mode.border.color.b, 1.0f);
                        if (ImGui::ColorEdit3("##BorderColorThin", (float*)&borderCol, ImGuiColorEditFlags_NoInputs)) {
                            mode.border.color = { borderCol.x, borderCol.y, borderCol.z };
                            g_configIsDirty = true;
                        }
                        ImGui::Text(trc("images.border_width"));
                        if (Spinner("##BorderWidthThin", &mode.border.width, 1, 1, 50)) { g_configIsDirty = true; }
                            ImGui::Text(trc("images.border_radius"));
                            if (Spinner("##BorderRadiusThin", &mode.border.radius, 1, 0, 100)) { g_configIsDirty = true; }
                    }
                    ImGui::TreePop();
                }

                renderModeSourceAssignments(mode, mode.id);
                if (ImGui::TreeNode(trc("modes.sensitivity_override"))) {
                    if (ImGui::Checkbox(trc("modes.override_sensitivity"), &mode.sensitivityOverrideEnabled)) { g_configIsDirty = true; }
                    HelpMarker(trc("modes.tooltip.override_sensitivity"));

                    if (mode.sensitivityOverrideEnabled) {
                        if (ImGui::Checkbox(trc("modes.separate_xy_sensitivity"), &mode.separateXYSensitivity)) {
                            g_configIsDirty = true;
                            if (mode.separateXYSensitivity) {
                                mode.modeSensitivityX = mode.modeSensitivity;
                                mode.modeSensitivityY = mode.modeSensitivity;
                            }
                        }
                        ImGui::SameLine();
                        HelpMarker(trc("modes.tooltip.separate_xy_sensitivity"));

                        if (mode.separateXYSensitivity) {
                            RawInputSensitivityNote();
                            ImGui::Text(trc("modes.sensitivity_x"));
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##ThinSensitivityX", &mode.modeSensitivityX, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                            RawInputSensitivityNote();
                            ImGui::Text(trc("modes.sensitivity_y"));
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##ThinSensitivityY", &mode.modeSensitivityY, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                        } else {
                            RawInputSensitivityNote();
                            ImGui::Text(trc("modes.sensitivity"));
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##ThinSensitivity", &mode.modeSensitivity, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                            ImGui::SameLine();
                            HelpMarker(trc("modes.tooltip.sensitivity"));
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

    for (size_t i = 0; i < g_config.modes.size(); ++i) {
        auto& mode = g_config.modes[i];
        if (EqualsIgnoreCase(mode.id, "Wide")) {
            ImGui::PushID((int)i + 30000);

            bool node_open = ImGui::TreeNodeEx("##mode_node", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", mode.id.c_str());
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::GetFrameHeight());
            ImGui::Dummy(ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));

            if (node_open) {
                if (!resolutionSupported) { ImGui::BeginDisabled(); }

                bool useManualPixelSizeWide = !mode.useRelativeSize;
                if (ImGui::Checkbox((tr("modes.label.manual_pixel_size") + "##Wide").c_str(), &useManualPixelSizeWide)) {
                    mode.useRelativeSize = !useManualPixelSizeWide;
                    mode.widthExpr.clear();
                    mode.heightExpr.clear();
                    if (mode.useRelativeSize) {
                        mode.relativeWidth = (std::max)(0.01f, (std::min)(1.0f, static_cast<float>(mode.width) / static_cast<float>(screenWidth)));
                        mode.relativeHeight =
                            (std::max)(0.01f, (std::min)(1.0f, static_cast<float>(mode.height) / static_cast<float>(screenHeight)));
                    } else {
                        mode.relativeWidth = -1.0f;
                        mode.relativeHeight = -1.0f;
                    }
                    g_configIsDirty = true;
                }

                ImGui::Columns(2, "wide_dims", false);

                ImGui::Text(trc("label.width"));
                ImGui::NextColumn();
                if (mode.useRelativeSize) {
                    float widthPct = ((mode.relativeWidth >= 0.0f && mode.relativeWidth <= 1.0f)
                                          ? mode.relativeWidth
                                          : (static_cast<float>(mode.width) / static_cast<float>(screenWidth))) *
                                     100.0f;
                    widthPct = (std::max)(1.0f, (std::min)(100.0f, widthPct));
                    if (ImGui::SliderFloat("##WideWidthPercent", &widthPct, 1.0f, 100.0f, "%.1f%%")) {
                        mode.useRelativeSize = true;
                        mode.relativeWidth = widthPct / 100.0f;
                        mode.width = (std::max)(1, static_cast<int>(mode.relativeWidth * static_cast<float>(screenWidth)));
                        g_configIsDirty = true;
                        if (g_currentModeId == mode.id) {
                            HWND hwnd = g_minecraftHwnd.load();
                            if (hwnd) {
                                RetargetActiveModeTransition(mode);
                                PublishGuiConfigSnapshot();
                                RequestWindowClientResize(hwnd, mode.width, mode.height, "gui:wide_width_slider");
                            }
                        }
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%d px)", mode.width);
                } else {
                    int tempWidth4 = mode.width;
                    if (Spinner("##Width", &tempWidth4, 1, 1, screenWidth)) {
                        applyManualModeDimensionChange(mode, true, tempWidth4, 1, "gui:wide_width_spinner");
                    }
                }
                ImGui::NextColumn();
                ImGui::Text(trc("label.height"));
                ImGui::NextColumn();
                if (mode.useRelativeSize) {
                    float heightPct = ((mode.relativeHeight >= 0.0f && mode.relativeHeight <= 1.0f)
                                           ? mode.relativeHeight
                                           : (static_cast<float>(mode.height) / static_cast<float>(screenHeight))) *
                                      100.0f;
                    heightPct = (std::max)(1.0f, (std::min)(100.0f, heightPct));
                    if (ImGui::SliderFloat("##WideHeightPercent", &heightPct, 1.0f, 100.0f, "%.1f%%")) {
                        mode.useRelativeSize = true;
                        mode.relativeHeight = heightPct / 100.0f;
                        mode.height = (std::max)(1, static_cast<int>(mode.relativeHeight * static_cast<float>(screenHeight)));
                        g_configIsDirty = true;
                        if (g_currentModeId == mode.id) {
                            HWND hwnd = g_minecraftHwnd.load();
                            if (hwnd) {
                                RetargetActiveModeTransition(mode);
                                PublishGuiConfigSnapshot();
                                RequestWindowClientResize(hwnd, mode.width, mode.height, "gui:wide_height_slider");
                            }
                        }
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%d px)", mode.height);
                } else {
                    int tempHeight4 = mode.height;
                    if (Spinner("##Height", &tempHeight4, 1, 1, screenHeight)) {
                        applyManualModeDimensionChange(mode, false, tempHeight4, 1, "gui:wide_height_spinner");
                    }
                }
                ImGui::Columns(1);

                if (ImGui::Button((tr("modes.switch_to_this_mode") + "##Wide").c_str())) {
                    std::lock_guard<std::mutex> pendingLock(g_pendingModeSwitchMutex);
                    g_pendingModeSwitch.pending = true;
                    g_pendingModeSwitch.modeId = mode.id;
                    g_pendingModeSwitch.source = "GUI Wide mode";
                }

                if (!resolutionSupported) { ImGui::EndDisabled(); }

                if (g_currentModeId == mode.id) {
                    ImGui::SameLine();
                    ImGui::TextDisabled(trc("label.current"));
                }

                ImGui::Separator();
                if (ImGui::TreeNode(trc("modes.transition_settings"))) {
                    RenderTransitionSettingsHorizontal(mode, "Wide");
                    if (ImGui::Checkbox(trc("modes.slide_mirrors_in"), &mode.slideMirrorsIn)) { g_configIsDirty = true; }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Mirrors slide in from the screen edges instead of appearing instantly");
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode(trc("modes.background"))) {
                    if (ImGui::RadioButton(trc("modes.color"), mode.background.selectedMode == "color")) {
                        mode.background.selectedMode = "color";
                        g_configIsDirty = true;
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton(trc("modes.gradient"), mode.background.selectedMode == "gradient")) {
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
                    if (ImGui::RadioButton(trc("modes.image"), mode.background.selectedMode == "image")) {
                        if (mode.background.selectedMode != "image") {
                            mode.background.selectedMode = "image";
                            g_configIsDirty = true;
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
                        if (ImGui::SliderFloat((tr("modes.gradient_angle") + "##bgGradAngleWide").c_str(), &mode.background.gradientAngle, 0.0f, 360.0f, "%.0f deg")) {
                            g_configIsDirty = true;
                        }
                        ImGui::Text(trc("modes.color_stops"));
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
                            if (ImGui::Button(trc("modes.gradient_add_color_stop"))) {
                                GradientColorStop newStop;
                                newStop.position = 0.5f;
                                newStop.color = { 0.5f, 0.5f, 0.5f };
                                mode.background.gradientStops.push_back(newStop);
                                std::sort(mode.background.gradientStops.begin(), mode.background.gradientStops.end(),
                                          [](const GradientColorStop& a, const GradientColorStop& b) { return a.position < b.position; });
                                g_configIsDirty = true;
                            }
                        }

                        ImGui::Separator();
                        ImGui::Text(trc("modes.gradient_animation"));
                        const char* animTypeNamesWide[] = { trc("modes.gradient_animation_none"), trc("modes.gradient_animation_rotate"),
                                                           trc("modes.gradient_animation_slide"), trc("modes.gradient_animation_wave"),
                                                           trc("modes.gradient_animation_spiral"), trc("modes.gradient_animation_fade") };
                        int currentAnimTypeWide = static_cast<int>(mode.background.gradientAnimation);
                        ImGui::SetNextItemWidth(120);
                        if (ImGui::Combo((tr("modes.gradient_animation_type") + "##GradAnimWide").c_str(), &currentAnimTypeWide, animTypeNamesWide, IM_ARRAYSIZE(animTypeNamesWide))) {
                            mode.background.gradientAnimation = static_cast<GradientAnimationType>(currentAnimTypeWide);
                            g_configIsDirty = true;
                        }
                        if (mode.background.gradientAnimation != GradientAnimationType::None) {
                            ImGui::SetNextItemWidth(150);
                            if (ImGui::SliderFloat((tr("modes.gradient_animation_speed") + "##GradAnimSpeedWide").c_str(), &mode.background.gradientAnimationSpeed, 0.1f, 5.0f, "%.1fx")) {
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
                        if (RenderMaskedPathInput((tr("modes.bg_image_path") + "##Wide").c_str(), mode.background.image)) {
                            ClearImageError(wideErrorKey);
                            g_configIsDirty = true;
                            g_allImagesLoaded = false;
                            g_pendingImageLoad = true;
                        }
                        ImGui::SameLine();
                        if (ImGui::Button((tr("button.browse") + "##wide_bg").c_str())) {
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
                        renderBackgroundImageOptions(mode, "wide_bg_fit");
                        std::string wideBgError = GetImageError(wideErrorKey);
                        if (!wideBgError.empty()) { ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", wideBgError.c_str()); }
                    }
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode(trc("modes.border_settings"))) {
                    if (ImGui::Checkbox(trc("modes.enable_border"), &mode.border.enabled)) { g_configIsDirty = true; }
                    if (mode.border.enabled) {
                        ImGui::Text(trc("modes.color"));
                        ImVec4 borderCol = ImVec4(mode.border.color.r, mode.border.color.g, mode.border.color.b, 1.0f);
                        if (ImGui::ColorEdit3("##BorderColorWide", (float*)&borderCol, ImGuiColorEditFlags_NoInputs)) {
                            mode.border.color = { borderCol.x, borderCol.y, borderCol.z };
                            g_configIsDirty = true;
                        }
                        ImGui::Text(trc("images.border_width"));
                        if (Spinner("##BorderWidthWide", &mode.border.width, 1, 1, 50)) { g_configIsDirty = true; }
                            ImGui::Text(trc("images.border_radius"));
                            if (Spinner("##BorderRadiusWide", &mode.border.radius, 1, 0, 100)) { g_configIsDirty = true; }
                    }
                    ImGui::TreePop();
                }

                renderModeSourceAssignments(mode, mode.id);

                if (ImGui::TreeNode(trc("modes.sensitivity_override"))) {
                    if (ImGui::Checkbox(trc("modes.override_sensitivity"), &mode.sensitivityOverrideEnabled)) { g_configIsDirty = true; }
                    HelpMarker(trc("modes.tooltip.override_sensitivity"));

                    if (mode.sensitivityOverrideEnabled) {
                        if (ImGui::Checkbox(trc("modes.separate_xy_sensitivity"), &mode.separateXYSensitivity)) {
                            g_configIsDirty = true;
                            if (mode.separateXYSensitivity) {
                                mode.modeSensitivityX = mode.modeSensitivity;
                                mode.modeSensitivityY = mode.modeSensitivity;
                            }
                        }
                        ImGui::SameLine();
                        HelpMarker(trc("modes.tooltip.separate_xy_sensitivity"));

                        if (mode.separateXYSensitivity) {
                            RawInputSensitivityNote();
                            ImGui::Text(trc("modes.sensitivity_x"));
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##WideSensitivityX", &mode.modeSensitivityX, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                            RawInputSensitivityNote();
                            ImGui::Text(trc("modes.sensitivity_y"));
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##WideSensitivityY", &mode.modeSensitivityY, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                        } else {
                            RawInputSensitivityNote();
                            ImGui::Text(trc("modes.sensitivity"));
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##WideSensitivity", &mode.modeSensitivity, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                            ImGui::SameLine();
                            HelpMarker(trc("modes.tooltip.sensitivity"));
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

    ImGui::SeparatorText(trc("modes.custom_modes"));

    for (size_t i = 0; i < g_config.modes.size(); ++i) {
        auto& mode = g_config.modes[i];
        if (!IsHardcodedMode(mode.id)) {
            ImGui::PushID((int)i);

            if (!resolutionSupported) { ImGui::BeginDisabled(); }

            std::string delete_button_label = "X##delete_mode_" + std::to_string(i);
            std::string delete_popup_id = tr("modes.delete_mode") + "###delete_mode_" + std::to_string(i);
            if (ImGui::Button(delete_button_label.c_str(), ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()))) {
                ImGui::OpenPopup(delete_popup_id.c_str());
            }

            if (!resolutionSupported) { ImGui::EndDisabled(); }

            // Popup modal outside of node_open block so it can be displayed even when collapsed
            if (ImGui::BeginPopupModal(delete_popup_id.c_str(), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
                const std::string delete_mode_confirm = tr("modes.delete_mode_confirm", mode.id);
                ImGui::TextWrapped("%s", delete_mode_confirm.c_str());
                ImGui::TextUnformatted(trc("label.action_cannot_be_undone"));
                ImGui::Separator();
                if (ImGui::Button(trc("button.ok"), ImVec2(120, 0))) {
                    mode_to_remove = (int)i;
                    g_configIsDirty = true;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SetItemDefaultFocus();
                ImGui::SameLine();
                if (ImGui::Button(trc("button.cancel"), ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
                ImGui::EndPopup();
            }

            ImGui::SameLine();
            bool node_open = ImGui::TreeNodeEx("##mode_node", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", mode.id.c_str());

            if (node_open) {
                ImGui::Text(trc("label.name"));
                ImGui::SetNextItemWidth(250);

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
                    bool newIsReserved = IsHardcodedMode(mode.id);
                    if (!HasDuplicateModeName(mode.id, i) && !newIsReserved) {
                        g_configIsDirty = true;
                    } else {
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

                int modeScreenW = GetCachedWindowWidth();
                int modeScreenH = GetCachedWindowHeight();
                if (modeScreenW < 1) modeScreenW = 1;
                if (modeScreenH < 1) modeScreenH = 1;

                bool useManualPixelSize = !mode.useRelativeSize;
                if (ImGui::Checkbox((tr("modes.label.manual_pixel_size") + "##CustomMode").c_str(), &useManualPixelSize)) {
                    mode.useRelativeSize = !useManualPixelSize;
                    mode.widthExpr.clear();
                    mode.heightExpr.clear();

                    if (mode.useRelativeSize) {
                        float computedRelativeWidth = static_cast<float>(mode.width) / static_cast<float>(modeScreenW);
                        float computedRelativeHeight = static_cast<float>(mode.height) / static_cast<float>(modeScreenH);

                        mode.relativeWidth = (std::max)(0.01f, (std::min)(1.0f, computedRelativeWidth));
                        mode.relativeHeight = (std::max)(0.01f, (std::min)(1.0f, computedRelativeHeight));

                        int newWidth = static_cast<int>(mode.relativeWidth * static_cast<float>(modeScreenW));
                        int newHeight = static_cast<int>(mode.relativeHeight * static_cast<float>(modeScreenH));
                        if (newWidth < 1) newWidth = 1;
                        if (newHeight < 1) newHeight = 1;
                        mode.width = newWidth;
                        mode.height = newHeight;
                    } else {
                        mode.relativeWidth = -1.0f;
                        mode.relativeHeight = -1.0f;
                    }

                    g_configIsDirty = true;
                }
                ImGui::SameLine();
                HelpMarker("Off = mode size is entered as a % of current game window size.\nOn = mode size is entered directly in pixels.");

                ImGui::Columns(2, "dims", false);

                ImGui::Text(trc("label.width"));
                ImGui::NextColumn();
                if (mode.useRelativeSize) {
                    float widthPct = ((mode.relativeWidth >= 0.0f && mode.relativeWidth <= 1.0f)
                                          ? mode.relativeWidth
                                          : (static_cast<float>(mode.width) / static_cast<float>(modeScreenW))) *
                                     100.0f;
                    widthPct = (std::max)(1.0f, (std::min)(100.0f, widthPct));

                    if (ImGui::SliderFloat("##WidthPercent", &widthPct, 1.0f, 100.0f, "%.1f%%")) {
                        mode.useRelativeSize = true;
                        mode.relativeWidth = widthPct / 100.0f;

                        int newWidth = static_cast<int>(mode.relativeWidth * static_cast<float>(modeScreenW));
                        if (newWidth < 1) newWidth = 1;
                        mode.width = newWidth;
                        g_configIsDirty = true;

                        if (g_currentModeId == mode.id) {
                            HWND hwnd = g_minecraftHwnd.load();
                            if (hwnd) {
                                RetargetActiveModeTransition(mode);
                                PublishGuiConfigSnapshot();
                                RequestWindowClientResize(hwnd, mode.width, mode.height, "gui:eyezoom_width_slider");
                            }
                        }
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%d px)", mode.width);
                } else {
                    int tempWidth5 = mode.width;
                    if (Spinner("##Width", &tempWidth5, 1, 1)) {
                        applyManualModeDimensionChange(mode, true, tempWidth5, 1, "gui:custom_mode_width_spinner");
                    }
                }

                ImGui::NextColumn();
                ImGui::Text(trc("label.height"));
                ImGui::NextColumn();
                if (mode.useRelativeSize) {
                    float heightPct = ((mode.relativeHeight >= 0.0f && mode.relativeHeight <= 1.0f)
                                           ? mode.relativeHeight
                                           : (static_cast<float>(mode.height) / static_cast<float>(modeScreenH))) *
                                      100.0f;
                    heightPct = (std::max)(1.0f, (std::min)(100.0f, heightPct));

                    if (ImGui::SliderFloat("##HeightPercent", &heightPct, 1.0f, 100.0f, "%.1f%%")) {
                        mode.useRelativeSize = true;
                        mode.relativeHeight = heightPct / 100.0f;

                        int newHeight = static_cast<int>(mode.relativeHeight * static_cast<float>(modeScreenH));
                        if (newHeight < 1) newHeight = 1;
                        mode.height = newHeight;
                        g_configIsDirty = true;

                        if (g_currentModeId == mode.id) {
                            HWND hwnd = g_minecraftHwnd.load();
                            if (hwnd) {
                                RetargetActiveModeTransition(mode);
                                PublishGuiConfigSnapshot();
                                RequestWindowClientResize(hwnd, mode.width, mode.height, "gui:eyezoom_height_slider");
                            }
                        }
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%d px)", mode.height);
                } else {
                    int tempHeight5 = mode.height;
                    if (Spinner("##Height", &tempHeight5, 1, 1)) {
                        applyManualModeDimensionChange(mode, false, tempHeight5, 1, "gui:custom_mode_height_spinner");
                    }
                }
                ImGui::Columns(1);

                if (ImGui::Button(trc("modes.switch_to_this_mode"))) {
                    // Defer mode switch to avoid deadlock (g_configMutex is held during GUI rendering)
                    std::lock_guard<std::mutex> pendingLock(g_pendingModeSwitchMutex);
                    g_pendingModeSwitch.pending = true;
                    g_pendingModeSwitch.modeId = mode.id;
                    g_pendingModeSwitch.source = "GUI mode detail";
                    Log("[GUI] Deferred mode switch to: " + mode.id);
                }

                ImGui::Separator();
                if (ImGui::TreeNode(trc("modes.transition_settings"))) {
                    RenderTransitionSettingsHorizontal(mode, "CustomMode");
                    if (ImGui::Checkbox(trc("modes.slide_mirrors_in"), &mode.slideMirrorsIn)) { g_configIsDirty = true; }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Mirrors slide in from the screen edges instead of appearing instantly");
                    }
                    ImGui::TreePop();
                }
                ImGui::Separator();
                ;

                if (ImGui::TreeNode(trc("modes.border_settings"))) {
                    if (ImGui::Checkbox(trc("modes.enable_border"), &mode.border.enabled)) { g_configIsDirty = true; }
                    ImGui::SameLine();
                    HelpMarker("Draw a border around the game viewport. Border appears outside the game area.");

                    if (mode.border.enabled) {
                        ImGui::Text(trc("modes.color"));
                        ImVec4 borderCol = ImVec4(mode.border.color.r, mode.border.color.g, mode.border.color.b, 1.0f);
                        if (ImGui::ColorEdit3("##BorderColorCustom", (float*)&borderCol, ImGuiColorEditFlags_NoInputs)) {
                            mode.border.color = { borderCol.x, borderCol.y, borderCol.z };
                            g_configIsDirty = true;
                        }

                        ImGui::Text(trc("images.border_width"));
                        ImGui::SetNextItemWidth(100);
                        if (Spinner("##BorderWidthCustom", &mode.border.width, 1, 1, 50)) { g_configIsDirty = true; }
                        ImGui::SameLine();
                        ImGui::TextDisabled(trc("label.px"));

                        ImGui::Text(trc("images.border_radius"));
                        ImGui::SetNextItemWidth(100);
                        if (Spinner("##BorderRadiusCustom", &mode.border.radius, 1, 0, 100)) { g_configIsDirty = true; }
                        ImGui::SameLine();
                        ImGui::TextDisabled(trc("label.px"));
                    }
                    ImGui::TreePop();
                }
                ImGui::Separator();

                if (ImGui::TreeNode(trc("modes.background"))) {
                    if (ImGui::RadioButton(trc("modes.color"), mode.background.selectedMode == "color")) {
                        if (mode.background.selectedMode != "color") {
                            mode.background.selectedMode = "color";
                            g_configIsDirty = true;
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton(trc("modes.gradient"), mode.background.selectedMode == "gradient")) {
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
                    if (ImGui::RadioButton(trc("modes.image"), mode.background.selectedMode == "image")) {
                        if (mode.background.selectedMode != "image") {
                            mode.background.selectedMode = "image";
                            g_configIsDirty = true;
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
                        if (ImGui::SliderFloat((tr("modes.gradient_angle") + gradId).c_str(), &mode.background.gradientAngle, 0.0f, 360.0f, "%.0f deg")) {
                            g_configIsDirty = true;
                        }
                        ImGui::Text(trc("modes.color_stops"));
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
                            if (ImGui::Button((tr("modes.gradient_add_color_stop") + gradId).c_str())) {
                                GradientColorStop newStop;
                                newStop.position = 0.5f;
                                newStop.color = { 0.5f, 0.5f, 0.5f };
                                mode.background.gradientStops.push_back(newStop);
                                std::sort(mode.background.gradientStops.begin(), mode.background.gradientStops.end(),
                                          [](const GradientColorStop& a, const GradientColorStop& b) { return a.position < b.position; });
                                g_configIsDirty = true;
                            }
                        }

                        ImGui::Separator();
                        ImGui::Text(trc("modes.gradient_animation"));
                        const char* animTypeNamesCustom[] = { trc("modes.gradient_animation_none"), trc("modes.gradient_animation_rotate"),
                                                             trc("modes.gradient_animation_slide"), trc("modes.gradient_animation_wave"),
                                                             trc("modes.gradient_animation_spiral"), trc("modes.gradient_animation_fade") };
                        int currentAnimTypeCustom = static_cast<int>(mode.background.gradientAnimation);
                        ImGui::SetNextItemWidth(120);
                        if (ImGui::Combo((tr("modes.gradient_animation_type") + gradId).c_str(), &currentAnimTypeCustom, animTypeNamesCustom, IM_ARRAYSIZE(animTypeNamesCustom))) {
                            mode.background.gradientAnimation = static_cast<GradientAnimationType>(currentAnimTypeCustom);
                            g_configIsDirty = true;
                        }
                        if (mode.background.gradientAnimation != GradientAnimationType::None) {
                            ImGui::SetNextItemWidth(150);
                            if (ImGui::SliderFloat((tr("modes.gradient_animation_speed") + gradId).c_str(), &mode.background.gradientAnimationSpeed, 0.1f, 5.0f, "%.1fx")) {
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
                        if (RenderMaskedPathInput(trc("modes.bg_image_path"), mode.background.image)) {
                            ClearImageError(modeErrorKey);
                            g_configIsDirty = true;
                            g_allImagesLoaded = false;
                            g_pendingImageLoad = true;
                        }
                        ImGui::SameLine();
                        if (ImGui::Button(("Browse...##mode_bg_" + mode.id).c_str())) {
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

                        renderBackgroundImageOptions(mode, "custom_bg_fit_" + mode.id);

                        std::string bgError = GetImageError(modeErrorKey);
                        if (!bgError.empty()) { ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", bgError.c_str()); }
                    }
                    ImGui::TreePop();
                }

                renderModeSourceAssignments(mode, mode.id);

                if (ImGui::TreeNode("Stretch Properties")) {
                    ImGui::TextDisabled("Override the game viewport placement and size inside the window.");
                    if (ImGui::Checkbox("Enable Stretch", &mode.stretch.enabled)) g_configIsDirty = true;
                    ImGui::Columns(2, "stretch_cols", false);
                    ImGui::SetColumnWidth(0, 150);
                    ImGui::Text("X Position");
                    ImGui::NextColumn();
                    if (Spinner("##StretchX", &mode.stretch.x)) g_configIsDirty = true;
                    ImGui::SameLine();
                    if (ImGui::Button("Center H")) {
                        mode.stretch.x = (GetCachedWindowWidth() - mode.stretch.width) / 2;
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
                        mode.stretch.y = (GetCachedWindowHeight() - mode.stretch.height) / 2;
                        g_configIsDirty = true;
                    }
                    ImGui::NextColumn();
                    ImGui::Text("Height");
                    ImGui::NextColumn();
                    if (Spinner("##StretchH", &mode.stretch.height, 1, 1)) g_configIsDirty = true;
                    ImGui::Columns(1);
                    ImGui::TreePop();
                }


                if (ImGui::TreeNode(trc("modes.sensitivity_override"))) {
                    if (ImGui::Checkbox(trc("modes.override_sensitivity"), &mode.sensitivityOverrideEnabled)) { g_configIsDirty = true; }
                    HelpMarker(trc("modes.tooltip.override_sensitivity"));

                    if (mode.sensitivityOverrideEnabled) {
                        if (ImGui::Checkbox(trc("modes.separate_xy_sensitivity"), &mode.separateXYSensitivity)) {
                            g_configIsDirty = true;
                            if (mode.separateXYSensitivity) {
                                mode.modeSensitivityX = mode.modeSensitivity;
                                mode.modeSensitivityY = mode.modeSensitivity;
                            }
                        }
                        ImGui::SameLine();
                        HelpMarker(trc("modes.tooltip.separate_xy_sensitivity"));

                        if (mode.separateXYSensitivity) {
                            RawInputSensitivityNote();
                            ImGui::Text(trc("modes.sensitivity_x"));
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##ModeSensitivityX", &mode.modeSensitivityX, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                            RawInputSensitivityNote();
                            ImGui::Text(trc("modes.sensitivity_y"));
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##ModeSensitivityY", &mode.modeSensitivityY, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                        } else {
                            RawInputSensitivityNote();
                            ImGui::Text(trc("modes.sensitivity"));
                            ImGui::SetNextItemWidth(200);
                            if (ImGui::SliderFloat("##ModeSensitivity", &mode.modeSensitivity, 0.001f, 10.0f, "%.3fx")) {
                                g_configIsDirty = true;
                            }
                            ImGui::SameLine();
                            HelpMarker(trc("modes.tooltip.sensitivity"));
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
    if (mode_to_remove != -1) {
        auto& modeToDelete = g_config.modes[mode_to_remove];
        if (!IsHardcodedMode(modeToDelete.id)) {
            const std::string removedModeId = modeToDelete.id;
            std::string currentMode;
            {
                std::lock_guard<std::mutex> modeLock(g_modeIdMutex);
                currentMode = g_currentModeId;
            }
            if (EqualsIgnoreCase(currentMode, removedModeId)) {
                std::string fallbackMode = (EqualsIgnoreCase(g_config.defaultMode, removedModeId) || g_config.defaultMode.empty())
                                               ? "Fullscreen" : g_config.defaultMode;
                std::lock_guard<std::mutex> pendingLock(g_pendingModeSwitchMutex);
                g_pendingModeSwitch.pending = true;
                g_pendingModeSwitch.modeId = fallbackMode;
                g_pendingModeSwitch.source = "Mode deleted";
                g_pendingModeSwitch.isPreview = false;
                g_pendingModeSwitch.forceInstant = true;
                Log("[GUI] Mode '" + removedModeId + "' was active and is being deleted - switching to " + fallbackMode);
            }
            if (EqualsIgnoreCase(g_config.defaultMode, removedModeId)) {
                g_config.defaultMode = "Fullscreen";
            }
            g_config.modes.erase(g_config.modes.begin() + mode_to_remove);
            RemoveInvalidHotkeyModeReferences(g_config);
            ResetAllHotkeySecondaryModes();
            std::lock_guard<std::mutex> hotkeyLock(g_hotkeyMainKeysMutex);
            RebuildHotkeyMainKeys_Internal();
            g_configIsDirty = true;
        }
    }

    ImGui::Separator();

    bool openDefaultModeConfirm = false;

    ImGui::TextUnformatted(trc("modes.status.default_mode"));
    ImGui::SameLine();
    const float defaultModeComboWidth =
        (std::max)(120.0f, (std::min)(200.0f, ImGui::CalcTextSize(g_config.defaultMode.c_str()).x + ImGui::GetStyle().FramePadding.x * 10.0f));
    ImGui::SetNextItemWidth(defaultModeComboWidth);
    if (ImGui::BeginCombo("##default_mode_selector", g_config.defaultMode.c_str())) {
        for (const auto& mode : g_config.modes) {
            const bool isSelected = EqualsIgnoreCase(mode.id, g_config.defaultMode);
            if (ImGui::Selectable(mode.id.c_str(), isSelected) && !isSelected) {
                pendingDefaultModeId = mode.id;
                openDefaultModeConfirm = true;
            }
            if (isSelected) { ImGui::SetItemDefaultFocus(); }
        }
        ImGui::EndCombo();
    }

    if (openDefaultModeConfirm) {
        ImGui::OpenPopup((tr("modes.change_default_mode_confirm") + "##confirm").c_str());
    }

    if (ImGuiViewport* viewport = ImGui::GetMainViewport()) {
        ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    }
    if (ImGui::BeginPopupModal((tr("modes.change_default_mode_confirm") + "##confirm").c_str(), NULL,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s", trc("modes.change_default_mode_confirm_message", pendingDefaultModeId));
        ImGui::Separator();
        if (ImGui::Button(trc("button.ok"), ImVec2(120, 0))) {
            g_config.defaultMode = pendingDefaultModeId;
            g_configIsDirty = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(trc("button.cancel"), ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    if (!resolutionSupported) { ImGui::BeginDisabled(); }

    if (ImGui::Button(trc("modes.add_new_mode"))) {
        ModeConfig newMode;
        newMode.id = "New Mode " + std::to_string(g_config.modes.size() + 1);
        newMode.useRelativeSize = true;
        newMode.relativeWidth = 1.0f;
        newMode.relativeHeight = 1.0f;
        newMode.width = GetCachedWindowWidth();
        newMode.height = GetCachedWindowHeight();
        newMode.manualWidth = newMode.width;
        newMode.manualHeight = newMode.height;
        newMode.stretch.width = 300;
        newMode.stretch.height = GetCachedWindowHeight();
        g_config.modes.push_back(newMode);
        g_configIsDirty = true;
    }

    ImGui::SameLine();
    if (ImGui::Button((tr("button.reset_defaults") + "##modes").c_str())) { ImGui::OpenPopup(trc("modes.reset_to_defaults")); }

    if (!resolutionSupported) { ImGui::EndDisabled(); }

    if (ImGui::BeginPopupModal(trc("modes.reset_to_defaults"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f), "WARNING:");
        ImGui::Text("This will delete ALL user-created modes and restore the default modes.");
        ImGui::Text("This action cannot be undone.");
        ImGui::Separator();
        if (ImGui::Button("Confirm Reset", ImVec2(120, 0))) {
            g_config.modes = GetDefaultModes();
            g_config.eyezoom = GetDefaultEyeZoomConfig();
            SetOverlayTextFontSize(g_config.eyezoom.textFontSize);

            // (Example: Wide mode uses height = 0.25, which must be converted to pixels.)
            int screenW = GetCachedWindowWidth();
            int screenH = GetCachedWindowHeight();
            if (screenW < 1) screenW = 1;
            if (screenH < 1) screenH = 1;

            for (auto& mode : g_config.modes) {
                bool widthIsRelative = mode.relativeWidth >= 0.0f && mode.relativeWidth <= 1.0f;
                bool heightIsRelative = mode.relativeHeight >= 0.0f && mode.relativeHeight <= 1.0f;

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

            RemoveInvalidHotkeyModeReferences(g_config);
            ResetAllHotkeySecondaryModes();
            {
                std::lock_guard<std::mutex> hotkeyLock(g_hotkeyMainKeysMutex);
                RebuildHotkeyMainKeys_Internal();
            }
            RecalculateModeDimensions();

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


