#include "render/background_fit_layout.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace {

constexpr int kMaxTiles = 4096;

float ClampScale(float scale) {
    if (!(scale > 0.0f)) { return 1.0f; }
    return (std::min)(kBackgroundImageScaleMax, (std::max)(kBackgroundImageScaleMin, scale));
}

int ScaledDimension(int dimension, float scale) {
    return (std::max)(1, static_cast<int>(std::lround(static_cast<float>(dimension) * scale)));
}

bool IEquals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) { return false; }
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) { return false; }
    }
    return true;
}

}

BackgroundImageFit ParseBackgroundImageFit(std::string_view value) {
    if (IEquals(value, "fill")) { return BackgroundImageFit::Fill; }
    if (IEquals(value, "fit")) { return BackgroundImageFit::Fit; }
    if (IEquals(value, "center")) { return BackgroundImageFit::Center; }
    if (IEquals(value, "tile")) { return BackgroundImageFit::Tile; }
    return BackgroundImageFit::Stretch;
}

const char* BackgroundImageFitToString(BackgroundImageFit fit) {
    switch (fit) {
        case BackgroundImageFit::Fill: return "fill";
        case BackgroundImageFit::Fit: return "fit";
        case BackgroundImageFit::Center: return "center";
        case BackgroundImageFit::Tile: return "tile";
        case BackgroundImageFit::Stretch: break;
    }
    return "stretch";
}

bool BackgroundImageFitShowsBackdrop(BackgroundImageFit fit) {
    return fit == BackgroundImageFit::Fit || fit == BackgroundImageFit::Center || fit == BackgroundImageFit::Tile;
}

BackgroundFitRect ResolveBackgroundImageDestRect(BackgroundImageFit fit, float centerScale, int imageW, int imageH, int areaW,
                                                 int areaH) {
    BackgroundFitRect rect{ 0, 0, areaW, areaH };
    if (imageW <= 0 || imageH <= 0 || areaW <= 0 || areaH <= 0 || fit == BackgroundImageFit::Stretch
        || fit == BackgroundImageFit::Tile) {
        return rect;
    }

    int destW = areaW;
    int destH = areaH;
    if (fit == BackgroundImageFit::Center) {
        const float scale = ClampScale(centerScale);
        destW = ScaledDimension(imageW, scale);
        destH = ScaledDimension(imageH, scale);
    } else {
        const float scaleX = static_cast<float>(areaW) / static_cast<float>(imageW);
        const float scaleY = static_cast<float>(areaH) / static_cast<float>(imageH);
        const float scale = (fit == BackgroundImageFit::Fit) ? (std::min)(scaleX, scaleY) : (std::max)(scaleX, scaleY);
        destW = ScaledDimension(imageW, scale);
        destH = ScaledDimension(imageH, scale);
    }

    rect.left = (areaW - destW) / 2;
    rect.bottom = (areaH - destH) / 2;
    rect.right = rect.left + destW;
    rect.top = rect.bottom + destH;
    return rect;
}

std::vector<BackgroundFitRect> ComputeBackgroundTileRects(float tileScale, int spacing, int imageW, int imageH, int areaW,
                                                          int areaH) {
    std::vector<BackgroundFitRect> rects;
    if (imageW <= 0 || imageH <= 0 || areaW <= 0 || areaH <= 0) { return rects; }

    const float scale = ClampScale(tileScale);
    const int tileW = ScaledDimension(imageW, scale);
    const int tileH = ScaledDimension(imageH, scale);
    const int gap = (std::min)(kBackgroundImageSpacingMax, (std::max)(0, spacing));
    const int periodX = tileW + gap;
    const int periodY = tileH + gap;

    const int left0 = areaW / 2 - tileW / 2;
    const int bottom0 = areaH / 2 - tileH / 2;
    const int halfCountX = (areaW / 2) / periodX + 2;
    const int halfCountY = (areaH / 2) / periodY + 2;

    const long long estimate = static_cast<long long>(2 * halfCountX + 1) * (2 * halfCountY + 1);
    rects.reserve(static_cast<size_t>((std::min<long long>)(kMaxTiles, estimate)));

    for (int j = -halfCountY; j <= halfCountY; ++j) {
        const int bottom = bottom0 + j * periodY;
        if (bottom + tileH <= 0 || bottom >= areaH) { continue; }
        for (int i = -halfCountX; i <= halfCountX; ++i) {
            const int left = left0 + i * periodX;
            if (left + tileW <= 0 || left >= areaW) { continue; }
            rects.push_back(BackgroundFitRect{ left, bottom, left + tileW, bottom + tileH });
            if (static_cast<int>(rects.size()) >= kMaxTiles) { return rects; }
        }
    }
    return rects;
}
