#pragma once

#include <GL/glew.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

// Forward declarations - render thread looks up configs directly from g_config
struct ModeConfig;
struct MirrorConfig;
struct ImageConfig;
struct GLState;
struct GameViewportGeometry;

constexpr int RENDER_THREAD_FBO_COUNT = 3; // Triple buffering

// Lightweight struct - render thread looks up active elements from g_config directly
// This avoids expensive vector copies on every frame
struct FrameRenderRequest {
    // Frame identification
    uint64_t frameNumber = 0;

    // Screen dimensions
    int fullW = 0;
    int fullH = 0;

    // Game viewport geometry (where game rendered)
    int gameW = 0;
    int gameH = 0;
    int finalX = 0;
    int finalY = 0;
    int finalW = 0;
    int finalH = 0;

    // Game texture (shared between contexts)
    // If UINT_MAX, mirrors should sample from backbuffer instead
    GLuint gameTextureId = 0;

    // Mode ID - render thread looks up ModeConfig and collects active elements
    std::string modeId;

    // Transition state
    bool isAnimating = false;
    float overlayOpacity = 1.0f;

    // OBS detection
    bool obsDetected = false;
    bool excludeOnlyOnMyScreen = false;
    bool skipAnimation = false;
    bool relativeStretching = false; // When true, viewport-relative overlays scale with viewport during animation

    // Transition interpolation - for smooth overlay animation from A to B
    float transitionProgress = 1.0f; // 0.0 = at FROM position, 1.0 = at TO position
    int fromX = 0;                   // FROM viewport position (where it started)
    int fromY = 0;
    int fromW = 0;
    int fromH = 0;
    int toX = 0; // TO viewport position (where it will end)
    int toY = 0;
    int toW = 0;
    int toH = 0;

    // If true, this request is for OBS animated frame (WITH animation)
    // Render thread will render game at animatedX/Y/W/H position and include overlays
    bool isObsPass = false;

    // Animated viewport position (for OBS pass - where the game should appear WITH animation)
    int animatedX = 0;
    int animatedY = 0;
    int animatedW = 0;
    int animatedH = 0;

    // Mode background color (for filling areas outside game viewport)
    float bgR = 0.0f;
    float bgG = 0.0f;
    float bgB = 0.0f;

    // --- Background/Border Rendering (for non-OBS async overlay) ---
    // These allow background and border to be rendered on the render thread
    bool backgroundIsImage = false; // true = use image background, false = use color
    // bgR/bgG/bgB above already hold the background color

    // Game border config
    bool borderEnabled = false;
    float borderR = 1.0f;
    float borderG = 1.0f;
    float borderB = 1.0f;
    int borderWidth = 0;
    int borderRadius = 0;

    // Transition: from-mode background/border (for transitioning TO Fullscreen)
    bool transitioningToFullscreen = false;
    bool fromBackgroundIsImage = false;
    float fromBgR = 0.0f;
    float fromBgG = 0.0f;
    float fromBgB = 0.0f;
    bool fromBorderEnabled = false;
    float fromBorderR = 1.0f;
    float fromBorderG = 1.0f;
    float fromBorderB = 1.0f;
    int fromBorderWidth = 0;
    int fromBorderRadius = 0;
    std::string fromModeId; // For looking up from-mode's background texture

    // Slide mirrors animation - per-mode setting for mirror slide in/out
    bool fromSlideMirrorsIn = false;  // FROM mode's slideMirrorsIn setting
    bool toSlideMirrorsIn = false;    // TO mode's slideMirrorsIn setting
    float mirrorSlideProgress = 1.0f; // 0.0 = at start, 1.0 = complete (independent of overlay transition type)

    // Stencil letterbox extend (for Bounce animation sub-pixel coverage)
    int letterboxExtendX = 0;
    int letterboxExtendY = 0;

    // GPU fence for game texture synchronization (OBS pass)
    // The main thread creates this fence after the game finishes rendering.
    // The render thread waits on this to ensure game is done before reading.
    // This is separate from the mirror thread's blit fence.
    GLsync gameTextureFence = nullptr;

    // --- ImGui Rendering State ---
    // These control whether to render GUI elements on the render thread
    bool shouldRenderGui = false;
    bool showPerformanceOverlay = false;
    bool showProfiler = false;
    bool showEyeZoom = false;
    float eyeZoomFadeOpacity = 1.0f;
    int eyeZoomAnimatedViewportX = -1;       // Animated viewport X for EyeZoom positioning (-1 = use static)
    bool isTransitioningFromEyeZoom = false; // True when transitioning FROM EyeZoom (use snapshot)
    GLuint eyeZoomSnapshotTexture = 0;       // Snapshot texture to use when transitioning from EyeZoom
    int eyeZoomSnapshotWidth = 0;            // Width of the snapshot texture
    int eyeZoomSnapshotHeight = 0;           // Height of the snapshot texture
    bool showTextureGrid = false;
    int textureGridModeWidth = 0;
    int textureGridModeHeight = 0;

    // Welcome toast (shown briefly after DLL injection) - bypasses isRawWindowedMode
    bool showWelcomeToast = false;
    bool welcomeToastIsFullscreen = false;

