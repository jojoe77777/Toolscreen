            if (BeginSelectableSettingsTopTabItem(trc("tabs.appearance"))) {
                g_currentlyEditingMirror = "";

                const bool showAllAppearanceSections = MatchesConfigTopTabCategorySearch(ConfigTopTabId::Appearance, s_configGuiSearchState.query);
                const bool showColorSchemeSection = ShouldRenderConfigSearchSection(showAllAppearanceSections, {
                    trc("appearance.color_scheme"),
                    trc("appearance.preset_themes"),
                    trc("appearance.dark"),
                    trc("appearance.light"),
                    trc("appearance.classic"),
                    trc("appearance.dracula"),
                    trc("appearance.nord"),
                    trc("appearance.solarized"),
                    trc("appearance.red"),
                    trc("appearance.green"),
                    trc("appearance.reset_to_default_dark"),
                    "theme",
                    "themes",
                    "color scheme"
                });
                const bool showAllAppearanceColorSections = showAllAppearanceSections ||
                                                            (IsConfigGuiSearchActive() &&
                                                             DoesActiveConfigSearchMatch({
                                                                 trc("appearance.custom_colors"),
                                                                 "custom colors"
                                                             }));
                const bool showAppearanceWindowSection = ShouldRenderConfigSearchSection(showAllAppearanceColorSections, {
                    trc("appearance.window"),
                    trc("appearance.window_background"),
                    trc("appearance.child_background"),
                    trc("appearance.popup_background"),
                    trc("appearance.border"),
                    "window background",
                    "windowbg",
                    "childbg",
                    "popupbg"
                });
                const bool showAppearanceTextSection = ShouldRenderConfigSearchSection(showAllAppearanceColorSections, {
                    trc("appearance.text"),
                    trc("appearance.text_disabled"),
                    "text"
                });
                const bool showAppearanceFrameSection = ShouldRenderConfigSearchSection(showAllAppearanceColorSections, {
                    trc("appearance.frame"),
                    trc("appearance.frame_background"),
                    trc("appearance.frame_hovered"),
                    trc("appearance.frame_active"),
                    "frame background"
                });
                const bool showAppearanceTitleBarSection = ShouldRenderConfigSearchSection(showAllAppearanceColorSections, {
                    trc("appearance.title_bar"),
                    trc("appearance.title_background"),
                    trc("appearance.title_active"),
                    trc("appearance.title_collapsed"),
                    "title bar"
                });
                const bool showAppearanceButtonsSection = ShouldRenderConfigSearchSection(showAllAppearanceColorSections, {
                    trc("appearance.buttons"),
                    trc("appearance.button"),
                    trc("appearance.button_hovered"),
                    trc("appearance.button_active"),
                    "button"
                });
                const bool showAppearanceHeadersSection = ShouldRenderConfigSearchSection(showAllAppearanceColorSections, {
                    trc("appearance.headers"),
                    trc("appearance.header"),
                    trc("appearance.header_hovered"),
                    trc("appearance.header_active"),
                    "header"
                });
                const bool showAppearanceTabsSection = ShouldRenderConfigSearchSection(showAllAppearanceColorSections, {
                    trc("appearance.tabs"),
                    trc("appearance.tab"),
                    trc("appearance.tab_hovered"),
                    trc("appearance.tab_selected"),
                    "tab"
                });
                const bool showAppearanceSlidersSection = ShouldRenderConfigSearchSection(showAllAppearanceColorSections, {
                    trc("appearance.sliders_scrollbars"),
                    trc("appearance.slider_grab"),
                    trc("appearance.slider_grab_active"),
                    trc("appearance.scrollbar_bg"),
                    trc("appearance.scrollbar_grab"),
                    trc("appearance.scrollbar_grab_hovered"),
                    trc("appearance.scrollbar_grab_active"),
                    "slider",
                    "slider grab",
                    "scrollbar"
                });
                const bool showAppearanceCheckboxesSection = ShouldRenderConfigSearchSection(showAllAppearanceColorSections, {
                    trc("appearance.checkboxes_selections"),
                    trc("appearance.check_mark"),
                    trc("appearance.text_selected_bg"),
                    "checkbox",
                    "selection"
                });
                const bool showAppearanceSeparatorsSection = ShouldRenderConfigSearchSection(showAllAppearanceColorSections, {
                    trc("appearance.separators_resize_grips"),
                    trc("appearance.separator"),
                    trc("appearance.separator_hovered"),
                    trc("appearance.separator_active"),
                    trc("appearance.resize_grip"),
                    trc("appearance.resize_grip_hovered"),
                    trc("appearance.resize_grip_active"),
                    "separator",
                    "resize grip"
                });
                const bool showAnyAppearanceColorSection = showAppearanceWindowSection || showAppearanceTextSection ||
                                                           showAppearanceFrameSection || showAppearanceTitleBarSection ||
                                                           showAppearanceButtonsSection || showAppearanceHeadersSection ||
                                                           showAppearanceTabsSection || showAppearanceSlidersSection ||
                                                           showAppearanceCheckboxesSection || showAppearanceSeparatorsSection;

                if (showColorSchemeSection) {
                    ImGui::SeparatorText(trc("appearance.color_scheme"));
                    RecordConfigSearchSectionInteractionRect("config.section.appearance.color_scheme");

                    ImGui::Text(trc("appearance.preset_themes"));
                    ImGui::SameLine();
                    HelpMarker(trc("appearance.tooltip.reset_themes"));

                if (ImGui::Button(trc("appearance.dark"))) {
                    ImGui::StyleColorsDark();
                    g_config.appearance.theme = "Dark";
                    SaveTheme();
                }
                ImGui::SameLine();
                if (ImGui::Button(trc("appearance.light"))) {
                    ImGui::StyleColorsLight();
                    g_config.appearance.theme = "Light";
                    SaveTheme();
                }
                ImGui::SameLine();
                if (ImGui::Button(trc("appearance.classic"))) {
                    ImGui::StyleColorsClassic();
                    g_config.appearance.theme = "Classic";
                    SaveTheme();
                }
                ImGui::SameLine();
                if (ImGui::Button(trc("appearance.dracula"))) {
                    ImGuiStyle& style = ImGui::GetStyle();
                    ImGui::StyleColorsDark();
                    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.16f, 0.16f, 0.21f, 1.00f);
                    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.16f, 0.16f, 0.21f, 1.00f);
                    style.Colors[ImGuiCol_PopupBg] = ImVec4(0.16f, 0.16f, 0.21f, 0.94f);
                    style.Colors[ImGuiCol_Border] = ImVec4(0.27f, 0.29f, 0.40f, 1.00f);
                    style.Colors[ImGuiCol_Text] = ImVec4(0.97f, 0.98f, 0.98f, 1.00f);
                    style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.38f, 0.42f, 0.53f, 1.00f);
                    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.27f, 0.29f, 0.40f, 0.54f);
                    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.35f, 0.38f, 0.53f, 0.54f);
                    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.55f, 0.48f, 0.76f, 0.67f);
                    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.16f, 0.16f, 0.21f, 1.00f);
                    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.16f, 0.16f, 0.21f, 1.00f);
                    style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.16f, 0.16f, 0.21f, 0.51f);
                    style.Colors[ImGuiCol_Button] = ImVec4(0.55f, 0.48f, 0.76f, 0.40f);
                    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.55f, 0.48f, 0.76f, 1.00f);
                    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.98f, 0.47f, 0.60f, 1.00f);
                    style.Colors[ImGuiCol_Header] = ImVec4(0.55f, 0.48f, 0.76f, 0.31f);
                    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.55f, 0.48f, 0.76f, 0.80f);
                    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.55f, 0.48f, 0.76f, 1.00f);
                    style.Colors[ImGuiCol_Tab] = ImVec4(0.27f, 0.29f, 0.40f, 0.86f);
                    style.Colors[ImGuiCol_TabHovered] = ImVec4(0.55f, 0.48f, 0.76f, 0.80f);
                    style.Colors[ImGuiCol_TabSelected] = ImVec4(0.55f, 0.48f, 0.76f, 1.00f);
                    style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.55f, 0.48f, 0.76f, 1.00f);
                    style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.98f, 0.47f, 0.60f, 1.00f);
                    style.Colors[ImGuiCol_CheckMark] = ImVec4(0.31f, 0.98f, 0.48f, 1.00f);
                    style.Colors[ImGuiCol_Separator] = ImVec4(0.27f, 0.29f, 0.40f, 1.00f);
                    g_config.appearance.theme = "Dracula";
                    SaveTheme();
                }

                if (ImGui::Button(trc("appearance.nord"))) {
                    ImGuiStyle& style = ImGui::GetStyle();
                    ImGui::StyleColorsDark();
                    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.18f, 0.20f, 0.25f, 1.00f);
                    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.18f, 0.20f, 0.25f, 1.00f);
                    style.Colors[ImGuiCol_PopupBg] = ImVec4(0.18f, 0.20f, 0.25f, 0.94f);
                    style.Colors[ImGuiCol_Border] = ImVec4(0.26f, 0.30f, 0.37f, 1.00f);
                    style.Colors[ImGuiCol_Text] = ImVec4(0.93f, 0.94f, 0.96f, 1.00f);
                    style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.43f, 0.47f, 0.55f, 1.00f);
                    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.26f, 0.30f, 0.37f, 0.54f);
                    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.33f, 0.43f, 0.58f, 0.54f);
                    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.53f, 0.75f, 0.82f, 0.67f);
                    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.18f, 0.20f, 0.25f, 1.00f);
                    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.18f, 0.20f, 0.25f, 1.00f);
                    style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.18f, 0.20f, 0.25f, 0.51f);
                    style.Colors[ImGuiCol_Button] = ImVec4(0.33f, 0.43f, 0.58f, 0.40f);
                    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.53f, 0.75f, 0.82f, 1.00f);
                    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.69f, 0.76f, 0.82f, 1.00f);
                    style.Colors[ImGuiCol_Header] = ImVec4(0.33f, 0.43f, 0.58f, 0.31f);
                    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.53f, 0.75f, 0.82f, 0.80f);
                    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.53f, 0.75f, 0.82f, 1.00f);
                    style.Colors[ImGuiCol_Tab] = ImVec4(0.26f, 0.30f, 0.37f, 0.86f);
                    style.Colors[ImGuiCol_TabHovered] = ImVec4(0.53f, 0.75f, 0.82f, 0.80f);
                    style.Colors[ImGuiCol_TabSelected] = ImVec4(0.53f, 0.75f, 0.82f, 1.00f);
                    style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.53f, 0.75f, 0.82f, 1.00f);
                    style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.69f, 0.76f, 0.82f, 1.00f);
                    style.Colors[ImGuiCol_CheckMark] = ImVec4(0.64f, 0.83f, 0.64f, 1.00f);
                    style.Colors[ImGuiCol_Separator] = ImVec4(0.26f, 0.30f, 0.37f, 1.00f);
                    g_config.appearance.theme = "Nord";
                    SaveTheme();
                }
                ImGui::SameLine();
                if (ImGui::Button(trc("appearance.solarized"))) {
                    ImGuiStyle& style = ImGui::GetStyle();
                    ImGui::StyleColorsDark();
                    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.00f, 0.17f, 0.21f, 1.00f);
                    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.00f, 0.17f, 0.21f, 1.00f);
                    style.Colors[ImGuiCol_PopupBg] = ImVec4(0.00f, 0.17f, 0.21f, 0.94f);
                    style.Colors[ImGuiCol_Border] = ImVec4(0.35f, 0.43f, 0.46f, 0.50f);
                    style.Colors[ImGuiCol_Text] = ImVec4(0.51f, 0.58f, 0.59f, 1.00f);
                    style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.35f, 0.43f, 0.46f, 1.00f);
                    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.03f, 0.21f, 0.26f, 0.54f);
                    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.15f, 0.55f, 0.67f, 0.54f);
                    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.15f, 0.55f, 0.67f, 0.67f);
                    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.00f, 0.17f, 0.21f, 1.00f);
                    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.00f, 0.17f, 0.21f, 1.00f);
                    style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.00f, 0.17f, 0.21f, 0.51f);
                    style.Colors[ImGuiCol_Button] = ImVec4(0.15f, 0.55f, 0.67f, 0.40f);
                    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.15f, 0.55f, 0.67f, 1.00f);
                    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.52f, 0.60f, 0.00f, 1.00f);
                    style.Colors[ImGuiCol_Header] = ImVec4(0.15f, 0.55f, 0.67f, 0.31f);
                    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.15f, 0.55f, 0.67f, 0.80f);
                    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.15f, 0.55f, 0.67f, 1.00f);
                    style.Colors[ImGuiCol_Tab] = ImVec4(0.03f, 0.21f, 0.26f, 0.86f);
                    style.Colors[ImGuiCol_TabHovered] = ImVec4(0.15f, 0.55f, 0.67f, 0.80f);
                    style.Colors[ImGuiCol_TabSelected] = ImVec4(0.15f, 0.55f, 0.67f, 1.00f);
                    style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.15f, 0.55f, 0.67f, 1.00f);
                    style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.52f, 0.60f, 0.00f, 1.00f);
                    style.Colors[ImGuiCol_CheckMark] = ImVec4(0.52f, 0.60f, 0.00f, 1.00f);
                    style.Colors[ImGuiCol_Separator] = ImVec4(0.35f, 0.43f, 0.46f, 0.50f);
                    g_config.appearance.theme = "Solarized";
                    SaveTheme();
                }
                ImGui::SameLine();
                if (ImGui::Button(trc("appearance.monokai"))) {
                    ImGuiStyle& style = ImGui::GetStyle();
                    ImGui::StyleColorsDark();
                    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.15f, 0.15f, 0.13f, 1.00f);
                    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.15f, 0.15f, 0.13f, 1.00f);
                    style.Colors[ImGuiCol_PopupBg] = ImVec4(0.15f, 0.15f, 0.13f, 0.94f);
                    style.Colors[ImGuiCol_Border] = ImVec4(0.46f, 0.44f, 0.37f, 0.50f);
                    style.Colors[ImGuiCol_Text] = ImVec4(0.97f, 0.97f, 0.95f, 1.00f);
                    style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.46f, 0.44f, 0.37f, 1.00f);
                    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.23f, 0.23f, 0.20f, 0.54f);
                    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.40f, 0.88f, 0.33f, 0.54f);
                    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.40f, 0.88f, 0.33f, 0.67f);
                    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.15f, 0.15f, 0.13f, 1.00f);
                    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.15f, 0.15f, 0.13f, 1.00f);
                    style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.15f, 0.15f, 0.13f, 0.51f);
                    style.Colors[ImGuiCol_Button] = ImVec4(0.98f, 0.15f, 0.45f, 0.40f);
                    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.98f, 0.15f, 0.45f, 1.00f);
                    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.40f, 0.88f, 0.33f, 1.00f);
                    style.Colors[ImGuiCol_Header] = ImVec4(0.98f, 0.15f, 0.45f, 0.31f);
                    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.98f, 0.15f, 0.45f, 0.80f);
                    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.98f, 0.15f, 0.45f, 1.00f);
                    style.Colors[ImGuiCol_Tab] = ImVec4(0.23f, 0.23f, 0.20f, 0.86f);
                    style.Colors[ImGuiCol_TabHovered] = ImVec4(0.98f, 0.15f, 0.45f, 0.80f);
                    style.Colors[ImGuiCol_TabSelected] = ImVec4(0.98f, 0.15f, 0.45f, 1.00f);
                    style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.40f, 0.88f, 0.33f, 1.00f);
                    style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.98f, 0.89f, 0.36f, 1.00f);
                    style.Colors[ImGuiCol_CheckMark] = ImVec4(0.40f, 0.88f, 0.33f, 1.00f);
                    style.Colors[ImGuiCol_Separator] = ImVec4(0.46f, 0.44f, 0.37f, 0.50f);
                    g_config.appearance.theme = "Monokai";
                    SaveTheme();
                }
                ImGui::SameLine();
                if (ImGui::Button(trc("appearance.catppuccin"))) {
                    ImGuiStyle& style = ImGui::GetStyle();
                    ImGui::StyleColorsDark();
                    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.12f, 0.12f, 0.18f, 1.00f);
                    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.12f, 0.12f, 0.18f, 1.00f);
                    style.Colors[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.12f, 0.18f, 0.94f);
                    style.Colors[ImGuiCol_Border] = ImVec4(0.27f, 0.28f, 0.35f, 1.00f);
                    style.Colors[ImGuiCol_Text] = ImVec4(0.81f, 0.84f, 0.96f, 1.00f);
                    style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.42f, 0.44f, 0.53f, 1.00f);
                    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.17f, 0.18f, 0.25f, 0.54f);
                    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.53f, 0.56f, 0.89f, 0.54f);
                    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.53f, 0.56f, 0.89f, 0.67f);
                    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.12f, 0.12f, 0.18f, 1.00f);
                    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.12f, 0.18f, 1.00f);
                    style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.12f, 0.12f, 0.18f, 0.51f);
                    style.Colors[ImGuiCol_Button] = ImVec4(0.53f, 0.56f, 0.89f, 0.40f);
                    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.53f, 0.56f, 0.89f, 1.00f);
                    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.95f, 0.55f, 0.66f, 1.00f);
                    style.Colors[ImGuiCol_Header] = ImVec4(0.53f, 0.56f, 0.89f, 0.31f);
                    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.53f, 0.56f, 0.89f, 0.80f);
                    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.53f, 0.56f, 0.89f, 1.00f);
                    style.Colors[ImGuiCol_Tab] = ImVec4(0.17f, 0.18f, 0.25f, 0.86f);
                    style.Colors[ImGuiCol_TabHovered] = ImVec4(0.53f, 0.56f, 0.89f, 0.80f);
                    style.Colors[ImGuiCol_TabSelected] = ImVec4(0.53f, 0.56f, 0.89f, 1.00f);
                    style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.53f, 0.56f, 0.89f, 1.00f);
                    style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.95f, 0.55f, 0.66f, 1.00f);
                    style.Colors[ImGuiCol_CheckMark] = ImVec4(0.65f, 0.89f, 0.63f, 1.00f);
                    style.Colors[ImGuiCol_Separator] = ImVec4(0.27f, 0.28f, 0.35f, 1.00f);
                    g_config.appearance.theme = "Catppuccin";
                    SaveTheme();
                }

                if (ImGui::Button(trc("appearance.one_dark"))) {
                    ImGuiStyle& style = ImGui::GetStyle();
                    ImGui::StyleColorsDark();
                    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.16f, 0.18f, 0.21f, 1.00f);
                    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.16f, 0.18f, 0.21f, 1.00f);
                    style.Colors[ImGuiCol_PopupBg] = ImVec4(0.16f, 0.18f, 0.21f, 0.94f);
                    style.Colors[ImGuiCol_Border] = ImVec4(0.28f, 0.31f, 0.36f, 1.00f);
                    style.Colors[ImGuiCol_Text] = ImVec4(0.67f, 0.73f, 0.82f, 1.00f);
                    style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.39f, 0.42f, 0.47f, 1.00f);
                    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.21f, 0.24f, 0.28f, 0.54f);
                    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.38f, 0.53f, 0.87f, 0.54f);
                    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.38f, 0.53f, 0.87f, 0.67f);
                    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.16f, 0.18f, 0.21f, 1.00f);
                    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.16f, 0.18f, 0.21f, 1.00f);
                    style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.16f, 0.18f, 0.21f, 0.51f);
                    style.Colors[ImGuiCol_Button] = ImVec4(0.38f, 0.53f, 0.87f, 0.40f);
                    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.38f, 0.53f, 0.87f, 1.00f);
                    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.20f, 0.80f, 0.62f, 1.00f);
                    style.Colors[ImGuiCol_Header] = ImVec4(0.38f, 0.53f, 0.87f, 0.31f);
                    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.38f, 0.53f, 0.87f, 0.80f);
                    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.38f, 0.53f, 0.87f, 1.00f);
                    style.Colors[ImGuiCol_Tab] = ImVec4(0.21f, 0.24f, 0.28f, 0.86f);
                    style.Colors[ImGuiCol_TabHovered] = ImVec4(0.38f, 0.53f, 0.87f, 0.80f);
                    style.Colors[ImGuiCol_TabSelected] = ImVec4(0.38f, 0.53f, 0.87f, 1.00f);
                    style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.38f, 0.53f, 0.87f, 1.00f);
                    style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.20f, 0.80f, 0.62f, 1.00f);
                    style.Colors[ImGuiCol_CheckMark] = ImVec4(0.20f, 0.80f, 0.62f, 1.00f);
                    style.Colors[ImGuiCol_Separator] = ImVec4(0.28f, 0.31f, 0.36f, 1.00f);
                    g_config.appearance.theme = "One Dark";
                    SaveTheme();
                }
                ImGui::SameLine();
                if (ImGui::Button(trc("appearance.gruvbox"))) {
                    ImGuiStyle& style = ImGui::GetStyle();
                    ImGui::StyleColorsDark();
                    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.16f, 0.15f, 0.13f, 1.00f);
                    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.16f, 0.15f, 0.13f, 1.00f);
                    style.Colors[ImGuiCol_PopupBg] = ImVec4(0.16f, 0.15f, 0.13f, 0.94f);
                    style.Colors[ImGuiCol_Border] = ImVec4(0.40f, 0.36f, 0.32f, 0.50f);
                    style.Colors[ImGuiCol_Text] = ImVec4(0.92f, 0.86f, 0.70f, 1.00f);
                    style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.57f, 0.51f, 0.45f, 1.00f);
                    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.24f, 0.22f, 0.20f, 0.54f);
                    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.82f, 0.56f, 0.26f, 0.54f);
                    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.82f, 0.56f, 0.26f, 0.67f);
                    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.16f, 0.15f, 0.13f, 1.00f);
                    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.16f, 0.15f, 0.13f, 1.00f);
                    style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.16f, 0.15f, 0.13f, 0.51f);
                    style.Colors[ImGuiCol_Button] = ImVec4(0.82f, 0.56f, 0.26f, 0.40f);
                    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.82f, 0.56f, 0.26f, 1.00f);
                    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.72f, 0.73f, 0.15f, 1.00f);
                    style.Colors[ImGuiCol_Header] = ImVec4(0.82f, 0.56f, 0.26f, 0.31f);
                    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.82f, 0.56f, 0.26f, 0.80f);
                    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.82f, 0.56f, 0.26f, 1.00f);
                    style.Colors[ImGuiCol_Tab] = ImVec4(0.24f, 0.22f, 0.20f, 0.86f);
                    style.Colors[ImGuiCol_TabHovered] = ImVec4(0.82f, 0.56f, 0.26f, 0.80f);
                    style.Colors[ImGuiCol_TabSelected] = ImVec4(0.82f, 0.56f, 0.26f, 1.00f);
                    style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.82f, 0.56f, 0.26f, 1.00f);
                    style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.72f, 0.73f, 0.15f, 1.00f);
                    style.Colors[ImGuiCol_CheckMark] = ImVec4(0.72f, 0.73f, 0.15f, 1.00f);
                    style.Colors[ImGuiCol_Separator] = ImVec4(0.40f, 0.36f, 0.32f, 0.50f);
                    g_config.appearance.theme = "Gruvbox";
                    SaveTheme();
                }
                ImGui::SameLine();
                if (ImGui::Button(trc("appearance.tokyo_night"))) {
                    ImGuiStyle& style = ImGui::GetStyle();
                    ImGui::StyleColorsDark();
                    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.10f, 0.11f, 0.17f, 1.00f);
                    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.10f, 0.11f, 0.17f, 1.00f);
                    style.Colors[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.11f, 0.17f, 0.94f);
                    style.Colors[ImGuiCol_Border] = ImVec4(0.21f, 0.23f, 0.33f, 1.00f);
                    style.Colors[ImGuiCol_Text] = ImVec4(0.66f, 0.70f, 0.87f, 1.00f);
                    style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.33f, 0.36f, 0.51f, 1.00f);
                    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.16f, 0.24f, 0.54f);
                    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.48f, 0.52f, 0.98f, 0.54f);
                    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.48f, 0.52f, 0.98f, 0.67f);
                    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.10f, 0.11f, 0.17f, 1.00f);
                    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.10f, 0.11f, 0.17f, 1.00f);
                    style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.10f, 0.11f, 0.17f, 0.51f);
                    style.Colors[ImGuiCol_Button] = ImVec4(0.48f, 0.52f, 0.98f, 0.40f);
                    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.48f, 0.52f, 0.98f, 1.00f);
                    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.98f, 0.55f, 0.67f, 1.00f);
                    style.Colors[ImGuiCol_Header] = ImVec4(0.48f, 0.52f, 0.98f, 0.31f);
                    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.48f, 0.52f, 0.98f, 0.80f);
                    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.48f, 0.52f, 0.98f, 1.00f);
                    style.Colors[ImGuiCol_Tab] = ImVec4(0.15f, 0.16f, 0.24f, 0.86f);
                    style.Colors[ImGuiCol_TabHovered] = ImVec4(0.48f, 0.52f, 0.98f, 0.80f);
                    style.Colors[ImGuiCol_TabSelected] = ImVec4(0.48f, 0.52f, 0.98f, 1.00f);
                    style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.48f, 0.52f, 0.98f, 1.00f);
                    style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.98f, 0.55f, 0.67f, 1.00f);
                    style.Colors[ImGuiCol_CheckMark] = ImVec4(0.45f, 0.89f, 0.85f, 1.00f);
                    style.Colors[ImGuiCol_Separator] = ImVec4(0.21f, 0.23f, 0.33f, 1.00f);
                    g_config.appearance.theme = "Tokyo Night";
                    SaveTheme();
                }

                if (ImGui::Button(trc("appearance.purple"))) {
                    ImGuiStyle& style = ImGui::GetStyle();
                    ImGui::StyleColorsDark();
                    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.10f, 0.08f, 0.14f, 1.00f);
                    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.10f, 0.08f, 0.14f, 1.00f);
                    style.Colors[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.08f, 0.14f, 0.94f);
                    style.Colors[ImGuiCol_Border] = ImVec4(0.50f, 0.30f, 0.70f, 0.50f);
                    style.Colors[ImGuiCol_Text] = ImVec4(0.95f, 0.90f, 1.00f, 1.00f);
                    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.20f, 0.15f, 0.28f, 0.54f);
                    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.60f, 0.40f, 0.80f, 0.54f);
                    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.60f, 0.40f, 0.80f, 0.67f);
                    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.10f, 0.08f, 0.14f, 1.00f);
                    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.20f, 0.15f, 0.28f, 1.00f);
                    style.Colors[ImGuiCol_Button] = ImVec4(0.55f, 0.35f, 0.75f, 0.40f);
                    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.65f, 0.45f, 0.85f, 1.00f);
                    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.75f, 0.55f, 0.95f, 1.00f);
                    style.Colors[ImGuiCol_Header] = ImVec4(0.55f, 0.35f, 0.75f, 0.31f);
                    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.65f, 0.45f, 0.85f, 0.80f);
                    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.65f, 0.45f, 0.85f, 1.00f);
                    style.Colors[ImGuiCol_Tab] = ImVec4(0.20f, 0.15f, 0.28f, 0.86f);
                    style.Colors[ImGuiCol_TabHovered] = ImVec4(0.65f, 0.45f, 0.85f, 0.80f);
                    style.Colors[ImGuiCol_TabSelected] = ImVec4(0.55f, 0.35f, 0.75f, 1.00f);
                    style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.65f, 0.45f, 0.85f, 1.00f);
                    style.Colors[ImGuiCol_CheckMark] = ImVec4(0.80f, 0.60f, 1.00f, 1.00f);
                    g_config.appearance.theme = "Purple";
                    SaveTheme();
                }
                ImGui::SameLine();
                if (ImGui::Button(trc("appearance.pink"))) {
                    ImGuiStyle& style = ImGui::GetStyle();
                    ImGui::StyleColorsDark();
                    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.12f, 0.08f, 0.10f, 1.00f);
                    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.12f, 0.08f, 0.10f, 1.00f);
                    style.Colors[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.08f, 0.10f, 0.94f);
                    style.Colors[ImGuiCol_Border] = ImVec4(0.80f, 0.40f, 0.60f, 0.50f);
                    style.Colors[ImGuiCol_Text] = ImVec4(1.00f, 0.92f, 0.96f, 1.00f);
                    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.25f, 0.15f, 0.20f, 0.54f);
                    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.90f, 0.50f, 0.70f, 0.54f);
                    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.90f, 0.50f, 0.70f, 0.67f);
                    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.12f, 0.08f, 0.10f, 1.00f);
                    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.25f, 0.15f, 0.20f, 1.00f);
                    style.Colors[ImGuiCol_Button] = ImVec4(0.85f, 0.45f, 0.65f, 0.40f);
                    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.95f, 0.55f, 0.75f, 1.00f);
                    style.Colors[ImGuiCol_ButtonActive] = ImVec4(1.00f, 0.65f, 0.85f, 1.00f);
                    style.Colors[ImGuiCol_Header] = ImVec4(0.85f, 0.45f, 0.65f, 0.31f);
                    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.95f, 0.55f, 0.75f, 0.80f);
                    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.95f, 0.55f, 0.75f, 1.00f);
                    style.Colors[ImGuiCol_Tab] = ImVec4(0.25f, 0.15f, 0.20f, 0.86f);
                    style.Colors[ImGuiCol_TabHovered] = ImVec4(0.95f, 0.55f, 0.75f, 0.80f);
                    style.Colors[ImGuiCol_TabSelected] = ImVec4(0.85f, 0.45f, 0.65f, 1.00f);
                    style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.95f, 0.55f, 0.75f, 1.00f);
                    style.Colors[ImGuiCol_CheckMark] = ImVec4(1.00f, 0.70f, 0.90f, 1.00f);
                    g_config.appearance.theme = "Pink";
                    SaveTheme();
                }
                ImGui::SameLine();
                if (ImGui::Button(trc("appearance.blue"))) {
                    ImGuiStyle& style = ImGui::GetStyle();
                    ImGui::StyleColorsDark();
                    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.10f, 0.14f, 1.00f);
                    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.08f, 0.10f, 0.14f, 1.00f);
                    style.Colors[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.10f, 0.14f, 0.94f);
                    style.Colors[ImGuiCol_Border] = ImVec4(0.30f, 0.50f, 0.80f, 0.50f);
                    style.Colors[ImGuiCol_Text] = ImVec4(0.90f, 0.95f, 1.00f, 1.00f);
                    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.20f, 0.30f, 0.54f);
                    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.40f, 0.60f, 0.90f, 0.54f);
                    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.40f, 0.60f, 0.90f, 0.67f);
                    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.08f, 0.10f, 0.14f, 1.00f);
                    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.15f, 0.20f, 0.30f, 1.00f);
                    style.Colors[ImGuiCol_Button] = ImVec4(0.30f, 0.50f, 0.80f, 0.40f);
                    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.40f, 0.60f, 0.90f, 1.00f);
                    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.50f, 0.70f, 1.00f, 1.00f);
                    style.Colors[ImGuiCol_Header] = ImVec4(0.30f, 0.50f, 0.80f, 0.31f);
                    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.40f, 0.60f, 0.90f, 0.80f);
                    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.40f, 0.60f, 0.90f, 1.00f);
                    style.Colors[ImGuiCol_Tab] = ImVec4(0.15f, 0.20f, 0.30f, 0.86f);
                    style.Colors[ImGuiCol_TabHovered] = ImVec4(0.40f, 0.60f, 0.90f, 0.80f);
                    style.Colors[ImGuiCol_TabSelected] = ImVec4(0.30f, 0.50f, 0.80f, 1.00f);
                    style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.40f, 0.60f, 0.90f, 1.00f);
                    style.Colors[ImGuiCol_CheckMark] = ImVec4(0.50f, 0.75f, 1.00f, 1.00f);
                    g_config.appearance.theme = "Blue";
                    SaveTheme();
                }
                ImGui::SameLine();
                if (ImGui::Button(trc("appearance.teal"))) {
                    ImGuiStyle& style = ImGui::GetStyle();
                    ImGui::StyleColorsDark();
                    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.12f, 0.12f, 1.00f);
                    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.08f, 0.12f, 0.12f, 1.00f);
                    style.Colors[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.12f, 0.12f, 0.94f);
                    style.Colors[ImGuiCol_Border] = ImVec4(0.20f, 0.70f, 0.70f, 0.50f);
                    style.Colors[ImGuiCol_Text] = ImVec4(0.90f, 1.00f, 1.00f, 1.00f);
                    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.12f, 0.22f, 0.22f, 0.54f);
                    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.30f, 0.80f, 0.80f, 0.54f);
                    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.30f, 0.80f, 0.80f, 0.67f);
                    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.08f, 0.12f, 0.12f, 1.00f);
                    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.22f, 0.22f, 1.00f);
                    style.Colors[ImGuiCol_Button] = ImVec4(0.20f, 0.60f, 0.60f, 0.40f);
                    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.30f, 0.80f, 0.80f, 1.00f);
                    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.40f, 0.90f, 0.90f, 1.00f);
                    style.Colors[ImGuiCol_Header] = ImVec4(0.20f, 0.60f, 0.60f, 0.31f);
                    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.30f, 0.80f, 0.80f, 0.80f);
                    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.30f, 0.80f, 0.80f, 1.00f);
                    style.Colors[ImGuiCol_Tab] = ImVec4(0.12f, 0.22f, 0.22f, 0.86f);
                    style.Colors[ImGuiCol_TabHovered] = ImVec4(0.30f, 0.80f, 0.80f, 0.80f);
                    style.Colors[ImGuiCol_TabSelected] = ImVec4(0.20f, 0.60f, 0.60f, 1.00f);
                    style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.30f, 0.80f, 0.80f, 1.00f);
                    style.Colors[ImGuiCol_CheckMark] = ImVec4(0.40f, 1.00f, 1.00f, 1.00f);
                    g_config.appearance.theme = "Teal";
                    SaveTheme();
                }
                ImGui::SameLine();
                if (ImGui::Button(trc("appearance.red"))) {
                    ImGuiStyle& style = ImGui::GetStyle();
                    ImGui::StyleColorsDark();
                    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.12f, 0.08f, 0.08f, 1.00f);
                    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.12f, 0.08f, 0.08f, 1.00f);
                    style.Colors[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.08f, 0.08f, 0.94f);
                    style.Colors[ImGuiCol_Border] = ImVec4(0.80f, 0.30f, 0.30f, 0.50f);
                    style.Colors[ImGuiCol_Text] = ImVec4(1.00f, 0.92f, 0.92f, 1.00f);
                    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.25f, 0.12f, 0.12f, 0.54f);
                    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.90f, 0.35f, 0.35f, 0.54f);
                    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.90f, 0.35f, 0.35f, 0.67f);
                    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.12f, 0.08f, 0.08f, 1.00f);
                    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.25f, 0.12f, 0.12f, 1.00f);
                    style.Colors[ImGuiCol_Button] = ImVec4(0.75f, 0.25f, 0.25f, 0.40f);
                    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.90f, 0.35f, 0.35f, 1.00f);
                    style.Colors[ImGuiCol_ButtonActive] = ImVec4(1.00f, 0.45f, 0.45f, 1.00f);
                    style.Colors[ImGuiCol_Header] = ImVec4(0.75f, 0.25f, 0.25f, 0.31f);
                    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.90f, 0.35f, 0.35f, 0.80f);
                    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.90f, 0.35f, 0.35f, 1.00f);
                    style.Colors[ImGuiCol_Tab] = ImVec4(0.25f, 0.12f, 0.12f, 0.86f);
                    style.Colors[ImGuiCol_TabHovered] = ImVec4(0.90f, 0.35f, 0.35f, 0.80f);
                    style.Colors[ImGuiCol_TabSelected] = ImVec4(0.75f, 0.25f, 0.25f, 1.00f);
                    style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.90f, 0.35f, 0.35f, 1.00f);
                    style.Colors[ImGuiCol_CheckMark] = ImVec4(1.00f, 0.50f, 0.50f, 1.00f);
                    g_config.appearance.theme = "Red";
                    SaveTheme();
                }
                ImGui::SameLine();
                if (ImGui::Button(trc("appearance.green"))) {
                    ImGuiStyle& style = ImGui::GetStyle();
                    ImGui::StyleColorsDark();
                    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.12f, 0.08f, 1.00f);
                    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.08f, 0.12f, 0.08f, 1.00f);
                    style.Colors[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.12f, 0.08f, 0.94f);
                    style.Colors[ImGuiCol_Border] = ImVec4(0.30f, 0.70f, 0.30f, 0.50f);
                    style.Colors[ImGuiCol_Text] = ImVec4(0.92f, 1.00f, 0.92f, 1.00f);
                    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.12f, 0.22f, 0.12f, 0.54f);
                    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.35f, 0.80f, 0.35f, 0.54f);
                    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.35f, 0.80f, 0.35f, 0.67f);
                    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.08f, 0.12f, 0.08f, 1.00f);
                    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.22f, 0.12f, 1.00f);
                    style.Colors[ImGuiCol_Button] = ImVec4(0.25f, 0.60f, 0.25f, 0.40f);
                    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.35f, 0.80f, 0.35f, 1.00f);
                    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.45f, 0.90f, 0.45f, 1.00f);
                    style.Colors[ImGuiCol_Header] = ImVec4(0.25f, 0.60f, 0.25f, 0.31f);
                    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.35f, 0.80f, 0.35f, 0.80f);
                    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.35f, 0.80f, 0.35f, 1.00f);
                    style.Colors[ImGuiCol_Tab] = ImVec4(0.12f, 0.22f, 0.12f, 0.86f);
                    style.Colors[ImGuiCol_TabHovered] = ImVec4(0.35f, 0.80f, 0.35f, 0.80f);
                    style.Colors[ImGuiCol_TabSelected] = ImVec4(0.25f, 0.60f, 0.25f, 1.00f);
                    style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.35f, 0.80f, 0.35f, 1.00f);
                    style.Colors[ImGuiCol_CheckMark] = ImVec4(0.50f, 1.00f, 0.50f, 1.00f);
                    g_config.appearance.theme = "Green";
                    SaveTheme();
                }

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();
                }

                if (showAnyAppearanceColorSection) {
                    ImGui::Text(trc("appearance.custom_colors"));
                    ImGui::SameLine();
                    HelpMarker(trc("appearance.tooltip.custom_colors"));

                    ImGui::Spacing();
                }

                ImGuiStyle& style = ImGui::GetStyle();

                auto beginAppearanceColorSection = [&](const char* interactionId,
                                                       bool visible,
                                                       std::initializer_list<std::string_view> searchTerms,
                                                       const char* label,
                                                       ImGuiTreeNodeFlags baseFlags = ImGuiTreeNodeFlags_None) {
                    if (!visible) {
                        return false;
                    }

                    const bool open = ImGui::CollapsingHeader(
                        label, GetConfigSearchSectionOpenFlags(showAllAppearanceColorSections, searchTerms, baseFlags));
                    RecordConfigSearchSectionInteractionRect(interactionId);
                    if (open) {
                        ImGui::Indent();
                    }
                    return open;
                };

                const bool colorListVisible = showAnyAppearanceColorSection && ImGui::BeginChild("ColorList", ImVec2(0, 400), true);
                if (colorListVisible) {
                    if (beginAppearanceColorSection("config.section.appearance.window", showAppearanceWindowSection, {
                            trc("appearance.window"),
                            trc("appearance.window_background"),
                            trc("appearance.child_background"),
                            trc("appearance.popup_background"),
                            trc("appearance.border"),
                            "window background",
                            "windowbg",
                            "childbg",
                            "popupbg"
                        }, trc("appearance.window"), ImGuiTreeNodeFlags_DefaultOpen)) {
                        if (ImGui::ColorEdit4((tr("appearance.window_background") + "##Col").c_str(), (float*)&style.Colors[ImGuiCol_WindowBg])) {
                            g_config.appearance.customColors["WindowBg"] = {style.Colors[ImGuiCol_WindowBg].x, style.Colors[ImGuiCol_WindowBg].y, style.Colors[ImGuiCol_WindowBg].z, style.Colors[ImGuiCol_WindowBg].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            RequestDeferredThemeSave();
                        }
                        if (ImGui::ColorEdit4((tr("appearance.child_background") + "##Col").c_str(), (float*)&style.Colors[ImGuiCol_ChildBg])) {
                            g_config.appearance.customColors["ChildBg"] = {style.Colors[ImGuiCol_ChildBg].x, style.Colors[ImGuiCol_ChildBg].y, style.Colors[ImGuiCol_ChildBg].z, style.Colors[ImGuiCol_ChildBg].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            RequestDeferredThemeSave();
                        }
                        if (ImGui::ColorEdit4((tr("appearance.popup_background") + "##Col").c_str(), (float*)&style.Colors[ImGuiCol_PopupBg])) {
                            g_config.appearance.customColors["PopupBg"] = {style.Colors[ImGuiCol_PopupBg].x, style.Colors[ImGuiCol_PopupBg].y, style.Colors[ImGuiCol_PopupBg].z, style.Colors[ImGuiCol_PopupBg].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            RequestDeferredThemeSave();
                        }
                        if (ImGui::ColorEdit4((tr("appearance.border") + "##Col").c_str(), (float*)&style.Colors[ImGuiCol_Border])) {
                            g_config.appearance.customColors["Border"] = {style.Colors[ImGuiCol_Border].x, style.Colors[ImGuiCol_Border].y, style.Colors[ImGuiCol_Border].z, style.Colors[ImGuiCol_Border].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            RequestDeferredThemeSave();
                        }
                        ImGui::Unindent();
                    }

                    if (beginAppearanceColorSection("config.section.appearance.text", showAppearanceTextSection, {
                            trc("appearance.text"),
                            trc("appearance.text_disabled"),
                            "text"
                        }, trc("appearance.text"))) {
                        if (ImGui::ColorEdit4((tr("appearance.text") + "##Col").c_str(), (float*)&style.Colors[ImGuiCol_Text])) {
                            g_config.appearance.customColors["Text"] = {style.Colors[ImGuiCol_Text].x, style.Colors[ImGuiCol_Text].y, style.Colors[ImGuiCol_Text].z, style.Colors[ImGuiCol_Text].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            RequestDeferredThemeSave();
                        }
                        if (ImGui::ColorEdit4((tr("appearance.text_disabled") + "##Col").c_str(), (float*)&style.Colors[ImGuiCol_TextDisabled])) {
                            g_config.appearance.customColors["TextDisabled"] = {style.Colors[ImGuiCol_TextDisabled].x, style.Colors[ImGuiCol_TextDisabled].y, style.Colors[ImGuiCol_TextDisabled].z, style.Colors[ImGuiCol_TextDisabled].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            RequestDeferredThemeSave();
                        }
                        ImGui::Unindent();
                    }

                    if (beginAppearanceColorSection("config.section.appearance.frame", showAppearanceFrameSection, {
                            trc("appearance.frame"),
                            trc("appearance.frame_background"),
                            trc("appearance.frame_hovered"),
                            trc("appearance.frame_active"),
                            "frame background"
                        }, trc("appearance.frame"))) {
                        if (ImGui::ColorEdit4((tr("appearance.frame_background") + "##Col").c_str(), (float*)&style.Colors[ImGuiCol_FrameBg])) {
                            g_config.appearance.customColors["FrameBg"] = {style.Colors[ImGuiCol_FrameBg].x, style.Colors[ImGuiCol_FrameBg].y, style.Colors[ImGuiCol_FrameBg].z, style.Colors[ImGuiCol_FrameBg].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            RequestDeferredThemeSave();
                        }
                        if (ImGui::ColorEdit4((tr("appearance.frame_hovered") + "##Col").c_str(), (float*)&style.Colors[ImGuiCol_FrameBgHovered])) {
                            g_config.appearance.customColors["FrameBgHovered"] = {style.Colors[ImGuiCol_FrameBgHovered].x, style.Colors[ImGuiCol_FrameBgHovered].y, style.Colors[ImGuiCol_FrameBgHovered].z, style.Colors[ImGuiCol_FrameBgHovered].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            RequestDeferredThemeSave();
                        }
                        if (ImGui::ColorEdit4((tr("appearance.frame_active") + "##Col").c_str(), (float*)&style.Colors[ImGuiCol_FrameBgActive])) {
                            g_config.appearance.customColors["FrameBgActive"] = {style.Colors[ImGuiCol_FrameBgActive].x, style.Colors[ImGuiCol_FrameBgActive].y, style.Colors[ImGuiCol_FrameBgActive].z, style.Colors[ImGuiCol_FrameBgActive].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            RequestDeferredThemeSave();
                        }
                        ImGui::Unindent();
                    }

                    if (beginAppearanceColorSection("config.section.appearance.title_bar", showAppearanceTitleBarSection, {
                            trc("appearance.title_bar"),
                            trc("appearance.title_background"),
                            trc("appearance.title_active"),
                            trc("appearance.title_collapsed"),
                            "title bar"
                        }, trc("appearance.title_bar"))) {
                        if (ImGui::ColorEdit4((tr("appearance.title_background") + "##Col").c_str(), (float*)&style.Colors[ImGuiCol_TitleBg])) {
                            g_config.appearance.customColors["TitleBg"] = {style.Colors[ImGuiCol_TitleBg].x, style.Colors[ImGuiCol_TitleBg].y, style.Colors[ImGuiCol_TitleBg].z, style.Colors[ImGuiCol_TitleBg].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            RequestDeferredThemeSave();
                        }
                        if (ImGui::ColorEdit4((tr("appearance.title_active") + "##Col").c_str(), (float*)&style.Colors[ImGuiCol_TitleBgActive])) {
                            g_config.appearance.customColors["TitleBgActive"] = {style.Colors[ImGuiCol_TitleBgActive].x, style.Colors[ImGuiCol_TitleBgActive].y, style.Colors[ImGuiCol_TitleBgActive].z, style.Colors[ImGuiCol_TitleBgActive].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            RequestDeferredThemeSave();
                        }
                        if (ImGui::ColorEdit4((tr("appearance.title_collapsed") + "##Col").c_str(), (float*)&style.Colors[ImGuiCol_TitleBgCollapsed])) {
                            g_config.appearance.customColors["TitleBgCollapsed"] = {style.Colors[ImGuiCol_TitleBgCollapsed].x, style.Colors[ImGuiCol_TitleBgCollapsed].y, style.Colors[ImGuiCol_TitleBgCollapsed].z, style.Colors[ImGuiCol_TitleBgCollapsed].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            RequestDeferredThemeSave();
                        }
                        ImGui::Unindent();
                    }

                    if (beginAppearanceColorSection("config.section.appearance.buttons", showAppearanceButtonsSection, {
                            trc("appearance.buttons"),
                            trc("appearance.button"),
                            trc("appearance.button_hovered"),
                            trc("appearance.button_active"),
                            "button"
                        }, trc("appearance.buttons"))) {
                        if (ImGui::ColorEdit4((tr("appearance.button") + "##Col").c_str(), (float*)&style.Colors[ImGuiCol_Button])) {
                            g_config.appearance.customColors["Button"] = {style.Colors[ImGuiCol_Button].x, style.Colors[ImGuiCol_Button].y, style.Colors[ImGuiCol_Button].z, style.Colors[ImGuiCol_Button].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            RequestDeferredThemeSave();
                        }
                        if (ImGui::ColorEdit4((tr("appearance.button_hovered") + "##Col").c_str(), (float*)&style.Colors[ImGuiCol_ButtonHovered])) {
                            g_config.appearance.customColors["ButtonHovered"] = {style.Colors[ImGuiCol_ButtonHovered].x, style.Colors[ImGuiCol_ButtonHovered].y, style.Colors[ImGuiCol_ButtonHovered].z, style.Colors[ImGuiCol_ButtonHovered].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            RequestDeferredThemeSave();
                        }
                        if (ImGui::ColorEdit4((tr("appearance.button_active") + "##Col").c_str(), (float*)&style.Colors[ImGuiCol_ButtonActive])) {
                            g_config.appearance.customColors["ButtonActive"] = {style.Colors[ImGuiCol_ButtonActive].x, style.Colors[ImGuiCol_ButtonActive].y, style.Colors[ImGuiCol_ButtonActive].z, style.Colors[ImGuiCol_ButtonActive].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            RequestDeferredThemeSave();
                        }
                        ImGui::Unindent();
                    }

                    if (beginAppearanceColorSection("config.section.appearance.headers", showAppearanceHeadersSection, {
                            trc("appearance.headers"),
                            trc("appearance.header"),
                            trc("appearance.header_hovered"),
                            trc("appearance.header_active"),
                            "header"
                        }, trc("appearance.headers"))) {
                        if (ImGui::ColorEdit4((tr("appearance.header") + "##Col").c_str(), (float*)&style.Colors[ImGuiCol_Header])) {
                            g_config.appearance.customColors["Header"] = {style.Colors[ImGuiCol_Header].x, style.Colors[ImGuiCol_Header].y, style.Colors[ImGuiCol_Header].z, style.Colors[ImGuiCol_Header].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            RequestDeferredThemeSave();
                        }
                        if (ImGui::ColorEdit4((tr("appearance.header_hovered") + "##Col").c_str(), (float*)&style.Colors[ImGuiCol_HeaderHovered])) {
                            g_config.appearance.customColors["HeaderHovered"] = {style.Colors[ImGuiCol_HeaderHovered].x, style.Colors[ImGuiCol_HeaderHovered].y, style.Colors[ImGuiCol_HeaderHovered].z, style.Colors[ImGuiCol_HeaderHovered].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            RequestDeferredThemeSave();
                        }
                        if (ImGui::ColorEdit4((tr("appearance.header_active") + "##Col").c_str(), (float*)&style.Colors[ImGuiCol_HeaderActive])) {
                            g_config.appearance.customColors["HeaderActive"] = {style.Colors[ImGuiCol_HeaderActive].x, style.Colors[ImGuiCol_HeaderActive].y, style.Colors[ImGuiCol_HeaderActive].z, style.Colors[ImGuiCol_HeaderActive].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            RequestDeferredThemeSave();
                        }
                        ImGui::Unindent();
                    }

                    if (beginAppearanceColorSection("config.section.appearance.tabs", showAppearanceTabsSection, {
                            trc("appearance.tabs"),
                            trc("appearance.tab"),
                            trc("appearance.tab_hovered"),
                            trc("appearance.tab_selected"),
                            "tab"
                        }, trc("appearance.tabs"))) {
                        if (ImGui::ColorEdit4((tr("appearance.tab") + "##Col").c_str(), (float*)&style.Colors[ImGuiCol_Tab])) {
                            g_config.appearance.customColors["Tab"] = {style.Colors[ImGuiCol_Tab].x, style.Colors[ImGuiCol_Tab].y, style.Colors[ImGuiCol_Tab].z, style.Colors[ImGuiCol_Tab].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            RequestDeferredThemeSave();
                        }
                        if (ImGui::ColorEdit4((tr("appearance.tab_hovered") + "##Col").c_str(), (float*)&style.Colors[ImGuiCol_TabHovered])) {
                            g_config.appearance.customColors["TabHovered"] = {style.Colors[ImGuiCol_TabHovered].x, style.Colors[ImGuiCol_TabHovered].y, style.Colors[ImGuiCol_TabHovered].z, style.Colors[ImGuiCol_TabHovered].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            RequestDeferredThemeSave();
                        }
                        if (ImGui::ColorEdit4((tr("appearance.tab_selected") + "##Col").c_str(), (float*)&style.Colors[ImGuiCol_TabSelected])) {
                            g_config.appearance.customColors["TabSelected"] = {style.Colors[ImGuiCol_TabSelected].x, style.Colors[ImGuiCol_TabSelected].y, style.Colors[ImGuiCol_TabSelected].z, style.Colors[ImGuiCol_TabSelected].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            RequestDeferredThemeSave();
                        }
                        ImGui::Unindent();
                    }

                    if (beginAppearanceColorSection("config.section.appearance.sliders_scrollbars", showAppearanceSlidersSection, {
                            trc("appearance.sliders_scrollbars"),
                            trc("appearance.slider_grab"),
                            trc("appearance.slider_grab_active"),
                            trc("appearance.scrollbar_bg"),
                            trc("appearance.scrollbar_grab"),
                            trc("appearance.scrollbar_grab_hovered"),
                            trc("appearance.scrollbar_grab_active"),
                            "slider",
                            "slider grab",
                            "scrollbar"
                        }, trc("appearance.sliders_scrollbars"))) {
                        if (ImGui::ColorEdit4((tr("appearance.slider_grab") + "##Col").c_str(), (float*)&style.Colors[ImGuiCol_SliderGrab])) {
                            g_config.appearance.customColors["SliderGrab"] = {style.Colors[ImGuiCol_SliderGrab].x, style.Colors[ImGuiCol_SliderGrab].y, style.Colors[ImGuiCol_SliderGrab].z, style.Colors[ImGuiCol_SliderGrab].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            RequestDeferredThemeSave();
                        }
                        if (ImGui::ColorEdit4((tr("appearance.slider_grab_active") + "##Col").c_str(), (float*)&style.Colors[ImGuiCol_SliderGrabActive])) {
                            g_config.appearance.customColors["SliderGrabActive"] = {style.Colors[ImGuiCol_SliderGrabActive].x, style.Colors[ImGuiCol_SliderGrabActive].y, style.Colors[ImGuiCol_SliderGrabActive].z, style.Colors[ImGuiCol_SliderGrabActive].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            RequestDeferredThemeSave();
                        }
                        if (ImGui::ColorEdit4((tr("appearance.scrollbar_bg") + "##Col").c_str(), (float*)&style.Colors[ImGuiCol_ScrollbarBg])) {
                            g_config.appearance.customColors["ScrollbarBg"] = {style.Colors[ImGuiCol_ScrollbarBg].x, style.Colors[ImGuiCol_ScrollbarBg].y, style.Colors[ImGuiCol_ScrollbarBg].z, style.Colors[ImGuiCol_ScrollbarBg].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            RequestDeferredThemeSave();
                        }
                        if (ImGui::ColorEdit4((tr("appearance.scrollbar_grab") + "##Col").c_str(), (float*)&style.Colors[ImGuiCol_ScrollbarGrab])) {
                            g_config.appearance.customColors["ScrollbarGrab"] = {style.Colors[ImGuiCol_ScrollbarGrab].x, style.Colors[ImGuiCol_ScrollbarGrab].y, style.Colors[ImGuiCol_ScrollbarGrab].z, style.Colors[ImGuiCol_ScrollbarGrab].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            RequestDeferredThemeSave();
                        }
                        if (ImGui::ColorEdit4((tr("appearance.scrollbar_grab_hovered") + "##Col").c_str(), (float*)&style.Colors[ImGuiCol_ScrollbarGrabHovered])) {
                            g_config.appearance.customColors["ScrollbarGrabHovered"] = {style.Colors[ImGuiCol_ScrollbarGrabHovered].x, style.Colors[ImGuiCol_ScrollbarGrabHovered].y, style.Colors[ImGuiCol_ScrollbarGrabHovered].z, style.Colors[ImGuiCol_ScrollbarGrabHovered].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            RequestDeferredThemeSave();
                        }
                        if (ImGui::ColorEdit4((tr("appearance.scrollbar_grab_active") + "##Col").c_str(), (float*)&style.Colors[ImGuiCol_ScrollbarGrabActive])) {
                            g_config.appearance.customColors["ScrollbarGrabActive"] = {style.Colors[ImGuiCol_ScrollbarGrabActive].x, style.Colors[ImGuiCol_ScrollbarGrabActive].y, style.Colors[ImGuiCol_ScrollbarGrabActive].z, style.Colors[ImGuiCol_ScrollbarGrabActive].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            RequestDeferredThemeSave();
                        }
                        ImGui::Unindent();
                    }

                    if (beginAppearanceColorSection("config.section.appearance.checkboxes_selections", showAppearanceCheckboxesSection, {
                            trc("appearance.checkboxes_selections"),
                            trc("appearance.check_mark"),
                            trc("appearance.text_selected_bg"),
                            "checkbox",
                            "selection"
                        }, trc("appearance.checkboxes_selections"))) {
                        if (ImGui::ColorEdit4((tr("appearance.check_mark") + "##Col").c_str(), (float*)&style.Colors[ImGuiCol_CheckMark])) {
                            g_config.appearance.customColors["CheckMark"] = {style.Colors[ImGuiCol_CheckMark].x, style.Colors[ImGuiCol_CheckMark].y, style.Colors[ImGuiCol_CheckMark].z, style.Colors[ImGuiCol_CheckMark].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            RequestDeferredThemeSave();
                        }
                        if (ImGui::ColorEdit4((tr("appearance.text_selected_bg") + "##Col").c_str(), (float*)&style.Colors[ImGuiCol_TextSelectedBg])) {
                            g_config.appearance.customColors["TextSelectedBg"] = {style.Colors[ImGuiCol_TextSelectedBg].x, style.Colors[ImGuiCol_TextSelectedBg].y, style.Colors[ImGuiCol_TextSelectedBg].z, style.Colors[ImGuiCol_TextSelectedBg].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            RequestDeferredThemeSave();
                        }
                        ImGui::Unindent();
                    }

                    if (beginAppearanceColorSection("config.section.appearance.separators_resize_grips", showAppearanceSeparatorsSection, {
                            trc("appearance.separators_resize_grips"),
                            trc("appearance.separator"),
                            trc("appearance.separator_hovered"),
                            trc("appearance.separator_active"),
                            trc("appearance.resize_grip"),
                            trc("appearance.resize_grip_hovered"),
                            trc("appearance.resize_grip_active"),
                            "separator",
                            "resize grip"
                        }, trc("appearance.separators_resize_grips"))) {
                        if (ImGui::ColorEdit4((tr("appearance.separator") + "##Col").c_str(), (float*)&style.Colors[ImGuiCol_Separator])) {
                            g_config.appearance.customColors["Separator"] = {style.Colors[ImGuiCol_Separator].x, style.Colors[ImGuiCol_Separator].y, style.Colors[ImGuiCol_Separator].z, style.Colors[ImGuiCol_Separator].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            RequestDeferredThemeSave();
                        }
                        if (ImGui::ColorEdit4((tr("appearance.separator_hovered") + "##Col").c_str(), (float*)&style.Colors[ImGuiCol_SeparatorHovered])) {
                            g_config.appearance.customColors["SeparatorHovered"] = {style.Colors[ImGuiCol_SeparatorHovered].x, style.Colors[ImGuiCol_SeparatorHovered].y, style.Colors[ImGuiCol_SeparatorHovered].z, style.Colors[ImGuiCol_SeparatorHovered].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            RequestDeferredThemeSave();
                        }
                        if (ImGui::ColorEdit4((tr("appearance.separator_active") + "##Col").c_str(), (float*)&style.Colors[ImGuiCol_SeparatorActive])) {
                            g_config.appearance.customColors["SeparatorActive"] = {style.Colors[ImGuiCol_SeparatorActive].x, style.Colors[ImGuiCol_SeparatorActive].y, style.Colors[ImGuiCol_SeparatorActive].z, style.Colors[ImGuiCol_SeparatorActive].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            RequestDeferredThemeSave();
                        }
                        if (ImGui::ColorEdit4((tr("appearance.resize_grip") + "##Col").c_str(), (float*)&style.Colors[ImGuiCol_ResizeGrip])) {
                            g_config.appearance.customColors["ResizeGrip"] = {style.Colors[ImGuiCol_ResizeGrip].x, style.Colors[ImGuiCol_ResizeGrip].y, style.Colors[ImGuiCol_ResizeGrip].z, style.Colors[ImGuiCol_ResizeGrip].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            RequestDeferredThemeSave();
                        }
                        if (ImGui::ColorEdit4((tr("appearance.resize_grip_hovered") + "##Col").c_str(), (float*)&style.Colors[ImGuiCol_ResizeGripHovered])) {
                            g_config.appearance.customColors["ResizeGripHovered"] = {style.Colors[ImGuiCol_ResizeGripHovered].x, style.Colors[ImGuiCol_ResizeGripHovered].y, style.Colors[ImGuiCol_ResizeGripHovered].z, style.Colors[ImGuiCol_ResizeGripHovered].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            RequestDeferredThemeSave();
                        }
                        if (ImGui::ColorEdit4((tr("appearance.resize_grip_active") + "##Col").c_str(), (float*)&style.Colors[ImGuiCol_ResizeGripActive])) {
                            g_config.appearance.customColors["ResizeGripActive"] = {style.Colors[ImGuiCol_ResizeGripActive].x, style.Colors[ImGuiCol_ResizeGripActive].y, style.Colors[ImGuiCol_ResizeGripActive].z, style.Colors[ImGuiCol_ResizeGripActive].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            RequestDeferredThemeSave();
                        }
                        ImGui::Unindent();
                    }

                }
                if (showAnyAppearanceColorSection) {
                    ImGui::EndChild();
                }
                // Colour edits apply live but only write theme.toml once the drag/typing/picker interaction ends.
                if (!ImGui::IsAnyItemActive()) { FlushDeferredThemeSave(); }

                ImGui::Spacing();

                if (showColorSchemeSection) {
                    if (ImGui::Button(trc("appearance.reset_to_default_dark"))) {
                        ImGui::StyleColorsDark();
                        g_config.appearance.theme = "Dark";
                        g_config.appearance.customColors.clear();
                        g_configIsDirty = true;
                        SaveTheme();
                    }
                    ImGui::SameLine();
                    HelpMarker(trc("appearance.tooltip.reset_to_default_dark"));
                }

                ImGui::EndTabItem();
            }


