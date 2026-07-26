#include "gui.h"

#include "config/config_toml.h"

#include <filesystem>
#include <fstream>
#include <map>

static void ApplyPresetThemeColors(const std::string& themeName) {
    ImGuiStyle& style = ImGui::GetStyle();

    if (themeName == "Dracula") {
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
    } else if (themeName == "Nord") {
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
    } else if (themeName == "Solarized") {
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
    } else if (themeName == "Monokai") {
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
    } else if (themeName == "Catppuccin") {
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
    } else if (themeName == "One Dark") {
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
    } else if (themeName == "Gruvbox") {
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
    } else if (themeName == "Tokyo Night") {
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
    } else if (themeName == "Purple") {
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
    } else if (themeName == "Pink") {
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
    } else if (themeName == "Blue") {
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
    } else if (themeName == "Teal") {
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
    } else if (themeName == "Red") {
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
    } else if (themeName == "Green") {
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
    }
}

void ApplyAppearanceConfig() {
    const std::string& theme = g_config.appearance.theme;

    ImGui::StyleColorsDark();

    if (theme == "Light") {
        ImGui::StyleColorsLight();
    } else if (theme == "Classic") {
        ImGui::StyleColorsClassic();
    } else if (theme == "Dracula" || theme == "Nord" || theme == "Solarized" || theme == "Monokai" || theme == "Catppuccin" ||
               theme == "One Dark" || theme == "Gruvbox" || theme == "Tokyo Night" || theme == "Purple" || theme == "Pink" ||
               theme == "Blue" || theme == "Teal" || theme == "Red" || theme == "Green") {
        ApplyPresetThemeColors(theme);
    }

    ImGui::GetStyle().Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.5f);

    if (theme == "Custom" && !g_config.appearance.customColors.empty()) {
        ImGuiStyle& style = ImGui::GetStyle();

        static const std::map<std::string, ImGuiCol_> colorNameToIdx = { { "WindowBg", ImGuiCol_WindowBg },
                                                                         { "ChildBg", ImGuiCol_ChildBg },
                                                                         { "PopupBg", ImGuiCol_PopupBg },
                                                                         { "Border", ImGuiCol_Border },
                                                                         { "Text", ImGuiCol_Text },
                                                                         { "TextDisabled", ImGuiCol_TextDisabled },
                                                                         { "FrameBg", ImGuiCol_FrameBg },
                                                                         { "FrameBgHovered", ImGuiCol_FrameBgHovered },
                                                                         { "FrameBgActive", ImGuiCol_FrameBgActive },
                                                                         { "TitleBg", ImGuiCol_TitleBg },
                                                                         { "TitleBgActive", ImGuiCol_TitleBgActive },
                                                                         { "TitleBgCollapsed", ImGuiCol_TitleBgCollapsed },
                                                                         { "Button", ImGuiCol_Button },
                                                                         { "ButtonHovered", ImGuiCol_ButtonHovered },
                                                                         { "ButtonActive", ImGuiCol_ButtonActive },
                                                                         { "Header", ImGuiCol_Header },
                                                                         { "HeaderHovered", ImGuiCol_HeaderHovered },
                                                                         { "HeaderActive", ImGuiCol_HeaderActive },
                                                                         { "Tab", ImGuiCol_Tab },
                                                                         { "TabHovered", ImGuiCol_TabHovered },
                                                                         { "TabSelected", ImGuiCol_TabSelected },
                                                                         { "SliderGrab", ImGuiCol_SliderGrab },
                                                                         { "SliderGrabActive", ImGuiCol_SliderGrabActive },
                                                                         { "ScrollbarBg", ImGuiCol_ScrollbarBg },
                                                                         { "ScrollbarGrab", ImGuiCol_ScrollbarGrab },
                                                                         { "ScrollbarGrabHovered", ImGuiCol_ScrollbarGrabHovered },
                                                                         { "ScrollbarGrabActive", ImGuiCol_ScrollbarGrabActive },
                                                                         { "CheckMark", ImGuiCol_CheckMark },
                                                                         { "TextSelectedBg", ImGuiCol_TextSelectedBg },
                                                                         { "Separator", ImGuiCol_Separator },
                                                                         { "SeparatorHovered", ImGuiCol_SeparatorHovered },
                                                                         { "SeparatorActive", ImGuiCol_SeparatorActive },
                                                                         { "ResizeGrip", ImGuiCol_ResizeGrip },
                                                                         { "ResizeGripHovered", ImGuiCol_ResizeGripHovered },
                                                                         { "ResizeGripActive", ImGuiCol_ResizeGripActive } };

        for (const auto& [name, color] : g_config.appearance.customColors) {
            auto it = colorNameToIdx.find(name);
            if (it != colorNameToIdx.end()) { style.Colors[it->second] = ImVec4(color.r, color.g, color.b, color.a); }
        }
    }

    Log("Applied appearance config: theme={}", theme);
}

