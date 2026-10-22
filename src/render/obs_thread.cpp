#include "obs_thread.h"
#include "common/profiler.h"
#include "gui/gui.h"
#include "mirror_thread.h"
#include "common/utils.h"

#include <chrono>
#include <cmath>
#include <array>
#include <mutex>
#include <thread>
#include <vector>

std::atomic<bool> g_obsOverrideEnabled{ false };
std::atomic<GLuint> g_obsOverrideTexture{ 0 };
std::atomic<int> g_obsOverrideWidth{ 0 };
std::atomic<int> g_obsOverrideHeight{ 0 };

std::atomic<bool> g_obsPre113Windowed{ false };
std::atomic<int> g_obsPre113OffsetX{ 0 };
std::atomic<int> g_obsPre113OffsetY{ 0 };
std::atomic<int> g_obsPre113ContentW{ 0 };
std::atomic<int> g_obsPre113ContentH{ 0 };

static std::atomic<bool> g_obsHookInitialized{ false };
static std::atomic<bool> g_obsHookActive{ false };

static GLuint g_obsRedirectFBO = 0;
static std::mutex g_obsHookMutex;
static GLuint g_obsRedirectAttachedTexture = 0;
static int g_obsRedirectAttachedWidth = 0;
static int g_obsRedirectAttachedHeight = 0;

static std::mutex g_obsOverrideSyncMutex;
static GLsync g_obsOverrideProducerFence = nullptr;
static constexpr size_t OBS_OVERRIDE_FENCE_DELETION_DELAY = 64;
static std::array<GLsync, OBS_OVERRIDE_FENCE_DELETION_DELAY> g_obsOverrideProducerFencesToDelete{};
static size_t g_obsOverrideProducerFenceDeleteIndex = 0;

struct ObsOverrideConsumerFence {
    GLuint texture = 0;
    GLsync fence = nullptr;
};

static constexpr size_t OBS_OVERRIDE_CONSUMER_FENCE_COUNT = 8;
static std::array<ObsOverrideConsumerFence, OBS_OVERRIDE_CONSUMER_FENCE_COUNT> g_obsOverrideConsumerFences{};

struct ObsRedirectValidationEntry {
    GLuint texture = 0;
    int width = 0;
    int height = 0;
};

static constexpr size_t OBS_REDIRECT_VALIDATION_CACHE_SIZE = 8;
static ObsRedirectValidationEntry g_obsRedirectValidationCache[OBS_REDIRECT_VALIDATION_CACHE_SIZE]{};
static size_t g_obsRedirectValidationCacheNext = 0;
static std::atomic<uint64_t> g_obsNextTextureUpdateTickUs{ 0 };
static std::atomic<uint64_t> g_obsLastGameCaptureSampleTickUs{ 0 };
static std::atomic<uint64_t> g_obsSmoothedGameCaptureIntervalUs{ 0 };

static constexpr int OBS_TARGET_DEFAULT_FPS = 60;
static constexpr int OBS_TARGET_MIN_FPS = 15;
static constexpr int OBS_TARGET_MAX_FPS = 360;
static constexpr int OBS_TARGET_HEADROOM_FPS = 1;
static constexpr int OBS_LIMITED_TARGET_MIN_FPS = 60;
static constexpr uint64_t OBS_TARGET_MIN_INTERVAL_US = 1000;
static constexpr uint64_t OBS_TARGET_STALE_TIMEOUT_US = 2ull * 1000ull * 1000ull;

static uint64_t GetObsSteadyNowUs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

static int ClampObsTargetFramerateValue(int value) {
    if (value < OBS_TARGET_MIN_FPS) { return OBS_TARGET_MIN_FPS; }
    if (value > OBS_TARGET_MAX_FPS) { return OBS_TARGET_MAX_FPS; }
    return value;
}

static int RoundUpToNearestFive(int value) {
    return ((value + 4) / 5) * 5;
}

