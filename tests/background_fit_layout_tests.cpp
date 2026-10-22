#include "render/background_fit_layout.h"

#include <cstring>
#include <functional>
#include <iostream>
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

void CheckRect(const BackgroundFitRect& r, int left, int bottom, int right, int top, const std::string& label) {
    CheckIntEq(r.left, left, label + ".left");
    CheckIntEq(r.bottom, bottom, label + ".bottom");
    CheckIntEq(r.right, right, label + ".right");
    CheckIntEq(r.top, top, label + ".top");
}

void ParseNormalizesCaseAndUnknown() {
    Check(ParseBackgroundImageFit("fill") == BackgroundImageFit::Fill, "fill");
    Check(ParseBackgroundImageFit("fit") == BackgroundImageFit::Fit, "fit");
    Check(ParseBackgroundImageFit("stretch") == BackgroundImageFit::Stretch, "stretch");
    Check(ParseBackgroundImageFit("center") == BackgroundImageFit::Center, "center");
    Check(ParseBackgroundImageFit("tile") == BackgroundImageFit::Tile, "tile");
    Check(ParseBackgroundImageFit("STRETCH") == BackgroundImageFit::Stretch, "uppercase normalizes");
    Check(ParseBackgroundImageFit("Center") == BackgroundImageFit::Center, "mixed case normalizes");
    Check(ParseBackgroundImageFit("bogus") == BackgroundImageFit::Stretch, "unknown falls back to stretch");
    Check(ParseBackgroundImageFit("") == BackgroundImageFit::Stretch, "empty falls back to stretch");
}

void ToStringRoundTrips() {
    const BackgroundImageFit all[] = { BackgroundImageFit::Fill, BackgroundImageFit::Fit, BackgroundImageFit::Stretch,
                                       BackgroundImageFit::Center, BackgroundImageFit::Tile };
    for (BackgroundImageFit fit : all) {
        Check(ParseBackgroundImageFit(BackgroundImageFitToString(fit)) == fit, "round-trip through string");
    }
    Check(std::strcmp(BackgroundImageFitToString(BackgroundImageFit::Stretch), "stretch") == 0, "stretch token");
    Check(std::strcmp(BackgroundImageFitToString(BackgroundImageFit::Tile), "tile") == 0, "tile token");
}

void BackdropOnlyForVisibleModes() {
    Check(BackgroundImageFitShowsBackdrop(BackgroundImageFit::Fit), "fit shows backdrop");
    Check(BackgroundImageFitShowsBackdrop(BackgroundImageFit::Center), "center shows backdrop");
    Check(BackgroundImageFitShowsBackdrop(BackgroundImageFit::Tile), "tile shows backdrop");
    Check(!BackgroundImageFitShowsBackdrop(BackgroundImageFit::Fill), "fill hides backdrop");
    Check(!BackgroundImageFitShowsBackdrop(BackgroundImageFit::Stretch), "stretch hides backdrop");
}

void StretchFillsWholeArea() {
    BackgroundFitRect r = ResolveBackgroundImageDestRect(BackgroundImageFit::Stretch, 1.0f, 100, 100, 200, 100);
    CheckRect(r, 0, 0, 200, 100, "stretch");
}

void FitLetterboxesAndCenters() {
    BackgroundFitRect r = ResolveBackgroundImageDestRect(BackgroundImageFit::Fit, 1.0f, 100, 100, 200, 100);
    CheckRect(r, 50, 0, 150, 100, "fit");
}

void FillCoversAndCrops() {
    BackgroundFitRect r = ResolveBackgroundImageDestRect(BackgroundImageFit::Fill, 1.0f, 100, 100, 200, 100);
    CheckRect(r, 0, -50, 200, 150, "fill");
}

void CenterUsesNativeSizeByDefault() {
    BackgroundFitRect r = ResolveBackgroundImageDestRect(BackgroundImageFit::Center, 1.0f, 100, 100, 200, 100);
    CheckRect(r, 50, 0, 150, 100, "center default");
}

