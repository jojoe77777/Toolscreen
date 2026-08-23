#include "features/interactive_mirror_create.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace {

int SaturatingRound(double value) {
    constexpr double kIntMin = static_cast<double>(std::numeric_limits<int>::min());
    constexpr double kIntMax = static_cast<double>(std::numeric_limits<int>::max());
    if (std::isnan(value)) return 0;
    if (!std::isfinite(value)) return value < 0.0 ? std::numeric_limits<int>::min() : std::numeric_limits<int>::max();
    if (value <= kIntMin) return std::numeric_limits<int>::min();
    if (value >= kIntMax) return std::numeric_limits<int>::max();
    return static_cast<int>(std::lround(value));
}

int RoundDiv(int64_t numerator, float scale) {
    if (scale <= 0.0001f) return SaturatingRound(static_cast<double>(numerator));
    return SaturatingRound(static_cast<double>(numerator) / static_cast<double>(scale));
}

int ClampInt(int value, int lo, int hi) {
    return std::clamp(value, lo, hi);
}

float ClampScale(float value) {
    using namespace InteractiveMirrorLimits;
    return std::clamp(value, kMinScale, kMaxScale);
}

}  // namespace

InteractiveMirrorParams BuildInteractiveMirrorParams(const InteractiveRect& sourceRectScreen,
                                                     const InteractiveRect& destRectScreen,
                                                     bool relativeToScreen,
                                                     int finalX,
                                                     int finalY,
                                                     int finalW,
                                                     int finalH,
                                                     int gameW,
                                                     int gameH,
                                                     int fullW,
                                                     int fullH) {
    using namespace InteractiveMirrorLimits;

    const float xScale = (gameW > 0 && finalW > 0) ? static_cast<float>(finalW) / static_cast<float>(gameW) : 1.0f;
    const float yScale = (gameH > 0 && finalH > 0) ? static_cast<float>(finalH) / static_cast<float>(gameH) : 1.0f;

    InteractiveMirrorParams params;

    params.captureWidth = ClampInt(RoundDiv(sourceRectScreen.w, xScale), kMinCaptureDimension, kMaxCaptureDimension);
    params.captureHeight = ClampInt(RoundDiv(sourceRectScreen.h, yScale), kMinCaptureDimension, kMaxCaptureDimension);
    params.inputX = RoundDiv(static_cast<int64_t>(sourceRectScreen.x) - finalX, xScale);
    params.inputY = RoundDiv(static_cast<int64_t>(sourceRectScreen.y) - finalY, yScale);
    params.captureRelativeTo = "topLeftViewport";

    const float fitW = static_cast<float>(destRectScreen.w) / static_cast<float>(params.captureWidth);
    const float fitH = static_cast<float>(destRectScreen.h) / static_cast<float>(params.captureHeight);
    const float fitScale = ClampScale(std::min(fitW, fitH));
    params.separateScale = false;
    params.scale = fitScale;
    params.scaleX = fitScale;
    params.scaleY = fitScale;

    const int contentW = SaturatingRound(static_cast<double>(params.captureWidth) * fitScale);
    const int contentH = SaturatingRound(static_cast<double>(params.captureHeight) * fitScale);
    const int contentX = SaturatingRound(static_cast<double>(destRectScreen.x) +
                                         (static_cast<int64_t>(destRectScreen.w) - contentW) / 2);
    const int contentY = SaturatingRound(static_cast<double>(destRectScreen.y) +
                                         (static_cast<int64_t>(destRectScreen.h) - contentH) / 2);

    if (relativeToScreen) {
        params.outputRelativeTo = "topLeftScreen";
        params.outputX = contentX;
        params.outputY = contentY;
        params.useRelativePosition = true;
    } else {
        params.outputRelativeTo = "topLeftViewport";
        params.outputX = SaturatingRound(static_cast<double>(static_cast<int64_t>(contentX) - finalX));
        params.outputY = SaturatingRound(static_cast<double>(static_cast<int64_t>(contentY) - finalY));
        params.useRelativePosition = false;
    }
    params.relativeX = (fullW > 0) ? static_cast<float>(contentX) / static_cast<float>(fullW) : 0.0f;
    params.relativeY = (fullH > 0) ? static_cast<float>(contentY) / static_cast<float>(fullH) : 0.0f;

    return params;
}
