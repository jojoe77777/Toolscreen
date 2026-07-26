if (BeginSelectableSettingsTopTabItem(trc("tabs.other"))) {
    g_currentlyEditingMirror = "";

    auto drawMirrorColorspaceSetting = []() {
        const char* gammaModes[] = { trc("settings.mirrors_auto"), trc("settings.mirrors_assume_srgb"), trc("settings.mirrors_assume_linear") };
        int gm = static_cast<int>(g_config.mirrorGammaMode);
        ImGui::SetNextItemWidth(250);
        if (ImGui::Combo(trc("settings.mirrors_match_colorspace"), &gm, gammaModes, IM_ARRAYSIZE(gammaModes))) {
            g_config.mirrorGammaMode = static_cast<MirrorGammaMode>(gm);
            g_configIsDirty = true;

            SetGlobalMirrorGammaMode(g_config.mirrorGammaMode);

            std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex);
            for (auto& kv : g_mirrorInstances) {
                kv.second.forceUpdateFrames = 3;
                kv.second.hasValidContent = false;
            }
        }
        ImGui::SameLine();
        HelpMarker(trc("settings.tooltip.colorspace"));
    };

    SliderCtrlClickTip();

    const bool showAllSettingsSections = MatchesConfigTopTabCategorySearch(ConfigTopTabId::Settings, s_configGuiSearchState.query);
    const bool showCaptureStreamingSection = ShouldRenderConfigSearchSection(showAllSettingsSections, {
        trc("settings.capture_streaming"),
        trc("settings.hide_animations_in_game"),
        trc("settings.enable_virtual_camera"),
        trc("settings.capture_fake_cursor_overlay"),
        "capture",
        "streaming",
        "virtual camera",
        "cursor"
    });
    const bool showAdvancedSettingsSection = ShouldRenderConfigSearchSection(showAllSettingsSections, {
        trc("config_mode.advanced"),
        trc("settings.auto_borderless"),
        trc("settings.restore_windowed_mode_on_fullscreen_exit"),
        "advanced",
        "borderless",
        "fullscreen"
    });
    const bool showPerformanceSection = ShouldRenderConfigSearchSection(showAllSettingsSections, {
        trc("settings.performance"),
        trc("label.fps_limit"),
        trc("settings.video_cache_budget_mib"),
        "performance",
        "fps",
        "fps limit",
        "video cache"
    });
    const bool showStartupIndicatorSection = ShouldRenderConfigSearchSection(
        showAllSettingsSections,
        {"startup", "indicator", "toast", "reminder", "hotkey", "welcome", "launch"});
    const bool showFontSection = ShouldRenderConfigSearchSection(showAllSettingsSections, {
        trc("label.font"),
        trc("label.font_path"),
        trc("label.scale"),
        "font",
        "font path",
        "gui scale"
    });
    const bool showAllDebugSections = showAllSettingsSections ||
                                      (IsConfigGuiSearchActive() && DoesActiveConfigSearchMatch({
                                          trc("settings.debug_options"),
                                          trc("settings.debug_mpeg_video_memory"),
                                          trc("settings.advanced_logging"),
                                          "debug",
                                          "logging"
                                      }));
    const bool showDebugOptionsSection = ShouldRenderConfigSearchSection(showAllDebugSections, {
        trc("settings.debug_options"),
        trc("settings.mirrors_match_colorspace"),
        trc("settings.limit_capture_framerate"),
        trc("settings.delay_rendering_until_finished"),
        trc("settings.show_performance_overlay"),
        trc("settings.show_profiler"),
        trc("settings.profiler_scale"),
        trc("settings.show_hotkey_debug"),
        trc("settings.fake_cursor_overlay"),
        trc("settings.show_texture_grid"),
        "profiler",
        "debug"
    });
    const bool showDebugVideoMemorySection = ShouldRenderConfigSearchSection(showAllDebugSections, {
        trc("settings.debug_mpeg_video_memory"),
        trc("settings.debug_mpeg_video_memory_uploaded_clips"),
        trc("settings.debug_mpeg_video_memory_uploaded_textures"),
        trc("settings.debug_mpeg_video_memory_vram"),
        "mpeg",
        "video memory",
        "vram"
    });
    const bool showAdvancedLoggingSection = ShouldRenderConfigSearchSection(showAllDebugSections, {
        trc("settings.advanced_logging"),
        trc("settings.enable_verbose_logging"),
        trc("settings.log_mode_switch"),
        trc("settings.log_animation"),
        trc("settings.log_hotkey"),
        trc("settings.log_obs"),
        trc("settings.log_window_overlay"),
        trc("settings.log_file_monitor"),
        trc("settings.log_image_monitor"),
        trc("settings.log_performance"),
        trc("settings.log_texture_ops"),
        trc("settings.log_gui"),
        trc("settings.log_init"),
        trc("settings.log_cursor_textures"),
        "logging",
        "verbose logging"
    });

    if (showCaptureStreamingSection) {
        ImGui::Spacing();
        ImGui::SeparatorText(trc("settings.capture_streaming"));
        RecordConfigSearchSectionInteractionRect("config.section.settings.capture_streaming");
        if (ImGui::Checkbox(trc("settings.hide_animations_in_game"), &g_config.hideAnimationsInGame)) { g_configIsDirty = true; }
        ImGui::SameLine();
        HelpMarker(trc("tooltip.hide_animations_in_game"));

        bool driverInstalled = IsVirtualCameraDriverInstalled();
        bool inUseByOBS = driverInstalled && IsVirtualCameraInUseByOBS();
        ImGui::BeginDisabled(!driverInstalled || inUseByOBS);
        bool vcEnabled = g_config.debug.virtualCameraEnabled;
        if (ImGui::Checkbox(trc("settings.enable_virtual_camera"), &vcEnabled)) {
            g_config.debug.virtualCameraEnabled = vcEnabled;
            g_configIsDirty = true;
            if (vcEnabled) {
                uint32_t vcWidth = 0;
                uint32_t vcHeight = 0;
                if (GetPreferredVirtualCameraResolution(vcWidth, vcHeight)) {
                    StartVirtualCamera(vcWidth, vcHeight);
                }
            } else {
                StopVirtualCamera();
            }
        }

        ImGui::EndDisabled();
        ImGui::SameLine();
        if (!driverInstalled) {
            ImGui::TextDisabled(trc("settings.virtual.camera_not_installed"));
        } else if (inUseByOBS) {
            ImGui::TextDisabled(trc("settings.virtual.camera_in_use"));
        } else {
            HelpMarker(trc("settings.tooltip.virtual_camera"));
        }

        if (ImGui::Checkbox(trc("settings.capture_fake_cursor_overlay"), &g_config.captureFakeCursor)) {
            g_configIsDirty = true;
        }
        ImGui::SameLine();
        HelpMarker(trc("settings.tooltip.capture_fake_cursor"));
    }

    if (showAdvancedSettingsSection) {
        ImGui::Spacing();
        ImGui::SeparatorText(trc("config_mode.advanced"));
        RecordConfigSearchSectionInteractionRect("config.section.settings.advanced");
        ImGui::PushID("settings_auto_borderless");
        if (ImGui::Checkbox(trc("settings.auto_borderless"), &g_config.autoBorderless)) { g_configIsDirty = true; }
        ImGui::SameLine();
        HelpMarker(trc("tooltip.auto_borderless"));
        ImGui::PopID();

        if (ImGui::Checkbox(trc("settings.restore_windowed_mode_on_fullscreen_exit"), &g_config.restoreWindowedModeOnFullscreenExit)) {
            g_configIsDirty = true;
        }
        ImGui::SameLine();
        HelpMarker(trc("settings.tooltip.restore_windowed_mode_on_fullscreen_exit"));
    }

    if (showPerformanceSection) {
        ImGui::Spacing();
        ImGui::SeparatorText(trc("settings.performance"));
        RecordConfigSearchSectionInteractionRect("config.section.settings.performance");
        ImGui::Text(trc("label.fps_limit"));
        ImGui::SetNextItemWidth(300);
        int fpsLimitValue = (g_config.fpsLimit == 0) ? 1001 : g_config.fpsLimit;
        if (ImGui::SliderInt("##fpsLimit", &fpsLimitValue, 30, 1001, fpsLimitValue == 1001 ? trc("label.unlimited") : "%d fps")) {
            g_config.fpsLimit = (fpsLimitValue == 1001) ? 0 : fpsLimitValue;
            g_configIsDirty = true;
        }
        ImGui::SameLine();
        HelpMarker(trc("tooltip.fps_limit.advanced"));

        ImGui::Spacing();
        ImGui::Text(trc("settings.video_cache_budget_mib"));
        ImGui::SetNextItemWidth(300);
        int videoCacheBudgetMiB = g_config.debug.videoCacheBudgetMiB;
        if (ImGui::SliderInt("##videoCacheBudgetMiB", &videoCacheBudgetMiB, 0, 2048,
                             videoCacheBudgetMiB == 0 ? trc("label.disabled") : "%d MiB")) {
            g_config.debug.videoCacheBudgetMiB = videoCacheBudgetMiB;
            g_configIsDirty = true;
        }
        ImGui::SameLine();
        HelpMarker(trc("settings.tooltip.video_cache_budget_mib"));
    }

    if (showStartupIndicatorSection) {
        ImGui::Spacing();
        ImGui::PushID("startup_indicator");
        ImGui::SeparatorText(trc("startup_indicator.section"));
        RecordConfigSearchSectionInteractionRect("config.section.settings.startup_indicator");

        if (g_config.startupIndicatorMode == 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f), trc("label.warning"));
            ImGui::TextWrapped("%s", trc("startup_indicator.warning"));
        }

        std::string startupHotkey = GetKeyComboString(g_config.guiHotkey);
        if (startupHotkey.empty()) startupHotkey = trc("startup_indicator.hotkey_unset");
        ImGui::TextUnformatted(trc("startup_indicator.current_hotkey", startupHotkey));

        ImGui::Spacing();

        int startupMode = g_config.startupIndicatorMode;
        bool startupModeChanged = false;
        startupModeChanged |= ImGui::RadioButton(trc("startup_indicator.none"), &startupMode, 0);
        ImGui::SameLine(); HelpMarker(trc("startup_indicator.none_help"));
        startupModeChanged |= ImGui::RadioButton(trc("startup_indicator.shortcut_reminder"), &startupMode, 1);
        ImGui::SameLine(); HelpMarker(trc("startup_indicator.shortcut_reminder_help"));
        startupModeChanged |= ImGui::RadioButton(trc("startup_indicator.custom"), &startupMode, 2);
        ImGui::SameLine(); HelpMarker(trc("startup_indicator.custom_help"));

        if (startupModeChanged) {
            g_config.startupIndicatorMode = startupMode;
            g_configIsDirty = true;
        }

        if (g_config.startupIndicatorMode == 2) {
            ImGui::Indent();
            ImGui::TextUnformatted(trc("startup_indicator.image_path"));

            if (ImGui::Button(trc("button.browse"))) {
                ImagePickerResult result = OpenImagePickerAndValidate(g_minecraftHwnd.load(), g_toolscreenPath, g_toolscreenPath);
                if (result.completed && result.success) {
                    if (DetectVisualMediaKindFromPath(result.path) == VisualMediaKind::VideoMpeg1) {
                        SetImageError("startup_indicator", trc("startup_indicator.error.video_unsupported"));
                    } else {
                        g_config.startupIndicatorImagePath = result.path;
                        ClearImageError("startup_indicator");
                        g_configIsDirty = true;
                        InvalidateStartupIndicatorTexture();
                    }
                } else if (result.completed && !result.success && !result.error.empty()) {
                    SetImageError("startup_indicator", result.error);
                }
            }

            ImGui::SameLine();
            if (RenderMaskedPathInput("##startup_ind_path", g_config.startupIndicatorImagePath)) {
                g_configIsDirty = true;
                InvalidateStartupIndicatorTexture();
                const std::string& p = g_config.startupIndicatorImagePath;
                if (p.empty()) {
                    ClearImageError("startup_indicator");
                } else {
                    std::string err = ValidateImageFile(p, g_toolscreenPath);
                    if (err.empty()) ClearImageError("startup_indicator");
                    else SetImageError("startup_indicator", err);
                }
            }

            if (g_config.startupIndicatorImagePath.empty()) {
                ImGui::TextDisabled("%s", trc("startup_indicator.custom_no_image"));
            }
            std::string startupImgErr = GetImageError("startup_indicator");
            if (!startupImgErr.empty()) {
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", startupImgErr.c_str());
            }
            ImGui::Unindent();
        }
        ImGui::PopID();
    }

    if (showFontSection) {
        ImGui::Spacing();
        ImGui::SeparatorText(trc("label.font"));
        RecordConfigSearchSectionInteractionRect("config.section.settings.font");

        const std::vector<FontPickerOption> mainGuiFontOptions = BuildFontPickerOptions();
        auto applyMainGuiFontChange = []() {
            g_configIsDirty = true;
            RequestDynamicGuiFontRefresh(true);
        };

        ImGui::Text(trc("label.font"));
        const bool usingCustomMainGuiFont = RenderFontPickerCombo("##AdvancedMainGuiFontChoice", 300.0f, mainGuiFontOptions,
                                                                  g_config.fontPath, s_mainGuiFontPickerState, applyMainGuiFontChange);
        ImGui::SameLine();
        HelpMarker(trc("tooltip.font"));

        if (usingCustomMainGuiFont) {
            ImGui::Text(trc("label.font_path"));
            RenderCustomFontPathEditor("##AdvancedFontPath", "##AdvancedMainGuiFont", 300.0f, mainGuiFontOptions,
                                       g_config.fontPath, s_mainGuiFontPickerState, "Select Main GUI Font",
                                       applyMainGuiFontChange);
        }

        ImGui::Text(trc("label.scale"));
        ImGui::SetNextItemWidth(160);
        if (ImGui::SliderFloat("##AdvancedGuiFontScale", &g_config.appearance.guiFontScale, 0.75f, 2.0f, "%.2fx")) {
            g_config.appearance.guiFontScale = std::clamp(g_config.appearance.guiFontScale, 0.75f, 2.0f);
            g_configIsDirty = true;
            RequestDynamicGuiFontRefresh(true);
        }
        ImGui::SameLine();
        HelpMarker(trc("tooltip.gui_font_scale"));

        ImGui::Spacing();
    }

    static bool s_debugUnlocked = false;
    static char s_passcodeInput[16] = "";

    if (!s_debugUnlocked) {
        if (ImGui::Button(trc("button.debug"))) {
            ImGui::OpenPopup(trc("settings.debug_passcode"));
            memset(s_passcodeInput, 0, sizeof(s_passcodeInput));
        }

        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing,
                                ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal(trc("settings.debug_passcode"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text(trc("settings.debug_passcode_prompt"));
            ImGui::Spacing();

            ImGui::SetNextItemWidth(150);
            bool enterPressed = ImGui::InputText("##passcode", s_passcodeInput, sizeof(s_passcodeInput),
                                                 ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);

            ImGui::Spacing();

            if (ImGui::Button(trc("button.ok"), ImVec2(80, 0)) || enterPressed) {
                if (strcmp(s_passcodeInput, "5739") == 0) {
                    s_debugUnlocked = true;
                    ImGui::CloseCurrentPopup();
                } else {
                    memset(s_passcodeInput, 0, sizeof(s_passcodeInput));
                }
            }
            ImGui::SameLine();
            if (ImGui::Button(trc("button.cancel"), ImVec2(80, 0))) { ImGui::CloseCurrentPopup(); }

            ImGui::EndPopup();
        }
    } else {
        struct MpegVideoTextureDebugStats {
            size_t uploadedVramBytes = 0;
            size_t uploadedClipCount = 0;
            size_t uploadedTextureCount = 0;
        };

        auto collectMpegVideoTextureDebugStats = []() {
            MpegVideoTextureDebugStats stats;

            auto tryMultiply = [](size_t left, size_t right, size_t& out) {
                if (left == 0 || right == 0) {
                    out = 0;
                    return true;
                }
                if (left > (std::numeric_limits<size_t>::max)() / right) {
                    return false;
                }
                out = left * right;
                return true;
            };

            auto tryComputeRgbaBytes = [&tryMultiply](int width, int height, size_t& outBytes) {
                if (width <= 0 || height <= 0) {
                    return false;
                }

                size_t pixelCount = 0;
                if (!tryMultiply(static_cast<size_t>(width), static_cast<size_t>(height), pixelCount)) {
                    return false;
                }
                return tryMultiply(pixelCount, 4, outBytes);
            };

            auto computeTextureBytes = [&tryComputeRgbaBytes](int width, int height) {
                if (width <= 0 || height <= 0) {
                    return static_cast<size_t>(0);
                }

                size_t textureBytes = 0;
                if (!tryComputeRgbaBytes(width, height, textureBytes)) {
                    return static_cast<size_t>(0);
                }
                return textureBytes;
            };

            auto computeInstanceTextureBytes = [&computeTextureBytes](const auto& inst) {
                size_t totalBytes = 0;
                if (inst.isAnimated && !inst.frameTextures.empty()) {
                    if (!inst.frameTextureHeights.empty()) {
                        for (int textureHeight : inst.frameTextureHeights) {
                            totalBytes += computeTextureBytes(inst.width, textureHeight);
                        }
                        return totalBytes;
                    }

                    const int textureHeight = inst.textureStorageHeight > 0 ? inst.textureStorageHeight : inst.height;
                    return computeTextureBytes(inst.width, textureHeight) * inst.frameTextures.size();
                }

                if (inst.textureId == 0) {
                    return static_cast<size_t>(0);
                }

                const int textureHeight = inst.textureStorageHeight > 0 ? inst.textureStorageHeight : inst.height;
                return computeTextureBytes(inst.width, textureHeight);
            };

            {
                std::lock_guard<std::mutex> lock(g_backgroundTexturesMutex);
                for (const auto& [id, inst] : g_backgroundTextures) {
                    (void)id;
                    if (!inst.isVideo) {
                        continue;
                    }

                    const size_t textureCount = inst.isAnimated ? inst.frameTextures.size() : (inst.textureId != 0 ? 1u : 0u);
                    if (textureCount == 0) {
                        continue;
                    }

                    stats.uploadedClipCount += 1;
                    stats.uploadedTextureCount += textureCount;
                    stats.uploadedVramBytes += computeInstanceTextureBytes(inst);
                }
            }

            {
                std::lock_guard<std::mutex> lock(g_userImagesMutex);
                for (const auto& [id, inst] : g_userImages) {
                    (void)id;
                    if (!inst.isVideo) {
                        continue;
                    }

                    const size_t textureCount = inst.isAnimated ? inst.frameTextures.size() : (inst.textureId != 0 ? 1u : 0u);
                    if (textureCount == 0) {
                        continue;
                    }

                    stats.uploadedClipCount += 1;
                    stats.uploadedTextureCount += textureCount;
                    stats.uploadedVramBytes += computeInstanceTextureBytes(inst);
                }
            }

            return stats;
        };

        if (showDebugOptionsSection) {
            ImGui::SeparatorText(trc("settings.debug_options"));
            RecordConfigSearchSectionInteractionRect("config.section.settings.debug_options");
            drawMirrorColorspaceSetting();
            ImGui::Spacing();
            if (ImGui::Checkbox(trc("settings.limit_capture_framerate"), &g_config.limitCaptureFramerate)) { g_configIsDirty = true; }
            ImGui::SameLine();
            HelpMarker(trc("settings.tooltip.limit_capture_framerate"));
            ImGui::Spacing();
            if (ImGui::Checkbox(trc("settings.delay_rendering_until_finished"), &g_config.debug.delayRenderingUntilFinished)) { g_configIsDirty = true; }
            ImGui::SameLine();
            HelpMarker(trc("settings.tooltip.delay_rendering_until_finished"));
            ImGui::Spacing();
            if (ImGui::Checkbox(trc("settings.show_performance_overlay"), &g_config.debug.showPerformanceOverlay)) { g_configIsDirty = true; }
            if (ImGui::Checkbox(trc("settings.show_profiler"), &g_config.debug.showProfiler)) { g_configIsDirty = true; }
            ImGui::SetNextItemWidth(300);
            if (ImGui::SliderFloat(trc("settings.profiler_scale"), &g_config.debug.profilerScale, 0.25f, 2.0f, "%.2f")) { g_configIsDirty = true; }
            ImGui::SameLine();
            HelpMarker(trc("settings.tooltip.profiler_scale"));
            if (ImGui::Checkbox(trc("settings.show_hotkey_debug"), &g_config.debug.showHotkeyDebug)) { g_configIsDirty = true; }
            if (ImGui::Checkbox(trc("settings.fake_cursor_overlay"), &g_config.debug.fakeCursor)) { g_configIsDirty = true; }
            ImGui::SameLine();
            HelpMarker(trc("settings.tooltip.fake_cursor"));
            if (ImGui::Checkbox(trc("settings.show_texture_grid"), &g_config.debug.showTextureGrid)) { g_configIsDirty = true; }
            ImGui::SameLine();
            HelpMarker(trc("settings.tooltip.show_texture_grid"));
        }

        if (showDebugVideoMemorySection) {
            ImGui::Spacing();
            ImGui::SeparatorText(trc("settings.debug_mpeg_video_memory"));
            RecordConfigSearchSectionInteractionRect("config.section.settings.debug_mpeg_video_memory");
            const MpegVideoTextureDebugStats mpegVideoStats = collectMpegVideoTextureDebugStats();
            if (mpegVideoStats.uploadedClipCount == 0) {
                ImGui::TextDisabled(trc("settings.debug_mpeg_video_memory_empty"));
            } else {
                ImGui::Text("%s %zu", trc("settings.debug_mpeg_video_memory_uploaded_clips"), mpegVideoStats.uploadedClipCount);
                ImGui::Text("%s %zu", trc("settings.debug_mpeg_video_memory_uploaded_textures"), mpegVideoStats.uploadedTextureCount);
                ImGui::Text("%s %.2f MiB (%llu bytes)", trc("settings.debug_mpeg_video_memory_vram"),
                            static_cast<double>(mpegVideoStats.uploadedVramBytes) / (1024.0 * 1024.0),
                            static_cast<unsigned long long>(mpegVideoStats.uploadedVramBytes));
                ImGui::TextDisabled(trc("settings.debug_mpeg_video_memory_note"));
            }
        }

        if (showAdvancedLoggingSection) {
            ImGui::Spacing();
            const bool advancedLoggingOpen = ImGui::CollapsingHeader(
                trc("settings.advanced_logging"),
                GetConfigSearchSectionOpenFlags(showAllDebugSections, {
                    trc("settings.advanced_logging"),
                    trc("settings.enable_verbose_logging"),
                    trc("settings.log_mode_switch"),
                    trc("settings.log_animation"),
                    trc("settings.log_hotkey"),
                    trc("settings.log_obs"),
                    trc("settings.log_window_overlay"),
                    trc("settings.log_browser_overlay"),
                    trc("settings.log_ninjabrain"),
                    trc("settings.log_file_monitor"),
                    trc("settings.log_image_monitor"),
                    trc("settings.log_performance"),
                    trc("settings.log_texture_ops"),
                    trc("settings.log_gui"),
                    trc("settings.log_init"),
                    trc("settings.log_cursor_textures"),
                    "logging",
                    "verbose logging"
                }));
            RecordConfigSearchSectionInteractionRect("config.section.settings.advanced_logging");
            if (advancedLoggingOpen) {
                ImGui::Indent();
                ImGui::TextDisabled(trc("settings.enable_verbose_logging"));
                ImGui::Spacing();
                if (ImGui::Checkbox(trc("settings.log_mode_switch"), &g_config.debug.logModeSwitch)) { g_configIsDirty = true; }
                if (ImGui::Checkbox(trc("settings.log_animation"), &g_config.debug.logAnimation)) { g_configIsDirty = true; }
                if (ImGui::Checkbox(trc("settings.log_hotkey"), &g_config.debug.logHotkey)) { g_configIsDirty = true; }
                if (ImGui::Checkbox(trc("settings.log_obs"), &g_config.debug.logObs)) { g_configIsDirty = true; }
                if (ImGui::Checkbox(trc("settings.log_window_overlay"), &g_config.debug.logWindowOverlay)) { g_configIsDirty = true; }
                if (ImGui::Checkbox(trc("settings.log_browser_overlay"), &g_config.debug.logBrowserOverlay)) { g_configIsDirty = true; }
                if (ImGui::Checkbox(trc("settings.log_ninjabrain"), &g_config.debug.logNinjabrain)) { g_configIsDirty = true; }
                if (ImGui::Checkbox(trc("settings.log_file_monitor"), &g_config.debug.logFileMonitor)) { g_configIsDirty = true; }
                if (ImGui::Checkbox(trc("settings.log_image_monitor"), &g_config.debug.logImageMonitor)) { g_configIsDirty = true; }
                if (ImGui::Checkbox(trc("settings.log_performance"), &g_config.debug.logPerformance)) { g_configIsDirty = true; }
                if (ImGui::Checkbox(trc("settings.log_texture_ops"), &g_config.debug.logTextureOps)) { g_configIsDirty = true; }
                if (ImGui::Checkbox(trc("settings.log_gui"), &g_config.debug.logGui)) { g_configIsDirty = true; }
                if (ImGui::Checkbox(trc("settings.log_init"), &g_config.debug.logInit)) { g_configIsDirty = true; }
                if (ImGui::Checkbox(trc("settings.log_cursor_textures"), &g_config.debug.logCursorTextures)) { g_configIsDirty = true; }
                ImGui::Unindent();
            }
        }
    }
    ImGui::EndTabItem();
}


