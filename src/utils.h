#pragma once

#include <GL/glew.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <windows.h>

#include "gui.h"

// Config access: Reader threads use GetConfigSnapshot() for safe, lock-free access.
// g_config is the mutable draft, only touched by the GUI/main thread.
// After any mutation, PublishConfigSnapshot() makes it available to readers.

struct MirrorInstance {
    GLuint fbo = 0;
    GLuint fboTexture = 0; // Front buffer (read by main thread for display)
    int fbo_w = 0;
    int fbo_h = 0;
    std::chrono::steady_clock::time_point lastUpdateTime{};
    int forceUpdateFrames = 0;              // Counter to bypass FPS throttling for multiple frames
    std::vector<unsigned char> pixelBuffer; // For EyeZoom mode: stores captured pixels from framebuffer
    GLuint tempCaptureTexture = 0;          // For EyeZoom mode: temporary texture for raw capture before filtering

    // Double-buffering for threaded capture
    GLuint fboBack = 0;                      // Back FBO (written by capture thread)
    GLuint fboTextureBack = 0;               // Back texture (written by capture thread)
    std::atomic<bool> captureReady{ false }; // True when back buffer has new frame ready to swap
    bool hasValidContent = false;            // True after first successful buffer swap (front buffer has renderable content)

    // Track how the FBO was captured to ensure render uses matching shader
    // This prevents race conditions when rawOutput setting changes mid-frame
    bool capturedAsRawOutput = false;     // How front buffer was captured
    bool capturedAsRawOutputBack = false; // How back buffer was captured (swapped with capturedAsRawOutput)

    // Desired rawOutput state - written by main thread, read by capture thread
    // This ensures capture thread always uses the latest value
    std::atomic<bool> desiredRawOutput{ false };

    // Final rendered content (with borders/shaders applied) - ready for screen blit
    // The capture thread renders here, the render thread just blits this
    GLuint finalFbo = 0;                    // Front FBO with screen-ready content
    GLuint finalTexture = 0;                // Front texture with screen-ready content
    GLuint finalFboBack = 0;                // Back FBO (capture thread writes)
    GLuint finalTextureBack = 0;            // Back texture (capture thread writes)
    int final_w = 0, final_h = 0;           // Dimensions of FRONT final FBO
    int final_w_back = 0, final_h_back = 0; // Dimensions of BACK final FBO (may differ during scale changes)

    // Track whether the mirror has actual filtered content (not empty)
    // Used by static borders to avoid rendering when mirror has no matching pixels
    // Both default to true so that borders render from the very first frame.
    // The async PBO content detection will update to false once a readback proves no content.
    // This avoids a 1-frame flicker where the border disappears then reappears.
    bool hasFrameContent = true;      // Front buffer has content (read by render thread)
    bool hasFrameContentBack = true;  // Back buffer has content (written by capture thread)

    // Cross-context GPU synchronization fences
    // These ensure the render thread waits for capture thread's GPU work to complete
    // before reading from the texture. glFinish() only syncs within one context;
    // fences work across shared contexts via glWaitSync.
    GLsync gpuFence = nullptr;     // Front buffer fence (render thread waits on this)
    GLsync gpuFenceBack = nullptr; // Back buffer fence (capture thread sets this)

    // Cached render state - computed by capture thread, used by render thread
    // This minimizes per-frame calculations on the render thread
    struct CachedMirrorRenderState {
        // Inputs (for invalidation detection)
        float outputScale = -1.0f;
        bool outputSeparateScale = false;
        float outputScaleX = 1.0f;
        float outputScaleY = 1.0f;
        int outputX = 0, outputY = 0;
        std::string outputRelativeTo;
        int gameW = 0, gameH = 0;
        int screenW = 0, screenH = 0;
        int finalX = 0, finalY = 0, finalW = 0, finalH = 0;
        int fbo_w = 0, fbo_h = 0;