static bool ShouldLimitObsCaptureFramerate() {
    auto cfgSnapshot = GetConfigSnapshot();
    if (!cfgSnapshot) { return ConfigDefaults::CONFIG_LIMIT_CAPTURE_FRAMERATE; }
    return cfgSnapshot->limitCaptureFramerate;
}

static uint64_t BlendObsGameCaptureIntervalUs(uint64_t currentUs, uint64_t newUs) {
    if (currentUs == 0) { return newUs; }
    return ((currentUs * 3ull) + newUs) / 4ull;
}

static void RecordObsGameCaptureSample() {
    const uint64_t nowUs = GetObsSteadyNowUs();
    const uint64_t previousUs = g_obsLastGameCaptureSampleTickUs.exchange(nowUs, std::memory_order_acq_rel);
    if (previousUs == 0 || nowUs <= previousUs) { return; }

    const uint64_t intervalUs = nowUs - previousUs;
    if (intervalUs < OBS_TARGET_MIN_INTERVAL_US) { return; }
    if (intervalUs >= OBS_TARGET_STALE_TIMEOUT_US) {
        g_obsSmoothedGameCaptureIntervalUs.store(0, std::memory_order_release);
        return;
    }

    uint64_t expectedUs = g_obsSmoothedGameCaptureIntervalUs.load(std::memory_order_acquire);
    for (;;) {
        const uint64_t desiredUs = BlendObsGameCaptureIntervalUs(expectedUs, intervalUs);
        if (g_obsSmoothedGameCaptureIntervalUs.compare_exchange_weak(expectedUs, desiredUs, std::memory_order_acq_rel,
                                                                     std::memory_order_acquire)) {
            return;
        }
    }
}

static int CalculateObsTargetFramerate(uint64_t intervalUs) {
    if (intervalUs == 0) { return OBS_TARGET_DEFAULT_FPS; }

    const double sampledFps = 1000000.0 / static_cast<double>(intervalUs);
    const int fpsWithHeadroom = static_cast<int>(std::ceil(sampledFps + static_cast<double>(OBS_TARGET_HEADROOM_FPS)));
    return ClampObsTargetFramerateValue(RoundUpToNearestFive(fpsWithHeadroom));
}

static int ApplyObsCaptureFramerateLimit(int targetFramerate) {
    if (!ShouldLimitObsCaptureFramerate()) { return targetFramerate; }

    const int halvedFramerate = RoundUpToNearestFive(static_cast<int>(std::ceil(static_cast<double>(targetFramerate) * 0.5)));
    return ClampObsTargetFramerateValue((std::max)(OBS_LIMITED_TARGET_MIN_FPS, halvedFramerate));
}

static bool IsObsRedirectAttachmentValidated(GLuint texture, int width, int height) {
    for (const auto& entry : g_obsRedirectValidationCache) {
        if (entry.texture == texture && entry.width == width && entry.height == height) {
            return true;
        }
    }
    return false;
}

static void CacheObsRedirectAttachmentValidation(GLuint texture, int width, int height) {
    g_obsRedirectValidationCache[g_obsRedirectValidationCacheNext] = { texture, width, height };
    g_obsRedirectValidationCacheNext = (g_obsRedirectValidationCacheNext + 1) % OBS_REDIRECT_VALIDATION_CACHE_SIZE;
}

static void ClearObsRedirectAttachmentValidationCache() {
    for (auto& entry : g_obsRedirectValidationCache) {
        entry.texture = 0;
        entry.width = 0;
        entry.height = 0;
    }
    g_obsRedirectValidationCacheNext = 0;
}

static void RetireObsOverrideProducerFenceLocked(GLsync fence) {
    if (!fence) { return; }

    GLsync& delayedFence = g_obsOverrideProducerFencesToDelete[g_obsOverrideProducerFenceDeleteIndex];
    if (delayedFence && glIsSync(delayedFence)) { glDeleteSync(delayedFence); }
    delayedFence = fence;
    g_obsOverrideProducerFenceDeleteIndex =
        (g_obsOverrideProducerFenceDeleteIndex + 1) % OBS_OVERRIDE_FENCE_DELETION_DELAY;
}

