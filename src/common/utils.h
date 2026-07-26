#pragma once

#ifndef GLEW_STATIC
#define GLEW_STATIC
#endif
#include <GL/glew.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <windows.h>

#include "gui/gui.h"
#include "features/game_state_source.h"

// Config access: Reader threads use GetConfigSnapshot() for safe, lock-free access.
// g_config is the mutable draft, only touched by the GUI/main thread.

struct MirrorInstance {
    GLuint fbo = 0;
    GLuint fboTexture = 0; // Front buffer (read by main thread for display)
    int fbo_w = 0;
    int fbo_h = 0;
    std::chrono::steady_clock::time_point lastUpdateTime{};
    int forceUpdateFrames = 0;
    std::vector<unsigned char> pixelBuffer;
    GLuint tempCaptureTexture = 0;
    int tempCaptureTextureW = 0;
    int tempCaptureTextureH = 0;

    // Double-buffering for threaded capture
    GLuint fboBack = 0;                      // Back FBO (written by capture thread)
    GLuint fboTextureBack = 0;               // Back texture (written by capture thread)
    std::atomic<bool> captureReady{ false };
    bool hasValidContent = false;

    // This prevents race conditions when rawOutput setting changes mid-frame
    bool capturedAsRawOutput = false;
    bool capturedAsRawOutputBack = false;

    // Desired rawOutput state - written by main thread, read by capture thread
    // This ensures capture thread always uses the latest value
    std::atomic<bool> desiredRawOutput{ false };

    // The capture thread renders here, the render thread just blits this
    GLuint finalFbo = 0;
    GLuint finalTexture = 0;
    GLuint finalFboBack = 0;                // Back FBO (capture thread writes)
    GLuint finalTextureBack = 0;            // Back texture (capture thread writes)
    int final_w = 0, final_h = 0;
    int final_w_back = 0, final_h_back = 0;

    bool hasFrameContent = false;     // Front buffer has content (read by render thread)
    bool hasFrameContentBack = false; // Back buffer has content (written by capture thread)

    // Cross-context GPU synchronization fences
    // These ensure the render thread waits for capture thread's GPU work to complete
    // fences work across shared contexts via glWaitSync.
    GLsync gpuFence = nullptr;     // Front buffer fence (render thread waits on this)
    GLsync gpuFenceBack = nullptr; // Back buffer fence (capture thread sets this)

    // Cached render state - computed by capture thread, used by render thread
    // This minimizes per-frame calculations on the render thread
    struct CachedMirrorRenderState {
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

        float vertices[24];
        int outW = 0, outH = 0;

        int mirrorScreenX = 0, mirrorScreenY = 0;
        int mirrorScreenW = 0, mirrorScreenH = 0;

        bool isValid = false;
    };
    CachedMirrorRenderState cachedRenderState;
    CachedMirrorRenderState cachedRenderStateBack;

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
          tempCaptureTextureW(other.tempCaptureTextureW),
          tempCaptureTextureH(other.tempCaptureTextureH),
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

    MirrorInstance(MirrorInstance&& other) noexcept
        : fbo(other.fbo),
          fboTexture(other.fboTexture),
          fbo_w(other.fbo_w),
          fbo_h(other.fbo_h),
          lastUpdateTime(other.lastUpdateTime),
          forceUpdateFrames(other.forceUpdateFrames),
          pixelBuffer(std::move(other.pixelBuffer)),
          tempCaptureTexture(other.tempCaptureTexture),
          tempCaptureTextureW(other.tempCaptureTextureW),
          tempCaptureTextureH(other.tempCaptureTextureH),
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
            tempCaptureTextureW = other.tempCaptureTextureW;
            tempCaptureTextureH = other.tempCaptureTextureH;
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
            tempCaptureTextureW = other.tempCaptureTextureW;
            tempCaptureTextureH = other.tempCaptureTextureH;
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

struct LogInstanceInfo {
    DWORD processId = 0;
    uint64_t processStartFileTime = 0;
    std::wstring logFilePath;
};

struct LogSession {
    std::wstring logsDirectory;
    std::wstring logFilePath;
    std::wstring ownerFilePath;
    DWORD processId = 0;
    uint64_t processStartFileTime = 0;
    size_t slotIndex = 0;
    std::vector<LogInstanceInfo> otherOpenInstances;
};

struct UserImageInstance {
    GLuint textureId = 0;
    int width = 0;
    int height = 0;
    int textureStorageHeight = 0;
    int frameCount = 1;
    int framesPerTexture = 1;
    bool isFullyTransparent = false;

    // Render-thread-only texture sampling state cache.
    bool filterInitialized = false;
    bool lastPixelatedScaling = false;