        // Pre-computed GPU data (ready to upload to VBO)
        float vertices[24];     // 6 vertices * 4 floats (x, y, u, v)
        int outW = 0, outH = 0; // Output dimensions on screen

        // Mirror screen position (for static border rendering)
        int mirrorScreenX = 0, mirrorScreenY = 0;
        int mirrorScreenW = 0, mirrorScreenH = 0;

        bool isValid = false;
    };
    CachedMirrorRenderState cachedRenderState;
    CachedMirrorRenderState cachedRenderStateBack; // Back buffer cache (swapped to front)

    // Default constructor
    MirrorInstance() = default;

    MirrorInstance(const MirrorInstance& other)
        : fbo(other.fbo),
          fboTexture(other.fboTexture),
          fbo_w(other.fbo_w),
          fbo_h(other.fbo_h),
          lastUpdateTime(other.lastUpdateTime),
          forceUpdateFrames(other.forceUpdateFrames),
          pixelBuffer(other.pixelBuffer),
          tempCaptureTexture(other.tempCaptureTexture),
          fboBack(other.fboBack),
          fboTextureBack(other.fboTextureBack),
          captureReady(other.captureReady.load(std::memory_order_relaxed)),
          hasValidContent(other.hasValidContent),
          capturedAsRawOutput(other.capturedAsRawOutput),
          capturedAsRawOutputBack(other.capturedAsRawOutputBack),
          desiredRawOutput(other.desiredRawOutput.load(std::memory_order_relaxed)),
          finalFbo(other.finalFbo),
          finalTexture(other.finalTexture),
          finalFboBack(other.finalFboBack),
          finalTextureBack(other.finalTextureBack),
          final_w(other.final_w),
          final_h(other.final_h),
          final_w_back(other.final_w_back),
          final_h_back(other.final_h_back),
          hasFrameContent(other.hasFrameContent),
          hasFrameContentBack(other.hasFrameContentBack),
          gpuFence(nullptr),
          gpuFenceBack(nullptr), // Fences are GPU resources, don't copy
          cachedRenderState(other.cachedRenderState),
          cachedRenderStateBack(other.cachedRenderStateBack) {}

    // Move constructor
    MirrorInstance(MirrorInstance&& other) noexcept
        : fbo(other.fbo),
          fboTexture(other.fboTexture),
          fbo_w(other.fbo_w),
          fbo_h(other.fbo_h),
          lastUpdateTime(other.lastUpdateTime),
          forceUpdateFrames(other.forceUpdateFrames),
          pixelBuffer(std::move(other.pixelBuffer)),
          tempCaptureTexture(other.tempCaptureTexture),
          fboBack(other.fboBack),
          fboTextureBack(other.fboTextureBack),
          captureReady(other.captureReady.load(std::memory_order_relaxed)),
          hasValidContent(other.hasValidContent),
          capturedAsRawOutput(other.capturedAsRawOutput),
          capturedAsRawOutputBack(other.capturedAsRawOutputBack),
          desiredRawOutput(other.desiredRawOutput.load(std::memory_order_relaxed)),
          finalFbo(other.finalFbo),
          finalTexture(other.finalTexture),
          finalFboBack(other.finalFboBack),
          finalTextureBack(other.finalTextureBack),
          final_w(other.final_w),
          final_h(other.final_h),
          final_w_back(other.final_w_back),
          final_h_back(other.final_h_back),
          hasFrameContent(other.hasFrameContent),
          hasFrameContentBack(other.hasFrameContentBack),
          gpuFence(other.gpuFence),
          gpuFenceBack(other.gpuFenceBack),
          cachedRenderState(std::move(other.cachedRenderState)),
          cachedRenderStateBack(std::move(other.cachedRenderStateBack)) {
        // Transfer fence ownership
        other.gpuFence = nullptr;
        other.gpuFenceBack = nullptr;
    }