static GLsync TakeObsOverrideConsumerFence(GLuint texture) {
    if (texture == 0) { return nullptr; }

    std::lock_guard<std::mutex> lock(g_obsOverrideSyncMutex);
    for (auto& entry : g_obsOverrideConsumerFences) {
        if (entry.texture == texture) {
            GLsync fence = entry.fence;
            entry.texture = 0;
            entry.fence = nullptr;
            return fence;
        }
    }
    return nullptr;
}

static void PublishObsOverrideConsumerFence(GLuint texture) {
    if (texture == 0) { return; }

    GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (!fence) { return; }
    // The producer may be on another context and must be able to see the
    // fence before it considers this texture available for reuse.
    glFlush();

    std::lock_guard<std::mutex> lock(g_obsOverrideSyncMutex);
    ObsOverrideConsumerFence* freeEntry = nullptr;
    for (auto& entry : g_obsOverrideConsumerFences) {
        if (entry.texture == texture) {
            // A later fence in this context also covers the earlier sample.
            if (entry.fence && glIsSync(entry.fence)) { glDeleteSync(entry.fence); }
            entry.fence = fence;
            return;
        }
        if (!freeEntry && entry.texture == 0) { freeEntry = &entry; }
    }

    if (freeEntry) {
        freeEntry->texture = texture;
        freeEntry->fence = fence;
        return;
    }

    // This should only be reachable after a resize/reinitialization churn.
    // Keep the newest fence rather than leaking it.
    if (glIsSync(fence)) { glDeleteSync(fence); }
}

void WaitForObsOverrideTextureConsumer(GLuint texture) {
    GLsync fence = TakeObsOverrideConsumerFence(texture);
    if (!fence) { return; }

    if (glIsSync(fence)) { glWaitSync(fence, 0, GL_TIMEOUT_IGNORED); }
    if (glIsSync(fence)) { glDeleteSync(fence); }
}

bool ShouldUpdateObsTextureNow() {
    const uint64_t nowUs = GetObsSteadyNowUs();
    const int targetFramerate = GetObsTargetFramerate();
    const uint64_t intervalUs = (1000000ull + static_cast<uint64_t>(targetFramerate) - 1ull) /
                                static_cast<uint64_t>(targetFramerate);

    uint64_t expectedUs = g_obsNextTextureUpdateTickUs.load(std::memory_order_acquire);
    for (;;) {
        if (expectedUs != 0 && nowUs < expectedUs) { return false; }

        const uint64_t desiredUs = nowUs + intervalUs;
        if (g_obsNextTextureUpdateTickUs.compare_exchange_weak(expectedUs, desiredUs, std::memory_order_acq_rel,
                                                               std::memory_order_acquire)) {
            return true;
        }
    }
}

int GetObsTargetFramerate() {
    const uint64_t lastSampleUs = g_obsLastGameCaptureSampleTickUs.load(std::memory_order_acquire);
    const uint64_t smoothedIntervalUs = g_obsSmoothedGameCaptureIntervalUs.load(std::memory_order_acquire);
    if (lastSampleUs == 0 || smoothedIntervalUs == 0) { return ApplyObsCaptureFramerateLimit(OBS_TARGET_DEFAULT_FPS); }

    const uint64_t nowUs = GetObsSteadyNowUs();
    if (nowUs > lastSampleUs && (nowUs - lastSampleUs) >= OBS_TARGET_STALE_TIMEOUT_US) {
        return ApplyObsCaptureFramerateLimit(OBS_TARGET_DEFAULT_FPS);
    }

    return ApplyObsCaptureFramerateLimit(CalculateObsTargetFramerate(smoothedIntervalUs));
}

void ResetObsTextureUpdateSchedule() {
    g_obsNextTextureUpdateTickUs.store(0, std::memory_order_release);
}