    bool isAnimated = false;
    bool isVideo = false;
    std::vector<GLuint> frameTextures;
    std::vector<int> frameTextureHeights;
    std::vector<int> frameDelays;
    std::vector<uint64_t> frameEndTimesMs;
    uint64_t totalAnimationDurationMs = 0;
    size_t currentFrame = 0;
    std::chrono::steady_clock::time_point lastFrameTime;

    struct CachedImageRenderState {
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
extern std::wstring g_hermesAliveFilePath;
extern std::wstring g_stateOutputFilePath;
extern std::atomic<bool> g_isStateOutputAvailable;
extern std::atomic<GameStateSourceKind> g_activeGameStateSource;
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

void APIENTRY BindTextureDirect(GLenum target, GLuint texture);
void InvalidateTrackedGameTextureId(bool clearSwapThread = false, bool clearCachedTexture = true);

bool AcquireLatestLogSession(const std::wstring& logsDirectory, LogSession& outSession);
void ReleaseLatestLogSession(LogSession& session);
std::string BuildLogSessionHeader(const LogSession& session);
bool GetCurrentProcessLogFilePath(const std::wstring& logsDirectory, std::wstring& outLogFilePath);

void StartLogThread(); // Start background log writer thread
void StopLogThread();  // Stop background log writer thread (flushes first)
void FlushLogs();
void QueueArchivedLogCompression(const std::wstring& archivedLogPath);
void ProcessQueuedArchivedLogCompressions();

enum LogCategoryEnum : uint8_t {
    Log_ModeSwitch,
    Log_Animation,
    Log_Hotkey,
    Log_Obs,
    Log_WindowOverlay,
    Log_BrowserOverlay,
    Log_Ninjabrain,
    Log_FileMonitor,
    Log_ImageMonitor,
    Log_Performance,
    Log_TextureOps,
    Log_Gui,
    Log_Init,
    Log_CursorTextures,
    Log_HookChain,
};
bool ShouldLogCategory(LogCategoryEnum category);
void LogCategory(LogCategoryEnum, const std::string& message);

template <typename... Args> requires (sizeof...(Args) > 0)
void LogCategory(const LogCategoryEnum category, const std::format_string<Args...>& fmt, Args&&... args) {
    // The same check is in the implementation of "LogCategory(const std::string&)"
    // Avoid actually formatting unless the message is going to be logged
    if (ShouldLogCategory(category)) {
        Log(std::format(fmt, std::forward<Args>(args)...));
    }
}

std::wstring Utf8ToWide(const std::string& utf8_string);
std::string WideToUtf8(const std::wstring& wstr);
std::string SanitizePathForDisplay(const std::string& path);
std::wstring SanitizePathForDisplay(const std::wstring& path);
std::string FileNameForDisplay(const std::string& path);
std::wstring GetToolscreenPath();

bool CompressFileToGzip(const std::wstring& srcPath, const std::wstring& dstPath);

inline std::string GetKeyComboString(const std::vector<DWORD>& keys) {
    std::string keyStr;
    for (size_t k = 0; k < keys.size(); ++k) {
        keyStr += VkToString(keys[k]);
        if (k < keys.size() - 1) keyStr += '+';
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

inline int GetCenteredAxisOffset(int containerSize, int contentSize) {
    return containerSize / 2 - contentSize / 2;
}

bool GetMonitorRectForWindow(HWND hwnd, RECT& outRect);
bool GetMonitorSizeForWindow(HWND hwnd, int& outW, int& outH);
bool GetWindowClientRectInScreen(HWND hwnd, RECT& outRect);
bool IsWindowInForegroundTree(HWND hwnd);
BOOL ClipCursorDirect(const RECT* lpRect);
bool ApplyConfineCursorToGameWindow();
void ApplyDeferredGuiCursorModeAfterClose();
void FinalizeGuiCursorStateAfterClose();

UINT GetToolscreenBorderlessToggleMessageId();
void RememberRequestedWindowClientResize(int width, int height);
bool RequestWindowClientResize(HWND hwnd, int width, int height, const char* source = nullptr);
bool GetRecentRequestedWindowClientResizes(int& outCurrentW, int& outCurrentH, int& outPreviousW, int& outPreviousH);
bool CenterWindowedRestoreOnCurrentMonitor(HWND hwnd, const char* source = nullptr);
void ToggleBorderlessWindowedFullscreen(HWND hwnd);
bool IsCursorVisible();
void WriteCurrentModeToFile(const std::string& modeId);
bool SwitchToMode(const std::string& newModeId, const std::string& source = "", bool forceCut = false);
bool IsHardcodedMode(const std::string& modeId);
bool EqualsIgnoreCase(const std::string& a, const std::string& b);
std::string ModeSourceTypeToString(ModeSourceType type);
ModeSourceType StringToModeSourceType(const std::string& value);
std::vector<ModeSourceRef> BuildModeSourcesFromLegacyLists(const std::vector<std::string>& mirrorIds,
                                                           const std::vector<std::string>& mirrorGroupIds,
                                                           const std::vector<std::string>& imageIds,
                                                           const std::vector<std::string>& windowOverlayIds,
                                                           const std::vector<std::string>& browserOverlayIds);
bool ModeHasSource(const ModeConfig& mode, ModeSourceType type, const std::string& id);
bool AddModeSource(ModeConfig& mode, ModeSourceType type, const std::string& id);
bool RemoveModeSource(ModeConfig& mode, ModeSourceType type, const std::string& id);
size_t RemoveAllModeSources(ModeConfig& mode, ModeSourceType type, const std::string& id);
bool RenameModeSource(ModeConfig& mode, ModeSourceType type, const std::string& oldId, const std::string& newId);
bool MoveModeSource(ModeConfig& mode, size_t fromIndex, size_t toIndex);
void CollectModeMirrorIds(const Config& config, const ModeConfig& mode, std::unordered_set<std::string>& outMirrorIds);
void CollectModeOrderedMirrorIds(const Config& config, const ModeConfig& mode, std::vector<std::string>& outMirrorIds);
const ModeConfig* GetMode(const std::string& id);
const ModeConfig* GetMode_Internal(const std::string& id);
ModeConfig* GetModeMutable(const std::string& id);
MirrorConfig* GetMutableMirror(const std::string& name);

const ModeConfig* GetModeFromSnapshot(const Config& config, const std::string& id);
const ModeConfig* GetModeFromSnapshotOrFallback(const Config& config, const std::string& id,
                                                std::string* resolvedId = nullptr);
const MirrorConfig* GetMirrorFromSnapshot(const Config& config, const std::string& name);
bool isWallTitleOrWaiting(const std::string& state);
ModeViewportInfo GetCurrentModeViewport();
ModeViewportInfo GetCurrentModeViewport_Internal(); // Lock-free implementation using double-buffered mode ID
bool ResolvePresentedGameViewport(ModeViewportInfo& outViewport);
bool ResolvePresentedGameBlitRect(int& outDstX0, int& outDstY0, int& outDstX1, int& outDstY1);
bool GetLatestGameViewportSize(int& outWidth, int& outHeight);
void InvalidateLatestGameViewportSize();
#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
void SetLatestGameViewportSizeForTests(int width, int height);
#endif

GLuint CompileShader(GLenum type, const char* source);
GLuint CreateShaderProgram(const char* vert, const char* frag);

void LoadImageAsync(DecodedImageData::Type type, std::string id, std::string path, const std::wstring& toolscreenPath);
void LoadAllImages();

bool MatchesConfiguredInputKeyEvent(DWORD incomingVk, DWORD incomingRawVk, DWORD configuredKey);
bool IsConfiguredInputKeyDown(DWORD key);

bool CheckHotkeyMatch(const std::vector<DWORD>& keys, WPARAM wParam, const std::vector<DWORD>& exclusionKeys = {},
                      bool skipLiveKeyStateChecks = false, size_t minKeyCount = 0, WPARAM rawWParam = 0,
                      bool hasIncomingKeyState = false, bool incomingIsKeyDown = false, bool skipExclusionChecks = false);

std::string FindHotkeyConflict(const std::vector<DWORD>& newKeys, const std::string& excludeLabel);

void BackupConfigFile();

void GetRelativeCoords(const std::string& type, int relX, int relY, int w, int h, int containerW, int containerH, int& outX, int& outY);
void GetRelativeCoordsForImage(const std::string& type, int relX, int relY, int w, int h, int containerW, int containerH, int& outX,
                               int& outY);
void GetRelativeCoordsForImageWithViewport(const std::string& type, int relX, int relY, int w, int h, int gameX, int gameY, int gameW,
                                           int gameH, int fullW, int fullH, int& outX, int& outY);

inline bool IsViewportRelativeAnchor(const std::string& relativeTo) {
    if (relativeTo.length() > 8 && relativeTo.substr(relativeTo.length() - 8) == "Viewport") { return true; }
    return false;
}
void CalculateFinalScreenPos(const MirrorConfig* conf, const MirrorInstance& inst, int gameW, int gameH, int finalX, int finalY, int finalW,
                             int finalH, int fullW, int fullH, int& outScreenX, int& outScreenY);

void ScreenDeltaToMirrorConfigDelta(const std::string& relativeTo,
                                    int screenDx, int screenDy,
                                    int gameW, int gameH,
                                    int finalW, int finalH,
                                    int& outConfigDx, int& outConfigDy);

void ScreenshotToClipboard(int width, int height);

DWORD WINAPI FileMonitorThread(LPVOID lpParam);
DWORD WINAPI ImageMonitorThread(LPVOID lpParam);

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