    // Copy assignment operator
    MirrorInstance& operator=(const MirrorInstance& other) {
        if (this != &other) {
            fbo = other.fbo;
            fboTexture = other.fboTexture;
            fbo_w = other.fbo_w;
            fbo_h = other.fbo_h;
            lastUpdateTime = other.lastUpdateTime;
            forceUpdateFrames = other.forceUpdateFrames;
            pixelBuffer = other.pixelBuffer;
            tempCaptureTexture = other.tempCaptureTexture;
            fboBack = other.fboBack;
            fboTextureBack = other.fboTextureBack;
            captureReady.store(other.captureReady.load(std::memory_order_relaxed), std::memory_order_relaxed);
            hasValidContent = other.hasValidContent;
            capturedAsRawOutput = other.capturedAsRawOutput;
            capturedAsRawOutputBack = other.capturedAsRawOutputBack;
            desiredRawOutput.store(other.desiredRawOutput.load(std::memory_order_relaxed), std::memory_order_relaxed);
            finalFbo = other.finalFbo;
            finalTexture = other.finalTexture;
            finalFboBack = other.finalFboBack;
            finalTextureBack = other.finalTextureBack;
            final_w = other.final_w;
            final_h = other.final_h;
            final_w_back = other.final_w_back;
            final_h_back = other.final_h_back;
            hasFrameContent = other.hasFrameContent;
            hasFrameContentBack = other.hasFrameContentBack;
            cachedRenderState = other.cachedRenderState;
            cachedRenderStateBack = other.cachedRenderStateBack;
            // Fences are GPU resources - don't copy, just clear ours
            gpuFence = nullptr;
            gpuFenceBack = nullptr;
        }
        return *this;
    }

    // Move assignment operator
    MirrorInstance& operator=(MirrorInstance&& other) noexcept {
        if (this != &other) {
            fbo = other.fbo;
            fboTexture = other.fboTexture;
            fbo_w = other.fbo_w;
            fbo_h = other.fbo_h;
            lastUpdateTime = other.lastUpdateTime;
            forceUpdateFrames = other.forceUpdateFrames;
            pixelBuffer = std::move(other.pixelBuffer);
            tempCaptureTexture = other.tempCaptureTexture;
            fboBack = other.fboBack;
            fboTextureBack = other.fboTextureBack;
            captureReady.store(other.captureReady.load(std::memory_order_relaxed), std::memory_order_relaxed);
            hasValidContent = other.hasValidContent;
            capturedAsRawOutput = other.capturedAsRawOutput;
            capturedAsRawOutputBack = other.capturedAsRawOutputBack;
            desiredRawOutput.store(other.desiredRawOutput.load(std::memory_order_relaxed), std::memory_order_relaxed);
            finalFbo = other.finalFbo;
            finalTexture = other.finalTexture;
            finalFboBack = other.finalFboBack;
            finalTextureBack = other.finalTextureBack;
            final_w = other.final_w;
            final_h = other.final_h;
            final_w_back = other.final_w_back;
            final_h_back = other.final_h_back;
            hasFrameContent = other.hasFrameContent;
            hasFrameContentBack = other.hasFrameContentBack;
            cachedRenderState = std::move(other.cachedRenderState);
            cachedRenderStateBack = std::move(other.cachedRenderStateBack);
            // Transfer fence ownership
            gpuFence = other.gpuFence;
            gpuFenceBack = other.gpuFenceBack;
            other.gpuFence = nullptr;
            other.gpuFenceBack = nullptr;
        }
        return *this;
    }
};

struct UserImageInstance {
    GLuint textureId = 0;
    int width = 0;
    int height = 0;
    bool isFullyTransparent = false; // True if all pixels have alpha = 0

    // Render-thread-only texture sampling state cache.
    // Avoids redundant glTexParameteri calls every frame.
    bool filterInitialized = false;
    bool lastPixelatedScaling = false;

    // Animation data (for animated GIFs)
    bool isAnimated = false;
    std::vector<GLuint> frameTextures; // All frame textures for animation
    std::vector<int> frameDelays;      // Delay in ms between each frame
    size_t currentFrame = 0;
    std::chrono::steady_clock::time_point lastFrameTime;

