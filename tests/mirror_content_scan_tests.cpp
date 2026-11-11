#include "render/mirror_content_scan.h"

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

std::vector<unsigned char> MakePixels(size_t pixelCount) { return std::vector<unsigned char>(pixelCount * 4, 0); }

bool Scan(const std::vector<unsigned char>& pixels) { return MirrorPixelsHaveNonZeroAlpha(pixels.data(), pixels.size() / 4); }

void TestEmptyAndTransparentHaveNoContent() {
    Check(!MirrorPixelsHaveNonZeroAlpha(nullptr, 100), "null buffer has no content");
    for (size_t count : { size_t{ 1 }, kMirrorContentScanBlockPixels, kMirrorContentScanBlockPixels + 1 }) {
        auto pixels = MakePixels(count);
        Check(!Scan(pixels), "all-zero buffer of " + std::to_string(count) + " pixels has no content");
        for (size_t index = 0; index < count; ++index) {
            pixels[index * 4 + 0] = 255;
            pixels[index * 4 + 1] = 255;
            pixels[index * 4 + 2] = 255;
        }
        Check(!Scan(pixels), "white with alpha 0 over " + std::to_string(count) + " pixels has no content");
    }
}

void TestSingleAlphaPixelFoundAtBlockBoundaries() {
    const size_t block = kMirrorContentScanBlockPixels;
    const size_t count = block * 3 + 5;
    for (size_t position : { size_t{ 0 }, block - 1, block, block + 1, block * 2 - 1, block * 2, count - 1 }) {
        auto pixels = MakePixels(count);
        pixels[position * 4 + 3] = 1;
        Check(Scan(pixels), "alpha 1 at pixel " + std::to_string(position) + " of " + std::to_string(count) + " is content");
    }
}

void TestOnlyAlphaCountsWithinRequestedPixels() {
    auto pixels = MakePixels(600);
    for (size_t channel = 0; channel < 3; ++channel) {
        pixels.assign(600 * 4, 0);
        pixels[300 * 4 + channel] = 200;
        Check(!Scan(pixels), "channel " + std::to_string(channel) + " alone is not content");
    }

    pixels.assign(600 * 4, 0);
    pixels[99 * 4 + 3] = 255;
    Check(!MirrorPixelsHaveNonZeroAlpha(pixels.data(), 99), "alpha past the requested pixel count is ignored");
    Check(MirrorPixelsHaveNonZeroAlpha(pixels.data(), 100), "alpha inside the requested pixel count is found");
}

struct TestCase {
    const char* name;
    std::function<void()> run;
};

const std::vector<TestCase>& Registry() {
    static const std::vector<TestCase> cases = {
        { "empty_and_transparent_have_no_content", TestEmptyAndTransparentHaveNoContent },
        { "single_alpha_pixel_found_at_block_boundaries", TestSingleAlphaPixelFoundAtBlockBoundaries },
        { "only_alpha_counts_within_requested_pixels", TestOnlyAlphaCountsWithinRequestedPixels },
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
