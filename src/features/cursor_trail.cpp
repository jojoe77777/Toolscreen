#include "cursor_trail.h"

#ifndef GLEW_STATIC
#define GLEW_STATIC
#endif
#include <GL/glew.h>

#include "gui/gui.h"
#include "common/utils.h"
#include "common/gl_overlay.h"
#include "third_party/stb_image.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>
#include <windows.h>

void Log(const std::string& msg);

namespace {

constexpr int kMaxStamps = 512;
constexpr int kMaxStampsPerFrame = kMaxStamps / 4;
constexpr int kMaxSpritePixels = 256;
constexpr int kProceduralSpriteSize = 64;
constexpr uint64_t kMaxSampleGapMs = 100;

struct TrailStamp {
    float x = 0.0f;
    float y = 0.0f;
    uint64_t birthTimeMs = 0;
    float sizeBoost = 1.0f;
};

struct TrailState {
    std::array<TrailStamp, kMaxStamps> stamps{};
    int head = 0;
    int count = 0;

    std::array<std::pair<float, float>, 3> samples{};
    int sampleCount = 0;
    uint64_t lastCallTimeMs = 0;

    DWORD lastThreadId = 0;
    int lastWindowWidth = 0;
    int lastWindowHeight = 0;
    uint64_t lastUpdateFrameTag = 0;

    GLuint texture = 0;
    std::string cachedSpritePath;
    bool spriteLoaded = false;

    gloverlay::QuadBatch batch;
    std::vector<float> vertexScratch;
};

TrailState g_trail;

void PushVertex(std::vector<float>& buf, float x, float y, float u, float v, float r, float g, float b, float a) {
    buf.push_back(x);
    buf.push_back(y);
    buf.push_back(u);
    buf.push_back(v);
    buf.push_back(r);
    buf.push_back(g);
    buf.push_back(b);
    buf.push_back(a);
}

uint64_t NowMs() {
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    static const LARGE_INTEGER freq = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return value;
    }();
    if (freq.QuadPart == 0) { return 0; }
    return static_cast<uint64_t>((counter.QuadPart * 1000LL) / freq.QuadPart);
}

void ResetTrailBuffer() {
    g_trail.head = 0;
    g_trail.count = 0;
    g_trail.sampleCount = 0;
    g_trail.lastUpdateFrameTag = 0;
}

bool ReadFileBytes(const std::wstring& widePath, std::vector<unsigned char>& out) {
    HANDLE file = CreateFileW(widePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { return false; }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 32 * 1024 * 1024) {
        CloseHandle(file);
        return false;
    }

    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD bytesRead = 0;
    const BOOL ok = ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &bytesRead, nullptr);
    CloseHandle(file);
    if (!ok || bytesRead != out.size()) { return false; }
    return true;
}

std::vector<unsigned char> BuildProceduralDot() {
    const int w = kProceduralSpriteSize;
    const int h = kProceduralSpriteSize;
    std::vector<unsigned char> pixels(static_cast<size_t>(w) * h * 4, 0);
    const float cx = (w - 1) * 0.5f;
    const float cy = (h - 1) * 0.5f;
    const float radius = (w * 0.5f) - 1.0f;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float dx = x - cx;
            const float dy = y - cy;
            const float d = std::sqrt(dx * dx + dy * dy) / radius;
            float a = 1.0f - std::clamp(d, 0.0f, 1.0f);
            a = a * a;
            const size_t idx = (static_cast<size_t>(y) * w + x) * 4;
            pixels[idx + 0] = 255;
            pixels[idx + 1] = 255;
            pixels[idx + 2] = 255;
            pixels[idx + 3] = static_cast<unsigned char>(std::clamp(a * 255.0f, 0.0f, 255.0f));
        }
    }
    return pixels;
}