    // Cached rendering data (invalidated when config changes)
    struct CachedImageRenderState {
        // Config hash to detect changes
        int crop_left = -1;
        int crop_right = -1;
        int crop_top = -1;
        int crop_bottom = -1;
        float scale = -1.0f;
        int x = 0;
        int y = 0;
        std::string relativeTo;
        int screenWidth = 0;
        int screenHeight = 0;

        // Cached computed values
        int displayW = 0;
        int displayH = 0;
        float tx1 = 0, ty1 = 0, tx2 = 0, ty2 = 0;
        float nx1 = 0, ny1 = 0, nx2 = 0, ny2 = 0;

        bool isValid = false;
    } cachedRenderState;
};

extern std::ofstream logFile;
extern std::mutex g_logFileMutex;
extern std::wstring g_toolscreenPath;
extern std::wstring g_modeFilePath;
extern std::wstring g_stateFilePath;
extern std::atomic<bool> g_isStateOutputAvailable;
extern std::atomic<bool> g_stopMonitoring;
extern std::atomic<bool> g_stopImageMonitoring;
extern std::atomic<bool> g_isShuttingDown;
extern Config g_config;

extern std::atomic<bool> g_allImagesLoaded;
extern std::mutex g_decodedImagesMutex;
extern std::vector<DecodedImageData> g_decodedImagesQueue;
extern std::atomic<HWND> g_minecraftHwnd;
extern std::string g_gameStateBuffers[2];
extern std::atomic<int> g_currentGameStateIndex;
extern std::mutex g_hotkeyMainKeysMutex;
extern std::atomic<HCURSOR> g_specialCursorHandle;

void Log(const std::string& message);
void Log(const std::wstring& message);

// Async logging system
void StartLogThread(); // Start background log writer thread
void StopLogThread();  // Stop background log writer thread (flushes first)
void FlushLogs();      // Force flush all pending logs (for crash/shutdown)

// Category-based logging - only logs if category is enabled in debug config
// Categories: "mode_switch", "animation", "hotkey", "obs", "window_overlay",
//             "file_monitor", "image_monitor", "performance"
void LogCategory(const char* category, const std::string& message);

std::wstring Utf8ToWide(const std::string& utf8_string);
std::string WideToUtf8(const std::wstring& wstr);
std::wstring GetToolscreenPath();

// Compress a file to gzip format (.gz) using in-process DEFLATE compression.
// Returns true on success.
bool CompressFileToGzip(const std::wstring& srcPath, const std::wstring& dstPath);

inline std::string GetKeyComboString(const std::vector<DWORD>& keys) {
    std::string keyStr;
    for (size_t k = 0; k < keys.size(); ++k) {
        keyStr += VkToString(keys[k]);
        if (k < keys.size() - 1) keyStr += "+";
    }
    return keyStr;
}

struct ModeViewportInfo {
    bool valid = false;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int stretchX = 0;
    int stretchY = 0;
    int stretchWidth = 0;
    int stretchHeight = 0;
    bool stretchEnabled = false;
};

// Multi-monitor helpers
// Returns the rcMonitor rect for the monitor nearest to the given window.
// Returns false if the monitor info couldn't be queried.
bool GetMonitorRectForWindow(HWND hwnd, RECT& outRect);
// Convenience wrapper to get monitor width/height for a window.
bool GetMonitorSizeForWindow(HWND hwnd, int& outW, int& outH);

