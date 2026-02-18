#pragma once

#include <GL/glew.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

// Need gui.h for Color and MirrorCaptureConfig used as value types in ThreadedMirrorConfig
#include "gui.h"

// Forward declarations
struct MirrorInstance;

// Thread runs independently, capturing game content to back-buffer FBOs

// Is the mirror capture thread currently running
extern std::atomic<bool> g_mirrorCaptureRunning;

// Safe capture window flag: true during SwapBuffers hook execution (between entry and owglSwapBuffers call)
// Capture thread only captures while this is true - if it becomes false, capture is aborted
extern std::atomic<bool> g_safeToCapture;

// Number of active mirrors currently configured for capture in the current mode.
// Updated by UpdateMirrorCaptureConfigs() (logic thread) and read by SwapBuffers hook to
// avoid doing expensive full-frame GPU copies when nothing consumes them.
extern std::atomic<int> g_activeMirrorCaptureCount;

// Named ThreadedMirrorConfig to avoid conflict with MirrorCaptureConfig in gui.h
struct ThreadedMirrorConfig {
    std::string name;
    int captureWidth = 0;
    int captureHeight = 0;

    // Border configuration
    MirrorBorderType borderType = MirrorBorderType::Dynamic;
    int dynamicBorderThickness = 0; // For dynamic border (shader-based)
    // Static border settings (rendered if staticBorderThickness > 0)
    MirrorBorderShape staticBorderShape = MirrorBorderShape::Rectangle;
    Color staticBorderColor = { 1.0f, 1.0f, 1.0f };
    int staticBorderThickness = 2;
    int staticBorderRadius = 0;
    int staticBorderOffsetX = 0;
    int staticBorderOffsetY = 0;
    int staticBorderWidth = 0;  // 0 = use mirror width
    int staticBorderHeight = 0; // 0 = use mirror height

    int fps = 0;
    bool rawOutput = false;
    bool colorPassthrough = false;   // If true, output original pixel color instead of Output Color when matching
    std::vector<Color> targetColors; // Multiple target colors - any matching pixel is shown
    Color outputColor;
    Color borderColor; // Border color for dynamic render shader
    float colorSensitivity = 0.0f;
    std::vector<MirrorCaptureConfig> input; // Uses MirrorCaptureConfig from gui.h
    std::chrono::steady_clock::time_point lastCaptureTime;

    // Output positioning config (for pre-computing render cache)
    float outputScale = 1.0f;
    bool outputSeparateScale = false; // When true, use outputScaleX/Y instead of outputScale
    float outputScaleX = 1.0f;        // X-axis scale
    float outputScaleY = 1.0f;        // Y-axis scale
    int outputX = 0, outputY = 0;
    std::string outputRelativeTo;
};

// External access to threaded mirror configs (protected by mutex)
extern std::vector<ThreadedMirrorConfig> g_threadedMirrorConfigs;
extern std::mutex g_threadedMirrorConfigMutex;

// Game state for capture thread (main thread writes, capture thread reads)
extern std::atomic<int> g_captureGameW;
extern std::atomic<int> g_captureGameH;
extern std::atomic<GLuint> g_captureGameTexture;

// Screen/viewport geometry for render cache computation (main thread writes, capture thread reads)
extern std::atomic<int> g_captureScreenW;
extern std::atomic<int> g_captureScreenH;
extern std::atomic<int> g_captureFinalX;
extern std::atomic<int> g_captureFinalY;
extern std::atomic<int> g_captureFinalW;
extern std::atomic<int> g_captureFinalH;

// Frame capture notification - sent from SwapBuffers to mirror thread
// SwapBuffers only creates fence - mirror thread does the actual GPU blit
struct FrameCaptureNotification {
    GLuint gameTextureId; // Game texture to copy from (mirror thread does the blit)
    GLsync fence;         // Fence to wait on before reading game texture
    int width;
    int height;
    int textureIndex; // Which copy texture (0 or 1) this notification refers to - fixes race condition
};

// Lock-free SPSC (Single Producer Single Consumer) ring buffer for capture notifications
// This allows the render thread to push without any locking
constexpr int CAPTURE_QUEUE_SIZE = 2; // Only need 1 pending frame (size must be power of 2)
extern FrameCaptureNotification g_captureQueue[CAPTURE_QUEUE_SIZE];
extern std::atomic<int> g_captureQueueHead; // Write index (render thread only)
extern std::atomic<int> g_captureQueueTail; // Read index (capture thread only)

