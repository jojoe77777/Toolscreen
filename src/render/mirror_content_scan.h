#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

inline constexpr size_t kMirrorContentScanBlockPixels = 512;
inline constexpr uint32_t kMirrorContentScanAlphaMask = 0xFF000000u;

inline bool MirrorPixelsHaveNonZeroAlpha(const unsigned char* rgba, size_t pixelCount) {
    if (rgba == nullptr) { return false; }

    for (size_t blockStart = 0; blockStart < pixelCount; blockStart += kMirrorContentScanBlockPixels) {
        const size_t blockEnd = (std::min)(blockStart + kMirrorContentScanBlockPixels, pixelCount);
        uint32_t alphaBits = 0;
        for (size_t pixelIndex = blockStart; pixelIndex < blockEnd; ++pixelIndex) {
            uint32_t pixel = 0;
            std::memcpy(&pixel, rgba + pixelIndex * sizeof(pixel), sizeof(pixel));
            alphaBits |= pixel & kMirrorContentScanAlphaMask;
        }
        if (alphaBits != 0) { return true; }
    }

    return false;
}
