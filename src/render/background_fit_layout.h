#pragma once

#include <string_view>
#include <vector>

enum class BackgroundImageFit { Fill, Fit, Stretch, Center, Tile };

inline constexpr float kBackgroundImageScaleMin = 0.05f;
inline constexpr float kBackgroundImageScaleMax = 4.0f;
inline constexpr int kBackgroundImageSpacingMax = 512;

struct BackgroundFitRect {
    int left = 0;
    int bottom = 0;
    int right = 0;
    int top = 0;
};

BackgroundImageFit ParseBackgroundImageFit(std::string_view value);
const char* BackgroundImageFitToString(BackgroundImageFit fit);
bool BackgroundImageFitShowsBackdrop(BackgroundImageFit fit);

BackgroundFitRect ResolveBackgroundImageDestRect(BackgroundImageFit fit, float centerScale, int imageW, int imageH, int areaW,
                                                 int areaH);

std::vector<BackgroundFitRect> ComputeBackgroundTileRects(float tileScale, int spacing, int imageW, int imageH, int areaW, int areaH);
