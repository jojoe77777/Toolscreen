#pragma once

#ifndef GLEW_STATIC
#define GLEW_STATIC
#endif
#include <GL/glew.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <vector>

inline int GetAnimatedTextureDelayMs(const std::vector<int>& frameDelays, size_t frameIndex) {
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
void InitializeAnimatedTexturePlayback(TextureInstance& inst, size_t frameCount) {
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
AnimatedTextureResolveResult<TextureInstance> ResolveAnimatedTexture(TextureInstance& inst) {
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