void UploadTexture(const unsigned char* rgba, int w, int h) {
    if (g_trail.texture == 0) { glGenTextures(1, &g_trail.texture); }
    if (g_trail.texture == 0) { return; }
    BindTextureDirect(GL_TEXTURE_2D, g_trail.texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void EnsureSpriteLoaded(const std::string& spritePath) {
    if (g_trail.spriteLoaded && g_trail.cachedSpritePath == spritePath && g_trail.texture != 0) { return; }

    bool loadedFromFile = false;
    if (!spritePath.empty()) {
        std::vector<unsigned char> bytes;
        const std::wstring widePath = Utf8ToWide(spritePath);
        if (ReadFileBytes(widePath, bytes)) {
            int w = 0;
            int h = 0;
            int channels = 0;
            unsigned char* pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h,
                                                          &channels, 4);
            if (pixels) {
                if (w > 0 && h > 0 && w <= kMaxSpritePixels && h <= kMaxSpritePixels) {
                    UploadTexture(pixels, w, h);
                    loadedFromFile = true;
                } else {
                    Log("[CursorTrail] Sprite '" + spritePath + "' exceeds " + std::to_string(kMaxSpritePixels) +
                        "px max, falling back to default");
                }
                stbi_image_free(pixels);
            } else {
                Log("[CursorTrail] stb_image failed to decode '" + spritePath + "', falling back to default");
            }
        } else {
            Log("[CursorTrail] Could not open sprite '" + spritePath + "', falling back to default");
        }
    }

    if (!loadedFromFile) {
        const std::vector<unsigned char> dot = BuildProceduralDot();
        UploadTexture(dot.data(), kProceduralSpriteSize, kProceduralSpriteSize);
    }

    g_trail.cachedSpritePath = spritePath;
    g_trail.spriteLoaded = true;
}

void EmitStampsAlongQuadBezier(float p0x, float p0y, float p1x, float p1y, float p2x, float p2y,
                               int spacingPx, uint64_t now, float sizeBoost) {
    const float cx = p2x - p0x;
    const float cy = p2y - p0y;
    const float chord = std::sqrt(cx * cx + cy * cy);
    const float minSpacing = static_cast<float>(std::max(1, spacingPx));
    if (chord < minSpacing) { return; }

    int steps = static_cast<int>(chord / minSpacing);
    if (steps < 1) { steps = 1; }
    if (steps > kMaxStampsPerFrame) { steps = kMaxStampsPerFrame; }

    for (int i = 1; i <= steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const float omt = 1.0f - t;
        const float a = omt * omt;
        const float b = 2.0f * omt * t;
        const float c = t * t;
        const float sx = a * p0x + b * p1x + c * p2x;
        const float sy = a * p0y + b * p1y + c * p2y;
        TrailStamp& slot = g_trail.stamps[g_trail.head];
        slot.x = sx;
        slot.y = sy;
        slot.birthTimeMs = now;
        slot.sizeBoost = sizeBoost;
        g_trail.head = (g_trail.head + 1) % kMaxStamps;
        if (g_trail.count < kMaxStamps) { g_trail.count++; }
    }
}

} // namespace