void SaveTheme() {
    if (g_toolscreenPath.empty()) {
        Log("ERROR: Cannot save theme, toolscreen path is not available.");
        return;
    }

    std::wstring themePath = g_toolscreenPath + L"\\theme.toml";
    try {
        toml::table tbl;
        tbl.insert_or_assign("theme", g_config.appearance.theme);

        toml::table colorsTbl;
        for (const auto& [name, color] : g_config.appearance.customColors) {
            if (!IsValidAppearanceCustomColorKey(name)) {
                continue;
            }
            colorsTbl.insert(name, ColorToTomlArray(color));
        }
        tbl.insert_or_assign("customColors", colorsTbl);

        std::ofstream o(std::filesystem::path(themePath), std::ios::binary | std::ios::trunc);
        if (!o.is_open()) {
            Log("ERROR: Failed to open theme.toml for writing.");
            return;
        }
        o << tbl;
        o.close();
        Log("Saved theme to theme.toml: {}", g_config.appearance.theme);
    } catch (const std::exception& e) { Log("ERROR: Failed to save theme: {}", e.what()); }
}

void LoadTheme() {
    if (g_toolscreenPath.empty()) {
        Log("WARNING: Cannot load theme, toolscreen path is not available.");
        return;
    }

    std::wstring themePath = g_toolscreenPath + L"\\theme.toml";
    std::ifstream testFile(std::filesystem::path(themePath), std::ios::binary);
    if (!testFile.good()) {
        Log("theme.toml not found, using default theme.");
        return;
    }
    try {
        toml::table tbl;
#if TOML_EXCEPTIONS
        tbl = toml::parse(testFile, themePath);
#else
        toml::parse_result result = toml::parse(testFile, themePath);
        if (!result) {
            const auto& err = result.error();
            Log("ERROR: Failed to parse theme.toml: {}", err.description());
            return;
        }
        tbl = std::move(result).table();
#endif
        const bool cleanedThemeCustomColors = SanitizeAppearanceCustomColorsTable(tbl, "customColors");
        if (tbl.contains("theme")) {
            std::string themeName = tbl["theme"].value_or<std::string>("Dark");
            g_config.appearance.theme = themeName;
            Log("Loaded theme from theme.toml: {}", themeName);
        }

        if (const toml::node* ccNode = tbl.get("customColors")) {
            if (const toml::table* colorsTbl = ccNode->as_table()) {
                g_config.appearance.customColors.clear();
                for (const auto& [key, value] : *colorsTbl) {
                    const std::string keyName(key.str());
                    if (!IsValidAppearanceCustomColorKey(keyName)) {
                        continue;
                    }
                    if (auto arr = value.as_array(); IsValidAppearanceCustomColorArray(arr)) {
                        g_config.appearance.customColors[keyName] =
                            ColorFromTomlArray(arr, { 0.0f, 0.0f, 0.0f, 1.0f });
                    }
                }
            }
        }

        if (cleanedThemeCustomColors) {
            SaveTheme();
            Log("WARNING: Removed invalid customColors entries while loading theme.toml.");
        }
    } catch (const std::exception& e) { Log("ERROR: Failed to load theme: {}", e.what()); }
}
