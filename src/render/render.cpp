#include "render.h"
#include "platform/resource.h"
#include "features/cursor_trail.h"
#include "features/ninjabrain_data.h"
#include "features/fake_cursor.h"
#include "gui/gui.h"
#include "runtime/logic_thread.h"
#include "mirror_thread.h"
#include "obs_thread.h"
#include "common/i18n.h"
#include "common/ninjabrain_information_messages.h"
#include "common/profiler.h"
#include "common/font_assets.h"
#include "gui/imgui_input_queue.h"
#include "third_party/stb_image.h"
#include "common/utils.h"
#include "features/browser_overlay.h"
#include "features/virtual_camera.h"
#include "features/window_overlay.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_win32.h"
#include <Shlwapi.h>
#include <cctype>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

namespace {
constexpr size_t kMaxDecodedImageUploadBytes = 512ull * 1024ull * 1024ull;
constexpr size_t kMaxVirtualCameraSyncBytes = 128ull * 1024ull * 1024ull;

bool TryMultiplySize(size_t left, size_t right, size_t& out) {
    if (left == 0 || right == 0) {
        out = 0;
        return true;
    }
    if (left > (std::numeric_limits<size_t>::max)() / right) { return false; }
    out = left * right;
    return true;
}

bool TryComputeImageByteCount(int width, int height, int channels, size_t& outBytes) {
    if (width <= 0 || height <= 0 || channels <= 0) { return false; }

    size_t pixelCount = 0;
    if (!TryMultiplySize(static_cast<size_t>(width), static_cast<size_t>(height), pixelCount)) { return false; }
    return TryMultiplySize(pixelCount, static_cast<size_t>(channels), outBytes);
}

std::string FormatByteCount(size_t bytes) {
    const size_t mib = 1024ull * 1024ull;
    return std::to_string(bytes) + " bytes (" + std::to_string(bytes / mib) + " MiB)";
}

bool TryDescribeDecodedImageStorage(const DecodedImageData& imgData, size_t& outBytes, std::string& outReason) {
    if (imgData.width <= 0 || imgData.height <= 0) {
        outReason = "invalid dimensions " + std::to_string(imgData.width) + "x" + std::to_string(imgData.height);
        return false;
    }

    if (imgData.isAnimated) {
        if (imgData.frameCount <= 1) {
            outReason = "animated image has invalid frameCount=" + std::to_string(imgData.frameCount);
            return false;
        }
        if (imgData.frameHeight <= 0) {
            outReason = "animated image has invalid frameHeight=" + std::to_string(imgData.frameHeight);
            return false;
        }

        size_t expectedHeight = 0;
        if (!TryMultiplySize(static_cast<size_t>(imgData.frameHeight), static_cast<size_t>(imgData.frameCount), expectedHeight)) {
            outReason = "animated image height overflowed for frameHeight=" + std::to_string(imgData.frameHeight) +
                        ", frameCount=" + std::to_string(imgData.frameCount);
            return false;
        }
        if (expectedHeight != static_cast<size_t>(imgData.height)) {
            outReason = "animated image height mismatch: got " + std::to_string(imgData.height) + ", expected " +
                        std::to_string(expectedHeight);
            return false;
        }
    }

    if (!TryComputeImageByteCount(imgData.width, imgData.height, 4, outBytes)) {
        outReason = "byte-count overflowed for " + std::to_string(imgData.width) + "x" + std::to_string(imgData.height) + "x4";
        return false;
    }

    return true;
}

bool TryComputeVirtualCameraSyncBytes(int width, int height, size_t& yBytes, size_t& uvBytes, size_t& totalBytes, std::string& outReason) {
    if (width <= 0 || height <= 0) {
        outReason = "invalid dimensions " + std::to_string(width) + "x" + std::to_string(height);
        return false;
    }
    if ((width & 1) != 0 || (height & 1) != 0) {
        outReason = "dimensions must be even for NV12: " + std::to_string(width) + "x" + std::to_string(height);
        return false;
    }

    if (!TryComputeImageByteCount(width, height, 1, yBytes)) {
        outReason = "luma byte-count overflowed for " + std::to_string(width) + "x" + std::to_string(height);
        return false;
    }
    uvBytes = yBytes / 2u;
    if (!TryMultiplySize(static_cast<size_t>(width / 2), static_cast<size_t>(height / 2), totalBytes)) {
        outReason = "chroma plane size overflowed for " + std::to_string(width) + "x" + std::to_string(height);
        return false;
    }
    totalBytes = yBytes + uvBytes;
    return true;
}

class ScopedTextureFilterGuard {
public:
    ScopedTextureFilterGuard(GLuint texture, GLint minFilter, GLint magFilter, GLenum textureUnit = GL_TEXTURE0,
                             bool clearBoundSampler = false)
        : texture_(texture), textureUnit_(textureUnit) {
        if (texture_ == 0) { return; }

        glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture_);
        if (previousActiveTexture_ != static_cast<GLint>(textureUnit_)) {
            glActiveTexture(textureUnit_);
        }

        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTextureBinding_);
        if (static_cast<GLuint>(previousTextureBinding_) != texture_) {
            BindTextureDirect(GL_TEXTURE_2D, texture_);
            restoreTextureBinding_ = true;
        }

        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &previousMinFilter_);
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &previousMagFilter_);

        if (previousMinFilter_ != minFilter) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minFilter);
        }
        if (previousMagFilter_ != magFilter) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magFilter);
        }

        if (clearBoundSampler && (GLEW_VERSION_3_3 || GLEW_ARB_sampler_objects)) {
            glGetIntegerv(GL_SAMPLER_BINDING, &previousSampler_);
            if (previousSampler_ != 0) {
                glBindSampler(static_cast<GLuint>(textureUnit_ - GL_TEXTURE0), 0);
                restoreSampler_ = true;
            }
        }

        if (previousActiveTexture_ != static_cast<GLint>(textureUnit_)) {
            glActiveTexture(previousActiveTexture_);
        }

        active_ = true;
    }

    ~ScopedTextureFilterGuard() {
        if (!active_) { return; }

        GLint currentActiveTexture = 0;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &currentActiveTexture);
        if (currentActiveTexture != static_cast<GLint>(textureUnit_)) {
            glActiveTexture(textureUnit_);
        }

        GLint currentTextureBinding = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &currentTextureBinding);
        if (static_cast<GLuint>(currentTextureBinding) != texture_) {
            BindTextureDirect(GL_TEXTURE_2D, texture_);
        }

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, previousMinFilter_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, previousMagFilter_);

        if (restoreSampler_) {
            glBindSampler(static_cast<GLuint>(textureUnit_ - GL_TEXTURE0), static_cast<GLuint>(previousSampler_));
        }
        if (restoreTextureBinding_) {
            BindTextureDirect(GL_TEXTURE_2D, static_cast<GLuint>(previousTextureBinding_));
        }

        if (currentActiveTexture != static_cast<GLint>(textureUnit_)) {
            glActiveTexture(currentActiveTexture);
        }
    }

private:
    GLuint texture_ = 0;
    GLenum textureUnit_ = GL_TEXTURE0;
    GLint previousActiveTexture_ = GL_TEXTURE0;
    GLint previousTextureBinding_ = 0;
    GLint previousMinFilter_ = GL_NEAREST;
    GLint previousMagFilter_ = GL_NEAREST;
    GLint previousSampler_ = 0;
    bool restoreTextureBinding_ = false;
    bool restoreSampler_ = false;
    bool active_ = false;
};

void LogVirtualCameraSyncGuardOnce(const std::string& reason, int width, int height, size_t totalBytes) {
    static int s_lastWidth = 0;
    static int s_lastHeight = 0;
    static size_t s_lastBytes = 0;
    static std::string s_lastReason;

    if (s_lastWidth == width && s_lastHeight == height && s_lastBytes == totalBytes && s_lastReason == reason) { return; }

    s_lastWidth = width;
    s_lastHeight = height;
    s_lastBytes = totalBytes;
    s_lastReason = reason;

    Log("Virtual Camera: rejecting synchronous readback at " + std::to_string(width) + "x" + std::to_string(height) +
        " (" + FormatByteCount(totalBytes) + "): " + reason);
}
} // namespace

static std::unordered_map<std::string, size_t> s_mirrorLookupCache;
static std::unordered_map<std::string, size_t> s_imageLookupCache;
static std::unordered_map<std::string, size_t> s_windowOverlayLookupCache;
static std::unordered_map<std::string, size_t> s_browserOverlayLookupCache;
static std::atomic<uint64_t> s_configCacheVersion{ 0 };
static uint64_t s_lastCacheRebuildVersion = 0;
static std::mutex s_lookupCacheMutex;

static std::string MakeLowercaseKey(const std::string& value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (unsigned char ch : value) {
        lowered.push_back(static_cast<char>(std::tolower(ch)));
    }
    return lowered;
}

static void RebuildConfigLookupCaches() {
    s_mirrorLookupCache.clear();
    s_imageLookupCache.clear();
    s_windowOverlayLookupCache.clear();
    s_browserOverlayLookupCache.clear();

    s_mirrorLookupCache.reserve(g_config.mirrors.size());
    for (size_t i = 0; i < g_config.mirrors.size(); ++i) { s_mirrorLookupCache[g_config.mirrors[i].name] = i; }

    s_imageLookupCache.reserve(g_config.images.size());
    for (size_t i = 0; i < g_config.images.size(); ++i) { s_imageLookupCache[g_config.images[i].name] = i; }

    s_windowOverlayLookupCache.reserve(g_config.windowOverlays.size());
    for (size_t i = 0; i < g_config.windowOverlays.size(); ++i) { s_windowOverlayLookupCache[g_config.windowOverlays[i].name] = i; }

    s_browserOverlayLookupCache.reserve(g_config.browserOverlays.size());
    for (size_t i = 0; i < g_config.browserOverlays.size(); ++i) { s_browserOverlayLookupCache[g_config.browserOverlays[i].name] = i; }
}

void InvalidateConfigLookupCaches() { s_configCacheVersion.fetch_add(1, std::memory_order_release); }

static void EnsureConfigCachesValid() {
    uint64_t currentVersion = s_configCacheVersion.load(std::memory_order_acquire);
    if (g_configIsDirty) {
        InvalidateConfigLookupCaches();
        currentVersion = s_configCacheVersion.load(std::memory_order_acquire);
    }
    if (currentVersion != s_lastCacheRebuildVersion) {
        std::lock_guard<std::mutex> lock(s_lookupCacheMutex);
        // Double-check after acquiring lock
        currentVersion = s_configCacheVersion.load(std::memory_order_acquire);
        if (currentVersion != s_lastCacheRebuildVersion) {
            RebuildConfigLookupCaches();
            s_lastCacheRebuildVersion = currentVersion;
        }
    }
}

void CollectActiveElementsForMode(const Config& config, const std::string& modeId, bool onlyOnMyScreenPass,
                                  std::vector<MirrorConfig>& outMirrors, std::vector<ImageConfig>& outImages,
                                  std::vector<WindowOverlayConfig>& outWindowOverlays,
                                  std::vector<BrowserOverlayConfig>& outBrowserOverlays,
                                  int screenWOverride, int screenHOverride) {
    outMirrors.clear();
    outImages.clear();
    outWindowOverlays.clear();
    outBrowserOverlays.clear();

    std::unordered_map<std::string, const ModeConfig*> modeById;
    std::unordered_map<std::string, const ModeConfig*> modeByIdLowered;
    std::unordered_map<std::string, const MirrorConfig*> mirrorByName;
    std::unordered_map<std::string, const MirrorGroupConfig*> groupByName;
    std::unordered_map<std::string, const ImageConfig*> imageByName;
    std::unordered_map<std::string, const WindowOverlayConfig*> windowOverlayByName;
    std::unordered_map<std::string, const BrowserOverlayConfig*> browserOverlayByName;

    modeById.reserve(config.modes.size());
    modeByIdLowered.reserve(config.modes.size());
    for (const auto& m : config.modes) {
        modeById[m.id] = &m;
        modeByIdLowered[MakeLowercaseKey(m.id)] = &m;
    }

    mirrorByName.reserve(config.mirrors.size());
    for (const auto& m : config.mirrors) { mirrorByName[m.name] = &m; }

    groupByName.reserve(config.mirrorGroups.size());
    for (const auto& g : config.mirrorGroups) { groupByName[g.name] = &g; }

    imageByName.reserve(config.images.size());
    for (const auto& img : config.images) { imageByName[img.name] = &img; }

    windowOverlayByName.reserve(config.windowOverlays.size());
    for (const auto& overlay : config.windowOverlays) { windowOverlayByName[overlay.name] = &overlay; }

    browserOverlayByName.reserve(config.browserOverlays.size());
    for (const auto& overlay : config.browserOverlays) { browserOverlayByName[overlay.name] = &overlay; }

    const ModeConfig* mode = nullptr;
    if (auto it = modeById.find(modeId); it != modeById.end()) {
        mode = it->second;
    } else {
        auto loweredIt = modeByIdLowered.find(MakeLowercaseKey(modeId));
        if (loweredIt != modeByIdLowered.end()) {
            mode = loweredIt->second;
        }
    }
    if (!mode) return;

    outMirrors.reserve(mode->mirrorIds.size() + mode->mirrorGroupIds.size());
    outImages.reserve(mode->imageIds.size());
    outWindowOverlays.reserve(mode->windowOverlayIds.size());
    outBrowserOverlays.reserve(mode->browserOverlayIds.size());
    const int resolvedScreenW = screenWOverride > 0 ? screenWOverride : GetCachedWindowWidth();
    const int resolvedScreenH = screenHOverride > 0 ? screenHOverride : GetCachedWindowHeight();

    for (const auto& mirrorName : mode->mirrorIds) {
        auto it = mirrorByName.find(mirrorName);
        if (it == mirrorByName.end() || !it->second) continue;
        const MirrorConfig& mirror = *it->second;
        if (!onlyOnMyScreenPass || mirror.onlyOnMyScreen) { outMirrors.push_back(mirror); }
    }

    for (const auto& groupName : mode->mirrorGroupIds) {
        auto git = groupByName.find(groupName);
        if (git == groupByName.end() || !git->second) continue;
        const auto& group = *git->second;

        for (const auto& item : group.mirrors) {
            if (!item.enabled) continue;

            auto mit = mirrorByName.find(item.mirrorId);
            if (mit == mirrorByName.end() || !mit->second) continue;

            const auto& mirror = *mit->second;
            if (onlyOnMyScreenPass && !mirror.onlyOnMyScreen) continue;

            MirrorConfig groupedMirror = mirror;
            int groupX = group.output.x;
            int groupY = group.output.y;
            if (group.output.useRelativePosition) {
                if (resolvedScreenW > 0) {
                    groupX = static_cast<int>(group.output.relativeX * static_cast<float>(resolvedScreenW));
                }
                if (resolvedScreenH > 0) {
                    groupY = static_cast<int>(group.output.relativeY * static_cast<float>(resolvedScreenH));
                }
            }

            groupedMirror.output.x = groupX + item.offsetX;
            groupedMirror.output.y = groupY + item.offsetY;
            groupedMirror.output.relativeTo = group.output.relativeTo;
            groupedMirror.output.useRelativePosition = group.output.useRelativePosition;
            groupedMirror.output.relativeX = group.output.relativeX;
            groupedMirror.output.relativeY = group.output.relativeY;
            groupedMirror.runtimeGrouped = true;
            groupedMirror.runtimeGroupName = group.name;
            if (item.widthPercent != 1.0f || item.heightPercent != 1.0f) {
                groupedMirror.output.separateScale = true;
                float baseScaleX = mirror.output.separateScale ? mirror.output.scaleX : mirror.output.scale;
                float baseScaleY = mirror.output.separateScale ? mirror.output.scaleY : mirror.output.scale;
                groupedMirror.output.scaleX = baseScaleX * item.widthPercent;
                groupedMirror.output.scaleY = baseScaleY * item.heightPercent;
            }

            outMirrors.push_back(groupedMirror);
        }
    }

    if (g_imageOverlaysVisible.load(std::memory_order_acquire)) {
        for (const auto& imageName : mode->imageIds) {
            auto it = imageByName.find(imageName);
            if (it == imageByName.end() || !it->second) continue;
            const ImageConfig& image = *it->second;
            if (!onlyOnMyScreenPass || image.onlyOnMyScreen) { outImages.push_back(image); }
        }
    }

    if (g_windowOverlaysVisible.load(std::memory_order_acquire)) {
        for (const auto& overlayId : mode->windowOverlayIds) {
            auto it = windowOverlayByName.find(overlayId);
            if (it == windowOverlayByName.end() || !it->second) continue;
            const WindowOverlayConfig& overlay = *it->second;
            if (!onlyOnMyScreenPass || overlay.onlyOnMyScreen) { outWindowOverlays.push_back(overlay); }
        }
    }

    if (g_browserOverlaysVisible.load(std::memory_order_acquire)) {
        for (const auto& overlayId : mode->browserOverlayIds) {
            auto it = browserOverlayByName.find(overlayId);
            if (it == browserOverlayByName.end() || !it->second) continue;
            const BrowserOverlayConfig& overlay = *it->second;
            if (!onlyOnMyScreenPass || overlay.onlyOnMyScreen) { outBrowserOverlays.push_back(overlay); }
        }
    }
}

extern std::atomic<bool> g_graphicsHookDetected;

GLuint g_filterProgram = 0;
GLuint g_renderProgram = 0;
GLuint g_renderPassthroughProgram = 0;
GLuint g_backgroundProgram = 0;
GLuint g_solidColorProgram = 0;
GLuint g_imageRenderProgram = 0;
GLuint g_passthroughProgram = 0;
GLuint g_gradientProgram = 0;
static GLuint g_staticBorderProgram = 0;
static GLuint g_virtualCameraNv12Program = 0;

FilterShaderLocs g_filterShaderLocs;
RenderShaderLocs g_renderShaderLocs;
RenderPassthroughShaderLocs g_renderPassthroughShaderLocs;
BackgroundShaderLocs g_backgroundShaderLocs;
SolidColorShaderLocs g_solidColorShaderLocs;
ImageRenderShaderLocs g_imageRenderShaderLocs;
PassthroughShaderLocs g_passthroughShaderLocs;
GradientShaderLocs g_gradientShaderLocs;
static struct {
    GLint shape = -1;
    GLint borderColor = -1;
    GLint thickness = -1;
    GLint radius = -1;
    GLint size = -1;
    GLint quadSize = -1;
} g_staticBorderShaderLocs;
static struct {
    GLint screenTexture = -1;
    GLint sourceTexelSize = -1;
    GLint outputMode = -1;
    GLint colorSpaceMode = -1;
} g_virtualCameraNv12ShaderLocs;

std::atomic<bool> g_shouldRenderGui{ false };
std::atomic<bool> g_showPerformanceOverlay{ false };
std::atomic<bool> g_showProfiler{ false };
std::atomic<bool> g_showEyeZoom{ false };
std::atomic<bool> g_eyeZoomFontNeedsReload{ false };
std::atomic<float> g_eyeZoomFadeOpacity{ 1.0f };
std::atomic<int> g_eyeZoomAnimatedViewportX{ -1 };
std::atomic<bool> g_isTransitioningFromEyeZoom{ false };
std::atomic<bool> g_showTextureGrid{ false };
std::atomic<int> g_textureGridModeWidth{ 0 };
std::atomic<int> g_textureGridModeHeight{ 0 };

static GLuint s_eyeZoomSnapshotTexture = 0;
static GLuint s_eyeZoomSnapshotFBO = 0;
static int s_eyeZoomSnapshotWidth = 0;
static int s_eyeZoomSnapshotHeight = 0;
static bool s_eyeZoomSnapshotValid = false;

static GLuint s_eyeZoomTempFBO = 0;
static GLuint s_eyeZoomTempTexture = 0;
static int s_eyeZoomTempWidth = 0;
static int s_eyeZoomTempHeight = 0;
static GLuint s_eyeZoomBlitFBO = 0;

static GLuint s_boatIconTex[4] = {0, 0, 0, 0};
static GLuint s_ninjabrainMessageIconTex[3] = {0, 0, 0};
static bool   s_ninjabrainOverlayIconsLoaded = false;

GLuint GetEyeZoomSnapshotTexture() { return s_eyeZoomSnapshotTexture; }
int GetEyeZoomSnapshotWidth() { return s_eyeZoomSnapshotWidth; }
int GetEyeZoomSnapshotHeight() { return s_eyeZoomSnapshotHeight; }

std::unordered_map<std::string, MirrorInstance> g_mirrorInstances;
std::unordered_map<std::string, BackgroundTextureInstance> g_backgroundTextures;
std::unordered_map<std::string, UserImageInstance> g_userImages;
GLuint g_vao = 0;
GLuint g_vbo = 0;
GLuint g_debugVAO = 0;
GLuint g_debugVBO = 0;
GLuint g_sceneFBO = 0;
GLuint g_sceneTexture = 0;
static GLsizeiptr g_vboCapacityBytes = 0;

int g_sceneW = 0;
int g_sceneH = 0;
static constexpr int SAME_THREAD_OBS_BUFFER_COUNT = 2;
static GLuint g_sameThreadObsComposeFBOs[SAME_THREAD_OBS_BUFFER_COUNT] = {};
static GLuint g_sameThreadObsComposeTextures[SAME_THREAD_OBS_BUFFER_COUNT] = {};
static int g_sameThreadObsComposeW = 0;
static int g_sameThreadObsComposeH = 0;
static int g_sameThreadObsComposePublishedIndex = -1;
static int g_sameThreadObsComposeWriteIndex = 0;
static GLuint g_sameThreadVirtualCameraScaleFBO = 0;
static GLuint g_sameThreadVirtualCameraScaleTexture = 0;
static int g_sameThreadVirtualCameraScaleW = 0;
static int g_sameThreadVirtualCameraScaleH = 0;
static GLuint g_sameThreadVirtualCameraReadFBO = 0;
static GLuint g_sameThreadVirtualCameraConvertFBO = 0;
static GLuint g_sameThreadVirtualCameraLumaTexture = 0;
static GLuint g_sameThreadVirtualCameraChromaTexture = 0;
static int g_sameThreadVirtualCameraNv12W = 0;
static int g_sameThreadVirtualCameraNv12H = 0;
static int g_sameThreadVirtualCameraSynchronousRecoveryFrames = 0;
static constexpr int SAME_THREAD_VIRTUAL_CAMERA_PBO_COUNT = 3;
struct SameThreadVirtualCameraReadbackSlot {
    GLuint yPbo = 0;
    GLuint uvPbo = 0;
    GLuint yTexture = 0;
    GLuint uvTexture = 0;
    GLsync fence = nullptr;
    uint64_t timestamp = 0;
    int width = 0;
    int height = 0;
    int textureWidth = 0;
    int textureHeight = 0;
    bool pending = false;
};
static SameThreadVirtualCameraReadbackSlot g_sameThreadVirtualCameraReadbackSlots[SAME_THREAD_VIRTUAL_CAMERA_PBO_COUNT] = {};
static int g_sameThreadVirtualCameraReadbackW = 0;
static int g_sameThreadVirtualCameraReadbackH = 0;
static int g_sameThreadVirtualCameraReadbackWriteIndex = 0;
static int g_sameThreadVirtualCameraCaptureSourceW = 0;
static int g_sameThreadVirtualCameraCaptureSourceH = 0;

static bool ConvertSameThreadVirtualCameraTextureToNv12(GLuint srcTexture, int srcW, int srcH,
                                                        const SameThreadVirtualCameraReadbackSlot& slot);

GLuint g_fullscreenQuadVAO = 0;
GLuint g_fullscreenQuadVBO = 0;

// These maps are accessed from multiple threads (render + GUI)
std::shared_mutex g_mirrorInstancesMutex;
std::mutex g_userImagesMutex;
std::mutex g_backgroundTexturesMutex;

std::vector<GLuint> g_texturesToDelete;
std::mutex g_texturesToDeleteMutex;
std::atomic<bool> g_hasTexturesToDelete{ false };
std::atomic<bool> g_glInitialized{ false };
std::atomic<bool> g_isGameFocused{ true };
GameViewportGeometry g_lastFrameGeometry;
std::mutex g_geometryMutex;

std::string s_hoveredImageName = "";
std::string s_draggedImageName = "";
bool s_isDragging = false;
POINT s_lastMousePos = { 0, 0 };
POINT s_dragStartPos = { 0, 0 };

std::string s_hoveredWindowOverlayName = "";
std::string s_draggedWindowOverlayName = "";
bool s_isWindowOverlayDragging = false;
std::string s_hoveredBrowserOverlayName = "";
std::string s_draggedBrowserOverlayName = "";
bool s_isBrowserOverlayDragging = false;
POINT s_windowOverlayDragStart = { 0, 0 };
int s_initialX = 0, s_initialY = 0;

struct EyeZoomTextLabel {
    int number;
    float centerX;
    float centerY;
    float boxWidth;
    float boxHeight;
    float clipMinX;
    float clipMinY;
    float clipMaxX;
    float clipMaxY;
    EyeZoomFontSizeMode fontSizeMode;
    Color color;
};
static std::vector<EyeZoomTextLabel> s_eyezoomTextLabels;

static void DiscardSameThreadVirtualCameraReadbacks() {
    for (auto& slot : g_sameThreadVirtualCameraReadbackSlots) {
        if (slot.fence && glIsSync(slot.fence)) { glDeleteSync(slot.fence); }
        slot.fence = nullptr;
        slot.pending = false;
        slot.timestamp = 0;
        slot.width = 0;
        slot.height = 0;
    }

    g_sameThreadVirtualCameraReadbackWriteIndex = 0;
    g_sameThreadVirtualCameraCaptureSourceW = 0;
    g_sameThreadVirtualCameraCaptureSourceH = 0;
}

static void ReleaseSameThreadVirtualCameraReadbacks() {
    DiscardSameThreadVirtualCameraReadbacks();

    for (auto& slot : g_sameThreadVirtualCameraReadbackSlots) {
        if (slot.yPbo != 0) {
            glDeleteBuffers(1, &slot.yPbo);
            slot.yPbo = 0;
        }
        if (slot.uvPbo != 0) {
            glDeleteBuffers(1, &slot.uvPbo);
            slot.uvPbo = 0;
        }
        if (slot.yTexture != 0) {
            glDeleteTextures(1, &slot.yTexture);
            slot.yTexture = 0;
        }
        if (slot.uvTexture != 0) {
            glDeleteTextures(1, &slot.uvTexture);
            slot.uvTexture = 0;
        }
        slot.textureWidth = 0;
        slot.textureHeight = 0;
    }

    g_sameThreadVirtualCameraReadbackW = 0;
    g_sameThreadVirtualCameraReadbackH = 0;
    g_sameThreadVirtualCameraReadbackWriteIndex = 0;
    g_sameThreadVirtualCameraCaptureSourceW = 0;
    g_sameThreadVirtualCameraCaptureSourceH = 0;
}

void ResetSameThreadVirtualCameraCaptureState() {
    DiscardSameThreadVirtualCameraReadbacks();
    g_sameThreadVirtualCameraSynchronousRecoveryFrames = 0;

    if (g_sameThreadVirtualCameraScaleTexture != 0) {
        glDeleteTextures(1, &g_sameThreadVirtualCameraScaleTexture);
        g_sameThreadVirtualCameraScaleTexture = 0;
    }
    if (g_sameThreadVirtualCameraLumaTexture != 0) {
        glDeleteTextures(1, &g_sameThreadVirtualCameraLumaTexture);
        g_sameThreadVirtualCameraLumaTexture = 0;
    }
    if (g_sameThreadVirtualCameraChromaTexture != 0) {
        glDeleteTextures(1, &g_sameThreadVirtualCameraChromaTexture);
        g_sameThreadVirtualCameraChromaTexture = 0;
    }
    if (g_sameThreadVirtualCameraScaleFBO != 0) {
        glDeleteFramebuffers(1, &g_sameThreadVirtualCameraScaleFBO);
        g_sameThreadVirtualCameraScaleFBO = 0;
    }
    if (g_sameThreadVirtualCameraReadFBO != 0) {
        glDeleteFramebuffers(1, &g_sameThreadVirtualCameraReadFBO);
        g_sameThreadVirtualCameraReadFBO = 0;
    }
    if (g_sameThreadVirtualCameraConvertFBO != 0) {
        glDeleteFramebuffers(1, &g_sameThreadVirtualCameraConvertFBO);
        g_sameThreadVirtualCameraConvertFBO = 0;
    }

    g_sameThreadVirtualCameraScaleW = 0;
    g_sameThreadVirtualCameraScaleH = 0;
    g_sameThreadVirtualCameraNv12W = 0;
    g_sameThreadVirtualCameraNv12H = 0;
}

static bool EnsureSameThreadVirtualCameraSynchronousTargets(int width, int height) {
    if (width <= 0 || height <= 0) { return false; }
    if (g_sameThreadVirtualCameraNv12W == width && g_sameThreadVirtualCameraNv12H == height &&
        g_sameThreadVirtualCameraLumaTexture != 0 && g_sameThreadVirtualCameraChromaTexture != 0) {
        return true;
    }

    if (g_sameThreadVirtualCameraLumaTexture == 0) { glGenTextures(1, &g_sameThreadVirtualCameraLumaTexture); }
    if (g_sameThreadVirtualCameraChromaTexture == 0) { glGenTextures(1, &g_sameThreadVirtualCameraChromaTexture); }
    if (g_sameThreadVirtualCameraLumaTexture == 0 || g_sameThreadVirtualCameraChromaTexture == 0) { return false; }

    BindTextureDirect(GL_TEXTURE_2D, g_sameThreadVirtualCameraLumaTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    BindTextureDirect(GL_TEXTURE_2D, g_sameThreadVirtualCameraChromaTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, width / 2, height / 2, 0, GL_RG, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    BindTextureDirect(GL_TEXTURE_2D, 0);
    g_sameThreadVirtualCameraNv12W = width;
    g_sameThreadVirtualCameraNv12H = height;
    return true;
}

static bool EnsureSameThreadVirtualCameraReadbacks(int width, int height) {
    if (width <= 0 || height <= 0) { return false; }

    const bool needsResize = (g_sameThreadVirtualCameraReadbackW != width) || (g_sameThreadVirtualCameraReadbackH != height);
    const size_t yBytes = static_cast<size_t>(width) * static_cast<size_t>(height);
    const size_t uvBytes = yBytes / 2u;
    for (auto& slot : g_sameThreadVirtualCameraReadbackSlots) {
        const bool slotMissingBuffers = (slot.yPbo == 0) || (slot.uvPbo == 0) || (slot.yTexture == 0) || (slot.uvTexture == 0);
        if (slot.yPbo == 0) { glGenBuffers(1, &slot.yPbo); }
        if (slot.uvPbo == 0) { glGenBuffers(1, &slot.uvPbo); }
        if (slot.yTexture == 0) { glGenTextures(1, &slot.yTexture); }
        if (slot.uvTexture == 0) { glGenTextures(1, &slot.uvTexture); }
        if (slot.yPbo == 0 || slot.uvPbo == 0 || slot.yTexture == 0 || slot.uvTexture == 0) { return false; }

        if (needsResize || slotMissingBuffers) {
            if (slot.fence && glIsSync(slot.fence)) { glDeleteSync(slot.fence); }
            slot.fence = nullptr;
            slot.pending = false;
            slot.timestamp = 0;
            slot.width = 0;
            slot.height = 0;

            glBindBuffer(GL_PIXEL_PACK_BUFFER, slot.yPbo);
            glBufferData(GL_PIXEL_PACK_BUFFER, static_cast<GLsizeiptr>(yBytes), nullptr, GL_STREAM_READ);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, slot.uvPbo);
            glBufferData(GL_PIXEL_PACK_BUFFER, static_cast<GLsizeiptr>(uvBytes), nullptr, GL_STREAM_READ);

            BindTextureDirect(GL_TEXTURE_2D, slot.yTexture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

            BindTextureDirect(GL_TEXTURE_2D, slot.uvTexture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, width / 2, height / 2, 0, GL_RG, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

            slot.textureWidth = width;
            slot.textureHeight = height;
        }
    }
    BindTextureDirect(GL_TEXTURE_2D, 0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    g_sameThreadVirtualCameraReadbackW = width;
    g_sameThreadVirtualCameraReadbackH = height;
    return true;
}

static bool HarvestSameThreadVirtualCameraReadback() {
    for (auto& slot : g_sameThreadVirtualCameraReadbackSlots) {
        if (!slot.pending || !slot.fence || slot.yPbo == 0 || slot.uvPbo == 0 || slot.width <= 0 || slot.height <= 0) { continue; }

        GLenum fenceStatus = glClientWaitSync(slot.fence, 0, 0);
        if (fenceStatus != GL_ALREADY_SIGNALED && fenceStatus != GL_CONDITION_SATISFIED) { continue; }

        GLint previousPackBuffer = 0;
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previousPackBuffer);
        const size_t yBytes = static_cast<size_t>(slot.width) * static_cast<size_t>(slot.height);
        const size_t uvBytes = yBytes / 2u;

        glBindBuffer(GL_PIXEL_PACK_BUFFER, slot.yPbo);
        const uint8_t* yMapped =
            static_cast<const uint8_t*>(glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, static_cast<GLsizeiptr>(yBytes), GL_MAP_READ_BIT));

        glBindBuffer(GL_PIXEL_PACK_BUFFER, slot.uvPbo);
        const uint8_t* uvMapped =
            static_cast<const uint8_t*>(glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, static_cast<GLsizeiptr>(uvBytes), GL_MAP_READ_BIT));

        if (yMapped && uvMapped) {
            WriteVirtualCameraFrameNV12Planes(yMapped, uvMapped, static_cast<uint32_t>(slot.width), static_cast<uint32_t>(slot.height),
                                              slot.timestamp);
        }

        if (yMapped) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, slot.yPbo);
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        }
        if (uvMapped) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, slot.uvPbo);
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        }

        glBindBuffer(GL_PIXEL_PACK_BUFFER, previousPackBuffer);

        if (glIsSync(slot.fence)) { glDeleteSync(slot.fence); }
        slot.fence = nullptr;
        slot.pending = false;
        slot.timestamp = 0;
        slot.width = 0;
        slot.height = 0;
        return true;
    }

    return false;
}

static bool SubmitSameThreadVirtualCameraFrameSync(GLuint srcTexture, int width, int height, uint64_t timestamp) {
    if (srcTexture == 0 || width <= 0 || height <= 0) { return false; }
    if (!EnsureSameThreadVirtualCameraSynchronousTargets(width, height)) { return false; }
    if (g_sameThreadVirtualCameraReadFBO == 0) { glGenFramebuffers(1, &g_sameThreadVirtualCameraReadFBO); }
    if (g_sameThreadVirtualCameraReadFBO == 0) { return false; }

    size_t yBytes = 0;
    size_t uvBytes = 0;
    size_t totalBytes = 0;
    std::string syncReason;
    if (!TryComputeVirtualCameraSyncBytes(width, height, yBytes, uvBytes, totalBytes, syncReason)) {
        LogVirtualCameraSyncGuardOnce(syncReason, width, height, totalBytes);
        return false;
    }
    if (totalBytes > kMaxVirtualCameraSyncBytes) {
        LogVirtualCameraSyncGuardOnce("requested buffer size exceeds guard limit of " + FormatByteCount(kMaxVirtualCameraSyncBytes),
                                      width, height, totalBytes);
        return false;
    }

    SameThreadVirtualCameraReadbackSlot syncSlot{};
    syncSlot.yTexture = g_sameThreadVirtualCameraLumaTexture;
    syncSlot.uvTexture = g_sameThreadVirtualCameraChromaTexture;
    syncSlot.textureWidth = width;
    syncSlot.textureHeight = height;
    if (!ConvertSameThreadVirtualCameraTextureToNv12(srcTexture, width, height, syncSlot)) { return false; }

    static std::vector<uint8_t> yPlane;
    static std::vector<uint8_t> uvPlane;
    yPlane.resize(yBytes);
    uvPlane.resize(uvBytes);

    GLint previousReadFbo = 0;
    GLint previousPackBuffer = 0;
    GLint previousPackAlignment = 0;
    GLint previousPackRowLength = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFbo);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previousPackBuffer);
    glGetIntegerv(GL_PACK_ALIGNMENT, &previousPackAlignment);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &previousPackRowLength);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_sameThreadVirtualCameraReadFBO);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);

    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_sameThreadVirtualCameraLumaTexture, 0);
    glReadPixels(0, 0, width, height, GL_RED, GL_UNSIGNED_BYTE, yPlane.data());

    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_sameThreadVirtualCameraChromaTexture, 0);
    glReadPixels(0, 0, width / 2, height / 2, GL_RG, GL_UNSIGNED_BYTE, uvPlane.data());

    glBindFramebuffer(GL_READ_FRAMEBUFFER, previousReadFbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, previousPackBuffer);
    glPixelStorei(GL_PACK_ROW_LENGTH, previousPackRowLength);
    glPixelStorei(GL_PACK_ALIGNMENT, previousPackAlignment);

    return WriteVirtualCameraFrameNV12Planes(yPlane.data(), uvPlane.data(), static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                                             timestamp);
}

static SameThreadVirtualCameraReadbackSlot* AcquireSameThreadVirtualCameraReadbackSlot() {
    for (int offset = 0; offset < SAME_THREAD_VIRTUAL_CAMERA_PBO_COUNT; ++offset) {
        const int slotIndex = (g_sameThreadVirtualCameraReadbackWriteIndex + offset) % SAME_THREAD_VIRTUAL_CAMERA_PBO_COUNT;
        auto& slot = g_sameThreadVirtualCameraReadbackSlots[slotIndex];
        if (!slot.pending) {
            g_sameThreadVirtualCameraReadbackWriteIndex = (slotIndex + 1) % SAME_THREAD_VIRTUAL_CAMERA_PBO_COUNT;
            return &slot;
        }
    }

    if (!HarvestSameThreadVirtualCameraReadback()) { return nullptr; }

    for (int offset = 0; offset < SAME_THREAD_VIRTUAL_CAMERA_PBO_COUNT; ++offset) {
        const int slotIndex = (g_sameThreadVirtualCameraReadbackWriteIndex + offset) % SAME_THREAD_VIRTUAL_CAMERA_PBO_COUNT;
        auto& slot = g_sameThreadVirtualCameraReadbackSlots[slotIndex];
        if (!slot.pending) {
            g_sameThreadVirtualCameraReadbackWriteIndex = (slotIndex + 1) % SAME_THREAD_VIRTUAL_CAMERA_PBO_COUNT;
            return &slot;
        }
    }

    return nullptr;
}
static std::mutex s_eyezoomTextMutex;

static void EnsureSharedVertexBufferCapacity(GLsizeiptr requiredBytes) {
    if (g_vbo == 0 || requiredBytes <= 0 || requiredBytes <= g_vboCapacityBytes) { return; }

    GLsizeiptr newCapacityBytes = g_vboCapacityBytes > 0 ? g_vboCapacityBytes : static_cast<GLsizeiptr>(sizeof(float) * 192);
    while (newCapacityBytes < requiredBytes) {
        newCapacityBytes *= 2;
    }

    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, newCapacityBytes, nullptr, GL_DYNAMIC_DRAW);
    g_vboCapacityBytes = newCapacityBytes;
}

struct TextureGridLabel {
    GLuint textureId;
    float x;
    float y;
    int tileSize;
    int width;
    int height;
    float sizeMB;
    GLenum internalFormat;
    GLint minFilter;
    GLint magFilter;
    GLint wrapS;
    GLint wrapT;
    bool isOurs;
};
static std::vector<TextureGridLabel> s_textureGridLabels;
static std::mutex s_textureGridMutex;

static ImFont* g_overlayTextFont = nullptr;
static float g_overlayTextFontSize = 24.0f;

static void CacheEyeZoomTextLabel(int number, float centerX, float centerY, float boxWidth, float boxHeight,
                                                                    EyeZoomFontSizeMode fontSizeMode,
                                  const Color& color, float clipMinX, float clipMinY, float clipMaxX, float clipMaxY) {
    std::lock_guard<std::mutex> lock(s_eyezoomTextMutex);
    s_eyezoomTextLabels.push_back({ number, centerX, centerY, boxWidth, boxHeight, clipMinX, clipMinY, clipMaxX, clipMaxY,
                                                                        fontSizeMode, color });
}

void DrawOverlayBorder(float nx1, float ny1, float nx2, float ny2, float borderWidth, float borderHeight, bool isDragging,
                       bool drawCorners = false) {
    glUseProgram(g_solidColorProgram);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if (isDragging) {
        glUniform4f(g_solidColorShaderLocs.color, 0.0f, 1.0f, 0.0f, 0.8f);
    } else {
        glUniform4f(g_solidColorShaderLocs.color, 1.0f, 1.0f, 0.0f, 0.6f);
    }

    float allBorders[24 * 4] = {
                                 nx1, ny2 - borderHeight, 0, 0, nx2, ny2 - borderHeight, 0, 0, nx2, ny2, 0, 0, nx1, ny2 - borderHeight, 0,
                                 0, nx2, ny2, 0, 0, nx1, ny2, 0, 0,

                                 nx1, ny1, 0, 0, nx2, ny1, 0, 0, nx2, ny1 + borderHeight, 0, 0, nx1, ny1, 0, 0, nx2, ny1 + borderHeight, 0,
                                 0, nx1, ny1 + borderHeight, 0, 0,

                                 nx1, ny1, 0, 0, nx1 + borderWidth, ny1, 0, 0, nx1 + borderWidth, ny2, 0, 0, nx1, ny1, 0, 0,
                                 nx1 + borderWidth, ny2, 0, 0, nx1, ny2, 0, 0,

                                 nx2 - borderWidth, ny1, 0, 0, nx2, ny1, 0, 0, nx2, ny2, 0, 0, nx2 - borderWidth, ny1, 0, 0, nx2, ny2, 0, 0,
                                 nx2 - borderWidth, ny2, 0, 0
    };

    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(allBorders), allBorders);
    glDrawArrays(GL_TRIANGLES, 0, 24);

                g_vboCapacityBytes = 0;
    if (drawCorners) {
        float cornerSize = borderWidth * 2.5f;
        float cornerSizeH = borderHeight * 2.5f;

        glUniform4f(g_solidColorShaderLocs.color, 1.0f, 0.5f, 0.0f, 0.9f);

        float allCorners[24 * 4] = {
                                     nx1, ny2 - cornerSizeH, 0, 0, nx1 + cornerSize, ny2 - cornerSizeH, 0, 0, nx1 + cornerSize, ny2, 0, 0,
                                     nx1, ny2 - cornerSizeH, 0, 0, nx1 + cornerSize, ny2, 0, 0, nx1, ny2, 0, 0,

                                     nx2 - cornerSize, ny2 - cornerSizeH, 0, 0, nx2, ny2 - cornerSizeH, 0, 0, nx2, ny2, 0, 0,
                                     nx2 - cornerSize, ny2 - cornerSizeH, 0, 0, nx2, ny2, 0, 0, nx2 - cornerSize, ny2, 0, 0,

                                     nx1, ny1, 0, 0, nx1 + cornerSize, ny1, 0, 0, nx1 + cornerSize, ny1 + cornerSizeH, 0, 0, nx1, ny1, 0, 0,
                                     nx1 + cornerSize, ny1 + cornerSizeH, 0, 0, nx1, ny1 + cornerSizeH, 0, 0,

                                     nx2 - cornerSize, ny1, 0, 0, nx2, ny1, 0, 0, nx2, ny1 + cornerSizeH, 0, 0, nx2 - cornerSize, ny1, 0, 0,
                                     nx2, ny1 + cornerSizeH, 0, 0, nx2 - cornerSize, ny1 + cornerSizeH, 0, 0
        };

        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(allCorners), allCorners);
        glDrawArrays(GL_TRIANGLES, 0, 24);
    }

    glDisable(GL_BLEND);
}

void RenderGameBorder(int x, int y, int w, int h, int borderWidth, int radius, const Color& color, int fullW, int fullH) {
    if (borderWidth <= 0) return;

    glUseProgram(g_solidColorProgram);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);

    glUniform4f(g_solidColorShaderLocs.color, color.r, color.g, color.b, 1.0f);

    int y_gl = fullH - y - h;

    int outerLeft = x - borderWidth;
    int outerRight = x + w + borderWidth;
    int outerBottom = y_gl - borderWidth;
    int outerTop = y_gl + h + borderWidth;

    int effectiveRadius = radius;
    int maxRadius = (w < h ? w : h) / 2 + borderWidth;
    if (effectiveRadius > maxRadius) effectiveRadius = maxRadius;

    auto toNdcX = [fullW](int px) { return (static_cast<float>(px) / fullW) * 2.0f - 1.0f; };
    auto toNdcY = [fullH](int py) { return (static_cast<float>(py) / fullH) * 2.0f - 1.0f; };

    if (effectiveRadius <= 0) {
        float allBorders[] = {
            toNdcX(outerLeft),  toNdcY(y_gl + h), 0, 0, toNdcX(outerRight), toNdcY(y_gl + h), 0, 0,
            toNdcX(outerRight), toNdcY(outerTop),  0, 0, toNdcX(outerLeft),  toNdcY(y_gl + h), 0, 0,
            toNdcX(outerRight), toNdcY(outerTop),  0, 0, toNdcX(outerLeft),  toNdcY(outerTop),  0, 0,
            toNdcX(outerLeft),  toNdcY(outerBottom), 0, 0, toNdcX(outerRight), toNdcY(outerBottom), 0, 0,
            toNdcX(outerRight), toNdcY(y_gl),        0, 0, toNdcX(outerLeft),  toNdcY(outerBottom), 0, 0,
            toNdcX(outerRight), toNdcY(y_gl),        0, 0, toNdcX(outerLeft),  toNdcY(y_gl),        0, 0,
            toNdcX(outerLeft), toNdcY(y_gl),     0, 0, toNdcX(x),         toNdcY(y_gl),     0, 0,
            toNdcX(x),         toNdcY(y_gl + h), 0, 0, toNdcX(outerLeft), toNdcY(y_gl),     0, 0,
            toNdcX(x),         toNdcY(y_gl + h), 0, 0, toNdcX(outerLeft), toNdcY(y_gl + h), 0, 0,
            toNdcX(x + w),      toNdcY(y_gl),     0, 0, toNdcX(outerRight), toNdcY(y_gl),     0, 0,
            toNdcX(outerRight), toNdcY(y_gl + h), 0, 0, toNdcX(x + w),      toNdcY(y_gl),     0, 0,
            toNdcX(outerRight), toNdcY(y_gl + h), 0, 0, toNdcX(x + w),      toNdcY(y_gl + h), 0, 0
        };
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(allBorders), allBorders);
        glDrawArrays(GL_TRIANGLES, 0, 24);
    } else {
        int segments = 8;

        float straightBorders[] = {
            toNdcX(x + effectiveRadius),     toNdcY(y_gl + h), 0, 0, toNdcX(x + w - effectiveRadius), toNdcY(y_gl + h), 0, 0,
            toNdcX(x + w - effectiveRadius), toNdcY(outerTop), 0, 0, toNdcX(x + effectiveRadius),     toNdcY(y_gl + h), 0, 0,
            toNdcX(x + w - effectiveRadius), toNdcY(outerTop), 0, 0, toNdcX(x + effectiveRadius),     toNdcY(outerTop), 0, 0,
            toNdcX(x + effectiveRadius),     toNdcY(outerBottom), 0, 0, toNdcX(x + w - effectiveRadius), toNdcY(outerBottom), 0, 0,
            toNdcX(x + w - effectiveRadius), toNdcY(y_gl),        0, 0, toNdcX(x + effectiveRadius),     toNdcY(outerBottom), 0, 0,
            toNdcX(x + w - effectiveRadius), toNdcY(y_gl),        0, 0, toNdcX(x + effectiveRadius),     toNdcY(y_gl),        0, 0,
            toNdcX(outerLeft), toNdcY(y_gl + effectiveRadius),     0, 0, toNdcX(x),         toNdcY(y_gl + effectiveRadius),     0, 0,
            toNdcX(x),         toNdcY(y_gl + h - effectiveRadius), 0, 0, toNdcX(outerLeft), toNdcY(y_gl + effectiveRadius),     0, 0,
            toNdcX(x),         toNdcY(y_gl + h - effectiveRadius), 0, 0, toNdcX(outerLeft), toNdcY(y_gl + h - effectiveRadius), 0, 0,
            toNdcX(x + w),      toNdcY(y_gl + effectiveRadius),     0, 0, toNdcX(outerRight), toNdcY(y_gl + effectiveRadius),     0, 0,
            toNdcX(outerRight), toNdcY(y_gl + h - effectiveRadius), 0, 0, toNdcX(x + w),      toNdcY(y_gl + effectiveRadius),     0, 0,
            toNdcX(outerRight), toNdcY(y_gl + h - effectiveRadius), 0, 0, toNdcX(x + w),      toNdcY(y_gl + h - effectiveRadius), 0, 0
        };
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(straightBorders), straightBorders);
        glDrawArrays(GL_TRIANGLES, 0, 24);

        auto renderCornerArc = [&](float centerX, float centerY, float innerR, float outerR, float startAngle, float endAngle) {
            float angleStep = (endAngle - startAngle) / segments;
            std::vector<float> arcVerts;
            arcVerts.reserve(segments * 6 * 4);
            for (int s = 0; s < segments; s++) {
                float a1 = startAngle + s * angleStep;
                float a2 = startAngle + (s + 1) * angleStep;

                float c1 = cosf(a1), s1 = sinf(a1);
                float c2 = cosf(a2), s2 = sinf(a2);

                float tri[] = {
                    toNdcX((int)(centerX + innerR * c1)), toNdcY((int)(centerY + innerR * s1)), 0, 0,
                    toNdcX((int)(centerX + outerR * c1)), toNdcY((int)(centerY + outerR * s1)), 0, 0,
                    toNdcX((int)(centerX + outerR * c2)), toNdcY((int)(centerY + outerR * s2)), 0, 0,
                    toNdcX((int)(centerX + innerR * c1)), toNdcY((int)(centerY + innerR * s1)), 0, 0,
                    toNdcX((int)(centerX + outerR * c2)), toNdcY((int)(centerY + outerR * s2)), 0, 0,
                    toNdcX((int)(centerX + innerR * c2)), toNdcY((int)(centerY + innerR * s2)), 0, 0
                };
                arcVerts.insert(arcVerts.end(), std::begin(tri), std::end(tri));
            }
            EnsureSharedVertexBufferCapacity(static_cast<GLsizeiptr>(arcVerts.size() * sizeof(float)));
            glBufferSubData(GL_ARRAY_BUFFER, 0, arcVerts.size() * sizeof(float), arcVerts.data());
            glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(arcVerts.size() / 4));
        };

        const float PI = 3.14159265358979323846f;
        float innerR = (float)effectiveRadius;
        float outerR = (float)(effectiveRadius + borderWidth);

        renderCornerArc((float)(x + effectiveRadius), (float)(y_gl + h - effectiveRadius), innerR, outerR, PI * 0.5f, PI);

        renderCornerArc((float)(x + w - effectiveRadius), (float)(y_gl + h - effectiveRadius), innerR, outerR, 0.0f, PI * 0.5f);

        renderCornerArc((float)(x + effectiveRadius), (float)(y_gl + effectiveRadius), innerR, outerR, PI, PI * 1.5f);

        renderCornerArc((float)(x + w - effectiveRadius), (float)(y_gl + effectiveRadius), innerR, outerR, PI * 1.5f, PI * 2.0f);
    }
}

static float GetViewportRelativeImageScale(float scaleX, float scaleY) {
    if (scaleX > 0.0f && scaleY > 0.0f) {
        return (std::min)(scaleX, scaleY);
    }
    if (scaleX > 0.0f) {
        return scaleX;
    }
    if (scaleY > 0.0f) {
        return scaleY;
    }
    return 1.0f;
}

static void ScaleViewportRelativeImageSize(int baseW, int baseH, bool relativeStretching, float scaleX, float scaleY, int& outW,
                                           int& outH) {
    if (!relativeStretching) {
        outW = baseW;
        outH = baseH;
        return;
    }

    const float uniformScale = GetViewportRelativeImageScale(scaleX, scaleY);
    outW = (std::max)(1, static_cast<int>(baseW * uniformScale));
    outH = (std::max)(1, static_cast<int>(baseH * uniformScale));
}

static void ResolveConfiguredImageDimensions(const ImageConfig& img, int sourceWidth, int sourceHeight, int& outW, int& outH) {
    auto c = ResolveCrop(img.crop_top, img.crop_bottom, img.crop_left, img.crop_right,
                         img.cropToWidth, img.cropToHeight, sourceWidth, sourceHeight);
    int croppedWidth = sourceWidth - c.left - c.right;
    int croppedHeight = sourceHeight - c.top - c.bottom;
    croppedWidth = (std::max)(1, croppedWidth);
    croppedHeight = (std::max)(1, croppedHeight);

    if (!img.relativeSizing && img.width > 0 && img.height > 0) {
        outW = (std::max)(1, img.width);
        outH = (std::max)(1, img.height);
        return;
    }

    outW = (std::max)(1, static_cast<int>(croppedWidth * img.scale));
    outH = (std::max)(1, static_cast<int>(croppedHeight * img.scale));
}

void CalculateImageDimensions(const ImageConfig& img, int& outW, int& outH) {
    if (!img.relativeSizing && img.width > 0 && img.height > 0) {
        outW = (std::max)(1, img.width);
        outH = (std::max)(1, img.height);
        return;
    }

    // NOTE: This is used in UI/drag hit-testing; avoid blocking if another thread is updating textures.
    std::unique_lock<std::mutex> lock(g_userImagesMutex, std::try_to_lock);
    if (lock.owns_lock()) {
        auto it = g_userImages.find(img.name);
        if (it != g_userImages.end() && it->second.textureId != 0) {
            ResolveConfiguredImageDimensions(img, it->second.width, it->second.height, outW, outH);
            return;
        }
    }

    // Default size if texture not loaded / mutex busy
    outW = static_cast<int>(100 * img.scale);
    outH = static_cast<int>(100 * img.scale);
    if (outW < 1) outW = 1;
    if (outH < 1) outH = 1;
}

// Helper to calculate dimensions when mutex is already held (faster path)
static void CalculateWindowOverlayDimensionsUnsafe(const WindowOverlayConfig& overlay, int& outW, int& outH) {
    // NOTE: Caller must hold g_windowOverlayCacheMutex
    auto it = g_windowOverlayCache.find(overlay.name);
    if (it != g_windowOverlayCache.end() && it->second) {
        int texWidth = it->second->glTextureWidth;
        int texHeight = it->second->glTextureHeight;
        auto cc = ResolveCrop(overlay.crop_top, overlay.crop_bottom, overlay.crop_left, overlay.crop_right,
                              overlay.cropToWidth, overlay.cropToHeight, texWidth, texHeight);
        int croppedW = (std::max)(1, texWidth - cc.left - cc.right);
        int croppedH = (std::max)(1, texHeight - cc.top - cc.bottom);
        outW = static_cast<int>(croppedW * overlay.scale);
        outH = static_cast<int>(croppedH * overlay.scale);
    } else {
        outW = static_cast<int>(100 * overlay.scale);
        outH = static_cast<int>(100 * overlay.scale);
    }
}

static void CalculateWindowOverlayDimensions(const WindowOverlayConfig& overlay, int& outW, int& outH) {
    // Use try_lock to avoid blocking during hover detection
    std::unique_lock<std::mutex> lock(g_windowOverlayCacheMutex, std::try_to_lock);
    if (lock.owns_lock()) {
        CalculateWindowOverlayDimensionsUnsafe(overlay, outW, outH);
    } else {
        // Default size if mutex is busy
        outW = static_cast<int>(100 * overlay.scale);
        outH = static_cast<int>(100 * overlay.scale);
    }
}

const char* solid_vert_shader = R"(#version 330 core
layout(location = 0) in vec2 aPos;
void main() {
    gl_Position = vec4(aPos.x, aPos.y, 0.0, 1.0);
})";

const char* passthrough_vert_shader = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
out vec2 TexCoord;
out vec2 BaseTexCoord;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    BaseTexCoord = aTexCoord;
    TexCoord = aTexCoord;
})";

const char* filter_vert_shader = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
out vec2 TexCoord;
out vec2 BaseTexCoord;
uniform vec4 u_sourceRect;

void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    BaseTexCoord = aTexCoord;
    TexCoord = u_sourceRect.xy + aTexCoord * u_sourceRect.zw;
})";

const char* filter_frag_shader = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;
uniform sampler2D screenTexture;
uniform vec3 targetColor;
uniform vec3 outputColor;
uniform float u_sensitivity;

void main() {
    vec3 screenColorSRGB = texture(screenTexture, TexCoord).rgb;
    vec3 screenColorLinear = pow(screenColorSRGB, vec3(2.2));
    vec3 targetColorLinear = pow(targetColor, vec3(2.2));

    if (distance(screenColorLinear, targetColorLinear) < u_sensitivity) {
        FragColor = vec4(outputColor, 1.0);
    } else {
        FragColor = vec4(0.0, 0.0, 0.0, 0.0);
    }
})";

const char* render_frag_shader = R"(#version 330 core
    out vec4 FragColor;
    in vec2 TexCoord;

    uniform sampler2D filterTexture;
    uniform int u_borderWidth;
    uniform vec4 u_outputColor;
    uniform vec4 u_borderColor;
    uniform vec2 u_screenPixel;

    bool hasBorderSample(vec2 coord, vec2 pixel) {
        return texture(filterTexture, coord + vec2(-pixel.x, -pixel.y)).a > 0.001 ||
               texture(filterTexture, coord + vec2(0.0, -pixel.y)).a > 0.001 ||
               texture(filterTexture, coord + vec2(pixel.x, -pixel.y)).a > 0.001 ||
               texture(filterTexture, coord + vec2(-pixel.x, 0.0)).a > 0.001 ||
               texture(filterTexture, coord + vec2(pixel.x, 0.0)).a > 0.001 ||
               texture(filterTexture, coord + vec2(-pixel.x, pixel.y)).a > 0.001 ||
               texture(filterTexture, coord + vec2(0.0, pixel.y)).a > 0.001 ||
               texture(filterTexture, coord + vec2(pixel.x, pixel.y)).a > 0.001;
    }

    void main() {
        float centerAlpha = texture(filterTexture, TexCoord).a;

        if (centerAlpha > 0.001) {
            FragColor = u_outputColor;
            return;
        }

        if (u_borderWidth == 1) {
            if (hasBorderSample(TexCoord, u_screenPixel)) {
                FragColor = u_borderColor;
                return;
            }
            discard;
        }

        
        for (int x = -u_borderWidth; x <= u_borderWidth; x++) {
            for (int y = -u_borderWidth; y <= u_borderWidth; y++) {
                
                if (x == 0 && y == 0) continue;

                vec2 offset = vec2(float(x), float(y)) * u_screenPixel;
                float alpha = texture(filterTexture, TexCoord + offset).a;

                if (alpha > 0.001) {
                    FragColor = u_borderColor;
                    return;
                }
            }
        }

        discard;
    })";

const char* render_passthrough_frag_shader = R"(#version 330 core
    out vec4 FragColor;
    in vec2 TexCoord;

    uniform sampler2D filterTexture;
    uniform int u_borderWidth;
    uniform vec4 u_borderColor;
    uniform vec2 u_screenPixel;
    uniform float u_opacity;

    bool hasBorderSample(vec2 coord, vec2 pixel) {
        return texture(filterTexture, coord + vec2(-pixel.x, -pixel.y)).a > 0.001 ||
               texture(filterTexture, coord + vec2(0.0, -pixel.y)).a > 0.001 ||
               texture(filterTexture, coord + vec2(pixel.x, -pixel.y)).a > 0.001 ||
               texture(filterTexture, coord + vec2(-pixel.x, 0.0)).a > 0.001 ||
               texture(filterTexture, coord + vec2(pixel.x, 0.0)).a > 0.001 ||
               texture(filterTexture, coord + vec2(-pixel.x, pixel.y)).a > 0.001 ||
               texture(filterTexture, coord + vec2(0.0, pixel.y)).a > 0.001 ||
               texture(filterTexture, coord + vec2(pixel.x, pixel.y)).a > 0.001;
    }

    void main() {
        vec4 centerColor = texture(filterTexture, TexCoord);

        if (centerColor.a > 0.001) {
            FragColor = vec4(centerColor.rgb, u_opacity);
            return;
        }

        if (u_borderWidth == 1) {
            if (hasBorderSample(TexCoord, u_screenPixel)) {
                FragColor = u_borderColor;
                return;
            }
            discard;
        }

        for (int x = -u_borderWidth; x <= u_borderWidth; x++) {
            for (int y = -u_borderWidth; y <= u_borderWidth; y++) {
                if (x == 0 && y == 0) continue;

                vec2 offset = vec2(float(x), float(y)) * u_screenPixel;
                float alpha = texture(filterTexture, TexCoord + offset).a;

                if (alpha > 0.001) {
                    FragColor = u_borderColor;
                    return;
                }
            }
        }

        discard;
    })";

const char* background_frag_shader = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;
uniform sampler2D backgroundTexture;
uniform float u_opacity;
void main() {
    vec4 texColor = texture(backgroundTexture, TexCoord);
    FragColor = vec4(texColor.rgb, texColor.a * u_opacity);
})";

const char* solid_color_frag_shader = R"(#version 330 core
out vec4 FragColor;
uniform vec4 u_color;
void main() {
    FragColor = u_color;
})";

const char* image_render_frag_shader = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;

#define MAX_COLOR_KEYS 8

uniform sampler2D imageTexture;
uniform bool u_enableColorKey;
uniform int u_numColorKeys;
uniform vec3 u_colorKeys[MAX_COLOR_KEYS];
uniform float u_sensitivities[MAX_COLOR_KEYS];
uniform float u_opacity;

void main() {
    vec4 texColor = texture(imageTexture, TexCoord);

    if (u_enableColorKey) {
        vec3 linearTexColor = pow(texColor.rgb, vec3(2.2));
        for (int i = 0; i < u_numColorKeys; i++) {
            vec3 linearKeyColor = pow(u_colorKeys[i], vec3(2.2));
            float dist = distance(linearTexColor, linearKeyColor);
            if (dist < u_sensitivities[i]) {
                discard;
            }
        }
    }
    
    FragColor = vec4(texColor.rgb, texColor.a * u_opacity);
})";

const char* static_border_frag_shader = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;
uniform int u_shape;
uniform vec4 u_borderColor;
uniform float u_thickness;
uniform float u_radius;
uniform vec2 u_size;
uniform vec2 u_quadSize;

float sdRoundedBox(vec2 p, vec2 b, float r) {
    float maxR = min(b.x, b.y);
    r = clamp(r, 0.0, maxR);
    vec2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

float sdEllipse(vec2 p, vec2 ab) {
    vec2 pn = p / ab;
    float len = length(pn);
    if (len < 0.0001) return -min(ab.x, ab.y);

    float d = len - 1.0;
    vec2 grad = pn / (ab * len);
    float gradLen = length(grad);
    return d / gradLen;
}

void main() {
    vec2 pixelPos = TexCoord * u_quadSize;
    vec2 centeredPixelPos = pixelPos - u_quadSize * 0.5;
    vec2 halfSize = max(u_size * 0.5, vec2(1.0, 1.0));

    float dist;
    if (u_shape == 0) {
        dist = sdRoundedBox(centeredPixelPos, halfSize, u_radius);
    } else {
        dist = sdEllipse(centeredPixelPos, halfSize);
    }

    float epsilon = 0.5;
    if (dist >= -epsilon && dist <= u_thickness + epsilon) {
        FragColor = u_borderColor;
    } else {
        discard;
    }
})";

const char* virtual_camera_nv12_frag_shader = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;

uniform sampler2D screenTexture;
uniform vec2 u_sourceTexelSize;
uniform int u_outputMode;
uniform int u_colorSpaceMode;

const int COLOR_SPACE_BT601 = 0;
const int COLOR_SPACE_BT709 = 1;

vec3 sampleRgb(vec2 uv) {
    return texture(screenTexture, vec2(uv.x, 1.0 - uv.y)).rgb;
}

float encodeY(vec3 rgb) {
    vec3 coeffs = (u_colorSpaceMode == COLOR_SPACE_BT709)
        ? vec3(47.0 / 256.0, 157.0 / 256.0, 16.0 / 256.0)
        : vec3(66.0 / 256.0, 129.0 / 256.0, 25.0 / 256.0);
    return clamp(dot(rgb, coeffs) + (16.0 / 255.0), 0.0, 1.0);
}

vec2 encodeUV(vec3 rgb) {
    vec3 uCoeffs = (u_colorSpaceMode == COLOR_SPACE_BT709)
        ? vec3(-26.0 / 256.0, -87.0 / 256.0, 112.0 / 256.0)
        : vec3(-38.0 / 256.0, -74.0 / 256.0, 112.0 / 256.0);
    vec3 vCoeffs = (u_colorSpaceMode == COLOR_SPACE_BT709)
        ? vec3(112.0 / 256.0, -102.0 / 256.0, -10.0 / 256.0)
        : vec3(112.0 / 256.0, -94.0 / 256.0, -18.0 / 256.0);
    float u = dot(rgb, uCoeffs) + (128.0 / 255.0);
    float v = dot(rgb, vCoeffs) + (128.0 / 255.0);
    return clamp(vec2(u, v), 0.0, 1.0);
}

void main() {
    if (u_outputMode == 0) {
        FragColor = vec4(encodeY(sampleRgb(TexCoord)), 0.0, 0.0, 1.0);
        return;
    }

    vec2 srcPixel = floor(gl_FragCoord.xy - vec2(0.5)) * 2.0;
    vec2 uv00 = (srcPixel + vec2(0.5, 0.5)) * u_sourceTexelSize;
    vec2 uv10 = (srcPixel + vec2(1.5, 0.5)) * u_sourceTexelSize;
    vec2 uv01 = (srcPixel + vec2(0.5, 1.5)) * u_sourceTexelSize;
    vec2 uv11 = (srcPixel + vec2(1.5, 1.5)) * u_sourceTexelSize;

    vec3 avgRgb = 0.25 * (sampleRgb(uv00) + sampleRgb(uv10) + sampleRgb(uv01) + sampleRgb(uv11));
    vec2 uv = encodeUV(avgRgb);
    FragColor = vec4(uv, 0.0, 1.0);
}
)";

const char* passthrough_frag_shader = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;
in vec2 BaseTexCoord;
uniform sampler2D screenTexture;
uniform vec4 u_sourceRect;
uniform float u_opacity;
uniform vec2 u_sourceTexelSize;
uniform vec2 u_sourcePixelSize;
uniform int u_snapToSourcePixels;

void main() {
    vec2 sampleCoord = TexCoord;
    if (u_snapToSourcePixels != 0) {
        vec2 sourcePixel = floor(BaseTexCoord * u_sourcePixelSize);
        vec2 sourcePixelMax = max(u_sourcePixelSize - vec2(1.0), vec2(0.0));
        sourcePixel = clamp(sourcePixel, vec2(0.0), sourcePixelMax);
        sampleCoord = u_sourceRect.xy + (sourcePixel + vec2(0.5)) * u_sourceTexelSize;
    }
    FragColor = vec4(texture(screenTexture, sampleCoord).rgb, u_opacity);
})";

const char* gradient_frag_shader = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;

#define MAX_STOPS 8
#define ANIM_NONE 0
#define ANIM_ROTATE 1
#define ANIM_SLIDE 2
#define ANIM_WAVE 3
#define ANIM_SPIRAL 4
#define ANIM_FADE 5

uniform int u_numStops;
uniform vec4 u_stopColors[MAX_STOPS];
uniform float u_stopPositions[MAX_STOPS];
uniform float u_angle;
uniform float u_time;
uniform int u_animationType;
uniform float u_animationSpeed;
uniform bool u_colorFade;

vec4 getGradientColorSeamless(float t) {
    t = fract(t);
    
    
    float lastPos = u_stopPositions[u_numStops - 1];
    float firstPos = u_stopPositions[0];
    float wrapSize = (1.0 - lastPos) + firstPos;
    
    if (t <= firstPos && wrapSize > 0.001) {
        float wrapT = (firstPos - t) / wrapSize;
        return mix(u_stopColors[0], u_stopColors[u_numStops - 1], wrapT);
    }
    else if (t >= lastPos && wrapSize > 0.001) {
        float wrapT = (t - lastPos) / wrapSize;
        return mix(u_stopColors[u_numStops - 1], u_stopColors[0], wrapT);
    }
    
    vec4 color = u_stopColors[0];
    for (int i = 0; i < u_numStops - 1; i++) {
        if (t >= u_stopPositions[i] && t <= u_stopPositions[i + 1]) {
            float segmentT = (t - u_stopPositions[i]) / max(u_stopPositions[i + 1] - u_stopPositions[i], 0.0001);
            color = mix(u_stopColors[i], u_stopColors[i + 1], segmentT);
            break;
        }
    }
    return color;
}

vec4 getGradientColor(float t, float timeOffset) {
    float adjustedT = t;
    if (u_colorFade) {
        adjustedT = fract(t + timeOffset * 0.1);
    }
    adjustedT = clamp(adjustedT, 0.0, 1.0);
    
    vec4 color = u_stopColors[0];
    for (int i = 0; i < u_numStops - 1; i++) {
        if (adjustedT >= u_stopPositions[i] && adjustedT <= u_stopPositions[i + 1]) {
            float segmentT = (adjustedT - u_stopPositions[i]) / max(u_stopPositions[i + 1] - u_stopPositions[i], 0.0001);
            color = mix(u_stopColors[i], u_stopColors[i + 1], segmentT);
            break;
        }
    }
    if (adjustedT >= u_stopPositions[u_numStops - 1]) {
        color = u_stopColors[u_numStops - 1];
    }
    return color;
}

vec4 getFadeColor(float timeOffset) {
    float cyclePos = fract(timeOffset * 0.1);
    
    vec4 color = u_stopColors[0];
    for (int i = 0; i < u_numStops - 1; i++) {
        if (cyclePos >= u_stopPositions[i] && cyclePos <= u_stopPositions[i + 1]) {
            float segmentT = (cyclePos - u_stopPositions[i]) / max(u_stopPositions[i + 1] - u_stopPositions[i], 0.0001);
            color = mix(u_stopColors[i], u_stopColors[i + 1], segmentT);
            break;
        }
    }
    if (cyclePos > u_stopPositions[u_numStops - 1]) {
        float wrapRange = 1.0 - u_stopPositions[u_numStops - 1] + u_stopPositions[0];
        float wrapT = (cyclePos - u_stopPositions[u_numStops - 1]) / max(wrapRange, 0.0001);
        color = mix(u_stopColors[u_numStops - 1], u_stopColors[0], wrapT);
    }
    else if (cyclePos < u_stopPositions[0]) {
        float wrapRange = 1.0 - u_stopPositions[u_numStops - 1] + u_stopPositions[0];
        float wrapT = (u_stopPositions[0] - cyclePos) / max(wrapRange, 0.0001);
        color = mix(u_stopColors[0], u_stopColors[u_numStops - 1], wrapT);
    }
    return color;
}

void main() {
    vec2 center = vec2(0.5, 0.5);
    vec2 uv = TexCoord - center;
    float effectiveAngle = u_angle;
    float t = 0.0;
    float timeOffset = u_time * u_animationSpeed;
    
    if (u_animationType == ANIM_NONE) {
        vec2 dir = vec2(cos(u_angle), sin(u_angle));
        t = dot(uv, dir) + 0.5;
        t = clamp(t, 0.0, 1.0);
        FragColor = getGradientColor(t, timeOffset);
    }
    else if (u_animationType == ANIM_ROTATE) {
        effectiveAngle = u_angle + timeOffset;
        vec2 dir = vec2(cos(effectiveAngle), sin(effectiveAngle));
        t = dot(uv, dir) + 0.5;
        t = clamp(t, 0.0, 1.0);
        FragColor = getGradientColor(t, timeOffset);
    }
    else if (u_animationType == ANIM_SLIDE) {
        vec2 dir = vec2(cos(u_angle), sin(u_angle));
        t = dot(uv, dir) + 0.5;
        t = t + timeOffset * 0.2;
        FragColor = getGradientColorSeamless(t);
    }
    else if (u_animationType == ANIM_WAVE) {
        vec2 dir = vec2(cos(u_angle), sin(u_angle));
        vec2 perpDir = vec2(-sin(u_angle), cos(u_angle));
        float perpPos = dot(uv, perpDir);
        float wave = sin(perpPos * 8.0 + timeOffset * 2.0) * 0.08;
        t = dot(uv, dir) + 0.5 + wave;
        t = clamp(t, 0.0, 1.0);
        FragColor = getGradientColor(t, timeOffset);
    }
    else if (u_animationType == ANIM_SPIRAL) {
        float dist = length(uv) * 2.0;
        float angle = atan(uv.y, uv.x);
        t = dist + angle / 6.28318 - timeOffset * 0.3;
        FragColor = getGradientColorSeamless(t);
    }
    else if (u_animationType == ANIM_FADE) {
        FragColor = getFadeColor(timeOffset);
    }
    else {
        t = clamp(t, 0.0, 1.0);
        FragColor = getGradientColor(t, timeOffset);
    }
})";

void InitializeShaders() {
    PROFILE_SCOPE_CAT("Shader Initialization", "GPU Operations");
    g_filterProgram = CreateShaderProgram(filter_vert_shader, filter_frag_shader);
    g_renderProgram = CreateShaderProgram(passthrough_vert_shader, render_frag_shader);
    g_renderPassthroughProgram = CreateShaderProgram(passthrough_vert_shader, render_passthrough_frag_shader);
    g_backgroundProgram = CreateShaderProgram(passthrough_vert_shader, background_frag_shader);
    g_solidColorProgram = CreateShaderProgram(solid_vert_shader, solid_color_frag_shader);
    g_imageRenderProgram = CreateShaderProgram(passthrough_vert_shader, image_render_frag_shader);
    g_passthroughProgram = CreateShaderProgram(filter_vert_shader, passthrough_frag_shader);
    g_gradientProgram = CreateShaderProgram(passthrough_vert_shader, gradient_frag_shader);
    g_staticBorderProgram = CreateShaderProgram(passthrough_vert_shader, static_border_frag_shader);
    g_virtualCameraNv12Program = CreateShaderProgram(passthrough_vert_shader, virtual_camera_nv12_frag_shader);

    if (!g_filterProgram || !g_renderProgram || !g_renderPassthroughProgram || !g_backgroundProgram || !g_solidColorProgram ||
        !g_imageRenderProgram || !g_passthroughProgram || !g_gradientProgram || !g_staticBorderProgram ||
        !g_virtualCameraNv12Program) {
        Log("FATAL: Failed to create one or more shader programs. Aborting shader initialization.");
        return;
    }

    g_filterShaderLocs.screenTexture = glGetUniformLocation(g_filterProgram, "screenTexture");
    g_filterShaderLocs.targetColor = glGetUniformLocation(g_filterProgram, "targetColor");
    g_filterShaderLocs.outputColor = glGetUniformLocation(g_filterProgram, "outputColor");
    g_filterShaderLocs.sensitivity = glGetUniformLocation(g_filterProgram, "u_sensitivity");
    g_filterShaderLocs.sourceRect = glGetUniformLocation(g_filterProgram, "u_sourceRect");

    g_renderShaderLocs.filterTexture = glGetUniformLocation(g_renderProgram, "filterTexture");
    g_renderShaderLocs.borderWidth = glGetUniformLocation(g_renderProgram, "u_borderWidth");
    g_renderShaderLocs.outputColor = glGetUniformLocation(g_renderProgram, "u_outputColor");
    g_renderShaderLocs.borderColor = glGetUniformLocation(g_renderProgram, "u_borderColor");
    g_renderShaderLocs.screenPixel = glGetUniformLocation(g_renderProgram, "u_screenPixel");

    g_renderPassthroughShaderLocs.filterTexture = glGetUniformLocation(g_renderPassthroughProgram, "filterTexture");
    g_renderPassthroughShaderLocs.borderWidth = glGetUniformLocation(g_renderPassthroughProgram, "u_borderWidth");
    g_renderPassthroughShaderLocs.borderColor = glGetUniformLocation(g_renderPassthroughProgram, "u_borderColor");
    g_renderPassthroughShaderLocs.screenPixel = glGetUniformLocation(g_renderPassthroughProgram, "u_screenPixel");
    g_renderPassthroughShaderLocs.opacity = glGetUniformLocation(g_renderPassthroughProgram, "u_opacity");

    g_backgroundShaderLocs.backgroundTexture = glGetUniformLocation(g_backgroundProgram, "backgroundTexture");
    g_backgroundShaderLocs.opacity = glGetUniformLocation(g_backgroundProgram, "u_opacity");

    g_solidColorShaderLocs.color = glGetUniformLocation(g_solidColorProgram, "u_color");

    g_imageRenderShaderLocs.imageTexture = glGetUniformLocation(g_imageRenderProgram, "imageTexture");
    g_imageRenderShaderLocs.enableColorKey = glGetUniformLocation(g_imageRenderProgram, "u_enableColorKey");
    g_imageRenderShaderLocs.numColorKeys = glGetUniformLocation(g_imageRenderProgram, "u_numColorKeys");
    g_imageRenderShaderLocs.colorKeys = glGetUniformLocation(g_imageRenderProgram, "u_colorKeys");
    g_imageRenderShaderLocs.sensitivities = glGetUniformLocation(g_imageRenderProgram, "u_sensitivities");
    g_imageRenderShaderLocs.opacity = glGetUniformLocation(g_imageRenderProgram, "u_opacity");

    g_staticBorderShaderLocs.shape = glGetUniformLocation(g_staticBorderProgram, "u_shape");
    g_staticBorderShaderLocs.borderColor = glGetUniformLocation(g_staticBorderProgram, "u_borderColor");
    g_staticBorderShaderLocs.thickness = glGetUniformLocation(g_staticBorderProgram, "u_thickness");
    g_staticBorderShaderLocs.radius = glGetUniformLocation(g_staticBorderProgram, "u_radius");
    g_staticBorderShaderLocs.size = glGetUniformLocation(g_staticBorderProgram, "u_size");
    g_staticBorderShaderLocs.quadSize = glGetUniformLocation(g_staticBorderProgram, "u_quadSize");

    g_passthroughShaderLocs.screenTexture = glGetUniformLocation(g_passthroughProgram, "screenTexture");
    g_passthroughShaderLocs.sourceRect = glGetUniformLocation(g_passthroughProgram, "u_sourceRect");
    g_passthroughShaderLocs.opacity = glGetUniformLocation(g_passthroughProgram, "u_opacity");
    g_passthroughShaderLocs.sourceTexelSize = glGetUniformLocation(g_passthroughProgram, "u_sourceTexelSize");
    g_passthroughShaderLocs.sourcePixelSize = glGetUniformLocation(g_passthroughProgram, "u_sourcePixelSize");
    g_passthroughShaderLocs.snapToSourcePixels = glGetUniformLocation(g_passthroughProgram, "u_snapToSourcePixels");

    g_gradientShaderLocs.numStops = glGetUniformLocation(g_gradientProgram, "u_numStops");
    g_gradientShaderLocs.stopColors = glGetUniformLocation(g_gradientProgram, "u_stopColors");
    g_gradientShaderLocs.stopPositions = glGetUniformLocation(g_gradientProgram, "u_stopPositions");
    g_gradientShaderLocs.angle = glGetUniformLocation(g_gradientProgram, "u_angle");
    g_gradientShaderLocs.time = glGetUniformLocation(g_gradientProgram, "u_time");
    g_gradientShaderLocs.animationType = glGetUniformLocation(g_gradientProgram, "u_animationType");
    g_gradientShaderLocs.animationSpeed = glGetUniformLocation(g_gradientProgram, "u_animationSpeed");
    g_gradientShaderLocs.colorFade = glGetUniformLocation(g_gradientProgram, "u_colorFade");

    g_virtualCameraNv12ShaderLocs.screenTexture = glGetUniformLocation(g_virtualCameraNv12Program, "screenTexture");
    g_virtualCameraNv12ShaderLocs.sourceTexelSize = glGetUniformLocation(g_virtualCameraNv12Program, "u_sourceTexelSize");
    g_virtualCameraNv12ShaderLocs.outputMode = glGetUniformLocation(g_virtualCameraNv12Program, "u_outputMode");
    g_virtualCameraNv12ShaderLocs.colorSpaceMode = glGetUniformLocation(g_virtualCameraNv12Program, "u_colorSpaceMode");

    glUseProgram(g_renderProgram);
    glUniform1i(g_renderShaderLocs.filterTexture, 0);

    glUseProgram(g_renderPassthroughProgram);
    glUniform1i(g_renderPassthroughShaderLocs.filterTexture, 0);
    glUniform1f(g_renderPassthroughShaderLocs.opacity, 1.0f);

    glUseProgram(g_backgroundProgram);
    glUniform1i(g_backgroundShaderLocs.backgroundTexture, 0);

    glUseProgram(g_imageRenderProgram);
    glUniform1i(g_imageRenderShaderLocs.imageTexture, 0);

    glUseProgram(g_staticBorderProgram);

    glUseProgram(g_filterProgram);
    glUniform1i(g_filterShaderLocs.screenTexture, 0);

    glUseProgram(g_passthroughProgram);
    glUniform1i(g_passthroughShaderLocs.screenTexture, 0);
    glUniform1f(g_passthroughShaderLocs.opacity, 1.0f);
    glUniform2f(g_passthroughShaderLocs.sourceTexelSize, 1.0f, 1.0f);
    glUniform2f(g_passthroughShaderLocs.sourcePixelSize, 1.0f, 1.0f);
    glUniform1i(g_passthroughShaderLocs.snapToSourcePixels, 0);

    glUseProgram(g_virtualCameraNv12Program);
    glUniform1i(g_virtualCameraNv12ShaderLocs.screenTexture, 0);
    if (g_virtualCameraNv12ShaderLocs.colorSpaceMode >= 0) {
        glUniform1i(g_virtualCameraNv12ShaderLocs.colorSpaceMode, 0);
    }

    glUseProgram(0);

}

void CleanupShaders() {
    if (g_filterProgram) {
        glDeleteProgram(g_filterProgram);
        g_filterProgram = 0;
    }
    if (g_renderProgram) {
        glDeleteProgram(g_renderProgram);
        g_renderProgram = 0;
    }
    if (g_renderPassthroughProgram) {
        glDeleteProgram(g_renderPassthroughProgram);
        g_renderPassthroughProgram = 0;
    }
    if (g_backgroundProgram) {
        glDeleteProgram(g_backgroundProgram);
        g_backgroundProgram = 0;
    }
    if (g_solidColorProgram) {
        glDeleteProgram(g_solidColorProgram);
        g_solidColorProgram = 0;
    }
    if (g_imageRenderProgram) {
        glDeleteProgram(g_imageRenderProgram);
        g_imageRenderProgram = 0;
    }
    if (g_passthroughProgram) {
        glDeleteProgram(g_passthroughProgram);
        g_passthroughProgram = 0;
    }
    if (g_gradientProgram) {
        glDeleteProgram(g_gradientProgram);
        g_gradientProgram = 0;
    }
    if (g_staticBorderProgram) {
        glDeleteProgram(g_staticBorderProgram);
        g_staticBorderProgram = 0;
    }
    if (g_virtualCameraNv12Program) {
        glDeleteProgram(g_virtualCameraNv12Program);
        g_virtualCameraNv12Program = 0;
    }
}

static void CollectAllGPUImageTexturesForDeletionUnsafe(std::vector<GLuint>& texturesToDelete) {
    auto collectTextureIdsForDeletion = [&texturesToDelete](const auto& inst) {
        if (inst.textureId != 0) {
            texturesToDelete.push_back(inst.textureId);
        }
        for (GLuint tex : inst.frameTextures) {
            if (tex != 0 && tex != inst.textureId) {
                texturesToDelete.push_back(tex);
            }
        }
    };

    for (auto const& [id, inst] : g_backgroundTextures) {
        collectTextureIdsForDeletion(inst);
    }
    g_backgroundTextures.clear();

    for (auto const& [id, inst] : g_userImages) {
        collectTextureIdsForDeletion(inst);
    }
    g_userImages.clear();
}

static void DiscardAllGPUImagesLocked() {
    PROFILE_SCOPE_CAT("GPU Image Discard", "GPU Operations");
    std::vector<GLuint> texturesToDelete;

    CollectAllGPUImageTexturesForDeletionUnsafe(texturesToDelete);

    // Enqueue for deletion after releasing resource-map locks.
    {
        std::lock_guard<std::mutex> lock(g_texturesToDeleteMutex);
        g_texturesToDelete.insert(g_texturesToDelete.end(), texturesToDelete.begin(), texturesToDelete.end());
    }
    if (!texturesToDelete.empty()) { g_hasTexturesToDelete.store(true, std::memory_order_release); }
    Log("All background and user image textures have been queued for deletion.");
}

void DiscardAllGPUImages() {
    std::lock_guard<std::mutex> bgLock(g_backgroundTexturesMutex);
    std::lock_guard<std::mutex> imageLock(g_userImagesMutex);

    DiscardAllGPUImagesLocked();
}

void DiscardUnusedUserImageCaches() {
    std::unordered_set<std::string> allowedImageIds;
    allowedImageIds.reserve(g_config.images.size() + g_config.eyezoom.overlays.size());
    for (const auto& img : g_config.images) {
        allowedImageIds.insert(img.name);
    }
    for (const auto& overlay : g_config.eyezoom.overlays) {
        allowedImageIds.insert("ezoverlay_" + overlay.name);
    }

    std::vector<GLuint> texturesToDelete;
    size_t removedGpuEntries = 0;
    {
        std::lock_guard<std::mutex> lock(g_userImagesMutex);
        for (auto it = g_userImages.begin(); it != g_userImages.end();) {
            if (allowedImageIds.find(it->first) != allowedImageIds.end()) {
                ++it;
                continue;
            }

            if (it->second.textureId != 0) {
                texturesToDelete.push_back(it->second.textureId);
            }
            for (GLuint tex : it->second.frameTextures) {
                if (tex != 0 && tex != it->second.textureId) {
                    texturesToDelete.push_back(tex);
                }
            }
            it = g_userImages.erase(it);
            removedGpuEntries += 1;
        }
    }

    size_t removedPendingDecodes = 0;
    {
        std::lock_guard<std::mutex> lock(g_decodedImagesMutex);
        for (auto it = g_decodedImagesQueue.begin(); it != g_decodedImagesQueue.end();) {
            if (it->type != DecodedImageData::Type::UserImage || allowedImageIds.find(it->id) != allowedImageIds.end()) {
                ++it;
                continue;
            }

            if (it->data) {
                stbi_image_free(it->data);
                it->data = nullptr;
            }
            it = g_decodedImagesQueue.erase(it);
            removedPendingDecodes += 1;
        }
    }

    if (!texturesToDelete.empty()) {
        std::lock_guard<std::mutex> lock(g_texturesToDeleteMutex);
        g_texturesToDelete.insert(g_texturesToDelete.end(), texturesToDelete.begin(), texturesToDelete.end());
        g_hasTexturesToDelete.store(true, std::memory_order_release);
    }

    if (removedGpuEntries > 0 || removedPendingDecodes > 0) {
        LogCategory("image_monitor", "Pruned deleted user image caches: gpuEntries=" + std::to_string(removedGpuEntries) +
                                         ", pendingDecodes=" + std::to_string(removedPendingDecodes) + ".");
    }
}

struct PixelStoreStateGuard {
    GLint unpackRowLength = 0;
    GLint unpackSkipPixels = 0;
    GLint unpackSkipRows = 0;
    GLint packAlignment = 0;
    GLint unpackAlignment = 0;

    PixelStoreStateGuard() {
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &unpackRowLength);
        glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &unpackSkipPixels);
        glGetIntegerv(GL_UNPACK_SKIP_ROWS, &unpackSkipRows);
        glGetIntegerv(GL_PACK_ALIGNMENT, &packAlignment);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpackAlignment);
    }

    ~PixelStoreStateGuard() {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, unpackRowLength);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, unpackSkipPixels);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, unpackSkipRows);
        glPixelStorei(GL_PACK_ALIGNMENT, packAlignment);
        glPixelStorei(GL_UNPACK_ALIGNMENT, unpackAlignment);
    }
};

static bool SupportsSamplerObjects() {
    return GLEW_VERSION_3_3 || GLEW_ARB_sampler_objects;
}

void SaveGLState(GLState* s) {
    {
        PROFILE_SCOPE_CAT("Save GL Bindings", "SwapBuffers");
        glGetIntegerv(GL_CURRENT_PROGRAM, &s->p);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s->va);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s->ab);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &s->read_fb);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &s->draw_fb);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &s->at);

        glGetIntegerv(GL_TEXTURE_BINDING_2D, &s->t);
        if (SupportsSamplerObjects()) {
            glGetIntegerv(GL_SAMPLER_BINDING, &s->smp);
        } else {
            s->smp = 0;
            s->smp0 = 0;
            s->smp1 = 0;
        }

        if (s->at == GL_TEXTURE0) {
            s->t0 = s->t;
            s->smp0 = s->smp;
        } else {
            glActiveTexture(GL_TEXTURE0);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &s->t0);
            if (SupportsSamplerObjects()) { glGetIntegerv(GL_SAMPLER_BINDING, &s->smp0); }
        }

        if (s->at == GL_TEXTURE1) {
            s->t1 = s->t;
            s->smp1 = s->smp;
        } else {
            glActiveTexture(GL_TEXTURE1);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &s->t1);
            if (SupportsSamplerObjects()) { glGetIntegerv(GL_SAMPLER_BINDING, &s->smp1); }
        }

        glActiveTexture(s->at);

        s->fb = s->draw_fb;
    }

    {
        PROFILE_SCOPE_CAT("Save GL Enable State", "SwapBuffers");
        s->be = glIsEnabled(GL_BLEND);
        s->de = glIsEnabled(GL_DEPTH_TEST);
        s->sc = glIsEnabled(GL_SCISSOR_TEST);
        s->ce = glIsEnabled(GL_CULL_FACE);
        s->ste = glIsEnabled(GL_STENCIL_TEST);
        s->srgb_enabled = glIsEnabled(GL_FRAMEBUFFER_SRGB);
        s->rasterizer_discard = glIsEnabled(GL_RASTERIZER_DISCARD);
        s->color_logic_op = glIsEnabled(GL_COLOR_LOGIC_OP);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &s->depth_mask);
    }

    {
        PROFILE_SCOPE_CAT("Save GL Draw State", "SwapBuffers");
        glGetIntegerv(GL_BLEND_SRC_RGB, &s->blend_src_rgb);
        glGetIntegerv(GL_BLEND_DST_RGB, &s->blend_dst_rgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &s->blend_src_alpha);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &s->blend_dst_alpha);
        glGetIntegerv(GL_BLEND_EQUATION_RGB, &s->blend_eq_rgb);
        glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &s->blend_eq_alpha);
        glGetIntegerv(GL_DRAW_BUFFER, &s->draw_buffer);
        glGetIntegerv(GL_READ_BUFFER, &s->read_buffer);
        glGetIntegerv(GL_PACK_ROW_LENGTH, &s->pack_row_length);

        glGetIntegerv(GL_VIEWPORT, &s->vp[0]);
        glGetIntegerv(GL_SCISSOR_BOX, s->sb);

        glGetFloatv(GL_COLOR_CLEAR_VALUE, s->cc);
        glGetFloatv(GL_BLEND_COLOR, s->blend_color);
        glGetFloatv(GL_LINE_WIDTH, &s->lw);
        glGetBooleanv(GL_COLOR_WRITEMASK, s->color_mask);
    }
}

void RestoreGLState(const GLState& s) {
    {
        PROFILE_SCOPE_CAT("Restore GL Bindings", "SwapBuffers");
        glUseProgram(s.p);
        glBindVertexArray(s.va);
        glBindBuffer(GL_ARRAY_BUFFER, s.ab);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, s.read_fb);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s.draw_fb);

        glActiveTexture(GL_TEXTURE0);
        BindTextureDirect(GL_TEXTURE_2D, s.t0);
        glActiveTexture(GL_TEXTURE1);
        BindTextureDirect(GL_TEXTURE_2D, s.t1);
        if (SupportsSamplerObjects()) {
            glBindSampler(0, static_cast<GLuint>(s.smp0));
            glBindSampler(1, static_cast<GLuint>(s.smp1));
        }
        if (s.at != GL_TEXTURE0 && s.at != GL_TEXTURE1) {
            glActiveTexture(s.at);
            BindTextureDirect(GL_TEXTURE_2D, s.t);
            if (SupportsSamplerObjects() && s.at >= GL_TEXTURE0) {
                glBindSampler(static_cast<GLuint>(s.at - GL_TEXTURE0), static_cast<GLuint>(s.smp));
            }
        } else {
            glActiveTexture(s.at);
        }
    }

    {
        PROFILE_SCOPE_CAT("Restore GL Enable State", "SwapBuffers");
        if (s.be)
            glEnable(GL_BLEND);
        else
            glDisable(GL_BLEND);
        if (s.de)
            glEnable(GL_DEPTH_TEST);
        else
            glDisable(GL_DEPTH_TEST);
        if (s.sc)
            glEnable(GL_SCISSOR_TEST);
        else
            glDisable(GL_SCISSOR_TEST);
        if (s.ce)
            glEnable(GL_CULL_FACE);
        else
            glDisable(GL_CULL_FACE);
        if (s.ste)
            glEnable(GL_STENCIL_TEST);
        else
            glDisable(GL_STENCIL_TEST);
        if (s.srgb_enabled)
            glEnable(GL_FRAMEBUFFER_SRGB);
        else
            glDisable(GL_FRAMEBUFFER_SRGB);
        if (s.rasterizer_discard)
            glEnable(GL_RASTERIZER_DISCARD);
        else
            glDisable(GL_RASTERIZER_DISCARD);
        if (s.color_logic_op)
            glEnable(GL_COLOR_LOGIC_OP);
        else
            glDisable(GL_COLOR_LOGIC_OP);
    }

    {
        PROFILE_SCOPE_CAT("Restore GL Draw State", "SwapBuffers");
        glBlendEquationSeparate(s.blend_eq_rgb, s.blend_eq_alpha);
        glBlendFuncSeparate(s.blend_src_rgb, s.blend_dst_rgb, s.blend_src_alpha, s.blend_dst_alpha);
        glBlendColor(s.blend_color[0], s.blend_color[1], s.blend_color[2], s.blend_color[3]);
        glDepthMask(s.depth_mask);
        glDrawBuffer(s.draw_buffer);
        glReadBuffer(s.read_buffer);
        glPixelStorei(GL_PACK_ROW_LENGTH, s.pack_row_length);

        if (oglViewport)
            oglViewport(s.vp[0], s.vp[1], s.vp[2], s.vp[3]);
        else
            glViewport(s.vp[0], s.vp[1], s.vp[2], s.vp[3]);
        glScissor(s.sb[0], s.sb[1], s.sb[2], s.sb[3]);

        glClearColor(s.cc[0], s.cc[1], s.cc[2], s.cc[3]);
        glLineWidth(s.lw);
        glColorMask(s.color_mask[0], s.color_mask[1], s.color_mask[2], s.color_mask[3]);
    }
}

static void PrepareSameThreadOverlayState(const GLState& s, int fullW, int fullH) {
    glBindFramebuffer(GL_FRAMEBUFFER, s.fb);
    if (s.fb == 0) {
        glDrawBuffer(s.draw_buffer);
        glReadBuffer(s.read_buffer);
    }

    if (oglViewport)
        oglViewport(0, 0, fullW, fullH);
    else
        glViewport(0, 0, fullW, fullH);

    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glDisable(GL_RASTERIZER_DISCARD);
    glDisable(GL_COLOR_LOGIC_OP);
    if (SupportsSamplerObjects()) {
        glBindSampler(0, 0);
        glBindSampler(1, 0);
    }
    glActiveTexture(GL_TEXTURE0);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glBlendColor(0.0f, 0.0f, 0.0f, 0.0f);
    glDepthMask(GL_FALSE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

void CleanupGPUResources() {
    Log("CleanupGPUResources: Starting cleanup...");

    CleanupCaptureTexture();

    HGLRC currentContext = wglGetCurrentContext();
    if (!currentContext) {
        Log("CleanupGPUResources: WARNING - No current GL context, cannot perform GPU cleanup");
        return;
    }

    CleanupMirrorCaptureGpuResources();

    // Lock all GPU resource mutexes during cleanup
    std::unique_lock<std::shared_mutex> mirrorLock(g_mirrorInstancesMutex); // Write lock - cleanup
    std::lock_guard<std::mutex> bgLock(g_backgroundTexturesMutex);
    std::lock_guard<std::mutex> imageLock(g_userImagesMutex);

    std::string currentMirrorId;
    std::string currentResource = "<none>";
    uintptr_t currentHandle = 0;

    auto trackCleanupResource = [&](const std::string& mirrorId, const std::string& resource, uintptr_t handle) {
        currentMirrorId = mirrorId;
        currentResource = resource;
        currentHandle = handle;
    };
    auto clearTrackedCleanupResource = [&]() {
        currentMirrorId.clear();
        currentResource = "<none>";
        currentHandle = 0;
    };
    auto formatCleanupDebugState = [&](const char* phase) {
        size_t queuedTextureDeleteCount = 0;
        {
            std::lock_guard<std::mutex> lock(g_texturesToDeleteMutex);
            queuedTextureDeleteCount = g_texturesToDelete.size();
        }

        std::stringstream ss;
        ss << "phase=" << phase << ", glContext=0x" << std::hex << reinterpret_cast<uintptr_t>(currentContext) << std::dec
           << ", currentMirror=" << (currentMirrorId.empty() ? std::string("<none>") : currentMirrorId)
           << ", currentResource=" << currentResource << ", currentHandle=0x" << std::hex << currentHandle << std::dec
           << ", mirrorCount=" << g_mirrorInstances.size() << ", sceneFBO=" << g_sceneFBO << ", sceneTexture=" << g_sceneTexture
           << ", obsComposeFBOs={" << g_sameThreadObsComposeFBOs[0] << "," << g_sameThreadObsComposeFBOs[1] << "}"
           << ", obsComposeTextures={" << g_sameThreadObsComposeTextures[0] << "," << g_sameThreadObsComposeTextures[1] << "}"
           << ", vcScaleFBO=" << g_sameThreadVirtualCameraScaleFBO << ", vcConvertFBO=" << g_sameThreadVirtualCameraConvertFBO
           << ", vcReadFBO=" << g_sameThreadVirtualCameraReadFBO << ", vcScaleTexture=" << g_sameThreadVirtualCameraScaleTexture
           << ", vcLumaTexture=" << g_sameThreadVirtualCameraLumaTexture << ", vcChromaTexture=" << g_sameThreadVirtualCameraChromaTexture
           << ", queuedTextureDeletes=" << queuedTextureDeleteCount << ", vao=" << g_vao << ", vbo=" << g_vbo
           << ", debugVao=" << g_debugVAO << ", debugVbo=" << g_debugVBO;
        return ss.str();
    };
    auto logCleanupStdException = [&](const char* phase, const std::exception& e) {
        LogException("CleanupGPUResources [" + formatCleanupDebugState(phase) + "]", e);
    };
    auto logCleanupUnknownException = [&](const char* phase) {
        Log("CleanupGPUResources: Unknown exception [" + formatCleanupDebugState(phase) + "]");
    };

    // PBO system cleanup is handled by CleanupCapturePBOs() in mirror_thread.cpp
    // No fence cleanup needed here since captureFence was removed from MirrorInstance

    try {
        for (auto const& [k, v] : g_mirrorInstances) {
            if (v.fbo) {
                trackCleanupResource(k, "mirror.fbo", static_cast<uintptr_t>(v.fbo));
                glDeleteFramebuffers(1, &v.fbo);
                while (glGetError() != GL_NO_ERROR) {}
            }
            // Clean up the back-buffer FBO used by mirror capture.
            if (v.fboBack) {
                trackCleanupResource(k, "mirror.fboBack", static_cast<uintptr_t>(v.fboBack));
                glDeleteFramebuffers(1, &v.fboBack);
                while (glGetError() != GL_NO_ERROR) {}
            }
            if (v.finalFbo) {
                trackCleanupResource(k, "mirror.finalFbo", static_cast<uintptr_t>(v.finalFbo));
                glDeleteFramebuffers(1, &v.finalFbo);
                while (glGetError() != GL_NO_ERROR) {}
            }
            if (v.finalFboBack) {
                trackCleanupResource(k, "mirror.finalFboBack", static_cast<uintptr_t>(v.finalFboBack));
                glDeleteFramebuffers(1, &v.finalFboBack);
                while (glGetError() != GL_NO_ERROR) {}
            }
        }
        if (g_sceneFBO) {
            trackCleanupResource("<global>", "g_sceneFBO", static_cast<uintptr_t>(g_sceneFBO));
            glDeleteFramebuffers(1, &g_sceneFBO);
            while (glGetError() != GL_NO_ERROR) {}
            g_sceneFBO = 0;
        }
        for (int i = 0; i < SAME_THREAD_OBS_BUFFER_COUNT; ++i) {
            if (g_sameThreadObsComposeFBOs[i]) {
                trackCleanupResource("<global>", "g_sameThreadObsComposeFBOs[" + std::to_string(i) + "]",
                                     static_cast<uintptr_t>(g_sameThreadObsComposeFBOs[i]));
                glDeleteFramebuffers(1, &g_sameThreadObsComposeFBOs[i]);
                while (glGetError() != GL_NO_ERROR) {}
                g_sameThreadObsComposeFBOs[i] = 0;
            }
        }
        clearTrackedCleanupResource();
    } catch (const std::exception& e) {
        logCleanupStdException("fbo cleanup", e);
    } catch (...) { logCleanupUnknownException("fbo cleanup"); }

    try {
        for (auto const& [k, v] : g_mirrorInstances) {
            if (v.fboTexture) {
                trackCleanupResource(k, "mirror.fboTexture", static_cast<uintptr_t>(v.fboTexture));
                glDeleteTextures(1, &v.fboTexture);
                while (glGetError() != GL_NO_ERROR) {}
            }
            if (v.tempCaptureTexture) {
                trackCleanupResource(k, "mirror.tempCaptureTexture", static_cast<uintptr_t>(v.tempCaptureTexture));
                glDeleteTextures(1, &v.tempCaptureTexture);
                while (glGetError() != GL_NO_ERROR) {}
            }
            // Clean up the back-buffer texture used by mirror capture.
            if (v.fboTextureBack) {
                trackCleanupResource(k, "mirror.fboTextureBack", static_cast<uintptr_t>(v.fboTextureBack));
                glDeleteTextures(1, &v.fboTextureBack);
                while (glGetError() != GL_NO_ERROR) {}
            }
            if (v.finalTexture) {
                trackCleanupResource(k, "mirror.finalTexture", static_cast<uintptr_t>(v.finalTexture));
                glDeleteTextures(1, &v.finalTexture);
                while (glGetError() != GL_NO_ERROR) {}
            }
            if (v.finalTextureBack) {
                trackCleanupResource(k, "mirror.finalTextureBack", static_cast<uintptr_t>(v.finalTextureBack));
                glDeleteTextures(1, &v.finalTextureBack);
                while (glGetError() != GL_NO_ERROR) {}
            }

            // Clean up GPU sync fences
            if (v.gpuFence && glIsSync(v.gpuFence)) {
                trackCleanupResource(k, "mirror.gpuFence", reinterpret_cast<uintptr_t>(v.gpuFence));
                glDeleteSync(v.gpuFence);
            }
            if (v.gpuFenceBack && glIsSync(v.gpuFenceBack)) {
                trackCleanupResource(k, "mirror.gpuFenceBack", reinterpret_cast<uintptr_t>(v.gpuFenceBack));
                glDeleteSync(v.gpuFenceBack);
            }
        }
        g_mirrorInstances.clear();

        if (g_sceneTexture) {
            trackCleanupResource("<global>", "g_sceneTexture", static_cast<uintptr_t>(g_sceneTexture));
            glDeleteTextures(1, &g_sceneTexture);
            while (glGetError() != GL_NO_ERROR) {}
            g_sceneTexture = 0;
        }
        for (int i = 0; i < SAME_THREAD_OBS_BUFFER_COUNT; ++i) {
            if (g_sameThreadObsComposeTextures[i]) {
                trackCleanupResource("<global>", "g_sameThreadObsComposeTextures[" + std::to_string(i) + "]",
                                     static_cast<uintptr_t>(g_sameThreadObsComposeTextures[i]));
                glDeleteTextures(1, &g_sameThreadObsComposeTextures[i]);
                while (glGetError() != GL_NO_ERROR) {}
                g_sameThreadObsComposeTextures[i] = 0;
            }
        }
        if (g_sameThreadVirtualCameraScaleTexture) {
            trackCleanupResource("<global>", "g_sameThreadVirtualCameraScaleTexture",
                                 static_cast<uintptr_t>(g_sameThreadVirtualCameraScaleTexture));
            glDeleteTextures(1, &g_sameThreadVirtualCameraScaleTexture);
            while (glGetError() != GL_NO_ERROR) {}
            g_sameThreadVirtualCameraScaleTexture = 0;
        }
        if (g_sameThreadVirtualCameraLumaTexture) {
            trackCleanupResource("<global>", "g_sameThreadVirtualCameraLumaTexture",
                                 static_cast<uintptr_t>(g_sameThreadVirtualCameraLumaTexture));
            glDeleteTextures(1, &g_sameThreadVirtualCameraLumaTexture);
            while (glGetError() != GL_NO_ERROR) {}
            g_sameThreadVirtualCameraLumaTexture = 0;
        }
        if (g_sameThreadVirtualCameraChromaTexture) {
            trackCleanupResource("<global>", "g_sameThreadVirtualCameraChromaTexture",
                                 static_cast<uintptr_t>(g_sameThreadVirtualCameraChromaTexture));
            glDeleteTextures(1, &g_sameThreadVirtualCameraChromaTexture);
            while (glGetError() != GL_NO_ERROR) {}
            g_sameThreadVirtualCameraChromaTexture = 0;
        }
        if (g_sameThreadVirtualCameraScaleFBO) {
            trackCleanupResource("<global>", "g_sameThreadVirtualCameraScaleFBO", static_cast<uintptr_t>(g_sameThreadVirtualCameraScaleFBO));
            glDeleteFramebuffers(1, &g_sameThreadVirtualCameraScaleFBO);
            while (glGetError() != GL_NO_ERROR) {}
            g_sameThreadVirtualCameraScaleFBO = 0;
        }
        if (g_sameThreadVirtualCameraConvertFBO) {
            trackCleanupResource("<global>", "g_sameThreadVirtualCameraConvertFBO",
                                 static_cast<uintptr_t>(g_sameThreadVirtualCameraConvertFBO));
            glDeleteFramebuffers(1, &g_sameThreadVirtualCameraConvertFBO);
            while (glGetError() != GL_NO_ERROR) {}
            g_sameThreadVirtualCameraConvertFBO = 0;
        }
        if (g_sameThreadVirtualCameraReadFBO) {
            trackCleanupResource("<global>", "g_sameThreadVirtualCameraReadFBO", static_cast<uintptr_t>(g_sameThreadVirtualCameraReadFBO));
            glDeleteFramebuffers(1, &g_sameThreadVirtualCameraReadFBO);
            while (glGetError() != GL_NO_ERROR) {}
            g_sameThreadVirtualCameraReadFBO = 0;
        }
        trackCleanupResource("<global>", "ReleaseSameThreadVirtualCameraReadbacks", 0);
        ReleaseSameThreadVirtualCameraReadbacks();
        g_sameThreadVirtualCameraScaleW = 0;
        g_sameThreadVirtualCameraScaleH = 0;
        g_sameThreadVirtualCameraNv12W = 0;
        g_sameThreadVirtualCameraNv12H = 0;
        g_sameThreadObsComposePublishedIndex = -1;
        g_sameThreadObsComposeWriteIndex = 0;
        g_sameThreadObsComposeW = 0;
        g_sameThreadObsComposeH = 0;

        trackCleanupResource("<global>", "DiscardAllGPUImagesLocked", 0);
        DiscardAllGPUImagesLocked();

        {
            std::lock_guard<std::mutex> lock(g_texturesToDeleteMutex);
            if (!g_texturesToDelete.empty()) {
                trackCleanupResource("<global>", "g_texturesToDelete batch", static_cast<uintptr_t>(g_texturesToDelete.size()));
                glDeleteTextures((GLsizei)g_texturesToDelete.size(), g_texturesToDelete.data());
                while (glGetError() != GL_NO_ERROR) {}
                g_texturesToDelete.clear();
            }
        }
        clearTrackedCleanupResource();
    } catch (const std::exception& e) {
        logCleanupStdException("texture cleanup", e);
    } catch (...) { logCleanupUnknownException("texture cleanup"); }

    {
        std::lock_guard<std::mutex> lock(g_decodedImagesMutex);
        if (!g_decodedImagesQueue.empty()) {
            Log("Cleaning up " + std::to_string(g_decodedImagesQueue.size()) + " " + "pending decoded images to prevent memory leaks...");
            for (auto& decodedImg : g_decodedImagesQueue) {
                if (decodedImg.data) {
                    stbi_image_free(decodedImg.data);
                    decodedImg.data = nullptr;
                }
            }
            g_decodedImagesQueue.clear();
        }
    }

    try {
        if (g_vao) {
            trackCleanupResource("<global>", "g_vao", static_cast<uintptr_t>(g_vao));
            glDeleteVertexArrays(1, &g_vao);
            while (glGetError() != GL_NO_ERROR) {}
            g_vao = 0;
        }
        if (g_vbo) {
            trackCleanupResource("<global>", "g_vbo", static_cast<uintptr_t>(g_vbo));
            glDeleteBuffers(1, &g_vbo);
            while (glGetError() != GL_NO_ERROR) {}
            g_vbo = 0;
        }
        if (g_debugVAO) {
            trackCleanupResource("<global>", "g_debugVAO", static_cast<uintptr_t>(g_debugVAO));
            glDeleteVertexArrays(1, &g_debugVAO);
            while (glGetError() != GL_NO_ERROR) {}
            g_debugVAO = 0;
        }
        if (g_debugVBO) {
            trackCleanupResource("<global>", "g_debugVBO", static_cast<uintptr_t>(g_debugVBO));
            glDeleteBuffers(1, &g_debugVBO);
            while (glGetError() != GL_NO_ERROR) {}
            g_debugVBO = 0;
        }
        clearTrackedCleanupResource();
    } catch (const std::exception& e) {
        logCleanupStdException("vao/vbo cleanup", e);
    } catch (...) { logCleanupUnknownException("vao/vbo cleanup"); }

    try {
        trackCleanupResource("<global>", "CleanupShaders", 0);
        CleanupShaders();
        while (glGetError() != GL_NO_ERROR) {}
        clearTrackedCleanupResource();
    } catch (const std::exception& e) {
        logCleanupStdException("shader cleanup", e);
    } catch (...) { logCleanupUnknownException("shader cleanup"); }

    g_sceneW = g_sceneH = 0;
    g_glInitialized.store(false, std::memory_order_release);
    Log("CleanupGPUResources: Cleanup complete.");
}
void UploadDecodedImageToGPU(const DecodedImageData& imgData) {
    PixelStoreStateGuard pixelStoreGuard;
    UploadDecodedImageToGPU_Internal(imgData);
}

template <typename TextureInstance>
static void InitializeAnimatedTexturePlayback(TextureInstance& inst, size_t frameCount);

void UploadDecodedImageToGPU_Internal(const DecodedImageData& imgData) {
    PROFILE_SCOPE_CAT("GPU Image Upload", "GPU Operations");

    size_t decodedBytes = 0;
    std::string decodedReason;
    if (!TryDescribeDecodedImageStorage(imgData, decodedBytes, decodedReason)) {
        LogCategory("image_monitor", "Skipping GPU upload for image '" + imgData.id + "' due to invalid decoded image data: " + decodedReason + ".");
        return;
    }
    if (!imgData.isVideo && decodedBytes > kMaxDecodedImageUploadBytes) {
        LogCategory("image_monitor",
                    "Skipping GPU upload for image '" + imgData.id + "' because decoded storage " + FormatByteCount(decodedBytes) +
                        " exceeds guard limit of " + FormatByteCount(kMaxDecodedImageUploadBytes) + ".");
        return;
    }

    LogCategory("image_monitor", "Uploading decoded image '" + imgData.id + "' to GPU: " + std::to_string(imgData.width) + "x" +
                                   std::to_string(imgData.height) + ", frameCount=" + std::to_string(imgData.frameCount) +
                                   ", bytes=" + FormatByteCount(decodedBytes) + ".");

    if (imgData.type == DecodedImageData::Type::Background) {
        std::lock_guard<std::mutex> bgLock(g_backgroundTexturesMutex);

        auto it = g_backgroundTextures.find(imgData.id);
        if (it != g_backgroundTextures.end()) {
            BackgroundTextureInstance& oldInst = it->second;
            std::lock_guard<std::mutex> lock(g_texturesToDeleteMutex);
            if (oldInst.textureId != 0) {
                g_texturesToDelete.push_back(oldInst.textureId);
            }
            for (GLuint tex : oldInst.frameTextures) {
                if (tex != 0 && tex != oldInst.textureId) {
                    g_texturesToDelete.push_back(tex);
                }
            }
            g_hasTexturesToDelete.store(true, std::memory_order_release);
            g_backgroundTextures.erase(it);
        }

        if (imgData.data) {
            BackgroundTextureInstance inst;
            inst.width = imgData.width;
            inst.height = imgData.frameHeight;
            inst.textureStorageHeight = imgData.height;
            inst.frameCount = (imgData.isAnimated && imgData.frameCount > 1) ? imgData.frameCount : 1;
            inst.isVideo = imgData.isVideo;

            if (imgData.isAnimated && imgData.frameCount > 1) {
                inst.isAnimated = true;
                inst.frameDelays = imgData.frameDelays;
                InitializeAnimatedTexturePlayback(inst, static_cast<size_t>(inst.frameCount));

                GLint maxTextureSize = 0;
                glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
                const int frameHeight = imgData.frameHeight;
                if (maxTextureSize > 0 && (imgData.width > maxTextureSize || frameHeight > maxTextureSize)) {
                    LogCategory("image_monitor", "Skipping animated background atlas upload for '" + imgData.id +
                                                     "' because frame dimensions exceed GL_MAX_TEXTURE_SIZE=" +
                                                     std::to_string(maxTextureSize) + ": " + std::to_string(imgData.width) + "x" +
                                                     std::to_string(frameHeight) + ".");
                    return;
                }
                const int framesPerTexture = (std::max)(1, maxTextureSize > 0 ? (maxTextureSize / (std::max)(1, frameHeight)) : imgData.frameCount);
                inst.framesPerTexture = framesPerTexture;
                inst.textureStorageHeight = (std::min)(imgData.height, frameHeight * framesPerTexture);

                for (int frameStart = 0; frameStart < imgData.frameCount; frameStart += framesPerTexture) {
                    const int framesThisTexture = (std::min)(framesPerTexture, imgData.frameCount - frameStart);
                    const int pageHeight = frameHeight * framesThisTexture;
                    GLuint texture = 0;
                    glGenTextures(1, &texture);
                    BindTextureDirect(GL_TEXTURE_2D, texture);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
                    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
                    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

                    unsigned char* pageData = imgData.data + (static_cast<size_t>(frameStart) * frameHeight * imgData.width * 4ull);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, imgData.width, pageHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, pageData);
                    inst.frameTextures.push_back(texture);
                    inst.frameTextureHeights.push_back(pageHeight);
                }

                if (!inst.frameTextures.empty()) {
                    inst.textureId = inst.frameTextures.front();
                }

                g_backgroundTextures[imgData.id] = inst;
                Log("Uploaded animated background atlas for '" + imgData.id + "' to GPU (" + std::to_string(imgData.frameCount) +
                    " frames across " + std::to_string(inst.frameTextures.size()) + " texture page(s)).");
            } else {
                inst.isAnimated = false;

                GLuint t;
                glGenTextures(1, &t);
                BindTextureDirect(GL_TEXTURE_2D, t);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
                glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, imgData.width, imgData.frameHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, imgData.data);

                inst.textureId = t;
                g_backgroundTextures[imgData.id] = inst;
                Log("Uploaded background for '" + imgData.id + "' to GPU.");
            }
        } else {
            Log("Skipping GPU upload for background '" + imgData.id + "' due to null image data.");
        }
    } else if (imgData.type == DecodedImageData::Type::UserImage) {
        // Remove old instance under lock, but avoid holding the lock while uploading new textures.
        UserImageInstance oldInst;
        bool hadOldInst = false;
        {
            std::lock_guard<std::mutex> imageLock(g_userImagesMutex);
            auto it = g_userImages.find(imgData.id);
            if (it != g_userImages.end()) {
                oldInst = std::move(it->second);
                g_userImages.erase(it);
                hadOldInst = true;
            }
        }
        if (hadOldInst) {
            std::lock_guard<std::mutex> lock(g_texturesToDeleteMutex);
            if (oldInst.textureId != 0) {
                g_texturesToDelete.push_back(oldInst.textureId);
            }
            for (GLuint tex : oldInst.frameTextures) {
                if (tex != 0 && tex != oldInst.textureId) {
                    g_texturesToDelete.push_back(tex);
                }
            }
        }
        if (hadOldInst) { g_hasTexturesToDelete.store(true, std::memory_order_release); }

        if (imgData.data) {
            UserImageInstance inst;
            inst.width = imgData.width;
            inst.height = imgData.frameHeight;
            inst.textureStorageHeight = imgData.height;
            inst.frameCount = (imgData.isAnimated && imgData.frameCount > 1) ? imgData.frameCount : 1;
            inst.isVideo = imgData.isVideo;

            inst.isFullyTransparent = true;
            const size_t totalPixels = static_cast<size_t>(imgData.width) * static_cast<size_t>(imgData.height);
            for (size_t i = 0; i < totalPixels; ++i) {
                if (imgData.data[i * 4 + 3] > 0) {
                    inst.isFullyTransparent = false;
                    break;
                }
            }

            if (imgData.isAnimated && imgData.frameCount > 1) {
                inst.isAnimated = true;
                inst.frameDelays = imgData.frameDelays;
                InitializeAnimatedTexturePlayback(inst, static_cast<size_t>(inst.frameCount));

                GLint maxTextureSize = 0;
                glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
                const int frameHeight = imgData.frameHeight;
                if (maxTextureSize > 0 && (imgData.width > maxTextureSize || frameHeight > maxTextureSize)) {
                    LogCategory("image_monitor", "Skipping animated user image atlas upload for '" + imgData.id +
                                                     "' because frame dimensions exceed GL_MAX_TEXTURE_SIZE=" +
                                                     std::to_string(maxTextureSize) + ": " + std::to_string(imgData.width) + "x" +
                                                     std::to_string(frameHeight) + ".");
                    return;
                }
                const int framesPerTexture = (std::max)(1, maxTextureSize > 0 ? (maxTextureSize / (std::max)(1, frameHeight)) : imgData.frameCount);
                inst.framesPerTexture = framesPerTexture;
                inst.textureStorageHeight = (std::min)(imgData.height, frameHeight * framesPerTexture);

                for (int frameStart = 0; frameStart < imgData.frameCount; frameStart += framesPerTexture) {
                    const int framesThisTexture = (std::min)(framesPerTexture, imgData.frameCount - frameStart);
                    const int pageHeight = frameHeight * framesThisTexture;
                    GLuint texture = 0;
                    glGenTextures(1, &texture);
                    BindTextureDirect(GL_TEXTURE_2D, texture);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
                    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
                    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

                    unsigned char* pageData = imgData.data + (static_cast<size_t>(frameStart) * frameHeight * imgData.width * 4ull);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, imgData.width, pageHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, pageData);
                    inst.frameTextures.push_back(texture);
                    inst.frameTextureHeights.push_back(pageHeight);
                }

                if (!inst.frameTextures.empty()) {
                    inst.textureId = inst.frameTextures.front();
                }

                const size_t pageCount = inst.frameTextures.size();

                {
                    std::lock_guard<std::mutex> imageLock(g_userImagesMutex);
                    g_userImages[imgData.id] = std::move(inst);
                }
                LogCategory("image_monitor", "Uploaded animated user image atlas '" + imgData.id + "' to GPU (" +
                                                 std::to_string(imgData.frameCount) + " frames across " +
                                                 std::to_string(pageCount) + " texture page(s)).");
            } else {
                inst.isAnimated = false;

                glGenTextures(1, &inst.textureId);
                BindTextureDirect(GL_TEXTURE_2D, inst.textureId);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
                glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, imgData.width, imgData.frameHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, imgData.data);

                {
                    std::lock_guard<std::mutex> imageLock(g_userImagesMutex);
                    g_userImages[imgData.id] = std::move(inst);
                }
                LogCategory("image_monitor", "Uploaded user image '" + imgData.id + "' to GPU.");
            }
        } else {
            Log("Skipping GPU upload for user image '" + imgData.id + "' due to null image data.");
        }
    }
}

void ProcessPendingDecodedImages() {
    std::vector<DecodedImageData> pendingImages;
    {
        std::lock_guard<std::mutex> lock(g_decodedImagesMutex);
        if (g_decodedImagesQueue.empty()) {
            return;
        }
        pendingImages.swap(g_decodedImagesQueue);
    }

    PROFILE_SCOPE_CAT("Process Decoded Images", "GPU Operations");
    LogCategory("image_monitor", "Processing " + std::to_string(pendingImages.size()) + " decoded images on render thread.");
    for (auto& decodedImg : pendingImages) {
        if (!decodedImg.data) {
            continue;
        }

        UploadDecodedImageToGPU(decodedImg);
        stbi_image_free(decodedImg.data);
        decodedImg.data = nullptr;
    }
}

void InitializeGPUResources() {
    PROFILE_SCOPE_CAT("GPU Resource Initialization", "GPU Operations");

    GLint last_program;
    glGetIntegerv(GL_CURRENT_PROGRAM, &last_program);
    GLint last_texture;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_texture);
    GLint last_active_texture;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &last_active_texture);
    GLint last_array_buffer;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &last_array_buffer);
    GLint last_vertex_array;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &last_vertex_array);
    GLint last_framebuffer;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &last_framebuffer);

    CleanupGPUResources();

    // This prevents potential race conditions with g_configMutex during startup

    if (g_configLoadFailed.load()) {
        Log("FATAL: Config load failed. Aborting GPU resource initialization.");
        return;
    }

    InitializeShaders();

    if (!g_filterProgram || !g_renderProgram || !g_renderPassthroughProgram || !g_backgroundProgram || !g_solidColorProgram ||
        !g_imageRenderProgram || !g_passthroughProgram) {
        Log("FATAL: Failed to create one or more shader programs. Aborting GPU resource initialization.");
        glBindFramebuffer(GL_FRAMEBUFFER, last_framebuffer);
        glBindVertexArray(last_vertex_array);
        glUseProgram(last_program);
        return;
    }

    g_pendingImageLoad = true;

    std::vector<MirrorConfig> mirrorsToCreate;
    {
        auto initSnap = GetConfigSnapshot();
        if (initSnap) { mirrorsToCreate = initSnap->mirrors; }
        LogCategory("init", "Found " + std::to_string(mirrorsToCreate.size()) + " mirrors in config to create.");
    }
    // Release the framebuffer binding before calling CreateMirrorGPUResources
    glBindFramebuffer(GL_FRAMEBUFFER, last_framebuffer);

    for (const auto& conf : mirrorsToCreate) {
        CreateMirrorGPUResources(conf);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, last_framebuffer);
    glGenVertexArrays(1, &g_vao);
    glGenBuffers(1, &g_vbo);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 192, nullptr, GL_DYNAMIC_DRAW);
    g_vboCapacityBytes = sizeof(float) * 192;
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glGenVertexArrays(1, &g_debugVAO);
    glGenBuffers(1, &g_debugVBO);
    glBindVertexArray(g_debugVAO);
    glBindBuffer(GL_ARRAY_BUFFER, g_debugVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 4 * 2, nullptr, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    static const float fullscreenQuadVerts[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
        1.0f,  -1.0f, 1.0f, 0.0f,
        1.0f,  1.0f,  1.0f, 1.0f,
        -1.0f, -1.0f, 0.0f, 0.0f,
        1.0f,  1.0f,  1.0f, 1.0f,
        -1.0f, 1.0f,  0.0f, 1.0f
    };
    glGenVertexArrays(1, &g_fullscreenQuadVAO);
    glGenBuffers(1, &g_fullscreenQuadVBO);
    glBindVertexArray(g_fullscreenQuadVAO);
    glBindBuffer(GL_ARRAY_BUFFER, g_fullscreenQuadVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(fullscreenQuadVerts), fullscreenQuadVerts, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);

    LogCategory("init", "Restoring original OpenGL state...");
    glUseProgram(last_program);
    glActiveTexture(last_active_texture);
    BindTextureDirect(GL_TEXTURE_2D, last_texture);
    glBindVertexArray(last_vertex_array);
    glBindBuffer(GL_ARRAY_BUFFER, last_array_buffer);
    glBindFramebuffer(GL_FRAMEBUFFER, last_framebuffer);

    g_glInitialized.store(true, std::memory_order_release);
    LogCategory("init", "--- GPU resources initialized successfully. ---");
}

static bool CreateMirrorFramebuffer(GLuint& fbo, GLuint& texture, int w, int h, GLenum filter) {
    if (w <= 0 || h <= 0) { return false; }

    if (fbo == 0) { glGenFramebuffers(1, &fbo); }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    if (texture == 0) { glGenTextures(1, &texture); }
    BindTextureDirect(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    return (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
}

static void InvalidateTextureSampleabilityCache(GLuint texture);

static void DeleteMirrorFramebuffer(GLuint& fbo, GLuint& texture) {
    if (texture != 0) {
        InvalidateTextureSampleabilityCache(texture);
        glDeleteTextures(1, &texture);
        texture = 0;
    }
    if (fbo != 0) {
        glDeleteFramebuffers(1, &fbo);
        fbo = 0;
    }
}

void CreateMirrorGPUResources(const MirrorConfig& conf) {
    PROFILE_SCOPE_CAT("Create Mirror GPU Resources", "GPU Operations");

    if (conf.input.empty()) {
        Log("Warning: Mirror '" + conf.name + "' has no input regions. Skipping GPU resource creation.");
        return;
    }

    // Lock mutex before accessing g_mirrorInstances
    std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex); // Write lock - creating instance

    auto it = g_mirrorInstances.find(conf.name);
    if (it != g_mirrorInstances.end()) {
        Log("Mirror '" + conf.name + "' GPU resources already exist. Skipping creation.");
        return;
    }

    GLint last_framebuffer;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &last_framebuffer);
    GLint last_texture;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_texture);

    MirrorInstance inst;
    int padding = (conf.border.type == MirrorBorderType::Dynamic) ? conf.border.dynamicThickness : 0;
    inst.fbo_w = conf.captureWidth + 2 * padding;
    inst.fbo_h = conf.captureHeight + 2 * padding;

    bool frontComplete = CreateMirrorFramebuffer(inst.fbo, inst.fboTexture, inst.fbo_w, inst.fbo_h, GL_NEAREST);

    // Create final screen-ready FBOs for mirror composition.
    float scaleX = conf.output.separateScale ? conf.output.scaleX : conf.output.scale;
    float scaleY = conf.output.separateScale ? conf.output.scaleY : conf.output.scale;
    inst.final_w = static_cast<int>(inst.fbo_w * scaleX);
    inst.final_h = static_cast<int>(inst.fbo_h * scaleY);
    inst.final_w_back = inst.final_w;
    inst.final_h_back = inst.final_h;

    bool finalFrontComplete = CreateMirrorFramebuffer(inst.finalFbo, inst.finalTexture, inst.final_w, inst.final_h, GL_NEAREST);

    if (frontComplete && finalFrontComplete) {
        inst.captureReady.store(false, std::memory_order_relaxed);
        inst.hasValidContent = false;
        // Initialize rawOutput state from config for proper initial synchronization
        inst.desiredRawOutput.store(conf.rawOutput, std::memory_order_relaxed);
        inst.capturedAsRawOutput = conf.rawOutput;
        inst.capturedAsRawOutputBack = conf.rawOutput;
        g_mirrorInstances[conf.name] = inst;
        LogCategory("init", "Created single-buffered GPU resources for mirror '" + conf.name + "' (FBO: " +
                                std::to_string(inst.fbo) + ", FinalFBO: " + std::to_string(inst.finalFbo) + " [" +
                                std::to_string(inst.final_w) + "x" + std::to_string(inst.final_h) + "])");
    } else {
        Log("ERROR: Failed to create complete framebuffers for mirror '" + conf.name + "'");
        DeleteMirrorFramebuffer(inst.fbo, inst.fboTexture);
        DeleteMirrorFramebuffer(inst.fboBack, inst.fboTextureBack);
        DeleteMirrorFramebuffer(inst.finalFbo, inst.finalTexture);
        DeleteMirrorFramebuffer(inst.finalFboBack, inst.finalTextureBack);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, last_framebuffer);
    BindTextureDirect(GL_TEXTURE_2D, last_texture);
}

// MirrorRenderData is defined in render.h so both render units can share the layout.

static bool IsSampleableTexture2D(GLuint texture, int* outW = nullptr, int* outH = nullptr) {
    if (outW) *outW = 0;
    if (outH) *outH = 0;
    if (texture == 0 || glIsTexture(texture) != GL_TRUE) { return false; }

    GLint prevTexture = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTexture);
    BindTextureDirect(GL_TEXTURE_2D, texture);

    GLint texW = 0;
    GLint texH = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &texW);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &texH);

    BindTextureDirect(GL_TEXTURE_2D, static_cast<GLuint>(prevTexture));
    if (outW) *outW = texW;
    if (outH) *outH = texH;
    return texW > 0 && texH > 0;
}

static void LogInvalidTextureSampleThrottled(const std::string& stage, GLuint texture, int expectedW = 0, int expectedH = 0) {
    static std::unordered_map<std::string, ULONGLONG> s_lastLogByStage;
    constexpr ULONGLONG kLogIntervalMs = 2000;

    ULONGLONG now = GetTickCount64();
    auto it = s_lastLogByStage.find(stage);
    if (it != s_lastLogByStage.end() && (now - it->second) < kLogIntervalMs) { return; }
    s_lastLogByStage[stage] = now;

    int actualW = 0;
    int actualH = 0;
    bool valid = IsSampleableTexture2D(texture, &actualW, &actualH);

    std::string expected;
    if (expectedW > 0 && expectedH > 0) {
        expected = " expected=" + std::to_string(expectedW) + "x" + std::to_string(expectedH);
    }

    LogCategory("texture_ops", "Main Render: Invalid texture sample stage=" + stage + " tex=" + std::to_string(texture) +
                                   " valid=" + std::to_string(valid ? 1 : 0) + " actual=" + std::to_string(actualW) + "x" +
                                   std::to_string(actualH) + expected);
}

static int GetAnimatedTextureDelayMs(const std::vector<int>& frameDelays, size_t frameIndex) {
    int delay = 100;
    if (frameIndex < frameDelays.size() && frameDelays[frameIndex] > 0) {
        delay = frameDelays[frameIndex];
    }
    if (delay < 10) {
        delay = 100;
    }
    return delay;
}

template <typename TextureInstance>
static void InitializeAnimatedTexturePlayback(TextureInstance& inst, size_t frameCount) {
    inst.frameEndTimesMs.clear();
    inst.frameEndTimesMs.reserve(frameCount);

    uint64_t totalDurationMs = 0;
    for (size_t frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
        totalDurationMs += static_cast<uint64_t>(GetAnimatedTextureDelayMs(inst.frameDelays, frameIndex));
        inst.frameEndTimesMs.push_back(totalDurationMs);
    }

    inst.totalAnimationDurationMs = totalDurationMs;
    inst.currentFrame = 0;
    inst.lastFrameTime = std::chrono::steady_clock::now();
}

template <typename TextureInstance>
struct AnimatedTextureResolveResult {
    GLuint textureId = 0;
    float sourceRect[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
};

template <typename TextureInstance>
static AnimatedTextureResolveResult<TextureInstance> ResolveAnimatedTexture(TextureInstance& inst) {
    AnimatedTextureResolveResult<TextureInstance> result;
    result.textureId = inst.textureId;

    if (!inst.isAnimated) {
        return result;
    }

    const size_t frameCount = inst.frameCount > 1 ? static_cast<size_t>(inst.frameCount)
                                                  : (!inst.frameTextures.empty() ? inst.frameTextures.size() : 1u);
    if (frameCount <= 1) {
        return result;
    }

    if (inst.frameEndTimesMs.size() != frameCount || inst.totalAnimationDurationMs == 0) {
        InitializeAnimatedTexturePlayback(inst, frameCount);
    }

    if (inst.lastFrameTime.time_since_epoch().count() == 0) {
        inst.lastFrameTime = std::chrono::steady_clock::now();
    }

    const auto now = std::chrono::steady_clock::now();
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - inst.lastFrameTime).count();
    const uint64_t cyclePositionMs = inst.totalAnimationDurationMs > 0
                                         ? static_cast<uint64_t>((std::max)(int64_t{ 0 }, elapsedMs)) % inst.totalAnimationDurationMs
                                         : 0;
    auto frameIt = std::upper_bound(inst.frameEndTimesMs.begin(), inst.frameEndTimesMs.end(), cyclePositionMs);
    size_t resolvedFrame = static_cast<size_t>(std::distance(inst.frameEndTimesMs.begin(), frameIt));
    if (resolvedFrame >= frameCount) {
        resolvedFrame = frameCount - 1;
    }
    inst.currentFrame = resolvedFrame;

    if (!inst.frameTextures.empty()) {
        const size_t framesPerTexture = static_cast<size_t>((std::max)(1, inst.framesPerTexture));
        const size_t pageIndex = (std::min)(resolvedFrame / framesPerTexture, inst.frameTextures.size() - 1);
        const size_t frameIndexWithinPage = resolvedFrame - pageIndex * framesPerTexture;
        inst.textureId = inst.frameTextures[pageIndex];
        result.textureId = inst.textureId;

        int pageHeight = inst.textureStorageHeight > 0 ? inst.textureStorageHeight : inst.height;
        if (pageIndex < inst.frameTextureHeights.size() && inst.frameTextureHeights[pageIndex] > 0) {
            pageHeight = inst.frameTextureHeights[pageIndex];
        }
        const int frameHeight = inst.height > 0 ? inst.height : pageHeight;
        if (pageHeight > 0 && frameHeight > 0) {
            const float frameScale = static_cast<float>(frameHeight) / static_cast<float>(pageHeight);
            result.sourceRect[1] = frameScale * static_cast<float>(frameIndexWithinPage);
            result.sourceRect[3] = frameScale;
        }
        return result;
    }

    const int storageHeight = inst.textureStorageHeight > 0 ? inst.textureStorageHeight : inst.height;
    const int frameHeight = inst.height > 0 ? inst.height : storageHeight;
    if (storageHeight > 0 && frameHeight > 0) {
        const float frameScale = static_cast<float>(frameHeight) / static_cast<float>(storageHeight);
        result.sourceRect[1] = frameScale * static_cast<float>(resolvedFrame);
        result.sourceRect[3] = frameScale;
    }

    return result;
}

struct TextureSampleabilityCacheEntry {
    int width = 0;
    int height = 0;
    ULONGLONG checkedAtMs = 0;
    bool valid = false;
};

static std::unordered_map<GLuint, TextureSampleabilityCacheEntry> s_textureSampleabilityCache;
static std::mutex s_textureSampleabilityCacheMutex;

static void InvalidateTextureSampleabilityCache(GLuint texture) {
    std::lock_guard<std::mutex> lock(s_textureSampleabilityCacheMutex);
    s_textureSampleabilityCache.erase(texture);
}

static bool IsSampleableTexture2DCached(GLuint texture, int expectedW = 0, int expectedH = 0) {
    if (texture == 0) { return false; }

    constexpr ULONGLONG kCacheLifetimeMs = 250;
    const ULONGLONG now = GetTickCount64();

    {
        std::lock_guard<std::mutex> lock(s_textureSampleabilityCacheMutex);
        auto it = s_textureSampleabilityCache.find(texture);
        if (it != s_textureSampleabilityCache.end()) {
            const TextureSampleabilityCacheEntry& cached = it->second;
            const bool dimsMatch = expectedW <= 0 || expectedH <= 0 || (cached.width == expectedW && cached.height == expectedH);
            if (dimsMatch && (now - cached.checkedAtMs) <= kCacheLifetimeMs) {
                return cached.valid;
            }
        }
    }

    int actualW = 0;
    int actualH = 0;
    const bool valid = IsSampleableTexture2D(texture, &actualW, &actualH);

    {
        std::lock_guard<std::mutex> lock(s_textureSampleabilityCacheMutex);
        s_textureSampleabilityCache[texture] = { actualW, actualH, now, valid };

        if (s_textureSampleabilityCache.size() > 512) {
            for (auto it = s_textureSampleabilityCache.begin(); it != s_textureSampleabilityCache.end();) {
                if ((now - it->second.checkedAtMs) > 5000) {
                    it = s_textureSampleabilityCache.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }

    return valid;
}

static void ClearEyeZoomTextLabels() {
    std::lock_guard<std::mutex> lock(s_eyezoomTextMutex);
    s_eyezoomTextLabels.clear();
}

static void RenderCachedEyeZoomTextLabels() {
    std::vector<EyeZoomTextLabel> labels;
    {
        std::lock_guard<std::mutex> lock(s_eyezoomTextMutex);
        if (s_eyezoomTextLabels.empty()) { return; }
        labels.swap(s_eyezoomTextLabels);
    }

    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    ImFont* font = g_overlayTextFont ? g_overlayTextFont : ImGui::GetFont();
    const float requestedFontSize = (std::max)(1.0f, g_overlayTextFontSize);
    float sharedAutoFontSize = requestedFontSize;
    bool hasSharedAutoLabels = false;

    for (const auto& label : labels) {
        if (label.fontSizeMode != EyeZoomFontSizeMode::Auto) { continue; }

        hasSharedAutoLabels = true;
        const std::string text = std::to_string(label.number);
        const float maxTextWidth = (std::max)(1.0f, label.boxWidth * 0.82f);
        const float maxTextHeight = (std::max)(1.0f, label.boxHeight * 0.82f);
        const ImVec2 requestedTextSize = font->CalcTextSizeA(requestedFontSize, FLT_MAX, 0.0f, text.c_str());
        if (requestedTextSize.x <= 0.0f || requestedTextSize.y <= 0.0f) { continue; }

        const float fitScale = (std::min)((std::min)(maxTextWidth / requestedTextSize.x, maxTextHeight / requestedTextSize.y), 1.0f);
        sharedAutoFontSize = (std::max)(1.0f, (std::min)(sharedAutoFontSize, requestedFontSize * fitScale));
    }

    for (const auto& label : labels) {
        const std::string text = std::to_string(label.number);
        float fontSize = requestedFontSize;
        if (label.fontSizeMode == EyeZoomFontSizeMode::Auto && hasSharedAutoLabels) {
            fontSize = sharedAutoFontSize;
        } else if (label.fontSizeMode == EyeZoomFontSizeMode::PerSquareAuto) {
            const float maxTextWidth = (std::max)(1.0f, label.boxWidth * 0.82f);
            const float maxTextHeight = (std::max)(1.0f, label.boxHeight * 0.82f);
            const ImVec2 requestedTextSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text.c_str());
            if (requestedTextSize.x > 0.0f && requestedTextSize.y > 0.0f) {
                const float fitScale = (std::min)((std::min)(maxTextWidth / requestedTextSize.x, maxTextHeight / requestedTextSize.y), 1.0f);
                fontSize = (std::max)(1.0f, fontSize * fitScale);
            }
        }

        ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text.c_str());
        ImVec2 pos(label.centerX - textSize.x * 0.5f, label.centerY - textSize.y * 0.5f);
        ImU32 color = IM_COL32(static_cast<int>(label.color.r * 255.0f), static_cast<int>(label.color.g * 255.0f),
                               static_cast<int>(label.color.b * 255.0f), static_cast<int>(label.color.a * 255.0f));
        const bool hasClipRect = label.clipMaxX > label.clipMinX && label.clipMaxY > label.clipMinY;
        if (hasClipRect) {
            drawList->PushClipRect(ImVec2(label.clipMinX, label.clipMinY), ImVec2(label.clipMaxX, label.clipMaxY), true);
        }
        drawList->AddText(font, fontSize, pos, color, text.c_str());
        if (hasClipRect) {
            drawList->PopClipRect();
        }
    }
}

static bool SelectSameThreadGameTexture(GLuint preferredTexture, int preferredW, int preferredH, GLuint& outTexture, int& outW,
                                        int& outH) {
    if (preferredTexture != 0 && preferredTexture != UINT_MAX && preferredW > 0 && preferredH > 0) {
        outTexture = preferredTexture;
        outW = preferredW;
        outH = preferredH;
        return true;
    }

    outTexture = GetReadyGameTexture();
    outW = GetReadyGameWidth();
    outH = GetReadyGameHeight();
    if (outTexture != 0 && outW > 0 && outH > 0) { return true; }

    outTexture = GetFallbackGameTexture();
    outW = GetFallbackGameWidth();
    outH = GetFallbackGameHeight();
    if (outTexture != 0 && outW > 0 && outH > 0) { return true; }

    outTexture = GetSafeReadTexture();
    if (outTexture != 0 && IsSampleableTexture2D(outTexture, &outW, &outH)) { return true; }

    outTexture = 0;
    outW = 0;
    outH = 0;
    return false;
}

static bool SelectEyeZoomCaptureTexture(GLuint preferredTexture, int preferredW, int preferredH, GLuint& outTexture, int& outW,
                                        int& outH, const char** outSourceName = nullptr, bool preferPreferredFirst = false) {
    if (outSourceName) { *outSourceName = "none"; }

    (void)preferredW;
    (void)preferredH;
    (void)preferPreferredFirst;

    if (preferredTexture != 0 && preferredTexture != UINT_MAX && IsSampleableTexture2D(preferredTexture, &outW, &outH)) {
        outTexture = preferredTexture;
        if (outSourceName) { *outSourceName = "preferred"; }
        return true;
    }

    outTexture = 0;
    outW = 0;
    outH = 0;
    return false;
}

static void LogEyeZoomDebugThrottled(const char* stage, const std::string& message) {
    struct EyeZoomDebugLogState {
        ULONGLONG lastLogMs = 0;
        std::string lastMessage;
    };

    static std::unordered_map<std::string, EyeZoomDebugLogState> s_logStateByStage;
    constexpr ULONGLONG kLogIntervalMs = 2000;

    const ULONGLONG now = GetTickCount64();
    EyeZoomDebugLogState& state = s_logStateByStage[stage];
    if (state.lastMessage == message && (now - state.lastLogMs) < kLogIntervalMs) { return; }

    state.lastLogMs = now;
    state.lastMessage = message;
    LogCategory("texture_ops", std::string("EyeZoom: ") + stage + " " + message);
}

static void LogEyeZoomFramebufferStatusThrottled(const char* stage, GLuint texture, GLenum status, int width, int height) {
    static std::unordered_map<std::string, ULONGLONG> s_lastLogByStage;
    constexpr ULONGLONG kLogIntervalMs = 2000;

    const ULONGLONG now = GetTickCount64();
    auto it = s_lastLogByStage.find(stage);
    if (it != s_lastLogByStage.end() && (now - it->second) < kLogIntervalMs) { return; }
    s_lastLogByStage[stage] = now;

    LogCategory("texture_ops",
                std::string("EyeZoom: framebuffer incomplete at ") + stage + " status=" + std::to_string(status) +
                    " tex=" + std::to_string(texture) + " size=" + std::to_string(width) + "x" + std::to_string(height));
}

static void DrawPassthroughTextureRegion(GLuint textureId, const float sourceRect[4], int dstLeft, int dstBottom, int dstRight,
                                         int dstTop, int fullW, int fullH, float opacity, bool snapToSourcePixels = false,
                                         int textureWidth = 0, int textureHeight = 0, int sourcePixelWidth = 0,
                                         int sourcePixelHeight = 0) {
    if (textureId == 0 || dstRight <= dstLeft || dstTop <= dstBottom || fullW <= 0 || fullH <= 0) { return; }

    ScopedTextureFilterGuard samplingGuard(textureId, GL_NEAREST, GL_NEAREST, GL_TEXTURE0, true);
    const bool usePixelSnapping =
        snapToSourcePixels && textureWidth > 0 && textureHeight > 0 && sourcePixelWidth > 0 && sourcePixelHeight > 0;

    glUseProgram(g_passthroughProgram);
    glActiveTexture(GL_TEXTURE0);
    BindTextureDirect(GL_TEXTURE_2D, textureId);
    glUniform4f(g_passthroughShaderLocs.sourceRect, sourceRect[0], sourceRect[1], sourceRect[2], sourceRect[3]);
    glUniform1f(g_passthroughShaderLocs.opacity, opacity);
    glUniform2f(g_passthroughShaderLocs.sourceTexelSize,
                usePixelSnapping ? 1.0f / static_cast<float>(textureWidth) : 1.0f,
                usePixelSnapping ? 1.0f / static_cast<float>(textureHeight) : 1.0f);
    glUniform2f(g_passthroughShaderLocs.sourcePixelSize,
                usePixelSnapping ? static_cast<float>(sourcePixelWidth) : 1.0f,
                usePixelSnapping ? static_cast<float>(sourcePixelHeight) : 1.0f);
    glUniform1i(g_passthroughShaderLocs.snapToSourcePixels, usePixelSnapping ? 1 : 0);

    glBindVertexArray(g_fullscreenQuadVAO);
    const int regionW = dstRight - dstLeft;
    const int regionH = dstTop - dstBottom;
    if (oglViewport) {
        oglViewport(dstLeft, dstBottom, regionW, regionH);
    } else {
        glViewport(dstLeft, dstBottom, regionW, regionH);
    }
    glDrawArrays(GL_TRIANGLES, 0, 6);
    if (oglViewport) {
        oglViewport(0, 0, fullW, fullH);
    } else {
        glViewport(0, 0, fullW, fullH);
    }
}

static void AppendUniqueMirrorsByName(std::vector<MirrorConfig>& dest, const std::vector<MirrorConfig>& src) {
    if (src.empty()) return;

    std::unordered_set<std::string> seen;
    seen.reserve(dest.size() + src.size());
    for (const auto& mirror : dest) {
        seen.insert(mirror.name);
    }

    for (const auto& mirror : src) {
        if (seen.insert(mirror.name).second) {
            dest.push_back(mirror);
        }
    }
}

static void RenderMirrorsDirect(const std::vector<MirrorConfig>& activeMirrors, const GameViewportGeometry& geo, int fullW, int fullH,
                                float modeOpacity, bool excludeOnlyOnMyScreen, bool relativeStretching, float transitionProgress,
                                float mirrorSlideProgress, int fromX, int fromY, int fromW, int fromH, int toX, int toY, int toW,
                                int toH, int fromFullW, int fromFullH, bool isEyeZoomMode, bool isTransitioningFromEyeZoom,
                                int eyeZoomAnimatedViewportX,
                                bool skipAnimation, const std::string& fromModeId, bool fromSlideMirrorsIn, bool toSlideMirrorsIn,
                                bool isSlideOutPass, const Config& cfg) {
    if (activeMirrors.empty()) return;

    const bool isAnimating = transitionProgress < 1.0f;
    const float toScaleX = (toW > 0 && geo.gameW > 0) ? static_cast<float>(toW) / geo.gameW : 1.0f;
    const float toScaleY = (toH > 0 && geo.gameH > 0) ? static_cast<float>(toH) / geo.gameH : 1.0f;
    const float fromScaleX = (fromW > 0 && geo.gameW > 0) ? static_cast<float>(fromW) / geo.gameW : toScaleX;
    const float fromScaleY = (fromH > 0 && geo.gameH > 0) ? static_cast<float>(fromH) / geo.gameH : toScaleY;
    const int effectiveFromH = isTransitioningFromEyeZoom ? toH : fromH;
    const int effectiveFromY = isTransitioningFromEyeZoom ? toY : fromY;
    const bool wantsTransitionSlide = mirrorSlideProgress < 1.0f && !skipAnimation;
    const int targetViewportX = GetCenteredAxisOffset(fullW, cfg.eyezoom.windowWidth);
    const bool hasEyeZoomAnimatedPosition = eyeZoomAnimatedViewportX >= 0 && targetViewportX > 0;
    const bool isEyeZoomTransitioning = hasEyeZoomAnimatedPosition && eyeZoomAnimatedViewportX < targetViewportX;
    const bool wantsEyeZoomSlide =
        cfg.eyezoom.slideMirrorsIn && hasEyeZoomAnimatedPosition && isEyeZoomMode && isEyeZoomTransitioning;
    const float eyeZoomSlideProgress = wantsEyeZoomSlide ? static_cast<float>(eyeZoomAnimatedViewportX) / targetViewportX : 1.0f;
    std::vector<MirrorConfig> sourceMirrors;
    std::unordered_map<std::string, const MirrorConfig*> sourceMirrorConfigs;
    if (!fromModeId.empty() && (isAnimating || fromSlideMirrorsIn || toSlideMirrorsIn || cfg.eyezoom.slideMirrorsIn)) {
        std::vector<ImageConfig> unusedImages;
        std::vector<WindowOverlayConfig> unusedWindowOverlays;
        std::vector<BrowserOverlayConfig> unusedBrowserOverlays;
        const int resolvedSourceScreenW = fromFullW > 0 ? fromFullW : fullW;
        const int resolvedSourceScreenH = fromFullH > 0 ? fromFullH : fullH;
        CollectActiveElementsForMode(cfg, fromModeId, false, sourceMirrors, unusedImages, unusedWindowOverlays,
                                     unusedBrowserOverlays, resolvedSourceScreenW, resolvedSourceScreenH);
        sourceMirrorConfigs.reserve(sourceMirrors.size());
        for (const auto& sourceMirror : sourceMirrors) {
            sourceMirrorConfigs[sourceMirror.name] = &sourceMirror;
        }
    }
    const bool allowCachedMirrorVertices = !isAnimating && !wantsTransitionSlide && !wantsEyeZoomSlide && sourceMirrorConfigs.empty();

    auto splitRelativeAnchor = [](const std::string& relativeTo, std::string& anchorOut, bool& isScreenRelativeOut) {
        anchorOut = relativeTo;
        isScreenRelativeOut = false;
        if (anchorOut.length() > 6 && anchorOut.substr(anchorOut.length() - 6) == "Screen") {
            anchorOut = anchorOut.substr(0, anchorOut.length() - 6);
            isScreenRelativeOut = true;
        } else if (anchorOut.length() > 8 && anchorOut.substr(anchorOut.length() - 8) == "Viewport") {
            anchorOut = anchorOut.substr(0, anchorOut.length() - 8);
        }
    };

    struct MirrorLayoutState {
        int finalXScreen = 0;
        int finalYScreen = 0;
        int finalWScreen = 0;
        int finalHScreen = 0;
        int slideAnchorX = 0;
        int slideAnchorW = 0;
        bool shouldApplySlide = false;
        float slideProgress = 1.0f;
    };

    struct GroupSlideBounds {
        int minX = 0;
        int maxX = 0;
        bool valid = false;
    };

    auto resolveSourceConfig = [&](const MirrorConfig& conf) -> const MirrorConfig* {
        if (isSlideOutPass) {
            return nullptr;
        }

        auto sourceIt = sourceMirrorConfigs.find(conf.name);
        if (sourceIt == sourceMirrorConfigs.end()) {
            return nullptr;
        }

        return sourceIt->second;
    };

    auto resolveMirrorLayout = [&](const MirrorConfig& conf, const MirrorConfig* sourceConf, int renderedOutW,
                                   int renderedOutH) {
        MirrorLayoutState layout{};

        std::string targetAnchor;
        bool targetIsScreenRelative = false;
        splitRelativeAnchor(conf.output.relativeTo, targetAnchor, targetIsScreenRelative);

        std::string sourceAnchor = targetAnchor;
        bool sourceIsScreenRelative = targetIsScreenRelative;
        if (sourceConf) {
            splitRelativeAnchor(sourceConf->output.relativeTo, sourceAnchor, sourceIsScreenRelative);
        }

        const float targetScaleBaseX = conf.output.separateScale ? conf.output.scaleX : conf.output.scale;
        const float targetScaleBaseY = conf.output.separateScale ? conf.output.scaleY : conf.output.scale;
        float sourceScaleBaseX = targetScaleBaseX;
        float sourceScaleBaseY = targetScaleBaseY;
        if (sourceConf) {
            sourceScaleBaseX = sourceConf->output.separateScale ? sourceConf->output.scaleX : sourceConf->output.scale;
            sourceScaleBaseY = sourceConf->output.separateScale ? sourceConf->output.scaleY : sourceConf->output.scale;
        }

        int targetBaseW = renderedOutW;
        int targetBaseH = renderedOutH;
        int sourceBaseW = renderedOutW;
        int sourceBaseH = renderedOutH;
        if (sourceConf) {
            if (targetScaleBaseX > 0.0f) {
                sourceBaseW = static_cast<int>(renderedOutW * (sourceScaleBaseX / targetScaleBaseX));
            }
            if (targetScaleBaseY > 0.0f) {
                sourceBaseH = static_cast<int>(renderedOutH * (sourceScaleBaseY / targetScaleBaseY));
            }
        }

        const int targetSizeW = relativeStretching ? static_cast<int>(targetBaseW * toScaleX) : targetBaseW;
        const int targetSizeH = relativeStretching ? static_cast<int>(targetBaseH * toScaleY) : targetBaseH;
        const int sourceSizeW = relativeStretching ? static_cast<int>(sourceBaseW * fromScaleX) : sourceBaseW;
        const int sourceSizeH = relativeStretching ? static_cast<int>(sourceBaseH * fromScaleY) : sourceBaseH;

        int toPosX = 0;
        int toPosY = 0;
        if (targetIsScreenRelative) {
            GetRelativeCoords(targetAnchor, conf.output.x, conf.output.y, targetSizeW, targetSizeH, fullW, fullH, toPosX, toPosY);
        } else {
            int toOutX = 0;
            int toOutY = 0;
            GetRelativeCoords(targetAnchor, conf.output.x, conf.output.y, targetSizeW, targetSizeH, toW, toH, toOutX, toOutY);
            toPosX = toX + toOutX;
            toPosY = toY + toOutY;
        }

        int fromPosX = toPosX;
        int fromPosY = toPosY;
        if (sourceConf) {
            if (sourceIsScreenRelative) {
                GetRelativeCoords(sourceAnchor, sourceConf->output.x, sourceConf->output.y, sourceSizeW, sourceSizeH, fullW, fullH,
                                  fromPosX, fromPosY);
            } else {
                int fromOutX = 0;
                int fromOutY = 0;
                const int effectiveFromSizeH = isTransitioningFromEyeZoom ? targetSizeH : sourceSizeH;
                GetRelativeCoords(sourceAnchor, sourceConf->output.x, sourceConf->output.y, sourceSizeW, effectiveFromSizeH, fromW,
                                  effectiveFromH, fromOutX, fromOutY);
                fromPosX = fromX + fromOutX;
                fromPosY = effectiveFromY + fromOutY;
            }
        } else if (!targetIsScreenRelative) {
            int fromOutX = 0;
            int fromOutY = 0;
            const int effectiveFromSizeH = isTransitioningFromEyeZoom ? targetSizeH : sourceSizeH;
            GetRelativeCoords(targetAnchor, conf.output.x, conf.output.y, sourceSizeW, effectiveFromSizeH, fromW, effectiveFromH,
                              fromOutX, fromOutY);
            fromPosX = fromX + fromOutX;
            fromPosY = effectiveFromY + fromOutY;
        }

        layout.finalWScreen = targetSizeW;
        layout.finalHScreen = targetSizeH;
        auto applyLayoutProgress = [&](float layoutProgress) {
            layout.finalXScreen = static_cast<int>(fromPosX + (toPosX - fromPosX) * layoutProgress);
            layout.finalYScreen = static_cast<int>(fromPosY + (toPosY - fromPosY) * layoutProgress);
            if (sourceConf || relativeStretching) {
                layout.finalWScreen = static_cast<int>(sourceSizeW + (targetSizeW - sourceSizeW) * layoutProgress);
                layout.finalHScreen = static_cast<int>(sourceSizeH + (targetSizeH - sourceSizeH) * layoutProgress);
            }
        };
        applyLayoutProgress(transitionProgress);

        layout.slideAnchorX = layout.finalXScreen;
        layout.slideAnchorW = layout.finalWScreen;
        if (wantsTransitionSlide) {
            if (fromSlideMirrorsIn && isSlideOutPass) {
                layout.slideAnchorX = fromPosX;
                layout.slideAnchorW = sourceSizeW;
            } else if (toSlideMirrorsIn && !isSlideOutPass) {
                layout.slideAnchorX = toPosX;
                layout.slideAnchorW = targetSizeW;
            }
        }

        const bool isTransitioningToEyeZoom = wantsEyeZoomSlide && !isTransitioningFromEyeZoom;
        const bool isEyeZoomSlideOut = wantsEyeZoomSlide && isTransitioningFromEyeZoom;
        if (isTransitioningToEyeZoom || isEyeZoomSlideOut) {
            layout.shouldApplySlide = true;
            layout.slideProgress = eyeZoomSlideProgress;
        }
        if (!layout.shouldApplySlide && wantsTransitionSlide) {
            if (toSlideMirrorsIn && !isSlideOutPass) {
                layout.shouldApplySlide = true;
                layout.slideProgress = mirrorSlideProgress;
            } else if (fromSlideMirrorsIn && isSlideOutPass) {
                layout.shouldApplySlide = true;
                layout.slideProgress = 1.0f - mirrorSlideProgress;
            }
        }

        bool shouldUseSharedOnScreenLerp = false;
        if (wantsTransitionSlide && !isSlideOutPass && sourceConf != nullptr) {
            layout.shouldApplySlide = false;
            shouldUseSharedOnScreenLerp = (fromPosX != toPosX) || (fromPosY != toPosY) || (sourceSizeW != targetSizeW) ||
                                          (sourceSizeH != targetSizeH);
        }
        if (shouldUseSharedOnScreenLerp) {
            applyLayoutProgress((std::max)(0.0f, (std::min)(1.0f, mirrorSlideProgress)));
        }

        return layout;
    };

    std::vector<MirrorRenderData> mirrorsToRender;
    mirrorsToRender.reserve(activeMirrors.size());

    {
        PROFILE_SCOPE_CAT("Collect Mirror Render Data", "Rendering");
        std::shared_lock<std::shared_mutex> mirrorLock(g_mirrorInstancesMutex);
        for (const auto& conf : activeMirrors) {
            if (excludeOnlyOnMyScreen && conf.onlyOnMyScreen) continue;

            const float effectiveOpacity = modeOpacity * conf.opacity;
            if (effectiveOpacity <= 0.0f) continue;

            auto it = g_mirrorInstances.find(conf.name);
            if (it == g_mirrorInstances.end()) continue;
            const MirrorInstance& inst = it->second;
            if (!inst.hasValidContent) continue;
            const bool useDynamicBorderComposite =
                conf.border.type == MirrorBorderType::Dynamic && conf.border.dynamicThickness == 1 && !conf.gradientOutput &&
                !inst.capturedAsRawOutput;

            MirrorRenderData data{};
            data.config = &conf;
            float scaleX = conf.output.separateScale ? conf.output.scaleX : conf.output.scale;
            float scaleY = conf.output.separateScale ? conf.output.scaleY : conf.output.scale;
            if (useDynamicBorderComposite) {
                data.texture = inst.fboTexture;
                data.tex_w = inst.fbo_w;
                data.tex_h = inst.fbo_h;
                data.useDynamicBorderComposite = true;
            } else if (inst.finalTexture != 0 && inst.final_w > 0 && inst.final_h > 0) {
                data.texture = inst.finalTexture;
                data.tex_w = inst.final_w;
                data.tex_h = inst.final_h;
            } else {
                data.texture = inst.fboTexture;
                data.tex_w = inst.fbo_w;
                data.tex_h = inst.fbo_h;
            }
            data.outW = static_cast<int>(inst.fbo_w * scaleX);
            data.outH = static_cast<int>(inst.fbo_h * scaleY);
            data.hasFrameContent = inst.hasFrameContent;

            const auto& cache = inst.cachedRenderState;
            const bool cacheMatchesCurrentGeo =
                allowCachedMirrorVertices && cache.isValid && cache.finalX == geo.finalX && cache.finalY == geo.finalY &&
                cache.finalW == geo.finalW && cache.gameW == geo.gameW && cache.gameH == geo.gameH &&
                cache.finalH == geo.finalH && cache.screenW == fullW && cache.screenH == fullH && cache.outputX == conf.output.x &&
                cache.outputY == conf.output.y && cache.outputScale == conf.output.scale &&
                cache.outputSeparateScale == conf.output.separateScale && cache.outputScaleX == conf.output.scaleX &&
                cache.outputScaleY == conf.output.scaleY && cache.outputRelativeTo == conf.output.relativeTo;
            if (cacheMatchesCurrentGeo) {
                memcpy(data.vertices, cache.vertices, sizeof(data.vertices));
                data.screenX = cache.mirrorScreenX;
                data.screenY = cache.mirrorScreenY;
                data.screenW = cache.mirrorScreenW;
                data.screenH = cache.mirrorScreenH;
                data.cacheValid = true;
            }

            mirrorsToRender.push_back(data);
        }
    }

    if (mirrorsToRender.empty()) return;

    std::unordered_map<std::string, GroupSlideBounds> groupedSlideBounds;
    if (wantsTransitionSlide || wantsEyeZoomSlide) {
        for (const auto& renderData : mirrorsToRender) {
            const MirrorConfig& conf = *renderData.config;
            if (!conf.runtimeGrouped || conf.runtimeGroupName.empty()) {
                continue;
            }

            const MirrorConfig* sourceConf = resolveSourceConfig(conf);
            const MirrorLayoutState layout = resolveMirrorLayout(conf, sourceConf, renderData.outW, renderData.outH);
            if (!layout.shouldApplySlide) {
                continue;
            }

            GroupSlideBounds& bounds = groupedSlideBounds[conf.runtimeGroupName];
            const int slideMinX = layout.slideAnchorX;
            const int slideMaxX = layout.slideAnchorX + layout.slideAnchorW;
            if (!bounds.valid) {
                bounds.minX = slideMinX;
                bounds.maxX = slideMaxX;
                bounds.valid = true;
            } else {
                bounds.minX = (std::min)(bounds.minX, slideMinX);
                bounds.maxX = (std::max)(bounds.maxX, slideMaxX);
            }
        }
    }

    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    GLuint lastBoundMirrorTexture = 0;
    float lastMirrorOpacity = -1.0f;
    GLuint lastMirrorProgram = 0;
    bool renderUniformsValid = false;
    int lastRenderBorderWidth = -1;
    float lastRenderOutputColor[4] = { -1.0f, -1.0f, -1.0f, -1.0f };
    float lastRenderBorderColor[4] = { -1.0f, -1.0f, -1.0f, -1.0f };
    float lastRenderScreenPixelX = -1.0f;
    float lastRenderScreenPixelY = -1.0f;
    bool renderPassthroughUniformsValid = false;
    int lastRenderPassthroughBorderWidth = -1;
    float lastRenderPassthroughBorderColor[4] = { -1.0f, -1.0f, -1.0f, -1.0f };
    float lastRenderPassthroughScreenPixelX = -1.0f;
    float lastRenderPassthroughScreenPixelY = -1.0f;
    float lastRenderPassthroughOpacity = -1.0f;

    auto bindMirrorProgram = [&](GLuint program) {
        if (lastMirrorProgram != program) {
            glUseProgram(program);
            lastMirrorProgram = program;
            lastBoundMirrorTexture = 0;
            lastMirrorOpacity = -1.0f;
        }
    };

    {
        PROFILE_SCOPE_CAT("Render Mirror Textures", "Rendering");
        for (auto& renderData : mirrorsToRender) {
            const MirrorConfig& conf = *renderData.config;
            const float effectiveOpacity = modeOpacity * conf.opacity;
            if (effectiveOpacity <= 0.0f) continue;

            if (renderData.useDynamicBorderComposite) {
                const float screenPixelX = 1.0f / static_cast<float>((std::max)(1, renderData.outW));
                const float screenPixelY = 1.0f / static_cast<float>((std::max)(1, renderData.outH));

                if (conf.colorPassthrough) {
                    bindMirrorProgram(g_renderPassthroughProgram);
                    const float borderColor[4] = { conf.colors.border.r, conf.colors.border.g, conf.colors.border.b,
                                                   conf.colors.border.a * effectiveOpacity };
                    if (!renderPassthroughUniformsValid || lastRenderPassthroughBorderWidth != conf.border.dynamicThickness) {
                        glUniform1i(g_renderPassthroughShaderLocs.borderWidth, conf.border.dynamicThickness);
                        lastRenderPassthroughBorderWidth = conf.border.dynamicThickness;
                    }
                    if (!renderPassthroughUniformsValid || lastRenderPassthroughBorderColor[0] != borderColor[0] ||
                        lastRenderPassthroughBorderColor[1] != borderColor[1] ||
                        lastRenderPassthroughBorderColor[2] != borderColor[2] ||
                        lastRenderPassthroughBorderColor[3] != borderColor[3]) {
                        glUniform4f(g_renderPassthroughShaderLocs.borderColor, borderColor[0], borderColor[1], borderColor[2],
                                    borderColor[3]);
                        memcpy(lastRenderPassthroughBorderColor, borderColor, sizeof(borderColor));
                    }
                    if (!renderPassthroughUniformsValid || lastRenderPassthroughScreenPixelX != screenPixelX ||
                        lastRenderPassthroughScreenPixelY != screenPixelY) {
                        glUniform2f(g_renderPassthroughShaderLocs.screenPixel, screenPixelX, screenPixelY);
                        lastRenderPassthroughScreenPixelX = screenPixelX;
                        lastRenderPassthroughScreenPixelY = screenPixelY;
                    }
                    if (!renderPassthroughUniformsValid || lastRenderPassthroughOpacity != effectiveOpacity) {
                        glUniform1f(g_renderPassthroughShaderLocs.opacity, effectiveOpacity);
                        lastRenderPassthroughOpacity = effectiveOpacity;
                    }
                    renderPassthroughUniformsValid = true;
                } else {
                    bindMirrorProgram(g_renderProgram);
                    const float outputColor[4] = { conf.colors.output.r, conf.colors.output.g, conf.colors.output.b,
                                                   conf.colors.output.a * effectiveOpacity };
                    const float borderColor[4] = { conf.colors.border.r, conf.colors.border.g, conf.colors.border.b,
                                                   conf.colors.border.a * effectiveOpacity };
                    if (!renderUniformsValid || lastRenderBorderWidth != conf.border.dynamicThickness) {
                        glUniform1i(g_renderShaderLocs.borderWidth, conf.border.dynamicThickness);
                        lastRenderBorderWidth = conf.border.dynamicThickness;
                    }
                    if (!renderUniformsValid || lastRenderOutputColor[0] != outputColor[0] ||
                        lastRenderOutputColor[1] != outputColor[1] || lastRenderOutputColor[2] != outputColor[2] ||
                        lastRenderOutputColor[3] != outputColor[3]) {
                        glUniform4f(g_renderShaderLocs.outputColor, outputColor[0], outputColor[1], outputColor[2], outputColor[3]);
                        memcpy(lastRenderOutputColor, outputColor, sizeof(outputColor));
                    }
                    if (!renderUniformsValid || lastRenderBorderColor[0] != borderColor[0] ||
                        lastRenderBorderColor[1] != borderColor[1] || lastRenderBorderColor[2] != borderColor[2] ||
                        lastRenderBorderColor[3] != borderColor[3]) {
                        glUniform4f(g_renderShaderLocs.borderColor, borderColor[0], borderColor[1], borderColor[2], borderColor[3]);
                        memcpy(lastRenderBorderColor, borderColor, sizeof(borderColor));
                    }
                    if (!renderUniformsValid || lastRenderScreenPixelX != screenPixelX || lastRenderScreenPixelY != screenPixelY) {
                        glUniform2f(g_renderShaderLocs.screenPixel, screenPixelX, screenPixelY);
                        lastRenderScreenPixelX = screenPixelX;
                        lastRenderScreenPixelY = screenPixelY;
                    }
                    renderUniformsValid = true;
                }
            } else {
                bindMirrorProgram(g_backgroundProgram);
                if (lastMirrorOpacity != effectiveOpacity) {
                    glUniform1f(g_backgroundShaderLocs.opacity, effectiveOpacity);
                    lastMirrorOpacity = effectiveOpacity;
                }
            }

            if (lastBoundMirrorTexture != renderData.texture) {
                BindTextureDirect(GL_TEXTURE_2D, renderData.texture);
                lastBoundMirrorTexture = renderData.texture;
            }

            if (renderData.cacheValid) {
                glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(renderData.vertices), renderData.vertices);
            } else {
                const MirrorConfig* sourceConf = resolveSourceConfig(conf);
                const MirrorLayoutState layout = resolveMirrorLayout(conf, sourceConf, renderData.outW, renderData.outH);

                int finalXScreen = layout.finalXScreen;
                if (layout.shouldApplySlide) {
                    const float slideProgress = (std::max)(0.0f, (std::min)(1.0f, layout.slideProgress));
                    auto groupedBoundsIt = groupedSlideBounds.end();
                    if (conf.runtimeGrouped && !conf.runtimeGroupName.empty()) {
                        groupedBoundsIt = groupedSlideBounds.find(conf.runtimeGroupName);
                    }

                    if (groupedBoundsIt != groupedSlideBounds.end() && groupedBoundsIt->second.valid) {
                        const int groupMinX = groupedBoundsIt->second.minX;
                        const int groupWidth = (std::max)(1, groupedBoundsIt->second.maxX - groupedBoundsIt->second.minX);
                        const bool isOnLeftSide = (groupMinX + groupWidth / 2) < (fullW / 2);
                        int groupedSlideX = 0;
                        if (isOnLeftSide) {
                            groupedSlideX = -groupWidth + static_cast<int>((groupMinX + groupWidth) * slideProgress);
                        } else {
                            groupedSlideX = fullW - static_cast<int>((fullW - groupMinX) * slideProgress);
                        }
                        finalXScreen += groupedSlideX - groupMinX;
                    } else {
                        const bool isOnLeftSide = (layout.slideAnchorX + layout.slideAnchorW / 2) < (fullW / 2);
                        if (isOnLeftSide) {
                            finalXScreen = -layout.finalWScreen + static_cast<int>((layout.slideAnchorX + layout.finalWScreen) * slideProgress);
                        } else {
                            finalXScreen = fullW - static_cast<int>((fullW - layout.slideAnchorX) * slideProgress);
                        }
                    }
                }

                renderData.screenX = finalXScreen;
                renderData.screenY = layout.finalYScreen;
                renderData.screenW = layout.finalWScreen;
                renderData.screenH = layout.finalHScreen;

                int finalYGl = fullH - renderData.screenY - renderData.screenH;
            float nx1 = (static_cast<float>(finalXScreen) / fullW) * 2.0f - 1.0f;
            float ny1 = (static_cast<float>(finalYGl) / fullH) * 2.0f - 1.0f;
                float nx2 = (static_cast<float>(finalXScreen + renderData.screenW) / fullW) * 2.0f - 1.0f;
                float ny2 = (static_cast<float>(finalYGl + renderData.screenH) / fullH) * 2.0f - 1.0f;
            float verts[] = { nx1, ny1, 0, 0, nx2, ny1, 1, 0, nx2, ny2, 1, 1, nx1, ny1, 0, 0, nx2, ny2, 1, 1, nx1, ny2, 0, 1 };
            glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
            }

            const bool needsScaledMirrorSampling =
                renderData.texture != 0 && renderData.screenW > 0 && renderData.screenH > 0 &&
                (renderData.screenW != renderData.tex_w || renderData.screenH != renderData.tex_h);
            if (needsScaledMirrorSampling) {
                ScopedTextureFilterGuard mirrorSamplingGuard(renderData.texture, GL_NEAREST, GL_NEAREST);
                glDrawArrays(GL_TRIANGLES, 0, 6);
            } else {
                glDrawArrays(GL_TRIANGLES, 0, 6);
            }
        }
    }

    {
        PROFILE_SCOPE_CAT("Render Static Mirror Borders", "Rendering");
        glUseProgram(g_staticBorderProgram);

        bool staticBorderUniformsValid = false;
        int lastStaticBorderShape = 0;
        float lastStaticBorderColorR = 0.0f;
        float lastStaticBorderColorG = 0.0f;
        float lastStaticBorderColorB = 0.0f;
        float lastStaticBorderColorA = 0.0f;
        float lastStaticBorderThickness = 0.0f;
        float lastStaticBorderRadius = 0.0f;
        float lastStaticBorderBaseW = 0.0f;
        float lastStaticBorderBaseH = 0.0f;
        float lastStaticBorderQuadW = 0.0f;
        float lastStaticBorderQuadH = 0.0f;

        for (const auto& renderData : mirrorsToRender) {
            const MirrorConfig& conf = *renderData.config;
            const MirrorBorderConfig& border = conf.border;
            if (border.type != MirrorBorderType::Static || border.staticThickness <= 0 || !renderData.hasFrameContent ||
                renderData.screenW <= 0 || renderData.screenH <= 0) {
                continue;
            }

            int baseW = (border.staticWidth > 0) ? border.staticWidth : renderData.screenW;
            int baseH = (border.staticHeight > 0) ? border.staticHeight : renderData.screenH;
            int borderExtension = border.staticThickness + 1;
            int quadW = baseW + borderExtension * 2;
            int quadH = baseH + borderExtension * 2;
            int centerOffsetX = (baseW - renderData.screenW) / 2;
            int centerOffsetY = (baseH - renderData.screenH) / 2;
            int quadX = renderData.screenX - centerOffsetX + border.staticOffsetX - borderExtension;
            int quadY = renderData.screenY - centerOffsetY + border.staticOffsetY - borderExtension;
            int quadYGl = fullH - (quadY + quadH);
            const int shape = static_cast<int>(border.staticShape);
            const float borderColorR = border.staticColor.r;
            const float borderColorG = border.staticColor.g;
            const float borderColorB = border.staticColor.b;
            const float borderColorA = border.staticColor.a * conf.opacity * modeOpacity;
            const float borderThickness = static_cast<float>(border.staticThickness);
            const float borderRadius = static_cast<float>(border.staticRadius);
            const float baseWF = static_cast<float>(baseW);
            const float baseHF = static_cast<float>(baseH);
            const float quadWF = static_cast<float>(quadW);
            const float quadHF = static_cast<float>(quadH);

            if (!staticBorderUniformsValid || lastStaticBorderShape != shape) {
                glUniform1i(g_staticBorderShaderLocs.shape, shape);
                lastStaticBorderShape = shape;
            }
            if (!staticBorderUniformsValid || lastStaticBorderColorR != borderColorR ||
                lastStaticBorderColorG != borderColorG || lastStaticBorderColorB != borderColorB ||
                lastStaticBorderColorA != borderColorA) {
                glUniform4f(g_staticBorderShaderLocs.borderColor, borderColorR, borderColorG, borderColorB, borderColorA);
                lastStaticBorderColorR = borderColorR;
                lastStaticBorderColorG = borderColorG;
                lastStaticBorderColorB = borderColorB;
                lastStaticBorderColorA = borderColorA;
            }
            if (!staticBorderUniformsValid || lastStaticBorderThickness != borderThickness) {
                glUniform1f(g_staticBorderShaderLocs.thickness, borderThickness);
                lastStaticBorderThickness = borderThickness;
            }
            if (!staticBorderUniformsValid || lastStaticBorderRadius != borderRadius) {
                glUniform1f(g_staticBorderShaderLocs.radius, borderRadius);
                lastStaticBorderRadius = borderRadius;
            }
            if (!staticBorderUniformsValid || lastStaticBorderBaseW != baseWF || lastStaticBorderBaseH != baseHF) {
                glUniform2f(g_staticBorderShaderLocs.size, baseWF, baseHF);
                lastStaticBorderBaseW = baseWF;
                lastStaticBorderBaseH = baseHF;
            }
            if (!staticBorderUniformsValid || lastStaticBorderQuadW != quadWF || lastStaticBorderQuadH != quadHF) {
                glUniform2f(g_staticBorderShaderLocs.quadSize, quadWF, quadHF);
                lastStaticBorderQuadW = quadWF;
                lastStaticBorderQuadH = quadHF;
            }
            staticBorderUniformsValid = true;

            float nx1 = (static_cast<float>(quadX) / fullW) * 2.0f - 1.0f;
            float ny1 = (static_cast<float>(quadYGl) / fullH) * 2.0f - 1.0f;
            float nx2 = (static_cast<float>(quadX + quadW) / fullW) * 2.0f - 1.0f;
            float ny2 = (static_cast<float>(quadYGl + quadH) / fullH) * 2.0f - 1.0f;
            float verts[] = { nx1, ny1, 0, 0, nx2, ny1, 1, 0, nx2, ny2, 1, 1, nx1, ny1, 0, 0, nx2, ny2, 1, 1, nx1, ny2, 0, 1 };
            glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        }
    }

    glDisable(GL_BLEND);
}

static void RenderImagesDirect(const std::vector<ImageConfig>& activeImages, int fullW, int fullH, int gameX, int gameY, int gameW,
                               int gameH, int gameResW, int gameResH, bool relativeStretching, float transitionProgress, int fromX,
                               int fromY, int fromW, int fromH, float modeOpacity, bool excludeOnlyOnMyScreen) {
    if (activeImages.empty()) return;

    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    for (const auto& conf : activeImages) {
        if (excludeOnlyOnMyScreen && conf.onlyOnMyScreen) continue;

        const float effectiveOpacity = conf.opacity * modeOpacity;
        const bool hasBg = conf.background.enabled && conf.background.opacity > 0.0f;
        const bool hasBorder = conf.border.enabled && conf.border.width > 0;
        if (effectiveOpacity <= 0.0f && !hasBg && !hasBorder) continue;

        GLuint texId = 0;
        int texWidth = 0;
        int texHeight = 0;
        bool isFullyTransparent = false;
        float animatedSourceRect[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
        {
            std::lock_guard<std::mutex> lock(g_userImagesMutex);
            auto it = g_userImages.find(conf.name);
            if (it == g_userImages.end() || it->second.textureId == 0) continue;
            UserImageInstance& inst = it->second;
            const auto animatedTexture = ResolveAnimatedTexture(inst);
            texId = animatedTexture.textureId;
            texWidth = inst.width;
            texHeight = inst.height;
            isFullyTransparent = inst.isFullyTransparent;
            memcpy(animatedSourceRect, animatedTexture.sourceRect, sizeof(animatedSourceRect));

            if (!inst.filterInitialized || inst.lastPixelatedScaling != conf.pixelatedScaling) {
                const GLint minFilter = conf.pixelatedScaling ? GL_NEAREST : GL_LINEAR;
                const GLint magFilter = conf.pixelatedScaling ? GL_NEAREST : GL_LINEAR;
                if (inst.isAnimated && !inst.frameTextures.empty()) {
                    for (GLuint frameTex : inst.frameTextures) {
                        BindTextureDirect(GL_TEXTURE_2D, frameTex);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minFilter);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magFilter);
                    }
                } else {
                    BindTextureDirect(GL_TEXTURE_2D, texId);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minFilter);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magFilter);
                }
                inst.lastPixelatedScaling = conf.pixelatedScaling;
                inst.filterInitialized = true;
            }
        }

        int displayW = 0;
        int displayH = 0;
        ResolveConfiguredImageDimensions(conf, texWidth, texHeight, displayW, displayH);

        bool isViewportRelative = conf.relativeTo.length() > 8 && conf.relativeTo.substr(conf.relativeTo.length() - 8) == "Viewport";
        int finalScreenX = 0;
        int finalScreenY = 0;
        int finalDisplayW = displayW;
        int finalDisplayH = displayH;

        if (isViewportRelative) {
            float toScaleX = (gameW > 0 && gameResW > 0) ? static_cast<float>(gameW) / gameResW : 1.0f;
            float toScaleY = (gameH > 0 && gameResH > 0) ? static_cast<float>(gameH) / gameResH : 1.0f;
            float fromScaleX = (fromW > 0 && gameResW > 0) ? static_cast<float>(fromW) / gameResW : toScaleX;
            float fromScaleY = (fromH > 0 && gameResH > 0) ? static_cast<float>(fromH) / gameResH : toScaleY;
            int toDisplayW = displayW;
            int toDisplayH = displayH;
            int fromDisplayW = displayW;
            int fromDisplayH = displayH;
            ScaleViewportRelativeImageSize(displayW, displayH, relativeStretching, toScaleX, toScaleY, toDisplayW, toDisplayH);
            ScaleViewportRelativeImageSize(displayW, displayH, relativeStretching, fromScaleX, fromScaleY, fromDisplayW,
                                           fromDisplayH);

            int toPosX = 0;
            int toPosY = 0;
            GetRelativeCoordsForImageWithViewport(conf.relativeTo, conf.x, conf.y, toDisplayW, toDisplayH, gameX, gameY, gameW, gameH,
                                                  fullW, fullH, toPosX, toPosY);

            int fromPosX = 0;
            int fromPosY = 0;
            GetRelativeCoordsForImageWithViewport(conf.relativeTo, conf.x, conf.y, fromDisplayW, fromDisplayH, fromX, fromY, fromW,
                                                  fromH, fullW, fullH, fromPosX, fromPosY);

            float t = transitionProgress;
            finalScreenX = static_cast<int>(fromPosX + (toPosX - fromPosX) * t);
            finalScreenY = static_cast<int>(fromPosY + (toPosY - fromPosY) * t);
            if (relativeStretching) {
                finalDisplayW = static_cast<int>(fromDisplayW + (toDisplayW - fromDisplayW) * t);
                finalDisplayH = static_cast<int>(fromDisplayH + (toDisplayH - fromDisplayH) * t);
            }
        } else {
            GetRelativeCoordsForImageWithViewport(conf.relativeTo, conf.x, conf.y, finalDisplayW, finalDisplayH, gameX, gameY, gameW,
                                                  gameH, fullW, fullH, finalScreenX, finalScreenY);
        }

        int finalScreenYGl = fullH - finalScreenY - finalDisplayH;
        float nx1 = (static_cast<float>(finalScreenX) / fullW) * 2.0f - 1.0f;
        float ny1 = (static_cast<float>(finalScreenYGl) / fullH) * 2.0f - 1.0f;
        float nx2 = (static_cast<float>(finalScreenX + finalDisplayW) / fullW) * 2.0f - 1.0f;
        float ny2 = (static_cast<float>(finalScreenYGl + finalDisplayH) / fullH) * 2.0f - 1.0f;

        if (hasBg && !isFullyTransparent) {
            glUseProgram(g_solidColorProgram);
            glUniform4f(g_solidColorShaderLocs.color, conf.background.color.r, conf.background.color.g, conf.background.color.b,
                        conf.background.opacity * modeOpacity);
            float bgVerts[] = { nx1, ny1, 0, 0, nx2, ny1, 0, 0, nx2, ny2, 0, 0, nx1, ny1, 0, 0, nx2, ny2, 0, 0, nx1, ny2, 0, 0 };
            glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(bgVerts), bgVerts);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        }

        glUseProgram(g_imageRenderProgram);
        BindTextureDirect(GL_TEXTURE_2D, texId);
        const bool hasColorKeys = conf.enableColorKey && !conf.colorKeys.empty();
        glUniform1i(g_imageRenderShaderLocs.enableColorKey, hasColorKeys ? 1 : 0);
        if (hasColorKeys) {
            int numKeys = (std::min)(static_cast<int>(conf.colorKeys.size()), ConfigDefaults::MAX_COLOR_KEYS);
            float colors[ConfigDefaults::MAX_COLOR_KEYS * 3] = {};
            float sensitivities[ConfigDefaults::MAX_COLOR_KEYS] = {};
            for (int i = 0; i < numKeys; ++i) {
                colors[i * 3 + 0] = conf.colorKeys[i].color.r;
                colors[i * 3 + 1] = conf.colorKeys[i].color.g;
                colors[i * 3 + 2] = conf.colorKeys[i].color.b;
                sensitivities[i] = conf.colorKeys[i].sensitivity;
            }
            glUniform1i(g_imageRenderShaderLocs.numColorKeys, numKeys);
            glUniform3fv(g_imageRenderShaderLocs.colorKeys, numKeys, colors);
            glUniform1fv(g_imageRenderShaderLocs.sensitivities, numKeys, sensitivities);
        }
        glUniform1f(g_imageRenderShaderLocs.opacity, effectiveOpacity);

        const float invW = (texWidth > 0) ? (1.0f / texWidth) : 0.0f;
        auto cc = ResolveCrop(conf.crop_top, conf.crop_bottom, conf.crop_left, conf.crop_right,
                              conf.cropToWidth, conf.cropToHeight, texWidth, texHeight);
        float tu1 = cc.left * invW;
        float tu2 = (texWidth - cc.right) * invW;
        const float invFrameH = (texHeight > 0) ? (1.0f / texHeight) : 0.0f;
        float tv1 = animatedSourceRect[1] + cc.bottom * invFrameH * animatedSourceRect[3];
        float tv2 = animatedSourceRect[1] + (texHeight - cc.top) * invFrameH * animatedSourceRect[3];
        float verts[] = { nx1, ny1, tu1, tv1, nx2, ny1, tu2, tv1, nx2, ny2, tu2, tv2,
                          nx1, ny1, tu1, tv1, nx2, ny2, tu2, tv2, nx1, ny2, tu1, tv2 };
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        if (hasBorder && !isFullyTransparent) {
            RenderGameBorder(finalScreenX, finalScreenY, finalDisplayW, finalDisplayH, conf.border.width, conf.border.radius,
                             conf.border.color, fullW, fullH);
        }
    }

    glDisable(GL_BLEND);
}

static void RenderWindowOverlaysDirect(const std::vector<WindowOverlayConfig>& overlays, int fullW, int fullH, int gameX,
                                       int gameY, int gameW, int gameH, int gameResW, int gameResH, bool relativeStretching,
                                       float transitionProgress, int fromX, int fromY, int fromW, int fromH, float modeOpacity,
                                       bool excludeOnlyOnMyScreen) {
    if (overlays.empty()) return;

    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    std::lock_guard<std::mutex> cacheLock(g_windowOverlayCacheMutex);

    const std::string focusedName = GetFocusedWindowOverlayName();
    glUseProgram(g_imageRenderProgram);
    glUniform1i(g_imageRenderShaderLocs.enableColorKey, 0);

    for (const auto& conf : overlays) {
        if (excludeOnlyOnMyScreen && conf.onlyOnMyScreen) continue;

        const float effectiveOpacity = conf.opacity * modeOpacity;
        const bool hasBg = conf.background.enabled && conf.background.opacity > 0.0f;
        const bool hasBorder = conf.border.enabled && conf.border.width > 0;
        if (effectiveOpacity <= 0.0f && !hasBg && !hasBorder) continue;

        auto it = g_windowOverlayCache.find(conf.name);
        if (it == g_windowOverlayCache.end() || !it->second) continue;
        WindowOverlayCacheEntry& entry = *it->second;

        if (entry.hasNewFrame.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(entry.swapMutex);
            entry.readyBuffer.swap(entry.backBuffer);
            entry.hasNewFrame.store(false, std::memory_order_release);
        }

        WindowOverlayRenderData* renderData = entry.backBuffer.get();
        if (renderData && renderData->pixelData && renderData->width > 0 && renderData->height > 0) {
            if (renderData != entry.lastUploadedRenderData) {
                PixelStoreStateGuard pixelStoreGuard;
                if (entry.glTextureId == 0) {
                    glGenTextures(1, &entry.glTextureId);
                    BindTextureDirect(GL_TEXTURE_2D, entry.glTextureId);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    entry.filterInitialized = false;
                }

                BindTextureDirect(GL_TEXTURE_2D, entry.glTextureId);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
                glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                if (entry.glTextureWidth != renderData->width || entry.glTextureHeight != renderData->height) {
                    entry.glTextureWidth = renderData->width;
                    entry.glTextureHeight = renderData->height;
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, renderData->width, renderData->height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                                 renderData->pixelData);
                } else {
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, renderData->width, renderData->height, GL_RGBA, GL_UNSIGNED_BYTE,
                                    renderData->pixelData);
                }
                entry.lastUploadedRenderData = renderData;
            }
        }

        if (entry.glTextureId == 0) continue;

        auto cc = ResolveCrop(conf.crop_top, conf.crop_bottom, conf.crop_left, conf.crop_right,
                              conf.cropToWidth, conf.cropToHeight, entry.glTextureWidth, entry.glTextureHeight);
        int croppedW = (std::max)(1, entry.glTextureWidth - cc.left - cc.right);
        int croppedH = (std::max)(1, entry.glTextureHeight - cc.top - cc.bottom);
        int displayW = (std::max)(1, static_cast<int>(croppedW * conf.scale));
        int displayH = (std::max)(1, static_cast<int>(croppedH * conf.scale));

        bool isViewportRelative = conf.relativeTo.length() > 8 && conf.relativeTo.substr(conf.relativeTo.length() - 8) == "Viewport";
        int screenX = 0;
        int screenY = 0;
        if (isViewportRelative) {
            float toScaleX = (gameW > 0 && gameResW > 0) ? static_cast<float>(gameW) / gameResW : 1.0f;
            float toScaleY = (gameH > 0 && gameResH > 0) ? static_cast<float>(gameH) / gameResH : 1.0f;
            float fromScaleX = (fromW > 0 && gameResW > 0) ? static_cast<float>(fromW) / gameResW : toScaleX;
            float fromScaleY = (fromH > 0 && gameResH > 0) ? static_cast<float>(fromH) / gameResH : toScaleY;
            int toDisplayW = relativeStretching ? static_cast<int>(displayW * toScaleX) : displayW;
            int toDisplayH = relativeStretching ? static_cast<int>(displayH * toScaleY) : displayH;
            int fromDisplayW = relativeStretching ? static_cast<int>(displayW * fromScaleX) : displayW;
            int fromDisplayH = relativeStretching ? static_cast<int>(displayH * fromScaleY) : displayH;
            int toPosX = 0;
            int toPosY = 0;
            GetRelativeCoordsForImageWithViewport(conf.relativeTo, conf.x, conf.y, toDisplayW, toDisplayH, gameX, gameY, gameW,
                                                  gameH, fullW, fullH, toPosX, toPosY);
            int fromPosX = 0;
            int fromPosY = 0;
            GetRelativeCoordsForImageWithViewport(conf.relativeTo, conf.x, conf.y, fromDisplayW, fromDisplayH, fromX, fromY, fromW,
                                                  fromH, fullW, fullH, fromPosX, fromPosY);
            float t = transitionProgress;
            screenX = static_cast<int>(fromPosX + (toPosX - fromPosX) * t);
            screenY = static_cast<int>(fromPosY + (toPosY - fromPosY) * t);
            if (relativeStretching) {
                displayW = static_cast<int>(fromDisplayW + (toDisplayW - fromDisplayW) * t);
                displayH = static_cast<int>(fromDisplayH + (toDisplayH - fromDisplayH) * t);
            }
        } else {
            GetRelativeCoordsForImageWithViewport(conf.relativeTo, conf.x, conf.y, displayW, displayH, gameX, gameY, gameW, gameH,
                                                  fullW, fullH, screenX, screenY);
        }

        int screenYGl = fullH - screenY - displayH;
        float nx1 = (static_cast<float>(screenX) / fullW) * 2.0f - 1.0f;
        float ny1 = (static_cast<float>(screenYGl) / fullH) * 2.0f - 1.0f;
        float nx2 = (static_cast<float>(screenX + displayW) / fullW) * 2.0f - 1.0f;
        float ny2 = (static_cast<float>(screenYGl + displayH) / fullH) * 2.0f - 1.0f;

        if (hasBg) {
            glUseProgram(g_solidColorProgram);
            glUniform4f(g_solidColorShaderLocs.color, conf.background.color.r, conf.background.color.g, conf.background.color.b,
                        conf.background.opacity * modeOpacity);
            float bgVerts[] = { nx1, ny1, 0, 0, nx2, ny1, 0, 0, nx2, ny2, 0, 0, nx1, ny1, 0, 0, nx2, ny2, 0, 0, nx1, ny2, 0, 0 };
            glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(bgVerts), bgVerts);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        }

        glUseProgram(g_imageRenderProgram);
        BindTextureDirect(GL_TEXTURE_2D, entry.glTextureId);
        if (!entry.filterInitialized || entry.lastPixelatedScaling != conf.pixelatedScaling) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, conf.pixelatedScaling ? GL_NEAREST : GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, conf.pixelatedScaling ? GL_NEAREST : GL_LINEAR);
            entry.lastPixelatedScaling = conf.pixelatedScaling;
            entry.filterInitialized = true;
        }

        glUniform1i(g_imageRenderShaderLocs.enableColorKey, 0);
        glUniform1f(g_imageRenderShaderLocs.opacity, effectiveOpacity);
        float tu1 = static_cast<float>(cc.left) / entry.glTextureWidth;
        float tv1 = static_cast<float>(cc.top) / entry.glTextureHeight;
        float tu2 = static_cast<float>(entry.glTextureWidth - cc.right) / entry.glTextureWidth;
        float tv2 = static_cast<float>(entry.glTextureHeight - cc.bottom) / entry.glTextureHeight;
        float verts[] = { nx1, ny1, tu1, tv2, nx2, ny1, tu2, tv2, nx2, ny2, tu2, tv1,
                          nx1, ny1, tu1, tv2, nx2, ny2, tu2, tv1, nx1, ny2, tu1, tv1 };
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        if (hasBorder) {
            RenderGameBorder(screenX, screenY, displayW, displayH, conf.border.width, conf.border.radius, conf.border.color, fullW,
                             fullH);
        }
        if (!focusedName.empty() && focusedName == conf.name) {
            RenderGameBorder(screenX, screenY, displayW, displayH, 3, conf.border.enabled ? conf.border.radius : 0,
                             { 0.0f, 1.0f, 0.0f, 1.0f }, fullW, fullH);
        }
    }

    glDisable(GL_BLEND);
}

static void RenderBrowserOverlaysDirect(const std::vector<BrowserOverlayConfig>& overlays, int fullW, int fullH, int gameX,
                                        int gameY, int gameW, int gameH, int gameResW, int gameResH, bool relativeStretching,
                                        float transitionProgress, int fromX, int fromY, int fromW, int fromH, float modeOpacity,
                                        bool excludeOnlyOnMyScreen) {
    if (overlays.empty()) return;

    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(g_imageRenderProgram);
    glUniform1i(g_imageRenderShaderLocs.enableColorKey, 0);

    for (const auto& conf : overlays) {
        if (excludeOnlyOnMyScreen && conf.onlyOnMyScreen) continue;

        const float effectiveOpacity = conf.opacity * modeOpacity;
        const bool hasBg = conf.background.enabled && conf.background.opacity > 0.0f;
        const bool hasBorder = conf.border.enabled && conf.border.width > 0;
        if (effectiveOpacity <= 0.0f && !hasBg && !hasBorder) continue;

        BrowserOverlayTextureFrame frame{};
        if (!PrepareBrowserOverlayTexture(conf, frame)) continue;

        auto cc = ResolveCrop(conf.crop_top, conf.crop_bottom, conf.crop_left, conf.crop_right,
                              conf.cropToWidth, conf.cropToHeight, frame.textureWidth, frame.textureHeight);
        int croppedW = frame.textureWidth - cc.left - cc.right;
        int croppedH = frame.textureHeight - cc.top - cc.bottom;
        croppedW = (std::max)(1, croppedW);
        croppedH = (std::max)(1, croppedH);
        int displayW = (std::max)(1, static_cast<int>(croppedW * conf.scale));
        int displayH = (std::max)(1, static_cast<int>(croppedH * conf.scale));

        bool isViewportRelative = conf.relativeTo.length() > 8 && conf.relativeTo.substr(conf.relativeTo.length() - 8) == "Viewport";
        int screenX = 0;
        int screenY = 0;
        if (isViewportRelative) {
            float toScaleX = (gameW > 0 && gameResW > 0) ? static_cast<float>(gameW) / gameResW : 1.0f;
            float toScaleY = (gameH > 0 && gameResH > 0) ? static_cast<float>(gameH) / gameResH : 1.0f;
            float fromScaleX = (fromW > 0 && gameResW > 0) ? static_cast<float>(fromW) / gameResW : toScaleX;
            float fromScaleY = (fromH > 0 && gameResH > 0) ? static_cast<float>(fromH) / gameResH : toScaleY;
            int toDisplayW = relativeStretching ? static_cast<int>(displayW * toScaleX) : displayW;
            int toDisplayH = relativeStretching ? static_cast<int>(displayH * toScaleY) : displayH;
            int fromDisplayW = relativeStretching ? static_cast<int>(displayW * fromScaleX) : displayW;
            int fromDisplayH = relativeStretching ? static_cast<int>(displayH * fromScaleY) : displayH;
            int toPosX = 0;
            int toPosY = 0;
            GetRelativeCoordsForImageWithViewport(conf.relativeTo, conf.x, conf.y, toDisplayW, toDisplayH, gameX, gameY, gameW,
                                                  gameH, fullW, fullH, toPosX, toPosY);
            int fromPosX = 0;
            int fromPosY = 0;
            GetRelativeCoordsForImageWithViewport(conf.relativeTo, conf.x, conf.y, fromDisplayW, fromDisplayH, fromX, fromY, fromW,
                                                  fromH, fullW, fullH, fromPosX, fromPosY);
            float t = transitionProgress;
            screenX = static_cast<int>(fromPosX + (toPosX - fromPosX) * t);
            screenY = static_cast<int>(fromPosY + (toPosY - fromPosY) * t);
            if (relativeStretching) {
                displayW = static_cast<int>(fromDisplayW + (toDisplayW - fromDisplayW) * t);
                displayH = static_cast<int>(fromDisplayH + (toDisplayH - fromDisplayH) * t);
            }
        } else {
            GetRelativeCoordsForImageWithViewport(conf.relativeTo, conf.x, conf.y, displayW, displayH, gameX, gameY, gameW, gameH,
                                                  fullW, fullH, screenX, screenY);
        }

        int screenYGl = fullH - screenY - displayH;
        float nx1 = (static_cast<float>(screenX) / fullW) * 2.0f - 1.0f;
        float ny1 = (static_cast<float>(screenYGl) / fullH) * 2.0f - 1.0f;
        float nx2 = (static_cast<float>(screenX + displayW) / fullW) * 2.0f - 1.0f;
        float ny2 = (static_cast<float>(screenYGl + displayH) / fullH) * 2.0f - 1.0f;

        if (hasBg) {
            glUseProgram(g_solidColorProgram);
            glUniform4f(g_solidColorShaderLocs.color, conf.background.color.r, conf.background.color.g, conf.background.color.b,
                        conf.background.opacity * modeOpacity);
            float bgVerts[] = { nx1, ny1, 0, 0, nx2, ny1, 0, 0, nx2, ny2, 0, 0, nx1, ny1, 0, 0, nx2, ny2, 0, 0, nx1, ny2, 0, 0 };
            glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(bgVerts), bgVerts);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        }

        glUseProgram(g_imageRenderProgram);
        BindTextureDirect(GL_TEXTURE_2D, frame.textureId);
        const bool hasColorKeys = conf.enableColorKey && !conf.colorKeys.empty();
        glUniform1i(g_imageRenderShaderLocs.enableColorKey, hasColorKeys ? 1 : 0);
        if (hasColorKeys) {
            int numKeys = (std::min)(static_cast<int>(conf.colorKeys.size()), ConfigDefaults::MAX_COLOR_KEYS);
            float colors[ConfigDefaults::MAX_COLOR_KEYS * 3] = {};
            float sensitivities[ConfigDefaults::MAX_COLOR_KEYS] = {};
            for (int i = 0; i < numKeys; ++i) {
                colors[i * 3 + 0] = conf.colorKeys[i].color.r;
                colors[i * 3 + 1] = conf.colorKeys[i].color.g;
                colors[i * 3 + 2] = conf.colorKeys[i].color.b;
                sensitivities[i] = conf.colorKeys[i].sensitivity;
            }
            glUniform1i(g_imageRenderShaderLocs.numColorKeys, numKeys);
            glUniform3fv(g_imageRenderShaderLocs.colorKeys, numKeys, colors);
            glUniform1fv(g_imageRenderShaderLocs.sensitivities, numKeys, sensitivities);
        }
        glUniform1f(g_imageRenderShaderLocs.opacity, effectiveOpacity);
        float tu1 = static_cast<float>(cc.left) / frame.textureWidth;
        float tv1 = static_cast<float>(cc.top) / frame.textureHeight;
        float tu2 = static_cast<float>(frame.textureWidth - cc.right) / frame.textureWidth;
        float tv2 = static_cast<float>(frame.textureHeight - cc.bottom) / frame.textureHeight;
        float verts[] = { nx1, ny1, tu1, tv2, nx2, ny1, tu2, tv2, nx2, ny2, tu2, tv1,
                          nx1, ny1, tu1, tv2, nx2, ny2, tu2, tv1, nx1, ny2, tu1, tv1 };
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glUniform1i(g_imageRenderShaderLocs.enableColorKey, 0);

        if (hasBorder) {
            RenderGameBorder(screenX, screenY, displayW, displayH, conf.border.width, conf.border.radius, conf.border.color, fullW,
                             fullH);
        }
    }

    glDisable(GL_BLEND);
}

struct SameThreadOverlayState {
    int fullW = 0;
    int fullH = 0;

    int gameW = 0;
    int gameH = 0;
    int finalX = 0;
    int finalY = 0;
    int finalW = 0;
    int finalH = 0;

    GLuint gameTextureId = 0;
    std::string modeId;

    bool isAnimating = false;
    float overlayOpacity = 1.0f;

    bool excludeOnlyOnMyScreen = false;
    bool skipAnimation = false;
    bool relativeStretching = false;

    float transitionProgress = 1.0f;
    int fromX = 0;
    int fromY = 0;
    int fromW = 0;
    int fromH = 0;
    int fromFullW = 0;
    int fromFullH = 0;
    int toX = 0;
    int toY = 0;
    int toW = 0;
    int toH = 0;
    int toFullW = 0;
    int toFullH = 0;

    bool shouldRenderGui = false;
    bool showPerformanceOverlay = false;
    bool showProfiler = false;
    bool showEyeZoom = false;
    float eyeZoomFadeOpacity = 1.0f;
    int eyeZoomAnimatedViewportX = -1;
    bool isTransitioningFromEyeZoom = false;
    GLuint eyeZoomSnapshotTexture = 0;
    int eyeZoomSnapshotWidth = 0;
    int eyeZoomSnapshotHeight = 0;
    bool showTextureGrid = false;
    int textureGridModeWidth = 0;
    int textureGridModeHeight = 0;

    bool showWelcomeToast = false;
    bool welcomeToastIsFullscreen = false;
    bool showRebindIndicator = false;
    bool showCursorTrail = false;
    bool modeHasMirrors = false;
    bool modeHasImages = false;
    bool modeHasWindowOverlays = false;
    bool modeHasBrowserOverlays = false;

    bool isRawWindowedMode = false;
    std::string fromModeId;
    bool fromSlideMirrorsIn = false;
    bool toSlideMirrorsIn = false;
    float mirrorSlideProgress = 1.0f;
    bool allowMirrorCaptureReuse = false;
    uint64_t mirrorCaptureFrameTag = 0;
};

struct SameThreadMirrorCaptureReuseState {
    bool available = false;
    uint64_t frameTag = 0;
    const Config* configIdentity = nullptr;
    std::string modeId;
    std::string fromModeId;
    bool hasEyeZoomSlideOut = false;
    bool hasTransitionSlideOut = false;
    GLuint sourceTexture = 0;
    int sourceW = 0;
    int sourceH = 0;
};

static SameThreadMirrorCaptureReuseState s_sameThreadMirrorCaptureReuseState;
static uint64_t s_sameThreadMirrorCaptureFrameTag = 0;

static bool IsNinjabrainOverlayModeAllowed(const NinjabrainOverlayConfig& nb, const std::string& modeId) {
    if (nb.allowedModes.empty()) { return true; }
    return std::find(nb.allowedModes.begin(), nb.allowedModes.end(), modeId) != nb.allowedModes.end();
}

static bool HasNinjabrainOverlayContent(const NinjabrainOverlayConfig& nb, const NinjabrainData& data,
                                        bool* outHasTriangulation = nullptr, bool* outShowForBoat = nullptr) {
    const bool hasTriangulation = data.validPrediction &&
        (data.resultType == "TRIANGULATION" || data.resultType == "DIVINE");
    const bool hasFailedResult = data.resultType == "FAILED";
    const bool hasBlindResult = data.blind.enabled && data.blind.hasResult;
    const bool hasInformationMessages = data.informationMessageCount > 0;
    const bool boatError = (data.boatState == "ERROR");
    const bool showForBoat = nb.alwaysShowBoat || boatError;

    if (outHasTriangulation) { *outHasTriangulation = hasTriangulation; }
    if (outShowForBoat) { *outShowForBoat = showForBoat; }
    return hasTriangulation || hasFailedResult || hasBlindResult || hasInformationMessages || showForBoat ||
           nb.alwaysShow;
}

static bool IsNinjabrainOverlayStale(const NinjabrainOverlayConfig& nb, const NinjabrainData& data) {
    if (!nb.hideIfStale) { return false; }
    if (data.lastUpdateTime == std::chrono::steady_clock::time_point{}) { return true; }

    const auto hideDelay = std::chrono::seconds((std::max)(1, nb.hideIfStaleDelaySeconds));
    return (std::chrono::steady_clock::now() - data.lastUpdateTime) >= hideDelay;
}

static const char* GetNinjabrainOverlayRenderEligibilityFailure(const SameThreadOverlayState& request) {
    auto configSnapshot = GetConfigSnapshot();
    if (!configSnapshot) { return "config snapshot missing"; }

    const auto& nb = configSnapshot->ninjabrainOverlay;
    if (!nb.enabled) { return "overlay disabled"; }
    if (!g_ninjabrainOverlayVisible.load(std::memory_order_acquire)) { return "overlay visibility toggle disabled"; }
    if (request.excludeOnlyOnMyScreen && nb.onlyOnMyScreen) { return "only-on-my-screen filtered out"; }
    if (!request.excludeOnlyOnMyScreen && nb.onlyOnObs) { return "only-on-obs filtered out"; }
    if (!IsNinjabrainOverlayModeAllowed(nb, request.modeId)) { return "mode not allowed"; }

    const auto dataSnapshot = GetNinjabrainDataSnapshot();
    if (!dataSnapshot) { return "data snapshot missing"; }
    if (IsNinjabrainOverlayStale(nb, *dataSnapshot)) { return "data snapshot stale"; }
    if (!HasNinjabrainOverlayContent(nb, *dataSnapshot)) { return "data snapshot has no renderable content"; }

    return nullptr;
}

static bool ShouldRenderNinjabrainOverlayForRequest(const SameThreadOverlayState& request) {
    return GetNinjabrainOverlayRenderEligibilityFailure(request) == nullptr;
}

static bool ShouldRenderCursorTrailForRequest(const SameThreadOverlayState& request, const Config& cfg) {
    if (!request.showCursorTrail || !cfg.cursorTrail.enabled) { return false; }
    if (request.excludeOnlyOnMyScreen && cfg.cursorTrail.onlyOnMyScreen) { return false; }
    if (!request.excludeOnlyOnMyScreen && cfg.cursorTrail.onlyOnObs) { return false; }
    return true;
}

#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
const char* GetNinjabrainOverlayRenderEligibilityFailureForIntegrationTest(const std::string& modeId,
                                                                           bool excludeOnlyOnMyScreen) {
    SameThreadOverlayState request;
    request.modeId = modeId;
    request.excludeOnlyOnMyScreen = excludeOnlyOnMyScreen;
    return GetNinjabrainOverlayRenderEligibilityFailure(request);
}
#endif

static bool HasSameThreadOverlayWork(const SameThreadOverlayState& request, const Config& cfg, bool renderNinjabrainOverlay) {
    if (request.modeHasMirrors || request.modeHasImages || request.modeHasWindowOverlays || request.modeHasBrowserOverlays ||
        request.shouldRenderGui || request.showCursorTrail ||
        request.showPerformanceOverlay || request.showProfiler || request.showTextureGrid || request.showEyeZoom ||
        request.showWelcomeToast || request.showRebindIndicator || renderNinjabrainOverlay) {
        return true;
    }

    if (request.isRawWindowedMode || request.skipAnimation) { return false; }

    if (request.isTransitioningFromEyeZoom && cfg.eyezoom.slideMirrorsIn) {
        return true;
    }

    return request.fromSlideMirrorsIn && !request.fromModeId.empty() && request.mirrorSlideProgress < 1.0f;
}

static uint64_t BeginSameThreadMirrorCaptureFrame() {
    ++s_sameThreadMirrorCaptureFrameTag;
    if (s_sameThreadMirrorCaptureFrameTag == 0) {
        ++s_sameThreadMirrorCaptureFrameTag;
    }

    s_sameThreadMirrorCaptureReuseState.available = false;
    return s_sameThreadMirrorCaptureFrameTag;
}

static bool CanReuseSameThreadMirrorCapture(const Config& cfg, const SameThreadOverlayState& request,
                                            bool hasEyeZoomSlideOutMirrors, bool hasTransitionSlideOutMirrors,
                                            GLuint sourceTexture, int sourceW, int sourceH) {
    return request.allowMirrorCaptureReuse && request.mirrorCaptureFrameTag != 0 && s_sameThreadMirrorCaptureReuseState.available &&
           s_sameThreadMirrorCaptureReuseState.frameTag == request.mirrorCaptureFrameTag &&
           s_sameThreadMirrorCaptureReuseState.configIdentity == &cfg &&
           s_sameThreadMirrorCaptureReuseState.modeId == request.modeId &&
           s_sameThreadMirrorCaptureReuseState.fromModeId == request.fromModeId &&
           s_sameThreadMirrorCaptureReuseState.hasEyeZoomSlideOut == hasEyeZoomSlideOutMirrors &&
           s_sameThreadMirrorCaptureReuseState.hasTransitionSlideOut == hasTransitionSlideOutMirrors &&
           s_sameThreadMirrorCaptureReuseState.sourceTexture == sourceTexture &&
           s_sameThreadMirrorCaptureReuseState.sourceW == sourceW && s_sameThreadMirrorCaptureReuseState.sourceH == sourceH;
}

static void CacheSameThreadMirrorCapture(const Config& cfg, const SameThreadOverlayState& request,
                                         bool hasEyeZoomSlideOutMirrors, bool hasTransitionSlideOutMirrors, GLuint sourceTexture,
                                         int sourceW, int sourceH) {
    s_sameThreadMirrorCaptureReuseState.available = true;
    s_sameThreadMirrorCaptureReuseState.frameTag = request.mirrorCaptureFrameTag;
    s_sameThreadMirrorCaptureReuseState.configIdentity = &cfg;
    s_sameThreadMirrorCaptureReuseState.modeId = request.modeId;
    s_sameThreadMirrorCaptureReuseState.fromModeId = request.fromModeId;
    s_sameThreadMirrorCaptureReuseState.hasEyeZoomSlideOut = hasEyeZoomSlideOutMirrors;
    s_sameThreadMirrorCaptureReuseState.hasTransitionSlideOut = hasTransitionSlideOutMirrors;
    s_sameThreadMirrorCaptureReuseState.sourceTexture = sourceTexture;
    s_sameThreadMirrorCaptureReuseState.sourceW = sourceW;
    s_sameThreadMirrorCaptureReuseState.sourceH = sourceH;
}

static void RenderSameThreadImGui(const SameThreadOverlayState& request, bool renderNinjabrainOverlay) {
    const bool shouldRenderAnyImGui = request.shouldRenderGui || request.showPerformanceOverlay || request.showProfiler ||
                                      request.showTextureGrid || request.showEyeZoom || renderNinjabrainOverlay;
    if (!shouldRenderAnyImGui) return;

    std::lock_guard<std::recursive_mutex> imguiLock(GetImGuiContextMutex());

    PROFILE_SCOPE_CAT("Render Same-Thread ImGui", "Rendering");

    HWND hwnd = g_minecraftHwnd.load();
    if (!hwnd) return;
    {
        PROFILE_SCOPE_CAT("ImGui Setup", "ImGui");

        {
            PROFILE_SCOPE_CAT("ImGui Context Init", "ImGui");
            InitializeImGuiContext(hwnd);
        }
        if (ImGui::GetCurrentContext() == nullptr) { return; }

        {
            PROFILE_SCOPE_CAT("ImGui Backend NewFrame", "ImGui");
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplWin32_NewFrame();
        }
        {
            PROFILE_SCOPE_CAT("ImGui Display Metrics Sync", "ImGui");
            SyncImGuiDisplayMetrics(hwnd);
        }
        {
            PROFILE_SCOPE_CAT("ImGui Main Font Refresh Check", "ImGui");
            ApplyDynamicGuiFontRefresh();
        }
        {
            PROFILE_SCOPE_CAT("ImGui Keyboard Font Refresh Check", "ImGui");
            ApplyPendingKeyboardLayoutFontRefresh();
        }

        // Feed queued input from the window thread into the main-thread ImGui context.
        {
            PROFILE_SCOPE_CAT("ImGui Input Drain", "ImGui");
            ImGuiInputQueue_DrainToImGui();
        }
        {
            PROFILE_SCOPE_CAT("ImGui NewFrame", "ImGui");
            ImGui::NewFrame();
        }
    }

    if (request.showTextureGrid) {
        PROFILE_SCOPE_CAT("ImGui Texture Grid Overlay", "ImGui");
        RenderTextureGridOverlay(true, request.textureGridModeWidth, request.textureGridModeHeight);
    }

    {
        PROFILE_SCOPE_CAT("ImGui Cached Labels", "ImGui");
        RenderCachedTextureGridLabels();
        RenderCachedEyeZoomTextLabels();
    }

    if (request.showPerformanceOverlay) {
        PROFILE_SCOPE_CAT("ImGui Performance Overlay", "ImGui");
        RenderPerformanceOverlay(true);
    }

    // NinjabrainBot overlay (rendered in ImGui space)
    if (renderNinjabrainOverlay) {
        PROFILE_SCOPE_CAT("ImGui Ninjabrain Overlay", "ImGui");
        auto nbCfg = GetConfigSnapshot();
        if (nbCfg) {
            RenderNinjabrainOverlay(nbCfg->ninjabrainOverlay, GetNinjabrainFont(), request.modeId, request.shouldRenderGui);
        }
    }

    if (request.showProfiler) {
        PROFILE_SCOPE_CAT("ImGui Profiler Overlay", "ImGui");
        RenderProfilerOverlay(true, request.showPerformanceOverlay);
    }

    if (request.shouldRenderGui) {
        PROFILE_SCOPE_CAT("ImGui Settings Window", "ImGui");
        RenderSettingsGUI();
        ImGuiInputQueue_PublishCaptureState();
    }

    {
        PROFILE_SCOPE_CAT("ImGui Build Draw Data", "ImGui");
        ImGui::Render();
    }
    {
        PROFILE_SCOPE_CAT("ImGui Submit Draw Data", "ImGui");
        RenderImGuiWithStateProtection(true);
    }
}

static bool RenderSameThreadOverlayPass(const SameThreadOverlayState& request, const Config& cfg, const GLState& s) {
    bool renderNinjabrainOverlay = false;
    bool renderCursorTrail = false;
    {
        PROFILE_SCOPE_CAT("Resolve Same-Thread Overlay Work", "Rendering");
        renderNinjabrainOverlay = ShouldRenderNinjabrainOverlayForRequest(request);
        renderCursorTrail = ShouldRenderCursorTrailForRequest(request, cfg);
        if (!HasSameThreadOverlayWork(request, cfg, renderNinjabrainOverlay)) { return false; }
    }

    {
        PROFILE_SCOPE_CAT("Prepare Overlay GL State", "Rendering");
        PrepareSameThreadOverlayState(s, request.fullW, request.fullH);
    }

    static const Config* s_cachedActiveConfig = nullptr;
    static std::string s_cachedActiveModeId;
    static int s_cachedActiveScreenW = 0;
    static int s_cachedActiveScreenH = 0;
    static bool s_cachedActiveImagesVisible = false;
    static bool s_cachedActiveWindowOverlaysVisible = false;
    static bool s_cachedActiveBrowserOverlaysVisible = false;
    static std::vector<MirrorConfig> s_cachedActiveMirrors;
    static std::vector<ImageConfig> s_cachedActiveImages;
    static std::vector<WindowOverlayConfig> s_cachedActiveWindowOverlays;
    static std::vector<BrowserOverlayConfig> s_cachedActiveBrowserOverlays;
    static const Config* s_cachedEyeZoomSlideOutConfig = nullptr;
    static std::string s_cachedEyeZoomSlideOutTargetModeId;
    static int s_cachedEyeZoomSlideOutScreenW = 0;
    static int s_cachedEyeZoomSlideOutScreenH = 0;
    static std::vector<MirrorConfig> s_cachedEyeZoomSlideOutMirrors;
    static const Config* s_cachedTransitionSlideOutConfig = nullptr;
    static std::string s_cachedTransitionSlideOutFromModeId;
    static std::string s_cachedTransitionSlideOutTargetModeId;
    static int s_cachedTransitionSlideOutScreenW = 0;
    static int s_cachedTransitionSlideOutScreenH = 0;
    static std::vector<MirrorConfig> s_cachedTransitionSlideOutMirrors;
    static const Config* s_cachedSameThreadCaptureConfig = nullptr;
    static std::string s_cachedSameThreadCaptureModeId;
    static int s_cachedSameThreadCaptureScreenW = 0;
    static int s_cachedSameThreadCaptureScreenH = 0;
    static std::vector<ThreadedMirrorConfig> s_cachedSameThreadCaptureConfigs;
    static const std::vector<MirrorConfig> s_emptyMirrors;
    static const std::vector<ImageConfig> s_emptyImages;
    static const std::vector<WindowOverlayConfig> s_emptyWindowOverlays;
    static const std::vector<BrowserOverlayConfig> s_emptyBrowserOverlays;

    GameViewportGeometry geo{};
    geo.gameW = request.gameW;
    geo.gameH = request.gameH;
    geo.finalX = request.finalX;
    geo.finalY = request.finalY;
    geo.finalW = request.finalW;
    geo.finalH = request.finalH;

    const std::vector<MirrorConfig>* eyeZoomSlideOutMirrors = &s_emptyMirrors;
    const std::vector<MirrorConfig>* transitionSlideOutMirrors = &s_emptyMirrors;
    const bool hasMirrorSlideOutWork = !request.isRawWindowedMode &&
                                       ((request.isTransitioningFromEyeZoom && cfg.eyezoom.slideMirrorsIn && !request.skipAnimation) ||
                                        (!request.isTransitioningFromEyeZoom && request.fromSlideMirrorsIn && !request.fromModeId.empty() &&
                                         request.mirrorSlideProgress < 1.0f && !request.skipAnimation));
    const bool needModeElements = request.modeHasMirrors || request.modeHasImages || request.modeHasWindowOverlays ||
                                  request.modeHasBrowserOverlays || hasMirrorSlideOutWork;
    const bool imagesVisible = request.modeHasImages;
    const bool windowOverlaysVisible = request.modeHasWindowOverlays;
    const bool browserOverlaysVisible = request.modeHasBrowserOverlays;
    const int resolvedTargetScreenW = request.toFullW > 0 ? request.toFullW : request.fullW;
    const int resolvedTargetScreenH = request.toFullH > 0 ? request.toFullH : request.fullH;
    const int resolvedSourceScreenW = request.fromFullW > 0 ? request.fromFullW : request.fullW;
    const int resolvedSourceScreenH = request.fromFullH > 0 ? request.fromFullH : request.fullH;
    if (needModeElements) {
        if (s_cachedActiveConfig != &cfg || s_cachedActiveModeId != request.modeId ||
            s_cachedActiveScreenW != resolvedTargetScreenW || s_cachedActiveScreenH != resolvedTargetScreenH ||
            s_cachedActiveImagesVisible != imagesVisible || s_cachedActiveWindowOverlaysVisible != windowOverlaysVisible ||
            s_cachedActiveBrowserOverlaysVisible != browserOverlaysVisible) {
            PROFILE_SCOPE_CAT("Collect Active Mode Elements", "Rendering");
            s_cachedActiveConfig = &cfg;
            s_cachedActiveModeId = request.modeId;
            s_cachedActiveScreenW = resolvedTargetScreenW;
            s_cachedActiveScreenH = resolvedTargetScreenH;
            s_cachedActiveImagesVisible = imagesVisible;
            s_cachedActiveWindowOverlaysVisible = windowOverlaysVisible;
            s_cachedActiveBrowserOverlaysVisible = browserOverlaysVisible;
            CollectActiveElementsForMode(cfg, request.modeId, false, s_cachedActiveMirrors, s_cachedActiveImages,
                                         s_cachedActiveWindowOverlays, s_cachedActiveBrowserOverlays,
                                         resolvedTargetScreenW, resolvedTargetScreenH);
        }
    }
    const std::vector<MirrorConfig>& activeMirrors = needModeElements ? s_cachedActiveMirrors : s_emptyMirrors;
    const std::vector<ImageConfig>& activeImages = needModeElements ? s_cachedActiveImages : s_emptyImages;
    const std::vector<WindowOverlayConfig>& activeWindowOverlays =
        needModeElements ? s_cachedActiveWindowOverlays : s_emptyWindowOverlays;
    const std::vector<BrowserOverlayConfig>& activeBrowserOverlays =
        needModeElements ? s_cachedActiveBrowserOverlays : s_emptyBrowserOverlays;

    if (!request.isRawWindowedMode && request.isTransitioningFromEyeZoom && cfg.eyezoom.slideMirrorsIn && !request.skipAnimation) {
        if (s_cachedEyeZoomSlideOutConfig != &cfg || s_cachedEyeZoomSlideOutTargetModeId != request.modeId ||
            s_cachedEyeZoomSlideOutScreenW != resolvedSourceScreenW ||
            s_cachedEyeZoomSlideOutScreenH != resolvedSourceScreenH) {
            PROFILE_SCOPE_CAT("Resolve EyeZoom Slide-Out Mirrors", "Rendering");
            std::vector<MirrorConfig> eyeZoomMirrors;
            std::vector<ImageConfig> unusedImages;
            std::vector<WindowOverlayConfig> unusedOverlays;
            std::vector<BrowserOverlayConfig> unusedBrowserOverlays;
            std::unordered_set<std::string> activeMirrorNames;
            activeMirrorNames.reserve(activeMirrors.size());
            for (const auto& targetMirror : activeMirrors) {
                activeMirrorNames.insert(targetMirror.name);
            }

            CollectActiveElementsForMode(cfg, "EyeZoom", false, eyeZoomMirrors, unusedImages, unusedOverlays,
                                         unusedBrowserOverlays, resolvedSourceScreenW, resolvedSourceScreenH);

            s_cachedEyeZoomSlideOutMirrors.clear();
            s_cachedEyeZoomSlideOutMirrors.reserve(eyeZoomMirrors.size());
            for (const auto& ezMirror : eyeZoomMirrors) {
                if (activeMirrorNames.find(ezMirror.name) == activeMirrorNames.end()) {
                    s_cachedEyeZoomSlideOutMirrors.push_back(ezMirror);
                }
            }

            s_cachedEyeZoomSlideOutConfig = &cfg;
            s_cachedEyeZoomSlideOutTargetModeId = request.modeId;
            s_cachedEyeZoomSlideOutScreenW = resolvedSourceScreenW;
            s_cachedEyeZoomSlideOutScreenH = resolvedSourceScreenH;
        }

        eyeZoomSlideOutMirrors = &s_cachedEyeZoomSlideOutMirrors;
    }

    if (!request.isRawWindowedMode && !request.isTransitioningFromEyeZoom && request.fromSlideMirrorsIn && !request.fromModeId.empty() &&
        request.mirrorSlideProgress < 1.0f && !request.skipAnimation) {
        if (s_cachedTransitionSlideOutConfig != &cfg || s_cachedTransitionSlideOutFromModeId != request.fromModeId ||
            s_cachedTransitionSlideOutTargetModeId != request.modeId ||
            s_cachedTransitionSlideOutScreenW != resolvedSourceScreenW ||
            s_cachedTransitionSlideOutScreenH != resolvedSourceScreenH) {
            PROFILE_SCOPE_CAT("Resolve Transition Slide-Out Mirrors", "Rendering");
            std::vector<MirrorConfig> fromModeMirrors;
            std::vector<ImageConfig> unusedImages;
            std::vector<WindowOverlayConfig> unusedOverlays;
            std::vector<BrowserOverlayConfig> unusedBrowserOverlays;
            std::unordered_set<std::string> activeMirrorNames;
            activeMirrorNames.reserve(activeMirrors.size());
            for (const auto& targetMirror : activeMirrors) {
                activeMirrorNames.insert(targetMirror.name);
            }

            CollectActiveElementsForMode(cfg, request.fromModeId, false, fromModeMirrors, unusedImages, unusedOverlays,
                                         unusedBrowserOverlays, resolvedSourceScreenW, resolvedSourceScreenH);

            s_cachedTransitionSlideOutMirrors.clear();
            s_cachedTransitionSlideOutMirrors.reserve(fromModeMirrors.size());
            for (const auto& fromMirror : fromModeMirrors) {
                if (activeMirrorNames.find(fromMirror.name) == activeMirrorNames.end()) {
                    s_cachedTransitionSlideOutMirrors.push_back(fromMirror);
                }
            }

            s_cachedTransitionSlideOutConfig = &cfg;
            s_cachedTransitionSlideOutFromModeId = request.fromModeId;
            s_cachedTransitionSlideOutTargetModeId = request.modeId;
            s_cachedTransitionSlideOutScreenW = resolvedSourceScreenW;
            s_cachedTransitionSlideOutScreenH = resolvedSourceScreenH;
        }

        transitionSlideOutMirrors = &s_cachedTransitionSlideOutMirrors;
    }

    if (!request.isRawWindowedMode && request.showEyeZoom) {
        PROFILE_SCOPE_CAT("EyeZoom Overlay", "Rendering");
        ClearEyeZoomTextLabels();
        handleEyeZoomMode(s, cfg.eyezoom, request.fullW, request.fullH, request.eyeZoomFadeOpacity,
                          request.eyeZoomAnimatedViewportX, request.isTransitioningFromEyeZoom, request.gameTextureId,
                          request.gameW, request.gameH);
        {
            PROFILE_SCOPE_CAT("Prepare Overlay GL State", "Rendering");
            PrepareSameThreadOverlayState(s, request.fullW, request.fullH);
        }
    } else {
        ClearEyeZoomTextLabels();
    }

    if (!request.isRawWindowedMode) {
        GLuint sourceTexture = 0;
        int sourceW = 0;
        int sourceH = 0;
        const bool hasEyeZoomSlideOutMirrors = !eyeZoomSlideOutMirrors->empty();
        const bool hasTransitionSlideOutMirrors = !transitionSlideOutMirrors->empty();
        if (s_cachedSameThreadCaptureConfig != &cfg || s_cachedSameThreadCaptureModeId != request.modeId ||
            s_cachedSameThreadCaptureScreenW != resolvedTargetScreenW ||
            s_cachedSameThreadCaptureScreenH != resolvedTargetScreenH) {
            PROFILE_SCOPE_CAT("Build Same-Thread Capture Configs", "Rendering");
            std::vector<MirrorConfig> mirrorsForCapture = activeMirrors;
            BuildThreadedMirrorConfigs(mirrorsForCapture, s_cachedSameThreadCaptureConfigs);

            s_cachedSameThreadCaptureConfig = &cfg;
            s_cachedSameThreadCaptureModeId = request.modeId;
            s_cachedSameThreadCaptureScreenW = resolvedTargetScreenW;
            s_cachedSameThreadCaptureScreenH = resolvedTargetScreenH;
        }

        bool hasCaptureSource = false;
        if (!s_cachedSameThreadCaptureConfigs.empty()) {
            PROFILE_SCOPE_CAT("Resolve Mirror Capture Source", "Rendering");
            hasCaptureSource =
                SelectSameThreadGameTexture(request.gameTextureId, request.gameW, request.gameH, sourceTexture, sourceW, sourceH);
        }

        if (!s_cachedSameThreadCaptureConfigs.empty() && hasCaptureSource) {
            const bool reuseMirrorCaptures =
                CanReuseSameThreadMirrorCapture(cfg, request, hasEyeZoomSlideOutMirrors, hasTransitionSlideOutMirrors,
                                                sourceTexture, sourceW, sourceH);
            if (!reuseMirrorCaptures) {
                PROFILE_SCOPE_CAT("Capture Mirror Sources", "Rendering");
                if (RenderMirrorCapturesOnCurrentThread(s_cachedSameThreadCaptureConfigs, sourceTexture, sourceW, sourceH,
                                                       request.fullW, request.fullH, geo.finalX, geo.finalY, geo.finalW,
                                                       geo.finalH)) {
                    PROFILE_SCOPE_CAT("Prepare Overlay GL State", "Rendering");
                    PrepareSameThreadOverlayState(s, request.fullW, request.fullH);
                }
                CacheSameThreadMirrorCapture(cfg, request, hasEyeZoomSlideOutMirrors, hasTransitionSlideOutMirrors,
                                             sourceTexture, sourceW, sourceH);
            }
        }

        if (!activeMirrors.empty()) {
            PROFILE_SCOPE_CAT("Render Active Mirrors", "Rendering");
            const bool isEyeZoomMode = (request.modeId == "EyeZoom");
            RenderMirrorsDirect(activeMirrors, geo, request.fullW, request.fullH, request.overlayOpacity,
                                request.excludeOnlyOnMyScreen, request.relativeStretching, request.transitionProgress,
                                request.mirrorSlideProgress, request.fromX, request.fromY, request.fromW, request.fromH, request.toX,
                                request.toY, request.toW, request.toH, request.fromFullW, request.fromFullH, isEyeZoomMode,
                                request.isTransitioningFromEyeZoom,
                                request.eyeZoomAnimatedViewportX, request.skipAnimation, request.fromModeId, request.fromSlideMirrorsIn,
                                request.toSlideMirrorsIn, false, cfg);
        }

        if (!eyeZoomSlideOutMirrors->empty()) {
            PROFILE_SCOPE_CAT("Render EyeZoom Slide-Out Mirrors", "Rendering");
            RenderMirrorsDirect(*eyeZoomSlideOutMirrors, geo, request.fullW, request.fullH, request.overlayOpacity,
                                request.excludeOnlyOnMyScreen, request.relativeStretching, request.transitionProgress,
                                request.mirrorSlideProgress, request.fromX, request.fromY, request.fromW, request.fromH,
                                request.toX, request.toY, request.toW, request.toH, request.fromFullW, request.fromFullH, true,
                                request.isTransitioningFromEyeZoom,
                                request.eyeZoomAnimatedViewportX, request.skipAnimation, request.modeId, cfg.eyezoom.slideMirrorsIn,
                                request.toSlideMirrorsIn, true, cfg);
        }

        if (!transitionSlideOutMirrors->empty()) {
            PROFILE_SCOPE_CAT("Render Transition Slide-Out Mirrors", "Rendering");
            RenderMirrorsDirect(*transitionSlideOutMirrors, geo, request.fullW, request.fullH, request.overlayOpacity,
                                request.excludeOnlyOnMyScreen, request.relativeStretching, request.transitionProgress,
                                request.mirrorSlideProgress, request.fromX, request.fromY, request.fromW, request.fromH,
                                request.toX, request.toY, request.toW, request.toH, request.fromFullW, request.fromFullH, false,
                                false, -1, request.skipAnimation,
                                request.modeId, request.fromSlideMirrorsIn, request.toSlideMirrorsIn, true, cfg);
        }
    }

    if (!request.isRawWindowedMode && !activeImages.empty()) {
        PROFILE_SCOPE_CAT("Render Active Images", "Rendering");
        RenderImagesDirect(activeImages, request.fullW, request.fullH, request.toX, request.toY, request.toW, request.toH,
                           request.gameW, request.gameH, request.relativeStretching, request.transitionProgress, request.fromX,
                           request.fromY, request.fromW, request.fromH, request.overlayOpacity, request.excludeOnlyOnMyScreen);
    }

    if (!activeWindowOverlays.empty()) {
        PROFILE_SCOPE_CAT("Render Window Overlays", "Rendering");
        RenderWindowOverlaysDirect(activeWindowOverlays, request.fullW, request.fullH, request.toX, request.toY, request.toW,
                                   request.toH, request.gameW, request.gameH, request.relativeStretching, request.transitionProgress,
                                   request.fromX, request.fromY, request.fromW, request.fromH, request.overlayOpacity,
                                   request.excludeOnlyOnMyScreen);
    }

    if (!activeBrowserOverlays.empty()) {
        PROFILE_SCOPE_CAT("Render Browser Overlays", "Rendering");
        RenderBrowserOverlaysDirect(activeBrowserOverlays, request.fullW, request.fullH, request.toX, request.toY, request.toW,
                                    request.toH, request.gameW, request.gameH, request.relativeStretching,
                                    request.transitionProgress, request.fromX, request.fromY, request.fromW, request.fromH,
                                    request.overlayOpacity, request.excludeOnlyOnMyScreen);
    }

    if (request.showRebindIndicator) {
        PROFILE_SCOPE_CAT("Render Rebind Indicator", "Rendering");
        RenderRebindIndicator();
    }

    if (renderCursorTrail) {
        HWND hwnd = g_minecraftHwnd.load();
        if (hwnd) {
            PROFILE_SCOPE_CAT("Render Cursor Trail", "Rendering");
            RenderCursorTrail(hwnd, request.fullW, request.fullH, cfg.cursorTrail, request.mirrorCaptureFrameTag);
        }
    }

    RenderSameThreadImGui(request, renderNinjabrainOverlay);
    if (request.showWelcomeToast) {
        PROFILE_SCOPE_CAT("Render Welcome Toast", "Rendering");
        RenderWelcomeToast(request.welcomeToastIsFullscreen);
    }
    return !activeMirrors.empty() || !eyeZoomSlideOutMirrors->empty() || !transitionSlideOutMirrors->empty() || !activeImages.empty() ||
            !activeWindowOverlays.empty() || !activeBrowserOverlays.empty() || request.shouldRenderGui || request.showPerformanceOverlay || request.showProfiler ||
           request.showTextureGrid || request.showEyeZoom || request.showWelcomeToast || request.showRebindIndicator || renderCursorTrail || renderNinjabrainOverlay;
}

bool RenderModeOverlaysForIntegrationTest(const Config& config, const ModeConfig& modeToRender, const GLState& s, int fullW,
                                          int fullH, int gameX, int gameY, int gameW, int gameH,
                                          bool excludeOnlyOnMyScreen, GLuint gameTextureId, bool renderGui) {
    SameThreadOverlayState request;
    request.fullW = fullW;
    request.fullH = fullH;
    request.gameW = gameW;
    request.gameH = gameH;
    request.finalX = gameX;
    request.finalY = gameY;
    request.finalW = gameW;
    request.finalH = gameH;
    request.gameTextureId = gameTextureId;
    request.modeId = modeToRender.id;
    request.overlayOpacity = 1.0f;
    request.excludeOnlyOnMyScreen = excludeOnlyOnMyScreen;
    request.skipAnimation = true;
    request.relativeStretching = modeToRender.relativeStretching;
    request.transitionProgress = 1.0f;
    request.fromX = gameX;
    request.fromY = gameY;
    request.fromW = gameW;
    request.fromH = gameH;
    request.fromFullW = fullW;
    request.fromFullH = fullH;
    request.toX = gameX;
    request.toY = gameY;
    request.toW = gameW;
    request.toH = gameH;
    request.toFullW = fullW;
    request.toFullH = fullH;
    request.shouldRenderGui = renderGui;
    request.showCursorTrail = config.cursorTrail.enabled && IsCursorVisible();
    request.modeHasMirrors = gameTextureId != 0 && (!modeToRender.mirrorIds.empty() || !modeToRender.mirrorGroupIds.empty());
    request.modeHasImages = !modeToRender.imageIds.empty();
    request.modeHasWindowOverlays = g_windowOverlaysVisible.load(std::memory_order_acquire) &&
                                    !modeToRender.windowOverlayIds.empty();
    request.modeHasBrowserOverlays = g_browserOverlaysVisible.load(std::memory_order_acquire) &&
                                     !modeToRender.browserOverlayIds.empty();
    request.showRebindIndicator = IsRebindIndicatorVisible();
    request.isRawWindowedMode = !request.modeHasMirrors;
    request.toSlideMirrorsIn = modeToRender.slideMirrorsIn;
    request.mirrorSlideProgress = 1.0f;
    return RenderSameThreadOverlayPass(request, config, s);
}

static void EnsureSameThreadObsComposeTarget(int fullW, int fullH) {
    if (fullW <= 0 || fullH <= 0) { return; }

    const bool needsResize = g_sameThreadObsComposeW != fullW || g_sameThreadObsComposeH != fullH;
    const bool hasAllTargets = g_sameThreadObsComposeFBOs[0] != 0 && g_sameThreadObsComposeFBOs[1] != 0 &&
                               g_sameThreadObsComposeTextures[0] != 0 && g_sameThreadObsComposeTextures[1] != 0;
    if (!needsResize && hasAllTargets) { return; }

    ClearObsOverride();

    for (int i = 0; i < SAME_THREAD_OBS_BUFFER_COUNT; ++i) {
        if (g_sameThreadObsComposeFBOs[i] == 0) { glGenFramebuffers(1, &g_sameThreadObsComposeFBOs[i]); }
        if (g_sameThreadObsComposeTextures[i] != 0) {
            glDeleteTextures(1, &g_sameThreadObsComposeTextures[i]);
            g_sameThreadObsComposeTextures[i] = 0;
        }

        glGenTextures(1, &g_sameThreadObsComposeTextures[i]);
        BindTextureDirect(GL_TEXTURE_2D, g_sameThreadObsComposeTextures[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, fullW, fullH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glBindFramebuffer(GL_FRAMEBUFFER, g_sameThreadObsComposeFBOs[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_sameThreadObsComposeTextures[i], 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    BindTextureDirect(GL_TEXTURE_2D, 0);

    g_sameThreadObsComposeW = fullW;
    g_sameThreadObsComposeH = fullH;
    g_sameThreadObsComposePublishedIndex = -1;
    g_sameThreadObsComposeWriteIndex = 0;
}

static void EnsureSameThreadVirtualCameraScaleTarget(int outW, int outH) {
    if (outW <= 0 || outH <= 0) { return; }
    if (g_sameThreadVirtualCameraScaleW == outW && g_sameThreadVirtualCameraScaleH == outH &&
        g_sameThreadVirtualCameraScaleFBO != 0 && g_sameThreadVirtualCameraScaleTexture != 0) {
        return;
    }

    if (g_sameThreadVirtualCameraScaleFBO == 0) { glGenFramebuffers(1, &g_sameThreadVirtualCameraScaleFBO); }
    if (g_sameThreadVirtualCameraScaleTexture != 0) { glDeleteTextures(1, &g_sameThreadVirtualCameraScaleTexture); }

    glGenTextures(1, &g_sameThreadVirtualCameraScaleTexture);
    BindTextureDirect(GL_TEXTURE_2D, g_sameThreadVirtualCameraScaleTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, outW, outH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindFramebuffer(GL_FRAMEBUFFER, g_sameThreadVirtualCameraScaleFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_sameThreadVirtualCameraScaleTexture, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    BindTextureDirect(GL_TEXTURE_2D, 0);

    g_sameThreadVirtualCameraScaleW = outW;
    g_sameThreadVirtualCameraScaleH = outH;
}

static GLuint PrepareSameThreadVirtualCameraTexture(GLuint srcTexture, int srcW, int srcH, int outW, int outH) {
    if (srcTexture == 0 || srcW <= 0 || srcH <= 0 || outW <= 0 || outH <= 0) { return 0; }
    if (srcW == outW && srcH == outH) { return srcTexture; }

    EnsureSameThreadVirtualCameraScaleTarget(outW, outH);
    if (g_sameThreadVirtualCameraScaleFBO == 0 || g_sameThreadVirtualCameraScaleTexture == 0) { return 0; }
    if (g_sameThreadVirtualCameraReadFBO == 0) { glGenFramebuffers(1, &g_sameThreadVirtualCameraReadFBO); }

    GLint previousReadFbo = 0;
    GLint previousDrawFbo = 0;
    GLint previousViewport[4] = {};
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFbo);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFbo);
    glGetIntegerv(GL_VIEWPORT, previousViewport);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_sameThreadVirtualCameraReadFBO);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, srcTexture, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_sameThreadVirtualCameraScaleFBO);

    if (oglViewport) {
        oglViewport(0, 0, outW, outH);
    } else {
        glViewport(0, 0, outW, outH);
    }
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    const float scaleX = static_cast<float>(outW) / static_cast<float>(srcW);
    const float scaleY = static_cast<float>(outH) / static_cast<float>(srcH);
    const float fitScale = (std::min)(scaleX, scaleY);

    int fitW = static_cast<int>(static_cast<float>(srcW) * fitScale + 0.5f);
    int fitH = static_cast<int>(static_cast<float>(srcH) * fitScale + 0.5f);
    fitW = (std::max)(1, (std::min)(fitW, outW));
    fitH = (std::max)(1, (std::min)(fitH, outH));

    const int dstX = (outW - fitW) / 2;
    const int dstY = (outH - fitH) / 2;
    BlitFramebufferDirect(0, 0, srcW, srcH, dstX, dstY, dstX + fitW, dstY + fitH, GL_COLOR_BUFFER_BIT, GL_LINEAR);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, previousReadFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previousDrawFbo);
    if (oglViewport) {
        oglViewport(previousViewport[0], previousViewport[1], previousViewport[2], previousViewport[3]);
    } else {
        glViewport(previousViewport[0], previousViewport[1], previousViewport[2], previousViewport[3]);
    }

    return g_sameThreadVirtualCameraScaleTexture;
}

static bool ConvertSameThreadVirtualCameraTextureToNv12(GLuint srcTexture, int srcW, int srcH, const SameThreadVirtualCameraReadbackSlot& slot) {
    if (srcTexture == 0 || srcW <= 0 || srcH <= 0 || g_virtualCameraNv12Program == 0) { return false; }
    if (slot.yTexture == 0 || slot.uvTexture == 0 || slot.textureWidth != srcW || slot.textureHeight != srcH) { return false; }
    if (g_sameThreadVirtualCameraConvertFBO == 0) { glGenFramebuffers(1, &g_sameThreadVirtualCameraConvertFBO); }
    if (g_sameThreadVirtualCameraConvertFBO == 0) { return false; }

    const int colorSpaceMode = (srcW >= 1280 || srcH > 576) ? 1 : 0;

    GLint previousProgram = 0;
    GLint previousActiveTexture = 0;
    GLint previousTexture0 = 0;
    GLint previousVertexArray = 0;
    GLint previousDrawFbo = 0;
    GLint previousViewport[4] = {};
    glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture0);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVertexArray);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFbo);
    glGetIntegerv(GL_VIEWPORT, previousViewport);

    const GLboolean blendEnabled = glIsEnabled(GL_BLEND);
    const GLboolean scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_sameThreadVirtualCameraConvertFBO);
    glUseProgram(g_virtualCameraNv12Program);
    glUniform2f(g_virtualCameraNv12ShaderLocs.sourceTexelSize, 1.0f / static_cast<float>(srcW), 1.0f / static_cast<float>(srcH));
    if (g_virtualCameraNv12ShaderLocs.colorSpaceMode >= 0) {
        glUniform1i(g_virtualCameraNv12ShaderLocs.colorSpaceMode, colorSpaceMode);
    }
    glBindVertexArray(g_fullscreenQuadVAO);
    glActiveTexture(GL_TEXTURE0);
    BindTextureDirect(GL_TEXTURE_2D, srcTexture);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);

    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, slot.yTexture, 0);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    if (oglViewport) {
        oglViewport(0, 0, srcW, srcH);
    } else {
        glViewport(0, 0, srcW, srcH);
    }
    glUniform1i(g_virtualCameraNv12ShaderLocs.outputMode, 0);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, slot.uvTexture, 0);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    if (oglViewport) {
        oglViewport(0, 0, srcW / 2, srcH / 2);
    } else {
        glViewport(0, 0, srcW / 2, srcH / 2);
    }
    glUniform1i(g_virtualCameraNv12ShaderLocs.outputMode, 1);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    BindTextureDirect(GL_TEXTURE_2D, previousTexture0);
    glBindVertexArray(previousVertexArray);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previousDrawFbo);
    if (blendEnabled) {
        glEnable(GL_BLEND);
    } else {
        glDisable(GL_BLEND);
    }
    if (scissorEnabled) {
        glEnable(GL_SCISSOR_TEST);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
    if (oglViewport) {
        oglViewport(previousViewport[0], previousViewport[1], previousViewport[2], previousViewport[3]);
    } else {
        glViewport(previousViewport[0], previousViewport[1], previousViewport[2], previousViewport[3]);
    }
    glUseProgram(previousProgram);
    glActiveTexture(GL_TEXTURE0);
    BindTextureDirect(GL_TEXTURE_2D, previousTexture0);
    glActiveTexture(previousActiveTexture);

    return true;
}

void CaptureSameThreadVirtualCameraFrame() {
    if (!IsVirtualCameraActive()) { return; }

    uint32_t vcWidth = 0;
    uint32_t vcHeight = 0;
    if (!GetVirtualCameraResolution(vcWidth, vcHeight) || vcWidth < 2 || vcHeight < 2) { return; }

    int outW = static_cast<int>(vcWidth);
    int outH = static_cast<int>(vcHeight);
    if ((outW & 1) != 0) { --outW; }
    if ((outH & 1) != 0) { --outH; }
    if (outW <= 0 || outH <= 0) { return; }

    GLuint sourceTexture = 0;
    int captureSourceW = 0;
    int captureSourceH = 0;
    const GLuint obsOverrideTexture = g_obsOverrideTexture.load(std::memory_order_acquire);
    const int obsOverrideW = g_obsOverrideWidth.load(std::memory_order_acquire);
    const int obsOverrideH = g_obsOverrideHeight.load(std::memory_order_acquire);
    if (obsOverrideTexture != 0 && obsOverrideW > 0 && obsOverrideH > 0) {
        sourceTexture = obsOverrideTexture;
        captureSourceW = obsOverrideW;
        captureSourceH = obsOverrideH;
    }
    if (sourceTexture == 0 || captureSourceW <= 0 || captureSourceH <= 0) { return; }

    const bool sourceSizeChanged = g_sameThreadVirtualCameraCaptureSourceW > 0 && g_sameThreadVirtualCameraCaptureSourceH > 0 &&
                                   (g_sameThreadVirtualCameraCaptureSourceW != captureSourceW ||
                                    g_sameThreadVirtualCameraCaptureSourceH != captureSourceH);
    if (sourceSizeChanged) {
        // Pending async readbacks captured against the old backbuffer size can surface as mixed stale frames.
        // Drop the queue but keep the GPU ring allocated so resize recovery does not stall on reallocation.
        DiscardSameThreadVirtualCameraReadbacks();
    }
    g_sameThreadVirtualCameraCaptureSourceW = captureSourceW;
    g_sameThreadVirtualCameraCaptureSourceH = captureSourceH;

    HarvestSameThreadVirtualCameraReadback();

    const GLuint readTexture = PrepareSameThreadVirtualCameraTexture(sourceTexture, captureSourceW, captureSourceH, outW, outH);
    if (readTexture == 0) { return; }

    LARGE_INTEGER counter;
    LARGE_INTEGER frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    const uint64_t timestamp = (counter.QuadPart * 10000000ULL) / frequency.QuadPart;

    if (g_sameThreadVirtualCameraSynchronousRecoveryFrames > 0) {
        if (SubmitSameThreadVirtualCameraFrameSync(readTexture, outW, outH, timestamp)) {
            --g_sameThreadVirtualCameraSynchronousRecoveryFrames;
        }
        return;
    }

    if (g_sameThreadVirtualCameraReadFBO == 0) { glGenFramebuffers(1, &g_sameThreadVirtualCameraReadFBO); }

    if (!EnsureSameThreadVirtualCameraReadbacks(outW, outH)) { return; }

    SameThreadVirtualCameraReadbackSlot* slot = AcquireSameThreadVirtualCameraReadbackSlot();
    if (!slot || slot->yPbo == 0 || slot->uvPbo == 0 || slot->yTexture == 0 || slot->uvTexture == 0) { return; }
    if (!ConvertSameThreadVirtualCameraTextureToNv12(readTexture, outW, outH, *slot)) { return; }

    GLint previousReadFbo = 0;
    GLint previousPackBuffer = 0;
    GLint previousPackAlignment = 0;
    GLint previousPackRowLength = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFbo);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previousPackBuffer);
    glGetIntegerv(GL_PACK_ALIGNMENT, &previousPackAlignment);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &previousPackRowLength);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_sameThreadVirtualCameraReadFBO);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);

    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, slot->yTexture, 0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, slot->yPbo);
    glReadPixels(0, 0, outW, outH, GL_RED, GL_UNSIGNED_BYTE, nullptr);

    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, slot->uvTexture, 0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, slot->uvPbo);
    glReadPixels(0, 0, outW / 2, outH / 2, GL_RG, GL_UNSIGNED_BYTE, nullptr);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, previousReadFbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, previousPackBuffer);
    glPixelStorei(GL_PACK_ROW_LENGTH, previousPackRowLength);
    glPixelStorei(GL_PACK_ALIGNMENT, previousPackAlignment);

    if (slot->fence && glIsSync(slot->fence)) { glDeleteSync(slot->fence); }
    slot->fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    slot->pending = (slot->fence != nullptr);
    slot->timestamp = timestamp;
    slot->width = outW;
    slot->height = outH;
}

static GLuint ResolveModeBackgroundTextureId(const std::string& modeId, float outSourceRect[4] = nullptr) {
    std::lock_guard<std::mutex> bgLock(g_backgroundTexturesMutex);
    auto bgTexIt = g_backgroundTextures.find(modeId);
    if (bgTexIt == g_backgroundTextures.end()) {
        if (outSourceRect) {
            outSourceRect[0] = 0.0f;
            outSourceRect[1] = 0.0f;
            outSourceRect[2] = 1.0f;
            outSourceRect[3] = 1.0f;
        }
        return 0;
    }

    BackgroundTextureInstance& bgInst = bgTexIt->second;
    const auto animatedTexture = ResolveAnimatedTexture(bgInst);
    if (outSourceRect) {
        memcpy(outSourceRect, animatedTexture.sourceRect, sizeof(animatedTexture.sourceRect));
    }
    return animatedTexture.textureId;
}

static void DrawFullscreenSolidColor(const Color& color) {
    glUseProgram(g_solidColorProgram);
    glUniform4f(g_solidColorShaderLocs.color, color.r, color.g, color.b, color.a);
    glBindVertexArray(g_fullscreenQuadVAO);
    glDisable(GL_BLEND);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

static void DrawFullscreenGradient(const BackgroundConfig& bg) {
    if (bg.gradientStops.size() < 2) {
        DrawFullscreenSolidColor(bg.color);
        return;
    }

    glUseProgram(g_gradientProgram);
    glBindVertexArray(g_fullscreenQuadVAO);
    glDisable(GL_BLEND);

    const int numStops = (std::min)(static_cast<int>(bg.gradientStops.size()), MAX_GRADIENT_STOPS);
    glUniform1i(g_gradientShaderLocs.numStops, numStops);

    float colors[MAX_GRADIENT_STOPS * 4];
    float positions[MAX_GRADIENT_STOPS];
    for (int i = 0; i < numStops; ++i) {
        colors[i * 4 + 0] = bg.gradientStops[i].color.r;
        colors[i * 4 + 1] = bg.gradientStops[i].color.g;
        colors[i * 4 + 2] = bg.gradientStops[i].color.b;
        colors[i * 4 + 3] = bg.gradientStops[i].color.a;
        positions[i] = bg.gradientStops[i].position;
    }

    glUniform4fv(g_gradientShaderLocs.stopColors, numStops, colors);
    glUniform1fv(g_gradientShaderLocs.stopPositions, numStops, positions);
    glUniform1f(g_gradientShaderLocs.angle, bg.gradientAngle * 3.14159265f / 180.0f);

    static auto startTime = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const float timeSeconds = std::chrono::duration<float>(now - startTime).count();
    glUniform1f(g_gradientShaderLocs.time, timeSeconds);
    glUniform1i(g_gradientShaderLocs.animationType, static_cast<int>(bg.gradientAnimation));
    glUniform1f(g_gradientShaderLocs.animationSpeed, bg.gradientAnimationSpeed);
    glUniform1i(g_gradientShaderLocs.colorFade, bg.gradientColorFade ? 1 : 0);

    glDrawArrays(GL_TRIANGLES, 0, 6);
}

static void DrawFullscreenPassthroughTexture(GLuint textureId, float opacity) {
    const float sourceRect[] = { 0.0f, 0.0f, 1.0f, 1.0f };
    if (textureId == 0) { return; }

    glUseProgram(g_passthroughProgram);
    BindTextureDirect(GL_TEXTURE_2D, textureId);
    glUniform4f(g_passthroughShaderLocs.sourceRect, sourceRect[0], sourceRect[1], sourceRect[2], sourceRect[3]);
    glUniform1f(g_passthroughShaderLocs.opacity, opacity);
    glUniform2f(g_passthroughShaderLocs.sourceTexelSize, 1.0f, 1.0f);
    glUniform2f(g_passthroughShaderLocs.sourcePixelSize, 1.0f, 1.0f);
    glUniform1i(g_passthroughShaderLocs.snapToSourcePixels, 0);
    glBindVertexArray(g_fullscreenQuadVAO);
    glDisable(GL_BLEND);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

static void DrawFullscreenPassthroughTexture(GLuint textureId, const float sourceRect[4], float opacity) {
    if (textureId == 0) { return; }

    glUseProgram(g_passthroughProgram);
    glActiveTexture(GL_TEXTURE0);
    BindTextureDirect(GL_TEXTURE_2D, textureId);
    glUniform4f(g_passthroughShaderLocs.sourceRect, sourceRect[0], sourceRect[1], sourceRect[2], sourceRect[3]);
    glUniform1f(g_passthroughShaderLocs.opacity, opacity);
    glUniform2f(g_passthroughShaderLocs.sourceTexelSize, 1.0f, 1.0f);
    glUniform2f(g_passthroughShaderLocs.sourcePixelSize, 1.0f, 1.0f);
    glUniform1i(g_passthroughShaderLocs.snapToSourcePixels, 0);
    glBindVertexArray(g_fullscreenQuadVAO);
    glDisable(GL_BLEND);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

static void RenderSameThreadObsBackground(const ModeConfig* modeToRender, int fullW, int fullH) {
    if (!modeToRender || fullW <= 0 || fullH <= 0) { return; }

    const BackgroundConfig& bg = modeToRender->background;
    if (bg.selectedMode == "image") {
        float sourceRect[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
        const GLuint backgroundTexture = ResolveModeBackgroundTextureId(modeToRender->id, sourceRect);
        if (backgroundTexture != 0) {
            DrawFullscreenPassthroughTexture(backgroundTexture, sourceRect, 1.0f);
            return;
        }
    }

    if (bg.selectedMode == "gradient") {
        DrawFullscreenGradient(bg);
        return;
    }

    DrawFullscreenSolidColor(bg.color);
}

static void RenderSameThreadObsBackgroundConfig(const BackgroundConfig& bg, GLuint backgroundTexture, const float sourceRect[4], int fullW,
                                               int fullH) {
    if (fullW <= 0 || fullH <= 0) { return; }

    if (bg.selectedMode == "image" && backgroundTexture != 0) {
        DrawFullscreenPassthroughTexture(backgroundTexture, sourceRect, 1.0f);
        return;
    }

    if (bg.selectedMode == "gradient") {
        DrawFullscreenGradient(bg);
        return;
    }

    DrawFullscreenSolidColor(bg.color);
}

bool RenderSameThreadObsFrame(const ModeConfig* modeToRender, const GLState& s, int current_gameW, int current_gameH,
                              bool skipAnimation) {
    if (!modeToRender) { return false; }

    int fullW = 0;
    int fullH = 0;
    {
        PROFILE_SCOPE_CAT("Resolve OBS Output Size", "OBS");
        fullW = GetCachedWindowWidth();
        fullH = GetCachedWindowHeight();
    }
    if (fullW <= 0 || fullH <= 0) { return false; }

    {
        PROFILE_SCOPE_CAT("Prepare OBS Compose Target", "OBS");
        EnsureSameThreadObsComposeTarget(fullW, fullH);
    }
    const int composeIndex = g_sameThreadObsComposeWriteIndex;
    if (composeIndex < 0 || composeIndex >= SAME_THREAD_OBS_BUFFER_COUNT || g_sameThreadObsComposeFBOs[composeIndex] == 0 ||
        g_sameThreadObsComposeTextures[composeIndex] == 0) {
        return false;
    }

    ModeTransitionState transitionState;
    bool isAnimating = false;
    std::string fromModeId;
    bool transitioningToFullscreen = false;
    const ModeConfig* fromMode = nullptr;
    int finalX = 0;
    int finalY = 0;
    int finalW = 0;
    int finalH = 0;
    {
        PROFILE_SCOPE_CAT("Resolve OBS Frame Geometry", "OBS");
        transitionState = GetModeTransitionState();
        const bool transitionEffectivelyComplete =
            transitionState.active && transitionState.width == transitionState.targetWidth &&
            transitionState.height == transitionState.targetHeight && transitionState.x == transitionState.targetX &&
            transitionState.y == transitionState.targetY;
        isAnimating = transitionState.active && !skipAnimation && !transitionEffectivelyComplete;
        fromModeId = transitionState.fromModeId;
        transitioningToFullscreen = isAnimating && EqualsIgnoreCase(modeToRender->id, "Fullscreen");

        int modeWidth = modeToRender->width;
        int modeHeight = modeToRender->height;
        int modeX = 0;
        int modeY = 0;
        if (isAnimating) {
            modeWidth = transitionState.width;
            modeHeight = transitionState.height;
            modeX = transitionState.x;
            modeY = transitionState.y;
        }

        if (isAnimating) {
            finalX = modeX;
            finalY = modeY;
            finalW = modeWidth;
            finalH = modeHeight;
        } else if (modeToRender->stretch.enabled) {
            finalX = modeToRender->stretch.x;
            finalY = modeToRender->stretch.y;
            finalW = modeToRender->stretch.width;
            finalH = modeToRender->stretch.height;
        } else {
            finalW = modeWidth;
            finalH = modeHeight;
            finalX = GetCenteredAxisOffset(fullW, finalW);
            finalY = GetCenteredAxisOffset(fullH, finalH);
        }
    }

    GLuint gameTextureToUse = 0;
    int gameTextureW = 0;
    int gameTextureH = 0;
    {
        PROFILE_SCOPE_CAT("Resolve OBS Source Texture", "OBS");
        if (!SelectSameThreadGameTexture(g_cachedGameTextureId.load(std::memory_order_acquire), current_gameW, current_gameH,
                                         gameTextureToUse, gameTextureW, gameTextureH)) {
            return false;
        }
    }

    GLState obsState = s;
    obsState.fb = g_sameThreadObsComposeFBOs[composeIndex];
    obsState.read_fb = g_sameThreadObsComposeFBOs[composeIndex];
    obsState.draw_fb = g_sameThreadObsComposeFBOs[composeIndex];
    obsState.draw_buffer = GL_COLOR_ATTACHMENT0;
    obsState.read_buffer = GL_COLOR_ATTACHMENT0;

    {
        PROFILE_SCOPE_CAT("Prepare OBS Compose State", "OBS");
        PrepareSameThreadOverlayState(obsState, fullW, fullH);
    }

    bool useFromBackground = false;
    BackgroundConfig fromBackground;
    BorderConfig fromBorder;
    {
        PROFILE_SCOPE_CAT("Resolve OBS Background Source", "OBS");
        if (isAnimating && !fromModeId.empty()) {
            fromMode = GetMode_Internal(fromModeId);
            if (fromMode) {
                fromBackground = fromMode->background;
                fromBorder = fromMode->border;
                const bool fromHasSpecialBackground =
                    (fromBackground.selectedMode == "gradient" || fromBackground.selectedMode == "image");
                useFromBackground = transitioningToFullscreen || fromHasSpecialBackground;
            }
        }
    }

    GLuint obsBackgroundTexture = 0;
    float obsBackgroundSourceRect[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    {
        PROFILE_SCOPE_CAT("Resolve OBS Background Texture", "OBS");
        if (useFromBackground) {
            if (fromBackground.selectedMode == "image") {
                obsBackgroundTexture = ResolveModeBackgroundTextureId(fromModeId, obsBackgroundSourceRect);
            }
        } else if (modeToRender->background.selectedMode == "image") {
            obsBackgroundTexture = ResolveModeBackgroundTextureId(modeToRender->id, obsBackgroundSourceRect);
        }
    }

    {
        PROFILE_SCOPE_CAT("Render OBS Background", "OBS");
        if (useFromBackground) {
            RenderSameThreadObsBackgroundConfig(fromBackground, obsBackgroundTexture, obsBackgroundSourceRect, fullW, fullH);
        } else {
            RenderSameThreadObsBackgroundConfig(modeToRender->background, obsBackgroundTexture, obsBackgroundSourceRect, fullW, fullH);
        }
    }

    {
        PROFILE_SCOPE_CAT("Render OBS Game View", "OBS");
        const float sourceRect[] = { 0.0f, 0.0f, 1.0f, 1.0f };
        const int dstBottom = fullH - finalY - finalH;
        DrawPassthroughTextureRegion(gameTextureToUse, sourceRect, finalX, dstBottom, finalX + finalW, dstBottom + finalH, fullW,
                                     fullH, 1.0f);
    }

    {
        PROFILE_SCOPE_CAT("Render OBS Border", "OBS");
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        if (transitioningToFullscreen && fromBorder.enabled && fromBorder.width > 0) {
            RenderGameBorder(finalX, finalY, finalW, finalH, fromBorder.width, fromBorder.radius, fromBorder.color, fullW, fullH);
        }
        else if (modeToRender->border.enabled && modeToRender->border.width > 0) {
            RenderGameBorder(finalX, finalY, finalW, finalH, modeToRender->border.width, modeToRender->border.radius,
                modeToRender->border.color, fullW, fullH);
        };
        glDisable(GL_BLEND);
    }

    if (auto cfgSnap = GetConfigSnapshot()) {
        SameThreadOverlayState request;
        {
            PROFILE_SCOPE_CAT("Build OBS Overlay Request", "OBS");
            request.fullW = fullW;
            request.fullH = fullH;
            request.gameW = current_gameW;
            request.gameH = current_gameH;
            request.finalX = finalX;
            request.finalY = finalY;
            request.finalW = finalW;
            request.finalH = finalH;
            request.gameTextureId = gameTextureToUse;
            request.modeId = modeToRender->id;
            request.isAnimating = isAnimating;
            request.overlayOpacity = 1.0f;
            request.excludeOnlyOnMyScreen = true;
            request.skipAnimation = skipAnimation;
            request.relativeStretching = modeToRender->relativeStretching;

            const bool transitionEffectivelyCompleteForOverlays = transitionState.active && transitionState.moveProgress >= 1.0f;
            const bool overlaysShouldLerp = transitionState.active && !transitionEffectivelyCompleteForOverlays &&
                                            transitionState.overlayTransition != OverlayTransitionType::Cut;
            if (overlaysShouldLerp) {
                request.transitionProgress = transitionState.moveProgress;
                request.fromW = transitionState.fromWidth;
                request.fromH = transitionState.fromHeight;
                request.fromX = transitionState.fromX;
                request.fromY = transitionState.fromY;
                request.toW = transitionState.targetWidth;
                request.toH = transitionState.targetHeight;
                request.toX = transitionState.targetX;
                request.toY = transitionState.targetY;
            } else if (transitionState.active) {
                request.transitionProgress = 1.0f;
                request.fromX = transitionState.fromX;
                request.fromY = transitionState.fromY;
                request.fromW = transitionState.fromWidth;
                request.fromH = transitionState.fromHeight;
                request.toX = transitionState.targetX;
                request.toY = transitionState.targetY;
                request.toW = transitionState.targetWidth;
                request.toH = transitionState.targetHeight;
            } else {
                request.transitionProgress = 1.0f;
                request.fromX = finalX;
                request.fromY = finalY;
                request.fromW = finalW;
                request.fromH = finalH;
                request.toX = finalX;
                request.toY = finalY;
                request.toW = finalW;
                request.toH = finalH;
            }
            if (transitionState.active) {
                request.fromFullW = transitionState.fromNativeWidth > 0 ? transitionState.fromNativeWidth : fullW;
                request.fromFullH = transitionState.fromNativeHeight > 0 ? transitionState.fromNativeHeight : fullH;
                request.toFullW = transitionState.toNativeWidth > 0 ? transitionState.toNativeWidth : fullW;
                request.toFullH = transitionState.toNativeHeight > 0 ? transitionState.toNativeHeight : fullH;
            } else {
                request.fromFullW = fullW;
                request.fromFullH = fullH;
                request.toFullW = fullW;
                request.toFullH = fullH;
            }

            const bool slideAnimationsEnabled = transitionState.active && transitionState.gameTransition == GameTransitionType::Bounce;

            request.isTransitioningFromEyeZoom =
                slideAnimationsEnabled && g_isTransitioningFromEyeZoom.load(std::memory_order_acquire);
            request.shouldRenderGui = g_shouldRenderGui.load(std::memory_order_relaxed);
            request.showPerformanceOverlay = false;
            request.showProfiler = false;
            request.showEyeZoom = EqualsIgnoreCase(request.modeId, "EyeZoom") ||
                                  (request.isTransitioningFromEyeZoom && !request.skipAnimation);
            request.eyeZoomFadeOpacity = g_eyeZoomFadeOpacity.load(std::memory_order_relaxed);
            request.eyeZoomAnimatedViewportX = (skipAnimation || !slideAnimationsEnabled)
                                                  ? -1
                                                  : g_eyeZoomAnimatedViewportX.load(std::memory_order_relaxed);
            request.eyeZoomSnapshotTexture = GetEyeZoomSnapshotTexture();
            request.eyeZoomSnapshotWidth = GetEyeZoomSnapshotWidth();
            request.eyeZoomSnapshotHeight = GetEyeZoomSnapshotHeight();
            request.showTextureGrid = false;
            request.textureGridModeWidth = 0;
            request.textureGridModeHeight = 0;
            request.showWelcomeToast = false;
            request.welcomeToastIsFullscreen = false;
            request.showCursorTrail = cfgSnap->cursorTrail.enabled && IsCursorVisible();
            request.modeHasMirrors = !modeToRender->mirrorIds.empty() || !modeToRender->mirrorGroupIds.empty();
            request.modeHasImages = g_imageOverlaysVisible.load(std::memory_order_acquire) && !modeToRender->imageIds.empty();
            request.modeHasWindowOverlays = g_windowOverlaysVisible.load(std::memory_order_acquire) &&
                                            !modeToRender->windowOverlayIds.empty();
            request.modeHasBrowserOverlays = g_browserOverlaysVisible.load(std::memory_order_acquire) &&
                                             !modeToRender->browserOverlayIds.empty();
            request.isRawWindowedMode = false;
            request.fromModeId = transitionState.fromModeId;
            request.fromSlideMirrorsIn = fromMode && fromMode->slideMirrorsIn;
            request.toSlideMirrorsIn = modeToRender->slideMirrorsIn;
            request.mirrorSlideProgress =
                (slideAnimationsEnabled && transitionState.moveProgress < 1.0f) ? transitionState.moveProgress : 1.0f;
            request.allowMirrorCaptureReuse = true;
            request.mirrorCaptureFrameTag = s_sameThreadMirrorCaptureFrameTag;
        }

        PROFILE_SCOPE_CAT("Render OBS Overlays", "OBS");
        RenderSameThreadOverlayPass(request, *cfgSnap, obsState);

        if (cfgSnap->captureFakeCursor) {
            HWND hwnd = g_minecraftHwnd.load(std::memory_order_acquire);
            if (hwnd) {
                PROFILE_SCOPE_CAT("Render OBS Fake Cursor", "OBS");
                RenderFakeCursorToCurrentTarget(hwnd, fullW, fullH, finalX, finalY, finalW, finalH, current_gameW,
                                                current_gameH);
            }
        }
    }

    {
        PROFILE_SCOPE_CAT("Publish OBS Override", "OBS");
        SetObsOverrideTexture(g_sameThreadObsComposeTextures[composeIndex], fullW, fullH);
        g_sameThreadObsComposePublishedIndex = composeIndex;
        g_sameThreadObsComposeWriteIndex = (composeIndex + 1) % SAME_THREAD_OBS_BUFFER_COUNT;
    }
    return true;
}

void handleEyeZoomMode(const GLState& s, const EyeZoomConfig& zoomConfig, int fullW, int fullH, float opacity,
                       int animatedViewportX, bool useSnapshot, GLuint preferredGameTexture, int preferredGameW,
                       int preferredGameH) {
    PROFILE_SCOPE_CAT("EyeZoom Mode Rendering", "Rendering");

    if (opacity <= 0.0f || fullW <= 0 || fullH <= 0) { return; }

    GLuint gameTextureToUse = 0;
    int gameTextureW = 0;
    int gameTextureH = 0;
    const char* selectedCaptureSource = useSnapshot ? "snapshot" : "none";

    if (useSnapshot && !s_eyeZoomSnapshotValid) {
        LogEyeZoomDebugThrottled("snapshot_state",
                                 "snapshot requested but invalid tex=" + std::to_string(s_eyeZoomSnapshotTexture) + " size=" +
                                     std::to_string(s_eyeZoomSnapshotWidth) + "x" + std::to_string(s_eyeZoomSnapshotHeight));
        return;
    }

    if (!useSnapshot &&
        !SelectEyeZoomCaptureTexture(preferredGameTexture, preferredGameW, preferredGameH, gameTextureToUse, gameTextureW,
                                     gameTextureH, &selectedCaptureSource, true)) {
        int preferredActualW = 0;
        int preferredActualH = 0;
        bool preferredValid =
            (preferredGameTexture != 0 && preferredGameTexture != UINT_MAX) && IsSampleableTexture2D(preferredGameTexture, &preferredActualW, &preferredActualH);
        LogEyeZoomDebugThrottled(
            "source_failure",
            "no usable source preferredTex=" + std::to_string(preferredGameTexture) + " expected=" +
                std::to_string(preferredGameW) + "x" + std::to_string(preferredGameH) + " actual=" +
                std::to_string(preferredActualW) + "x" + std::to_string(preferredActualH) + " preferredValid=" +
                std::to_string(preferredValid ? 1 : 0));
        return;
    }

    if (!useSnapshot) {
        LogEyeZoomDebugThrottled("source_select",
                                 std::string("selected=") + selectedCaptureSource + " tex=" + std::to_string(gameTextureToUse) +
                                     " size=" + std::to_string(gameTextureW) + "x" + std::to_string(gameTextureH) +
                                     " preferredTex=" + std::to_string(preferredGameTexture) + " preferredExpected=" +
                                     std::to_string(preferredGameW) + "x" + std::to_string(preferredGameH));
    }

    glBindFramebuffer(GL_FRAMEBUFFER, s.fb);
    if (s.fb == 0) {
        glDrawBuffer(s.draw_buffer);
        glReadBuffer(s.read_buffer);
    }
    if (oglViewport)
        oglViewport(0, 0, fullW, fullH);
    else
        glViewport(0, 0, fullW, fullH);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glDisable(GL_SCISSOR_TEST);

    int modeWidth = zoomConfig.windowWidth;
    int targetViewportX = GetCenteredAxisOffset(fullW, modeWidth);

    int viewportX;
    if (animatedViewportX >= 0) {
        viewportX = animatedViewportX;
    } else {
        viewportX = targetViewportX;
    }

    int zoomOutputWidth = 0;
    int zoomOutputHeight = 0;
    int zoomX = 0;
    int zoomY = 0;

    int finalZoomOutputWidth = 0;
    int finalZoomOutputHeight = 0;
    int finalZoomX = 0;
    int finalZoomY = 0;

    if (zoomConfig.useCustomSizePosition) {
        finalZoomOutputWidth = zoomConfig.zoomAreaWidth;
        finalZoomOutputHeight = zoomConfig.zoomAreaHeight;
        finalZoomX = zoomConfig.positionX;
        finalZoomY = zoomConfig.positionY;

        zoomOutputWidth = finalZoomOutputWidth;
        zoomOutputHeight = finalZoomOutputHeight;
        zoomY = finalZoomY;
    } else {
        int autoHorizontalMargin = 0;
        if (targetViewportX > 0) autoHorizontalMargin = targetViewportX / 10;
        int autoVerticalMargin = fullH / 8;
        finalZoomOutputWidth = targetViewportX - (2 * autoHorizontalMargin);
        finalZoomOutputHeight = fullH - (2 * autoVerticalMargin);
        finalZoomX = autoHorizontalMargin;
        finalZoomY = GetCenteredAxisOffset(fullH, finalZoomOutputHeight);

        zoomOutputWidth = finalZoomOutputWidth;
        zoomOutputHeight = finalZoomOutputHeight;
        zoomY = finalZoomY;

        if (animatedViewportX >= 0 && targetViewportX > 0) {
            const float slideProgress =
                (std::max)(0.0f, (std::min)(1.0f, static_cast<float>(viewportX) / static_cast<float>(targetViewportX)));
            zoomX = -zoomOutputWidth + static_cast<int>((finalZoomX + zoomOutputWidth) * slideProgress);
        } else {
            zoomX = finalZoomX;
        }
    }

    if (zoomOutputWidth > fullW) zoomOutputWidth = fullW;
    if (finalZoomOutputWidth > fullW) finalZoomOutputWidth = fullW;

    if (zoomOutputWidth <= 20) {
        LogEyeZoomDebugThrottled("layout_skip",
                                 "zoom output width too small width=" + std::to_string(zoomOutputWidth) + " full=" +
                                     std::to_string(fullW) + "x" + std::to_string(fullH) + " viewportX=" +
                                     std::to_string(viewportX));
        return;
    }
    if (finalZoomOutputWidth < 1) { finalZoomOutputWidth = zoomOutputWidth; }

    if (zoomOutputHeight > fullH) zoomOutputHeight = fullH;
    if (finalZoomOutputHeight > fullH) finalZoomOutputHeight = fullH;

    if (zoomOutputHeight < 1) { zoomOutputHeight = 1; }
    if (finalZoomOutputHeight < 1) { finalZoomOutputHeight = zoomOutputHeight; }

    int maxZoomX = (std::max)(0, fullW - zoomOutputWidth);
    int maxZoomY = (std::max)(0, fullH - zoomOutputHeight);
    const bool allowEyeZoomSlideOffscreenLeft = animatedViewportX >= 0 && targetViewportX > 0;
    if (allowEyeZoomSlideOffscreenLeft) {
        zoomX = (std::min)(zoomX, maxZoomX);
    } else {
        zoomX = (std::max)(0, (std::min)(zoomX, maxZoomX));
    }
    zoomY = (std::max)(0, (std::min)(zoomY, maxZoomY));

    int maxFinalZoomX = (std::max)(0, fullW - finalZoomOutputWidth);
    int maxFinalZoomY = (std::max)(0, fullH - finalZoomOutputHeight);
    finalZoomX = (std::max)(0, (std::min)(finalZoomX, maxFinalZoomX));
    finalZoomY = (std::max)(0, (std::min)(finalZoomY, maxFinalZoomY));

    if (zoomConfig.useCustomSizePosition) {
        zoomY = finalZoomY;
        if (animatedViewportX >= 0 && targetViewportX > 0) {
            const float slideProgress =
                (std::max)(0.0f, (std::min)(1.0f, static_cast<float>(viewportX) / static_cast<float>(targetViewportX)));
            zoomX = -zoomOutputWidth + static_cast<int>((finalZoomX + zoomOutputWidth) * slideProgress);
        } else {
            zoomX = finalZoomX;
        }
    }

    int zoomY_gl = fullH - zoomY - zoomOutputHeight;

    int texWidth = useSnapshot ? s_eyeZoomSnapshotWidth : gameTextureW;
    int texHeight = useSnapshot ? s_eyeZoomSnapshotHeight : gameTextureH;

    int srcCenterX = texWidth / 2;
    int srcLeft = srcCenterX - zoomConfig.cloneWidth / 2;
    int srcRight = srcCenterX + zoomConfig.cloneWidth / 2;

    int srcCenterY = texHeight / 2;
    int srcBottom = srcCenterY - zoomConfig.cloneHeight / 2;
    int srcTop = srcCenterY + zoomConfig.cloneHeight / 2;

    srcLeft = (std::max)(0, srcLeft);
    srcBottom = (std::max)(0, srcBottom);
    srcRight = (std::min)(texWidth, srcRight);
    srcTop = (std::min)(texHeight, srcTop);
    if (srcRight <= srcLeft || srcTop <= srcBottom) {
        LogEyeZoomDebugThrottled("source_rect",
                                 "invalid source rect tex=" + std::to_string(texWidth) + "x" + std::to_string(texHeight) +
                                     " clone=" + std::to_string(zoomConfig.cloneWidth) + "x" +
                                     std::to_string(zoomConfig.cloneHeight) + " rect=" + std::to_string(srcLeft) + "," +
                                     std::to_string(srcBottom) + " -> " + std::to_string(srcRight) + "," +
                                     std::to_string(srcTop));
        return;
    }

    int dstLeft = zoomX;
    int dstRight = zoomX + zoomOutputWidth;
    int dstBottom = zoomY_gl;
    int dstTop = zoomY_gl + zoomOutputHeight;
    const int sourcePixelWidth = srcRight - srcLeft;
    const int sourcePixelHeight = srcTop - srcBottom;

    LogEyeZoomDebugThrottled("render_state",
                             std::string("mode=") + (useSnapshot ? "snapshot" : "live") + " source=" + selectedCaptureSource +
                                 " tex=" + std::to_string(useSnapshot ? s_eyeZoomSnapshotTexture : gameTextureToUse) +
                                 " texSize=" + std::to_string(texWidth) + "x" + std::to_string(texHeight) + " srcRect=" +
                                 std::to_string(srcLeft) + "," + std::to_string(srcBottom) + " -> " +
                                 std::to_string(srcRight) + "," + std::to_string(srcTop) + " dstRect=" +
                                 std::to_string(dstLeft) + "," + std::to_string(dstBottom) + " -> " +
                                 std::to_string(dstRight) + "," + std::to_string(dstTop) + " opacity=" +
                                 std::to_string(opacity));


    auto EnsureEyeZoomSnapshotAllocated = [&]() -> bool {
        if (s_eyeZoomSnapshotTexture == 0 || s_eyeZoomSnapshotWidth != zoomOutputWidth || s_eyeZoomSnapshotHeight != zoomOutputHeight) {
            if (s_eyeZoomSnapshotTexture != 0) { glDeleteTextures(1, &s_eyeZoomSnapshotTexture); }
            if (s_eyeZoomSnapshotFBO != 0) { glDeleteFramebuffers(1, &s_eyeZoomSnapshotFBO); }
            s_eyeZoomSnapshotTexture = 0;
            s_eyeZoomSnapshotFBO = 0;

            glGenTextures(1, &s_eyeZoomSnapshotTexture);
            BindTextureDirect(GL_TEXTURE_2D, s_eyeZoomSnapshotTexture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, zoomOutputWidth, zoomOutputHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

            glGenFramebuffers(1, &s_eyeZoomSnapshotFBO);
            glBindFramebuffer(GL_FRAMEBUFFER, s_eyeZoomSnapshotFBO);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_eyeZoomSnapshotTexture, 0);

            const GLenum snapshotStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            if (snapshotStatus != GL_FRAMEBUFFER_COMPLETE) {
                LogEyeZoomFramebufferStatusThrottled("snapshot_alloc", s_eyeZoomSnapshotTexture, snapshotStatus, zoomOutputWidth,
                                                    zoomOutputHeight);
                glDeleteTextures(1, &s_eyeZoomSnapshotTexture);
                glDeleteFramebuffers(1, &s_eyeZoomSnapshotFBO);
                s_eyeZoomSnapshotTexture = 0;
                s_eyeZoomSnapshotFBO = 0;
                s_eyeZoomSnapshotWidth = 0;
                s_eyeZoomSnapshotHeight = 0;
                s_eyeZoomSnapshotValid = false;
                return false;
            }

            s_eyeZoomSnapshotWidth = zoomOutputWidth;
            s_eyeZoomSnapshotHeight = zoomOutputHeight;
            s_eyeZoomSnapshotValid = false;
        }

        return s_eyeZoomSnapshotTexture != 0 && s_eyeZoomSnapshotFBO != 0;
    };

    auto EnsureEyeZoomTempAllocated = [&]() -> bool {
        if (s_eyeZoomTempTexture == 0 || s_eyeZoomTempWidth != zoomOutputWidth || s_eyeZoomTempHeight != zoomOutputHeight) {
            if (s_eyeZoomTempTexture != 0) { glDeleteTextures(1, &s_eyeZoomTempTexture); }
            if (s_eyeZoomTempFBO != 0) { glDeleteFramebuffers(1, &s_eyeZoomTempFBO); }
            s_eyeZoomTempTexture = 0;
            s_eyeZoomTempFBO = 0;

            glGenTextures(1, &s_eyeZoomTempTexture);
            BindTextureDirect(GL_TEXTURE_2D, s_eyeZoomTempTexture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, zoomOutputWidth, zoomOutputHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

            glGenFramebuffers(1, &s_eyeZoomTempFBO);
            glBindFramebuffer(GL_FRAMEBUFFER, s_eyeZoomTempFBO);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_eyeZoomTempTexture, 0);

            const GLenum tempStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            if (tempStatus != GL_FRAMEBUFFER_COMPLETE) {
                LogEyeZoomFramebufferStatusThrottled("temp_alloc", s_eyeZoomTempTexture, tempStatus, zoomOutputWidth,
                                                    zoomOutputHeight);
                glDeleteTextures(1, &s_eyeZoomTempTexture);
                glDeleteFramebuffers(1, &s_eyeZoomTempFBO);
                s_eyeZoomTempTexture = 0;
                s_eyeZoomTempFBO = 0;
                s_eyeZoomTempWidth = 0;
                s_eyeZoomTempHeight = 0;
                return false;
            }

            s_eyeZoomTempWidth = zoomOutputWidth;
            s_eyeZoomTempHeight = zoomOutputHeight;
        }

        return s_eyeZoomTempTexture != 0 && s_eyeZoomTempFBO != 0;
    };

    auto ForceOpaqueAlphaInCurrentDrawFbo = [&](int x, int y, int w, int h) {
        if (w <= 0 || h <= 0) { return; }

        GLboolean prevColorMask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
        glGetBooleanv(GL_COLOR_WRITEMASK, prevColorMask);

        GLboolean prevScissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
        GLint prevScissorBox[4] = { 0, 0, 0, 0 };
        if (prevScissorEnabled) { glGetIntegerv(GL_SCISSOR_BOX, prevScissorBox); }

        glEnable(GL_SCISSOR_TEST);
        glScissor(x, y, w, h);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glColorMask(prevColorMask[0], prevColorMask[1], prevColorMask[2], prevColorMask[3]);
        if (prevScissorEnabled) {
            glScissor(prevScissorBox[0], prevScissorBox[1], prevScissorBox[2], prevScissorBox[3]);
        } else {
            glDisable(GL_SCISSOR_TEST);
        }
    };

    if (useSnapshot) {
        const float sourceRect[] = { 0.0f, 0.0f, 1.0f, 1.0f };
        if (opacity < 1.0f) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        } else {
            glDisable(GL_BLEND);
        }
        DrawPassthroughTextureRegion(s_eyeZoomSnapshotTexture, sourceRect, dstLeft, dstBottom, dstRight, dstTop, fullW, fullH,
                                     opacity);
        if (opacity >= 1.0f && s.fb != 0) {
            ForceOpaqueAlphaInCurrentDrawFbo(dstLeft, dstBottom, zoomOutputWidth, zoomOutputHeight);
        }
    } else {
        const float sourceRect[] = {
            static_cast<float>(srcLeft) / gameTextureW,
            static_cast<float>(srcBottom) / gameTextureH,
            static_cast<float>(srcRight - srcLeft) / gameTextureW,
            static_cast<float>(srcTop - srcBottom) / gameTextureH,
        };
        const float fullSourceRect[] = { 0.0f, 0.0f, 1.0f, 1.0f };
        GLuint displayTexture = gameTextureToUse;
        const float* displaySourceRect = sourceRect;

        if (s.fb == 0 && EnsureEyeZoomTempAllocated()) {
            glBindFramebuffer(GL_FRAMEBUFFER, s_eyeZoomTempFBO);
            glDisable(GL_BLEND);
            DrawPassthroughTextureRegion(gameTextureToUse, sourceRect, 0, 0, s_eyeZoomTempWidth, s_eyeZoomTempHeight,
                                         s_eyeZoomTempWidth, s_eyeZoomTempHeight, 1.0f, true, gameTextureW, gameTextureH,
                                         sourcePixelWidth, sourcePixelHeight);
            displayTexture = s_eyeZoomTempTexture;
            displaySourceRect = fullSourceRect;
            LogEyeZoomDebugThrottled("present_path",
                                     "target=default_fbo via_temp=1 tempTex=" + std::to_string(s_eyeZoomTempTexture) +
                                         " tempSize=" + std::to_string(s_eyeZoomTempWidth) + "x" +
                                         std::to_string(s_eyeZoomTempHeight) + " sourceTex=" +
                                         std::to_string(gameTextureToUse));

            glBindFramebuffer(GL_FRAMEBUFFER, s.fb);
            if (oglViewport)
                oglViewport(0, 0, fullW, fullH);
            else
                glViewport(0, 0, fullW, fullH);
        } else if (s.fb == 0) {
            LogEyeZoomDebugThrottled("present_path",
                                     "target=default_fbo via_temp=0 sourceTex=" + std::to_string(gameTextureToUse) +
                                         " sourceSize=" + std::to_string(gameTextureW) + "x" +
                                         std::to_string(gameTextureH));
        }

        if (opacity < 1.0f) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        } else {
            glDisable(GL_BLEND);
        }
        DrawPassthroughTextureRegion(displayTexture, displaySourceRect, dstLeft, dstBottom, dstRight, dstTop, fullW, fullH,
                                     opacity, displayTexture == gameTextureToUse, gameTextureW, gameTextureH,
                                     sourcePixelWidth, sourcePixelHeight);
        if (opacity >= 1.0f && s.fb != 0) {
            ForceOpaqueAlphaInCurrentDrawFbo(dstLeft, dstBottom, zoomOutputWidth, zoomOutputHeight);
        }

        if (EnsureEyeZoomSnapshotAllocated()) {
            glBindFramebuffer(GL_FRAMEBUFFER, s_eyeZoomSnapshotFBO);
            glDisable(GL_BLEND);
            DrawPassthroughTextureRegion(displayTexture, displaySourceRect, 0, 0, s_eyeZoomSnapshotWidth, s_eyeZoomSnapshotHeight,
                                         s_eyeZoomSnapshotWidth, s_eyeZoomSnapshotHeight, 1.0f,
                                         displayTexture == gameTextureToUse, gameTextureW, gameTextureH, sourcePixelWidth,
                                         sourcePixelHeight);
            s_eyeZoomSnapshotValid = true;
        } else {
            s_eyeZoomSnapshotValid = false;
            LogEyeZoomDebugThrottled("snapshot_copy_state",
                                     std::string("snapshot invalidated after copy source=") + selectedCaptureSource + " tex=" +
                                         std::to_string(displayTexture) + " size=" + std::to_string(zoomOutputWidth) + "x" +
                                         std::to_string(zoomOutputHeight) + " snapshotTex=" +
                                         std::to_string(s_eyeZoomSnapshotTexture) + " snapshotSize=" +
                                         std::to_string(s_eyeZoomSnapshotWidth) + "x" +
                                         std::to_string(s_eyeZoomSnapshotHeight));
        }
        glBindFramebuffer(GL_FRAMEBUFFER, s.fb);
        if (oglViewport)
            oglViewport(0, 0, fullW, fullH);
        else
            glViewport(0, 0, fullW, fullH);
    }

    const float overlayOpacityScale = (std::min)(1.0f, opacity);
    const float textAlpha = (std::min)(1.0f, zoomConfig.textColor.a * zoomConfig.textColorOpacity * overlayOpacityScale);
    Color textColor = zoomConfig.textColor;
    textColor.a = textAlpha;

    int overlayLayoutX = zoomX;
    int overlayLayoutY = zoomY;
    int overlayLayoutWidth = zoomOutputWidth;
    int overlayLayoutHeight = zoomOutputHeight;
    if (!zoomConfig.useCustomSizePosition && animatedViewportX >= 0 && targetViewportX > 0) {
        const float overlaySlideProgress =
            (std::max)(0.0f, (std::min)(1.0f, static_cast<float>(viewportX) / static_cast<float>(targetViewportX)));
        overlayLayoutWidth = finalZoomOutputWidth;
        overlayLayoutHeight = finalZoomOutputHeight;
        overlayLayoutX = -overlayLayoutWidth + static_cast<int>((finalZoomX + overlayLayoutWidth) * overlaySlideProgress);
        overlayLayoutY = finalZoomY;
    }

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(g_solidColorProgram);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);

    GLboolean prevScissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
    GLint prevScissorBox[4] = { 0, 0, 0, 0 };
    if (prevScissorEnabled) {
        glGetIntegerv(GL_SCISSOR_BOX, prevScissorBox);
    }
    glEnable(GL_SCISSOR_TEST);
    glScissor(dstLeft, dstBottom, zoomOutputWidth, zoomOutputHeight);

    const EyeZoomOverlayConfig* activeOverlay = nullptr;
    GLuint activeOverlayTextureId = 0;
    int activeOverlayTextureWidth = 0;
    int activeOverlayTextureHeight = 0;
    float activeOverlaySourceRect[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    if (zoomConfig.activeOverlayIndex >= 0 && zoomConfig.activeOverlayIndex < (int)zoomConfig.overlays.size()) {
        activeOverlay = &zoomConfig.overlays[zoomConfig.activeOverlayIndex];
        std::lock_guard<std::mutex> lock(g_userImagesMutex);
        auto it = g_userImages.find("ezoverlay_" + activeOverlay->name);
        if (it != g_userImages.end() && it->second.textureId != 0) {
            UserImageInstance& inst = it->second;
            const auto animatedTexture = ResolveAnimatedTexture(inst);
            activeOverlayTextureId = animatedTexture.textureId;
            activeOverlayTextureWidth = inst.width;
            activeOverlayTextureHeight = inst.height;
            memcpy(activeOverlaySourceRect, animatedTexture.sourceRect, sizeof(activeOverlaySourceRect));
        }
    }

    const bool useDefaultOverlay = activeOverlay == nullptr || activeOverlayTextureId == 0 || activeOverlayTextureWidth <= 0 ||
                                    activeOverlayTextureHeight <= 0;
    if (useDefaultOverlay) {
        float pixelWidthOnScreen = overlayLayoutWidth / (float)zoomConfig.cloneWidth;
        int labelsPerSide = zoomConfig.cloneWidth / 2;
        int overlayLabelsPerSide = zoomConfig.overlayWidth;
        if (overlayLabelsPerSide < 0) overlayLabelsPerSide = labelsPerSide;
        if (overlayLabelsPerSide > labelsPerSide) overlayLabelsPerSide = labelsPerSide;
        float centerY = overlayLayoutY + overlayLayoutHeight / 2.0f;

        float boxHeight;
        if (zoomConfig.linkRectToFont) {
            boxHeight = g_overlayTextFontSize * 1.2f;
        } else {
            boxHeight = static_cast<float>(zoomConfig.rectHeight);
        }

        std::vector<float> evenVerts, oddVerts;
        evenVerts.reserve(overlayLabelsPerSide * 6 * 4);
        oddVerts.reserve(overlayLabelsPerSide * 6 * 4);

        for (int xOffset = -overlayLabelsPerSide; xOffset <= overlayLabelsPerSide; xOffset++) {
            if (xOffset == 0) continue;

            int boxIndex = xOffset + labelsPerSide - (xOffset > 0 ? 1 : 0);
            float boxLeft = overlayLayoutX + (boxIndex * pixelWidthOnScreen);
            float boxRight = boxLeft + pixelWidthOnScreen;
            float boxBottom = centerY - boxHeight / 2.0f;
            float boxTop = centerY + boxHeight / 2.0f;

            float boxNdcLeft = (boxLeft / (float)fullW) * 2.0f - 1.0f;
            float boxNdcRight = (boxRight / (float)fullW) * 2.0f - 1.0f;
            float boxNdcBottom = 1.0f - (boxTop / (float)fullH) * 2.0f;
            float boxNdcTop = 1.0f - (boxBottom / (float)fullH) * 2.0f;

            auto& verts = (boxIndex % 2 == 0) ? evenVerts : oddVerts;
            float quad[] = {
                boxNdcLeft, boxNdcBottom, 0, 0, boxNdcRight, boxNdcBottom, 0, 0, boxNdcRight, boxNdcTop, 0, 0,
                boxNdcLeft, boxNdcBottom, 0, 0, boxNdcRight, boxNdcTop,    0, 0, boxNdcLeft,  boxNdcTop, 0, 0,
            };
            verts.insert(verts.end(), std::begin(quad), std::end(quad));

            int displayNumber = abs(xOffset);
            float numberCenterX = boxLeft + pixelWidthOnScreen / 2.0f;
            float numberCenterY = centerY;
            CacheEyeZoomTextLabel(displayNumber, numberCenterX, numberCenterY, pixelWidthOnScreen, boxHeight,
                                  zoomConfig.fontSizeMode, textColor, static_cast<float>(zoomX), static_cast<float>(zoomY),
                                  static_cast<float>(zoomX + zoomOutputWidth), static_cast<float>(zoomY + zoomOutputHeight));
        }

        if (!evenVerts.empty()) {
            glUniform4f(g_solidColorShaderLocs.color, zoomConfig.gridColor1.r, zoomConfig.gridColor1.g, zoomConfig.gridColor1.b,
                        zoomConfig.gridColor1Opacity * overlayOpacityScale);
            EnsureSharedVertexBufferCapacity(static_cast<GLsizeiptr>(evenVerts.size() * sizeof(float)));
            glBufferSubData(GL_ARRAY_BUFFER, 0, evenVerts.size() * sizeof(float), evenVerts.data());
            glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(evenVerts.size() / 4));
        }
        if (!oddVerts.empty()) {
            glUniform4f(g_solidColorShaderLocs.color, zoomConfig.gridColor2.r, zoomConfig.gridColor2.g, zoomConfig.gridColor2.b,
                        zoomConfig.gridColor2Opacity * overlayOpacityScale);
            EnsureSharedVertexBufferCapacity(static_cast<GLsizeiptr>(oddVerts.size() * sizeof(float)));
            glBufferSubData(GL_ARRAY_BUFFER, 0, oddVerts.size() * sizeof(float), oddVerts.data());
            glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(oddVerts.size() / 4));
        }
    } else {
        int overlayDisplayW = overlayLayoutWidth;
        int overlayDisplayH = overlayLayoutHeight;
        const bool isManualOverlay = activeOverlay->displayMode == EyeZoomOverlayDisplayMode::Manual;
        const bool clipManualOverlayToZoomArea = isManualOverlay && activeOverlay->clipToZoomArea;
        switch (activeOverlay->displayMode) {
            case EyeZoomOverlayDisplayMode::Manual:
                overlayDisplayW = (std::max)(1, activeOverlay->manualWidth);
                overlayDisplayH = (std::max)(1, activeOverlay->manualHeight);
                break;
            case EyeZoomOverlayDisplayMode::Fit: {
                const float fitScaleX = static_cast<float>(overlayLayoutWidth) / activeOverlayTextureWidth;
                const float fitScaleY = static_cast<float>(overlayLayoutHeight) / activeOverlayTextureHeight;
                const float fitScale = (std::min)(fitScaleX, fitScaleY);
                overlayDisplayW = (std::max)(1, static_cast<int>(activeOverlayTextureWidth * fitScale));
                overlayDisplayH = (std::max)(1, static_cast<int>(activeOverlayTextureHeight * fitScale));
                break;
            }
            case EyeZoomOverlayDisplayMode::Stretch:
            default:
                overlayDisplayW = (std::max)(1, overlayLayoutWidth);
                overlayDisplayH = (std::max)(1, overlayLayoutHeight);
                break;
        }

        if (!isManualOverlay) {
            overlayDisplayW = (std::min)(overlayDisplayW, fullW);
            overlayDisplayH = (std::min)(overlayDisplayH, fullH);
        }

        const int overlayX = overlayLayoutX + (overlayLayoutWidth - overlayDisplayW) / 2;
        const int overlayY = overlayLayoutY + (overlayLayoutHeight - overlayDisplayH) / 2;
        const int overlayY_gl = fullH - overlayY - overlayDisplayH;

        const float nx1 = (static_cast<float>(overlayX) / fullW) * 2.0f - 1.0f;
        const float ny1 = (static_cast<float>(overlayY_gl) / fullH) * 2.0f - 1.0f;
        const float nx2 = (static_cast<float>(overlayX + overlayDisplayW) / fullW) * 2.0f - 1.0f;
        const float ny2 = (static_cast<float>(overlayY_gl + overlayDisplayH) / fullH) * 2.0f - 1.0f;
        const float effectiveOverlayOpacity = (std::max)(0.0f, (std::min)(1.0f, activeOverlay->opacity * overlayOpacityScale));

        if (!clipManualOverlayToZoomArea) {
            if (prevScissorEnabled) {
                glScissor(prevScissorBox[0], prevScissorBox[1], prevScissorBox[2], prevScissorBox[3]);
            } else {
                glDisable(GL_SCISSOR_TEST);
            }
        }

        glUseProgram(g_imageRenderProgram);
        BindTextureDirect(GL_TEXTURE_2D, activeOverlayTextureId);
        glUniform1i(g_imageRenderShaderLocs.enableColorKey, 0);
        glUniform1f(g_imageRenderShaderLocs.opacity, effectiveOverlayOpacity);

        float overlayVerts[] = {
            nx1, ny1, activeOverlaySourceRect[0], activeOverlaySourceRect[1],
            nx2, ny1, activeOverlaySourceRect[0] + activeOverlaySourceRect[2], activeOverlaySourceRect[1],
            nx2, ny2, activeOverlaySourceRect[0] + activeOverlaySourceRect[2], activeOverlaySourceRect[1] + activeOverlaySourceRect[3],
            nx1, ny1, activeOverlaySourceRect[0], activeOverlaySourceRect[1],
            nx2, ny2, activeOverlaySourceRect[0] + activeOverlaySourceRect[2], activeOverlaySourceRect[1] + activeOverlaySourceRect[3],
            nx1, ny2, activeOverlaySourceRect[0], activeOverlaySourceRect[1] + activeOverlaySourceRect[3],
        };
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(overlayVerts), overlayVerts);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        if (!clipManualOverlayToZoomArea) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(dstLeft, dstBottom, zoomOutputWidth, zoomOutputHeight);
        }

        glUseProgram(g_solidColorProgram);
    }

    float centerX = overlayLayoutX + overlayLayoutWidth / 2.0f;
    float centerLineWidth = 2.0f;
    float lineLeft = centerX - centerLineWidth / 2.0f;
    float lineRight = centerX + centerLineWidth / 2.0f;
    float lineBottom = (float)dstBottom;
    float lineTop = (float)dstTop;

    float lineNdcLeft = (lineLeft / (float)fullW) * 2.0f - 1.0f;
    float lineNdcRight = (lineRight / (float)fullW) * 2.0f - 1.0f;
    float lineNdcBottom = (lineBottom / (float)fullH) * 2.0f - 1.0f;
    float lineNdcTop = (lineTop / (float)fullH) * 2.0f - 1.0f;

    glUniform4f(g_solidColorShaderLocs.color, zoomConfig.centerLineColor.r, zoomConfig.centerLineColor.g,
                zoomConfig.centerLineColor.b, zoomConfig.centerLineColorOpacity * overlayOpacityScale);

    float centerLineVerts[] = {
        lineNdcLeft, lineNdcBottom, 0, 0, lineNdcRight, lineNdcBottom, 0, 0, lineNdcRight, lineNdcTop, 0, 0,
        lineNdcLeft, lineNdcBottom, 0, 0, lineNdcRight, lineNdcTop,    0, 0, lineNdcLeft,  lineNdcTop, 0, 0,
    };
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(centerLineVerts), centerLineVerts);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    if (prevScissorEnabled) {
        glScissor(prevScissorBox[0], prevScissorBox[1], prevScissorBox[2], prevScissorBox[3]);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }

    glDisable(GL_BLEND);
    glBindFramebuffer(GL_FRAMEBUFFER, s.fb);
    if (oglViewport)
        oglViewport(0, 0, fullW, fullH);
    else
        glViewport(0, 0, fullW, fullH);
}

void RenderModeInternal(const ModeConfig* modeToRender, const GLState& s, int current_gameW, int current_gameH, bool skipAnimation,
                        bool excludeOnlyOnMyScreen);

void RenderMode(const ModeConfig* modeToRender, const GLState& s, int current_gameW, int current_gameH, bool skipAnimation,
                bool excludeOnlyOnMyScreen) {
    RenderModeInternal(modeToRender, s, current_gameW, current_gameH, skipAnimation, excludeOnlyOnMyScreen);
}

void RenderModeInternal(const ModeConfig* modeToRender, const GLState& s, int current_gameW, int current_gameH, bool skipAnimation,
                        bool excludeOnlyOnMyScreen) {
    PROFILE_SCOPE_CAT("RenderModeInternal", "Rendering");

    int fullW, fullH;
    {
        PROFILE_SCOPE_CAT("GetSystemMetrics", "Rendering");
        fullW = GetCachedWindowWidth();
        fullH = GetCachedWindowHeight();
    }
    if (fullW <= 0 || fullH <= 0) { return; }

    // Single config snapshot for the entire frame - avoids repeated mutex acquisition
    auto configSnap = GetConfigSnapshot();

    // Get all animated state atomically to avoid race conditions
    ModeTransitionState transitionState;
    {
        PROFILE_SCOPE_CAT("GetModeTransitionState", "Rendering");
        transitionState = GetModeTransitionState();
    }
    bool transitionEffectivelyComplete = transitionState.active && transitionState.width == transitionState.targetWidth &&
                                         transitionState.height == transitionState.targetHeight &&
                                         transitionState.x == transitionState.targetX && transitionState.y == transitionState.targetY;
    bool isAnimating = transitionState.active && !skipAnimation && !transitionEffectivelyComplete;

    int modeWidth = modeToRender->width;
    int modeHeight = modeToRender->height;
    int modeX = 0;
    int modeY = 0;

    if (isAnimating) {
        modeWidth = transitionState.width;
        modeHeight = transitionState.height;
        modeX = transitionState.x;
        modeY = transitionState.y;
    }

    {
        PROFILE_SCOPE_CAT("GL State Setup", "Rendering");
        glDisable(GL_FRAMEBUFFER_SRGB);
        glDisable(GL_BLEND);
    }

    // Active elements are collected earlier; here we only check whether mirror resources are needed.
    bool hasMirrors = !modeToRender->mirrorIds.empty() || !modeToRender->mirrorGroupIds.empty();

    {
        PROFILE_SCOPE_CAT("Framebuffer/Viewport Setup", "Rendering");
        glBindFramebuffer(GL_FRAMEBUFFER, s.fb);
        if (oglViewport)
            oglViewport(0, 0, fullW, fullH);
        else
            glViewport(0, 0, fullW, fullH);
    }

    GLuint gameTextureToUse = g_cachedGameTextureId.load();

    GameViewportGeometry currentGeo;
    const bool useOptimizedPath =
        !isAnimating && (modeWidth == fullW && modeHeight == fullH &&
                         (!modeToRender->stretch.enabled || modeToRender->stretch.width == fullW && modeToRender->stretch.height == fullH &&
                                                                modeToRender->stretch.x == 0 && modeToRender->stretch.y == 0));

    if (useOptimizedPath) {
        PROFILE_SCOPE_CAT("Optimized Path", "Rendering");
        currentGeo = { current_gameW, current_gameH, 0, 0, fullW, fullH };

        // Animations are composed in the overlay pass and never appear on the backbuffer.
    } else {
        PROFILE_SCOPE_CAT("Non-Optimized Path", "Rendering");
        int finalX, finalY, finalW, finalH;
        if (isAnimating) {
            finalX = modeX;
            finalY = modeY;
            finalW = modeWidth;
            finalH = modeHeight;
        } else if (modeToRender->stretch.enabled) {
            finalX = modeToRender->stretch.x;
            finalY = modeToRender->stretch.y;
            finalW = modeToRender->stretch.width;
            finalH = modeToRender->stretch.height;
        } else {
            finalW = modeWidth;
            finalH = modeHeight;
            finalX = GetCenteredAxisOffset(fullW, finalW);
            finalY = GetCenteredAxisOffset(fullH, finalH);
        }
        currentGeo = { current_gameW, current_gameH, finalX, finalY, finalW, finalH };
        int finalY_gl = fullH - finalY - finalH;

        int letterboxExtendX = 0;
        int letterboxExtendY = 0;
        /*if (isAnimating && transitionState.gameTransition == GameTransitionType::Bounce) {
            if (transitionState.fromWidth != transitionState.targetWidth) { letterboxExtendX = 1; }
            if (transitionState.fromHeight != transitionState.targetHeight) { letterboxExtendY = 1; }
        }*/

        glEnable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);

        // Use fromModeId from transitionState (atomically read from snapshot) to avoid race conditions
        std::string fromModeId = transitionState.fromModeId;
        bool transitioningToFullscreen = isAnimating && EqualsIgnoreCase(modeToRender->id, "Fullscreen");
        bool transitioningFromFullscreen = isAnimating && !fromModeId.empty() && EqualsIgnoreCase(fromModeId, "Fullscreen");

        BackgroundConfig fromBackground;
        BorderConfig fromBorder;
        GLuint fromBgTex = 0;
        float fromBgSourceRect[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
        bool useFromBackground = false;

        if (isAnimating && !fromModeId.empty()) {
            const ModeConfig* fromMode = GetMode_Internal(fromModeId);
            if (fromMode) {
                fromBackground = fromMode->background;
                fromBorder = fromMode->border;

                bool fromHasSpecialBackground = (fromBackground.selectedMode == "gradient" || fromBackground.selectedMode == "image");
                useFromBackground = transitioningToFullscreen || fromHasSpecialBackground;
            }

            if (useFromBackground) {
                std::lock_guard<std::mutex> bgLock(g_backgroundTexturesMutex);
                auto fromBgTexIt = g_backgroundTextures.find(fromModeId);
                if (fromBgTexIt != g_backgroundTextures.end()) {
                    BackgroundTextureInstance& bgInst = fromBgTexIt->second;
                    const auto animatedTexture = ResolveAnimatedTexture(bgInst);
                    fromBgTex = animatedTexture.textureId;
                    memcpy(fromBgSourceRect, animatedTexture.sourceRect, sizeof(fromBgSourceRect));
                }
            }
        }

        GLuint bgTex = 0;
        float bgSourceRect[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
        {
            PROFILE_SCOPE_CAT("Background Texture Lookup", "Rendering");
            std::lock_guard<std::mutex> bgLock(g_backgroundTexturesMutex);
            auto bgTexIt = g_backgroundTextures.find(modeToRender->id);
            if (bgTexIt != g_backgroundTextures.end()) {
                BackgroundTextureInstance& bgInst = bgTexIt->second;
                const auto animatedTexture = ResolveAnimatedTexture(bgInst);
                bgTex = animatedTexture.textureId;
                memcpy(bgSourceRect, animatedTexture.sourceRect, sizeof(bgSourceRect));
            }
        }

        auto drawTexturedRegion = [&](int rx, int ry_gl, int rw, int rh, const float sourceRect[4]) {
            if (rw <= 0 || rh <= 0) return;
            glScissor(rx, ry_gl, rw, rh);

            float baseU1 = static_cast<float>(rx) / fullW;
            float baseU2 = static_cast<float>(rx + rw) / fullW;
            float baseV1 = static_cast<float>(ry_gl) / fullH;
            float baseV2 = static_cast<float>(ry_gl + rh) / fullH;

            float u1 = sourceRect[0] + baseU1 * sourceRect[2];
            float u2 = sourceRect[0] + baseU2 * sourceRect[2];
            float v1 = sourceRect[1] + baseV1 * sourceRect[3];
            float v2 = sourceRect[1] + baseV2 * sourceRect[3];

            float nx1 = baseU1 * 2.0f - 1.0f;
            float nx2 = baseU2 * 2.0f - 1.0f;
            float ny1 = baseV1 * 2.0f - 1.0f;
            float ny2 = baseV2 * 2.0f - 1.0f;

            float quad[] = { nx1, ny1, u1, v1, nx2, ny1, u2, v1, nx2, ny2, u2, v2, nx1, ny1, u1, v1, nx2, ny2, u2, v2, nx1, ny2, u1, v2 };
            glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(quad), quad);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        };

        auto drawColorRegion = [&](int rx, int ry_gl, int rw, int rh) {
            if (rw <= 0 || rh <= 0) return;
            glScissor(rx, ry_gl, rw, rh);

            float nx1 = (static_cast<float>(rx) / fullW) * 2.0f - 1.0f;
            float nx2 = (static_cast<float>(rx + rw) / fullW) * 2.0f - 1.0f;
            float ny1 = (static_cast<float>(ry_gl) / fullH) * 2.0f - 1.0f;
            float ny2 = (static_cast<float>(ry_gl + rh) / fullH) * 2.0f - 1.0f;

            float quad[] = { nx1, ny1, 0, 0, nx2, ny1, 0, 0, nx2, ny2, 0, 0, nx1, ny1, 0, 0, nx2, ny2, 0, 0, nx1, ny2, 0, 0 };
            glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(quad), quad);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        };

        auto drawGradientRegion = [&](int rx, int ry_gl, int rw, int rh) {
            if (rw <= 0 || rh <= 0) return;
            glScissor(rx, ry_gl, rw, rh);

            float u1 = static_cast<float>(rx) / fullW;
            float u2 = static_cast<float>(rx + rw) / fullW;
            float v1 = static_cast<float>(ry_gl) / fullH;
            float v2 = static_cast<float>(ry_gl + rh) / fullH;

            float nx1 = u1 * 2.0f - 1.0f;
            float nx2 = u2 * 2.0f - 1.0f;
            float ny1 = v1 * 2.0f - 1.0f;
            float ny2 = v2 * 2.0f - 1.0f;

            float quad[] = { nx1, ny1, u1, v1, nx2, ny1, u2, v1, nx2, ny2, u2, v2, nx1, ny1, u1, v1, nx2, ny2, u2, v2, nx1, ny2, u1, v2 };
            glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(quad), quad);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        };

        auto renderBackgroundImage = [&](GLuint texId, const float sourceRect[4], float opacity) {
            if (texId == 0) return;

            PROFILE_SCOPE_CAT("Scissor Background Image", "Rendering");

            GLint savedActiveTexture = 0;
            GLint savedTexture = 0;
            GLint savedSampler = 0;
            GLboolean savedRasterizerDiscard = GL_FALSE;
            GLboolean savedColorLogicOp = GL_FALSE;
            GLint savedBlendEqRgb = GL_FUNC_ADD;
            GLint savedBlendEqAlpha = GL_FUNC_ADD;
            GLfloat savedBlendColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

            glGetIntegerv(GL_ACTIVE_TEXTURE, &savedActiveTexture);
            glActiveTexture(GL_TEXTURE0);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTexture);
            if (SupportsSamplerObjects()) {
                glGetIntegerv(GL_SAMPLER_BINDING, &savedSampler);
                glBindSampler(0, 0);
            }
            savedRasterizerDiscard = glIsEnabled(GL_RASTERIZER_DISCARD);
            savedColorLogicOp = glIsEnabled(GL_COLOR_LOGIC_OP);
            glGetIntegerv(GL_BLEND_EQUATION_RGB, &savedBlendEqRgb);
            glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &savedBlendEqAlpha);
            glGetFloatv(GL_BLEND_COLOR, savedBlendColor);

            glEnable(GL_SCISSOR_TEST);
            glDisable(GL_RASTERIZER_DISCARD);
            glDisable(GL_COLOR_LOGIC_OP);
            glUseProgram(g_backgroundProgram);
            BindTextureDirect(GL_TEXTURE_2D, texId);
            glUniform1i(g_backgroundShaderLocs.backgroundTexture, 0);
            glUniform1f(g_backgroundShaderLocs.opacity, opacity);
            glBindVertexArray(g_vao);
            glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
            glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
            glBlendColor(0.0f, 0.0f, 0.0f, 0.0f);

            if (opacity < 1.0f) {
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            } else {
                glDisable(GL_BLEND);
            }

            int vpLeft = finalX + letterboxExtendX;
            int vpRight = finalX + finalW - letterboxExtendX;
            int vpBottom_gl = finalY_gl + letterboxExtendY;
            int vpTop_gl = finalY_gl + finalH - letterboxExtendY;

            drawTexturedRegion(0, 0, fullW, vpBottom_gl, sourceRect);
            drawTexturedRegion(0, vpTop_gl, fullW, fullH - vpTop_gl, sourceRect);
            drawTexturedRegion(0, vpBottom_gl, vpLeft, vpTop_gl - vpBottom_gl, sourceRect);
            drawTexturedRegion(vpRight, vpBottom_gl, fullW - vpRight, vpTop_gl - vpBottom_gl, sourceRect);

            glDisable(GL_SCISSOR_TEST);

            BindTextureDirect(GL_TEXTURE_2D, savedTexture);
            if (SupportsSamplerObjects()) { glBindSampler(0, static_cast<GLuint>(savedSampler)); }
            if (savedRasterizerDiscard) {
                glEnable(GL_RASTERIZER_DISCARD);
            } else {
                glDisable(GL_RASTERIZER_DISCARD);
            }
            if (savedColorLogicOp) {
                glEnable(GL_COLOR_LOGIC_OP);
            } else {
                glDisable(GL_COLOR_LOGIC_OP);
            }
            glBlendEquationSeparate(savedBlendEqRgb, savedBlendEqAlpha);
            glBlendColor(savedBlendColor[0], savedBlendColor[1], savedBlendColor[2], savedBlendColor[3]);
            glActiveTexture(savedActiveTexture);
        };

        auto renderBackgroundColor = [&](const Color& color, float opacity) {
            PROFILE_SCOPE_CAT("Scissor Background Color", "Rendering");

            glEnable(GL_SCISSOR_TEST);
            glUseProgram(g_solidColorProgram);
            glUniform4f(g_solidColorShaderLocs.color, color.r, color.g, color.b, opacity);
            glBindVertexArray(g_vao);
            glBindBuffer(GL_ARRAY_BUFFER, g_vbo);

            if (opacity < 1.0f) {
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            } else {
                glDisable(GL_BLEND);
            }

            int vpLeft = finalX + letterboxExtendX;
            int vpRight = finalX + finalW - letterboxExtendX;
            int vpBottom_gl = finalY_gl + letterboxExtendY;
            int vpTop_gl = finalY_gl + finalH - letterboxExtendY;

            drawColorRegion(0, 0, fullW, vpBottom_gl);
            drawColorRegion(0, vpTop_gl, fullW, fullH - vpTop_gl);
            drawColorRegion(0, vpBottom_gl, vpLeft, vpTop_gl - vpBottom_gl);
            drawColorRegion(vpRight, vpBottom_gl, fullW - vpRight, vpTop_gl - vpBottom_gl);

            glDisable(GL_SCISSOR_TEST);
        };

        auto renderBackgroundGradient = [&](const BackgroundConfig& bg, float opacity) {
            if (bg.gradientStops.size() < 2) return;

            PROFILE_SCOPE_CAT("Scissor Background Gradient", "Rendering");

            glEnable(GL_SCISSOR_TEST);
            glUseProgram(g_gradientProgram);
            glBindVertexArray(g_vao);
            glBindBuffer(GL_ARRAY_BUFFER, g_vbo);

            int numStops = (std::min)(static_cast<int>(bg.gradientStops.size()), MAX_GRADIENT_STOPS);
            glUniform1i(g_gradientShaderLocs.numStops, numStops);

            float colors[MAX_GRADIENT_STOPS * 4];
            float positions[MAX_GRADIENT_STOPS];
            for (int i = 0; i < numStops; i++) {
                colors[i * 4 + 0] = bg.gradientStops[i].color.r;
                colors[i * 4 + 1] = bg.gradientStops[i].color.g;
                colors[i * 4 + 2] = bg.gradientStops[i].color.b;
                colors[i * 4 + 3] = opacity;
                positions[i] = bg.gradientStops[i].position;
            }
            glUniform4fv(g_gradientShaderLocs.stopColors, numStops, colors);
            glUniform1fv(g_gradientShaderLocs.stopPositions, numStops, positions);
            glUniform1f(g_gradientShaderLocs.angle, bg.gradientAngle * 3.14159265f / 180.0f);

            static auto startTime = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            float timeSeconds = std::chrono::duration<float>(now - startTime).count();
            glUniform1f(g_gradientShaderLocs.time, timeSeconds);
            glUniform1i(g_gradientShaderLocs.animationType, static_cast<int>(bg.gradientAnimation));
            glUniform1f(g_gradientShaderLocs.animationSpeed, bg.gradientAnimationSpeed);
            glUniform1i(g_gradientShaderLocs.colorFade, bg.gradientColorFade ? 1 : 0);

            if (opacity < 1.0f) {
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            } else {
                glDisable(GL_BLEND);
            }

            int vpLeft = finalX + letterboxExtendX;
            int vpRight = finalX + finalW - letterboxExtendX;
            int vpBottom_gl = finalY_gl + letterboxExtendY;
            int vpTop_gl = finalY_gl + finalH - letterboxExtendY;

            drawGradientRegion(0, 0, fullW, vpBottom_gl);
            drawGradientRegion(0, vpTop_gl, fullW, fullH - vpTop_gl);
            drawGradientRegion(0, vpBottom_gl, vpLeft, vpTop_gl - vpBottom_gl);
            drawGradientRegion(vpRight, vpBottom_gl, fullW - vpRight, vpTop_gl - vpBottom_gl);

            glDisable(GL_SCISSOR_TEST);
        };

        if (useFromBackground) {
            PROFILE_SCOPE_CAT("Render From Background", "Rendering");
            if (fromBackground.selectedMode == "image" && fromBgTex != 0) {
                renderBackgroundImage(fromBgTex, fromBgSourceRect, 1.0f);
            } else if (fromBackground.selectedMode == "gradient" && fromBackground.gradientStops.size() >= 2) {
                renderBackgroundGradient(fromBackground, 1.0f);
            } else {
                renderBackgroundColor(fromBackground.color, 1.0f);
            }
        }

        if (!useFromBackground) {
            PROFILE_SCOPE_CAT("Render To Background", "Rendering");
            if (modeToRender->background.selectedMode == "image" && bgTex != 0) {
                renderBackgroundImage(bgTex, bgSourceRect, 1.0f);
            } else if (modeToRender->background.selectedMode == "gradient" && modeToRender->background.gradientStops.size() >= 2) {
                renderBackgroundGradient(modeToRender->background, 1.0f);
            } else {
                renderBackgroundColor(modeToRender->background.color, 1.0f);
            }
        }

        glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_sceneFBO);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s.fb);

        {
            PROFILE_SCOPE_CAT("Render Game Border", "Rendering");
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            if (transitioningToFullscreen && fromBorder.enabled && fromBorder.width > 0) {
                RenderGameBorder(finalX, finalY, finalW, finalH, fromBorder.width, fromBorder.radius, fromBorder.color, fullW, fullH);
            } else if (modeToRender->border.enabled && modeToRender->border.width > 0) {
                RenderGameBorder(finalX, finalY, finalW, finalH, modeToRender->border.width, modeToRender->border.radius,
                                 modeToRender->border.color, fullW, fullH);
            }
            glDisable(GL_BLEND);
        }
    }

    {
        PROFILE_SCOPE_CAT("Set Viewport Geometry", "Rendering");
        std::lock_guard<std::mutex> lock(g_geometryMutex);
        g_lastFrameGeometry = currentGeo;
    }

    // Update game state for mirror capture.
    if (hasMirrors) {
        PROFILE_SCOPE_CAT("Mirror Management", "Rendering");

        // Lazy auto-start OBS hook work once the graphics hook is available.
        if (g_graphicsHookDetected.load()) { StartObsHookThread(); }

        // NOTE: Mirror capture config updates are now handled by logic_thread (UpdateActiveMirrorConfigs)
        // This avoids doing the config collection work on every frame of the render path.
    }

    glBindFramebuffer(GL_FRAMEBUFFER, s.fb);
    if (oglViewport)
        oglViewport(0, 0, fullW, fullH);
    else
        glViewport(0, 0, fullW, fullH);

    if (g_showGui.load(std::memory_order_relaxed) || g_imageDragMode.load(std::memory_order_relaxed) ||
        g_windowOverlayDragMode.load(std::memory_order_relaxed) || g_browserOverlayDragMode.load(std::memory_order_relaxed)) {
        HWND hwnd = g_minecraftHwnd.load();
        if (hwnd) { InitializeImGuiContext(hwnd); }
    }

    if (g_imageDragMode.load() && g_imageOverlaysVisible.load(std::memory_order_acquire)) {
        PROFILE_SCOPE_CAT("Image Drag Mode", "Input Handling");
        ImGuiIO& io = ImGui::GetIO();
        if (io.WantCaptureMouse) {
            s_hoveredImageName = "";
        } else {
            HWND hwnd = g_minecraftHwnd.load();
            if (hwnd) {
                POINT mousePos;
                GetCursorPos(&mousePos);
                ScreenToClient(hwnd, &mousePos);

                bool leftButtonDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;

                std::string hoveredImage = "";
                {
                    const auto& dragImages = configSnap ? configSnap->images : std::vector<ImageConfig>{};

                    // Drag mode is rare, but this is on the game thread, so keep it cheap.
                    std::unordered_map<std::string, const ImageConfig*> imageByName;
                    if (configSnap) {
                        imageByName.reserve(dragImages.size());
                        for (const auto& img : dragImages) {
                            imageByName.emplace(img.name, &img);
                        }
                    }
                    for (const auto& imageName : modeToRender->imageIds) {
                        const ImageConfig* confPtr = nullptr;
                        if (!imageByName.empty()) {
                            auto it = imageByName.find(imageName);
                            if (it != imageByName.end()) { confPtr = it->second; }
                        } else {
                            for (const auto& img : dragImages) {
                                if (img.name == imageName) {
                                    confPtr = &img;
                                    break;
                                }
                            }
                        }
                        if (!confPtr) continue;
                        const ImageConfig& conf = *confPtr;

                        // Try-lock to avoid stalling the game thread if textures are being updated.
                        int texWidth = 0;
                        int texHeight = 0;
                        {
                            std::unique_lock<std::mutex> imageLock(g_userImagesMutex, std::try_to_lock);
                            if (!imageLock.owns_lock()) { continue; }
                            auto it_inst = g_userImages.find(conf.name);
                            if (it_inst == g_userImages.end() || it_inst->second.textureId == 0) continue;
                            texWidth = it_inst->second.width;
                            texHeight = it_inst->second.height;
                        }

                        // Calculate actual dimensions from scale (avoid calling CalculateImageDimensions to prevent nested locking)
                        int displayW = 0;
                        int displayH = 0;
                        ResolveConfiguredImageDimensions(conf, texWidth, texHeight, displayW, displayH);

                        const bool isViewportRelative =
                            conf.relativeTo.length() > 8 && conf.relativeTo.substr(conf.relativeTo.length() - 8) == "Viewport";
                        if (isViewportRelative) {
                            const float viewportScaleX =
                                (currentGeo.finalW > 0 && currentGeo.gameW > 0) ? static_cast<float>(currentGeo.finalW) / currentGeo.gameW : 1.0f;
                            const float viewportScaleY =
                                (currentGeo.finalH > 0 && currentGeo.gameH > 0) ? static_cast<float>(currentGeo.finalH) / currentGeo.gameH : 1.0f;
                            ScaleViewportRelativeImageSize(displayW, displayH, modeToRender->relativeStretching, viewportScaleX,
                                                           viewportScaleY, displayW, displayH);
                        }

                        int finalScreenX_win, finalScreenY_win;
                        GetRelativeCoordsForImageWithViewport(conf.relativeTo, conf.x, conf.y, displayW, displayH, currentGeo.finalX,
                                                              currentGeo.finalY, currentGeo.finalW, currentGeo.finalH, fullW, fullH,
                                                              finalScreenX_win, finalScreenY_win);

                        if (mousePos.x >= finalScreenX_win && mousePos.x < finalScreenX_win + displayW && mousePos.y >= finalScreenY_win &&
                            mousePos.y < finalScreenY_win + displayH) {
                            hoveredImage = conf.name;
                            break;
                        }
                    }

                    if (leftButtonDown && !s_isDragging && !hoveredImage.empty()) {
                        s_isDragging = true;
                        s_draggedImageName = hoveredImage;
                        s_dragStartPos = mousePos;
                        s_lastMousePos = mousePos;
                    }
                }

                if (leftButtonDown && s_isDragging && !s_draggedImageName.empty()) {
                    int deltaX = mousePos.x - s_lastMousePos.x;
                    int deltaY = mousePos.y - s_lastMousePos.y;

                    if (deltaX != 0 || deltaY != 0) {
                        // NOTE: This mutates g_config from the game thread. Safe because:
                        // 1. Only this thread writes drag x/y (no concurrent x/y writers)
                        // 2. GUI thread won't resize images vector during drag mode (drag mode disables GUI interaction)
                        for (auto& img : g_config.images) {
                            if (img.name == s_draggedImageName) {
                                img.x += deltaX;
                                img.y += deltaY;
                                g_configIsDirty = true;
                                break;
                            }
                        }
                        s_lastMousePos = mousePos;
                    }
                }
                else if (!leftButtonDown && s_isDragging) {
                    s_isDragging = false;
                    s_draggedImageName = "";
                }

                s_hoveredImageName = hoveredImage;
            }
        }
    } else {
        if (s_isDragging) {
            s_isDragging = false;
            s_draggedImageName = "";
            s_hoveredImageName = "";
        }
    }

    if (g_showGui.load(std::memory_order_relaxed) && g_windowOverlayDragMode.load(std::memory_order_relaxed) &&
        g_windowOverlaysVisible.load(std::memory_order_acquire)) {
        PROFILE_SCOPE_CAT("Window Overlay Drag Mode", "Input Handling");

        ImGuiIO& io = ImGui::GetIO();
        if (io.WantCaptureMouse) {
            s_hoveredWindowOverlayName = "";
        } else {
            HWND hwnd = g_minecraftHwnd.load();
            if (hwnd) {
                POINT mousePos;
                GetCursorPos(&mousePos);
                ScreenToClient(hwnd, &mousePos);

                if (mousePos.x >= s.vp[0] && mousePos.x < (s.vp[0] + s.vp[2]) && mousePos.y >= s.vp[1] &&
                    mousePos.y < (s.vp[1] + s.vp[3])) {

                    bool leftButtonDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;

                    // Keep previous hover state if we can't get the mutexes this frame
                    std::string hoveredOverlay = s_hoveredWindowOverlayName;

                    if (!s_isWindowOverlayDragging) {
                        PROFILE_SCOPE_CAT("Overlay Hover Detection", "Input Handling");

                        // Try to acquire cache mutex - skip hover detection if busy
                        std::unique_lock<std::mutex> cacheLock(g_windowOverlayCacheMutex, std::try_to_lock);

                        if (cacheLock.owns_lock()) {
                            hoveredOverlay = "";
                            std::vector<const WindowOverlayConfig*> activeOverlays;

                            std::unordered_map<std::string, const WindowOverlayConfig*> overlayByName;
                            if (configSnap) {
                                overlayByName.reserve(configSnap->windowOverlays.size());
                                for (const auto& ov : configSnap->windowOverlays) {
                                    overlayByName.emplace(ov.name, &ov);
                                }
                            }
                            for (const auto& overlayId : modeToRender->windowOverlayIds) {
                                const WindowOverlayConfig* config = nullptr;
                                if (!overlayByName.empty()) {
                                    auto it = overlayByName.find(overlayId);
                                    if (it != overlayByName.end()) { config = it->second; }
                                } else {
                                    config = configSnap ? FindWindowOverlayConfigIn(overlayId, *configSnap) : nullptr;
                                }
                                if (config) { activeOverlays.push_back(config); }
                            }

                            for (const WindowOverlayConfig* confPtr : activeOverlays) {
                                if (!confPtr) continue;
                                const WindowOverlayConfig& conf = *confPtr;
                                // Use Unsafe version since we already hold the cache mutex
                                int displayW, displayH;
                                CalculateWindowOverlayDimensionsUnsafe(conf, displayW, displayH);
                                int finalScreenX_win, finalScreenY_win;
                                GetRelativeCoordsForImageWithViewport(conf.relativeTo, conf.x, conf.y, displayW, displayH,
                                                                      currentGeo.finalX, currentGeo.finalY, currentGeo.finalW,
                                                                      currentGeo.finalH, fullW, fullH, finalScreenX_win, finalScreenY_win);

                                if (mousePos.x >= finalScreenX_win && mousePos.x < finalScreenX_win + displayW &&
                                    mousePos.y >= finalScreenY_win && mousePos.y < finalScreenY_win + displayH) {
                                    hoveredOverlay = conf.name;
                                    break;
                                }
                            }
                        }
                        // If we couldn't get both locks, hover detection is skipped this frame
                    }

                    if (leftButtonDown && !s_isWindowOverlayDragging && !hoveredOverlay.empty()) {
                        s_isWindowOverlayDragging = true;
                        s_draggedWindowOverlayName = hoveredOverlay;
                        s_lastMousePos = mousePos;

                        // Read initial position from snapshot for thread safety
                        if (configSnap) {
                            for (const auto& overlay : configSnap->windowOverlays) {
                                if (overlay.name == s_draggedWindowOverlayName) {
                                    s_initialX = overlay.x;
                                    s_initialY = overlay.y;
                                    break;
                                }
                            }
                        }
                    }
                    else if (leftButtonDown && s_isWindowOverlayDragging && !s_draggedWindowOverlayName.empty()) {
                        PROFILE_SCOPE_CAT("Overlay Drag Update", "Input Handling");

                        int deltaX = mousePos.x - s_lastMousePos.x;
                        int deltaY = mousePos.y - s_lastMousePos.y;

                        if (deltaX != 0 || deltaY != 0) {
                            // Mutate g_config from game thread - safe because:
                            // 1. Only this thread writes drag x/y (no concurrent x/y writers)
                            for (auto& overlay : g_config.windowOverlays) {
                                if (overlay.name == s_draggedWindowOverlayName) {
                                    overlay.x += deltaX;
                                    overlay.y += deltaY;
                                    g_configIsDirty = true;
                                    break;
                                }
                            }

                            s_lastMousePos = mousePos;
                        }
                    }
                    else if (!leftButtonDown && s_isWindowOverlayDragging) {
                        s_isWindowOverlayDragging = false;
                        s_draggedWindowOverlayName = "";
                        SaveConfigImmediate();
                    }

                    s_hoveredWindowOverlayName = hoveredOverlay;
                }
            }
        }
    } else {
        if (s_isWindowOverlayDragging) {
            s_isWindowOverlayDragging = false;
            s_draggedWindowOverlayName = "";
            s_hoveredWindowOverlayName = "";
        }
    }

    if (g_showGui.load(std::memory_order_relaxed) && g_browserOverlayDragMode.load(std::memory_order_relaxed) &&
        g_browserOverlaysVisible.load(std::memory_order_acquire)) {
        PROFILE_SCOPE_CAT("Browser Overlay Drag Mode", "Input Handling");

        ImGuiIO& io = ImGui::GetIO();
        if (io.WantCaptureMouse) {
            s_hoveredBrowserOverlayName = "";
        } else {
            HWND hwnd = g_minecraftHwnd.load();
            if (hwnd) {
                POINT mousePos;
                GetCursorPos(&mousePos);
                ScreenToClient(hwnd, &mousePos);

                bool leftButtonDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
                std::string hoveredOverlay = "";

                if (configSnap) {
                    std::unordered_map<std::string, const BrowserOverlayConfig*> overlayByName;
                    overlayByName.reserve(configSnap->browserOverlays.size());
                    for (const auto& overlay : configSnap->browserOverlays) {
                        overlayByName.emplace(overlay.name, &overlay);
                    }

                    for (const auto& overlayId : modeToRender->browserOverlayIds) {
                        auto overlayIt = overlayByName.find(overlayId);
                        if (overlayIt == overlayByName.end() || !overlayIt->second) {
                            continue;
                        }

                        const BrowserOverlayConfig& conf = *overlayIt->second;
                        BrowserOverlayTextureFrame frame{};
                        if (!PrepareBrowserOverlayTexture(conf, frame)) {
                            continue;
                        }

                        auto cc = ResolveCrop(conf.crop_top, conf.crop_bottom, conf.crop_left, conf.crop_right,
                                              conf.cropToWidth, conf.cropToHeight, frame.textureWidth, frame.textureHeight);
                        int croppedW = frame.textureWidth - cc.left - cc.right;
                        int croppedH = frame.textureHeight - cc.top - cc.bottom;
                        croppedW = (std::max)(1, croppedW);
                        croppedH = (std::max)(1, croppedH);
                        int displayW = (std::max)(1, static_cast<int>(croppedW * conf.scale));
                        int displayH = (std::max)(1, static_cast<int>(croppedH * conf.scale));

                        const bool isViewportRelative =
                            conf.relativeTo.length() > 8 && conf.relativeTo.substr(conf.relativeTo.length() - 8) == "Viewport";
                        if (isViewportRelative) {
                            const float viewportScaleX =
                                (currentGeo.finalW > 0 && currentGeo.gameW > 0) ? static_cast<float>(currentGeo.finalW) / currentGeo.gameW : 1.0f;
                            const float viewportScaleY =
                                (currentGeo.finalH > 0 && currentGeo.gameH > 0) ? static_cast<float>(currentGeo.finalH) / currentGeo.gameH : 1.0f;
                            ScaleViewportRelativeImageSize(displayW, displayH, modeToRender->relativeStretching, viewportScaleX,
                                                           viewportScaleY, displayW, displayH);
                        }

                        int finalScreenX = 0;
                        int finalScreenY = 0;
                        GetRelativeCoordsForImageWithViewport(conf.relativeTo, conf.x, conf.y, displayW, displayH, currentGeo.finalX,
                                                              currentGeo.finalY, currentGeo.finalW, currentGeo.finalH, fullW, fullH,
                                                              finalScreenX, finalScreenY);

                        if (mousePos.x >= finalScreenX && mousePos.x < finalScreenX + displayW && mousePos.y >= finalScreenY &&
                            mousePos.y < finalScreenY + displayH) {
                            hoveredOverlay = conf.name;
                            break;
                        }
                    }
                }

                if (leftButtonDown && !s_isBrowserOverlayDragging && !hoveredOverlay.empty()) {
                    s_isBrowserOverlayDragging = true;
                    s_draggedBrowserOverlayName = hoveredOverlay;
                    s_lastMousePos = mousePos;
                }
                else if (leftButtonDown && s_isBrowserOverlayDragging && !s_draggedBrowserOverlayName.empty()) {
                    int deltaX = mousePos.x - s_lastMousePos.x;
                    int deltaY = mousePos.y - s_lastMousePos.y;

                    if (deltaX != 0 || deltaY != 0) {
                        for (auto& overlay : g_config.browserOverlays) {
                            if (overlay.name == s_draggedBrowserOverlayName) {
                                overlay.x += deltaX;
                                overlay.y += deltaY;
                                g_configIsDirty = true;
                                break;
                            }
                        }

                        s_lastMousePos = mousePos;
                    }
                }
                else if (!leftButtonDown && s_isBrowserOverlayDragging) {
                    s_isBrowserOverlayDragging = false;
                    s_draggedBrowserOverlayName = "";
                    SaveConfigImmediate();
                }

                s_hoveredBrowserOverlayName = hoveredOverlay;
            }
        }
    } else {
        if (s_isBrowserOverlayDragging) {
            s_isBrowserOverlayDragging = false;
            s_draggedBrowserOverlayName = "";
            s_hoveredBrowserOverlayName = "";
        }
    }

    float overlayOpacity = 1.0f;

    const bool wantOverlayElements = true;
    const bool wantAnyImGui = g_shouldRenderGui.load(std::memory_order_relaxed) || g_showPerformanceOverlay.load(std::memory_order_relaxed) ||
                              g_showProfiler.load(std::memory_order_relaxed) || g_showEyeZoom.load(std::memory_order_relaxed) ||
                              g_showTextureGrid.load(std::memory_order_relaxed);
    const bool wantRebindIndicator = IsRebindIndicatorVisible();
    const bool isFullscreenMode = EqualsIgnoreCase(modeToRender->id, "Fullscreen");

    bool wantWelcomeToast = false;
    {
        static bool s_prevFullscreen = false;
        static std::chrono::steady_clock::time_point s_fullscreenEnterTime{};
        auto now = std::chrono::steady_clock::now();
        if (isFullscreenMode && !s_prevFullscreen) { s_fullscreenEnterTime = now; }
        s_prevFullscreen = isFullscreenMode;

        if (isFullscreenMode && !g_configurePromptDismissedThisSession.load(std::memory_order_relaxed)) {
            constexpr float kHoldSeconds = 10.0f;
            constexpr float kFadeSeconds = 1.5f;
            const float elapsed = std::chrono::duration_cast<std::chrono::duration<float>>(now - s_fullscreenEnterTime).count();
            wantWelcomeToast = (elapsed <= (kHoldSeconds + kFadeSeconds + 0.25f));
        }
    }

    const bool wantCursorTrail = configSnap && configSnap->cursorTrail.enabled && IsCursorVisible();
    const bool wantOverlayThisFrame = wantOverlayElements || wantAnyImGui || wantWelcomeToast || wantRebindIndicator ||
                                      wantCursorTrail;
    const auto populateOverlayState = [&](auto& target) {
        const bool slideAnimationsEnabled = transitionState.active && transitionState.gameTransition == GameTransitionType::Bounce;

        {
            PROFILE_SCOPE_CAT("Populate Overlay Geometry", "Rendering");
            target.fullW = (std::max)(1, fullW);
            target.fullH = (std::max)(1, fullH);
            target.gameW = current_gameW;
            target.gameH = current_gameH;
            target.finalX = currentGeo.finalX;
            target.finalY = currentGeo.finalY;
            target.finalW = currentGeo.finalW;
            target.finalH = currentGeo.finalH;
            target.gameTextureId = gameTextureToUse;
            target.modeId = modeToRender->id;
            target.isAnimating = isAnimating;
            target.overlayOpacity = overlayOpacity;
            target.excludeOnlyOnMyScreen = excludeOnlyOnMyScreen;
            target.skipAnimation = skipAnimation;
            target.relativeStretching = modeToRender->relativeStretching;
        }

        {
            PROFILE_SCOPE_CAT("Populate Overlay Transition", "Rendering");
            const bool transitionEffectivelyCompleteForOverlays = transitionState.active && transitionState.moveProgress >= 1.0f;
            const bool overlaysShouldLerp = transitionState.active && !transitionEffectivelyCompleteForOverlays &&
                                            transitionState.overlayTransition != OverlayTransitionType::Cut;
            if (overlaysShouldLerp) {
                target.transitionProgress = transitionState.moveProgress;
                target.fromW = transitionState.fromWidth;
                target.fromH = transitionState.fromHeight;
                target.fromX = transitionState.fromX;
                target.fromY = transitionState.fromY;
                target.toW = transitionState.targetWidth;
                target.toH = transitionState.targetHeight;
                target.toX = transitionState.targetX;
                target.toY = transitionState.targetY;
            } else if (transitionState.active) {
                target.transitionProgress = 1.0f;
                target.fromX = transitionState.fromX;
                target.fromY = transitionState.fromY;
                target.fromW = transitionState.fromWidth;
                target.fromH = transitionState.fromHeight;
                target.toX = transitionState.targetX;
                target.toY = transitionState.targetY;
                target.toW = transitionState.targetWidth;
                target.toH = transitionState.targetHeight;
            } else {
                target.transitionProgress = 1.0f;
                target.fromX = currentGeo.finalX;
                target.fromY = currentGeo.finalY;
                target.fromW = currentGeo.finalW;
                target.fromH = currentGeo.finalH;
                target.toX = currentGeo.finalX;
                target.toY = currentGeo.finalY;
                target.toW = currentGeo.finalW;
                target.toH = currentGeo.finalH;
            }
            if (transitionState.active) {
                target.fromFullW = transitionState.fromNativeWidth > 0 ? transitionState.fromNativeWidth : fullW;
                target.fromFullH = transitionState.fromNativeHeight > 0 ? transitionState.fromNativeHeight : fullH;
                target.toFullW = transitionState.toNativeWidth > 0 ? transitionState.toNativeWidth : fullW;
                target.toFullH = transitionState.toNativeHeight > 0 ? transitionState.toNativeHeight : fullH;
            } else {
                target.fromFullW = fullW;
                target.fromFullH = fullH;
                target.toFullW = fullW;
                target.toFullH = fullH;
            }
        }

        {
            PROFILE_SCOPE_CAT("Populate Overlay Runtime State", "Rendering");
            target.isTransitioningFromEyeZoom = slideAnimationsEnabled && g_isTransitioningFromEyeZoom.load(std::memory_order_acquire);
            target.shouldRenderGui = g_shouldRenderGui.load(std::memory_order_relaxed);
            target.showPerformanceOverlay = g_showPerformanceOverlay.load(std::memory_order_relaxed);
            target.showProfiler = g_showProfiler.load(std::memory_order_relaxed);
            target.showEyeZoom = EqualsIgnoreCase(target.modeId, "EyeZoom") ||
                         (target.isTransitioningFromEyeZoom && !target.skipAnimation);
            target.eyeZoomFadeOpacity = g_eyeZoomFadeOpacity.load(std::memory_order_relaxed);
            target.eyeZoomAnimatedViewportX = (skipAnimation || !slideAnimationsEnabled)
                                 ? -1
                                 : g_eyeZoomAnimatedViewportX.load(std::memory_order_relaxed);
            target.eyeZoomSnapshotTexture = GetEyeZoomSnapshotTexture();
            target.eyeZoomSnapshotWidth = GetEyeZoomSnapshotWidth();
            target.eyeZoomSnapshotHeight = GetEyeZoomSnapshotHeight();
            target.showTextureGrid = g_showTextureGrid.load(std::memory_order_relaxed);
            target.textureGridModeWidth = g_textureGridModeWidth.load(std::memory_order_relaxed);
            target.textureGridModeHeight = g_textureGridModeHeight.load(std::memory_order_relaxed);
        }

        {
            PROFILE_SCOPE_CAT("Populate Overlay Element State", "Rendering");
            target.welcomeToastIsFullscreen = isFullscreenMode;
            target.showWelcomeToast = wantWelcomeToast;
            target.showRebindIndicator = wantRebindIndicator;
            target.showCursorTrail = wantCursorTrail;
            target.modeHasMirrors = hasMirrors;
            target.modeHasImages = g_imageOverlaysVisible.load(std::memory_order_acquire) && !modeToRender->imageIds.empty();
            target.modeHasWindowOverlays = g_windowOverlaysVisible.load(std::memory_order_acquire) &&
                                           !modeToRender->windowOverlayIds.empty();
            target.modeHasBrowserOverlays = g_browserOverlaysVisible.load(std::memory_order_acquire) &&
                                            !modeToRender->browserOverlayIds.empty();

            target.fromModeId = transitionState.fromModeId;
            if (!transitionState.fromModeId.empty()) {
                if (const ModeConfig* fromMode = GetMode_Internal(transitionState.fromModeId)) {
                    target.fromSlideMirrorsIn = fromMode->slideMirrorsIn;
                }
            }
            target.toSlideMirrorsIn = modeToRender->slideMirrorsIn;
            target.mirrorSlideProgress =
                (slideAnimationsEnabled && transitionState.moveProgress < 1.0f) ? transitionState.moveProgress : 1.0f;
        }
    };

    const uint64_t mirrorCaptureFrameTag = BeginSameThreadMirrorCaptureFrame();
    if (wantOverlayThisFrame && configSnap) {
        PROFILE_SCOPE_CAT("Immediate Overlay Render", "Rendering");

        SameThreadOverlayState request;
        {
            PROFILE_SCOPE_CAT("Build Overlay Request", "Rendering");
            populateOverlayState(request);
            request.allowMirrorCaptureReuse = true;
            request.mirrorCaptureFrameTag = mirrorCaptureFrameTag;
        }
        RenderSameThreadOverlayPass(request, *configSnap, s);
    }

    if (g_showGui && !g_currentlyEditingMirror.empty()) {
        PROFILE_SCOPE_CAT("Debug Borders", "Rendering");
        if (MirrorConfig* conf = GetMutableMirror(g_currentlyEditingMirror)) {
            RenderDebugBordersForMirror(conf, { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, s.va);
        }
    }
}
void RenderDebugBordersForMirror(const MirrorConfig* conf, Color captureColor, Color outputColor, GLint originalVAO) {
    if (!conf || !g_glInitialized.load(std::memory_order_acquire)) return;

    const int fullW = GetCachedWindowWidth();
    const int fullH = GetCachedWindowHeight();
    if (fullW <= 0 || fullH <= 0) return;

    GameViewportGeometry geo;
    {
        std::lock_guard<std::mutex> lock(g_geometryMutex);
        geo = g_lastFrameGeometry;
    }

    glUseProgram(g_solidColorProgram);
    glLineWidth(2.0f);
    glDisable(GL_BLEND);

    glBindVertexArray(g_debugVAO);
    glBindBuffer(GL_ARRAY_BUFFER, g_debugVBO);

    float xScale = geo.gameW > 0 ? (float)geo.finalW / geo.gameW : 1.0f;
    float yScale = geo.gameH > 0 ? (float)geo.finalH / geo.gameH : 1.0f;

    glUniform4f(g_solidColorShaderLocs.color, captureColor.r, captureColor.g, captureColor.b, 1.0f);
    for (const auto& r : conf->input) {
        int capX, capY;
        GetRelativeCoords(r.relativeTo, r.x, r.y, conf->captureWidth, conf->captureHeight, geo.gameW, geo.gameH, capX, capY);

        int scaled_capX = geo.finalX + static_cast<int>(capX * xScale);
        int scaled_capY = geo.finalY + static_cast<int>(capY * yScale);
        int scaled_capW = static_cast<int>(conf->captureWidth * xScale);
        int scaled_capH = static_cast<int>(conf->captureHeight * yScale);

        int scaled_capY_gl = fullH - scaled_capY - scaled_capH;

        float x1 = (static_cast<float>(scaled_capX) / fullW) * 2.0f - 1.0f;
        float y1 = (static_cast<float>(scaled_capY_gl) / fullH) * 2.0f - 1.0f;
        float x2 = (static_cast<float>(scaled_capX + scaled_capW) / fullW) * 2.0f - 1.0f;
        float y2 = (static_cast<float>(scaled_capY_gl + scaled_capH) / fullH) * 2.0f - 1.0f;
        float v[] = { x1, y1, x2, y1, x2, y2, x1, y2 };
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(v), v);
        glDrawArrays(GL_LINE_LOOP, 0, 4);
    }

    auto instIt = g_mirrorInstances.find(conf->name);
    if (instIt != g_mirrorInstances.end()) {
        const MirrorInstance& inst = instIt->second;
        int finalScreenX, finalScreenY_win;
        CalculateFinalScreenPos(conf, inst, geo.gameW, geo.gameH, geo.finalX, geo.finalY, geo.finalW, geo.finalH, fullW, fullH,
                                finalScreenX, finalScreenY_win);
        float scaleX = conf->output.separateScale ? conf->output.scaleX : conf->output.scale;
        float scaleY = conf->output.separateScale ? conf->output.scaleY : conf->output.scale;
        int outW = static_cast<int>(inst.fbo_w * scaleX);
        int outH = static_cast<int>(inst.fbo_h * scaleY);

        int padding = (inst.fbo_w - conf->captureWidth) / 2;
        int paddingScaledX = static_cast<int>(padding * scaleX);
        int paddingScaledY = static_cast<int>(padding * scaleY);

        int finalScreenY_gl = fullH - finalScreenY_win - outH;

        glUniform4f(g_solidColorShaderLocs.color, outputColor.r, outputColor.g, outputColor.b, 1.0f);
        float x1 = (static_cast<float>(finalScreenX + paddingScaledX) / fullW) * 2.0f - 1.0f;
        float y1 = (static_cast<float>(finalScreenY_gl + paddingScaledY) / fullH) * 2.0f - 1.0f;
        float x2 = (static_cast<float>(finalScreenX + outW - paddingScaledX) / fullW) * 2.0f - 1.0f;
        float y2 = (static_cast<float>(finalScreenY_gl + outH - paddingScaledY) / fullH) * 2.0f - 1.0f;
        float v[] = { x1, y1, x2, y1, x2, y2, x1, y2 };
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(v), v);
        glDrawArrays(GL_LINE_LOOP, 0, 4);
    }

    glBindVertexArray(originalVAO);
}

void InitializeOverlayTextFont(const std::string& fontPath, float baseFontSize, float scaleFactor) {
    std::lock_guard<std::recursive_mutex> imguiLock(GetImGuiContextMutex());
    if (!ImGui::GetCurrentContext()) return;

    ImGuiIO& io = ImGui::GetIO();
    const float sizePixels = baseFontSize * 1.5f * scaleFactor;

    const std::string configuredOverlayPath = g_config.eyezoom.textFontPath;
    const std::string bundledFontPath = ResolveToolscreenRelativePath(ConfigDefaults::CONFIG_FONT_PATH, g_toolscreenPath);
    const std::string systemFallbackFontPath = ConfigDefaults::CONFIG_FALLBACK_FONT_PATH;

    std::string usePath = ResolveToolscreenRelativePath(configuredOverlayPath.empty() ? fontPath : configuredOverlayPath, g_toolscreenPath);
    if (usePath.empty()) { usePath = bundledFontPath; }

    auto isStable = [](const std::string& p, float sz) -> bool {
        if (p.empty()) return false;
        ImFontAtlas testAtlas;
        ImFont* f = testAtlas.AddFontFromFileTTF(p.c_str(), sz);
        if (!f) return false;
        return testAtlas.Build();
    };

    if (!isStable(usePath, sizePixels)) {
        usePath = isStable(bundledFontPath, sizePixels) ? bundledFontPath : systemFallbackFontPath;
    }

    g_overlayTextFont = io.Fonts->AddFontFromFileTTF(usePath.c_str(), sizePixels);
    if (!g_overlayTextFont && usePath != bundledFontPath) {
        g_overlayTextFont = io.Fonts->AddFontFromFileTTF(bundledFontPath.c_str(), sizePixels);
    }
    if (!g_overlayTextFont && usePath != systemFallbackFontPath) {
        g_overlayTextFont = io.Fonts->AddFontFromFileTTF(systemFallbackFontPath.c_str(), sizePixels);
    }
    if (!g_overlayTextFont) {
        g_overlayTextFont = io.Fonts->AddFontDefault();
    }
}

void SetOverlayTextFontSize(int sizePixels) {
    if (sizePixels < 1) sizePixels = 1;
    if (sizePixels > 512) sizePixels = 512;
    g_overlayTextFontSize = static_cast<float>(sizePixels);
    g_eyeZoomFontNeedsReload.store(true, std::memory_order_release);
}


void RenderTextureGridOverlay(bool showTextureGrid, int modeWidth, int modeHeight) {

    static bool loggedOnce = false;
    if (!loggedOnce) {
        Log("RenderTextureGridOverlay called - g_glInitialized: " +
            std::to_string(g_glInitialized.load(std::memory_order_acquire) ? 1 : 0) +
            ", g_solidColorProgram: " + std::to_string(g_solidColorProgram));
        loggedOnce = true;
    }

    if (!g_glInitialized.load(std::memory_order_acquire) || g_solidColorProgram == 0) { return; }

    const int MAX_TEXTURE_ID = 100;
    const int TILE_SIZE = 128;
    const int PADDING = 1;
    const int MARGIN = 1;

    int screenW = GetCachedWindowWidth();
    int screenH = GetCachedWindowHeight();

    // Collect all Toolscreen-owned texture IDs for the "OURS" label
    std::unordered_set<GLuint> ourTextureIds;
    {
        // Scene FBO texture
        if (g_sceneTexture != 0) ourTextureIds.insert(g_sceneTexture);

        // Same-thread OBS and virtual-camera helper textures
        for (GLuint textureId : g_sameThreadObsComposeTextures) {
            if (textureId != 0) { ourTextureIds.insert(textureId); }
        }
        if (g_sameThreadVirtualCameraScaleTexture != 0) { ourTextureIds.insert(g_sameThreadVirtualCameraScaleTexture); }
        for (const auto& slot : g_sameThreadVirtualCameraReadbackSlots) {
            if (slot.yTexture != 0) { ourTextureIds.insert(slot.yTexture); }
            if (slot.uvTexture != 0) { ourTextureIds.insert(slot.uvTexture); }
        }

        // OBS textures
        GLuint obsOverride = g_obsOverrideTexture.load(std::memory_order_acquire);
        if (obsOverride != 0) ourTextureIds.insert(obsOverride);
        GLuint obsCapture = GetObsCaptureTexture();
        if (obsCapture != 0) ourTextureIds.insert(obsCapture);

        // Mirror instance textures (try_lock to avoid blocking)
        {
            std::shared_lock<std::shared_mutex> lock(g_mirrorInstancesMutex, std::try_to_lock);
            if (lock.owns_lock()) {
                for (const auto& [name, inst] : g_mirrorInstances) {
                    if (inst.fboTexture != 0) ourTextureIds.insert(inst.fboTexture);
                    if (inst.fboTextureBack != 0) ourTextureIds.insert(inst.fboTextureBack);
                    if (inst.tempCaptureTexture != 0) ourTextureIds.insert(inst.tempCaptureTexture);
                    if (inst.finalTexture != 0) ourTextureIds.insert(inst.finalTexture);
                    if (inst.finalTextureBack != 0) ourTextureIds.insert(inst.finalTextureBack);
                }
            }
        }

        // User image textures
        {
            std::unique_lock<std::mutex> lock(g_userImagesMutex, std::try_to_lock);
            if (lock.owns_lock()) {
                for (const auto& [name, inst] : g_userImages) {
                    if (inst.textureId != 0) ourTextureIds.insert(inst.textureId);
                    for (GLuint ft : inst.frameTextures) { if (ft != 0) ourTextureIds.insert(ft); }
                }
            }
        }

        // Background textures
        {
            std::unique_lock<std::mutex> lock(g_backgroundTexturesMutex, std::try_to_lock);
            if (lock.owns_lock()) {
                for (const auto& [name, inst] : g_backgroundTextures) {
                    if (inst.textureId != 0) ourTextureIds.insert(inst.textureId);
                    for (GLuint ft : inst.frameTextures) { if (ft != 0) ourTextureIds.insert(ft); }
                }
            }
        }
    }

    struct TexInfo { GLuint id; GLint width; GLint height; GLint internalFormat; };
    std::vector<TexInfo> validTextures;
    for (GLuint id = 0; id <= MAX_TEXTURE_ID; id++) {
        if (glIsTexture(id)) {
            BindTextureDirect(GL_TEXTURE_2D, id);
            GLint texWidth = 0, texHeight = 0, internalFormat = 0;
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &texWidth);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &texHeight);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &internalFormat);

            if (modeWidth > 0 && modeHeight > 0) {
                if (texWidth == modeWidth && texHeight == modeHeight && internalFormat == GL_RGBA8) {
                    validTextures.push_back({ id, texWidth, texHeight, internalFormat });
                }
            } else {
                validTextures.push_back({ id, texWidth, texHeight, internalFormat });
            }
        }
    }

    if (validTextures.empty()) { return; }

    {
        std::lock_guard<std::mutex> lock(s_textureGridMutex);
        s_textureGridLabels.clear();
    }

    int tilesPerRow = (screenW - 2 * MARGIN) / (TILE_SIZE + PADDING);
    if (tilesPerRow < 1) tilesPerRow = 1;

    GLint lastProgram, lastTexture, lastVAO, lastArrayBuffer, lastActiveTexture;
    GLint lastBlendSrc, lastBlendDst;
    GLint lastMinFilter, lastMagFilter;
    GLboolean blendEnabled, depthEnabled;
    glGetIntegerv(GL_CURRENT_PROGRAM, &lastProgram);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &lastTexture);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &lastVAO);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &lastArrayBuffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &lastActiveTexture);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &lastBlendSrc);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &lastBlendDst);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &lastMinFilter);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &lastMagFilter);
    blendEnabled = glIsEnabled(GL_BLEND);
    depthEnabled = glIsEnabled(GL_DEPTH_TEST);

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(g_imageRenderProgram);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glActiveTexture(GL_TEXTURE0);

    glUniform1i(g_imageRenderShaderLocs.imageTexture, 0);
    glUniform1i(g_imageRenderShaderLocs.enableColorKey, 0);
    glUniform1f(g_imageRenderShaderLocs.opacity, 1.0f);

    std::unordered_map<GLuint, std::pair<GLint, GLint>> texFilterStates;

    int col = 0;
    int row = 0;
    for (const auto& tex : validTextures) {
        int x = MARGIN + col * (TILE_SIZE + PADDING);
        int y = MARGIN + row * (TILE_SIZE + PADDING);

        BindTextureDirect(GL_TEXTURE_2D, tex.id);

        GLint texWidth = tex.width;
        GLint texHeight = tex.height;
        GLint internalFormat = tex.internalFormat;

        GLint minFilter = 0, magFilter = 0, wrapS = 0, wrapT = 0;
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &minFilter);
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &magFilter);
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &wrapS);
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, &wrapT);

        float sizeMB = (texWidth * texHeight * 4) / (1024.0f * 1024.0f);

        {
            bool isOurs = ourTextureIds.count(tex.id) > 0;
            std::lock_guard<std::mutex> lock(s_textureGridMutex);
            s_textureGridLabels.push_back({ tex.id, (float)x, (float)y, TILE_SIZE, texWidth, texHeight, sizeMB, (GLenum)internalFormat,
                                            minFilter, magFilter, wrapS, wrapT, isOurs });
        }

        texFilterStates[tex.id] = { minFilter, magFilter };

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        float x1_ndc = (x / (float)screenW) * 2.0f - 1.0f;
        float x2_ndc = ((x + TILE_SIZE) / (float)screenW) * 2.0f - 1.0f;

        int y_gl = screenH - y - TILE_SIZE;
        float y1_ndc = (y_gl / (float)screenH) * 2.0f - 1.0f;
        float y2_ndc = ((y_gl + TILE_SIZE) / (float)screenH) * 2.0f - 1.0f;

        float verts[] = {
            x1_ndc, y1_ndc, 0.0f, 0.0f,
            x2_ndc, y1_ndc, 1.0f, 0.0f,
            x2_ndc, y2_ndc, 1.0f, 1.0f,
            x1_ndc, y1_ndc, 0.0f, 0.0f,
            x2_ndc, y2_ndc, 1.0f, 1.0f,
            x1_ndc, y2_ndc, 0.0f, 1.0f
        };

        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        col++;
        if (col >= tilesPerRow) {
            col = 0;
            row++;
        }
    }

    for (const auto& pair : texFilterStates) {
        BindTextureDirect(GL_TEXTURE_2D, pair.first);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, pair.second.first);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, pair.second.second);
    }

    glActiveTexture(lastActiveTexture);
    BindTextureDirect(GL_TEXTURE_2D, lastTexture);
    glBindVertexArray(lastVAO);
    glBindBuffer(GL_ARRAY_BUFFER, lastArrayBuffer);
    glUseProgram(lastProgram);

    if (depthEnabled)
        glEnable(GL_DEPTH_TEST);
    else
        glDisable(GL_DEPTH_TEST);

    if (blendEnabled) {
        glEnable(GL_BLEND);
        glBlendFunc(lastBlendSrc, lastBlendDst);
    } else {
        glDisable(GL_BLEND);
    }
}

void RenderCachedTextureGridLabels() {
    std::lock_guard<std::mutex> lock(s_textureGridMutex);

    if (!ImGui::GetCurrentContext() || s_textureGridLabels.empty()) { return; }

    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    float fontSize = ImGui::GetFontSize();

    for (const auto& label : s_textureGridLabels) {
        char idText[32];
        sprintf_s(idText, "ID: %u", label.textureId);

        char resText[64];
        sprintf_s(resText, "%dx%d", label.width, label.height);

        char sizeText[32];
        sprintf_s(sizeText, "%.2f MB", label.sizeMB);

        const char* formatStr = "UNK";
        if (label.internalFormat == GL_RGBA8)
            formatStr = "RGBA8";
        else if (label.internalFormat == GL_RGB8)
            formatStr = "RGB8";
        else if (label.internalFormat == GL_RGBA)
            formatStr = "RGBA";
        else if (label.internalFormat == GL_RGB)
            formatStr = "RGB";

        char formatText[32];
        sprintf_s(formatText, "Fmt: %s", formatStr);

        const char* minFilterStr = "?";
        if (label.minFilter == GL_NEAREST)
            minFilterStr = "N";
        else if (label.minFilter == GL_LINEAR)
            minFilterStr = "L";
        else if (label.minFilter == GL_NEAREST_MIPMAP_NEAREST)
            minFilterStr = "NMN";
        else if (label.minFilter == GL_LINEAR_MIPMAP_NEAREST)
            minFilterStr = "LMN";
        else if (label.minFilter == GL_NEAREST_MIPMAP_LINEAR)
            minFilterStr = "NML";
        else if (label.minFilter == GL_LINEAR_MIPMAP_LINEAR)
            minFilterStr = "LML";

        const char* magFilterStr = "?";
        if (label.magFilter == GL_NEAREST)
            magFilterStr = "N";
        else if (label.magFilter == GL_LINEAR)
            magFilterStr = "L";

        char filterText[32];
        sprintf_s(filterText, "F:%s/%s", minFilterStr, magFilterStr);

        const char* wrapSStr = "?";
        if (label.wrapS == GL_REPEAT)
            wrapSStr = "R";
        else if (label.wrapS == GL_CLAMP_TO_EDGE)
            wrapSStr = "C";
        else if (label.wrapS == GL_MIRRORED_REPEAT)
            wrapSStr = "M";
        else if (label.wrapS == GL_CLAMP_TO_BORDER)
            wrapSStr = "B";

        const char* wrapTStr = "?";
        if (label.wrapT == GL_REPEAT)
            wrapTStr = "R";
        else if (label.wrapT == GL_CLAMP_TO_EDGE)
            wrapTStr = "C";
        else if (label.wrapT == GL_MIRRORED_REPEAT)
            wrapTStr = "M";
        else if (label.wrapT == GL_CLAMP_TO_BORDER)
            wrapTStr = "B";

        char wrapText[32];
        sprintf_s(wrapText, "W:%s/%s", wrapSStr, wrapTStr);

        char ownerText[16];
        sprintf_s(ownerText, "%s", label.isOurs ? "OURS" : "GAME");

        //const char* lines[] = { idText, ownerText, resText, sizeText, formatText, filterText, wrapText };
        //const int lineCount = 7;
        const char* lines[] = { idText, ownerText };
        const int lineCount = 2;
        float lineSpacing = 2.0f;

        float currentY = label.y + 2.0f;
        for (int i = 0; i < lineCount; i++) {
            ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, lines[i]);
            ImVec2 textPos(label.x + (label.tileSize - textSize.x) / 2.0f, currentY);

            ImVec2 bgMin(textPos.x - 2, textPos.y - 1);
            ImVec2 bgMax(textPos.x + textSize.x + 2, textPos.y + textSize.y + 1);
            //drawList->AddRectFilled(bgMin, bgMax, IM_COL32(0, 0, 0, 180));

            ImU32 textCol = IM_COL32(255, 255, 255, 255);
            // Highlight the owner line: cyan for OURS, default for GAME
            if (i == 1 && label.isOurs) { textCol = IM_COL32(0, 200, 255, 255); }
            drawList->AddText(textPos, textCol, lines[i]);

            currentY += textSize.y + lineSpacing;
        }
    }
}

static float EaseOutPower(float t, float power) {
    float t1 = t - 1.0f;
    float sign = (t1 < 0) ? -1.0f : 1.0f;
    return sign * std::pow(std::abs(t1), power) + 1.0f;
}

static float EaseInPower(float t, float power) { return std::pow(t, power); }

static float ApplyDualEasing(float t, float easeInPower, float easeOutPower) {
    easeInPower = std::clamp(easeInPower, 1.0f, 10.0f);
    easeOutPower = std::clamp(easeOutPower, 1.0f, 10.0f);

    if (easeInPower <= 1.0f && easeOutPower <= 1.0f) { return t; }

    if (t < 0.5f) {
        float halfT = t * 2.0f;
        float easedHalfT = EaseInPower(halfT, easeInPower);
        return easedHalfT * 0.5f;
    } else {
        float halfT = (t - 0.5f) * 2.0f;
        float easedHalfT = EaseOutPower(halfT, easeOutPower);
        return 0.5f + easedHalfT * 0.5f;
    }
}

static float CalculateBounceOffset(float bounceProgress, int bounceIndex, int totalBounces, float intensity) {
    if (totalBounces <= 0 || bounceIndex >= totalBounces) return 0.0f;

    float decayFactor = 1.0f - (static_cast<float>(bounceIndex) / static_cast<float>(totalBounces));
    decayFactor = decayFactor * decayFactor;

    float angle = bounceProgress * 3.14159265f;
    float bounce = std::sin(angle) * intensity * decayFactor;

    return bounce;
}

static void ResolveModeTransitionTargetBounds(const ModeConfig& mode, int fullW, int fullH, int& outWidth, int& outHeight, int& outX,
                                              int& outY) {
    if (mode.stretch.enabled) {
        if (EqualsIgnoreCase(mode.id, "Fullscreen")) {
            outWidth = fullW;
            outHeight = fullH;
            outX = 0;
            outY = 0;
        } else {
            outWidth = mode.stretch.width;
            outHeight = mode.stretch.height;
            outX = mode.stretch.x;
            outY = mode.stretch.y;
        }
    } else {
        outWidth = mode.width;
        outHeight = mode.height;
        outX = GetCenteredAxisOffset(fullW, outWidth);
        outY = GetCenteredAxisOffset(fullH, outHeight);
    }
}

static void PublishViewportTransitionSnapshotLocked() {
    int nextSnapshotIndex = 1 - g_viewportTransitionSnapshotIndex.load(std::memory_order_relaxed);
    ViewportTransitionSnapshot& snapshot = g_viewportTransitionSnapshots[nextSnapshotIndex];
    snapshot.active = g_modeTransition.active;
    snapshot.isBounceTransition = (g_modeTransition.gameTransition == GameTransitionType::Bounce);
    snapshot.fromModeId = g_modeTransition.fromModeId;
    snapshot.toModeId = g_modeTransition.toModeId;
    snapshot.fromWidth = g_modeTransition.fromWidth;
    snapshot.fromHeight = g_modeTransition.fromHeight;
    snapshot.fromX = g_modeTransition.fromX;
    snapshot.fromY = g_modeTransition.fromY;
    snapshot.currentX = g_modeTransition.currentX;
    snapshot.currentY = g_modeTransition.currentY;
    snapshot.currentWidth = g_modeTransition.currentWidth;
    snapshot.currentHeight = g_modeTransition.currentHeight;
    snapshot.toX = g_modeTransition.toX;
    snapshot.toY = g_modeTransition.toY;
    snapshot.toWidth = g_modeTransition.toWidth;
    snapshot.toHeight = g_modeTransition.toHeight;
    snapshot.fromNativeWidth = g_modeTransition.fromNativeWidth;
    snapshot.fromNativeHeight = g_modeTransition.fromNativeHeight;
    snapshot.toNativeWidth = g_modeTransition.toNativeWidth;
    snapshot.toNativeHeight = g_modeTransition.toNativeHeight;
    snapshot.gameTransition = g_modeTransition.gameTransition;
    snapshot.overlayTransition = g_modeTransition.overlayTransition;
    snapshot.backgroundTransition = g_modeTransition.backgroundTransition;
    snapshot.progress = g_modeTransition.progress;
    snapshot.moveProgress = g_modeTransition.moveProgress;
    snapshot.startTime = g_modeTransition.startTime;
    g_viewportTransitionSnapshotIndex.store(nextSnapshotIndex, std::memory_order_release);
}

void StartModeTransition(const std::string& fromModeId, const std::string& toModeId, int fromWidth, int fromHeight, int fromX, int fromY,
                         int toWidth, int toHeight, int toX, int toY, const ModeConfig& toMode) {
    LogCategory("animation", "[ANIMATION] StartModeTransition entry - acquiring g_modeTransitionMutex...");
    std::lock_guard<std::mutex> lock(g_modeTransitionMutex);
    LogCategory("animation", "[ANIMATION] g_modeTransitionMutex acquired");

    bool transitioningToFullscreen = EqualsIgnoreCase(toModeId, "Fullscreen");
    bool transitioningFromFullscreen = EqualsIgnoreCase(fromModeId, "Fullscreen");

    bool isAllCutTransition = toMode.gameTransition == GameTransitionType::Cut && toMode.overlayTransition == OverlayTransitionType::Cut &&
                              toMode.backgroundTransition == BackgroundTransitionType::Cut;

    if (isAllCutTransition && !transitioningToFullscreen) {
        LogCategory("animation", "[ANIMATION] Cut/Cut/Cut transition - using 1-frame protection to prevent black flash");
    }

    g_modeTransition.active = true;
    g_modeTransition.startTime = std::chrono::steady_clock::now();

    bool transitioningToEyeZoom = EqualsIgnoreCase(toModeId, "EyeZoom");
    bool transitioningFromEyeZoom = EqualsIgnoreCase(fromModeId, "EyeZoom");
    auto transitionSnap = GetConfigSnapshot();
    const ModeConfig* sourceMode = transitionSnap ? GetModeFromSnapshot(*transitionSnap, fromModeId) : nullptr;
    const bool sourceSkipAnimateX = sourceMode && sourceMode->skipAnimateX;
    const bool sourceSkipAnimateY = sourceMode && sourceMode->skipAnimateY;
    const bool preserveEyeZoomSlideOutDuration = transitioningFromEyeZoom && !transitioningToEyeZoom && transitionSnap &&
                                                 transitionSnap->eyezoom.slideMirrorsIn &&
                                                 toMode.overlayTransition != OverlayTransitionType::Cut;

    bool allCutToFullscreen = transitioningToFullscreen && toMode.gameTransition == GameTransitionType::Cut;
    bool allCutWithFirstFrameProtection = isAllCutTransition && !transitioningToFullscreen;
    if (allCutToFullscreen || allCutWithFirstFrameProtection) {
        if (preserveEyeZoomSlideOutDuration) {
            const int sourceDurationMs = sourceMode ? sourceMode->transitionDurationMs : toMode.transitionDurationMs;
            g_modeTransition.duration = (std::max)(0.001f, sourceDurationMs / 1000.0f);
        } else {
            g_modeTransition.duration = 0.001f;
        }
    } else {
        g_modeTransition.duration = toMode.transitionDurationMs / 1000.0f;
    }

    g_modeTransition.gameTransition = toMode.gameTransition;
    g_modeTransition.overlayTransition = OverlayTransitionType::Cut;
    g_modeTransition.backgroundTransition = BackgroundTransitionType::Cut;

    g_modeTransition.easeInPower = toMode.easeInPower;
    g_modeTransition.easeOutPower = toMode.easeOutPower;
    g_modeTransition.bounceCount = toMode.bounceCount;
    g_modeTransition.bounceIntensity = toMode.bounceIntensity;
    g_modeTransition.bounceDurationMs = toMode.bounceDurationMs;

    g_modeTransition.skipAnimateX = sourceSkipAnimateX || toMode.skipAnimateX;
    g_modeTransition.skipAnimateY = sourceSkipAnimateY || toMode.skipAnimateY;

    g_modeTransition.fromModeId = fromModeId;
    g_modeTransition.fromWidth = fromWidth;
    g_modeTransition.fromHeight = fromHeight;
    g_modeTransition.fromX = fromX;
    g_modeTransition.fromY = fromY;

    g_modeTransition.toModeId = toModeId;
    g_modeTransition.toWidth = toWidth;
    g_modeTransition.toHeight = toHeight;
    g_modeTransition.toX = toX;
    g_modeTransition.toY = toY;

    // Use snapshot for thread-safe lookup of fromMode (called from multiple threads)
    const ModeConfig* fromModePtr = transitionSnap ? GetModeFromSnapshot(*transitionSnap, fromModeId) : nullptr;
    if (fromModePtr) {
        g_modeTransition.fromNativeWidth = fromModePtr->width;
        g_modeTransition.fromNativeHeight = fromModePtr->height;
    } else {
        g_modeTransition.fromNativeWidth = fromWidth;
        g_modeTransition.fromNativeHeight = fromHeight;
    }
    g_modeTransition.toNativeWidth = toMode.width > 0 ? toMode.width : toWidth;
    g_modeTransition.toNativeHeight = toMode.height > 0 ? toMode.height : toHeight;

    if (toMode.gameTransition == GameTransitionType::Bounce) {
        g_modeTransition.currentWidth = fromWidth;
        g_modeTransition.currentHeight = fromHeight;
        g_modeTransition.currentX = fromX;
        g_modeTransition.currentY = fromY;
    } else {
        g_modeTransition.currentWidth = toWidth;
        g_modeTransition.currentHeight = toHeight;
        g_modeTransition.currentX = toX;
        g_modeTransition.currentY = toY;
    }
    g_modeTransition.progress = 0.0f;
    g_modeTransition.moveProgress = 0.0f; // Must initialize to 0 for first frame overlay positioning
    g_modeTransition.wmSizeSent = false;

    g_modeTransition.lastSentWidth = 0;
    g_modeTransition.lastSentHeight = 0;

    if (transitioningFromEyeZoom && !transitioningToEyeZoom) {
        g_isTransitioningFromEyeZoom.store(true, std::memory_order_release);
        LogCategory("animation", "[ANIMATION] Set g_isTransitioningFromEyeZoom=true BEFORE WM_SIZE to freeze snapshot");
    } else {
        g_isTransitioningFromEyeZoom.store(false, std::memory_order_release);
    }

    int wmWidth = toMode.width > 0 ? toMode.width : toWidth;
    int wmHeight = toMode.height > 0 ? toMode.height : toHeight;

    HWND hwnd = g_minecraftHwnd.load();
    if (hwnd && wmWidth > 0 && wmHeight > 0) {
        const bool posted = RequestWindowClientResize(hwnd, wmWidth, wmHeight, "mode_transition:start");
        g_modeTransition.wmSizeSent = posted;
        if (posted) {
            g_modeTransition.lastSentWidth = wmWidth;
            g_modeTransition.lastSentHeight = wmHeight;
            LogCategory("animation", "[ANIMATION] WM_SIZE sent immediately: " + std::to_string(wmWidth) + "x" + std::to_string(wmHeight));
        }
    }

    LogCategory("animation", "[ANIMATION] Starting mode transition (Game:" + GameTransitionTypeToString(toMode.gameTransition) +
                                 ", Overlay:" + OverlayTransitionTypeToString(toMode.overlayTransition) +
                                 ", Bg:" + BackgroundTransitionTypeToString(toMode.backgroundTransition) + ", " +
                                 std::to_string(toMode.transitionDurationMs) + "ms): " + fromModeId + " (" + std::to_string(fromWidth) +
                                 "x" + std::to_string(fromHeight) + " at " + std::to_string(fromX) + "," + std::to_string(fromY) + ")" +
                                 " -> " + toModeId + " (" + std::to_string(toWidth) + "x" + std::to_string(toHeight) + " at " +
                                 std::to_string(toX) + "," + std::to_string(toY) + ")");

    PublishViewportTransitionSnapshotLocked();

    LogCategory("animation", "[ANIMATION] StartModeTransition complete - releasing g_modeTransitionMutex");
}

void RetargetActiveModeTransition(const ModeConfig& mode) {
    std::lock_guard<std::mutex> lock(g_modeTransitionMutex);

    if (!g_modeTransition.active || !EqualsIgnoreCase(g_modeTransition.toModeId, mode.id)) { return; }

    int fullW = GetCachedWindowWidth();
    int fullH = GetCachedWindowHeight();
    if (fullW < 1) fullW = 1;
    if (fullH < 1) fullH = 1;

    int toWidth = 0;
    int toHeight = 0;
    int toX = 0;
    int toY = 0;
    ResolveModeTransitionTargetBounds(mode, fullW, fullH, toWidth, toHeight, toX, toY);

    g_modeTransition.toWidth = toWidth;
    g_modeTransition.toHeight = toHeight;
    g_modeTransition.toX = toX;
    g_modeTransition.toY = toY;
    g_modeTransition.toNativeWidth = mode.width > 0 ? mode.width : toWidth;
    g_modeTransition.toNativeHeight = mode.height > 0 ? mode.height : toHeight;

    if (g_modeTransition.gameTransition != GameTransitionType::Bounce) {
        g_modeTransition.currentWidth = g_modeTransition.toWidth;
        g_modeTransition.currentHeight = g_modeTransition.toHeight;
        g_modeTransition.currentX = g_modeTransition.toX;
        g_modeTransition.currentY = g_modeTransition.toY;
    } else {
        if (g_modeTransition.skipAnimateX) {
            g_modeTransition.currentWidth = g_modeTransition.toWidth;
            g_modeTransition.currentX = g_modeTransition.toX;
        }
        if (g_modeTransition.skipAnimateY) {
            g_modeTransition.currentHeight = g_modeTransition.toHeight;
            g_modeTransition.currentY = g_modeTransition.toY;
        }
    }

    PublishViewportTransitionSnapshotLocked();
}

void UpdateModeTransition() {
    std::lock_guard<std::mutex> lock(g_modeTransitionMutex);

    if (!g_modeTransition.active) return;

    auto now = std::chrono::steady_clock::now();
    float elapsed = std::chrono::duration<float>(now - g_modeTransition.startTime).count();

    float baseDuration = g_modeTransition.duration;
    float totalBounceDuration =
        (g_modeTransition.bounceCount > 0) ? (g_modeTransition.bounceCount * g_modeTransition.bounceDurationMs / 1000.0f) : 0.0f;
    float totalDuration = baseDuration + totalBounceDuration;

    float progress = elapsed / totalDuration;
    g_modeTransition.progress = (progress < 1.0f) ? progress : 1.0f;

    if (g_modeTransition.gameTransition == GameTransitionType::Bounce) {
        float baseRatio = baseDuration / totalDuration;

        float moveProgress = 0.0f;
        float bounceOffset = 0.0f;

        if (g_modeTransition.progress < baseRatio) {
            float phaseProgress = g_modeTransition.progress / baseRatio;
            moveProgress = std::clamp(phaseProgress, 0.0f, 1.0f);
            moveProgress = ApplyDualEasing(moveProgress, g_modeTransition.easeInPower, g_modeTransition.easeOutPower);
        } else {
            moveProgress = 1.0f;

            if (g_modeTransition.bounceCount > 0 && totalBounceDuration > 0) {
                float bounceElapsed = (g_modeTransition.progress - baseRatio) * totalDuration;
                float singleBounceDuration = g_modeTransition.bounceDurationMs / 1000.0f;

                int currentBounce = static_cast<int>(bounceElapsed / singleBounceDuration);
                if (currentBounce < g_modeTransition.bounceCount) {
                    float bouncePhaseProgress = std::fmod(bounceElapsed, singleBounceDuration) / singleBounceDuration;
                    bounceOffset = CalculateBounceOffset(bouncePhaseProgress, currentBounce, g_modeTransition.bounceCount,
                                                         g_modeTransition.bounceIntensity);
                }
            }
        }

        int baseWidth, baseHeight, baseX, baseY;

        if (g_modeTransition.fromWidth == g_modeTransition.toWidth) {
            baseWidth = g_modeTransition.toWidth;
        } else {
            baseWidth =
                static_cast<int>(g_modeTransition.fromWidth + (g_modeTransition.toWidth - g_modeTransition.fromWidth) * moveProgress);
        }

        if (g_modeTransition.fromHeight == g_modeTransition.toHeight) {
            baseHeight = g_modeTransition.toHeight;
        } else {
            baseHeight =
                static_cast<int>(g_modeTransition.fromHeight + (g_modeTransition.toHeight - g_modeTransition.fromHeight) * moveProgress);
        }

        if (g_modeTransition.fromX == g_modeTransition.toX) {
            baseX = g_modeTransition.toX;
        } else {
            baseX = static_cast<int>(g_modeTransition.fromX + (g_modeTransition.toX - g_modeTransition.fromX) * moveProgress);
        }

        if (g_modeTransition.fromY == g_modeTransition.toY) {
            baseY = g_modeTransition.toY;
        } else {
            baseY = static_cast<int>(g_modeTransition.fromY + (g_modeTransition.toY - g_modeTransition.fromY) * moveProgress);
        }

        if (g_modeTransition.skipAnimateX) {
            baseWidth = g_modeTransition.toWidth;
            baseX = g_modeTransition.toX;
        }
        if (g_modeTransition.skipAnimateY) {
            baseHeight = g_modeTransition.toHeight;
            baseY = g_modeTransition.toY;
        }

        if (bounceOffset != 0.0f) {
            int deltaW = g_modeTransition.toWidth - g_modeTransition.fromWidth;
            int deltaH = g_modeTransition.toHeight - g_modeTransition.fromHeight;
            bool skipBounceX = g_modeTransition.skipAnimateX ||
                               (g_modeTransition.fromWidth == g_modeTransition.toWidth && g_modeTransition.fromX == g_modeTransition.toX);
            if (skipBounceX) {
                g_modeTransition.currentWidth = g_modeTransition.toWidth;
                g_modeTransition.currentX = g_modeTransition.toX;
            } else {
                g_modeTransition.currentWidth = g_modeTransition.toWidth - static_cast<int>(deltaW * bounceOffset);
                int deltaX = g_modeTransition.toX - g_modeTransition.fromX;
                g_modeTransition.currentX = g_modeTransition.toX - static_cast<int>(deltaX * bounceOffset);
            }
            bool skipBounceY = g_modeTransition.skipAnimateY ||
                               (g_modeTransition.fromHeight == g_modeTransition.toHeight && g_modeTransition.fromY == g_modeTransition.toY);
            if (skipBounceY) {
                g_modeTransition.currentHeight = g_modeTransition.toHeight;
                g_modeTransition.currentY = g_modeTransition.toY;
            } else {
                g_modeTransition.currentHeight = g_modeTransition.toHeight - static_cast<int>(deltaH * bounceOffset);
                int deltaY = g_modeTransition.toY - g_modeTransition.fromY;
                g_modeTransition.currentY = g_modeTransition.toY - static_cast<int>(deltaY * bounceOffset);
            }
        } else {
            g_modeTransition.currentWidth = baseWidth;
            g_modeTransition.currentHeight = baseHeight;
            g_modeTransition.currentX = baseX;
            g_modeTransition.currentY = baseY;
        }

        g_modeTransition.moveProgress = moveProgress;
    } else {
        g_modeTransition.moveProgress = g_modeTransition.progress;
    }


    bool allComplete = (elapsed >= totalDuration);

    if (allComplete) {
        LogCategory("animation", "[ANIMATION] Mode transition complete: " + g_modeTransition.toModeId + " (final stretch: " +
                                     std::to_string(g_modeTransition.toWidth) + "x" + std::to_string(g_modeTransition.toHeight) + " at " +
                                     std::to_string(g_modeTransition.toX) + "," + std::to_string(g_modeTransition.toY) + ")");

        g_modeTransition.currentWidth = g_modeTransition.toWidth;
        g_modeTransition.currentHeight = g_modeTransition.toHeight;
        g_modeTransition.currentX = g_modeTransition.toX;
        g_modeTransition.currentY = g_modeTransition.toY;

        g_modeTransition.active = false;
    }

    PublishViewportTransitionSnapshotLocked();
}

bool IsModeTransitionActive() {
    std::lock_guard<std::mutex> lock(g_modeTransitionMutex);
    return g_modeTransition.active;
}

GameTransitionType GetGameTransitionType() {
    std::lock_guard<std::mutex> lock(g_modeTransitionMutex);
    return g_modeTransition.active ? g_modeTransition.gameTransition : GameTransitionType::Cut;
}

OverlayTransitionType GetOverlayTransitionType() {
    std::lock_guard<std::mutex> lock(g_modeTransitionMutex);
    return g_modeTransition.active ? g_modeTransition.overlayTransition : OverlayTransitionType::Cut;
}

BackgroundTransitionType GetBackgroundTransitionType() {
    std::lock_guard<std::mutex> lock(g_modeTransitionMutex);
    return g_modeTransition.active ? g_modeTransition.backgroundTransition : BackgroundTransitionType::Cut;
}

std::string GetModeTransitionFromModeId() {
    std::lock_guard<std::mutex> lock(g_modeTransitionMutex);
    return g_modeTransition.active ? g_modeTransition.fromModeId : "";
}

void GetAnimatedModeViewport(int& outWidth, int& outHeight) {
    std::lock_guard<std::mutex> lock(g_modeTransitionMutex);
    if (g_modeTransition.active) {
        outWidth = g_modeTransition.currentWidth;
        outHeight = g_modeTransition.currentHeight;
    } else {
        ModeViewportInfo viewport = GetCurrentModeViewport();
        if (viewport.valid) {
            outWidth = viewport.stretchEnabled ? viewport.stretchWidth : viewport.width;
            outHeight = viewport.stretchEnabled ? viewport.stretchHeight : viewport.height;
        } else {
            outWidth = GetCachedWindowWidth();
            outHeight = GetCachedWindowHeight();
        }
    }
}

void GetAnimatedModePosition(int& outX, int& outY) {
    std::lock_guard<std::mutex> lock(g_modeTransitionMutex);
    if (g_modeTransition.active) {
        outX = g_modeTransition.currentX;
        outY = g_modeTransition.currentY;
    } else {
        ModeViewportInfo viewport = GetCurrentModeViewport();
        if (viewport.valid) {
            outX = viewport.stretchX;
            outY = viewport.stretchY;
        } else {
            int screenW = GetCachedWindowWidth();
            int screenH = GetCachedWindowHeight();
            outX = screenW / 2;
            outY = screenH / 2;
        }
    }
}

ModeTransitionState GetModeTransitionState() {
    // Lock-free read from double-buffered snapshot
    const ViewportTransitionSnapshot& snapshot =
        g_viewportTransitionSnapshots[g_viewportTransitionSnapshotIndex.load(std::memory_order_acquire)];

    ModeTransitionState state;
    state.active = snapshot.active;
    if (state.active) {
        state.width = snapshot.currentWidth;
        state.height = snapshot.currentHeight;
        state.x = snapshot.currentX;
        state.y = snapshot.currentY;
        state.gameTransition = snapshot.gameTransition;
        state.overlayTransition = snapshot.overlayTransition;
        state.backgroundTransition = snapshot.backgroundTransition;
        state.progress = snapshot.progress;
        state.moveProgress = snapshot.moveProgress;

        state.targetWidth = snapshot.toWidth;
        state.targetHeight = snapshot.toHeight;
        state.targetX = snapshot.toX;
        state.targetY = snapshot.toY;
        state.fromWidth = snapshot.fromWidth;
        state.fromHeight = snapshot.fromHeight;
        state.fromX = snapshot.fromX;
        state.fromY = snapshot.fromY;
        state.fromNativeWidth = snapshot.fromNativeWidth;
        state.fromNativeHeight = snapshot.fromNativeHeight;
        state.toNativeWidth = snapshot.toNativeWidth;
        state.toNativeHeight = snapshot.toNativeHeight;
        state.fromModeId = snapshot.fromModeId;
    } else {
        state.width = 0;
        state.height = 0;
        state.x = 0;
        state.y = 0;
        state.gameTransition = GameTransitionType::Cut;
        state.overlayTransition = OverlayTransitionType::Cut;
        state.backgroundTransition = BackgroundTransitionType::Cut;
        state.progress = 1.0f;
        state.moveProgress = 1.0f;
        state.targetWidth = 0;
        state.targetHeight = 0;
        state.targetX = 0;
        state.targetY = 0;
        state.fromWidth = 0;
        state.fromHeight = 0;
        state.fromX = 0;
        state.fromY = 0;
        state.fromNativeWidth = 0;
        state.fromNativeHeight = 0;
        state.toNativeWidth = 0;
        state.toNativeHeight = 0;
    }
    return state;
}





// NinjabrainBot Overlay

static ImFont* g_ninjabrainFont     = nullptr;
static float   g_ninjabrainFontSize = 64.0f;

ImFont* GetNinjabrainFont()     { return g_ninjabrainFont; }
float   GetNinjabrainFontSize() { return g_ninjabrainFontSize; }

static bool NB_IsFontStable(const std::string& fontPath, float /*sizePixels*/) {
    if (fontPath.empty()) return false;
    const DWORD attrs = GetFileAttributesA(fontPath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) return false;
    std::ifstream f(fontPath, std::ios::binary);
    if (!f) return false;
    unsigned char sig[4] = { 0, 0, 0, 0 };
    f.read(reinterpret_cast<char*>(sig), sizeof(sig));
    if (!f) return false;
    const unsigned char ttfSig[4] = { 0x00, 0x01, 0x00, 0x00 };
    if (memcmp(sig, ttfSig, 4) == 0) return true;
    if (memcmp(sig, "OTTO", 4) == 0) return true;
    if (memcmp(sig, "ttcf", 4) == 0) return true;
    if (memcmp(sig, "true", 4) == 0) return true;
    if (memcmp(sig, "typ1", 4) == 0) return true;
    return false;
}

static ImFont* NB_SafeAddFontFromFileTTF(ImFontAtlas* atlas, const char* path, float sizePixels,
                                         const ImFontConfig* fontCfg = nullptr) {
    if (!atlas || !path || !path[0]) return nullptr;
    ImFont* font = nullptr;
    __try {
        font = atlas->AddFontFromFileTTF(path, sizePixels, fontCfg);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        font = nullptr;
    }
    return font;
}

void LoadNinjabrainFont(ImFontAtlas* atlas, const NinjabrainOverlayConfig& overlay, float scaleFactor) {
    if (!atlas) return;
    const float overlayScale = (overlay.overlayScale > 0.01f) ? overlay.overlayScale : 1.0f;
    const float nbSize = (std::max)(1.0f, kNinjabrainOverlayBaseFontSize * overlayScale * scaleFactor);

    ImFontConfig fontCfg;
    fontCfg.OversampleH = overlay.fontAntialiasing ? 2 : 1;
    fontCfg.OversampleV = overlay.fontAntialiasing ? 2 : 1;
    fontCfg.PixelSnapH = !overlay.fontAntialiasing;

    auto resolvePath = [](const std::string& p) -> std::string { return ResolveToolscreenRelativePath(p, g_toolscreenPath); };

    auto tryFontResource = [&](int resourceId) -> bool {
        HMODULE hModule = NULL;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&LoadNinjabrainFont, &hModule);
        if (!hModule) return false;

        HRSRC hResource = FindResourceW(hModule, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
        if (!hResource) return false;

        HGLOBAL hData = LoadResource(hModule, hResource);
        DWORD dataSize = hData ? SizeofResource(hModule, hResource) : 0;
        const void* rawData = hData ? LockResource(hData) : nullptr;
        if (!rawData || dataSize == 0) return false;

        void* buffer = IM_ALLOC(dataSize);
        if (!buffer) return false;

        memcpy(buffer, rawData, dataSize);
        fontCfg.FontDataOwnedByAtlas = true;
        if (ImFont* f = atlas->AddFontFromMemoryTTF(buffer, (int)dataSize, nbSize, &fontCfg)) {
            g_ninjabrainFont = f;
            g_ninjabrainFontSize = nbSize;
            return true;
        }

        fontCfg.FontDataOwnedByAtlas = false;
        IM_FREE(buffer);
        return false;
    };

    if (!overlay.customFontPath.empty()) {
        std::string resolved = resolvePath(overlay.customFontPath);
        if (NB_IsFontStable(resolved, nbSize)) {
            if (ImFont* f = NB_SafeAddFontFromFileTTF(atlas, resolved.c_str(), nbSize, &fontCfg)) {
                g_ninjabrainFont     = f;
                g_ninjabrainFontSize = nbSize;
                return;
            }
        }
    }

    if (const BundledFontAsset* selectedBundledFont = FindBundledFontAssetByPath(overlay.customFontPath, g_toolscreenPath)) {
        if (tryFontResource(selectedBundledFont->resourceId)) return;
    }

    std::string resolvedBundledDefault = resolvePath(ConfigDefaults::CONFIG_FONT_PATH);
    if (NB_IsFontStable(resolvedBundledDefault, nbSize)) {
        if (ImFont* f = NB_SafeAddFontFromFileTTF(atlas, resolvedBundledDefault.c_str(), nbSize, &fontCfg)) {
            g_ninjabrainFont = f;
            g_ninjabrainFontSize = nbSize;
            return;
        }
    }

    if (tryFontResource(IDR_OPENSANS_FONT)) return;
    if (tryFontResource(IDR_MINECRAFT_FONT)) return;

    const std::string& fallbackFont = ConfigDefaults::CONFIG_FALLBACK_FONT_PATH;
    if (NB_IsFontStable(fallbackFont, nbSize)) {
        if (ImFont* f = NB_SafeAddFontFromFileTTF(atlas, fallbackFont.c_str(), nbSize, &fontCfg)) {
            g_ninjabrainFont = f;
            g_ninjabrainFontSize = nbSize;
            return;
        }
    }

    g_ninjabrainFont     = atlas->AddFontDefault();
    g_ninjabrainFontSize = nbSize;
}

static ImU32 ColorToImU32(const Color& color)
{
    return IM_COL32((int)(color.r * 255.0f), (int)(color.g * 255.0f), (int)(color.b * 255.0f), (int)(color.a * 255.0f));
}

static ImU32 NBGradientColor(double probability, const Color& lowColor, const Color& midColor, const Color& highColor)
{
    const float clampedProbability = (float)std::clamp(probability, 0.0, 1.0);
    const Color& startColor = (clampedProbability < 0.5f) ? lowColor : midColor;
    const Color& endColor = (clampedProbability < 0.5f) ? midColor : highColor;
    const float t = (clampedProbability < 0.5f) ? (clampedProbability * 2.0f) : ((clampedProbability - 0.5f) * 2.0f);

    const float red = startColor.r + (endColor.r - startColor.r) * t;
    const float green = startColor.g + (endColor.g - startColor.g) * t;
    const float blue = startColor.b + (endColor.b - startColor.b) * t;
    return IM_COL32((int)(red * 255.0f), (int)(green * 255.0f), (int)(blue * 255.0f), 255);
}


static bool LoadPngTextureResource(HMODULE hModule, int resourceId, GLuint& outTexture, const char* debugName)
{
    HRSRC hResource = FindResourceW(hModule, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (!hResource) {
        Log(std::string("Failed to find ") + debugName + " resource ID: " + std::to_string(resourceId));
        return false;
    }

    HGLOBAL hLoaded = LoadResource(hModule, hResource);
    if (!hLoaded) {
        Log(std::string("Failed to load ") + debugName + " resource ID: " + std::to_string(resourceId));
        return false;
    }

    void* pData = LockResource(hLoaded);
    DWORD dataSize = SizeofResource(hModule, hResource);
    if (!pData || dataSize == 0) {
        Log(std::string("Invalid ") + debugName + " resource data for ID: " + std::to_string(resourceId));
        return false;
    }

    int w = 0;
    int h = 0;
    int ch = 0;
    unsigned char* px = stbi_load_from_memory((const unsigned char*)pData, dataSize, &w, &h, &ch, 4);
    if (!px) {
        Log(std::string("Failed to decode ") + debugName + " PNG for resource ID: " + std::to_string(resourceId));
        return false;
    }

    glGenTextures(1, &outTexture);
    BindTextureDirect(GL_TEXTURE_2D, outTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    stbi_image_free(px);
    return true;
}

static void EnsureNinjabrainOverlayIconsLoaded()
{
    if (s_ninjabrainOverlayIconsLoaded) return;

    // Make sure OpenGL context is current
    if (!wglGetCurrentContext()) return;

    s_ninjabrainOverlayIconsLoaded = true;

    // Get our DLL's own module handle - resources live in the DLL, not the EXE
    HMODULE hModule = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&EnsureNinjabrainOverlayIconsLoaded, &hModule);
    if (!hModule) {
        Log("EnsureNinjabrainOverlayIconsLoaded: failed to get module handle");
        return;
    }

    PixelStoreStateGuard pixelStoreGuard;
    GLint previousTexture = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);

    // Boat icons are UI resources and should never inherit the global image-loader flip state.
    stbi_set_flip_vertically_on_load_thread(0);

    // Resource IDs for boat icons
    const int resourceIds[4] = {
        IDR_BOAT_GRAY,   // 0 - NONE/ERROR
        IDR_BOAT_BLUE,   // 1 - MEASURING
        IDR_BOAT_GREEN,  // 2 - VALID
        IDR_BOAT_RED     // 3 - ERROR
    };

    for (int i = 0; i < 4; i++) {
        if (LoadPngTextureResource(hModule, resourceIds[i], s_boatIconTex[i], "boat icon")) {
            Log("Loaded boat icon from resource ID: " + std::to_string(resourceIds[i]));
        }
    }

    const int messageResourceIds[3] = {
        IDR_NINJABRAIN_INFO_ICON,
        IDR_NINJABRAIN_WARNING_ICON,
        IDR_NINJABRAIN_LOCK_ICON,
    };
    const char* messageDebugNames[3] = {
        "Ninjabrain info icon",
        "Ninjabrain warning icon",
        "Ninjabrain lock icon",
    };
    for (int i = 0; i < 3; ++i) {
        if (LoadPngTextureResource(hModule, messageResourceIds[i], s_ninjabrainMessageIconTex[i], messageDebugNames[i])) {
            Log(std::string("Loaded ") + messageDebugNames[i] + " from resource ID: " + std::to_string(messageResourceIds[i]));
        }
    }

    BindTextureDirect(GL_TEXTURE_2D, previousTexture);
}
#include <algorithm>

void RenderNinjabrainOverlay(const NinjabrainOverlayConfig& nb, ImFont* font, const std::string& modeId,
                             bool renderBehindImGuiWindows)
{
    if (!ImGui::GetCurrentContext()) return;

    if (!IsNinjabrainOverlayModeAllowed(nb, modeId)) return;

    EnsureNinjabrainOverlayIconsLoaded();

    const auto dataSnapshot = GetNinjabrainDataSnapshot();
    if (!dataSnapshot) return;

    const NinjabrainData& data = *dataSnapshot;
    const bool blindResult = data.blind.enabled && data.blind.hasResult;
    const bool failedResult = data.resultType == "FAILED" && !blindResult;
    const bool hasInformationMessages = data.informationMessageCount > 0;

    bool hasTriangulation = false;
    bool showForBoat = false;
    if (!HasNinjabrainOverlayContent(nb, data, &hasTriangulation, &showForBoat)) return;
    const bool showEmptyState = nb.alwaysShow && !hasTriangulation && !failedResult && !blindResult &&
                                !hasInformationMessages;
    const bool showPredictionTable = hasTriangulation || showForBoat || showEmptyState;
    const bool renderBlankPredictionRows = showEmptyState && data.predictionCount <= 0;

    ImDrawList* drawList = renderBehindImGuiWindows ? ImGui::GetBackgroundDrawList() : ImGui::GetForegroundDrawList();
    const float scale = (nb.overlayScale > 0.01f) ? nb.overlayScale : 1.0f;
    const float fs = GetNinjabrainFontSize();
    if (!font || !font->IsLoaded()) font = ImGui::GetFont();
    const float lineH = fs;
    const float colGap = (std::max)(0.0f, nb.resultsColumnGap) * scale;
    const int outlineR = nb.outlineWidth;
        const float leftAlignedOutlineInset = (outlineR > 0) ? static_cast<float>(outlineR) : 0.0f;
    const float contentPadX = (8.0f + (float)outlineR) * scale;
    const float sidePadX = (std::max)(0.0f, nb.sidePadding) * scale;
    const float contentPadTop = (std::max)(0.0f, nb.contentPaddingTop) * scale;
    const float contentPadBottom = (std::max)(0.0f, nb.contentPaddingBottom) * scale;
    const float resultsMarginLeft = (std::max)(0.0f, nb.resultsMarginLeft) * scale;
    const float resultsMarginRight = (std::max)(0.0f, nb.resultsMarginRight) * scale;
    const float resultsMarginTop = (std::max)(0.0f, nb.resultsMarginTop) * scale;
    const float resultsMarginBottom = (std::max)(0.0f, nb.resultsMarginBottom) * scale;
    const float resultsHeaderPadY = (std::max)(0.0f, nb.resultsHeaderPaddingY) * scale;
    const float rowH = lineH + nb.rowSpacing * scale;
    const std::string infoPlacement =
        (nb.informationMessagesPlacement == "top" || nb.informationMessagesPlacement == "bottom")
            ? nb.informationMessagesPlacement
            : "middle";
    const float infoMarginLeft = (std::max)(0.0f, nb.informationMessagesMarginLeft) * scale;
    const float infoMarginRight = (std::max)(0.0f, nb.informationMessagesMarginRight) * scale;
    const float infoFontScale = (std::max)(0.4f, nb.informationMessagesFontScale);
    const float infoFs = fs * infoFontScale;
    const float infoLineH = (std::max)(1.0f, infoFs);
    const float throwsMarginLeft = (std::max)(0.0f, nb.throwsMarginLeft) * scale;
    const float throwsMarginRight = (std::max)(0.0f, nb.throwsMarginRight) * scale;
    const float throwsMarginTop = (std::max)(0.0f, nb.throwsMarginTop) * scale;
    const float throwsMarginBottom = (std::max)(0.0f, nb.throwsMarginBottom) * scale;
    const float throwsHeaderPadY = (std::max)(0.0f, nb.throwsHeaderPaddingY) * scale;
    const float throwsRowPadY = (std::max)(0.0f, nb.throwsRowPaddingY) * scale;
    const float failureMarginLeft = (std::max)(0.0f, nb.failureMarginLeft) * scale;
    const float failureMarginRight = (std::max)(0.0f, nb.failureMarginRight) * scale;
    const float failureMarginTop = (std::max)(0.0f, nb.failureMarginTop) * scale;
    const float failureMarginBottom = (std::max)(0.0f, nb.failureMarginBottom) * scale;
    const float failureLineGap = (std::max)(0.0f, nb.failureLineGap) * scale;
    const float blindMarginLeft = (std::max)(0.0f, nb.blindMarginLeft) * scale;
    const float blindMarginRight = (std::max)(0.0f, nb.blindMarginRight) * scale;
    const float blindMarginTop = (std::max)(0.0f, nb.blindMarginTop) * scale;
    const float blindMarginBottom = (std::max)(0.0f, nb.blindMarginBottom) * scale;
    const float blindLineGap = (std::max)(0.0f, nb.blindLineGap) * scale;
    const float summaryMarginLeft = blindResult ? blindMarginLeft : failureMarginLeft;
    const float summaryMarginRight = blindResult ? blindMarginRight : failureMarginRight;
    const float summaryMarginTop = blindResult ? blindMarginTop : failureMarginTop;
    const float summaryMarginBottom = blindResult ? blindMarginBottom : failureMarginBottom;
    const float summaryLineGap = blindResult ? blindLineGap : failureLineGap;
    const std::string* summaryAnchor = blindResult ? &nb.blindAnchor : &nb.failureAnchor;
    const float summaryOffsetX = (blindResult ? nb.blindOffsetX : nb.failureOffsetX) * scale;
    const float summaryOffsetY = (blindResult ? nb.blindOffsetY : nb.failureOffsetY) * scale;
    const int summaryDrawOrder = blindResult ? nb.blindDrawOrder : nb.failureDrawOrder;
    const int minimumThrowRows = std::clamp(nb.eyeThrowRows, 1, static_cast<int>(kNinjabrainThrowLimit));
    const float boatStateSize = (std::max)(0.0f, nb.boatStateSize) * scale;
    const float boatStateMarginRight = (std::max)(0.0f, nb.boatStateMarginRight) * scale;

    auto applyAlpha = [](ImU32 col, float alphaMul) -> ImU32 {
        if (alphaMul >= 1.0f) return col;
        if (alphaMul <= 0.0f) return col & 0x00FFFFFF;
        int a = (int)(((col >> 24) & 0xFF) * alphaMul);
        if (a < 0) a = 0;
        if (a > 255) a = 255;
        return (col & 0x00FFFFFF) | ((ImU32)a << 24);
    };
    auto colorWithAlpha = [&](const Color& color, float alphaMul) -> ImU32 {
        return applyAlpha(ColorToImU32(color), alphaMul);
    };

    auto getOutlineOffsets = [&](int radius) -> const std::vector<ImVec2>& {
        static std::unordered_map<int, std::vector<ImVec2>> s_outlineOffsetCache;

        radius = (std::max)(0, radius);
        const auto cacheIt = s_outlineOffsetCache.find(radius);
        if (cacheIt != s_outlineOffsetCache.end()) {
            return cacheIt->second;
        }

        std::set<std::pair<int, int>> uniqueOffsets;
        if (radius == 1) {
            static constexpr std::pair<int, int> kImmediateOutlineOffsets[] = {
                {-1, -1}, {0, -1}, {1, -1},
                {-1,  0},           {1,  0},
                {-1,  1}, {0,  1}, {1,  1},
            };
            for (const auto& offset : kImmediateOutlineOffsets) {
                uniqueOffsets.insert(offset);
            }
        } else if (radius > 1) {
            constexpr double kPi = 3.14159265358979323846;
            for (int ring = 1; ring <= radius; ++ring) {
                const int sampleCount = (std::max)(16, static_cast<int>(std::ceil(2.0 * kPi * ring * 1.25)));
                for (int sample = 0; sample < sampleCount; ++sample) {
                    const double angle = (2.0 * kPi * sample) / sampleCount;
                    const int offsetX = static_cast<int>(std::lround(std::cos(angle) * ring));
                    const int offsetY = static_cast<int>(std::lround(std::sin(angle) * ring));
                    if (offsetX == 0 && offsetY == 0) {
                        continue;
                    }
                    uniqueOffsets.emplace(offsetX, offsetY);
                }
            }
        }

        std::vector<ImVec2> offsets;
        offsets.reserve(uniqueOffsets.size());
        for (const auto& offset : uniqueOffsets) {
            offsets.emplace_back(static_cast<float>(offset.first), static_cast<float>(offset.second));
        }

        const auto inserted = s_outlineOffsetCache.emplace(radius, std::move(offsets));
        return inserted.first->second;
    };

    const Color bodyTextColor = (data.resultType == "DIVINE") ? nb.divineTextColor : nb.dataColor;
    ImU32 textCol = ColorToImU32(nb.textColor);
    ImU32 dataCol = ColorToImU32(bodyTextColor);
    ImU32 titleTextCol = ColorToImU32(nb.titleTextColor);
    ImU32 throwsTextCol = ColorToImU32(nb.throwsTextColor);
    ImU32 versionTextCol = ColorToImU32(nb.versionTextColor);
    ImU32 posAdjustmentCol = ColorToImU32(nb.subpixelPositiveColor);
    ImU32 negAdjustmentCol = ColorToImU32(nb.subpixelNegativeColor);
    ImU32 posCoordCol = ColorToImU32(nb.coordPositiveColor);
    ImU32 negCoordCol = ColorToImU32(nb.coordNegativeColor);

    std::string blindLine1Prefix;
    std::string blindLine1Highlight;
    std::string blindLine2Percent;
    std::string blindLine2Suffix;
    std::string blindLine3;
    ImU32 blindHighlightCol = dataCol;
    ImU32 blindProbabilityCol = dataCol;
    float blindLine1PrefixW = 0.0f;
    float blindLine2PercentW = 0.0f;
    float blindMessageW = 0.0f;
    bool showBlindImproveDirection = false;

    if (blindResult) {
        auto humanizeBlindEvaluation = [](const std::string& evaluation) {
            if (evaluation == "NOT_IN_RING") {
                return std::string(tr("ninjabrain.blind_evaluation.not_in_ring"));
            }
            if (evaluation == "BAD") {
                return std::string(tr("ninjabrain.blind_evaluation.bad"));
            }
            if (evaluation == "BAD_BUT_IN_RING") {
                return std::string(tr("ninjabrain.blind_evaluation.bad_but_in_ring"));
            }
            if (evaluation == "HIGHROLL_GOOD") {
                return std::string(tr("ninjabrain.blind_evaluation.highroll_good"));
            }
            if (evaluation == "HIGHROLL_OKAY") {
                return std::string(tr("ninjabrain.blind_evaluation.highroll_okay"));
            }
            if (evaluation == "EXCELLENT") {
                return std::string(tr("ninjabrain.blind_evaluation.excellent"));
            }
            return std::string(tr("ninjabrain.blind_evaluation.unknown"));
        };
        auto blindEvaluationGradientPosition = [](const std::string& evaluation, double& progress) {
            if (evaluation == "NOT_IN_RING") {
                progress = 0.0;
                return true;
            }
            if (evaluation == "BAD") {
                progress = 0.25;
                return true;
            }
            if (evaluation == "BAD_BUT_IN_RING") {
                progress = 0.5;
                return true;
            }
            if (evaluation == "HIGHROLL_GOOD") {
                progress = 1.0;
                return true;
            }
            if (evaluation == "HIGHROLL_OKAY") {
                progress = 0.75;
                return true;
            }
            if (evaluation == "EXCELLENT") {
                progress = 1.0;
                return true;
            }
            return false;
        };

        constexpr double kPi = 3.14159265358979323846;
        const int blindX = static_cast<int>(std::lround(data.blind.xInNether));
        const int blindZ = static_cast<int>(std::lround(data.blind.zInNether));
        const double blindProbability = std::clamp(data.blind.highrollProbability, 0.0, 1.0);
        const double blindProbabilityPercent = blindProbability * 100.0;
        const int blindThreshold = static_cast<int>(std::lround(data.blind.highrollThreshold));
        const int blindImproveHeading = static_cast<int>(std::lround(data.blind.improveDirection * 180.0 / kPi));
        const int blindImproveDistance = static_cast<int>(std::lround(data.blind.improveDistance));

        blindLine1Prefix = tr("ninjabrain.blind_coords_prefix", blindX, blindZ);
        blindLine1Prefix.push_back(' ');

        blindLine1Highlight = humanizeBlindEvaluation(data.blind.evaluation);
        showBlindImproveDirection = data.blind.evaluation != "EXCELLENT";

        char blindProbabilityText[32];
        std::snprintf(blindProbabilityText, sizeof(blindProbabilityText), "%.1f%%", blindProbabilityPercent);
        blindLine2Percent = blindProbabilityText;
        blindLine2Suffix = tr("ninjabrain.blind_highroll_probability_suffix", blindThreshold);
        if (showBlindImproveDirection) {
            blindLine3 = tr("ninjabrain.blind_improve_direction", blindImproveHeading, blindImproveDistance);
        } else {
            blindLine3.clear();
        }

        blindLine1PrefixW = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, blindLine1Prefix.c_str()).x;
        const float blindLine1W = blindLine1PrefixW + font->CalcTextSizeA(fs, FLT_MAX, 0.0f, blindLine1Highlight.c_str()).x;
        blindLine2PercentW = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, blindLine2Percent.c_str()).x;
        const float blindLine2W = blindLine2PercentW + font->CalcTextSizeA(fs, FLT_MAX, 0.0f, blindLine2Suffix.c_str()).x;
        const float blindLine3W = showBlindImproveDirection
            ? font->CalcTextSizeA(fs, FLT_MAX, 0.0f, blindLine3.c_str()).x
            : 0.0f;
        blindMessageW = (std::max)(blindLine1W, (std::max)(blindLine2W, blindLine3W));

        const double blindProbabilityColorProgress = std::clamp(blindProbabilityPercent / 10.0, 0.0, 1.0);
        blindProbabilityCol = NBGradientColor(blindProbabilityColorProgress, nb.certaintyLowColor, nb.certaintyMidColor,
                                              nb.certaintyColor);

        double blindEvaluationProgress = 0.0;
        if (blindEvaluationGradientPosition(data.blind.evaluation, blindEvaluationProgress)) {
            blindHighlightCol = NBGradientColor(blindEvaluationProgress, nb.certaintyLowColor, nb.certaintyMidColor,
                                                nb.certaintyColor);
        }
    }

    int boatIconIdx = 0;
    if      (data.boatState == "MEASURING") boatIconIdx = 1;
    else if (data.boatState == "VALID")     boatIconIdx = 2;
    else if (data.boatState == "ERROR")     boatIconIdx = 3;
    GLuint boatTex = s_boatIconTex[boatIconIdx];
    const bool hasBoatState = showForBoat || data.boatState != "NONE";
    const bool showBoatHeaderIcon = nb.showBoatStateInTopBar && hasBoatState;

    struct SegRow {
        char  text[80];
        ImU32 color;
        ImU32 part1Color;
        float xOffset;
        bool  centerOnPart1;
    };
    static constexpr int kRenderedRowLimit = (int)kNinjabrainPredictionLimit + 1;
    struct Col {
        char   header[32];
        SegRow rows[kRenderedRowLimit];
        float  width;
        int    rowCount;
        bool   isBoatIcon;
        bool   leftAlign;
    };
    Col cols[10];
    int numCols = 0;
    int numRows = nb.shownPredictions;
    if (numRows < 1) numRows = 1;
    if (numRows > (int)kNinjabrainPredictionLimit) numRows = (int)kNinjabrainPredictionLimit;
    if (!renderBlankPredictionRows && data.predictionCount > 0 && numRows > data.predictionCount) numRows = data.predictionCount;
    const bool boatOnly = !hasTriangulation && showForBoat && !showEmptyState;
    if (boatOnly) numRows = 1;
    const int populatedPredictionRows = (renderBlankPredictionRows || boatOnly)
        ? 0
        : (std::min)(numRows, data.predictionCount);

    auto initCol = [&](Col& c, const char* hdr, int rows) {
        c.rowCount   = rows;
        c.isBoatIcon = false;
        c.leftAlign  = false;
        snprintf(c.header, sizeof(c.header), "%s", hdr);
        c.width = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, c.header).x;
        for (int i = 0; i < kRenderedRowLimit; i++) {
            c.rows[i].text[0]    = 0;
            c.rows[i].color      = dataCol;
            c.rows[i].part1Color = textCol;
            c.rows[i].xOffset    = 0;
            c.rows[i].centerOnPart1 = false;
        }
    };
    auto measureRow = [&](Col& c, int ri) {
        float rowWidth = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, c.rows[ri].text).x;
        if (c.leftAlign && rowWidth > 0.0f) {
            rowWidth += leftAlignedOutlineInset;
        }
        c.width = (std::max)(c.width, rowWidth);
    };
    auto reserveStaticColWidth = [&](Col& c, const NinjabrainColumn& colCfg) {
        if (!nb.staticColumnWidths) return;

        auto reserveSampleWidth = [&](const char* sample) {
            float sampleWidth = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, sample).x;
            if (c.leftAlign && sampleWidth > 0.0f) {
                sampleWidth += leftAlignedOutlineInset;
            }
            c.width = (std::max)(c.width, sampleWidth);
        };

        if (colCfg.staticWidth > 0) {
            c.width = static_cast<float>(colCfg.staticWidth) * scale;
            return;
        }

        const std::string& colId = colCfg.id;

        if (colId == "coords") {
            reserveSampleWidth((nb.coordsDisplay == "chunk") ? "(-999, -999)" : "(-99999, -99999)");
        } else if (colId == "certainty") {
            reserveSampleWidth("100.00%");
        } else if (colId == "distance") {
            reserveSampleWidth("999999");
        } else if (colId == "nether") {
            reserveSampleWidth("(-99999, -99999)");
        } else if (colId == "angle") {
            if (nb.showDirectionToStronghold) {
                reserveSampleWidth("-359.99 (-> 359.0)");
            } else {
                reserveSampleWidth("-359.99");
            }
            if (!nb.showThrowDetails) {
                reserveSampleWidth("-359.99+999");
            }
        }
    };
    auto getCoordsDisplay = [&](int chunkX, int chunkZ, int& displayX, int& displayZ) {
        if (nb.coordsDisplay == "chunk") {
            displayX = chunkX;
            displayZ = chunkZ;
            return;
        }
        displayX = chunkX * 16 + 4;
        displayZ = chunkZ * 16 + 4;
    };
    auto getNetherDisplay = [&](int chunkX, int chunkZ, int& displayX, int& displayZ) {
        displayX = chunkX * 2;
        displayZ = chunkZ * 2;
    };

    if (!failedResult && showPredictionTable) {
        for (const auto& colCfg : nb.columns) {
            if (!colCfg.show) continue;
            if (numCols >= 10) break;
            if (colCfg.id == "angle_change" || colCfg.id == "eyes") continue;
            if (colCfg.id == "boat") continue;
            if (boatOnly) continue;

            Col& c = cols[numCols];

            if (colCfg.id == "coords") {
                initCol(c, colCfg.header.c_str(), numRows);
                for (int i = 0; i < numRows; i++) {
                    if (i >= populatedPredictionRows) {
                        continue;
                    }
                    int rawX = 0;
                    int rawZ = 0;
                    getCoordsDisplay(data.predictions[i].chunkX, data.predictions[i].chunkZ, rawX, rawZ);
                    snprintf(c.rows[i].text, sizeof(c.rows[i].text), "(%d, %d)", rawX, rawZ);
                    if ((rawX < 0) != (rawZ < 0)) {
                        char part1[32];
                        snprintf(part1, sizeof(part1), "(%d, ", rawX);
                        c.rows[i].xOffset    = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, part1).x;
                        c.rows[i].part1Color = (rawX < 0) ? negCoordCol : posCoordCol;
                        c.rows[i].color      = (rawZ < 0) ? negCoordCol : posCoordCol;
                    } else {
                        const bool anyNeg = rawX < 0 || rawZ < 0;
                        c.rows[i].color   = anyNeg ? negCoordCol : posCoordCol;
                        c.rows[i].xOffset = 0;
                    }
                    measureRow(c, i);
                }
            } else if (colCfg.id == "certainty") {
                initCol(c, colCfg.header.c_str(), numRows);
                for (int i = 0; i < numRows; i++) {
                    if (i >= populatedPredictionRows) {
                        continue;
                    }
                    snprintf(c.rows[i].text, sizeof(c.rows[i].text), "%.1f%%", data.predictions[i].certainty * 100.0);
                    c.rows[i].color = NBGradientColor(data.predictions[i].certainty, nb.certaintyLowColor,
                                                     nb.certaintyMidColor, nb.certaintyColor);
                    measureRow(c, i);
                }
            } else if (colCfg.id == "distance") {
                initCol(c, colCfg.header.c_str(), numRows);
                for (int i = 0; i < numRows; i++) {
                    if (i >= populatedPredictionRows) {
                        continue;
                    }
                    const double distanceValue = GetNinjabrainPredictionDisplayDistance(data, data.predictions[i]);
                    const int displayDistance = (int)std::floor(distanceValue);
                    snprintf(c.rows[i].text, sizeof(c.rows[i].text), "%d", displayDistance);
                    c.rows[i].color = dataCol;
                    measureRow(c, i);
                }
            } else if (colCfg.id == "nether") {
                initCol(c, colCfg.header.c_str(), numRows);
                for (int i = 0; i < numRows; i++) {
                    if (i >= populatedPredictionRows) {
                        continue;
                    }
                    int nx = 0;
                    int nz = 0;
                    getNetherDisplay(data.predictions[i].chunkX, data.predictions[i].chunkZ, nx, nz);
                    snprintf(c.rows[i].text, sizeof(c.rows[i].text), "(%d, %d)", nx, nz);
                    if ((nx < 0) != (nz < 0)) {
                        char part1[32];
                        snprintf(part1, sizeof(part1), "(%d, ", nx);
                        c.rows[i].xOffset    = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, part1).x;
                        c.rows[i].part1Color = (nx < 0) ? negCoordCol : posCoordCol;
                        c.rows[i].color      = (nz < 0) ? negCoordCol : posCoordCol;
                    } else {
                        const bool anyNeg = nx < 0 || nz < 0;
                        c.rows[i].color   = anyNeg ? negCoordCol : posCoordCol;
                        c.rows[i].xOffset = 0;
                    }
                    measureRow(c, i);
                }
            } else if (colCfg.id == "angle") {
                c.leftAlign = true;

                if (renderBlankPredictionRows) {
                    initCol(c, colCfg.header.c_str(), numRows);
                    c.leftAlign = true;
                } else
                if (data.eyeCount == 0) {
                    initCol(c, colCfg.header.c_str(), 1);
                    c.leftAlign = true;
                    snprintf(c.rows[0].text, sizeof(c.rows[0].text), "-");
                    c.rows[0].color = textCol;
                    measureRow(c, 0);
                } else {
                    int totalRows = numRows + (nb.showThrowDetails ? 0 : 1);
                    if (totalRows > kRenderedRowLimit) totalRows = kRenderedRowLimit;
                    initCol(c, colCfg.header.c_str(), totalRows);
                    c.leftAlign = true;

                    for (int i = 0; i < numRows; i++) {
                        if (data.predictionAngles[i].valid) {
                            double nc    = data.predictionAngles[i].neededCorrection;
                            double absNc = std::abs(nc);
                            if (!nb.showDirectionToStronghold) {
                                snprintf(c.rows[i].text, sizeof(c.rows[i].text),
                                         "%.2f", data.predictionAngles[i].actualAngle);
                                c.rows[i].color   = dataCol;
                                c.rows[i].xOffset = 0;
                            } else {
                                const char* arrow = (nc > 0) ? "-> " : "<- ";
                                char part1[32];
                                snprintf(part1, sizeof(part1), "%.2f ", data.predictionAngles[i].actualAngle);
                                c.rows[i].xOffset    = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, part1).x;
                                c.rows[i].part1Color = dataCol;
                                c.rows[i].centerOnPart1 = true;
                                snprintf(c.rows[i].text, sizeof(c.rows[i].text),
                                         "%.2f (%s%.1f)", data.predictionAngles[i].actualAngle, arrow, absNc);
                                c.rows[i].color = NBGradientColor(1.0 - absNc / 180.0, nb.certaintyLowColor,
                                                                 nb.certaintyMidColor, nb.certaintyColor);
                            }
                        } else {
                            snprintf(c.rows[i].text, sizeof(c.rows[i].text), "%.2f", data.lastAngle);
                            c.rows[i].color   = dataCol;
                            c.rows[i].xOffset = 0;
                        }
                        measureRow(c, i);
                    }

                    if (!nb.showThrowDetails) {
                        int infoIdx = numRows;
                        auto& ft = data.throws[data.eyeCount - 1];

                        int correctionIncrements = 0;
                        if (ft.hasCorrectionIncrements) {
                            correctionIncrements = ft.correctionIncrements;
                        } else {
                            correctionIncrements = data.correctionIncrements151;
                        }

                        if (correctionIncrements == 0) {
                            snprintf(c.rows[infoIdx].text, sizeof(c.rows[infoIdx].text), "%.2f", ft.angleWithoutCorrection);
                            c.rows[infoIdx].color = textCol;
                            c.rows[infoIdx].xOffset = 0;
                        } else {
                            char basePart[32];
                            snprintf(basePart, sizeof(basePart), "%.2f", ft.angleWithoutCorrection);
                            c.rows[infoIdx].xOffset = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, basePart).x;
                            c.rows[infoIdx].part1Color = textCol;
                            c.rows[infoIdx].centerOnPart1 = true;
                            snprintf(c.rows[infoIdx].text, sizeof(c.rows[infoIdx].text), "%.2f%+d", ft.angleWithoutCorrection,
                                     correctionIncrements);
                            c.rows[infoIdx].color = (correctionIncrements > 0) ? posAdjustmentCol : negAdjustmentCol;
                        }
                        measureRow(c, infoIdx);
                    }
                }
            } else {
                continue;
            }
            reserveStaticColWidth(c, colCfg);
            numCols++;
        }
    }

    if (numCols == 0 && !failedResult && !hasInformationMessages && !showBoatHeaderIcon) return;

    int maxRows = 0;
    for (int ci = 0; ci < numCols; ci++) maxRows = (std::max)(maxRows, cols[ci].rowCount);

    int sw = GetCachedWindowWidth();
    int sh = GetCachedWindowHeight();
    const auto& geo = g_lastFrameGeometry;

    const float opacityMul = (nb.overlayOpacity < 1.0f) ? nb.overlayOpacity : 1.0f;
    auto applyOpacity = [&](ImU32 col) -> ImU32 { return applyAlpha(col, opacityMul); };

    auto drawText = [&](float sz, ImVec2 pos, ImU32 col, const char* txt) {
        col = applyOpacity(col);
        pos.x = roundf(pos.x);
        pos.y = roundf(pos.y);
        if (outlineR > 0) {
            ImU32 outCol = applyOpacity(ColorToImU32(nb.outlineColor));
            const std::vector<ImVec2>& outlineOffsets = getOutlineOffsets(outlineR);
            for (const ImVec2& offset : outlineOffsets) {
                drawList->AddText(font, sz, ImVec2(pos.x + offset.x, pos.y + offset.y), outCol, txt);
            }
        }
        drawList->AddText(font, sz, pos, col, txt);
    };
    auto drawFailureSummaryBlock = [&](float messageX, float messageTop) {
        if (blindResult) {
            drawText(fs, ImVec2(messageX, messageTop), dataCol, blindLine1Prefix.c_str());
            drawText(fs, ImVec2(messageX + blindLine1PrefixW, messageTop), blindHighlightCol, blindLine1Highlight.c_str());
            drawText(fs, ImVec2(messageX, messageTop + lineH + summaryLineGap), blindProbabilityCol, blindLine2Percent.c_str());
            drawText(fs, ImVec2(messageX + blindLine2PercentW, messageTop + lineH + summaryLineGap), dataCol,
                     blindLine2Suffix.c_str());
            if (showBlindImproveDirection) {
                drawText(fs, ImVec2(messageX, messageTop + (lineH + summaryLineGap) * 2.0f), dataCol, blindLine3.c_str());
            }
            return;
        }

        drawText(fs, ImVec2(messageX, messageTop), dataCol, trc("ninjabrain.failed_line1"));
        drawText(fs, ImVec2(messageX, messageTop + lineH + summaryLineGap), dataCol, trc("ninjabrain.failed_line2"));
    };
    auto touchesPanelEdge = [](float edge, float panelEdge) {
        return std::fabs(edge - panelEdge) <= 0.5f;
    };
    auto roundedBandFlags = [](bool roundTop, bool roundBottom) {
        ImDrawFlags flags = ImDrawFlags_None;
        if (roundTop) {
            flags |= ImDrawFlags_RoundCornersTop;
        }
        if (roundBottom) {
            flags |= ImDrawFlags_RoundCornersBottom;
        }
        return flags;
    };
    auto drawRoundedBand = [&](ImVec2 min, ImVec2 max, ImU32 color, bool roundTop, bool roundBottom) {
        const ImDrawFlags flags = roundedBandFlags(roundTop, roundBottom);
        const float radius = (flags != ImDrawFlags_None) ? nb.cornerRadius : 0.0f;
        drawList->AddRectFilled(min, max, color, radius, flags);
    };
    auto drawOverlayBackground = [&](float left, float top, float width, float height, ImU32 color) {
        if (!nb.bgEnabled) {
            return;
        }
        drawList->AddRectFilled(ImVec2(left, top), ImVec2(left + width, top + height), color, nb.cornerRadius);
    };
    auto drawOverlayBackgroundGaps = [&](float left, float top, float width, float height, ImU32 color,
                                         std::vector<std::pair<float, float>> coveredBands) {
        if (!nb.bgEnabled) {
            return;
        }
        if (coveredBands.empty()) {
            drawOverlayBackground(left, top, width, height, color);
            return;
        }

        const float panelTop = top;
        const float panelBottom = top + height;
        std::sort(coveredBands.begin(), coveredBands.end(), [](const auto& a, const auto& b) {
            return a.first < b.first;
        });

        float cursor = panelTop;
        for (const auto& coveredBand : coveredBands) {
            const float coveredTop = (std::max)(panelTop, coveredBand.first);
            const float coveredBottom = (std::min)(panelBottom, coveredBand.second);
            if (coveredBottom <= coveredTop) {
                continue;
            }
            if (coveredTop > cursor) {
                drawRoundedBand(ImVec2(left, cursor), ImVec2(left + width, coveredTop), color,
                                touchesPanelEdge(cursor, panelTop), touchesPanelEdge(coveredTop, panelBottom));
            }
            cursor = (std::max)(cursor, coveredBottom);
        }

        if (cursor < panelBottom) {
            drawRoundedBand(ImVec2(left, cursor), ImVec2(left + width, panelBottom), color,
                            touchesPanelEdge(cursor, panelTop), true);
        }
    };
    auto drawOverlayBorder = [&](float left, float top, float width, float height, ImU32 color) {
        if (nb.borderWidth <= 0) {
            return;
        }
        const float borderThickness = static_cast<float>(nb.borderWidth);
        const float halfBorder = borderThickness * 0.5f;
        const float borderRadius = (std::max)(0.0f, nb.borderRadius);
        const float lineRadius = (std::max)(0.0f, borderRadius - halfBorder);
        drawList->AddRect(ImVec2(left - halfBorder, top - halfBorder),
                          ImVec2(left + width + halfBorder, top + height + halfBorder),
                          color, lineRadius, 0, borderThickness);
    };
    auto calculateRoundedHorizontalInset = [&](float panelLeft, float panelTop, float panelRight, float panelBottom,
                                               float y, float thickness) {
        const float panelWidth = panelRight - panelLeft;
        const float panelHeight = panelBottom - panelTop;
        float radius = (std::max)(0.0f, nb.cornerRadius);
        radius = (std::min)(radius, panelWidth * 0.5f);
        radius = (std::min)(radius, panelHeight * 0.5f);
        if (radius <= 0.0f) {
            return 0.0f;
        }

        const float halfThickness = (std::max)(0.0f, thickness * 0.5f);
        float sampleTop = panelTop + halfThickness;
        float sampleBottom = panelBottom - halfThickness;
        if (sampleBottom < sampleTop) {
            const float panelCenterY = panelTop + panelHeight * 0.5f;
            sampleTop = panelCenterY;
            sampleBottom = panelCenterY;
        }

        const float sampleY = std::clamp(y, sampleTop, sampleBottom);
        float inset = 0.0f;
        auto applyCornerInset = [&](float cornerCenterY) {
            const float deltaY = std::fabs(sampleY - cornerCenterY);
            if (deltaY >= radius) {
                return;
            }
            const float xExtent = std::sqrt((std::max)(0.0f, radius * radius - deltaY * deltaY));
            inset = (std::max)(inset, radius - xExtent);
        };

        if (sampleY < panelTop + radius) {
            applyCornerInset(panelTop + radius);
        }
        if (sampleY > panelBottom - radius) {
            applyCornerInset(panelBottom - radius);
        }
        return inset;
    };
    auto drawClippedHorizontalLine = [&](float panelLeft, float panelTop, float panelRight, float panelBottom, float y, ImU32 color) {
        const float thickness = 1.0f;
        const float inset = calculateRoundedHorizontalInset(panelLeft, panelTop, panelRight, panelBottom, y, thickness);
        const float clippedLeft = panelLeft + inset;
        const float clippedRight = panelRight - inset;
        if (clippedRight <= clippedLeft) {
            return;
        }
        drawList->AddLine(ImVec2(clippedLeft, y), ImVec2(clippedRight, y), color, thickness);
    };
    auto drawCenteredSegmentedText = [&](float left, float width, float y, const char* text, ImU32 color,
                                         float splitOffset, ImU32 part1Color, bool centerOnPart1, bool leftAlign) {
        drawList->PushClipRect(ImVec2(left, -FLT_MAX), ImVec2(left + width, FLT_MAX), true);
        float textW = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, text).x;
        float anchorW = centerOnPart1 && splitOffset > 0.0f ? splitOffset : textW;
        float textX = leftAlign ? left + leftAlignedOutlineInset : left + (width - anchorW) * 0.5f;
        if (splitOffset <= 0.0f) {
            drawText(fs, ImVec2(textX, y), color, text);
            drawList->PopClipRect();
            return;
        }

        const char* splitPtr = text;
        while (*splitPtr) {
            const char* next = splitPtr + 1;
            while ((*next & 0xC0) == 0x80) ++next;
            float measuredWidth = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, text, splitPtr).x;
            if (measuredWidth >= splitOffset) break;
            splitPtr = next;
        }

        char part1[64] = {};
        int part1Length = (int)(splitPtr - text);
        if (part1Length > 63) part1Length = 63;
        memcpy(part1, text, part1Length);
        drawText(fs, ImVec2(textX, y), part1Color, part1);

        float part1W = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, part1).x;
        drawText(fs, ImVec2(textX + part1W, y), color, splitPtr);
        drawList->PopClipRect();
    };
    auto drawBoatHeaderStateIcon = [&](float sectionRight, float headerTop, float headerHeight, float rightInset) {
        if (!showBoatHeaderIcon || boatTex == 0 || headerHeight <= 0.0f) {
            return;
        }

        const float maxIconSize = (std::max)(0.0f, headerHeight);
        if (maxIconSize <= 0.0f) {
            return;
        }

        const float iconSize = (boatStateSize > 0.0f) ? (std::min)(boatStateSize, maxIconSize) : maxIconSize;
        const float iconX = sectionRight - rightInset - boatStateMarginRight - iconSize;
        const float iconY = headerTop + (maxIconSize - iconSize) * 0.5f;
        const ImU32 iconTint = applyOpacity(IM_COL32(255, 255, 255, 255));
        drawList->AddImage((ImTextureID)(intptr_t)boatTex,
                           ImVec2(iconX, iconY),
                           ImVec2(iconX + iconSize, iconY + iconSize),
                           ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), iconTint);
    };
    auto containsAsciiCaseInsensitive = [](const std::string& value, const char* needle) {
        if (!needle || !needle[0]) { return false; }
        const size_t needleLen = strlen(needle);
        return std::search(value.begin(), value.end(), needle, needle + needleLen,
                           [](char left, char right) {
                               return std::tolower(static_cast<unsigned char>(left)) ==
                                      std::tolower(static_cast<unsigned char>(right));
                           }) != value.end();
    };
    const float infoPreferredContentW = (std::max)(120.0f, nb.informationMessagesMinWidth) * scale;
    const float infoMessageMarginTop = (std::max)(0.0f, nb.informationMessagesMarginTop) * scale;
    const float infoMessageMarginBottom = (std::max)(0.0f, nb.informationMessagesMarginBottom) * scale;
    const float infoIconTextMargin = (std::max)(0.0f, nb.informationMessagesIconTextMargin) * scale;
    const float infoLineGap = 2.0f * scale * infoFontScale;
    const float infoIconSize = infoLineH * 0.82f * std::clamp(nb.informationMessagesIconScale, 0.25f, 4.0f);
    const float infoTextInset = infoIconSize + infoIconTextMargin;
    std::array<NinjabrainFormattedInformationMessage, kNinjabrainInformationMessageLimit> formattedInformationMessages{};
    for (int index = 0; index < data.informationMessageCount; ++index) {
        formattedInformationMessages[index] = FormatNinjabrainInformationMessage(data.informationMessages[index]);
    }

    auto isAsciiSpace = [](char ch) {
        return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
    };
    auto nextUtf8Char = [](const char* cursor, const char* end) {
        if (cursor >= end) { return end; }
        const char* next = cursor + 1;
        while (next < end && ((*next & 0xC0) == 0x80)) { ++next; }
        return next;
    };
    struct WrappedInfoTextLine {
        size_t startOffset = 0;
        size_t endOffset = 0;
        float width = 0.0f;
    };
    auto buildWrappedInfoTextLines = [&](const std::string& text, float wrapWidth) {
        std::vector<WrappedInfoTextLine> lines;
        const char* cursor = text.c_str();
        const char* end = cursor + text.size();

        while (cursor < end) {
            while (cursor < end && isAsciiSpace(*cursor)) { ++cursor; }
            if (cursor >= end) { break; }

            const char* wrap = font->CalcWordWrapPosition(infoFs, cursor, end, wrapWidth);
            if (wrap == cursor) { wrap = nextUtf8Char(cursor, end); }

            const char* lineEnd = wrap;
            while (lineEnd > cursor && isAsciiSpace(lineEnd[-1])) { --lineEnd; }

            lines.push_back({
                static_cast<size_t>(cursor - text.c_str()),
                static_cast<size_t>(lineEnd - text.c_str()),
                font->CalcTextSizeA(infoFs, FLT_MAX, 0.0f, cursor, lineEnd).x,
            });
            cursor = wrap;
        }

        if (lines.empty()) {
            lines.push_back({0, 0, 0.0f});
        }

        return lines;
    };
    auto measureWrappedText = [&](const NinjabrainFormattedInformationMessage& message, float wrapWidth,
                                  float* outMaxLineWidth = nullptr) {
        const auto lines = buildWrappedInfoTextLines(message.plainText, wrapWidth);
        float maxLineWidth = 0.0f;
        for (const WrappedInfoTextLine& line : lines) {
            maxLineWidth = (std::max)(maxLineWidth, line.width);
        }
        if (outMaxLineWidth) { *outMaxLineWidth = maxLineWidth; }
        return static_cast<float>(lines.size()) * infoLineH +
               static_cast<float>((std::max)(0, static_cast<int>(lines.size()) - 1)) * infoLineGap;
    };
    auto drawWrappedText = [&](const NinjabrainFormattedInformationMessage& message, float textX, float textY,
                               float wrapWidth, ImU32 defaultColor) {
        const auto lines = buildWrappedInfoTextLines(message.plainText, wrapWidth);
        float lineY = textY;
        for (const WrappedInfoTextLine& line : lines) {
            float lineX = textX;
            size_t runStartOffset = 0;
            for (const NinjabrainInformationTextRun& run : message.runs) {
                const size_t runEndOffset = runStartOffset + run.text.size();
                if (runEndOffset <= line.startOffset) {
                    runStartOffset = runEndOffset;
                    continue;
                }
                if (runStartOffset >= line.endOffset) {
                    break;
                }

                const size_t localStart = (std::max)(line.startOffset, runStartOffset) - runStartOffset;
                const size_t localEnd = (std::min)(line.endOffset, runEndOffset) - runStartOffset;
                if (localEnd > localStart) {
                    const std::string part = run.text.substr(localStart, localEnd - localStart);
                    if (!part.empty()) {
                        ImU32 partColor = defaultColor;
                        if (run.hasColor) {
                            partColor = IM_COL32(
                                (run.colorRgb >> 16) & 0xFF,
                                (run.colorRgb >> 8) & 0xFF,
                                run.colorRgb & 0xFF,
                                255);
                        }

                        drawText(infoFs, ImVec2(lineX, lineY), partColor, part.c_str());
                        lineX += font->CalcTextSizeA(infoFs, FLT_MAX, 0.0f, part.c_str()).x;
                    }
                }

                runStartOffset = runEndOffset;
            }

            lineY += infoLineH + infoLineGap;
        }
    };
    auto measureInfoMessagesBlock = [&](float contentWidth) {
        if (!hasInformationMessages) { return 0.0f; }

        const float wrapWidth = (std::max)(1.0f, contentWidth - infoTextInset);
        float totalHeight = infoMessageMarginTop + infoMessageMarginBottom;
        for (int index = 0; index < data.informationMessageCount; ++index) {
            const float messageTextHeight = measureWrappedText(formattedInformationMessages[index], wrapWidth);
            totalHeight += (std::max)(messageTextHeight, infoIconSize);
        }
        return totalHeight;
    };
    auto drawInfoMessageIcon = [&](const NinjabrainInformationMessage& message, float centerX, float centerY) {
        const float halfSize = infoIconSize * 0.5f;
        const float iconTextSize = infoFs * 0.72f;

        GLuint iconTexture = 0;
        if (containsAsciiCaseInsensitive(message.type, "lock")) {
            iconTexture = s_ninjabrainMessageIconTex[2];
        } else if (message.severity == "WARNING" || message.severity == "ERROR") {
            iconTexture = s_ninjabrainMessageIconTex[1];
        } else {
            iconTexture = s_ninjabrainMessageIconTex[0];
        }

        if (iconTexture != 0) {
            drawList->AddImage((ImTextureID)(intptr_t)iconTexture,
                               ImVec2(centerX - halfSize, centerY - halfSize),
                               ImVec2(centerX + halfSize, centerY + halfSize),
                               ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), applyOpacity(IM_COL32(255, 255, 255, 255)));
            return;
        }

        if (message.severity == "WARNING") {
            const ImU32 triangleCol = applyOpacity(IM_COL32(232, 192, 57, 255));
            const ImU32 iconTextCol = applyOpacity(IM_COL32(36, 33, 22, 255));
            drawList->AddTriangleFilled(
                ImVec2(centerX, centerY - halfSize * 1.05f),
                ImVec2(centerX - halfSize, centerY + halfSize * 0.9f),
                ImVec2(centerX + halfSize, centerY + halfSize * 0.9f),
                triangleCol);
            const float textW = font->CalcTextSizeA(iconTextSize, FLT_MAX, 0.0f, "!").x;
            drawText(iconTextSize, ImVec2(centerX - textW * 0.5f, centerY - iconTextSize * 0.62f), iconTextCol, "!");
            return;
        }

        ImU32 fillCol = applyOpacity(IM_COL32(63, 168, 244, 255));
        ImU32 iconTextCol = applyOpacity(IM_COL32(255, 255, 255, 255));
        if (message.severity == "ERROR") {
            fillCol = applyOpacity(IM_COL32(224, 89, 89, 255));
        }

        drawList->AddCircleFilled(ImVec2(centerX, centerY), halfSize, fillCol, 20);
        const float textW = font->CalcTextSizeA(iconTextSize, FLT_MAX, 0.0f, "i").x;
        drawText(iconTextSize, ImVec2(centerX - textW * 0.5f, centerY - iconTextSize * 0.58f), iconTextCol, "i");
    };
    auto drawInfoMessagesBlock = [&](float surfaceLeft, float surfaceRight, float contentLeft, float top, float contentWidth,
                                     ImU32 dividerColor, ImU32 messageColor) {
        if (!hasInformationMessages) { return top; }

        const float wrapWidth = (std::max)(1.0f, contentWidth - infoTextInset);
        const float iconCenterX = contentLeft + infoIconSize * 0.5f;
        const float messageTextX = contentLeft + infoTextInset;
        float y = top + infoMessageMarginTop;
        for (int index = 0; index < data.informationMessageCount; ++index) {
            const auto& message = data.informationMessages[index];
            const auto& formattedMessage = formattedInformationMessages[index];
            const float messageTextHeight = measureWrappedText(formattedMessage, wrapWidth);
            const float messageHeight = (std::max)(messageTextHeight, infoIconSize);
            const float rowCenterY = y + messageHeight * 0.5f;
            drawInfoMessageIcon(message, iconCenterX, rowCenterY);

            float textY = y + (messageHeight - messageTextHeight) * 0.5f;
            drawWrappedText(formattedMessage, messageTextX, textY, wrapWidth, messageColor);

            y += messageHeight;
            if (index + 1 < data.informationMessageCount) {
                drawList->AddLine(ImVec2(surfaceLeft, y), ImVec2(surfaceRight, y), dividerColor, 1.0f);
            }
        }
        return y + infoMessageMarginBottom;
    };

    auto calculateOrigin = [&](float totalW, float totalH, float& ox, float& oy) {
        int oxI = 0;
        int oyI = 0;
        GetRelativeCoordsForImageWithViewport(nb.relativeTo, nb.x, nb.y, (int)totalW, (int)totalH, geo.finalX, geo.finalY,
                                             geo.finalW, geo.finalH, sw, sh, oxI, oyI);
        ox = (float)oxI;
        oy = (float)oyI;
    };
    const bool useManualSectionLayout = nb.sectionLayoutMode == "manual";

    enum class NinjabrainManualAnchor {
        TopLeft,
        TopRight,
        BottomLeft,
        BottomRight,
    };
    auto parseManualAnchor = [](const std::string& anchor) {
        if (anchor == "topRight") { return NinjabrainManualAnchor::TopRight; }
        if (anchor == "bottomLeft") { return NinjabrainManualAnchor::BottomLeft; }
        if (anchor == "bottomRight") { return NinjabrainManualAnchor::BottomRight; }
        return NinjabrainManualAnchor::TopLeft;
    };
    auto isRightManualAnchor = [](NinjabrainManualAnchor anchor) {
        return anchor == NinjabrainManualAnchor::TopRight || anchor == NinjabrainManualAnchor::BottomRight;
    };
    auto isBottomManualAnchor = [](NinjabrainManualAnchor anchor) {
        return anchor == NinjabrainManualAnchor::BottomLeft || anchor == NinjabrainManualAnchor::BottomRight;
    };
    struct NinjabrainManualSectionPlacement {
        NinjabrainManualAnchor anchor = NinjabrainManualAnchor::TopLeft;
        float offsetX = 0.0f;
        float offsetY = 0.0f;
        int drawOrder = 0;
        float width = 0.0f;
        float height = 0.0f;
    };
    auto computeManualContentSize = [&](const std::vector<NinjabrainManualSectionPlacement>& sections, float& outW, float& outH) {
        float leftWidth = 0.0f;
        float rightWidth = 0.0f;
        float topHeight = 0.0f;
        float bottomHeight = 0.0f;
        for (const auto& section : sections) {
            if (section.width <= 0.0f || section.height <= 0.0f) { continue; }
            const float horizontalExtent = section.offsetX + section.width;
            const float verticalExtent = section.offsetY + section.height;
            if (isRightManualAnchor(section.anchor)) {
                rightWidth = (std::max)(rightWidth, horizontalExtent);
            } else {
                leftWidth = (std::max)(leftWidth, horizontalExtent);
            }
            if (isBottomManualAnchor(section.anchor)) {
                bottomHeight = (std::max)(bottomHeight, verticalExtent);
            } else {
                topHeight = (std::max)(topHeight, verticalExtent);
            }
        }
        outW = leftWidth + rightWidth;
        outH = topHeight + bottomHeight;
    };
    auto computeManualSectionOrigin = [&](const NinjabrainManualSectionPlacement& section, float contentLeft, float contentTop,
                                          float contentWidth, float contentHeight, float& outX, float& outY) {
        const float localX = isRightManualAnchor(section.anchor)
            ? (contentWidth - section.offsetX - section.width)
            : section.offsetX;
        const float localY = isBottomManualAnchor(section.anchor)
            ? (contentHeight - section.offsetY - section.height)
            : section.offsetY;
        outX = contentLeft + localX;
        outY = contentTop + localY;
    };

    struct ThrowDetailRow {
        char x[32];
        char z[32];
        char angle[32];
        char error[32];
        ImU32 angleColor;
        ImU32 anglePart1Color;
        float angleSplitOffset;
        bool angleCenterOnPart1;
    };

    auto buildThrowDetailRow = [&](const NinjabrainThrow& throwData, int correctionIncrements, ThrowDetailRow& row) {
        if (throwData.hasPosition) {
            snprintf(row.x, sizeof(row.x), "%.2f", throwData.xInOverworld);
            snprintf(row.z, sizeof(row.z), "%.2f", throwData.zInOverworld);
        } else {
            snprintf(row.x, sizeof(row.x), "-");
            snprintf(row.z, sizeof(row.z), "-");
        }

        row.angleColor = throwsTextCol;
        row.anglePart1Color = throwsTextCol;
        row.angleSplitOffset = 0.0f;
        row.angleCenterOnPart1 = false;
        if (correctionIncrements == 0) {
            snprintf(row.angle, sizeof(row.angle), "%.2f", throwData.angleWithoutCorrection);
        } else {
            char basePart[32];
            snprintf(basePart, sizeof(basePart), "%.2f", throwData.angleWithoutCorrection);
            row.angleSplitOffset = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, basePart).x;
            row.anglePart1Color = throwsTextCol;
            row.angleCenterOnPart1 = true;
            snprintf(row.angle, sizeof(row.angle), "%.2f%+d", throwData.angleWithoutCorrection, correctionIncrements);
            row.angleColor = (correctionIncrements > 0) ? posAdjustmentCol : negAdjustmentCol;
        }

        if (std::abs(throwData.error) > 1e-9) {
            snprintf(row.error, sizeof(row.error), "%.4f", throwData.error);
        } else {
            snprintf(row.error, sizeof(row.error), "-");
        }
    };
    auto clearThrowDetailRow = [&](ThrowDetailRow& row) {
        row.x[0] = 0;
        row.z[0] = 0;
        row.angle[0] = 0;
        row.error[0] = 0;
        row.angleColor = throwsTextCol;
        row.anglePart1Color = throwsTextCol;
        row.angleSplitOffset = 0.0f;
        row.angleCenterOnPart1 = false;
    };

    if (failedResult || blindResult) {
        const float summaryBlockH = blindResult
            ? (showBlindImproveDirection ? (lineH * 3.0f + summaryLineGap * 2.0f) : (lineH * 2.0f + summaryLineGap))
            : (lineH * 2.0f + summaryLineGap);
        const float sectionHeaderH = lineH;
        const float detailHeaderH = lineH + throwsHeaderPadY * 2.0f;
        const float detailRowH = lineH + throwsRowPadY * 2.0f;
        const float cellPadX = 8.0f * scale;
        const int failedThrowDataCount = (std::min)(data.eyeCount, (int)kNinjabrainThrowLimit);
        const int failedThrowCount = (std::max)(failedThrowDataCount, minimumThrowRows);

        ThrowDetailRow failedRows[kNinjabrainThrowLimit] = {};
        const char* failedHeaders[4] = { "x", "z", "Angle", "Error" };
        float failedColWidths[4] = {};
        for (int fi = 0; fi < 4; ++fi) {
            failedColWidths[fi] = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, failedHeaders[fi]).x + cellPadX * 2.0f;
        }

        for (int ri = 0; ri < failedThrowCount; ++ri) {
            if (ri < failedThrowDataCount) {
                const auto& failedThrow = data.throws[ri];
                int correctionIncrements = 0;
                if (failedThrow.hasCorrectionIncrements) {
                    correctionIncrements = failedThrow.correctionIncrements;
                } else if (ri == failedThrowDataCount - 1) {
                    correctionIncrements = data.correctionIncrements151;
                }

                buildThrowDetailRow(failedThrow, correctionIncrements, failedRows[ri]);
            } else {
                clearThrowDetailRow(failedRows[ri]);
            }

            failedColWidths[0] = (std::max)(failedColWidths[0], font->CalcTextSizeA(fs, FLT_MAX, 0.0f, failedRows[ri].x).x + cellPadX * 2.0f);
            failedColWidths[1] = (std::max)(failedColWidths[1], font->CalcTextSizeA(fs, FLT_MAX, 0.0f, failedRows[ri].z).x + cellPadX * 2.0f);
            failedColWidths[2] = (std::max)(failedColWidths[2], font->CalcTextSizeA(fs, FLT_MAX, 0.0f, failedRows[ri].angle).x + cellPadX * 2.0f);
            failedColWidths[3] = (std::max)(failedColWidths[3], font->CalcTextSizeA(fs, FLT_MAX, 0.0f, failedRows[ri].error).x + cellPadX * 2.0f);
        }

        float failedTableMinW = 0.0f;
        for (float width : failedColWidths) failedTableMinW += width;

        const char* throwSectionTitle = "Ender eye throws";
        const float throwSectionTitleW = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, throwSectionTitle).x;
        const float summaryMessageW = blindResult
            ? blindMessageW
            : (std::max)(font->CalcTextSizeA(fs, FLT_MAX, 0.0f, trc("ninjabrain.failed_line1")).x,
                         font->CalcTextSizeA(fs, FLT_MAX, 0.0f, trc("ninjabrain.failed_line2")).x);
        const float failedThrowsMinContentW = (std::max)(throwSectionTitleW, failedTableMinW);
        const float summaryMinContentW = summaryMarginLeft + summaryMessageW + summaryMarginRight;
        const float failedPreferredContentW = (std::max)((std::max)(summaryMinContentW, infoMarginLeft + infoPreferredContentW + infoMarginRight),
                                 throwsMarginLeft + failedThrowsMinContentW + throwsMarginRight);

        if (useManualSectionLayout) {
            const float summarySectionW = (std::max)(summaryMinContentW, failedPreferredContentW);
            const float summarySectionH = summaryMarginTop + summaryBlockH + summaryMarginBottom;
            const float infoSectionContentW = (std::max)(infoPreferredContentW, failedPreferredContentW - infoMarginLeft - infoMarginRight);
            const float infoSectionW = infoMarginLeft + infoSectionContentW + infoMarginRight;
            const float infoSectionH = hasInformationMessages ? measureInfoMessagesBlock(infoSectionContentW) : 0.0f;
            const float throwsSectionContentW = (std::max)(failedThrowsMinContentW, failedPreferredContentW - throwsMarginLeft - throwsMarginRight);
            const float throwsSectionW = throwsMarginLeft + throwsSectionContentW + throwsMarginRight;
            const float throwsSectionH = failedThrowCount > 0
                ? (throwsMarginTop + sectionHeaderH + detailHeaderH + detailRowH * (float)failedThrowCount + throwsMarginBottom)
                : 0.0f;

            std::vector<NinjabrainManualSectionPlacement> sections;
            sections.reserve(3);
            sections.push_back({ parseManualAnchor(*summaryAnchor), summaryOffsetX, summaryOffsetY,
                                 summaryDrawOrder, summarySectionW, summarySectionH });
            if (hasInformationMessages) {
                sections.push_back({ parseManualAnchor(nb.informationMessagesAnchor), nb.informationMessagesOffsetX * scale,
                                     nb.informationMessagesOffsetY * scale, nb.informationMessagesDrawOrder,
                                     infoSectionW, infoSectionH });
            }
            if (failedThrowCount > 0) {
                sections.push_back({ parseManualAnchor(nb.throwsAnchor), nb.throwsOffsetX * scale, nb.throwsOffsetY * scale,
                                     nb.throwsDrawOrder, throwsSectionW, throwsSectionH });
            }

            float contentW = 0.0f;
            float contentH = 0.0f;
            computeManualContentSize(sections, contentW, contentH);
            const float totalW = (contentPadX + sidePadX) * 2.0f + contentW;
            const float totalH = contentPadTop + contentH + contentPadBottom;

            float ox = 0.0f;
            float oy = 0.0f;
            calculateOrigin(totalW, totalH, ox, oy);

            const float backgroundAlpha = nb.bgEnabled ? nb.bgOpacity * nb.overlayOpacity : 0.0f;
            const float surfaceAlpha = backgroundAlpha;
            const ImU32 panelBgCol = colorWithAlpha(nb.bgColor, backgroundAlpha);
            const ImU32 borderCol = colorWithAlpha(nb.borderColor, nb.overlayOpacity);
            const ImU32 dividerCol = colorWithAlpha(nb.dividerColor, backgroundAlpha);
            const ImU32 resultsDividerCol = dividerCol;
            const ImU32 headerDividerCol = colorWithAlpha(nb.headerDividerColor, backgroundAlpha);
            const ImU32 headerFillCol = colorWithAlpha(nb.headerFillColor, surfaceAlpha);
            const ImU32 throwsBgCol = colorWithAlpha(nb.throwsBackgroundColor, surfaceAlpha);

            drawOverlayBackground(ox, oy, totalW, totalH, panelBgCol);
            drawOverlayBorder(ox, oy, totalW, totalH, borderCol);
            drawBoatHeaderStateIcon(ox + totalW, oy, boatStateSize, 0.0f);

            const float contentLeft = ox + contentPadX + sidePadX;
            const float contentTop = oy + contentPadTop;
            std::vector<std::pair<int, std::function<void()>>> drawSections;
            drawSections.reserve(3);

            {
                NinjabrainManualSectionPlacement placement = {
                    parseManualAnchor(*summaryAnchor), summaryOffsetX, summaryOffsetY,
                    summaryDrawOrder, summarySectionW, summarySectionH
                };
                float sectionX = 0.0f;
                float sectionY = 0.0f;
                computeManualSectionOrigin(placement, contentLeft, contentTop, contentW, contentH, sectionX, sectionY);
                drawSections.push_back({ placement.drawOrder, [=, &drawText, &drawList]() {
                    const float blockTop = sectionY + summaryMarginTop;
                    const float messageTop = blockTop;
                    const float messageX = sectionX + summaryMarginLeft;
                    drawFailureSummaryBlock(messageX, messageTop);
                } });
            }

            if (hasInformationMessages) {
                NinjabrainManualSectionPlacement placement = {
                    parseManualAnchor(nb.informationMessagesAnchor), nb.informationMessagesOffsetX * scale,
                    nb.informationMessagesOffsetY * scale, nb.informationMessagesDrawOrder, infoSectionW, infoSectionH
                };
                float sectionX = 0.0f;
                float sectionY = 0.0f;
                computeManualSectionOrigin(placement, contentLeft, contentTop, contentW, contentH, sectionX, sectionY);
                drawSections.push_back({ placement.drawOrder, [=, &drawInfoMessagesBlock]() {
                    drawInfoMessagesBlock(sectionX, sectionX + infoSectionW, sectionX + infoMarginLeft, sectionY,
                                          infoSectionContentW, headerDividerCol, throwsTextCol);
                } });
            }

            if (failedThrowCount > 0) {
                NinjabrainManualSectionPlacement placement = {
                    parseManualAnchor(nb.throwsAnchor), nb.throwsOffsetX * scale, nb.throwsOffsetY * scale,
                    nb.throwsDrawOrder, throwsSectionW, throwsSectionH
                };
                float sectionX = 0.0f;
                float sectionY = 0.0f;
                computeManualSectionOrigin(placement, contentLeft, contentTop, contentW, contentH, sectionX, sectionY);
                drawSections.push_back({ placement.drawOrder, [=, &drawText, &drawCenteredSegmentedText, &drawList]() {
                    const float surfaceLeft = sectionX;
                    const float surfaceRight = sectionX + throwsSectionW;
                    const float sectionTop = sectionY + throwsMarginTop;
                    const float sectionTextY = sectionTop + (sectionHeaderH - lineH) * 0.5f;
                    const float detailHeaderY = sectionTop + sectionHeaderH;
                    const float detailRowY = detailHeaderY + detailHeaderH;
                    const float detailTableX = sectionX + throwsMarginLeft;

                    const float detailRowsBottomY = detailRowY + detailRowH * (float)failedThrowCount;
                    const bool roundHeaderTopCorners = touchesPanelEdge(sectionTop, oy);
                    const bool roundDetailBottomCorners = touchesPanelEdge(detailRowsBottomY, oy + totalH);
                    drawRoundedBand(ImVec2(surfaceLeft, sectionTop), ImVec2(surfaceRight, detailRowY), headerFillCol,
                                    roundHeaderTopCorners, false);
                    drawRoundedBand(ImVec2(surfaceLeft, detailRowY), ImVec2(surfaceRight, detailRowsBottomY), throwsBgCol,
                                    false, roundDetailBottomCorners);
                    drawClippedHorizontalLine(surfaceLeft, oy, surfaceRight, oy + totalH, sectionTop, headerDividerCol);
                    drawText(fs, ImVec2(detailTableX, sectionTextY), dataCol, throwSectionTitle);

                    float detailX = detailTableX;
                    for (int fi = 0; fi < 4; ++fi) {
                        drawCenteredSegmentedText(detailX, failedColWidths[fi], detailHeaderY + (detailHeaderH - lineH) * 0.5f,
                                                 failedHeaders[fi], textCol, 0.0f, textCol, false, false);
                        detailX += failedColWidths[fi];
                    }

                    drawClippedHorizontalLine(surfaceLeft, oy, surfaceRight, oy + totalH, detailRowY, headerDividerCol);
                    for (int ri = 0; ri < failedThrowCount; ++ri) {
                        const float rowTop = detailRowY + detailRowH * (float)ri;
                        const float rowBottom = rowTop + detailRowH;
                        const float rowTextY = rowTop + (detailRowH - lineH) * 0.5f;
                        float detailRowX = detailTableX;

                        drawCenteredSegmentedText(detailRowX, failedColWidths[0], rowTextY, failedRows[ri].x, throwsTextCol, 0.0f, throwsTextCol, false, false);
                        detailRowX += failedColWidths[0];
                        drawCenteredSegmentedText(detailRowX, failedColWidths[1], rowTextY, failedRows[ri].z, throwsTextCol, 0.0f, throwsTextCol, false, false);
                        detailRowX += failedColWidths[1];
                        drawCenteredSegmentedText(detailRowX, failedColWidths[2], rowTextY, failedRows[ri].angle,
                                                 failedRows[ri].angleColor, failedRows[ri].angleSplitOffset,
                                                 failedRows[ri].anglePart1Color, failedRows[ri].angleCenterOnPart1, false);
                        detailRowX += failedColWidths[2];
                        drawCenteredSegmentedText(detailRowX, failedColWidths[3], rowTextY, failedRows[ri].error, throwsTextCol, 0.0f, throwsTextCol, false, false);

                        const ImU32 rowDivider = (ri == failedThrowCount - 1) ? dividerCol : headerDividerCol;
                        drawClippedHorizontalLine(surfaceLeft, oy, surfaceRight, oy + totalH, rowBottom, rowDivider);
                    }
                } });
            }

            std::stable_sort(drawSections.begin(), drawSections.end(), [](const auto& a, const auto& b) {
                return a.first < b.first;
            });
            for (auto& drawSection : drawSections) {
                drawSection.second();
            }
            return;
        }

        float contentW = failedPreferredContentW;
        const float infoContentW = hasInformationMessages
            ? (std::max)(infoPreferredContentW, contentW - infoMarginLeft - infoMarginRight)
            : 0.0f;
        const float infoBlockH = hasInformationMessages ? measureInfoMessagesBlock(infoContentW) : 0.0f;
        const float failedThrowsContentW = (failedThrowCount > 0)
            ? (std::max)((std::max)(throwSectionTitleW, failedTableMinW), contentW - throwsMarginLeft - throwsMarginRight)
            : 0.0f;

        if (failedThrowCount > 0) {
            const float extraPerCol = (std::max)(0.0f, failedThrowsContentW - failedTableMinW) / 4.0f;
            for (float& width : failedColWidths) {
                width += extraPerCol;
            }
        }

        const float totalW = (contentPadX + sidePadX) * 2.0f + contentW;
        float totalH = contentPadTop;
        if (hasInformationMessages && infoPlacement == "top") {
            totalH += infoBlockH;
        }
        totalH += summaryMarginTop + summaryBlockH + summaryMarginBottom;
        if (hasInformationMessages && infoPlacement == "middle") {
            totalH += infoBlockH;
        }
        if (failedThrowCount > 0) {
            totalH += throwsMarginTop + sectionHeaderH + detailHeaderH + detailRowH * (float)failedThrowCount + throwsMarginBottom;
        }
        if (hasInformationMessages && infoPlacement == "bottom") {
            totalH += infoBlockH;
        }
        totalH += contentPadBottom;

        float ox = 0.0f;
        float oy = 0.0f;
        calculateOrigin(totalW, totalH, ox, oy);

        const float backgroundAlpha = nb.bgEnabled ? nb.bgOpacity * nb.overlayOpacity : 0.0f;
        const float surfaceAlpha = backgroundAlpha;
        const ImU32 panelBgCol = colorWithAlpha(nb.bgColor, backgroundAlpha);
        const ImU32 borderCol = colorWithAlpha(nb.borderColor, nb.overlayOpacity);
        const ImU32 dividerCol = colorWithAlpha(nb.dividerColor, backgroundAlpha);
        const ImU32 resultsDividerCol = dividerCol;
        const ImU32 headerDividerCol = colorWithAlpha(nb.headerDividerColor, backgroundAlpha);
        const ImU32 headerFillCol = colorWithAlpha(nb.headerFillColor, surfaceAlpha);
        const ImU32 throwsBgCol = colorWithAlpha(nb.throwsBackgroundColor, surfaceAlpha);

        std::vector<std::pair<float, float>> backgroundCoveredBands;
        float coveredCursorY = oy + contentPadTop;
        if (hasInformationMessages && infoPlacement == "top") {
            coveredCursorY += infoBlockH;
        }
        if (failedThrowCount > 0) {
            if (hasInformationMessages && infoPlacement == "middle") {
                coveredCursorY += summaryMarginTop + summaryBlockH + summaryMarginBottom + infoBlockH;
            } else {
                coveredCursorY += summaryMarginTop + summaryBlockH + summaryMarginBottom;
            }
            const float coveredSectionTop = coveredCursorY + throwsMarginTop;
            const float coveredSectionBottom = coveredSectionTop + sectionHeaderH + detailHeaderH + detailRowH * (float)failedThrowCount;
            backgroundCoveredBands.push_back({ coveredSectionTop, coveredSectionBottom });
        }
        drawOverlayBackgroundGaps(ox, oy, totalW, totalH, panelBgCol, std::move(backgroundCoveredBands));
        drawOverlayBorder(ox, oy, totalW, totalH, borderCol);
        drawBoatHeaderStateIcon(ox + totalW, oy, boatStateSize, 0.0f);

        const float surfaceLeft = ox;
        const float surfaceRight = ox + totalW;
        const float contentLeft = ox + contentPadX + sidePadX;
        float contentBottomY = oy + contentPadTop;
        if (hasInformationMessages && infoPlacement == "top") {
            contentBottomY = drawInfoMessagesBlock(
                surfaceLeft,
                surfaceRight,
                contentLeft + infoMarginLeft,
                contentBottomY,
                infoContentW,
                headerDividerCol,
                throwsTextCol);
        }

            const float summaryBlockTop = contentBottomY + summaryMarginTop;
            const float messageTop = summaryBlockTop;
            const float messageX = contentLeft + summaryMarginLeft;
        drawFailureSummaryBlock(messageX, messageTop);

            const float summaryBottomY = summaryBlockTop + summaryBlockH;
            contentBottomY = summaryBottomY + summaryMarginBottom;

        if (hasInformationMessages && infoPlacement == "middle") {
            contentBottomY = drawInfoMessagesBlock(
                surfaceLeft,
                surfaceRight,
                contentLeft + infoMarginLeft,
                contentBottomY,
                infoContentW,
                headerDividerCol,
            throwsTextCol);
        }

        if (failedThrowCount > 0) {
            const float sectionY = contentBottomY + throwsMarginTop;
            const float sectionTextY = sectionY + (sectionHeaderH - lineH) * 0.5f;
            const float detailHeaderY = sectionY + sectionHeaderH;
            const float detailRowY = detailHeaderY + detailHeaderH;
            const float detailTableX = contentLeft + throwsMarginLeft;

            const float detailRowsBottomY = detailRowY + detailRowH * (float)failedThrowCount;
            const bool roundHeaderTopCorners = touchesPanelEdge(sectionY, oy);
            const bool roundDetailBottomCorners = touchesPanelEdge(detailRowsBottomY, oy + totalH);
            drawRoundedBand(ImVec2(surfaceLeft, sectionY), ImVec2(surfaceRight, detailRowY), headerFillCol,
                            roundHeaderTopCorners, false);
            drawRoundedBand(ImVec2(surfaceLeft, detailRowY), ImVec2(surfaceRight, detailRowsBottomY), throwsBgCol,
                            false, roundDetailBottomCorners);
            drawClippedHorizontalLine(surfaceLeft, oy, surfaceRight, oy + totalH, sectionY, headerDividerCol);
            drawText(fs, ImVec2(detailTableX, sectionTextY), dataCol, throwSectionTitle);

            float detailX = detailTableX;
            for (int fi = 0; fi < 4; ++fi) {
                drawCenteredSegmentedText(detailX, failedColWidths[fi], detailHeaderY + (detailHeaderH - lineH) * 0.5f,
                                         failedHeaders[fi], textCol, 0.0f, textCol, false, false);
                detailX += failedColWidths[fi];
            }

            drawClippedHorizontalLine(surfaceLeft, oy, surfaceRight, oy + totalH, detailRowY, headerDividerCol);
            for (int ri = 0; ri < failedThrowCount; ++ri) {
                const float rowTop = detailRowY + detailRowH * (float)ri;
                const float rowBottom = rowTop + detailRowH;
                const float rowTextY = rowTop + (detailRowH - lineH) * 0.5f;
                float detailRowX = detailTableX;

                drawCenteredSegmentedText(detailRowX, failedColWidths[0], rowTextY, failedRows[ri].x, throwsTextCol, 0.0f, throwsTextCol, false, false);
                detailRowX += failedColWidths[0];
                drawCenteredSegmentedText(detailRowX, failedColWidths[1], rowTextY, failedRows[ri].z, throwsTextCol, 0.0f, throwsTextCol, false, false);
                detailRowX += failedColWidths[1];
                drawCenteredSegmentedText(detailRowX, failedColWidths[2], rowTextY, failedRows[ri].angle,
                                         failedRows[ri].angleColor, failedRows[ri].angleSplitOffset, failedRows[ri].anglePart1Color,
                                         failedRows[ri].angleCenterOnPart1, false);
                detailRowX += failedColWidths[2];
                drawCenteredSegmentedText(detailRowX, failedColWidths[3], rowTextY, failedRows[ri].error, throwsTextCol, 0.0f, throwsTextCol, false, false);

                const ImU32 rowDivider = (ri == failedThrowCount - 1) ? dividerCol : headerDividerCol;
                drawClippedHorizontalLine(surfaceLeft, oy, surfaceRight, oy + totalH, rowBottom, rowDivider);
            }

            contentBottomY = detailRowY + detailRowH * (float)failedThrowCount + throwsMarginBottom;
        }

        if (hasInformationMessages && infoPlacement == "bottom") {
            drawInfoMessagesBlock(
                surfaceLeft,
                surfaceRight,
                contentLeft + infoMarginLeft,
                contentBottomY,
                infoContentW,
                headerDividerCol,
                throwsTextCol);
        }

        return;
    }

    float topTableW = 0.0f;
    for (int ci = 0; ci < numCols; ++ci) {
        topTableW += cols[ci].width;
        if (ci < numCols - 1) topTableW += colGap;
    }
    const float topHeaderInnerW = (std::max)(topTableW, showBoatHeaderIcon ? (boatStateMarginRight + boatStateSize) : 0.0f);

    const bool showTopTable = numCols > 0 || showBoatHeaderIcon;
    const int detailRowCount = (std::min)(data.eyeCount, (int)kNinjabrainThrowLimit);
    const bool showDetailPanel = nb.showThrowDetails && (detailRowCount > 0 || (showPredictionTable && !boatOnly));
    const bool showTitleBar = false;
    const float headerBandPadY = showTopTable ? resultsHeaderPadY : 0.0f;
    const float headerBandH = showTopTable ? (lineH + headerBandPadY * 2.0f) : 0.0f;
    const float resultsRowSpacing = (std::max)(0.0f, nb.rowSpacing) * scale;
    const float resultsTextOffsetY = resultsRowSpacing * 0.5f;
    const float resultsLastRowH = showTopTable ? (lineH + resultsTextOffsetY) : 0.0f;
    const float resultsBodyH = (showTopTable && maxRows > 0)
        ? (rowH * (float)(maxRows - 1) + resultsLastRowH)
        : 0.0f;
    const float titleBarH = showTitleBar ? headerBandH : 0.0f;
    const float cellPadX = 8.0f * scale;
    const float sectionGap = showDetailPanel ? throwsMarginTop : 0.0f;
    const float sectionHeaderH = showDetailPanel ? lineH : 0.0f;
    const float detailHeaderH = showDetailPanel ? (lineH + throwsHeaderPadY * 2.0f) : 0.0f;
    const float detailRowH = showDetailPanel ? (lineH + throwsRowPadY * 2.0f) : 0.0f;

    const int detailRenderRowCount = showDetailPanel ? (std::max)(detailRowCount, minimumThrowRows) : 0;
    const char* detailHeaders[4] = { "x", "z", "Angle", "Error" };
    ThrowDetailRow detailRows[kNinjabrainThrowLimit] = {};
    if (showDetailPanel) {
        for (int ri = 0; ri < detailRenderRowCount; ++ri) {
            if (ri < detailRowCount) {
                const auto& throwData = data.throws[ri];
                int correctionIncrements = 0;
                if (throwData.hasCorrectionIncrements) {
                    correctionIncrements = throwData.correctionIncrements;
                } else if (ri == detailRowCount - 1) {
                    correctionIncrements = data.correctionIncrements151;
                }
                buildThrowDetailRow(throwData, correctionIncrements, detailRows[ri]);
            } else {
                clearThrowDetailRow(detailRows[ri]);
            }
        }
    }

    float detailMinColWidths[4] = {};
    float detailTableMinW = 0.0f;
    if (showDetailPanel) {
        for (int fi = 0; fi < 4; ++fi) {
            float headerW = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, detailHeaders[fi]).x;
            float valueW = 0.0f;
            for (int ri = 0; ri < detailRenderRowCount; ++ri) {
                const char* cellText = (fi == 0) ? detailRows[ri].x
                    : (fi == 1) ? detailRows[ri].z
                    : (fi == 2) ? detailRows[ri].angle
                                : detailRows[ri].error;
                valueW = (std::max)(valueW, font->CalcTextSizeA(fs, FLT_MAX, 0.0f, cellText).x);
            }
            if (nb.staticColumnWidths) {
                if (fi == 0 || fi == 1) {
                    valueW = (std::max)(valueW, font->CalcTextSizeA(fs, FLT_MAX, 0.0f, "-999999.99").x);
                } else if (fi == 2) {
                    valueW = (std::max)(valueW, font->CalcTextSizeA(fs, FLT_MAX, 0.0f, "359.99+999").x);
                } else if (fi == 3) {
                    valueW = (std::max)(valueW, font->CalcTextSizeA(fs, FLT_MAX, 0.0f, "-180.0000").x);
                }
            }
            detailMinColWidths[fi] = (std::max)(headerW, valueW) + cellPadX * 2.0f;
            detailTableMinW += detailMinColWidths[fi];
        }
    }

    const char* throwSectionTitle = "Ender eye throws";
    const float throwSectionTitleW = showDetailPanel ? font->CalcTextSizeA(fs, FLT_MAX, 0.0f, throwSectionTitle).x : 0.0f;
    const float throwsMinContentW = (std::max)(detailTableMinW, throwSectionTitleW);

    if (useManualSectionLayout) {
        const float resultsSectionW = showTopTable ? (resultsMarginLeft + topHeaderInnerW + resultsMarginRight) : 0.0f;
        const float resultsSectionH = showTopTable
            ? (resultsMarginTop + headerBandH + resultsBodyH + resultsMarginBottom)
            : 0.0f;
        const float infoSectionContentW = infoPreferredContentW;
        const float infoSectionW = infoMarginLeft + infoSectionContentW + infoMarginRight;
        const float infoSectionH = hasInformationMessages ? measureInfoMessagesBlock(infoSectionContentW) : 0.0f;
        const float throwsSectionContentW = throwsMinContentW;
        const float throwsSectionW = showDetailPanel ? (throwsMarginLeft + throwsSectionContentW + throwsMarginRight) : 0.0f;
        const float throwsSectionH = showDetailPanel
            ? (throwsMarginTop + sectionHeaderH + detailHeaderH + detailRowH * (float)detailRenderRowCount + throwsMarginBottom)
            : 0.0f;

        std::vector<NinjabrainManualSectionPlacement> sections;
        sections.reserve(3);
        if (showTopTable) {
            sections.push_back({ parseManualAnchor(nb.resultsAnchor), nb.resultsOffsetX * scale, nb.resultsOffsetY * scale,
                                 nb.resultsDrawOrder, resultsSectionW, resultsSectionH });
        }
        if (hasInformationMessages) {
            sections.push_back({ parseManualAnchor(nb.informationMessagesAnchor), nb.informationMessagesOffsetX * scale,
                                 nb.informationMessagesOffsetY * scale, nb.informationMessagesDrawOrder,
                                 infoSectionW, infoSectionH });
        }
        if (showDetailPanel) {
            sections.push_back({ parseManualAnchor(nb.throwsAnchor), nb.throwsOffsetX * scale, nb.throwsOffsetY * scale,
                                 nb.throwsDrawOrder, throwsSectionW, throwsSectionH });
        }

        float contentW = 0.0f;
        float contentH = 0.0f;
        computeManualContentSize(sections, contentW, contentH);
        const float totalW = (contentPadX + sidePadX) * 2.0f + contentW;
        const float totalH = titleBarH + contentPadTop + contentH + contentPadBottom;

        float ox = 0.0f;
        float oy = 0.0f;
        calculateOrigin(totalW, totalH, ox, oy);

        const float backgroundAlpha = nb.bgEnabled ? nb.bgOpacity * nb.overlayOpacity : 0.0f;
        const float surfaceAlpha = backgroundAlpha;
        const ImU32 panelBgCol = colorWithAlpha(nb.bgColor, backgroundAlpha);
        const ImU32 borderCol = colorWithAlpha(nb.borderColor, nb.overlayOpacity);
        const ImU32 dividerCol = colorWithAlpha(nb.dividerColor, backgroundAlpha);
        const ImU32 resultsDividerCol = dividerCol;
        const ImU32 headerDividerCol = colorWithAlpha(nb.headerDividerColor, backgroundAlpha);
        const ImU32 headerFillCol = colorWithAlpha(nb.headerFillColor, surfaceAlpha);
        const ImU32 throwsBgCol = colorWithAlpha(nb.throwsBackgroundColor, surfaceAlpha);

        drawOverlayBackground(ox, oy, totalW, totalH, panelBgCol);
        drawOverlayBorder(ox, oy, totalW, totalH, borderCol);

        const float contentLeft = ox + contentPadX + sidePadX;
        const float contentTop = oy + titleBarH + contentPadTop;
        std::vector<std::pair<int, std::function<void()>>> drawSections;
        drawSections.reserve(3);

        if (showTopTable) {
            NinjabrainManualSectionPlacement placement = {
                parseManualAnchor(nb.resultsAnchor), nb.resultsOffsetX * scale, nb.resultsOffsetY * scale,
                nb.resultsDrawOrder, resultsSectionW, resultsSectionH
            };
            float sectionX = 0.0f;
            float sectionY = 0.0f;
            computeManualSectionOrigin(placement, contentLeft, contentTop, contentW, contentH, sectionX, sectionY);
            drawSections.push_back({ placement.drawOrder, [=, &drawCenteredSegmentedText, &drawList]() {
                const float surfaceLeft = sectionX;
                const float surfaceRight = sectionX + resultsSectionW;
                const float headerBandY = sectionY + resultsMarginTop;
                const float headerY = headerBandY + (headerBandH - lineH) * 0.5f;
                const float headerSeparatorY = headerBandY + headerBandH;
                const float tableX = sectionX + resultsMarginLeft;

                drawRoundedBand(ImVec2(surfaceLeft, headerBandY), ImVec2(surfaceRight, headerSeparatorY), headerFillCol,
                                touchesPanelEdge(headerBandY, oy), false);

                float cx = tableX;
                for (int ci = 0; ci < numCols; ++ci) {
                    Col& col = const_cast<Col&>(cols[ci]);
                    drawCenteredSegmentedText(cx, col.width, headerY, col.header, textCol, 0.0f, textCol, false, false);
                    cx += col.width + (ci < numCols - 1 ? colGap : 0.0f);
                }
                drawBoatHeaderStateIcon(surfaceRight, headerBandY, headerBandH, resultsMarginRight);

                drawClippedHorizontalLine(surfaceLeft, oy, surfaceRight, oy + totalH, headerSeparatorY, headerDividerCol);

                for (int ri = 0; ri < maxRows; ++ri) {
                    const float rowTop = headerSeparatorY + rowH * (float)ri;
                    const bool isLastRow = ri + 1 == maxRows;
                    const float rowBottom = rowTop + (isLastRow ? resultsLastRowH : rowH);
                    const float rowTextY = rowTop + resultsTextOffsetY;

                        drawClippedHorizontalLine(surfaceLeft, oy, surfaceRight, oy + totalH, rowBottom, resultsDividerCol);

                    float rowX = tableX;
                    for (int ci = 0; ci < numCols; ++ci) {
                        Col& col = const_cast<Col&>(cols[ci]);
                        if (ri < col.rowCount) {
                            drawCenteredSegmentedText(rowX, col.width, rowTextY, col.rows[ri].text, col.rows[ri].color,
                                                     col.rows[ri].xOffset, col.rows[ri].part1Color, col.rows[ri].centerOnPart1, col.leftAlign);
                        }
                        rowX += col.width + (ci < numCols - 1 ? colGap : 0.0f);
                    }
                }
            } });
        }

        if (hasInformationMessages) {
            NinjabrainManualSectionPlacement placement = {
                parseManualAnchor(nb.informationMessagesAnchor), nb.informationMessagesOffsetX * scale,
                nb.informationMessagesOffsetY * scale, nb.informationMessagesDrawOrder, infoSectionW, infoSectionH
            };
            float sectionX = 0.0f;
            float sectionY = 0.0f;
            computeManualSectionOrigin(placement, contentLeft, contentTop, contentW, contentH, sectionX, sectionY);
            drawSections.push_back({ placement.drawOrder, [=, &drawInfoMessagesBlock]() {
                drawInfoMessagesBlock(sectionX, sectionX + infoSectionW, sectionX + infoMarginLeft, sectionY,
                                      infoSectionContentW, headerDividerCol, throwsTextCol);
            } });
        }

        if (showDetailPanel) {
            NinjabrainManualSectionPlacement placement = {
                parseManualAnchor(nb.throwsAnchor), nb.throwsOffsetX * scale, nb.throwsOffsetY * scale,
                nb.throwsDrawOrder, throwsSectionW, throwsSectionH
            };
            float sectionX = 0.0f;
            float sectionY = 0.0f;
            computeManualSectionOrigin(placement, contentLeft, contentTop, contentW, contentH, sectionX, sectionY);
            drawSections.push_back({ placement.drawOrder, [=, &drawText, &drawCenteredSegmentedText, &drawList]() {
                const float surfaceLeft = sectionX;
                const float surfaceRight = sectionX + throwsSectionW;
                const float sectionYTop = sectionY + throwsMarginTop;
                const float sectionTextY = sectionYTop + (sectionHeaderH - lineH) * 0.5f;
                const float detailTableX = sectionX + throwsMarginLeft;
                const float detailHeaderY = sectionYTop + sectionHeaderH;
                const float detailRowY = detailHeaderY + detailHeaderH;
                const float detailRowsBottomY = detailRowY + detailRowH * (float)detailRenderRowCount;

                drawRoundedBand(ImVec2(surfaceLeft, sectionYTop), ImVec2(surfaceRight, detailRowY), headerFillCol,
                                touchesPanelEdge(sectionYTop, oy), false);
                drawRoundedBand(ImVec2(surfaceLeft, detailRowY), ImVec2(surfaceRight, detailRowsBottomY), throwsBgCol,
                                false, touchesPanelEdge(detailRowsBottomY, oy + totalH));
                drawText(fs, ImVec2(detailTableX, sectionTextY), dataCol, throwSectionTitle);

                float detailX = detailTableX;
                for (int fi = 0; fi < 4; ++fi) {
                    drawCenteredSegmentedText(detailX, detailMinColWidths[fi], detailHeaderY + (detailHeaderH - lineH) * 0.5f,
                                             detailHeaders[fi], textCol, 0.0f, textCol, false, false);
                    detailX += detailMinColWidths[fi];
                }

                drawClippedHorizontalLine(surfaceLeft, oy, surfaceRight, oy + totalH, detailRowY, headerDividerCol);
                for (int ri = 0; ri < detailRenderRowCount; ++ri) {
                    const float rowTop = detailRowY + detailRowH * (float)ri;
                    const float rowBottom = rowTop + detailRowH;
                    const float rowTextY = rowTop + (detailRowH - lineH) * 0.5f;
                    float detailRowX = detailTableX;

                    drawCenteredSegmentedText(detailRowX, detailMinColWidths[0], rowTextY, detailRows[ri].x, throwsTextCol, 0.0f, throwsTextCol, false, false);
                    detailRowX += detailMinColWidths[0];
                    drawCenteredSegmentedText(detailRowX, detailMinColWidths[1], rowTextY, detailRows[ri].z, throwsTextCol, 0.0f, throwsTextCol, false, false);
                    detailRowX += detailMinColWidths[1];
                    drawCenteredSegmentedText(detailRowX, detailMinColWidths[2], rowTextY, detailRows[ri].angle,
                                             detailRows[ri].angleColor, detailRows[ri].angleSplitOffset,
                                             detailRows[ri].anglePart1Color, detailRows[ri].angleCenterOnPart1, false);
                    detailRowX += detailMinColWidths[2];
                    drawCenteredSegmentedText(detailRowX, detailMinColWidths[3], rowTextY, detailRows[ri].error, throwsTextCol, 0.0f, throwsTextCol, false, false);

                    drawClippedHorizontalLine(surfaceLeft, oy, surfaceRight, oy + totalH, rowBottom, dividerCol);
                }
            } });
        }

        std::stable_sort(drawSections.begin(), drawSections.end(), [](const auto& a, const auto& b) {
            return a.first < b.first;
        });
        for (auto& drawSection : drawSections) {
            drawSection.second();
        }
        return;
    }

    float contentW = showTopTable ? (resultsMarginLeft + topHeaderInnerW + resultsMarginRight) : 0.0f;
    if (showDetailPanel) {
        contentW = (std::max)(contentW, throwsMarginLeft + (std::max)(detailTableMinW, throwSectionTitleW) + throwsMarginRight);
    }
    if (hasInformationMessages) {
        contentW = (std::max)(contentW, infoMarginLeft + infoPreferredContentW + infoMarginRight);
    }
    const float infoContentW = hasInformationMessages
        ? (std::max)(infoPreferredContentW, contentW - infoMarginLeft - infoMarginRight)
        : 0.0f;
    const float infoBlockH = hasInformationMessages ? measureInfoMessagesBlock(infoContentW) : 0.0f;
    const float detailContentW = showDetailPanel
        ? (std::max)((std::max)(detailTableMinW, throwSectionTitleW), contentW - throwsMarginLeft - throwsMarginRight)
        : 0.0f;

    float detailColWidths[4] = {};
    if (showDetailPanel) {
        const float detailExtraPerCol = (std::max)(0.0f, detailContentW - detailTableMinW) / 4.0f;
        for (int fi = 0; fi < 4; ++fi) {
            detailColWidths[fi] = detailMinColWidths[fi] + detailExtraPerCol;
        }
    }

    float totalW = (contentPadX + sidePadX) * 2.0f + contentW;
    float totalH = titleBarH + contentPadTop;
    if (hasInformationMessages && infoPlacement == "top") totalH += infoBlockH;
    if (showTopTable) totalH += resultsMarginTop + headerBandH + resultsBodyH + resultsMarginBottom;
    if (hasInformationMessages && infoPlacement == "middle") totalH += infoBlockH;
    if (showDetailPanel) totalH += sectionGap + sectionHeaderH + detailHeaderH + detailRowH * (float)detailRenderRowCount + throwsMarginBottom;
    if (hasInformationMessages && infoPlacement == "bottom") totalH += infoBlockH;
    totalH += contentPadBottom;

    float ox = 0.0f;
    float oy = 0.0f;
    calculateOrigin(totalW, totalH, ox, oy);

    const float backgroundAlpha = nb.bgEnabled ? nb.bgOpacity * nb.overlayOpacity : 0.0f;
    const float surfaceAlpha = backgroundAlpha;
    const ImU32 panelBgCol = colorWithAlpha(nb.bgColor, backgroundAlpha);
    const ImU32 titleBarCol = colorWithAlpha(nb.chromeColor, surfaceAlpha);
    const ImU32 borderCol = colorWithAlpha(nb.borderColor, nb.overlayOpacity);
    const ImU32 dividerCol = colorWithAlpha(nb.dividerColor, backgroundAlpha);
    const ImU32 resultsDividerCol = dividerCol;
    const ImU32 headerDividerCol = colorWithAlpha(nb.headerDividerColor, backgroundAlpha);
    const ImU32 headerFillCol = colorWithAlpha(nb.headerFillColor, surfaceAlpha);
    const ImU32 throwsBgCol = colorWithAlpha(nb.throwsBackgroundColor, surfaceAlpha);

    std::vector<std::pair<float, float>> backgroundCoveredBands;
    if (showTitleBar) {
        backgroundCoveredBands.push_back({ oy, oy + titleBarH });
    }
    float coveredCursorY = oy + titleBarH + contentPadTop;
    if (hasInformationMessages && infoPlacement == "top") {
        coveredCursorY += infoBlockH;
    }
    if (showTopTable) {
        const float coveredHeaderTop = coveredCursorY + resultsMarginTop;
        const float coveredHeaderBottom = coveredHeaderTop + headerBandH;
        backgroundCoveredBands.push_back({ coveredHeaderTop, coveredHeaderBottom });
        coveredCursorY = coveredHeaderBottom + resultsBodyH + resultsMarginBottom;
    }
    if (hasInformationMessages && infoPlacement == "middle") {
        coveredCursorY += infoBlockH;
    }
    if (showDetailPanel) {
        const float coveredSectionTop = coveredCursorY + sectionGap;
        const float coveredSectionBottom = coveredSectionTop + sectionHeaderH + detailHeaderH + detailRowH * (float)detailRenderRowCount;
        backgroundCoveredBands.push_back({ coveredSectionTop, coveredSectionBottom });
    }
    drawOverlayBackgroundGaps(ox, oy, totalW, totalH, panelBgCol, std::move(backgroundCoveredBands));
    drawOverlayBorder(ox, oy, totalW, totalH, borderCol);

    const float surfaceLeft = ox;
    const float surfaceRight = ox + totalW;
    const float contentAreaX = ox + contentPadX + sidePadX;
    const float contentAreaRight = surfaceRight - contentPadX - sidePadX;
    const float tableX = contentAreaX;
    const float titleBarY = oy;

    if (showTitleBar) {
        const float titleTextY = titleBarY + (titleBarH - lineH) * 0.5f;
        const std::string versionLabel = "v" + GetToolscreenVersionString();
        const float versionTextW = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, versionLabel.c_str()).x;
        drawList->AddRectFilled(ImVec2(surfaceLeft, titleBarY), ImVec2(surfaceRight, titleBarY + titleBarH),
                                titleBarCol, nb.cornerRadius, ImDrawFlags_RoundCornersTop);
        drawText(fs, ImVec2(contentAreaX, titleTextY), titleTextCol, nb.titleText.c_str());
        if (versionTextW + 12.0f * scale < contentAreaRight - contentAreaX) {
            drawText(fs, ImVec2(contentAreaRight - versionTextW, titleTextY), versionTextCol, versionLabel.c_str());
        }
        if (nb.showSeparators) {
            drawClippedHorizontalLine(surfaceLeft, oy, surfaceRight, oy + totalH, titleBarY + titleBarH, headerDividerCol);
        }
    }

    float contentBottomY = oy + titleBarH + contentPadTop;
    if (hasInformationMessages && infoPlacement == "top") {
        contentBottomY = drawInfoMessagesBlock(
            surfaceLeft,
            surfaceRight,
            contentAreaX + infoMarginLeft,
            contentBottomY,
            infoContentW,
            headerDividerCol,
            throwsTextCol);
    }

    if (showTopTable) {
        contentBottomY += resultsMarginTop;
        const float headerBandY = contentBottomY;
        const float headerY = headerBandY + (headerBandH - lineH) * 0.5f;
        const float headerSeparatorY = headerBandY + headerBandH;
        const bool roundHeaderTopCorners = !showTitleBar && headerBandY <= oy + 0.5f;
                const bool drawTrailingResultsDivider =
                        !(showDetailPanel && !(hasInformationMessages && infoPlacement == "middle") &&
                            resultsMarginBottom <= 0.0f && throwsMarginTop <= 0.0f);
        const float tableX = contentAreaX + resultsMarginLeft;
        drawList->AddRectFilled(ImVec2(surfaceLeft, headerBandY), ImVec2(surfaceRight, headerSeparatorY), headerFillCol,
                                roundHeaderTopCorners ? nb.cornerRadius : 0.0f,
                                roundHeaderTopCorners ? ImDrawFlags_RoundCornersTop : 0);

        float cx = tableX;
        for (int ci = 0; ci < numCols; ++ci) {
            Col& col = cols[ci];
            drawCenteredSegmentedText(cx, col.width, headerY, col.header, textCol, 0.0f, textCol, false, false);
            cx += col.width + (ci < numCols - 1 ? colGap : 0.0f);
        }
        drawBoatHeaderStateIcon(surfaceRight, headerBandY, headerBandH, resultsMarginRight);

        drawClippedHorizontalLine(surfaceLeft, oy, surfaceRight, oy + totalH, headerSeparatorY, headerDividerCol);

        for (int ri = 0; ri < maxRows; ++ri) {
            const float rowTop = headerSeparatorY + rowH * (float)ri;
            const bool isLastRow = ri + 1 == maxRows;
            const float rowBottom = rowTop + (isLastRow ? resultsLastRowH : rowH);
            const float rowTextY = rowTop + resultsTextOffsetY;

            if (ri + 1 < maxRows || drawTrailingResultsDivider) {
                drawClippedHorizontalLine(surfaceLeft, oy, surfaceRight, oy + totalH, rowBottom, resultsDividerCol);
            }

            float rowX = tableX;
            for (int ci = 0; ci < numCols; ++ci) {
                Col& col = cols[ci];
                if (ri < col.rowCount) {
                    drawCenteredSegmentedText(rowX, col.width, rowTextY, col.rows[ri].text, col.rows[ri].color,
                                             col.rows[ri].xOffset, col.rows[ri].part1Color, col.rows[ri].centerOnPart1, col.leftAlign);
                }
                rowX += col.width + (ci < numCols - 1 ? colGap : 0.0f);
            }
        }

        contentBottomY = headerSeparatorY + resultsBodyH + resultsMarginBottom;
    }

    if (hasInformationMessages && infoPlacement == "middle") {
        contentBottomY = drawInfoMessagesBlock(
            surfaceLeft,
            surfaceRight,
            contentAreaX + infoMarginLeft,
            contentBottomY,
            infoContentW,
            headerDividerCol,
            throwsTextCol);
    }

    if (showDetailPanel) {
        const float sectionY = contentBottomY + sectionGap;
        const float sectionTextY = sectionY + (sectionHeaderH - lineH) * 0.5f;
        const float detailTableX = contentAreaX + throwsMarginLeft;
        const float detailHeaderY = sectionY + sectionHeaderH;
        const float detailRowY = detailHeaderY + detailHeaderH;
        const float detailRowsBottomY = detailRowY + detailRowH * (float)detailRenderRowCount;

        drawRoundedBand(ImVec2(surfaceLeft, sectionY), ImVec2(surfaceRight, detailRowY), headerFillCol,
                touchesPanelEdge(sectionY, oy), false);
        drawRoundedBand(ImVec2(surfaceLeft, detailRowY), ImVec2(surfaceRight, detailRowsBottomY), throwsBgCol,
                false, touchesPanelEdge(detailRowsBottomY, oy + totalH));
        drawText(fs, ImVec2(detailTableX, sectionTextY), dataCol, throwSectionTitle);

        float detailX = detailTableX;
        for (int fi = 0; fi < 4; ++fi) {
            drawCenteredSegmentedText(detailX, detailColWidths[fi], detailHeaderY + (detailHeaderH - lineH) * 0.5f,
                                     detailHeaders[fi], textCol, 0.0f, textCol, false, false);
            detailX += detailColWidths[fi];
        }

        drawClippedHorizontalLine(surfaceLeft, oy, surfaceRight, oy + totalH, sectionY, headerDividerCol);
        drawClippedHorizontalLine(surfaceLeft, oy, surfaceRight, oy + totalH, detailRowY, headerDividerCol);
        for (int ri = 0; ri < detailRenderRowCount; ++ri) {
            const float rowTop = detailRowY + detailRowH * (float)ri;
            const float rowBottom = rowTop + detailRowH;
            const float rowTextY = rowTop + (detailRowH - lineH) * 0.5f;
            float detailRowX = detailTableX;

            drawCenteredSegmentedText(detailRowX, detailColWidths[0], rowTextY, detailRows[ri].x, throwsTextCol, 0.0f, throwsTextCol, false, false);
            detailRowX += detailColWidths[0];
            drawCenteredSegmentedText(detailRowX, detailColWidths[1], rowTextY, detailRows[ri].z, throwsTextCol, 0.0f, throwsTextCol, false, false);
            detailRowX += detailColWidths[1];
            drawCenteredSegmentedText(detailRowX, detailColWidths[2], rowTextY, detailRows[ri].angle,
                                     detailRows[ri].angleColor, detailRows[ri].angleSplitOffset, detailRows[ri].anglePart1Color,
                                     detailRows[ri].angleCenterOnPart1, false);
            detailRowX += detailColWidths[2];
            drawCenteredSegmentedText(detailRowX, detailColWidths[3], rowTextY, detailRows[ri].error, throwsTextCol, 0.0f, throwsTextCol, false, false);

            drawClippedHorizontalLine(surfaceLeft, oy, surfaceRight, oy + totalH, rowBottom, dividerCol);
        }

        contentBottomY = detailRowsBottomY + throwsMarginBottom;
    }

    if (hasInformationMessages && infoPlacement == "bottom") {
        drawInfoMessagesBlock(
            surfaceLeft,
            surfaceRight,
            contentAreaX + infoMarginLeft,
            contentBottomY,
            infoContentW,
            headerDividerCol,
            throwsTextCol);
    }

}
