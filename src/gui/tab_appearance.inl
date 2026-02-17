            if (ImGui::BeginTabItem("Appearance")) {
                g_currentlyEditingMirror = "";
                g_imageDragMode.store(false);
                g_windowOverlayDragMode.store(false);

                ImGui::SeparatorText("Color Scheme");

                // Preset themes
                ImGui::Text("Preset Themes:");
                ImGui::SameLine();
                HelpMarker("Select a preset theme or customize individual colors below.");

                // Row 1: ImGui defaults
                if (ImGui::Button("Dark")) {
                    ImGui::StyleColorsDark();
                    g_config.appearance.theme = "Dark";
                    SaveTheme();
                }
                ImGui::SameLine();
                if (ImGui::Button("Light")) {
                    ImGui::StyleColorsLight();
                    g_config.appearance.theme = "Light";
                    SaveTheme();
                }
                ImGui::SameLine();
                if (ImGui::Button("Classic")) {
                    ImGui::StyleColorsClassic();
                    g_config.appearance.theme = "Classic";
                    SaveTheme();
                }
                ImGui::SameLine();
                if (ImGui::Button("Dracula")) {
                    ImGuiStyle& style = ImGui::GetStyle();
                    ImGui::StyleColorsDark();
                    // Dracula theme colors
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

                // Row 2: More themes
                if (ImGui::Button("Nord")) {
                    ImGuiStyle& style = ImGui::GetStyle();
                    ImGui::StyleColorsDark();
                    // Nord theme colors
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
                if (ImGui::Button("Solarized")) {
                    ImGuiStyle& style = ImGui::GetStyle();
                    ImGui::StyleColorsDark();
                    // Solarized Dark theme colors
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
                if (ImGui::Button("Monokai")) {
                    ImGuiStyle& style = ImGui::GetStyle();
                    ImGui::StyleColorsDark();
                    // Monokai theme colors
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
                if (ImGui::Button("Catppuccin")) {
                    ImGuiStyle& style = ImGui::GetStyle();
                    ImGui::StyleColorsDark();
                    // Catppuccin Mocha theme colors
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

                // Row 3: More themes
                if (ImGui::Button("One Dark")) {
                    ImGuiStyle& style = ImGui::GetStyle();
                    ImGui::StyleColorsDark();
                    // One Dark theme colors
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
                if (ImGui::Button("Gruvbox")) {
                    ImGuiStyle& style = ImGui::GetStyle();
                    ImGui::StyleColorsDark();
                    // Gruvbox Dark theme colors
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
                if (ImGui::Button("Tokyo Night")) {
                    ImGuiStyle& style = ImGui::GetStyle();
                    ImGui::StyleColorsDark();
                    // Tokyo Night theme colors
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

                // Row 4: Simple color themes
                if (ImGui::Button("Purple")) {
                    ImGuiStyle& style = ImGui::GetStyle();
                    ImGui::StyleColorsDark();
                    // Purple accent theme
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
                if (ImGui::Button("Pink")) {
                    ImGuiStyle& style = ImGui::GetStyle();
                    ImGui::StyleColorsDark();
                    // Pink accent theme
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
                if (ImGui::Button("Blue")) {
                    ImGuiStyle& style = ImGui::GetStyle();
                    ImGui::StyleColorsDark();
                    // Blue accent theme
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
                if (ImGui::Button("Teal")) {
                    ImGuiStyle& style = ImGui::GetStyle();
                    ImGui::StyleColorsDark();
                    // Teal accent theme
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
                if (ImGui::Button("Red")) {
                    ImGuiStyle& style = ImGui::GetStyle();
                    ImGui::StyleColorsDark();
                    // Red accent theme
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
                if (ImGui::Button("Green")) {
                    ImGuiStyle& style = ImGui::GetStyle();
                    ImGui::StyleColorsDark();
                    // Green accent theme
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

                // Custom color editing section
                ImGui::Text("Custom Colors:");
                ImGui::SameLine();
                HelpMarker("Click on a color to open the color picker. Changes are applied immediately.");

                ImGui::Spacing();

                // Get the current style for editing
                ImGuiStyle& style = ImGui::GetStyle();

                // Show colors in a scrollable child region
                if (ImGui::BeginChild("ColorList", ImVec2(0, 400), true)) {
                    // Organize colors into logical groups
                    if (ImGui::CollapsingHeader("Window", ImGuiTreeNodeFlags_DefaultOpen)) {
                        ImGui::Indent();
                        if (ImGui::ColorEdit4("Window Background##Col", (float*)&style.Colors[ImGuiCol_WindowBg])) {
                            g_config.appearance.customColors["WindowBg"] = {style.Colors[ImGuiCol_WindowBg].x, style.Colors[ImGuiCol_WindowBg].y, style.Colors[ImGuiCol_WindowBg].z, style.Colors[ImGuiCol_WindowBg].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            SaveTheme();
                        }
                        if (ImGui::ColorEdit4("Child Background##Col", (float*)&style.Colors[ImGuiCol_ChildBg])) {
                            g_config.appearance.customColors["ChildBg"] = {style.Colors[ImGuiCol_ChildBg].x, style.Colors[ImGuiCol_ChildBg].y, style.Colors[ImGuiCol_ChildBg].z, style.Colors[ImGuiCol_ChildBg].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            SaveTheme();
                        }
                        if (ImGui::ColorEdit4("Popup Background##Col", (float*)&style.Colors[ImGuiCol_PopupBg])) {
                            g_config.appearance.customColors["PopupBg"] = {style.Colors[ImGuiCol_PopupBg].x, style.Colors[ImGuiCol_PopupBg].y, style.Colors[ImGuiCol_PopupBg].z, style.Colors[ImGuiCol_PopupBg].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            SaveTheme();
                        }
                        if (ImGui::ColorEdit4("Border##Col", (float*)&style.Colors[ImGuiCol_Border])) {
                            g_config.appearance.customColors["Border"] = {style.Colors[ImGuiCol_Border].x, style.Colors[ImGuiCol_Border].y, style.Colors[ImGuiCol_Border].z, style.Colors[ImGuiCol_Border].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            SaveTheme();
                        }
                        ImGui::Unindent();
                    }

                    if (ImGui::CollapsingHeader("Text")) {
                        ImGui::Indent();
                        if (ImGui::ColorEdit4("Text##Col", (float*)&style.Colors[ImGuiCol_Text])) {
                            g_config.appearance.customColors["Text"] = {style.Colors[ImGuiCol_Text].x, style.Colors[ImGuiCol_Text].y, style.Colors[ImGuiCol_Text].z, style.Colors[ImGuiCol_Text].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            SaveTheme();
                        }
                        if (ImGui::ColorEdit4("Text Disabled##Col", (float*)&style.Colors[ImGuiCol_TextDisabled])) {
                            g_config.appearance.customColors["TextDisabled"] = {style.Colors[ImGuiCol_TextDisabled].x, style.Colors[ImGuiCol_TextDisabled].y, style.Colors[ImGuiCol_TextDisabled].z, style.Colors[ImGuiCol_TextDisabled].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            SaveTheme();
                        }
                        ImGui::Unindent();
                    }

                    if (ImGui::CollapsingHeader("Frame (Input Fields)")) {
                        ImGui::Indent();
                        if (ImGui::ColorEdit4("Frame Background##Col", (float*)&style.Colors[ImGuiCol_FrameBg])) {
                            g_config.appearance.customColors["FrameBg"] = {style.Colors[ImGuiCol_FrameBg].x, style.Colors[ImGuiCol_FrameBg].y, style.Colors[ImGuiCol_FrameBg].z, style.Colors[ImGuiCol_FrameBg].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            SaveTheme();
                        }
                        if (ImGui::ColorEdit4("Frame Hovered##Col", (float*)&style.Colors[ImGuiCol_FrameBgHovered])) {
                            g_config.appearance.customColors["FrameBgHovered"] = {style.Colors[ImGuiCol_FrameBgHovered].x, style.Colors[ImGuiCol_FrameBgHovered].y, style.Colors[ImGuiCol_FrameBgHovered].z, style.Colors[ImGuiCol_FrameBgHovered].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            SaveTheme();
                        }
                        if (ImGui::ColorEdit4("Frame Active##Col", (float*)&style.Colors[ImGuiCol_FrameBgActive])) {
                            g_config.appearance.customColors["FrameBgActive"] = {style.Colors[ImGuiCol_FrameBgActive].x, style.Colors[ImGuiCol_FrameBgActive].y, style.Colors[ImGuiCol_FrameBgActive].z, style.Colors[ImGuiCol_FrameBgActive].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            SaveTheme();
                        }
                        ImGui::Unindent();
                    }

                    if (ImGui::CollapsingHeader("Title Bar")) {
                        ImGui::Indent();
                        if (ImGui::ColorEdit4("Title Background##Col", (float*)&style.Colors[ImGuiCol_TitleBg])) {
                            g_config.appearance.customColors["TitleBg"] = {style.Colors[ImGuiCol_TitleBg].x, style.Colors[ImGuiCol_TitleBg].y, style.Colors[ImGuiCol_TitleBg].z, style.Colors[ImGuiCol_TitleBg].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            SaveTheme();
                        }
                        if (ImGui::ColorEdit4("Title Active##Col", (float*)&style.Colors[ImGuiCol_TitleBgActive])) {
                            g_config.appearance.customColors["TitleBgActive"] = {style.Colors[ImGuiCol_TitleBgActive].x, style.Colors[ImGuiCol_TitleBgActive].y, style.Colors[ImGuiCol_TitleBgActive].z, style.Colors[ImGuiCol_TitleBgActive].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            SaveTheme();
                        }
                        if (ImGui::ColorEdit4("Title Collapsed##Col", (float*)&style.Colors[ImGuiCol_TitleBgCollapsed])) {
                            g_config.appearance.customColors["TitleBgCollapsed"] = {style.Colors[ImGuiCol_TitleBgCollapsed].x, style.Colors[ImGuiCol_TitleBgCollapsed].y, style.Colors[ImGuiCol_TitleBgCollapsed].z, style.Colors[ImGuiCol_TitleBgCollapsed].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            SaveTheme();
                        }
                        ImGui::Unindent();
                    }

                    if (ImGui::CollapsingHeader("Buttons")) {
                        ImGui::Indent();
                        if (ImGui::ColorEdit4("Button##Col", (float*)&style.Colors[ImGuiCol_Button])) {
                            g_config.appearance.customColors["Button"] = {style.Colors[ImGuiCol_Button].x, style.Colors[ImGuiCol_Button].y, style.Colors[ImGuiCol_Button].z, style.Colors[ImGuiCol_Button].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            SaveTheme();
                        }
                        if (ImGui::ColorEdit4("Button Hovered##Col", (float*)&style.Colors[ImGuiCol_ButtonHovered])) {
                            g_config.appearance.customColors["ButtonHovered"] = {style.Colors[ImGuiCol_ButtonHovered].x, style.Colors[ImGuiCol_ButtonHovered].y, style.Colors[ImGuiCol_ButtonHovered].z, style.Colors[ImGuiCol_ButtonHovered].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            SaveTheme();
                        }
                        if (ImGui::ColorEdit4("Button Active##Col", (float*)&style.Colors[ImGuiCol_ButtonActive])) {
                            g_config.appearance.customColors["ButtonActive"] = {style.Colors[ImGuiCol_ButtonActive].x, style.Colors[ImGuiCol_ButtonActive].y, style.Colors[ImGuiCol_ButtonActive].z, style.Colors[ImGuiCol_ButtonActive].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            SaveTheme();
                        }
                        ImGui::Unindent();
                    }

                    if (ImGui::CollapsingHeader("Headers")) {
                        ImGui::Indent();
                        if (ImGui::ColorEdit4("Header##Col", (float*)&style.Colors[ImGuiCol_Header])) {
                            g_config.appearance.customColors["Header"] = {style.Colors[ImGuiCol_Header].x, style.Colors[ImGuiCol_Header].y, style.Colors[ImGuiCol_Header].z, style.Colors[ImGuiCol_Header].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            SaveTheme();
                        }
                        if (ImGui::ColorEdit4("Header Hovered##Col", (float*)&style.Colors[ImGuiCol_HeaderHovered])) {
                            g_config.appearance.customColors["HeaderHovered"] = {style.Colors[ImGuiCol_HeaderHovered].x, style.Colors[ImGuiCol_HeaderHovered].y, style.Colors[ImGuiCol_HeaderHovered].z, style.Colors[ImGuiCol_HeaderHovered].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            SaveTheme();
                        }
                        if (ImGui::ColorEdit4("Header Active##Col", (float*)&style.Colors[ImGuiCol_HeaderActive])) {
                            g_config.appearance.customColors["HeaderActive"] = {style.Colors[ImGuiCol_HeaderActive].x, style.Colors[ImGuiCol_HeaderActive].y, style.Colors[ImGuiCol_HeaderActive].z, style.Colors[ImGuiCol_HeaderActive].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            SaveTheme();
                        }
                        ImGui::Unindent();
                    }

                    if (ImGui::CollapsingHeader("Tabs")) {
                        ImGui::Indent();
                        if (ImGui::ColorEdit4("Tab##Col", (float*)&style.Colors[ImGuiCol_Tab])) {
                            g_config.appearance.customColors["Tab"] = {style.Colors[ImGuiCol_Tab].x, style.Colors[ImGuiCol_Tab].y, style.Colors[ImGuiCol_Tab].z, style.Colors[ImGuiCol_Tab].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            SaveTheme();
                        }
                        if (ImGui::ColorEdit4("Tab Hovered##Col", (float*)&style.Colors[ImGuiCol_TabHovered])) {
                            g_config.appearance.customColors["TabHovered"] = {style.Colors[ImGuiCol_TabHovered].x, style.Colors[ImGuiCol_TabHovered].y, style.Colors[ImGuiCol_TabHovered].z, style.Colors[ImGuiCol_TabHovered].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            SaveTheme();
                        }
                        if (ImGui::ColorEdit4("Tab Selected##Col", (float*)&style.Colors[ImGuiCol_TabSelected])) {
                            g_config.appearance.customColors["TabSelected"] = {style.Colors[ImGuiCol_TabSelected].x, style.Colors[ImGuiCol_TabSelected].y, style.Colors[ImGuiCol_TabSelected].z, style.Colors[ImGuiCol_TabSelected].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            SaveTheme();
                        }
                        ImGui::Unindent();
                    }

                    if (ImGui::CollapsingHeader("Sliders & Scrollbars")) {
                        ImGui::Indent();
                        if (ImGui::ColorEdit4("Slider Grab##Col", (float*)&style.Colors[ImGuiCol_SliderGrab])) {
                            g_config.appearance.customColors["SliderGrab"] = {style.Colors[ImGuiCol_SliderGrab].x, style.Colors[ImGuiCol_SliderGrab].y, style.Colors[ImGuiCol_SliderGrab].z, style.Colors[ImGuiCol_SliderGrab].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            SaveTheme();
                        }
                        if (ImGui::ColorEdit4("Slider Grab Active##Col", (float*)&style.Colors[ImGuiCol_SliderGrabActive])) {
                            g_config.appearance.customColors["SliderGrabActive"] = {style.Colors[ImGuiCol_SliderGrabActive].x, style.Colors[ImGuiCol_SliderGrabActive].y, style.Colors[ImGuiCol_SliderGrabActive].z, style.Colors[ImGuiCol_SliderGrabActive].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            SaveTheme();
                        }
                        if (ImGui::ColorEdit4("Scrollbar Background##Col", (float*)&style.Colors[ImGuiCol_ScrollbarBg])) {
                            g_config.appearance.customColors["ScrollbarBg"] = {style.Colors[ImGuiCol_ScrollbarBg].x, style.Colors[ImGuiCol_ScrollbarBg].y, style.Colors[ImGuiCol_ScrollbarBg].z, style.Colors[ImGuiCol_ScrollbarBg].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            SaveTheme();
                        }
                        if (ImGui::ColorEdit4("Scrollbar Grab##Col", (float*)&style.Colors[ImGuiCol_ScrollbarGrab])) {
                            g_config.appearance.customColors["ScrollbarGrab"] = {style.Colors[ImGuiCol_ScrollbarGrab].x, style.Colors[ImGuiCol_ScrollbarGrab].y, style.Colors[ImGuiCol_ScrollbarGrab].z, style.Colors[ImGuiCol_ScrollbarGrab].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            SaveTheme();
                        }
                        if (ImGui::ColorEdit4("Scrollbar Grab Hovered##Col", (float*)&style.Colors[ImGuiCol_ScrollbarGrabHovered])) {
                            g_config.appearance.customColors["ScrollbarGrabHovered"] = {style.Colors[ImGuiCol_ScrollbarGrabHovered].x, style.Colors[ImGuiCol_ScrollbarGrabHovered].y, style.Colors[ImGuiCol_ScrollbarGrabHovered].z, style.Colors[ImGuiCol_ScrollbarGrabHovered].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            SaveTheme();
                        }
                        if (ImGui::ColorEdit4("Scrollbar Grab Active##Col", (float*)&style.Colors[ImGuiCol_ScrollbarGrabActive])) {
                            g_config.appearance.customColors["ScrollbarGrabActive"] = {style.Colors[ImGuiCol_ScrollbarGrabActive].x, style.Colors[ImGuiCol_ScrollbarGrabActive].y, style.Colors[ImGuiCol_ScrollbarGrabActive].z, style.Colors[ImGuiCol_ScrollbarGrabActive].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            SaveTheme();
                        }
                        ImGui::Unindent();
                    }

                    if (ImGui::CollapsingHeader("Checkboxes & Selections")) {
                        ImGui::Indent();
                        if (ImGui::ColorEdit4("Check Mark##Col", (float*)&style.Colors[ImGuiCol_CheckMark])) {
                            g_config.appearance.customColors["CheckMark"] = {style.Colors[ImGuiCol_CheckMark].x, style.Colors[ImGuiCol_CheckMark].y, style.Colors[ImGuiCol_CheckMark].z, style.Colors[ImGuiCol_CheckMark].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            SaveTheme();
                        }
                        if (ImGui::ColorEdit4("Text Selected Background##Col", (float*)&style.Colors[ImGuiCol_TextSelectedBg])) {
                            g_config.appearance.customColors["TextSelectedBg"] = {style.Colors[ImGuiCol_TextSelectedBg].x, style.Colors[ImGuiCol_TextSelectedBg].y, style.Colors[ImGuiCol_TextSelectedBg].z, style.Colors[ImGuiCol_TextSelectedBg].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            SaveTheme();
                        }
                        ImGui::Unindent();
                    }

                    if (ImGui::CollapsingHeader("Separators & Resize Grips")) {
                        ImGui::Indent();
                        if (ImGui::ColorEdit4("Separator##Col", (float*)&style.Colors[ImGuiCol_Separator])) {
                            g_config.appearance.customColors["Separator"] = {style.Colors[ImGuiCol_Separator].x, style.Colors[ImGuiCol_Separator].y, style.Colors[ImGuiCol_Separator].z, style.Colors[ImGuiCol_Separator].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            SaveTheme();
                        }
                        if (ImGui::ColorEdit4("Separator Hovered##Col", (float*)&style.Colors[ImGuiCol_SeparatorHovered])) {
                            g_config.appearance.customColors["SeparatorHovered"] = {style.Colors[ImGuiCol_SeparatorHovered].x, style.Colors[ImGuiCol_SeparatorHovered].y, style.Colors[ImGuiCol_SeparatorHovered].z, style.Colors[ImGuiCol_SeparatorHovered].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            SaveTheme();
                        }
                        if (ImGui::ColorEdit4("Separator Active##Col", (float*)&style.Colors[ImGuiCol_SeparatorActive])) {
                            g_config.appearance.customColors["SeparatorActive"] = {style.Colors[ImGuiCol_SeparatorActive].x, style.Colors[ImGuiCol_SeparatorActive].y, style.Colors[ImGuiCol_SeparatorActive].z, style.Colors[ImGuiCol_SeparatorActive].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            SaveTheme();
                        }
                        if (ImGui::ColorEdit4("Resize Grip##Col", (float*)&style.Colors[ImGuiCol_ResizeGrip])) {
                            g_config.appearance.customColors["ResizeGrip"] = {style.Colors[ImGuiCol_ResizeGrip].x, style.Colors[ImGuiCol_ResizeGrip].y, style.Colors[ImGuiCol_ResizeGrip].z, style.Colors[ImGuiCol_ResizeGrip].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            SaveTheme();
                        }
                        if (ImGui::ColorEdit4("Resize Grip Hovered##Col", (float*)&style.Colors[ImGuiCol_ResizeGripHovered])) {
                            g_config.appearance.customColors["ResizeGripHovered"] = {style.Colors[ImGuiCol_ResizeGripHovered].x, style.Colors[ImGuiCol_ResizeGripHovered].y, style.Colors[ImGuiCol_ResizeGripHovered].z, style.Colors[ImGuiCol_ResizeGripHovered].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            SaveTheme();
                        }
                        if (ImGui::ColorEdit4("Resize Grip Active##Col", (float*)&style.Colors[ImGuiCol_ResizeGripActive])) {
                            g_config.appearance.customColors["ResizeGripActive"] = {style.Colors[ImGuiCol_ResizeGripActive].x, style.Colors[ImGuiCol_ResizeGripActive].y, style.Colors[ImGuiCol_ResizeGripActive].z, style.Colors[ImGuiCol_ResizeGripActive].w};
                            g_config.appearance.theme = "Custom";
                            g_configIsDirty = true;
                            SaveTheme();
                        }
                        ImGui::Unindent();
                    }

                    ImGui::EndChild();
                }

                ImGui::Spacing();

                // Reset button
                if (ImGui::Button("Reset to Default (Dark)")) {
                    ImGui::StyleColorsDark();
                    g_config.appearance.theme = "Dark";
                    g_config.appearance.customColors.clear();
                    g_configIsDirty = true;
                    SaveTheme();
                }
                ImGui::SameLine();
                HelpMarker("Reset all colors to the default dark theme.");

                ImGui::EndTabItem();
            }
