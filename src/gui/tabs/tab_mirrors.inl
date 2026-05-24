if (BeginSelectableSettingsNestedTabItem(trc("tabs.mirrors"))) {
    g_currentlyEditingMirror = "";
    g_imageDragMode.store(false);
    g_windowOverlayDragMode.store(false);

    SliderCtrlClickTip();

    static std::string selectedMirrorName = "";

    static std::string selectedGroupName = "";
    static std::string activeMirrorColorPickerName = "";
    static int activeMirrorColorPickerIndex = 0;
    static bool openMirrorColorPickerPopup = false;
    static bool mirrorColorPickerEscapeRequiresRelease = false;
    static GLuint s_mirrorColorPickerReadFbo = 0;

    constexpr float kSettingsLabelColumnWidth = 186.0f;
    constexpr float kInlineHelpMarkerSpacing = 4.0f;

    auto normalizeCaptureAnchor = [](const std::string& relativeTo) {
        if (relativeTo.length() > 8 && relativeTo.substr(relativeTo.length() - 8) == "Viewport") {
            return relativeTo.substr(0, relativeTo.length() - 8);
        }
        if (relativeTo.length() > 6 && relativeTo.substr(relativeTo.length() - 6) == "Screen") {
            return relativeTo.substr(0, relativeTo.length() - 6);
        }
        return relativeTo;
    };

    auto getRelativeCoordsNormalized = [](const std::string& anchor, int relX, int relY, int w, int h, int containerW,
                                          int containerH, int& outX, int& outY) {
        const char firstChar = anchor.empty() ? '\0' : anchor[0];

        if (firstChar == 't') {
            outY = relY;
            outX = (anchor == "topLeft") ? relX : containerW - w - relX;
        } else if (firstChar == 'c') {
            outX = (containerW - w) / 2 + relX;
            outY = (containerH - h) / 2 + relY;
        } else if (firstChar == 'p') {
            const int pieYTop = 220;
            const int pieXLeft = 92;
            const int pieXRight = 36;
            const int baseX = (anchor == "pieLeft") ? containerW - pieXLeft : containerW - pieXRight;
            outX = baseX + relX;
            outY = containerH - pieYTop + relY;
        } else {
            outY = containerH - h - relY;
            outX = (anchor == "bottomRight") ? containerW - w - relX : relX;
        }
    };

    auto sampleReadyGamePixel = [&](int gameX, int gameY, Color& outColor) {
        const GLuint readyTexture = GetReadyGameTexture();
        const int gameW = GetReadyGameWidth();
        const int gameH = GetReadyGameHeight();
        if (readyTexture == 0 || gameW <= 0 || gameH <= 0 || gameX < 0 || gameY < 0 || gameX >= gameW || gameY >= gameH) {
            return false;
        }

        if (s_mirrorColorPickerReadFbo == 0) {
            glGenFramebuffers(1, &s_mirrorColorPickerReadFbo);
        }

        GLint previousReadFramebuffer = 0;
        GLint previousPackAlignment = 0;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
        glGetIntegerv(GL_PACK_ALIGNMENT, &previousPackAlignment);

        glBindFramebuffer(GL_READ_FRAMEBUFFER, s_mirrorColorPickerReadFbo);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, readyTexture, 0);
        if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousReadFramebuffer));
            glPixelStorei(GL_PACK_ALIGNMENT, previousPackAlignment);
            return false;
        }

        unsigned char pixel[4] = { 0, 0, 0, 255 };
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(gameX, gameH - gameY - 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);

        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousReadFramebuffer));
        glPixelStorei(GL_PACK_ALIGNMENT, previousPackAlignment);

        outColor = {
            pixel[0] / 255.0f,
            pixel[1] / 255.0f,
            pixel[2] / 255.0f,
            pixel[3] / 255.0f,
        };
        return true;
    };

    int mirror_to_remove = -1;
    for (size_t i = 0; i < g_config.mirrors.size(); ++i) {
        auto& mirror = g_config.mirrors[i];
        ImGui::PushID((int)i);

        if (mirrorColorPickerEscapeRequiresRelease && !ImGui::IsKeyDown(ImGuiKey_Escape)) {
            mirrorColorPickerEscapeRequiresRelease = false;
        }

        bool is_selected = (selectedMirrorName == mirror.name);
        ImGuiTreeNodeFlags node_flags = ImGuiTreeNodeFlags_SpanAvailWidth | (is_selected ? ImGuiTreeNodeFlags_Selected : 0);

        std::string popup_id = (tr("mirrors.delete_mirror") + "##" + std::to_string(i));
        std::string delete_button_label = "X##delete_mirror_" + std::to_string(i);
        if (ImGui::Button(delete_button_label.c_str(), ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()))) {
            ImGui::OpenPopup(popup_id.c_str());
        }

        // Popup modal outside of node_open block so it can be displayed even when collapsed
        if (ImGui::BeginPopupModal(popup_id.c_str(), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            const std::string deleteMirrorConfirm = tr("mirrors.delete_mirror_confirm", mirror.name.c_str());
            ImGui::TextWrapped("%s", deleteMirrorConfirm.c_str());
            ImGui::Separator();
            if (ImGui::Button(trc("button.ok"), ImVec2(120, 0))) {
                if (selectedMirrorName == mirror.name) selectedMirrorName = "";
                mirror_to_remove = (int)i;
                g_configIsDirty = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetItemDefaultFocus();
            ImGui::SameLine();
            if (ImGui::Button(trc("label.cancel"), ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }

        ImGui::SameLine();

        std::string oldMirrorName = mirror.name;

        bool node_open = ImGui::TreeNodeEx("##mirror_node", node_flags, "%s", mirror.name.c_str());

        if (ImGui::IsItemClicked(0)) { selectedMirrorName = mirror.name; }

        if (node_open) {

            ImGui::Text(trc("mirrors.name"));
            ImGui::SameLine();

            bool hasDuplicate = HasDuplicateMirrorName(mirror.name, i);
            if (hasDuplicate) {
                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.6f, 0.2f, 0.2f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.7f, 0.3f, 0.3f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.8f, 0.3f, 0.3f, 1.0f));
            }

            if (ImGui::InputText("##Name", &mirror.name)) {
                if (!HasDuplicateMirrorName(mirror.name, i)) {
                    g_configIsDirty = true;
                    if (oldMirrorName != mirror.name) {
                        for (auto& mode : g_config.modes) {
                            for (auto& mirrorId : mode.mirrorIds) {
                                if (mirrorId == oldMirrorName) { mirrorId = mirror.name; }
                            }
                        }
                        for (auto& group : g_config.mirrorGroups) {
                            for (auto& item : group.mirrors) {
                                if (item.mirrorId == oldMirrorName) { item.mirrorId = mirror.name; }
                            }
                        }
                        {
                            std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex);
                            auto it = g_mirrorInstances.find(oldMirrorName);
                            if (it != g_mirrorInstances.end()) {
                                auto node = g_mirrorInstances.extract(it);
                                node.key() = mirror.name;
                                node.mapped().cachedRenderState.isValid = false;
                                node.mapped().forceUpdateFrames = 3;
                                g_mirrorInstances.insert(std::move(node));
                            }
                        }
                        // Update g_threadedMirrorConfigs so capture thread uses new name
                        {
                            std::lock_guard<std::mutex> lock(g_threadedMirrorConfigMutex);
                            for (auto& conf : g_threadedMirrorConfigs) {
                                if (conf.name == oldMirrorName) {
                                    conf.name = mirror.name;
                                    break;
                                }
                            }
                        }
                        if (selectedMirrorName == oldMirrorName) { selectedMirrorName = mirror.name; }
                    }
                } else {
                    mirror.name = oldMirrorName;
                }
            }

            if (hasDuplicate) { ImGui::PopStyleColor(3); }

            if (hasDuplicate) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), trc("mirrors.name_duplicate"));
            }

            auto syncMirrorCaptureSettings = [&]() {
                UpdateMirrorCaptureSettings(mirror.name, mirror.captureWidth, mirror.captureHeight, mirror.border, mirror.colors,
                                            mirror.colorSensitivity, mirror.rawOutput, mirror.colorPassthrough,
                                            mirror.gradientOutput, mirror.gradient);
                std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex);
                auto it = g_mirrorInstances.find(mirror.name);
                if (it != g_mirrorInstances.end()) it->second.forceUpdateFrames = 3;
            };

            auto syncMirrorOutputPosition = [&]() {
                UpdateMirrorOutputPosition(mirror.name, mirror.output.x, mirror.output.y, mirror.output.scale,
                                           mirror.output.separateScale, mirror.output.scaleX, mirror.output.scaleY,
                                           mirror.output.relativeTo);
                std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex);
                auto it = g_mirrorInstances.find(mirror.name);
                if (it != g_mirrorInstances.end()) { it->second.cachedRenderState.isValid = false; }
            };

            auto syncMirrorInputRegions = [&]() {
                UpdateMirrorInputRegions(mirror.name, mirror.input);
                std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex);
                auto it = g_mirrorInstances.find(mirror.name);
                if (it != g_mirrorInstances.end()) it->second.forceUpdateFrames = 3;
            };

            const ImGuiTableFlags mirrorSettingsTableFlags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings;
            auto beginMirrorSettingsTable = [&](const char* id) {
                if (!ImGui::BeginTable(id, 2, mirrorSettingsTableFlags)) return false;
                ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, kSettingsLabelColumnWidth);
                ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
                return true;
            };
            auto mirrorSettingsRowLabel = [&](const char* label, const char* tooltip = nullptr) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(label);
                if (tooltip != nullptr && tooltip[0] != '\0') {
                    ImGui::SameLine(0.0f, kInlineHelpMarkerSpacing);
                    HelpMarker(tooltip);
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(240.0f);
            };

            auto drawMirrorTargetColorPickerPreview = [&](int targetColorIndex) {
                const GLuint readyTexture = GetReadyGameTexture();
                const int gameW = GetReadyGameWidth();
                const int gameH = GetReadyGameHeight();
                if (readyTexture == 0 || gameW <= 0 || gameH <= 0 || mirror.input.empty() || mirror.captureWidth <= 0 ||
                    mirror.captureHeight <= 0) {
                    ImGui::TextDisabled("%s", trc("mirrors.color_picker_unavailable"));
                    return false;
                }

                constexpr float kMagnifierSize = 110.0f;
                constexpr float kPreviewWidth = 420.0f;
                constexpr float kPreviewMinHeight = kMagnifierSize + 24.0f;
                constexpr float kPreviewTileGap = 8.0f;
                constexpr int kMagnifierRadius = 4;

                const int inputCount = static_cast<int>(mirror.input.size());
                const int gridColumns = (std::max)(1, static_cast<int>(std::ceil(std::sqrt(static_cast<float>(inputCount)))));
                const int gridRows = (std::max)(1, (inputCount + gridColumns - 1) / gridColumns);
                const float tileWidth =
                    (kPreviewWidth - kPreviewTileGap * static_cast<float>((std::max)(0, gridColumns - 1))) / static_cast<float>(gridColumns);
                const float tileHeight = tileWidth * (static_cast<float>(mirror.captureHeight) /
                                                      (std::max)(1.0f, static_cast<float>(mirror.captureWidth)));
                const float previewContentHeight =
                    tileHeight * static_cast<float>(gridRows) + kPreviewTileGap * static_cast<float>((std::max)(0, gridRows - 1));

                const float previewHeight = (std::max)(
                    kPreviewMinHeight,
                    previewContentHeight);
                const ImVec2 previewSize(kPreviewWidth, previewHeight);
                ImGui::InvisibleButton("##mirror_target_color_picker_preview", previewSize);

                ImDrawList* drawList = ImGui::GetWindowDrawList();
                const ImVec2 previewMin = ImGui::GetItemRectMin();
                const ImVec2 previewMax = ImGui::GetItemRectMax();
                const float previewContentOffsetY = (previewHeight - previewContentHeight) * 0.5f;

                drawList->AddRectFilled(previewMin, previewMax, ImGui::GetColorU32(ImGuiCol_FrameBg), 4.0f);

                std::vector<ImVec2> tileMins;
                tileMins.reserve(mirror.input.size());
                for (size_t inputIndex = 0; inputIndex < mirror.input.size(); ++inputIndex) {
                    const auto& input = mirror.input[inputIndex];
                    const int column = static_cast<int>(inputIndex) % gridColumns;
                    const int row = static_cast<int>(inputIndex) / gridColumns;
                    const ImVec2 tileMin(previewMin.x + column * (tileWidth + kPreviewTileGap),
                                         previewMin.y + previewContentOffsetY + row * (tileHeight + kPreviewTileGap));
                    const ImVec2 tileMax(tileMin.x + tileWidth, tileMin.y + tileHeight);
                    tileMins.push_back(tileMin);

                    int capX = 0;
                    int capY = 0;
                    getRelativeCoordsNormalized(normalizeCaptureAnchor(input.relativeTo), input.x, input.y, mirror.captureWidth,
                                                mirror.captureHeight, gameW, gameH, capX, capY);

                    const float u0 = static_cast<float>(capX) / gameW;
                    const float v0 = static_cast<float>(gameH - capY - mirror.captureHeight) / gameH;
                    const float u1 = static_cast<float>(capX + mirror.captureWidth) / gameW;
                    const float v1 = static_cast<float>(gameH - capY) / gameH;
                    drawList->AddImage((ImTextureID)(intptr_t)readyTexture, tileMin, tileMax, ImVec2(u0, v1), ImVec2(u1, v0));
                    drawList->AddRect(tileMin, tileMax, ImGui::GetColorU32(ImGuiCol_Border), 4.0f);
                }

                drawList->AddRect(previewMin, previewMax, ImGui::GetColorU32(ImGuiCol_Border), 4.0f);

                bool changed = false;
                if (ImGui::IsItemHovered()) {
                    const ImVec2 mousePos = ImGui::GetMousePos();
                    bool foundSample = false;
                    int hoveredGameX = 0;
                    int hoveredGameY = 0;
                    int hoveredTileIndex = -1;
                    int previewPixelX = 0;
                    int previewPixelY = 0;
                    for (size_t inputIndex = 0; inputIndex < mirror.input.size(); ++inputIndex) {
                        const ImVec2 tileMin = tileMins[inputIndex];
                        const ImVec2 tileMax(tileMin.x + tileWidth, tileMin.y + tileHeight);
                        if (mousePos.x < tileMin.x || mousePos.x >= tileMax.x || mousePos.y < tileMin.y || mousePos.y >= tileMax.y) {
                            continue;
                        }

                        const float localX = std::clamp(mousePos.x - tileMin.x, 0.0f, tileWidth - 0.001f);
                        const float localY = std::clamp(mousePos.y - tileMin.y, 0.0f, tileHeight - 0.001f);
                        previewPixelX = std::clamp(static_cast<int>((localX / tileWidth) * mirror.captureWidth), 0,
                                                   mirror.captureWidth - 1);
                        previewPixelY = std::clamp(static_cast<int>((localY / tileHeight) * mirror.captureHeight), 0,
                                                   mirror.captureHeight - 1);

                        const auto& input = mirror.input[inputIndex];
                        int capX = 0;
                        int capY = 0;
                        getRelativeCoordsNormalized(normalizeCaptureAnchor(input.relativeTo), input.x, input.y, mirror.captureWidth,
                                                    mirror.captureHeight, gameW, gameH, capX, capY);

                        const int candidateGameX = capX + previewPixelX;
                        const int candidateGameY = capY + previewPixelY;
                        if (candidateGameX < 0 || candidateGameY < 0 || candidateGameX >= gameW || candidateGameY >= gameH) {
                            continue;
                        }

                        hoveredGameX = candidateGameX;
                        hoveredGameY = candidateGameY;
                        hoveredTileIndex = static_cast<int>(inputIndex);
                        foundSample = true;
                        break;
                    }

                    if (foundSample) {
                        Color hoveredColor;
                        if (sampleReadyGamePixel(hoveredGameX, hoveredGameY, hoveredColor)) {
                            const float cellWidth = tileWidth / (std::max)(1, mirror.captureWidth);
                            const float cellHeight = tileHeight / (std::max)(1, mirror.captureHeight);
                            const ImVec2 tileMin = tileMins[static_cast<size_t>(hoveredTileIndex)];
                            const ImVec2 hoverMin(tileMin.x + previewPixelX * cellWidth, tileMin.y + previewPixelY * cellHeight);
                            const ImVec2 hoverMax(hoverMin.x + cellWidth, hoverMin.y + cellHeight);
                            drawList->AddRect(hoverMin, hoverMax, IM_COL32(255, 255, 255, 255), 0.0f, 0, 2.0f);

                            const int zoomMinX = (std::max)(hoveredGameX - kMagnifierRadius, 0);
                            const int zoomMaxX = (std::min)(hoveredGameX + kMagnifierRadius + 1, gameW);
                            const int zoomMinY = (std::max)(hoveredGameY - kMagnifierRadius, 0);
                            const int zoomMaxY = (std::min)(hoveredGameY + kMagnifierRadius + 1, gameH);

                            const ImVec2 magnifierMin(previewMin.x + 8.0f, previewMin.y + 8.0f);
                            const ImVec2 magnifierMax(magnifierMin.x + kMagnifierSize, magnifierMin.y + kMagnifierSize);
                            drawList->AddRectFilled(magnifierMin, magnifierMax, IM_COL32(12, 12, 12, 230), 4.0f);
                            drawList->AddImage((ImTextureID)(intptr_t)readyTexture, magnifierMin, magnifierMax,
                                               ImVec2(static_cast<float>(zoomMinX) / gameW, static_cast<float>(gameH - zoomMinY) / gameH),
                                               ImVec2(static_cast<float>(zoomMaxX) / gameW, static_cast<float>(gameH - zoomMaxY) / gameH));
                            const ImVec2 magnifierCenter((magnifierMin.x + magnifierMax.x) * 0.5f, (magnifierMin.y + magnifierMax.y) * 0.5f);
                            drawList->AddLine(ImVec2(magnifierCenter.x, magnifierMin.y + 2.0f),
                                              ImVec2(magnifierCenter.x, magnifierMax.y - 2.0f), IM_COL32(255, 255, 255, 220));
                            drawList->AddLine(ImVec2(magnifierMin.x + 2.0f, magnifierCenter.y),
                                              ImVec2(magnifierMax.x - 2.0f, magnifierCenter.y), IM_COL32(255, 255, 255, 220));
                            drawList->AddRect(magnifierMin, magnifierMax, ImGui::GetColorU32(ImGuiCol_Border), 4.0f);

                            const ImVec2 swatchMin(magnifierMin.x, magnifierMax.y + 6.0f);
                            const ImVec2 swatchMax(swatchMin.x + 18.0f, swatchMin.y + 18.0f);
                            drawList->AddRectFilled(swatchMin, swatchMax,
                                                    ImGui::GetColorU32(ImVec4(hoveredColor.r, hoveredColor.g, hoveredColor.b, 1.0f)), 3.0f);
                            drawList->AddRect(swatchMin, swatchMax, ImGui::GetColorU32(ImGuiCol_Border), 3.0f);

                            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                                targetColorIndex >= 0 &&
                                targetColorIndex < static_cast<int>(mirror.colors.targetColors.size())) {
                                mirror.colors.targetColors[static_cast<size_t>(targetColorIndex)] = {
                                    hoveredColor.r,
                                    hoveredColor.g,
                                    hoveredColor.b,
                                };
                                g_configIsDirty = true;
                                syncMirrorCaptureSettings();
                                changed = true;
                            }
                        }
                    }
                }

                ImGui::TextWrapped("%s", trc("mirrors.color_picker_help"));
                return changed;
            };

            const bool disableGradientOutput = mirror.colorPassthrough || mirror.rawOutput;
            const bool disableSolidOutputColor = mirror.colorPassthrough || mirror.rawOutput || mirror.gradientOutput;

            ImGui::SeparatorText(trc("mirrors.capture"));
            if (beginMirrorSettingsTable("mirror_capture_settings")) {
                mirrorSettingsRowLabel(trc("label.fps"));
                int mirrorFpsSliderValue = (mirror.fps >= kMirrorRealtimeSliderValue) ? kMirrorRealtimeSliderValue
                                                                                       : (std::max)(5, (std::min)(mirror.fps, kMirrorRealtimeSliderValue - 1));
                if (ImGui::SliderInt("##fps", &mirrorFpsSliderValue, 5, kMirrorRealtimeSliderValue,
                                     mirrorFpsSliderValue == kMirrorRealtimeSliderValue ? trc("label.realtime") : "%d fps")) {
                    mirror.fps = (mirrorFpsSliderValue == kMirrorRealtimeSliderValue) ? kMirrorRealtimeFps : mirrorFpsSliderValue;
                    g_configIsDirty = true;
                    UpdateMirrorFPS(mirror.name, mirror.fps);
                    std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex);
                    auto it = g_mirrorInstances.find(mirror.name);
                    if (it != g_mirrorInstances.end()) it->second.forceUpdateFrames = 3;
                }

                mirrorSettingsRowLabel(trc("mirrors.capture_width"));
                if (Spinner("##cap_w", &mirror.captureWidth, 1, ConfigDefaults::MIRROR_CAPTURE_MIN_DIMENSION,
                            ConfigDefaults::MIRROR_CAPTURE_MAX_DIMENSION)) {
                    g_configIsDirty = true;
                    syncMirrorCaptureSettings();
                }

                mirrorSettingsRowLabel(trc("mirrors.capture_height"));
                if (Spinner("##cap_h", &mirror.captureHeight, 1, ConfigDefaults::MIRROR_CAPTURE_MIN_DIMENSION,
                            ConfigDefaults::MIRROR_CAPTURE_MAX_DIMENSION)) {
                    g_configIsDirty = true;
                    syncMirrorCaptureSettings();
                }
                ImGui::EndTable();
            }

            ImGui::SeparatorText(trc("mirrors.matching"));
            if (beginMirrorSettingsTable("mirror_matching_settings")) {
                mirrorSettingsRowLabel(trc("mirrors.target_color"));
                if (activeMirrorColorPickerName != mirror.name && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
                    activeMirrorColorPickerIndex = 0;
                }
                if (!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
                    activeMirrorColorPickerName = mirror.name;
                }
                activeMirrorColorPickerIndex = std::clamp(activeMirrorColorPickerIndex, 0,
                                                          (std::max)(0, static_cast<int>(mirror.colors.targetColors.size()) - 1));
                int target_color_to_remove = -1;
                const std::string mirrorColorPickerPopupId = std::string("mirror_target_color_picker##") + mirror.name;
                const std::string mirrorColorPickerPopupTitle = tr("mirrors.color_picker") + "###" + mirrorColorPickerPopupId;
                for (size_t j = 0; j < mirror.colors.targetColors.size(); ++j) {
                    ImGui::PushID(static_cast<int>(j));

                    std::string color_label = tr("mirrors.color") + " " + std::to_string(j + 1);
                    float targetColorArr[3] = {
                        mirror.colors.targetColors[j].r,
                        mirror.colors.targetColors[j].g,
                        mirror.colors.targetColors[j].b,
                    };
                    ImGui::SetNextItemWidth(280.0f);
                    bool targetColorChangedByWidget = ImGui::ColorEdit3(color_label.c_str(), targetColorArr);
                    bool targetColorChanged = targetColorChangedByWidget || targetColorArr[0] != mirror.colors.targetColors[j].r ||
                                              targetColorArr[1] != mirror.colors.targetColors[j].g ||
                                              targetColorArr[2] != mirror.colors.targetColors[j].b;
                    if (targetColorChanged) {
                        mirror.colors.targetColors[j].r = targetColorArr[0];
                        mirror.colors.targetColors[j].g = targetColorArr[1];
                        mirror.colors.targetColors[j].b = targetColorArr[2];
                        g_configIsDirty = true;
                        syncMirrorCaptureSettings();
                    }

                    if (mirror.colors.targetColors.size() > 1) {
                        ImGui::SameLine();
                        if (ImGui::Button("X##remove_target_color")) { target_color_to_remove = static_cast<int>(j); }
                    }

                    ImGui::SameLine();
                    if (ImGui::Button(trc("mirrors.pick_color"))) {
                        activeMirrorColorPickerName = mirror.name;
                        activeMirrorColorPickerIndex = static_cast<int>(j);
                        openMirrorColorPickerPopup = true;
                    }

                    ImGui::PopID();
                }

                if (target_color_to_remove != -1) {
                    mirror.colors.targetColors.erase(mirror.colors.targetColors.begin() + target_color_to_remove);
                    g_configIsDirty = true;
                    syncMirrorCaptureSettings();
                }

                if (mirror.colors.targetColors.size() < 8) {
                    if (ImGui::Button(trc("mirrors.add_target_color"))) {
                        Color newColor = { 0.0f, 1.0f, 0.0f };
                        mirror.colors.targetColors.push_back(newColor);
                        activeMirrorColorPickerName = mirror.name;
                        activeMirrorColorPickerIndex = static_cast<int>(mirror.colors.targetColors.size()) - 1;
                        g_configIsDirty = true;
                        syncMirrorCaptureSettings();
                    }
                }

                if (openMirrorColorPickerPopup && activeMirrorColorPickerName == mirror.name) {
                    ImGui::OpenPopup(mirrorColorPickerPopupTitle.c_str());
                    openMirrorColorPickerPopup = false;
                }

                const ImGuiViewport* pickerViewport = ImGui::GetMainViewport();
                const ImVec2 pickerMaxSize = pickerViewport != nullptr
                    ? ImVec2(pickerViewport->WorkSize.x * 0.9f, pickerViewport->WorkSize.y * 0.9f)
                    : ImVec2(FLT_MAX, FLT_MAX);
                ImGui::SetNextWindowPos(GetSettingsGuiCenterPosition(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
                ImGui::SetNextWindowSizeConstraints(ImVec2(480.0f, 0.0f), pickerMaxSize);
                if (ImGui::BeginPopupModal(mirrorColorPickerPopupTitle.c_str(), NULL,
                                           ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
                    MarkMirrorColorPickerPopupActive();
                    bool closeColorPickerPopup = false;
                    if (ImGui::IsKeyPressed(ImGuiKey_Escape) && !mirrorColorPickerEscapeRequiresRelease) {
                        mirrorColorPickerEscapeRequiresRelease = true;
                        closeColorPickerPopup = true;
                    }

                    if (!closeColorPickerPopup) {
                        ImGui::Separator();
                        const bool pickedColor = drawMirrorTargetColorPickerPreview(activeMirrorColorPickerIndex);
                        if (pickedColor) {
                            mirrorColorPickerEscapeRequiresRelease = true;
                            closeColorPickerPopup = true;
                        }
                        ImGui::Separator();
                        if (ImGui::Button(trc("button.cancel"), ImVec2(120, 0))) {
                            mirrorColorPickerEscapeRequiresRelease = true;
                            closeColorPickerPopup = true;
                        }
                    }

                    if (closeColorPickerPopup) {
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndPopup();
                }

                mirrorSettingsRowLabel(trc("mirrors.color_sensitivity"));
                if (ImGui::SliderFloat("##mirror_color_sensitivity", &mirror.colorSensitivity, 0.001f, 1.0f)) {
                    g_configIsDirty = true;
                    syncMirrorCaptureSettings();
                }
                ImGui::EndTable();
            }

            ImGui::SeparatorText(trc("mirrors.rendering"));
            if (beginMirrorSettingsTable("mirror_render_settings")) {
                mirrorSettingsRowLabel(trc("mirrors.opacity"));
                if (ImGui::SliderFloat("##mirror_opacity", &mirror.opacity, 0.0f, 1.0f)) {
                    g_configIsDirty = true;
                    auto it = g_mirrorInstances.find(mirror.name);
                    if (it != g_mirrorInstances.end()) it->second.forceUpdateFrames = 3;
                }

                mirrorSettingsRowLabel(trc("mirrors.raw_output"));
                if (ImGui::Checkbox("##mirror_raw_output", &mirror.rawOutput)) {
                    g_configIsDirty = true;
                    syncMirrorCaptureSettings();
                    std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex);
                    auto it = g_mirrorInstances.find(mirror.name);
                    if (it != g_mirrorInstances.end()) {
                        it->second.forceUpdateFrames = 3;
                        it->second.desiredRawOutput.store(mirror.rawOutput, std::memory_order_release);
                        it->second.hasValidContent = false;
                    }
                }

                mirrorSettingsRowLabel(trc("mirrors.color_passthrough"), trc("mirrors.tooltip.color_passthrough"));
                if (ImGui::Checkbox("##mirror_color_passthrough", &mirror.colorPassthrough)) {
                    g_configIsDirty = true;
                    syncMirrorCaptureSettings();
                }

                mirrorSettingsRowLabel(trc("mirrors.gradient_output"));
                if (disableGradientOutput) { ImGui::BeginDisabled(); }
                if (ImGui::Checkbox("##mirror_gradient_output", &mirror.gradientOutput)) {
                    g_configIsDirty = true;
                    syncMirrorCaptureSettings();
                }
                if (disableGradientOutput) { ImGui::EndDisabled(); }

                mirrorSettingsRowLabel(trc("mirrors.output_color"));
                if (disableSolidOutputColor) { ImGui::BeginDisabled(); }
                float outputColorArr[4] = { mirror.colors.output.r, mirror.colors.output.g, mirror.colors.output.b, mirror.colors.output.a };
                bool outputColorChangedByWidget = ImGui::ColorEdit4("##mirror_output_color", outputColorArr, ImGuiColorEditFlags_AlphaBar);
                bool outputColorChanged = outputColorChangedByWidget || outputColorArr[0] != mirror.colors.output.r ||
                                          outputColorArr[1] != mirror.colors.output.g || outputColorArr[2] != mirror.colors.output.b ||
                                          outputColorArr[3] != mirror.colors.output.a;
                if (outputColorChanged) {
                    mirror.colors.output = { outputColorArr[0], outputColorArr[1], outputColorArr[2], outputColorArr[3] };
                    g_configIsDirty = true;
                    syncMirrorCaptureSettings();
                }
                if (disableSolidOutputColor) { ImGui::EndDisabled(); }
                ImGui::EndTable();
            }

            if (mirror.gradientOutput && !disableGradientOutput) {
                ImGui::SeparatorText(trc("mirrors.gradient_settings"));
                if (beginMirrorSettingsTable("mirror_gradient_settings")) {
                    mirrorSettingsRowLabel(trc("modes.gradient_angle"));
                    if (ImGui::SliderFloat("##mirrorGradAngle", &mirror.gradient.gradientAngle, 0.0f, 360.0f, "%.0f deg")) {
                        g_configIsDirty = true;
                        syncMirrorCaptureSettings();
                    }

                    mirrorSettingsRowLabel(trc("modes.color_stops"));
                    int gradientStopToRemove = -1;
                    for (size_t gradientIndex = 0; gradientIndex < mirror.gradient.gradientStops.size(); ++gradientIndex) {
                        ImGui::PushID(static_cast<int>(gradientIndex) + 50000);
                        auto& stop = mirror.gradient.gradientStops[gradientIndex];

                        if (ImGui::ColorEdit3("##MirrorGradStopColor", &stop.color.r, ImGuiColorEditFlags_NoInputs)) {
                            g_configIsDirty = true;
                            syncMirrorCaptureSettings();
                        }
                        ImGui::SameLine();

                        float pos = stop.position * 100.0f;
                        ImGui::SetNextItemWidth(130.0f);
                        if (ImGui::SliderFloat("##MirrorGradStopPos", &pos, 0.0f, 100.0f, "%.0f%%")) {
                            stop.position = pos / 100.0f;
                            g_configIsDirty = true;
                            syncMirrorCaptureSettings();
                        }

                        if (mirror.gradient.gradientStops.size() > 2) {
                            ImGui::SameLine();
                            if (ImGui::Button("X##MirrorGradRemoveStop")) { gradientStopToRemove = static_cast<int>(gradientIndex); }
                        }

                        ImGui::PopID();
                    }

                    if (gradientStopToRemove >= 0) {
                        mirror.gradient.gradientStops.erase(mirror.gradient.gradientStops.begin() + gradientStopToRemove);
                        g_configIsDirty = true;
                        syncMirrorCaptureSettings();
                    }

                    if (mirror.gradient.gradientStops.size() < 8) {
                        if (ImGui::Button((tr("modes.gradient_add_color_stop") + "##mirrorGradient").c_str())) {
                            GradientColorStop newStop;
                            newStop.position = 0.5f;
                            newStop.color = { 0.5f, 0.5f, 0.5f };
                            mirror.gradient.gradientStops.push_back(newStop);
                            std::sort(mirror.gradient.gradientStops.begin(), mirror.gradient.gradientStops.end(),
                                      [](const GradientColorStop& left, const GradientColorStop& right) {
                                          return left.position < right.position;
                                      });
                            g_configIsDirty = true;
                            syncMirrorCaptureSettings();
                        }
                    }

                    mirrorSettingsRowLabel(trc("modes.gradient_animation"));
                    const char* animTypeNames[] = { trc("modes.gradient_animation_none"), trc("modes.gradient_animation_rotate"),
                                                    trc("modes.gradient_animation_slide"), trc("modes.gradient_animation_wave"),
                                                    trc("modes.gradient_animation_spiral"), trc("modes.gradient_animation_fade") };
                    int currentAnimType = static_cast<int>(mirror.gradient.gradientAnimation);
                    if (ImGui::Combo("##MirrorGradAnim", &currentAnimType, animTypeNames, IM_ARRAYSIZE(animTypeNames))) {
                        mirror.gradient.gradientAnimation = static_cast<GradientAnimationType>(currentAnimType);
                        g_configIsDirty = true;
                        syncMirrorCaptureSettings();
                    }

                    if (mirror.gradient.gradientAnimation != GradientAnimationType::None) {
                        mirrorSettingsRowLabel(trc("modes.gradient_animation_speed"));
                        if (ImGui::SliderFloat("##MirrorGradAnimSpeed", &mirror.gradient.gradientAnimationSpeed, 0.1f, 5.0f, "%.1fx")) {
                            g_configIsDirty = true;
                            syncMirrorCaptureSettings();
                        }
                    }
                    ImGui::EndTable();
                }
            }

            ImGui::SeparatorText(trc("mirrors.border_settings"));
            const char* borderTypes[] = { trc("mirrors.dynamic_border"), trc("mirrors.static_border") };
            int currentType = static_cast<int>(mirror.border.type);
            if (beginMirrorSettingsTable("mirror_border_settings")) {
                mirrorSettingsRowLabel(trc("mirrors.border_settings"));
                if (ImGui::Combo("##borderType", &currentType, borderTypes, IM_ARRAYSIZE(borderTypes))) {
                    mirror.border.type = static_cast<MirrorBorderType>(currentType);
                    g_configIsDirty = true;
                    syncMirrorCaptureSettings();
                }

                if (mirror.border.type == MirrorBorderType::Dynamic) {
                    mirrorSettingsRowLabel(trc("mirrors.border_thickness"));
                    if (Spinner("##dynamicThickness", &mirror.border.dynamicThickness, 1, 0)) {
                        g_configIsDirty = true;
                        syncMirrorCaptureSettings();
                    }

                    mirrorSettingsRowLabel(trc("mirrors.border_color"));
                    float dynColorArr[4] = { mirror.colors.border.r, mirror.colors.border.g, mirror.colors.border.b, mirror.colors.border.a };
                    if (ImGui::ColorEdit4("##dynBorderColor", dynColorArr, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar)) {
                        mirror.colors.border = { dynColorArr[0], dynColorArr[1], dynColorArr[2], dynColorArr[3] };
                        g_configIsDirty = true;
                        syncMirrorCaptureSettings();
                    }
                } else {
                    mirrorSettingsRowLabel(trc("mirrors.border_thickness"));
                    if (Spinner("##staticThickness", &mirror.border.staticThickness, 1, 0)) {
                        g_configIsDirty = true;
                        syncMirrorCaptureSettings();
                    }

                    if (mirror.border.staticThickness > 0) {
                        mirrorSettingsRowLabel(trc("mirrors.border_shape"));
                        const char* shapes[] = { trc("mirrors.shape.rectangle"), trc("mirrors.shape.circle_ellipse") };
                        int currentShape = static_cast<int>(mirror.border.staticShape);
                        if (ImGui::Combo("##staticShape", &currentShape, shapes, IM_ARRAYSIZE(shapes))) {
                            mirror.border.staticShape = static_cast<MirrorBorderShape>(currentShape);
                            g_configIsDirty = true;
                            syncMirrorCaptureSettings();
                        }

                        mirrorSettingsRowLabel(trc("mirrors.color"));
                        float staticColorArr[4] = { mirror.border.staticColor.r, mirror.border.staticColor.g, mirror.border.staticColor.b,
                                                    mirror.border.staticColor.a };
                        if (ImGui::ColorEdit4("##staticColor", staticColorArr,
                                              ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar)) {
                            mirror.border.staticColor = { staticColorArr[0], staticColorArr[1], staticColorArr[2], staticColorArr[3] };
                            g_configIsDirty = true;
                            syncMirrorCaptureSettings();
                        }

                        if (mirror.border.staticShape == MirrorBorderShape::Rectangle) {
                            mirrorSettingsRowLabel(trc("mirrors.border_radius"));
                            if (Spinner("##staticRadius", &mirror.border.staticRadius, 1, 0)) {
                                g_configIsDirty = true;
                                syncMirrorCaptureSettings();
                            }
                        }

                        mirrorSettingsRowLabel(trc("mirrors.x_offset"), trc("mirrors.tooltip.position_size_offsets"));
                        if (Spinner("##staticOffsetX", &mirror.border.staticOffsetX, 1)) {
                            g_configIsDirty = true;
                            syncMirrorCaptureSettings();
                        }

                        mirrorSettingsRowLabel(trc("mirrors.y_offset"));
                        if (Spinner("##staticOffsetY", &mirror.border.staticOffsetY, 1)) {
                            g_configIsDirty = true;
                            syncMirrorCaptureSettings();
                        }

                        mirrorSettingsRowLabel(trc("mirrors.width"));
                        if (Spinner("##staticWidth", &mirror.border.staticWidth, 1, 0)) {
                            g_configIsDirty = true;
                            syncMirrorCaptureSettings();
                        }

                        mirrorSettingsRowLabel(trc("mirrors.height"));
                        if (Spinner("##staticHeight", &mirror.border.staticHeight, 1, 0)) {
                            g_configIsDirty = true;
                            syncMirrorCaptureSettings();
                        }
                    }
                }
                ImGui::EndTable();
            }

            if (mirror.border.type == MirrorBorderType::Dynamic) {
                ImGui::TextDisabled(trc("mirrors.border_dynamic_shape_overlay"));
            } else {
                ImGui::TextDisabled(trc("mirrors.border_static_shape_overlay"));
            }

            ImGui::SeparatorText(trc("mirrors.output_position"));
            ImGui::TextDisabled(trc("mirrors.tooltip.output_scale"));
            if (beginMirrorSettingsTable("mirror_output_position_settings")) {
                mirrorSettingsRowLabel(trc("mirrors.output_scale"));
                if (ImGui::Checkbox((tr("mirrors.separate_x_y") + "##scale").c_str(), &mirror.output.separateScale)) {
                    g_configIsDirty = true;
                    if (mirror.output.separateScale) {
                        mirror.output.scaleX = mirror.output.scale;
                        mirror.output.scaleY = mirror.output.scale;
                    }
                    syncMirrorOutputPosition();
                }
                ImGui::SameLine();

                if (!mirror.output.separateScale) {
                    float scalePercent = mirror.output.scale * 100.0f;
                    ImGui::SetNextItemWidth(220.0f);
                    if (ImGui::SliderFloat("##scale", &scalePercent, 10.0f, 2000.0f, "%.0f%%")) {
                        mirror.output.scale = scalePercent / 100.0f;
                        g_configIsDirty = true;
                        syncMirrorOutputPosition();
                    }
                } else {
                    float scaleXPercent = mirror.output.scaleX * 100.0f;
                    float scaleYPercent = mirror.output.scaleY * 100.0f;
                    ImGui::SetNextItemWidth(105.0f);
                    if (ImGui::SliderFloat("X##scaleX", &scaleXPercent, 10.0f, 2000.0f, "%.0f%%")) {
                        mirror.output.scaleX = scaleXPercent / 100.0f;
                        g_configIsDirty = true;
                        syncMirrorOutputPosition();
                    }
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(105.0f);
                    if (ImGui::SliderFloat("Y##scaleY", &scaleYPercent, 10.0f, 2000.0f, "%.0f%%")) {
                        mirror.output.scaleY = scaleYPercent / 100.0f;
                        g_configIsDirty = true;
                        syncMirrorOutputPosition();
                    }
                }

                mirrorSettingsRowLabel(trc("mirrors.relative_to_screen"), trc("mirrors.tooltip.relative_to_screen"));
                if (ImGui::Checkbox("##MirrorPos", &mirror.output.useRelativePosition)) {
                    g_configIsDirty = true;
                    if (mirror.output.useRelativePosition) {
                        int screenWidth = GetCachedWindowWidth();
                        int screenHeight = GetCachedWindowHeight();
                        mirror.output.relativeX = static_cast<float>(mirror.output.x) / screenWidth;
                        mirror.output.relativeY = static_cast<float>(mirror.output.y) / screenHeight;
                    }
                }

                mirrorSettingsRowLabel(trc("mirrors.relative_to"));
                const char* output_rel_to_preview = getFriendlyName(mirror.output.relativeTo, relativeToOptions);
                if (ImGui::BeginCombo("##mirror_output_relative_to", output_rel_to_preview)) {
                    for (const auto& option : relativeToOptions) {
                        if (ImGui::Selectable(option.second, mirror.output.relativeTo == option.first)) {
                            mirror.output.relativeTo = option.first;
                            g_configIsDirty = true;
                            syncMirrorOutputPosition();
                        }
                    }
                    ImGui::EndCombo();
                }

                if (mirror.output.useRelativePosition) {
                    int screenWidth = GetCachedWindowWidth();
                    int screenHeight = GetCachedWindowHeight();

                    mirrorSettingsRowLabel(trc("mirrors.x_percent"));
                    float xPercent = mirror.output.relativeX * 100.0f;
                    if (ImGui::SliderFloat("##out_x_pct", &xPercent, -100.0f, 200.0f, "%.1f%%")) {
                        mirror.output.relativeX = xPercent / 100.0f;
                        mirror.output.x = static_cast<int>(mirror.output.relativeX * screenWidth);
                        g_configIsDirty = true;
                        syncMirrorOutputPosition();
                    }

                    mirrorSettingsRowLabel(trc("mirrors.y_percent"));
                    float yPercent = mirror.output.relativeY * 100.0f;
                    if (ImGui::SliderFloat("##out_y_pct", &yPercent, -100.0f, 200.0f, "%.1f%%")) {
                        mirror.output.relativeY = yPercent / 100.0f;
                        mirror.output.y = static_cast<int>(mirror.output.relativeY * screenHeight);
                        g_configIsDirty = true;
                        syncMirrorOutputPosition();
                    }
                } else {
                    mirrorSettingsRowLabel(trc("mirrors.x_offset"));
                    if (Spinner("##out_x", &mirror.output.x)) {
                        g_configIsDirty = true;
                        syncMirrorOutputPosition();
                    }

                    mirrorSettingsRowLabel(trc("mirrors.y_offset"));
                    if (Spinner("##out_y", &mirror.output.y)) {
                        g_configIsDirty = true;
                        syncMirrorOutputPosition();
                    }
                }
                ImGui::EndTable();
            }

            ImGui::SeparatorText(trc("mirrors.input_or_capture_zones"));
            int zone_to_remove = -1;
            if (ImGui::BeginTable("zones_table", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) {
                ImGui::TableSetupColumn("X", ImGuiTableColumnFlags_WidthFixed, 150.0f);
                ImGui::TableSetupColumn("Y", ImGuiTableColumnFlags_WidthFixed, 150.0f);
                ImGui::TableSetupColumn(trc("mirrors.relative_to"), ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("##delete_col", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight() + 4.0f);
                ImGui::TableHeadersRow();
                for (size_t j = 0; j < mirror.input.size(); ++j) {
                    ImGui::PushID((int)j);
                    ImGui::TableNextRow();

                    ImGui::TableSetColumnIndex(0);
                    if (Spinner("##X", &mirror.input[j].x)) {
                        g_configIsDirty = true;
                        syncMirrorInputRegions();
                    }
                    ImGui::TableSetColumnIndex(1);
                    if (Spinner("##Y", &mirror.input[j].y)) {
                        g_configIsDirty = true;
                        syncMirrorInputRegions();
                    }
                    ImGui::TableSetColumnIndex(2);
                    ImGui::SetNextItemWidth(-1);
                    const char* zone_rel_to_preview = getFriendlyName(mirror.input[j].relativeTo, relativeToOptions);
                    if (ImGui::BeginCombo("##zonerelto", zone_rel_to_preview)) {
                        for (const auto& option : relativeToOptions) {
                            if (ImGui::Selectable(option.second, mirror.input[j].relativeTo == option.first)) {
                                mirror.input[j].relativeTo = option.first;
                                g_configIsDirty = true;
                                syncMirrorInputRegions();
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TableSetColumnIndex(3);
                    std::string delete_zone_label = "X##delete_zone_" + std::to_string(j);
                    if (ImGui::Button(delete_zone_label.c_str(), ImVec2(-1, ImGui::GetFrameHeight()))) {
                        std::string zone_popup_id = tr("mirrors.delete_zone") + "##" + std::to_string(j);
                        ImGui::OpenPopup(zone_popup_id.c_str());
                    }
                    std::string zone_popup_id = tr("mirrors.delete_zone") + "##" + std::to_string(j);
                    if (ImGui::BeginPopupModal(zone_popup_id.c_str(), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
                        const std::string deleteCaptureZoneText = tr("mirrors.delete_capture_zone", j + 1);
                        ImGui::TextWrapped("%s", deleteCaptureZoneText.c_str());
                        ImGui::Separator();
                        if (ImGui::Button(trc("button.ok"))) {
                            zone_to_remove = (int)j;
                            g_configIsDirty = true;
                            ImGui::CloseCurrentPopup();
                        }
                        ImGui::SameLine();
                        if (ImGui::Button(trc("label.cancel"))) { ImGui::CloseCurrentPopup(); }
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }

            if (zone_to_remove != -1) {
                mirror.input.erase(mirror.input.begin() + zone_to_remove);
                syncMirrorInputRegions();
            }
            if (ImGui::Button(trc("mirrors.add_new_capture_zone"))) {
                MirrorCaptureConfig newZone;
                newZone.relativeTo = "centerViewport";
                mirror.input.push_back(newZone);
                g_configIsDirty = true;
                syncMirrorInputRegions();
            }

            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (mirror_to_remove != -1) {
        std::string deletedMirrorName = g_config.mirrors[mirror_to_remove].name;
        g_config.mirrors.erase(g_config.mirrors.begin() + mirror_to_remove);
        for (auto& mode : g_config.modes) {
            auto it = std::find(mode.mirrorIds.begin(), mode.mirrorIds.end(), deletedMirrorName);
            while (it != mode.mirrorIds.end()) {
                mode.mirrorIds.erase(it);
                it = std::find(mode.mirrorIds.begin(), mode.mirrorIds.end(), deletedMirrorName);
            }
        }
        for (auto& group : g_config.mirrorGroups) {
            group.mirrors.erase(
                std::remove_if(group.mirrors.begin(), group.mirrors.end(),
                               [&deletedMirrorName](const MirrorGroupItem& item) { return item.mirrorId == deletedMirrorName; }),
                group.mirrors.end());
        }
        g_configIsDirty = true;
    }
    ImGui::Separator();
    if (ImGui::Button(trc("mirrors.add_new_mirror"))) {
        MirrorConfig newMirror;
        newMirror.name = "New Mirror " + std::to_string(g_config.mirrors.size() + 1);
        newMirror.output.relativeTo = "centerViewport";
        MirrorCaptureConfig newZone;
        newZone.relativeTo = "centerViewport";
        newMirror.input.push_back(newZone);
        g_config.mirrors.push_back(newMirror);
        g_configIsDirty = true;
        CreateMirrorGPUResources(newMirror);
    }

    ImGui::SameLine();
    if (ImGui::Button((tr("mirrors.reset_to_defaults") + "##mirrors").c_str())) { ImGui::OpenPopup(trc("mirrors.reset_mirrors_and_groups")); }

    if (ImGui::BeginPopupModal(trc("mirrors.reset_mirrors_and_groups"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f), trc("label.warning"));
        ImGui::Text(trc("mirrors.warning.reset_mirrors_and_groups"));
        ImGui::Text(trc("label.action_cannot_be_undone"));
        ImGui::Separator();
        if (ImGui::Button(trc("button.confirm_reset"), ImVec2(120, 0))) {
            g_config.mirrors = GetDefaultMirrors();
            g_config.mirrorGroups = GetDefaultMirrorGroups();

            std::vector<std::string> mirrorNames;
            mirrorNames.reserve(g_config.mirrors.size());
            for (const auto& m : g_config.mirrors) { mirrorNames.push_back(m.name); }

            std::vector<std::string> groupNames;
            groupNames.reserve(g_config.mirrorGroups.size());
            for (const auto& g : g_config.mirrorGroups) { groupNames.push_back(g.name); }

            for (auto& mode : g_config.modes) {
                mode.mirrorIds.erase(std::remove_if(mode.mirrorIds.begin(), mode.mirrorIds.end(),
                                                    [&mirrorNames](const std::string& id) {
                                                        return std::find(mirrorNames.begin(), mirrorNames.end(), id) == mirrorNames.end();
                                                    }),
                                   mode.mirrorIds.end());
                mode.mirrorGroupIds.erase(std::remove_if(mode.mirrorGroupIds.begin(), mode.mirrorGroupIds.end(),
                                                         [&groupNames](const std::string& id) {
                                                             return std::find(groupNames.begin(), groupNames.end(), id) == groupNames.end();
                                                         }),
                                        mode.mirrorGroupIds.end());
            }

            for (auto& group : g_config.mirrorGroups) {
                group.mirrors.erase(
                    std::remove_if(group.mirrors.begin(), group.mirrors.end(),
                                   [&mirrorNames](const MirrorGroupItem& item) {
                                       return std::find(mirrorNames.begin(), mirrorNames.end(), item.mirrorId) == mirrorNames.end();
                                   }),
                    group.mirrors.end());
            }

            selectedMirrorName.clear();
            selectedGroupName.clear();

            for (const auto& mirror : g_config.mirrors) {
                CreateMirrorGPUResources(mirror);
            }

            g_configIsDirty = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (ImGui::Button(trc("button.cancel"), ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    ImGui::SeparatorText(trc("mirrors.mirror_groups"));

    int group_to_remove = -1;
    for (size_t i = 0; i < g_config.mirrorGroups.size(); ++i) {
        auto& group = g_config.mirrorGroups[i];
        ImGui::PushID((int)i + 100000);

        bool is_selected = (selectedGroupName == group.name);
        ImGuiTreeNodeFlags node_flags = ImGuiTreeNodeFlags_SpanAvailWidth | (is_selected ? ImGuiTreeNodeFlags_Selected : 0);

        std::string delete_group_label = "X##delete_mirror_group_" + std::to_string(i);
        if (ImGui::Button(delete_group_label.c_str(), ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()))) {
            std::string popup_id_group = tr("mirrors.delete_group") + "##" + std::to_string(i);
            ImGui::OpenPopup(popup_id_group.c_str());
        }

        std::string popup_id_group = tr("mirrors.delete_group") + "##" + std::to_string(i);
        if (ImGui::BeginPopupModal(popup_id_group.c_str(), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            const std::string deleteGroupConfirm = tr("mirrors.delete_group_confirm", group.name.c_str());
            ImGui::TextWrapped("%s", deleteGroupConfirm.c_str());
            ImGui::Separator();
            if (ImGui::Button(trc("button.ok"), ImVec2(120, 0))) {
                if (selectedGroupName == group.name) selectedGroupName = "";
                group_to_remove = (int)i;
                g_configIsDirty = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetItemDefaultFocus();
            ImGui::SameLine();
            if (ImGui::Button(trc("button.cancel"), ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }

        ImGui::SameLine();
        bool node_open = ImGui::TreeNodeEx("##mirror_group_node", node_flags, "%s", group.name.c_str());
        if (ImGui::IsItemClicked(0)) { selectedGroupName = group.name; }

        if (node_open) {
            ImGui::Text(trc("mirrors.name"));
            ImGui::SameLine();
            std::string oldGroupName = group.name;

            bool hasDuplicateGroup = HasDuplicateMirrorGroupName(group.name, i);
            if (hasDuplicateGroup) {
                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.6f, 0.2f, 0.2f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.7f, 0.3f, 0.3f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.8f, 0.3f, 0.3f, 1.0f));
            }

            if (ImGui::InputText("##GroupName", &group.name)) {
                if (!HasDuplicateMirrorGroupName(group.name, i)) {
                    g_configIsDirty = true;
                    if (oldGroupName != group.name) {
                        for (auto& mode : g_config.modes) {
                            for (auto& groupId : mode.mirrorGroupIds) {
                                if (groupId == oldGroupName) { groupId = group.name; }
                            }
                        }
                        if (selectedGroupName == oldGroupName) { selectedGroupName = group.name; }
                    }
                } else {
                    group.name = oldGroupName;
                }
            }

            if (hasDuplicateGroup) { ImGui::PopStyleColor(3); }
            if (hasDuplicateGroup) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), trc("mirrors.name_duplicate"));
            }

            auto syncGroupOutputPosition = [&]() {
                std::vector<std::string> groupMirrorIds;
                groupMirrorIds.reserve(group.mirrors.size());
                for (const auto& item : group.mirrors) { groupMirrorIds.push_back(item.mirrorId); }
                UpdateMirrorGroupOutputPosition(groupMirrorIds, group.output.x, group.output.y, group.output.scale,
                                                group.output.separateScale, group.output.scaleX, group.output.scaleY,
                                                group.output.relativeTo);
            };

            const ImGuiTableFlags groupSettingsTableFlags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings;
            auto beginGroupSettingsTable = [&](const char* id) {
                if (!ImGui::BeginTable(id, 2, groupSettingsTableFlags)) return false;
                ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, kSettingsLabelColumnWidth);
                ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
                return true;
            };
            auto groupSettingsRowLabel = [&](const char* label, const char* tooltip = nullptr) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(label);
                if (tooltip != nullptr && tooltip[0] != '\0') {
                    ImGui::SameLine(0.0f, kInlineHelpMarkerSpacing);
                    HelpMarker(tooltip);
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(240.0f);
            };

            ImGui::SeparatorText(trc("mirrors.group_output_position"));
            if (beginGroupSettingsTable("mirror_group_output_settings")) {
                groupSettingsRowLabel(trc("mirrors.output_scale"), trc("mirrors.tooltip.output_scale"));
                if (ImGui::Checkbox((tr("mirrors.separate_x_y") + "##group_scale").c_str(), &group.output.separateScale)) {
                    g_configIsDirty = true;
                    if (group.output.separateScale) {
                        group.output.scaleX = group.output.scale;
                        group.output.scaleY = group.output.scale;
                    }
                    syncGroupOutputPosition();
                }
                ImGui::SameLine();
                if (!group.output.separateScale) {
                    float scalePercent = group.output.scale * 100.0f;
                    ImGui::SetNextItemWidth(220.0f);
                    if (ImGui::SliderFloat("##group_scale", &scalePercent, 10.0f, 2000.0f, "%.0f%%")) {
                        group.output.scale = scalePercent / 100.0f;
                        g_configIsDirty = true;
                        syncGroupOutputPosition();
                    }
                } else {
                    float scaleXPercent = group.output.scaleX * 100.0f;
                    float scaleYPercent = group.output.scaleY * 100.0f;
                    ImGui::SetNextItemWidth(105.0f);
                    if (ImGui::SliderFloat("X##group_scaleX", &scaleXPercent, 10.0f, 2000.0f, "%.0f%%")) {
                        group.output.scaleX = scaleXPercent / 100.0f;
                        g_configIsDirty = true;
                        syncGroupOutputPosition();
                    }
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(105.0f);
                    if (ImGui::SliderFloat("Y##group_scaleY", &scaleYPercent, 10.0f, 2000.0f, "%.0f%%")) {
                        group.output.scaleY = scaleYPercent / 100.0f;
                        g_configIsDirty = true;
                        syncGroupOutputPosition();
                    }
                }

                groupSettingsRowLabel(trc("mirrors.relative_to_screen"), trc("mirrors.tooltip.relative_to_screen"));
                if (ImGui::Checkbox("##GroupPos", &group.output.useRelativePosition)) {
                    g_configIsDirty = true;
                    if (group.output.useRelativePosition) {
                        int screenWidth = GetCachedWindowWidth();
                        int screenHeight = GetCachedWindowHeight();
                        group.output.relativeX = static_cast<float>(group.output.x) / screenWidth;
                        group.output.relativeY = static_cast<float>(group.output.y) / screenHeight;
                    }
                }

                groupSettingsRowLabel(trc("mirrors.relative_to"));
                const char* group_output_rel_to_preview = getFriendlyName(group.output.relativeTo, relativeToOptions);
                if (ImGui::BeginCombo("##group_output", group_output_rel_to_preview)) {
                    for (const auto& option : relativeToOptions) {
                        if (ImGui::Selectable(option.second, group.output.relativeTo == option.first)) {
                            group.output.relativeTo = option.first;
                            g_configIsDirty = true;
                            syncGroupOutputPosition();
                        }
                    }
                    ImGui::EndCombo();
                }

                if (group.output.useRelativePosition) {
                    int screenWidth = GetCachedWindowWidth();
                    int screenHeight = GetCachedWindowHeight();

                    groupSettingsRowLabel(trc("mirrors.x_percent"));
                    float xPercent = group.output.relativeX * 100.0f;
                    if (ImGui::SliderFloat("##group_out_x_pct", &xPercent, -100.0f, 200.0f, "%.1f%%")) {
                        group.output.relativeX = xPercent / 100.0f;
                        group.output.x = static_cast<int>(group.output.relativeX * screenWidth);
                        g_configIsDirty = true;
                        syncGroupOutputPosition();
                    }

                    groupSettingsRowLabel(trc("mirrors.y_percent"));
                    float yPercent = group.output.relativeY * 100.0f;
                    if (ImGui::SliderFloat("##group_out_y_pct", &yPercent, -100.0f, 200.0f, "%.1f%%")) {
                        group.output.relativeY = yPercent / 100.0f;
                        group.output.y = static_cast<int>(group.output.relativeY * screenHeight);
                        g_configIsDirty = true;
                        syncGroupOutputPosition();
                    }
                } else {
                    groupSettingsRowLabel(trc("mirrors.x_offset"));
                    if (Spinner("##group_out_x", &group.output.x)) {
                        g_configIsDirty = true;
                        syncGroupOutputPosition();
                    }

                    groupSettingsRowLabel(trc("mirrors.y_offset"));
                    if (Spinner("##group_out_y", &group.output.y)) {
                        g_configIsDirty = true;
                        syncGroupOutputPosition();
                    }
                }
                ImGui::EndTable();
            }

            ImGui::SeparatorText(trc("mirrors.per_item_sizing"));
            HelpMarker(trc("mirrors.tooltip.per_item_sizing"));

            int group_mirror_to_remove = -1;
            if (ImGui::BeginTable("group_mirrors_table", 7,
                                  ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn(trc("label.enabled"), ImGuiTableColumnFlags_WidthFixed, 70.0f);
                ImGui::TableSetupColumn(trc("label.name"), ImGuiTableColumnFlags_WidthStretch, 1.4f);
                ImGui::TableSetupColumn(trc("label.width"), ImGuiTableColumnFlags_WidthFixed, 95.0f);
                ImGui::TableSetupColumn(trc("label.height"), ImGuiTableColumnFlags_WidthFixed, 95.0f);
                ImGui::TableSetupColumn(trc("label.x_offset"), ImGuiTableColumnFlags_WidthFixed, 125.0f);
                ImGui::TableSetupColumn(trc("label.y_offset"), ImGuiTableColumnFlags_WidthFixed, 125.0f);
                ImGui::TableSetupColumn("##remove", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight() + 6.0f);
                ImGui::TableHeadersRow();

                for (size_t j = 0; j < group.mirrors.size(); ++j) {
                    ImGui::PushID((int)j + 200000);
                    MirrorGroupItem& item = group.mirrors[j];

                    ImGui::TableNextRow();

                    ImGui::TableSetColumnIndex(0);
                    if (ImGui::Checkbox("##enabled", &item.enabled)) { g_configIsDirty = true; }
                    if (ImGui::IsItemHovered()) { ImGui::SetTooltip(trc("mirrors.tooltip.enable_in_group")); }

                    if (!item.enabled) { ImGui::BeginDisabled(); }

                    ImGui::TableSetColumnIndex(1);
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(item.mirrorId.c_str());

                    ImGui::TableSetColumnIndex(2);
                    float widthPct = item.widthPercent * 100.0f;
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::SliderFloat("##width_pct", &widthPct, 10.0f, 200.0f, "%.0f%%")) {
                        item.widthPercent = widthPct / 100.0f;
                        g_configIsDirty = true;
                    }

                    ImGui::TableSetColumnIndex(3);
                    float heightPct = item.heightPercent * 100.0f;
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::SliderFloat("##height_pct", &heightPct, 10.0f, 200.0f, "%.0f%%")) {
                        item.heightPercent = heightPct / 100.0f;
                        g_configIsDirty = true;
                    }

                    ImGui::TableSetColumnIndex(4);
                    ImGui::SetNextItemWidth(-1);
                    if (Spinner("##offset_x", &item.offsetX, 1, INT_MIN, INT_MAX)) { g_configIsDirty = true; }
                    if (ImGui::IsItemHovered()) { ImGui::SetTooltip(trc("label.x_offset_pixels")); }

                    ImGui::TableSetColumnIndex(5);
                    ImGui::SetNextItemWidth(-1);
                    if (Spinner("##offset_y", &item.offsetY, 1, INT_MIN, INT_MAX)) { g_configIsDirty = true; }
                    if (ImGui::IsItemHovered()) { ImGui::SetTooltip(trc("label.y_offset_pixels")); }

                    if (!item.enabled) { ImGui::EndDisabled(); }

                    ImGui::TableSetColumnIndex(6);
                    if (ImGui::Button("X##del_group_mirror", ImVec2(-1, 0))) { group_mirror_to_remove = (int)j; }

                    ImGui::PopID();
                }

                ImGui::EndTable();
            }

            if (group_mirror_to_remove != -1) {
                group.mirrors.erase(group.mirrors.begin() + group_mirror_to_remove);
                g_configIsDirty = true;
            }

            std::vector<std::string> existingMirrorIds;
            existingMirrorIds.reserve(group.mirrors.size());
            for (const auto& item : group.mirrors) { existingMirrorIds.push_back(item.mirrorId); }

            if (ImGui::BeginCombo((tr("mirrors.add_mirror") + "##add_mirror_to_group").c_str(), trc("mirrors.select_mirror"))) {
                for (const auto& mirrorConf : g_config.mirrors) {
                    if (std::find(existingMirrorIds.begin(), existingMirrorIds.end(), mirrorConf.name) == existingMirrorIds.end()) {
                        if (ImGui::Selectable(mirrorConf.name.c_str())) {
                            MirrorGroupItem newItem;
                            newItem.mirrorId = mirrorConf.name;
                            newItem.widthPercent = 1.0f;
                            newItem.heightPercent = 1.0f;
                            group.mirrors.push_back(newItem);
                            g_configIsDirty = true;
                        }
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::TreePop();
        }

        ImGui::PopID();
    }
    if (group_to_remove != -1) {
        std::string deletedGroupName = g_config.mirrorGroups[group_to_remove].name;
        g_config.mirrorGroups.erase(g_config.mirrorGroups.begin() + group_to_remove);
        for (auto& mode : g_config.modes) {
            auto it = std::find(mode.mirrorGroupIds.begin(), mode.mirrorGroupIds.end(), deletedGroupName);
            while (it != mode.mirrorGroupIds.end()) {
                mode.mirrorGroupIds.erase(it);
                it = std::find(mode.mirrorGroupIds.begin(), mode.mirrorGroupIds.end(), deletedGroupName);
            }
        }
        g_configIsDirty = true;
    }

    if (ImGui::Button(trc("mirrors.add_new_group"))) {
        MirrorGroupConfig newGroup;
        newGroup.name = "New Group " + std::to_string(g_config.mirrorGroups.size() + 1);
        newGroup.output.relativeTo = "centerViewport";
        g_config.mirrorGroups.push_back(newGroup);
        g_configIsDirty = true;
    }

    ImGui::EndTabItem();
}