static GLuint SelectObsRedirectTexture(GLsync& outFence, bool& outNeedsFenceWait, int& outWidth, int& outHeight) {
    outFence = nullptr;
    outNeedsFenceWait = false;
    outWidth = 0;
    outHeight = 0;

    std::lock_guard<std::mutex> lock(g_obsOverrideSyncMutex);
    const GLuint overrideTexture = g_obsOverrideTexture.load(std::memory_order_acquire);
    if (overrideTexture != 0) {
        outWidth = g_obsOverrideWidth.load(std::memory_order_acquire);
        outHeight = g_obsOverrideHeight.load(std::memory_order_acquire);
        outFence = g_obsOverrideProducerFence;
        outNeedsFenceWait = outFence != nullptr;
        return overrideTexture;
    }

    return 0;
}

bool TryObsBlitFramebufferRedirect(GLint readFBO,
                                   GLint srcX0,
                                   GLint srcY0,
                                   GLint srcX1,
                                   GLint srcY1,
                                   GLint dstX0,
                                   GLint dstY0,
                                   GLint dstX1,
                                   GLint dstY1,
                                   GLbitfield mask,
                                   GLenum filter) {
    if (!g_obsOverrideEnabled.load(std::memory_order_acquire)) {
        return false;
    }

    if (readFBO != 0) {
        return false;
    }

    RecordObsGameCaptureSample();

    GLsync obsFence = nullptr;
    bool needsFenceWait = false;
    int overrideWidth = 0;
    int overrideHeight = 0;
    GLuint obsTexture = SelectObsRedirectTexture(obsFence, needsFenceWait, overrideWidth, overrideHeight);
    const bool usingOverrideTexture = (obsTexture != 0 && overrideWidth > 0 && overrideHeight > 0);
    const bool mustReattachOverride =
        usingOverrideTexture &&
        (g_obsRedirectAttachedTexture != obsTexture || g_obsRedirectAttachedWidth != overrideWidth ||
         g_obsRedirectAttachedHeight != overrideHeight);

    if (obsTexture == 0) {
        return false;
    }

    PROFILE_SCOPE_CAT("OBS Capture Redirect", "OBS Hook");

    if (needsFenceWait && obsFence && glIsSync(obsFence)) { glWaitSync(obsFence, 0, GL_TIMEOUT_IGNORED); }

    if (needsFenceWait) { glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT); }

    if (g_obsRedirectFBO == 0) {
        glGenFramebuffers(1, &g_obsRedirectFBO);
        g_obsRedirectAttachedTexture = 0;
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_obsRedirectFBO);
    if (g_obsRedirectAttachedTexture != obsTexture || mustReattachOverride) {
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, obsTexture, 0);

        if (!IsObsRedirectAttachmentValidated(obsTexture, overrideWidth, overrideHeight)) {
            GLenum status = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
            if (status != GL_FRAMEBUFFER_COMPLETE) {
                static GLenum lastLoggedStatus = GL_FRAMEBUFFER_COMPLETE;
                if (status != lastLoggedStatus) {
                    Log("[OBS Hook] WARNING: Redirect FBO incomplete! Status: " + std::to_string(status) +
                        ", Texture: " + std::to_string(obsTexture));
                    lastLoggedStatus = status;
                }
                g_obsRedirectAttachedTexture = 0;
                g_obsRedirectAttachedWidth = 0;
                g_obsRedirectAttachedHeight = 0;
                glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                BlitFramebufferDirect(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
                return true;
            }
            CacheObsRedirectAttachmentValidation(obsTexture, overrideWidth, overrideHeight);
        }

        g_obsRedirectAttachedTexture = obsTexture;
        g_obsRedirectAttachedWidth = overrideWidth;
        g_obsRedirectAttachedHeight = overrideHeight;
    }

    GLint blitSrcX0 = srcX0;
    GLint blitSrcY0 = srcY0;
    GLint blitSrcX1 = srcX1;
    GLint blitSrcY1 = srcY1;
    if (usingOverrideTexture && overrideWidth > 0 && overrideHeight > 0) {
        blitSrcX0 = 0;
        blitSrcY0 = 0;
        blitSrcX1 = overrideWidth;
        blitSrcY1 = overrideHeight;
    } else if (g_obsPre113Windowed.load(std::memory_order_acquire)) {
        int offsetX = g_obsPre113OffsetX.load(std::memory_order_acquire);
        int offsetY = g_obsPre113OffsetY.load(std::memory_order_acquire);
        blitSrcX0 = srcX0 + offsetX;
        blitSrcY0 = srcY0 + offsetY;
        blitSrcX1 = srcX1 + offsetX;
        blitSrcY1 = srcY1 + offsetY;
    }

    BlitFramebufferDirect(blitSrcX0, blitSrcY0, blitSrcX1, blitSrcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);

    // Keep the published texture from being rendered into again until this
    // context has finished sampling it.  This matters when OBS capture runs
    // on a different shared context and the two-texture ring wraps quickly.
    PublishObsOverrideConsumerFence(obsTexture);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    return true;
}

