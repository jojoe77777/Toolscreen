if (BeginSelectableSettingsTopTabItem(trc("label.overlays"))) {
    if (ImGui::BeginTabBar("OverlaySettingsTabs")) {
        if (ShouldRenderConfigNestedTab(ConfigNestedTabId::Mirrors)) {
#include "tab_mirrors.inl"
        }
        if (ShouldRenderConfigNestedTab(ConfigNestedTabId::Images)) {
#include "tab_images.inl"
        }

        if (ShouldRenderConfigNestedTab(ConfigNestedTabId::WindowOverlays)) {
#include "tab_window_overlays.inl"
        }

        if (ShouldRenderConfigNestedTab(ConfigNestedTabId::BrowserOverlays)) {
#include "tab_browser_overlays.inl"
        }

        if (ShouldRenderConfigNestedTab(ConfigNestedTabId::Ninjabrain)) {
#include "tab_ninjabrain_overlay.inl"
        }
        
        if (ShouldRenderConfigNestedTab(ConfigNestedTabId::Keystrokes)) {
#include "tab_keystrokes.inl"
        }

        ImGui::EndTabBar();
    }

    ImGui::EndTabItem();
}