void RenderCursorTrail(HWND hwnd, int windowWidth, int windowHeight, const CursorTrailConfig& cfg, uint64_t frameTag) {
    if (!cfg.enabled) {
        ResetTrailBuffer();
        return;
    }
    if (windowWidth <= 0 || windowHeight <= 0) { return; }

    const DWORD currentTid = GetCurrentThreadId();
    if (g_trail.lastThreadId != currentTid) {
        ResetTrailBuffer();
        g_trail.lastThreadId = currentTid;
        g_trail.spriteLoaded = false;
        g_trail.texture = 0;
    }
    if (g_trail.lastWindowWidth != windowWidth || g_trail.lastWindowHeight != windowHeight) {
        ResetTrailBuffer();
        g_trail.lastWindowWidth = windowWidth;
        g_trail.lastWindowHeight = windowHeight;
    }

    POINT cursorPos{};
    if (!GetCursorPos(&cursorPos)) { return; }
    if (!ScreenToClient(hwnd, &cursorPos)) { return; }

    RECT clientRect{};
    if (!GetClientRect(hwnd, &clientRect)) { return; }
    const int gameWidth = clientRect.right - clientRect.left;
    const int gameHeight = clientRect.bottom - clientRect.top;
    if (gameWidth <= 0 || gameHeight <= 0) { return; }

    const float scaleX = static_cast<float>(windowWidth) / static_cast<float>(gameWidth);
    const float scaleY = static_cast<float>(windowHeight) / static_cast<float>(gameHeight);
    const float curX = static_cast<float>(cursorPos.x) * scaleX;
    const float curY = static_cast<float>(cursorPos.y) * scaleY;

    const uint64_t now = NowMs();
    const int lifetimeMs = std::max(1, cfg.lifetimeMs);

    const bool shouldAdvanceState = frameTag == 0 || g_trail.lastUpdateFrameTag != frameTag;
    if (shouldAdvanceState) {
        const uint64_t lastCall = g_trail.lastCallTimeMs;
        g_trail.lastCallTimeMs = now;
        if (frameTag != 0) { g_trail.lastUpdateFrameTag = frameTag; }
        if (lastCall > 0 && now > lastCall && (now - lastCall) > kMaxSampleGapMs) {
            g_trail.sampleCount = 0;
        }

        g_trail.samples[0] = g_trail.samples[1];
        g_trail.samples[1] = g_trail.samples[2];
        g_trail.samples[2] = { curX, curY };
        if (g_trail.sampleCount < 3) { g_trail.sampleCount++; }

        if (g_trail.sampleCount >= 2) {
            const float dxT = g_trail.samples[2].first - g_trail.samples[1].first;
            const float dyT = g_trail.samples[2].second - g_trail.samples[1].second;
            const float distT = std::sqrt(dxT * dxT + dyT * dyT);
            const float screenDiagonal = std::sqrt(
                static_cast<float>(windowWidth) * static_cast<float>(windowWidth) +
                static_cast<float>(windowHeight) * static_cast<float>(windowHeight));
            if (distT > screenDiagonal) {
                g_trail.sampleCount = 0;
            }
        }

        if (g_trail.sampleCount >= 2) {
            const float s1x = g_trail.samples[1].first;
            const float s1y = g_trail.samples[1].second;
            const float s2x = g_trail.samples[2].first;
            const float s2y = g_trail.samples[2].second;

            float ctlX;
            float ctlY;
            if (g_trail.sampleCount >= 3) {
                const float s0x = g_trail.samples[0].first;
                const float s0y = g_trail.samples[0].second;
                ctlX = s1x + (s2x - s0x) * 0.25f;
                ctlY = s1y + (s2y - s0y) * 0.25f;
            } else {
                ctlX = 0.5f * (s1x + s2x);
                ctlY = 0.5f * (s1y + s2y);
            }

            float sizeBoost = 1.0f;
            if (cfg.useVelocitySize && lastCall > 0 && now > lastCall) {
                constexpr float kReferencePxPerMs = 2.0f;
                const float dxV = s2x - s1x;
                const float dyV = s2y - s1y;
                const float distV = std::sqrt(dxV * dxV + dyV * dyV);
                const float dtMs = static_cast<float>(now - lastCall);
                const float velocityPxPerMs = distV / dtMs;
                const float velocityFraction = std::clamp(velocityPxPerMs / kReferencePxPerMs, 0.0f, 1.0f);
                const float intensity = std::clamp(cfg.velocitySizeIntensity, 0.0f, 1.0f);
                sizeBoost = 1.0f + intensity * velocityFraction;
            }

            EmitStampsAlongQuadBezier(s1x, s1y, ctlX, ctlY, s2x, s2y, cfg.stampSpacingPx, now, sizeBoost);
        }
    }

    if (g_trail.count == 0) { return; }

    EnsureSpriteLoaded(cfg.spritePath);
    if (g_trail.texture == 0) { return; }
    if (!g_trail.batch.Ensure()) { return; }

    const float baseSize = static_cast<float>(std::max(1, cfg.spriteSizePx));
    const float tailScale = std::clamp(cfg.tailSizeScale, 0.0f, 2.0f);
    const float opacity = std::clamp(cfg.opacity, 0.0f, 1.0f);
    const float colorR = std::clamp(cfg.color.r, 0.0f, 1.0f);
    const float colorG = std::clamp(cfg.color.g, 0.0f, 1.0f);
    const float colorB = std::clamp(cfg.color.b, 0.0f, 1.0f);
    const float tailColorR = std::clamp(cfg.tailColor.r, 0.0f, 1.0f);
    const float tailColorG = std::clamp(cfg.tailColor.g, 0.0f, 1.0f);
    const float tailColorB = std::clamp(cfg.tailColor.b, 0.0f, 1.0f);
    const bool useGradient = cfg.useGradient;

    const float invHalfW = 2.0f / static_cast<float>(windowWidth);
    const float invHalfH = 2.0f / static_cast<float>(windowHeight);
    auto toClipX = [invHalfW](float px) { return px * invHalfW - 1.0f; };
    auto toClipY = [invHalfH](float py) { return 1.0f - py * invHalfH; };

    std::vector<float>& verts = g_trail.vertexScratch;
    verts.clear();
    verts.reserve(static_cast<size_t>(g_trail.count) * gloverlay::kVerticesPerQuad * gloverlay::kFloatsPerVertex);

    int liveStamps = 0;
    for (int i = 0; i < g_trail.count; ++i) {
        const int ringIdx = (g_trail.head - 1 - i + kMaxStamps) % kMaxStamps;
        const TrailStamp& stamp = g_trail.stamps[ringIdx];
        const uint64_t ageMs = now >= stamp.birthTimeMs ? now - stamp.birthTimeMs : 0;
        if (static_cast<int>(ageMs) >= lifetimeMs) { continue; }
        ++liveStamps;

        const float ageFraction = static_cast<float>(ageMs) / static_cast<float>(lifetimeMs);
        const float invAge = 1.0f - ageFraction;
        const float alphaCurve = invAge * invAge;
        const float alpha = alphaCurve * opacity;
        if (alpha <= 0.001f) { continue; }

        const float sizeScale = (1.0f - ageFraction) + tailScale * ageFraction;
        const float half = baseSize * sizeScale * stamp.sizeBoost * 0.5f;
        if (half <= 0.5f) { continue; }

        const float cx0 = toClipX(stamp.x - half);
        const float cy0 = toClipY(stamp.y - half);
        const float cx1 = toClipX(stamp.x + half);
        const float cy1 = toClipY(stamp.y + half);

        float r = colorR;
        float g = colorG;
        float b = colorB;
        if (useGradient) {
            r = colorR * (1.0f - ageFraction) + tailColorR * ageFraction;
            g = colorG * (1.0f - ageFraction) + tailColorG * ageFraction;
            b = colorB * (1.0f - ageFraction) + tailColorB * ageFraction;
        }

        PushVertex(verts, cx0, cy0, 0.0f, 0.0f, r, g, b, alpha);
        PushVertex(verts, cx1, cy0, 1.0f, 0.0f, r, g, b, alpha);
        PushVertex(verts, cx1, cy1, 1.0f, 1.0f, r, g, b, alpha);
        PushVertex(verts, cx0, cy0, 0.0f, 0.0f, r, g, b, alpha);
        PushVertex(verts, cx1, cy1, 1.0f, 1.0f, r, g, b, alpha);
        PushVertex(verts, cx0, cy1, 0.0f, 1.0f, r, g, b, alpha);
    }

    if (liveStamps == 0) {
        g_trail.count = 0;
        g_trail.head = 0;
    }
    if (verts.empty()) { return; }

    gloverlay::ScopedState glState;
    if (cfg.blendMode == "Additive") { glBlendFunc(GL_SRC_ALPHA, GL_ONE); }

    BindTextureDirect(GL_TEXTURE_2D, g_trail.texture);
    g_trail.batch.Draw(verts.data(), static_cast<GLsizei>(verts.size() / gloverlay::kFloatsPerVertex));
}

