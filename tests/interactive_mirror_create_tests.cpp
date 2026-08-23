#include "features/interactive_mirror_create.h"

#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "  ASSERT FAILED: " << message << '\n';
        ++g_failures;
    }
}

void CheckIntEq(int actual, int expected, const std::string& label) {
    if (actual != expected) {
        std::cerr << "  ASSERT FAILED: " << label << " expected " << expected << " got " << actual << '\n';
        ++g_failures;
    }
}

void CheckFloatNear(float actual, float expected, float tol, const std::string& label) {
    if (std::fabs(actual - expected) > tol) {
        std::cerr << "  ASSERT FAILED: " << label << " expected " << expected << " got " << actual << '\n';
        ++g_failures;
    }
}

void UnscaledViewportMapsOneToOne() {
    InteractiveRect src{100, 120, 200, 150};
    InteractiveRect dst{400, 300, 200, 150};
    auto p = BuildInteractiveMirrorParams(src, dst, false, 0, 0, 1920, 1080, 1920, 1080, 1920, 1080);
    CheckIntEq(p.captureWidth, 200, "captureWidth");
    CheckIntEq(p.captureHeight, 150, "captureHeight");
    CheckIntEq(p.inputX, 100, "inputX");
    CheckIntEq(p.inputY, 120, "inputY");
    Check(p.captureRelativeTo == "topLeftViewport", "captureRelativeTo viewport");
}

void ScaledViewportDividesByScale() {
    InteractiveRect src{0, 0, 400, 300};
    InteractiveRect dst{0, 0, 400, 300};
    auto p = BuildInteractiveMirrorParams(src, dst, false, 0, 0, 1920, 1080, 960, 540, 1920, 1080);
    CheckIntEq(p.captureWidth, 200, "captureWidth halved");
    CheckIntEq(p.captureHeight, 150, "captureHeight halved");
}

void ViewportOffsetSubtractedFromCapture() {
    InteractiveRect src{300, 200, 100, 100};
    InteractiveRect dst{0, 0, 100, 100};
    auto p = BuildInteractiveMirrorParams(src, dst, false, 100, 50, 800, 600, 800, 600, 1000, 700);
    CheckIntEq(p.inputX, 200, "inputX offset");
    CheckIntEq(p.inputY, 150, "inputY offset");
}

void SourceInLetterboxGivesNegativeInput() {
    InteractiveRect src{20, 30, 100, 100};
    InteractiveRect dst{0, 0, 100, 100};
    auto p = BuildInteractiveMirrorParams(src, dst, false, 100, 0, 800, 600, 800, 600, 1000, 700);
    CheckIntEq(p.inputX, -80, "inputX negative in letterbox");
    CheckIntEq(p.inputY, 30, "inputY");
}

void DestRectKeepsAspectAndCenters() {
    InteractiveRect src{0, 0, 100, 100};
    InteractiveRect dst{0, 0, 300, 250};
    auto p = BuildInteractiveMirrorParams(src, dst, false, 0, 0, 1000, 1000, 1000, 1000, 1000, 1000);
    Check(!p.separateScale, "separateScale false");
    CheckFloatNear(p.scale, 2.5f, 0.001f, "scale = min(3, 2.5)");
    CheckFloatNear(p.scaleX, 2.5f, 0.001f, "scaleX mirrors scale");
    CheckFloatNear(p.scaleY, 2.5f, 0.001f, "scaleY mirrors scale");
    CheckIntEq(p.outputX, 25, "outputX centered");
    CheckIntEq(p.outputY, 0, "outputY centered");
}

void RelativeToScreenSetsFractionAndAnchor() {
    InteractiveRect src{0, 0, 100, 100};
    InteractiveRect dst{480, 270, 200, 200};
    auto p = BuildInteractiveMirrorParams(src, dst, true, 0, 0, 1920, 1080, 1920, 1080, 1920, 1080);
    Check(p.outputRelativeTo == "topLeftScreen", "output anchor screen");
    Check(p.useRelativePosition, "useRelativePosition true");
    CheckIntEq(p.outputX, 480, "outputX screen");
    CheckIntEq(p.outputY, 270, "outputY screen");
    CheckFloatNear(p.relativeX, 0.25f, 0.001f, "relativeX");
    CheckFloatNear(p.relativeY, 0.25f, 0.001f, "relativeY");
}

void FixedPixelsUseViewportAnchor() {
    InteractiveRect src{0, 0, 100, 100};
    InteractiveRect dst{500, 400, 200, 200};
    auto p = BuildInteractiveMirrorParams(src, dst, false, 100, 50, 800, 600, 800, 600, 1000, 700);
    Check(p.outputRelativeTo == "topLeftViewport", "output anchor viewport");
    Check(!p.useRelativePosition, "useRelativePosition false");
    CheckIntEq(p.outputX, 400, "outputX viewport offset");
    CheckIntEq(p.outputY, 350, "outputY viewport offset");
}