    // Windowed mode for pre-1.13 virtual camera centering
    bool isWindowed = false;
    int windowW = 0;
    int windowH = 0;
    bool isPre113Windowed = false;  // true if isWindowed && g_gameVersion < 1.13.0
    bool isRawWindowedMode = false; // true = just blit raw game content + cursor, skip all overlays
};

extern std::atomic<bool> g_renderThreadRunning;
extern std::atomic<uint64_t> g_renderFrameNumber;
extern std::atomic<bool> g_eyeZoomFontNeedsReload;

// Start the render thread (call from main thread after GL context is available)
// gameGLContext: the game's OpenGL context handle for sharing
void StartRenderThread(void* gameGLContext);

// Stop the render thread (call before DLL unload)
void StopRenderThread();

// Submit a frame for async rendering
// Returns immediately after queueing the request
void SubmitFrameForRendering(const FrameRenderRequest& request);

// Wait for the render thread to complete a frame
// Returns the index of the completed FBO (0, 1, or 2 for triple buffering)
// If timeout occurs, returns -1
int WaitForRenderComplete(int timeoutMs = 16);

// Get the FBO that contains the completed render
// Call this after WaitForRenderComplete returns successfully
// Get the texture from the completed render FBO
// Returns 0 if no texture is ready
GLuint GetCompletedRenderTexture();

// Get the fence associated with the completed render texture
// The main thread should wait on this fence before reading the texture
// Returns nullptr if no fence is available
// IMPORTANT: The caller must NOT delete this fence - it's managed by the render thread
GLsync GetCompletedRenderFence();

// === Cross-context producer/consumer safety ===
// The render thread renders into a ring of FBO textures. The main thread samples one of those
// textures during the final composite. If the render thread laps the main thread (e.g. scheduler
// jitter / very high FPS), it can start writing into a texture that the main thread is still
// sampling, producing a rare 1-frame "missing overlays" flicker.
//
// To prevent that, the main thread can publish a GLsync fence after it finishes sampling a
// completed texture; the render thread waits on that fence before reusing the corresponding FBO.
struct CompletedRenderFrame {
    GLuint texture = 0;
    GLsync fence = nullptr; // Fence signaling render-thread completion of this texture
    int fboIndex = -1;      // Which internal render-thread FBO owns `texture` (-1 if unknown)
};

// Returns the last completed render frame in a self-consistent way.
// (Texture is mapped to an internal FBO index by GL name.)
CompletedRenderFrame GetCompletedRenderFrame();

// Main thread: publish a fence that signals when it has finished sampling the completed texture.
// Render thread: waits on this before reusing that FBO as a render target.
void SubmitRenderFBOConsumerFence(int fboIndex, GLsync consumerFence);

// Get the texture from the completed OBS render
// Returns 0 if no texture is ready
GLuint GetCompletedObsTexture();

// Get the fence associated with the completed OBS render texture
// The caller should wait on this fence before reading the texture
// Returns nullptr if no fence is available
// IMPORTANT: The caller must NOT delete this fence - it's managed by the render thread
GLsync GetCompletedObsFence();

// --- Helper for building OBS frame requests ---
// Provides shared OBS context data to avoid repetition in dllmain.cpp
struct ObsFrameContext {
    int fullW, fullH;
    int gameW, gameH;
    GLuint gameTextureId;
    std::string modeId;
    bool relativeStretching;
    float bgR, bgG, bgB;

    // Windowed mode support (for virtual camera centering)
    bool isWindowed = false;        // True when game is not fullscreen
    bool isRawWindowedMode = false; // True = just blit raw game content + cursor, skip all overlays/backgrounds
    int windowW = 0;                // Actual game window width
    int windowH = 0;                // Actual game window height

    // GUI state
    bool shouldRenderGui;
    bool showPerformanceOverlay;
    bool showProfiler;
    bool isEyeZoom;
    bool isTransitioningFromEyeZoom;
    int eyeZoomAnimatedViewportX;
    GLuint eyeZoomSnapshotTexture;
    int eyeZoomSnapshotWidth;
    int eyeZoomSnapshotHeight;
    bool showTextureGrid;

    // Welcome toast (shown briefly after DLL injection)
    bool showWelcomeToast = false;
    bool welcomeToastIsFullscreen = false;
};

// Lightweight OBS submission struct - avoids building full FrameRenderRequest on main thread
// The render thread will call BuildObsFrameRequest with this context
struct ObsFrameSubmission {
    ObsFrameContext context;
    GLsync gameTextureFence = nullptr;
    bool isDualRenderingPath = false;
};

// Lightweight OBS submission - defers BuildObsFrameRequest to render thread
// This is more efficient as it avoids lock-free reads and struct building on main thread
void SubmitObsFrameContext(const ObsFrameSubmission& submission);

// Builds a FrameRenderRequest for OBS capture with proper transition state handling
// This consolidates the duplicated OBS frame building logic from dllmain.cpp
FrameRenderRequest BuildObsFrameRequest(const ObsFrameContext& ctx, bool isDualRenderingPath);
