if (BeginSelectableSettingsNestedTabItem(trc("tabs.images"))) {
    g_currentlyEditingMirror = "";

    static std::string scrollToThisFrame = "";
    if (!g_scrollToImageName.empty()) {
        scrollToThisFrame = g_scrollToImageName;
        g_scrollToImageName.clear();
    }

    SliderCtrlClickTip();
    const std::string g_currentModeId = GetPublishedCurrentModeId();

    ImGui::TextColored(ImVec4(0.7f, 0.9f, 0.7f, 1.0f),
                       trc("images.tooltip"));
    ImGui::Separator();

    int image_to_remove = -1;
    for (size_t i = 0; i < g_config.images.size(); ++i) {
        auto& img = g_config.images[i];
        ImGui::PushID(static_cast<int>(i));

        std::string delete_img_label = "X##delete_image_" + std::to_string(i);
        if (ImGui::Button(delete_img_label.c_str(), ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()))) {
            std::string img_popup_id = (tr("images.delete_images") + "##" + std::to_string(i));
            ImGui::OpenPopup(img_popup_id.c_str());
        }

        // Popup modal outside of node_open block so it can be displayed even when collapsed
        std::string img_popup_id = (tr("images.delete_images") + "##" + std::to_string(i));
        if (ImGui::BeginPopupModal(img_popup_id.c_str(), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            const std::string deleteImageConfirm = tr("images.delete_images_confirm", img.name.c_str());
            ImGui::TextWrapped("%s", deleteImageConfirm.c_str());
            ImGui::Separator();
            if (ImGui::Button(trc("button.ok"), ImVec2(120, 0))) {
                image_to_remove = (int)i;
                g_configIsDirty = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(trc("label.cancel"), ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }

        ImGui::SameLine();

        std::string oldImageName = img.name;

        if (scrollToThisFrame == img.name) {
            ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        }

        bool node_open = ImGui::TreeNodeEx("##image_node", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", img.name.c_str());

        if (scrollToThisFrame == img.name) {
            ImGui::SetScrollHereY(0.3f);
            scrollToThisFrame = "";
        }

        if (node_open) {
            if (img.relativeTo == "topLeft") {
                img.relativeTo = "topLeftScreen";
                g_configIsDirty = true;
            } else if (img.relativeTo == "topRight") {
                img.relativeTo = "topRightScreen";
                g_configIsDirty = true;
            } else if (img.relativeTo == "bottomLeft") {
                img.relativeTo = "bottomLeftScreen";
                g_configIsDirty = true;
            } else if (img.relativeTo == "bottomRight") {
                img.relativeTo = "bottomRightScreen";
                g_configIsDirty = true;
            } else if (img.relativeTo == "center") {
                img.relativeTo = "centerScreen";
                g_configIsDirty = true;
            }

            bool hasDuplicate = HasDuplicateImageName(img.name, i);
            if (hasDuplicate) {
                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.6f, 0.2f, 0.2f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.7f, 0.3f, 0.3f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.8f, 0.3f, 0.3f, 1.0f));
            }

            if (ImGui::InputText(trc("images.name"), &img.name)) {
                if (!HasDuplicateImageName(img.name, i)) {
                    g_configIsDirty = true;
                    if (oldImageName != img.name) {
                        for (auto& mode : g_config.modes) {
                            RenameModeSource(mode, ModeSourceType::Image, oldImageName, img.name);
                        }
                        // Render looks images up by name; re-key the loaded texture so it stays visible.
                        {
                            std::lock_guard<std::mutex> imageLock(g_userImagesMutex);
                            auto it = g_userImages.find(oldImageName);
                            if (it != g_userImages.end() && g_userImages.find(img.name) == g_userImages.end()) {
                                auto node = g_userImages.extract(it);
                                node.key() = img.name;
                                g_userImages.insert(std::move(node));
                            }
                        }
                    }
                } else {
                    img.name = oldImageName;
                }
            }

            if (hasDuplicate) { ImGui::PopStyleColor(3); }

            if (hasDuplicate) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), trc("images.name_duplicate"));
            }

            std::string imgErrorKey = "img_" + img.name;
            if (RenderMaskedPathInput(trc("images.path"), img.path)) {
                ClearImageError(imgErrorKey);
                g_configIsDirty = true;
            }
            ImGui::SameLine();
            if (ImGui::Button((tr("button.browse") + "##img_" + img.name).c_str())) {
                ImagePickerResult result = OpenImagePickerAndValidate(g_minecraftHwnd.load(), g_toolscreenPath, g_toolscreenPath);

                if (result.completed) {
                    if (result.success) {
                        img.path = result.path;
                        ClearImageError(imgErrorKey);
                        g_configIsDirty = true;
                        LoadImageAsync(DecodedImageData::UserImage, img.name, img.path, g_toolscreenPath);
                    } else if (!result.error.empty()) {
                        SetImageError(imgErrorKey, result.error);
                    }
                }
            }
            ImGui::SameLine();
            if (ImGui::Button((tr("button.validate") + "##img_val_" + img.name).c_str())) {
                std::string error = ValidateImageFile(img.path, g_toolscreenPath);
                if (error.empty()) {
                    ClearImageError(imgErrorKey);
                    LoadImageAsync(DecodedImageData::UserImage, img.name, img.path, g_toolscreenPath);
                } else {
                    SetImageError(imgErrorKey, error);
                }
            }

            std::string imgError = GetImageError(imgErrorKey);
            if (!imgError.empty()) { ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", imgError.c_str()); }

            ImGui::SeparatorText(trc("images.rendering"));
            ;
            if (ImGui::SliderFloat(trc("label.opacity"), &img.opacity, 0.0f, 1.0f)) g_configIsDirty = true;
            if (ImGui::Checkbox(trc("images.pixelated_scaling"), &img.pixelatedScaling)) g_configIsDirty = true;
            if (ImGui::Checkbox(trc("images.only_on_my_screen"), &img.onlyOnMyScreen)) g_configIsDirty = true;
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(trc("images.tooltip.only_on_my_screen"));
            }

            ImGui::Columns(2, "img_render", false);
            ImGui::SetColumnWidth(0, 150);
            ImGui::Text(trc("label.x"));
            ImGui::NextColumn();
            if (Spinner("##img_x", &img.x)) g_configIsDirty = true;
            ImGui::NextColumn();
            ImGui::Text(trc("label.y"));
            ImGui::NextColumn();
            if (Spinner("##img_y", &img.y)) g_configIsDirty = true;
            ImGui::NextColumn();
            if (!img.relativeSizing && (img.width <= 0 || img.height <= 0)) {
                int seededWidth = 0;
                int seededHeight = 0;
                CalculateImageDimensions(img, seededWidth, seededHeight);
                if (seededWidth > 0 && seededHeight > 0) {
                    img.width = seededWidth;
                    img.height = seededHeight;
                    g_configIsDirty = true;
                }
            }
            ImGui::Text(trc("label.width"));
            ImGui::NextColumn();
            ImGui::BeginDisabled(img.relativeSizing);
            if (Spinner("##img_width", &img.width, 1, 1)) g_configIsDirty = true;
            ImGui::EndDisabled();
            ImGui::NextColumn();
            ImGui::Text(trc("label.height"));
            ImGui::NextColumn();
            ImGui::BeginDisabled(img.relativeSizing);
            if (Spinner("##img_height", &img.height, 1, 1)) g_configIsDirty = true;
            ImGui::EndDisabled();
            ImGui::NextColumn();
            ImGui::Text(trc("images.relative_sizing"));
            const bool relativeSizingLabelHovered = ImGui::IsItemHovered();
            ImGui::NextColumn();
            if (ImGui::Checkbox("##img_relative_sizing", &img.relativeSizing)) {
                if (!img.relativeSizing && (img.width <= 0 || img.height <= 0)) {
                    int seededWidth = 0;
                    int seededHeight = 0;
                    CalculateImageDimensions(img, seededWidth, seededHeight);
                    if (seededWidth > 0 && seededHeight > 0) {
                        img.width = seededWidth;
                        img.height = seededHeight;
                    }
                }
                g_configIsDirty = true;
            }
            if (relativeSizingLabelHovered || ImGui::IsItemHovered()) {
                ImGui::SetTooltip(trc("images.tooltip.relative_sizing"));
            }
            ImGui::NextColumn();
            ImGui::Text(trc("label.scale"));
            ImGui::NextColumn();
            if (!img.relativeSizing && img.width > 0 && img.height > 0) {
                int croppedW = 0, croppedH = 0;
                if (GetCroppedImageDimensions(img, croppedW, croppedH)) {
                    float scaleX = static_cast<float>(img.width) / croppedW;
                    float scaleY = static_cast<float>(img.height) / croppedH;
                    img.scale = (scaleX + scaleY) * 0.5f;
                }
            }
            float scalePercent = img.scale * 100.0f;
            ImGui::SetNextItemWidth(250);
            if (ImGui::SliderFloat("##img_scale", &scalePercent, 10.0f, 300.0f, "%.0f%%")) {
                if (img.relativeSizing) {
                    img.scale = scalePercent / 100.0f;
                    g_configIsDirty = true;
                } else {
                    int croppedW = 0, croppedH = 0;
                    if (GetCroppedImageDimensions(img, croppedW, croppedH)) {
                        img.scale = scalePercent / 100.0f;
                        img.width = (std::max)(1, static_cast<int>(croppedW * img.scale));
                        img.height = (std::max)(1, static_cast<int>(croppedH * img.scale));
                        g_configIsDirty = true;
                    }
                }
            }
            ImGui::NextColumn();
            ImGui::Text(trc("label.relative_to"));
            ImGui::NextColumn();
            const char* current_rel_to = getFriendlyName(img.relativeTo, imageRelativeToOptions);
            ImGui::SetNextItemWidth(180);
            if (ImGui::BeginCombo("##img_rel_to", current_rel_to)) {
                for (const auto& option : imageRelativeToOptions) {
                    if (ImGui::Selectable(option.second, img.relativeTo == option.first)) {
                        img.relativeTo = option.first;
                        g_configIsDirty = true;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::Columns(1);

            ImGui::SeparatorText(trc("images.cropping"));
            ImGui::Columns(2, "img_crop", false);
            ImGui::SetColumnWidth(0, 120);
            ImGui::Text(trc("images.crop_top"));
            ImGui::NextColumn();
            if (Spinner("##img_crop_t", &img.crop_top, 1, 0)) g_configIsDirty = true;
            ImGui::NextColumn();
            ImGui::Text(img.cropToHeight ? trc("label.crop_to_height") : trc("images.crop_bottom"));
            ImGui::NextColumn();
            if (Spinner("##img_crop_b", &img.crop_bottom, 1, 0)) g_configIsDirty = true;
            ImGui::SameLine();
            if (ImGui::Checkbox(trc("label.crop_to_target_h"), &img.cropToHeight)) g_configIsDirty = true;
            ImGui::SameLine(); HelpMarker(trc("label.tooltip.crop_to"));
            ImGui::NextColumn();
            ImGui::Text(trc("images.crop_left"));
            ImGui::NextColumn();
            if (Spinner("##img_crop_l", &img.crop_left, 1, 0)) g_configIsDirty = true;
            ImGui::NextColumn();
            ImGui::Text(img.cropToWidth ? trc("label.crop_to_width") : trc("images.crop_right"));
            ImGui::NextColumn();
            if (Spinner("##img_crop_r", &img.crop_right, 1, 0)) g_configIsDirty = true;
            ImGui::SameLine();
            if (ImGui::Checkbox(trc("label.crop_to_target_w"), &img.cropToWidth)) g_configIsDirty = true;
            ImGui::SameLine(); HelpMarker(trc("label.tooltip.crop_to"));
            ImGui::NextColumn();
            ImGui::Columns(1);

            ImGui::SeparatorText(trc("images.background"));
            if (ImGui::Checkbox(trc("images.enable_background"), &img.background.enabled)) g_configIsDirty = true;
            ImGui::BeginDisabled(!img.background.enabled);
            if (ImGui::ColorEdit3(trc("images.bg_color"), &img.background.color.r)) g_configIsDirty = true;
            if (ImGui::SliderFloat(trc("images.bg_opacity"), &img.background.opacity, 0.0f, 1.0f)) g_configIsDirty = true;
            ImGui::EndDisabled();

            ImGui::SeparatorText(trc("images.color_keying"));
            if (ImGui::Checkbox(trc("images.enable_color_key"), &img.enableColorKey)) g_configIsDirty = true;
            ImGui::BeginDisabled(!img.enableColorKey);

            int imgColorKeyToRemove = -1;
            for (size_t k = 0; k < img.colorKeys.size(); k++) {
                ImGui::PushID(static_cast<int>(k));
                auto& ck = img.colorKeys[k];

                ImGui::Text(trc("images.key_format"), k + 1);
                ImGui::SameLine();
                ImGui::PushItemWidth(150.0f);
                if (ImGui::ColorEdit3("##color", &ck.color.r, ImGuiColorEditFlags_NoLabel)) g_configIsDirty = true;
                ImGui::PopItemWidth();
                ImGui::SameLine();
                ImGui::PushItemWidth(80.0f);
                if (ImGui::SliderFloat("##sens", &ck.sensitivity, 0.001f, 1.0f, "%.3f")) g_configIsDirty = true;
                ImGui::PopItemWidth();
                ImGui::SameLine();
                if (ImGui::Button("X##remove")) { imgColorKeyToRemove = static_cast<int>(k); }
                ImGui::PopID();
            }

            if (imgColorKeyToRemove >= 0) {
                img.colorKeys.erase(img.colorKeys.begin() + imgColorKeyToRemove);
                g_configIsDirty = true;
            }

            ImGui::BeginDisabled(img.colorKeys.size() >= ConfigDefaults::MAX_COLOR_KEYS);
            if (ImGui::Button(trc("images.add_color_key"))) {
                ColorKeyConfig newKey;
                newKey.color = { 0.0f, 0.0f, 0.0f };
                newKey.sensitivity = 0.05f;
                img.colorKeys.push_back(newKey);
                g_configIsDirty = true;
            }
            ImGui::EndDisabled();

            ImGui::EndDisabled();

            ImGui::SeparatorText(trc("images.border"));
            if (ImGui::Checkbox((tr("images.enable_border") + "##Image").c_str(), &img.border.enabled)) { g_configIsDirty = true; }
            ImGui::SameLine();
            HelpMarker(trc("images.tooltip.border"));

            if (img.border.enabled) {
                ImGui::Text(trc("images.border_color"));
                ImVec4 borderCol = ImVec4(img.border.color.r, img.border.color.g, img.border.color.b, 1.0f);
                if (ImGui::ColorEdit3("##BorderColorImage", (float*)&borderCol, ImGuiColorEditFlags_NoInputs)) {
                    img.border.color = { borderCol.x, borderCol.y, borderCol.z };
                    g_configIsDirty = true;
                }

                ImGui::Text(trc("images.border_width"));
                ImGui::SetNextItemWidth(100);
                if (Spinner("##BorderWidthImage", &img.border.width, 1, 1, 50)) { g_configIsDirty = true; }
                ImGui::SameLine();
                ImGui::TextDisabled(trc("label.px"));

                ImGui::Text(trc("images.border_radius"));
                ImGui::SetNextItemWidth(100);
                if (Spinner("##BorderRadiusImage", &img.border.radius, 1, 0, 100)) { g_configIsDirty = true; }
                ImGui::SameLine();
                ImGui::TextDisabled(trc("label.px"));
            }

            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (image_to_remove != -1) {
        std::string deletedImageName = g_config.images[image_to_remove].name;
        g_config.images.erase(g_config.images.begin() + image_to_remove);
        for (auto& mode : g_config.modes) {
            RemoveAllModeSources(mode, ModeSourceType::Image, deletedImageName);
        }
        DiscardUnusedUserImageCaches();
        g_configIsDirty = true;
    }

    ImGui::Separator();
    if (ImGui::Button(trc("button.add_image"))) {
        ImageConfig newImg;
        newImg.name = tr("images.new_image") + " " + std::to_string(g_config.images.size() + 1);
        newImg.relativeTo = "centerViewport";
        g_config.images.push_back(newImg);
        g_configIsDirty = true;

        if (!g_currentModeId.empty()) {
            for (auto& mode : g_config.modes) {
                if (mode.id == g_currentModeId) {
                    AddModeSource(mode, ModeSourceType::Image, newImg.name);
                    break;
                }
            }
        }
    }

    ImGui::SameLine();
    if (ImGui::Button((tr("button.reset_defaults") + "##images").c_str())) { ImGui::OpenPopup(trc("images.reset_to_defaults_popup")); }

    if (ImGui::BeginPopupModal(trc("images.reset_to_defaults_popup"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f), trc("label.warning"));
        ImGui::Text(trc("images.reset_warning"));
        ImGui::Text(trc("label.action_cannot_be_undone"));
        ImGui::Separator();
        if (ImGui::Button(trc("button.confirm_reset"), ImVec2(120, 0))) {
            g_config.images = GetDefaultImages();
            g_configIsDirty = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (ImGui::Button(trc("label.cancel"), ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    ImGui::EndTabItem();
}