void SetObsOverrideTexture(GLuint texture, int width, int height) {
    GLsync producerFence = nullptr;
    if (texture != 0 && width > 0 && height > 0) {
        producerFence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        // Make both the texture writes and the fence visible to a capture
        // context before publishing the new texture pointer.
        glFlush();
    }

    std::lock_guard<std::mutex> lock(g_obsOverrideSyncMutex);
    RetireObsOverrideProducerFenceLocked(g_obsOverrideProducerFence);
    g_obsOverrideProducerFence = producerFence;
    g_obsOverrideTexture.store(texture, std::memory_order_release);
    g_obsOverrideWidth.store(width, std::memory_order_release);
    g_obsOverrideHeight.store(height, std::memory_order_release);
    g_obsOverrideEnabled.store(true, std::memory_order_release);
}

void ClearObsOverride() {
    g_obsOverrideEnabled.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lock(g_obsOverrideSyncMutex);
    RetireObsOverrideProducerFenceLocked(g_obsOverrideProducerFence);
    g_obsOverrideProducerFence = nullptr;
    g_obsOverrideTexture.store(0, std::memory_order_release);
    g_obsOverrideWidth.store(0, std::memory_order_release);
    g_obsOverrideHeight.store(0, std::memory_order_release);
    ResetObsTextureUpdateSchedule();
    g_obsRedirectAttachedTexture = 0;
    g_obsRedirectAttachedWidth = 0;
    g_obsRedirectAttachedHeight = 0;
    ClearObsRedirectAttachmentValidationCache();
}

void EnableObsOverride() {
    g_obsOverrideEnabled.store(true, std::memory_order_release);
}

bool IsObsHookDetected() {
    return GetModuleHandleA("graphics-hook64.dll") != NULL;
}

void StartObsHookThread() {
    if (g_obsHookActive.load(std::memory_order_acquire)) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_obsHookMutex);
    if (g_obsHookActive.load(std::memory_order_acquire)) {
        return;
    }

    g_obsHookActive.store(true, std::memory_order_release);
    g_obsHookInitialized.store(true, std::memory_order_release);

    g_obsOverrideEnabled.store(true, std::memory_order_release);
    Log("OBS Hook: Activated redirect state on main glBlitFramebuffer hook");
}

void StopObsHookThread() {
    std::lock_guard<std::mutex> lock(g_obsHookMutex);

    g_obsOverrideEnabled.store(false, std::memory_order_release);
    g_obsHookActive.store(false, std::memory_order_release);

    if (g_obsRedirectFBO != 0) {
        glDeleteFramebuffers(1, &g_obsRedirectFBO);
        g_obsRedirectFBO = 0;
        g_obsRedirectAttachedTexture = 0;
    }

    ClearObsRedirectAttachmentValidationCache();

    g_obsHookInitialized.store(false, std::memory_order_release);
    Log("OBS Hook: Stopped");
}



