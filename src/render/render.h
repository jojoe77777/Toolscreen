#pragma once

#ifndef GLEW_STATIC
#define GLEW_STATIC
#endif
#include <GL/glew.h>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <string>
#include <vector>
#include <windows.h>

#include "gui/gui.h"
#include "mirror_thread.h"

#ifdef _DEBUG
#define GL_CALL(call)                                                                                                                      \
    do {                                                                                                                                   \
        call;                                                                                                                              \
        GLenum gl_err = glGetError();                                                                                                      \
        if (gl_err != GL_NO_ERROR) {                                                                                                       \
            Log("OpenGL Error 0x" + std::to_string(gl_err) + " in " + #call + " at " + __FILE__ + ":" + std::to_string(__LINE__));         \
        }                                                                                                                                  \
    } while (0)
#else
#define GL_CALL(call) call
#endif

struct MirrorInstance;
struct UserImageInstance;

// Cached mirror render data to minimize lock contention
// All border rendering is done in the mirror capture pass before final composition.
struct MirrorRenderData {
    GLuint texture;
    int tex_w, tex_h;
    const MirrorConfig* config;
    // Pre-computed from cached mirror geometry.
    float vertices[24];
    int outW, outH;
    bool cacheValid;
    // GPU fence copied from the mirror instance during collection.
    GLsync gpuFence;
    int screenX = 0;
    int screenY = 0;
    int screenW = 0;
    int screenH = 0;
    bool hasFrameContent = false;
    bool useDynamicBorderComposite = false;
};

struct FilterShaderLocs {
    GLint screenTexture, targetColor, outputColor, sensitivity, sourceRect;
};

struct RenderShaderLocs {
    GLint filterTexture, borderWidth, outputColor, borderColor, screenPixel;
};

struct RenderPassthroughShaderLocs {
    GLint filterTexture, borderWidth, borderColor, screenPixel, opacity;
};

struct BackgroundShaderLocs {
    GLint backgroundTexture;
    GLint opacity;
};

struct SolidColorShaderLocs {
    GLint color;
};

struct ImageRenderShaderLocs {
    GLint imageTexture;
    GLint enableColorKey;
    GLint numColorKeys;
    GLint colorKeys;
    GLint sensitivities;
    GLint opacity;
};

struct PassthroughShaderLocs {
    GLint screenTexture, sourceRect, opacity, sourceTexelSize, sourcePixelSize, snapToSourcePixels;
};

#define MAX_GRADIENT_STOPS 8
struct GradientShaderLocs {
    GLint numStops;
    GLint stopColors;
    GLint stopPositions;
    GLint angle;
    GLint time;
    GLint animationType;
    GLint animationSpeed;
    GLint colorFade;
};

struct GLState {
    GLint p;
    GLint t;
    GLint t0;
    GLint t1;
    GLint smp;
    GLint smp0;
    GLint smp1;
    GLint ab;
    GLint va;
    GLint fb;
    GLint read_fb, draw_fb;
    GLint at;

    GLboolean be;
    GLboolean de;
    GLboolean sc;
    GLboolean ce;
    GLboolean ste;
    GLboolean srgb_enabled;
    GLboolean depth_mask;
    GLboolean rasterizer_discard;
    GLboolean color_logic_op;
    GLboolean debug_output;

    GLint blend_src_rgb, blend_dst_rgb, blend_src_alpha, blend_dst_alpha;
    GLint blend_eq_rgb, blend_eq_alpha;
    GLint draw_buffer;
    GLint read_buffer;
    GLint pack_row_length;

    GLint vp[4];
    GLint sb[4];

    GLfloat cc[4];
    GLfloat blend_color[4];
    GLfloat lw;
    GLboolean color_mask[4];
};

extern GLuint g_filterProgram;
extern GLuint g_renderProgram;
extern GLuint g_renderPassthroughProgram;
extern GLuint g_backgroundProgram;
extern GLuint g_solidColorProgram;
extern GLuint g_imageRenderProgram;
extern GLuint g_passthroughProgram;
extern GLuint g_gradientProgram;

extern FilterShaderLocs g_filterShaderLocs;
extern RenderShaderLocs g_renderShaderLocs;
extern RenderPassthroughShaderLocs g_renderPassthroughShaderLocs;
extern BackgroundShaderLocs g_backgroundShaderLocs;
extern SolidColorShaderLocs g_solidColorShaderLocs;
extern ImageRenderShaderLocs g_imageRenderShaderLocs;
extern PassthroughShaderLocs g_passthroughShaderLocs;
extern GradientShaderLocs g_gradientShaderLocs;

// --- Global GUI State for Rendering ---
// These atomics are set by the main thread and read by render.cpp to populate FrameRenderRequest.
extern std::atomic<bool> g_shouldRenderGui;
extern std::atomic<bool> g_showPerformanceOverlay;
extern std::atomic<bool> g_showProfiler;
extern std::atomic<bool> g_showEyeZoom;
extern std::atomic<bool> g_eyeZoomFontNeedsReload;
extern std::atomic<float> g_eyeZoomFadeOpacity;
extern std::atomic<int> g_eyeZoomAnimatedViewportX;
extern std::atomic<bool> g_isTransitioningFromEyeZoom;
extern std::atomic<bool> g_showTextureGrid;
extern std::atomic<int> g_textureGridModeWidth;
extern std::atomic<int> g_textureGridModeHeight;

// Used by dllmain.cpp to pass the snapshot texture to OBS capture.
GLuint GetEyeZoomSnapshotTexture();
int GetEyeZoomSnapshotWidth();
int GetEyeZoomSnapshotHeight();

extern std::unordered_map<std::string, MirrorInstance> g_mirrorInstances;

struct BackgroundTextureInstance {
    GLuint textureId = 0;
    int width = 0;
    int height = 0;
    int textureStorageHeight = 0;
    int frameCount = 1;
    int framesPerTexture = 1;

    bool isAnimated = false;
    bool isVideo = false;
    std::vector<GLuint> frameTextures;
    std::vector<int> frameTextureHeights;
    std::vector<int> frameDelays;
    std::vector<uint64_t> frameEndTimesMs;
    uint64_t totalAnimationDurationMs = 0;
    size_t currentFrame = 0;
    std::chrono::steady_clock::time_point lastFrameTime;
};

extern std::unordered_map<std::string, BackgroundTextureInstance> g_backgroundTextures;
extern std::unordered_map<std::string, UserImageInstance> g_userImages;
extern GLuint g_vao;
extern GLuint g_vbo;
extern GLuint g_debugVAO;
extern GLuint g_debugVBO;
extern GLuint g_sceneFBO;
extern GLuint g_sceneTexture;
extern int g_sceneW;
extern int g_sceneH;

// --- Mutex Protection for GPU Resource Maps ---
// These maps are accessed from multiple threads (render + GUI).
// Lock ordering (when nesting): g_mirrorInstancesMutex > g_backgroundTexturesMutex
//   > g_userImagesMutex > g_texturesToDeleteMutex
extern std::shared_mutex g_mirrorInstancesMutex;
extern std::mutex g_userImagesMutex;
extern std::mutex g_backgroundTexturesMutex;

extern std::vector<GLuint> g_texturesToDelete;
extern std::mutex g_texturesToDeleteMutex;
extern std::atomic<bool> g_hasTexturesToDelete;
extern std::atomic<bool> g_glInitialized;
extern std::atomic<bool> g_isGameFocused;
extern GameViewportGeometry g_lastFrameGeometry;
extern std::mutex g_geometryMutex;
extern std::atomic<GLuint> g_cachedGameTextureId;

extern std::string s_hoveredImageName;
extern std::string s_draggedImageName;
extern bool s_isDragging;

enum class ResizeCorner;
extern std::string s_hoveredWindowOverlayName;
extern std::string s_draggedWindowOverlayName;
extern bool s_isWindowOverlayDragging;
extern std::string s_hoveredBrowserOverlayName;
extern std::string s_draggedBrowserOverlayName;
extern bool s_isBrowserOverlayDragging;

void InitializeShaders();
void CleanupShaders();

void DrawOverlayBorder(float nx1, float ny1, float nx2, float ny2, float borderWidth, float borderHeight, bool isDragging,
                       bool drawCorners);

void RenderGameBorder(int x, int y, int w, int h, int borderWidth, int radius, const Color& color, int fullW, int fullH);

void DiscardAllGPUImages();
void DiscardUnusedUserImageCaches();
void CleanupGPUResources();
void ProcessPendingDecodedImages();
void UploadDecodedImageToGPU(const DecodedImageData& imgData);
void UploadDecodedImageToGPU_Internal(const DecodedImageData& imgData);
void InitializeGPUResources();
void CreateMirrorGPUResources(const MirrorConfig& conf);

void AddMirrorToCurrentMode(MirrorConfig&& mirror);

// Mirror capture helpers are declared in mirror_thread.h

void InvalidateConfigLookupCaches();

void RenderMirrors(const std::vector<MirrorConfig>& activeMirrors, const GameViewportGeometry& geo, int fullW, int fullH,
                   float modeOpacity = 1.0f, bool excludeOnlyOnMyScreen = false);
void RenderImages(const std::vector<ImageConfig>& activeImages, int fullW, int fullH, float modeOpacity = 1.0f,
                  bool excludeOnlyOnMyScreen = false);
void CollectActiveElementsForMode(const Config& config, const std::string& modeId, bool onlyOnMyScreenPass, uint64_t configVersion,
                                  std::vector<MirrorConfig>& outMirrors, std::vector<ImageConfig>& outImages,
                                  std::vector<const WindowOverlayConfig*>& outWindowOverlays,
                                  std::vector<const BrowserOverlayConfig*>& outBrowserOverlays,
                                  int screenWOverride = 0, int screenHOverride = 0);
bool RenderModeOverlaysForIntegrationTest(const Config& config, const ModeConfig& modeToRender, const GLState& s, int fullW,
                                          int fullH, int gameX, int gameY, int gameW, int gameH,
                                          bool excludeOnlyOnMyScreen = false, GLuint gameTextureId = 0,
                                          bool renderGui = false);
#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
const char* GetNinjabrainOverlayRenderEligibilityFailureForIntegrationTest(const std::string& modeId,
                                                                           bool excludeOnlyOnMyScreen = false);
#endif
void RenderMode(const ModeConfig* modeToRender, const GLState& s, int current_gameW, int current_gameH, bool skipAnimation = false,
                bool excludeOnlyOnMyScreen = false);
bool RenderSameThreadObsFrame(const ModeConfig* modeToRender, const GLState& s, int current_gameW, int current_gameH,
                              bool skipAnimation = false);
void CaptureSameThreadVirtualCameraFrame();
void ResetSameThreadVirtualCameraCaptureState();
void RenderModeWithOpacity(const ModeConfig* modeToRender, const GLState& s, int current_gameW, int current_gameH, float opacity,
                           bool skipBackgroundClear = false);
void RenderDebugBordersForMirror(const MirrorConfig* conf, Color captureColor, Color outputColor, GLint originalVAO);
void RenderMirrorSelectionInfoPanel();
void RenderMirrorGroupSelectionInfoPanel();
void RenderWindowOverlaySelectionInfoPanel();
void RenderImageSelectionInfoPanel();
void handleEyeZoomMode(const GLState& s, const EyeZoomConfig& zoomConfig, int fullW, int fullH, float opacity = 1.0f,
                       int animatedViewportX = -1, bool useSnapshot = false, GLuint preferredGameTexture = 0,
                       int preferredGameW = 0, int preferredGameH = 0, const BorderConfig* cloneBorder = nullptr);
void InitializeOverlayTextFont(const std::string& fontPath, float baseFontSize, float scaleFactor);
void SetOverlayTextFontSize(int sizePixels);

bool GetImageSourceDimensions(const std::string& name, int& outW, int& outH);
bool GetCroppedImageDimensions(const ImageConfig& img, int& outCroppedW, int& outCroppedH);
void CalculateImageDimensions(const ImageConfig& img, int& outW, int& outH);

void SaveGLState(GLState* s);
void RestoreGLState(const GLState& s);

typedef void(WINAPI* GLVIEWPORTPROC)(GLint x, GLint y, GLsizei width, GLsizei height);
extern GLVIEWPORTPROC oglViewport;

struct ModeConfig;

void StartModeTransition(const std::string& fromModeId, const std::string& toModeId, int fromWidth, int fromHeight, int fromX, int fromY,
                         int toWidth, int toHeight, int toX, int toY, const ModeConfig& toMode);
void UpdateModeTransition();
bool IsModeTransitionActive();
GameTransitionType GetGameTransitionType();
OverlayTransitionType GetOverlayTransitionType();
BackgroundTransitionType GetBackgroundTransitionType();
std::string GetModeTransitionFromModeId();

// Struct to hold all transition state atomically
struct ModeTransitionState {
    bool active;
    int width;
    int height;
    int x;
    int y;
    GameTransitionType gameTransition;
    OverlayTransitionType overlayTransition;
    BackgroundTransitionType backgroundTransition;
    float progress;
    float moveProgress;
    int targetWidth;
    int targetHeight;
    int targetX;
    int targetY;
    int fromWidth;
    int fromHeight;
    int fromX;
    int fromY;
    int fromNativeWidth;
    int fromNativeHeight;
    int toNativeWidth;
    int toNativeHeight;
    std::string fromModeId;
};

// Get all transition state in a single atomic operation to avoid race conditions
ModeTransitionState GetModeTransitionState();

void RenderTextureGridOverlay(bool showTextureGrid, int modeWidth = 0, int modeHeight = 0);
void RenderCachedTextureGridLabels();

void GetAnimatedModePosition(int& outX, int& outY);




struct NinjabrainOverlayConfig;
struct ImFont;
struct ImFontAtlas;
void RenderNinjabrainOverlay(const NinjabrainOverlayConfig& nb, ImFont* font, const std::string& modeId,
                             bool renderBehindImGuiWindows = false);
ImFont* GetNinjabrainFont();
float   GetNinjabrainFontSize();
// Call this from RebuildImGuiFontAtlas (before io.Fonts->Build()) to add the NB font to the atlas.
void    LoadNinjabrainFont(ImFontAtlas* atlas, const NinjabrainOverlayConfig& overlay, float scaleFactor);

struct KeystrokesConfig;
void RenderKeystrokesOverlay(const KeystrokesConfig& ks, const std::string& modeId, bool renderBehindImGuiWindows = false);

struct KeystrokesState {
    std::atomic<bool> keysDown[256];

    KeystrokesState() {
        for (int i = 0; i < 256; i++) keysDown[i].store(false);
    }

    std::vector<double> keyClicks[256];
    std::mutex clicksMutex;
};

extern KeystrokesState g_keystrokesState;