bool IsFullscreen();
// Toggle borderless-windowed fullscreen for the given window.
// When toggling on, the window is resized to the current monitor's full rect and decorations are removed.
// When toggling off, the window returns to windowed mode centered on the current monitor at half its width/height.
void ToggleBorderlessWindowedFullscreen(HWND hwnd);
bool IsCursorVisible();
void WriteCurrentModeToFile(const std::string& modeId);
bool SwitchToMode(const std::string& newModeId, const std::string& source = "", bool forceCut = false);
bool IsHardcodedMode(const std::string& modeId);
bool EqualsIgnoreCase(const std::string& a, const std::string& b);
const ModeConfig* GetMode(const std::string& id);
const ModeConfig* GetMode_Internal(const std::string& id); // Direct access version
ModeConfig* GetModeMutable(const std::string& id);         // Mutable version for modifications
MirrorConfig* GetMutableMirror(const std::string& name);

// Snapshot-safe overloads: look up in a specific config snapshot instead of g_config
const ModeConfig* GetModeFromSnapshot(const Config& config, const std::string& id);
const MirrorConfig* GetMirrorFromSnapshot(const Config& config, const std::string& name);
bool isWallTitleOrWaiting(const std::string& state);
ModeViewportInfo GetCurrentModeViewport();
ModeViewportInfo GetCurrentModeViewport_Internal(); // Lock-free implementation using double-buffered mode ID

GLuint CompileShader(GLenum type, const char* source);
GLuint CreateShaderProgram(const char* vert, const char* frag);

void LoadImageAsync(DecodedImageData::Type type, std::string id, std::string path, const std::wstring& toolscreenPath);
void LoadAllImages();

bool CheckHotkeyMatch(const std::vector<DWORD>& keys, WPARAM wParam, const std::vector<DWORD>& exclusionKeys = {},
                      bool triggerOnRelease = false);

void BackupConfigFile();

void GetRelativeCoords(const std::string& type, int relX, int relY, int w, int h, int containerW, int containerH, int& outX, int& outY);
void GetRelativeCoordsForImage(const std::string& type, int relX, int relY, int w, int h, int containerW, int containerH, int& outX,
                               int& outY);
// Overload that supports viewport-relative positioning (for anchors ending with "Viewport")
// gameX/Y/W/H = current game viewport position on screen (may be animated)
// fullW/H = screen dimensions
void GetRelativeCoordsForImageWithViewport(const std::string& type, int relX, int relY, int w, int h, int gameX, int gameY, int gameW,
                                           int gameH, int fullW, int fullH, int& outX, int& outY);

// Check if an anchor type is viewport-relative (positions relative to game viewport, animates during transitions)
inline bool IsViewportRelativeAnchor(const std::string& relativeTo) {
    // Viewport-relative anchors end with "Viewport"
    if (relativeTo.length() > 8 && relativeTo.substr(relativeTo.length() - 8) == "Viewport") { return true; }
    return false;
}
void CalculateFinalScreenPos(const MirrorConfig* conf, const MirrorInstance& inst, int gameW, int gameH, int finalX, int finalY, int finalW,
                             int finalH, int fullW, int fullH, int& outScreenX, int& outScreenY);

void ScreenshotToClipboard(int width, int height);

DWORD WINAPI FileMonitorThread(LPVOID lpParam);
DWORD WINAPI ImageMonitorThread(LPVOID lpParam);

// Exception handling
class SE_Exception : public std::exception {
  public:
    SE_Exception(unsigned int code, EXCEPTION_POINTERS* info) : m_code(code), m_info(info) {}

    unsigned int getCode() const { return m_code; }
    EXCEPTION_POINTERS* getInfo() const { return m_info; }

    const char* what() const noexcept override {
        static char buffer[128];
        snprintf(buffer, sizeof(buffer), "Structured Exception: 0x%08X", m_code);
        return buffer;
    }

  private:
    unsigned int m_code;
    EXCEPTION_POINTERS* m_info;
};

void LogException(const std::string& context, const std::exception& e);
void LogException(const std::string& context, DWORD exceptionCode, EXCEPTION_POINTERS* exceptionInfo = nullptr);
void InstallGlobalExceptionHandlers();
LONG WINAPI CustomUnhandledExceptionFilter(EXCEPTION_POINTERS* exceptionInfo);
void SEHTranslator(unsigned int code, EXCEPTION_POINTERS* info);