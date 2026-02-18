#pragma once

#include <imgui.h>
#include <chrono>
#include <vector>

// ImGui Draw Data Cache for optimized rendering
// Updates ImGui logic at ~30 FPS but renders cached draw data every frame
class ImGuiDrawDataCache {
public:
    ImGuiDrawDataCache();
    ~ImGuiDrawDataCache();

    // Clone the current ImDrawData from ImGui::GetDrawData()
    void CacheFromCurrent();

    // Check if the cache is valid and can be rendered
    bool IsValid() const { return m_valid; }

    // Get cached draw data for rendering (returns nullptr if invalid)
    ImDrawData* GetCachedDrawData();

    // Clear the cache and free resources
    void Clear();

    // Check if enough time has passed to warrant a new update (~30 FPS = 33ms)
    bool ShouldUpdate() const;

    // Mark that an update was performed (resets the timer)
    void MarkUpdated();

    // Force the next ShouldUpdate() to return true
    void Invalidate();

private:
    // Deep copy a single draw list
    ImDrawList* CloneDrawList(const ImDrawList* src);

    bool m_valid = false;
    bool m_forceUpdate = true; // Start with forced update
    std::chrono::steady_clock::time_point m_lastUpdateTime;
    
    // Cached draw data
    ImDrawData m_cachedDrawData;
    
    // Owned draw lists (deep copies)
    std::vector<ImDrawList*> m_ownedDrawLists;
    
    // Update interval in milliseconds (33ms = ~30 FPS)
    static constexpr int UPDATE_INTERVAL_MS = 33;
};

// Global cache instance
extern ImGuiDrawDataCache g_imguiCache;

// Convenience functions
inline bool ShouldUpdateImGui() { return g_imguiCache.ShouldUpdate(); }
inline void CacheImGuiDrawData() { g_imguiCache.CacheFromCurrent(); g_imguiCache.MarkUpdated(); }
inline void RenderCachedImGuiDrawData(void (*renderFunc)(ImDrawData*)) {
    if (g_imguiCache.IsValid()) {
        renderFunc(g_imguiCache.GetCachedDrawData());
    }
}
inline void InvalidateImGuiCache() { g_imguiCache.Invalidate(); }