void TinySourceClampedToMin() {
    InteractiveRect src{10, 10, 1, 1};
    InteractiveRect dst{0, 0, 100, 100};
    auto p = BuildInteractiveMirrorParams(src, dst, false, 0, 0, 1000, 1000, 1000, 1000, 1000, 1000);
    CheckIntEq(p.captureWidth, InteractiveMirrorLimits::kMinCaptureDimension, "captureWidth clamped");
    CheckIntEq(p.captureHeight, InteractiveMirrorLimits::kMinCaptureDimension, "captureHeight clamped");
}

void HugeDestScaleClamped() {
    InteractiveRect src{0, 0, 10, 10};
    InteractiveRect dst{0, 0, 1000, 1000};
    auto p = BuildInteractiveMirrorParams(src, dst, false, 0, 0, 1000, 1000, 1000, 1000, 1000, 1000);
    Check(!p.separateScale, "separateScale false");
    CheckFloatNear(p.scale, InteractiveMirrorLimits::kMaxScale, 0.001f, "scale clamped");
}

void ExtremeCoordinatesSaturateWithoutOverflow() {
    const int max = std::numeric_limits<int>::max();
    const int min = std::numeric_limits<int>::min();
    InteractiveRect src{max, min, max, max};
    InteractiveRect dst{max, min, max, min};
    auto p = BuildInteractiveMirrorParams(src, dst, false, min, max, 1, 1, max, max, max, max);
    CheckIntEq(p.captureWidth, InteractiveMirrorLimits::kMaxCaptureDimension, "captureWidth bounded");
    CheckIntEq(p.captureHeight, InteractiveMirrorLimits::kMaxCaptureDimension, "captureHeight bounded");
    CheckIntEq(p.inputX, max, "inputX saturated");
    CheckIntEq(p.inputY, min, "inputY saturated");
    CheckIntEq(p.outputX, max, "outputX saturated");
    Check(std::isfinite(p.scale), "scale finite");
    Check(std::isfinite(p.relativeX), "relativeX finite");
    Check(std::isfinite(p.relativeY), "relativeY finite");
}

struct TestCase {
    const char* name;
    std::function<void()> run;
};

const std::vector<TestCase>& Registry() {
    static const std::vector<TestCase> cases = {
        {"unscaled_viewport_maps_one_to_one", &UnscaledViewportMapsOneToOne},
        {"scaled_viewport_divides_by_scale", &ScaledViewportDividesByScale},
        {"viewport_offset_subtracted_from_capture", &ViewportOffsetSubtractedFromCapture},
        {"source_in_letterbox_gives_negative_input", &SourceInLetterboxGivesNegativeInput},
        {"dest_rect_keeps_aspect_and_centers", &DestRectKeepsAspectAndCenters},
        {"relative_to_screen_sets_fraction_and_anchor", &RelativeToScreenSetsFractionAndAnchor},
        {"fixed_pixels_use_viewport_anchor", &FixedPixelsUseViewportAnchor},
        {"tiny_source_clamped_to_min", &TinySourceClampedToMin},
        {"huge_dest_scale_clamped", &HugeDestScaleClamped},
        {"extreme_coordinates_saturate_without_overflow", &ExtremeCoordinatesSaturateWithoutOverflow},
    };
    return cases;
}

int RunNamed(const std::string& name) {
    for (const auto& testCase : Registry()) {
        if (name == testCase.name) {
            g_failures = 0;
            std::cout << "RUN " << name << '\n';
            testCase.run();
            if (g_failures == 0) {
                std::cout << "PASS " << name << '\n';
                return 0;
            }
            std::cerr << "FAIL " << name << " (" << g_failures << " assertion(s))\n";
            return 1;
        }
    }
    std::cerr << "Unknown test case: " << name << '\n';
    return 2;
}

int RunAll() {
    int failed = 0;
    for (const auto& testCase : Registry()) {
        if (RunNamed(testCase.name) != 0) ++failed;
    }
    return failed == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 1 || (argc == 2 && std::strcmp(argv[1], "--run-all") == 0)) {
        return RunAll();
    }
    if (argc == 2 && std::strcmp(argv[1], "--list") == 0) {
        for (const auto& testCase : Registry()) std::cout << testCase.name << '\n';
        return 0;
    }
    if (argc == 3 && std::strcmp(argv[1], "--run") == 0) {
        return RunNamed(argv[2]);
    }
    std::cerr << "Usage: " << argv[0] << " [--run <case> | --run-all | --list]\n";
    return 2;
}