void CenterScaleAdjustsSize() {
    BackgroundFitRect r = ResolveBackgroundImageDestRect(BackgroundImageFit::Center, 2.0f, 100, 100, 200, 100);
    CheckRect(r, 0, -50, 200, 150, "center 2x");
    BackgroundFitRect z = ResolveBackgroundImageDestRect(BackgroundImageFit::Center, 0.0f, 100, 100, 200, 100);
    Check(z.right - z.left >= 1 && z.top - z.bottom >= 1, "center zero scale clamped");
    BackgroundFitRect big = ResolveBackgroundImageDestRect(BackgroundImageFit::Center, 100.0f, 100, 100, 200, 100);
    CheckRect(big, -100, -150, 300, 250, "center scale capped at 400%");
}

void DegenerateDimsFallBackToArea() {
    BackgroundFitRect r = ResolveBackgroundImageDestRect(BackgroundImageFit::Fit, 1.0f, 0, 100, 200, 100);
    CheckRect(r, 0, 0, 200, 100, "zero image width");
    BackgroundFitRect r2 = ResolveBackgroundImageDestRect(BackgroundImageFit::Center, 1.0f, 100, 100, 0, 100);
    CheckRect(r2, 0, 0, 0, 100, "zero area width");
}

void TileGridCoversAreaCentered() {
    std::vector<BackgroundFitRect> tiles = ComputeBackgroundTileRects(1.0f, 0, 100, 100, 300, 300);
    CheckIntEq(static_cast<int>(tiles.size()), 9, "3x3 tile count");
    bool hasOrigin = false;
    bool hasCorner = false;
    for (const BackgroundFitRect& t : tiles) {
        if (t.left == 0 && t.bottom == 0 && t.right == 100 && t.top == 100) { hasOrigin = true; }
        if (t.left == 200 && t.bottom == 200 && t.right == 300 && t.top == 300) { hasCorner = true; }
    }
    Check(hasOrigin, "tile at origin present");
    Check(hasCorner, "tile at far corner present");
}

void TileSpacingReducesCount() {
    std::vector<BackgroundFitRect> dense = ComputeBackgroundTileRects(1.0f, 0, 100, 100, 300, 300);
    std::vector<BackgroundFitRect> spaced = ComputeBackgroundTileRects(1.0f, 50, 100, 100, 300, 300);
    Check(!spaced.empty(), "spaced tiles non-empty");
    Check(spaced.size() <= dense.size(), "spacing does not increase tile count");
}

void TileDegenerateAndCapped() {
    Check(ComputeBackgroundTileRects(1.0f, 0, 100, 100, 0, 300).empty(), "zero area yields no tiles");
    Check(ComputeBackgroundTileRects(1.0f, 0, 0, 100, 300, 300).empty(), "zero image yields no tiles");
    std::vector<BackgroundFitRect> capped = ComputeBackgroundTileRects(0.05f, 0, 1, 1, 5000, 5000);
    Check(static_cast<int>(capped.size()) <= 4096, "tile count is capped");
    std::vector<BackgroundFitRect> hugeSpacing = ComputeBackgroundTileRects(1.0f, 2000000000, 100, 100, 300, 300);
    Check(static_cast<int>(hugeSpacing.size()) >= 1, "huge spacing clamps without overflow");
}

struct TestCase {
    const char* name;
    std::function<void()> run;
};

const std::vector<TestCase>& Registry() {
    static const std::vector<TestCase> cases = {
        {"parse_normalizes_case_and_unknown", &ParseNormalizesCaseAndUnknown},
        {"to_string_round_trips", &ToStringRoundTrips},
        {"backdrop_only_for_visible_modes", &BackdropOnlyForVisibleModes},
        {"stretch_fills_whole_area", &StretchFillsWholeArea},
        {"fit_letterboxes_and_centers", &FitLetterboxesAndCenters},
        {"fill_covers_and_crops", &FillCoversAndCrops},
        {"center_uses_native_size_by_default", &CenterUsesNativeSizeByDefault},
        {"center_scale_adjusts_size", &CenterScaleAdjustsSize},
        {"degenerate_dims_fall_back_to_area", &DegenerateDimsFallBackToArea},
        {"tile_grid_covers_area_centered", &TileGridCoversAreaCentered},
        {"tile_spacing_reduces_count", &TileSpacingReducesCount},
        {"tile_degenerate_and_capped", &TileDegenerateAndCapped},
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