// Lock-free queue operations (inline for performance)
inline bool CaptureQueuePush(const FrameCaptureNotification& notif) {
    int head = g_captureQueueHead.load(std::memory_order_relaxed);
    int nextHead = (head + 1) % CAPTURE_QUEUE_SIZE;

    // Check if queue is full (would overwrite unread data)
    if (nextHead == g_captureQueueTail.load(std::memory_order_acquire)) {
        return false; // Queue full, drop notification
    }

    g_captureQueue[head] = notif;
    g_captureQueueHead.store(nextHead, std::memory_order_release);
    return true;
}

inline bool CaptureQueuePop(FrameCaptureNotification& notif) {
    int tail = g_captureQueueTail.load(std::memory_order_relaxed);

    // Check if queue is empty
    if (tail == g_captureQueueHead.load(std::memory_order_acquire)) {
        return false; // Queue empty
    }

    notif = g_captureQueue[tail];
    g_captureQueueTail.store((tail + 1) % CAPTURE_QUEUE_SIZE, std::memory_order_release);
    return true;
}

// Start the mirror capture thread (call from main thread after GPU init)
// MUST be called from main thread where game context is current
void StartMirrorCaptureThread(void* gameGLContext);

// Stop the mirror capture thread
void StopMirrorCaptureThread();

// Swap buffers for all mirrors that have new captures ready
// Call this from main render thread each frame
void SwapMirrorBuffers();

// Update capture configs from main thread (call when active mirrors change)
void UpdateMirrorCaptureConfigs(const std::vector<MirrorConfig>& activeMirrors);

// Update FPS for a specific mirror (call from GUI when FPS spinner changes)
void UpdateMirrorFPS(const std::string& mirrorName, int fps);

// Update output position for a specific mirror (call from GUI when position changes)
void UpdateMirrorOutputPosition(const std::string& mirrorName, int x, int y, float scale, bool separateScale, float scaleX, float scaleY,
                                const std::string& relativeTo);

// Update output position for all mirrors in a group (call from GUI when group settings change)
void UpdateMirrorGroupOutputPosition(const std::vector<std::string>& mirrorIds, int x, int y, float scale, bool separateScale, float scaleX,
                                     float scaleY, const std::string& relativeTo);

// Update input/capture regions for a specific mirror (call from GUI when input zones change)
void UpdateMirrorInputRegions(const std::string& mirrorName, const std::vector<MirrorCaptureConfig>& inputRegions);

// Update capture-related settings for a specific mirror (call from GUI when capture settings change)
void UpdateMirrorCaptureSettings(const std::string& mirrorName, int captureWidth, int captureHeight, const MirrorBorderConfig& border,
                                 const MirrorColors& colors, float colorSensitivity, bool rawOutput, bool colorPassthrough);

// Global mirror match colorspace (applies to all mirrors)
void SetGlobalMirrorGammaMode(MirrorGammaMode mode);
MirrorGammaMode GetGlobalMirrorGammaMode();

void InitCaptureTexture(int width, int height);
void CleanupCaptureTexture();

// Start async GPU blit to copy game texture (called from SwapBuffers, non-blocking)
// The GPU executes the blit in background. Consumers call GetGameCopyTexture/Fence to access.
void SubmitFrameCapture(GLuint gameTexture, int width, int height);

// These provide access to the copied game texture for render_thread/OBS to use
// The copy is made by mirror thread (deferred from SwapBuffers)
GLuint GetGameCopyTexture(); // Returns texture ID of the most recent copy (0 if none ready)

// --- Ready Frame Accessors (for OBS render thread) ---
// These return GUARANTEED COMPLETE frames - GPU fence has signaled, safe to read without waiting
// Updated by mirror thread after fence signals, read by OBS without any fence wait
GLuint GetReadyGameTexture(); // Returns texture that is guaranteed complete (0 if none ready)
int GetReadyGameWidth();      // Width of ready frame content
int GetReadyGameHeight();     // Height of ready frame content

// --- Fallback Frame Accessors (for render_thread when ready frame not available) ---
// These return the last copy texture info, but require fence wait before use
// Used as fallback when GetReadyGameTexture() returns 0 due to timing
GLuint GetFallbackGameTexture(); // Returns texture from g_lastCopyReadIndex (0 if none)
int GetFallbackGameWidth();      // Width of fallback frame
int GetFallbackGameHeight();     // Height of fallback frame
GLsync GetFallbackCopyFence();   // Fence to wait on before using fallback texture

// Returns the texture NOT currently being written to - always safe to read (may be 1 frame old)
// No fence wait needed - this is a simple and reliable fallback
GLuint GetSafeReadTexture();

// Note: OBS capture is now handled by obs_thread.h/cpp via glBlitFramebuffer hook
