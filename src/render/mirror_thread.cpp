#include "mirror_thread.h"
#include "obs_thread.h"
#include "gui/gui.h"
#include "runtime/logic_thread.h"
#include "common/profiler.h"
#include "render.h"
#include "common/utils.h"
#include <array>
#include <algorithm>
#include <chrono>
#include <unordered_map>

// Updated by UpdateMirrorCaptureConfigs (logic thread) and read by SwapBuffers hook.
std::atomic<int> g_activeMirrorCaptureCount{ 0 };
std::atomic<long long> g_mirrorColorPickerCaptureRequestMs{ 0 };

// Summary for capture throttling: see mirror_thread.h
std::atomic<int> g_activeMirrorCaptureMaxFps{ 0 };

// Shared capture data for the same-thread mirror path.
std::vector<ThreadedMirrorConfig> g_threadedMirrorConfigs;
std::mutex g_threadedMirrorConfigMutex;


// Double-buffered copy textures published from SwapBuffers.
static GLuint g_copyFBO = 0;
static GLuint g_copyTextures[2] = { 0, 0 };
static std::atomic<int> g_copyTextureWriteIndex{ 0 };
static int g_copyTextureW = 0;
static int g_copyTextureH = 0;

// Track the last frame's copy fence for synchronous readers.
static std::atomic<GLsync> g_lastCopyFence{ nullptr };
static std::atomic<int> g_lastCopyReadIndex{ -1 };
static std::atomic<int> g_lastCopyWidth{ 0 };
static std::atomic<int> g_lastCopyHeight{ 0 };
static std::atomic<bool> g_safeReadTextureValid{ false };

// These track the last fully completed frame.
static std::atomic<int> g_readyFrameIndex{ -1 };
static std::atomic<int> g_readyFrameWidth{ 0 };
static std::atomic<int> g_readyFrameHeight{ 0 };

static std::atomic<int> g_globalMirrorGammaMode{ static_cast<int>(MirrorGammaMode::Auto) };

void SetGlobalMirrorGammaMode(MirrorGammaMode mode) {
    g_globalMirrorGammaMode.store(static_cast<int>(mode), std::memory_order_release);
}

MirrorGammaMode GetGlobalMirrorGammaMode() {
    int v = g_globalMirrorGammaMode.load(std::memory_order_acquire);
    if (v < 0 || v > 2) return MirrorGammaMode::Auto;
    return static_cast<MirrorGammaMode>(v);
}

namespace {
class MT_ScopedTextureSamplingGuard {
public:
    MT_ScopedTextureSamplingGuard(GLuint texture, GLint minFilter, GLint magFilter, GLenum textureUnit = GL_TEXTURE0,
                                  bool clearBoundSampler = false)
        : texture_(texture), textureUnit_(textureUnit) {
        if (texture_ == 0) { return; }

        glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture_);
        if (previousActiveTexture_ != static_cast<GLint>(textureUnit_)) {
            glActiveTexture(textureUnit_);
        }

        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTextureBinding_);
        if (static_cast<GLuint>(previousTextureBinding_) != texture_) {
            BindTextureDirect(GL_TEXTURE_2D, texture_);
            restoreTextureBinding_ = true;
        }

        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &previousMinFilter_);
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &previousMagFilter_);

        if (previousMinFilter_ != minFilter) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minFilter);
        }
        if (previousMagFilter_ != magFilter) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magFilter);
        }

        if (clearBoundSampler && (GLEW_VERSION_3_3 || GLEW_ARB_sampler_objects)) {
            glGetIntegerv(GL_SAMPLER_BINDING, &previousSampler_);
            if (previousSampler_ != 0) {
                glBindSampler(static_cast<GLuint>(textureUnit_ - GL_TEXTURE0), 0);
                restoreSampler_ = true;
            }
        }

        if (previousActiveTexture_ != static_cast<GLint>(textureUnit_)) {
            glActiveTexture(previousActiveTexture_);
        }

        active_ = true;
    }

    ~MT_ScopedTextureSamplingGuard() {
        if (!active_) { return; }

        GLint currentActiveTexture = 0;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &currentActiveTexture);
        if (currentActiveTexture != static_cast<GLint>(textureUnit_)) {
            glActiveTexture(textureUnit_);
        }

        GLint currentTextureBinding = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &currentTextureBinding);
        if (static_cast<GLuint>(currentTextureBinding) != texture_) {
            BindTextureDirect(GL_TEXTURE_2D, texture_);
        }

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, previousMinFilter_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, previousMagFilter_);

        if (restoreSampler_) {
            glBindSampler(static_cast<GLuint>(textureUnit_ - GL_TEXTURE0), static_cast<GLuint>(previousSampler_));
        }
        if (restoreTextureBinding_) {
            BindTextureDirect(GL_TEXTURE_2D, static_cast<GLuint>(previousTextureBinding_));
        }

        if (currentActiveTexture != static_cast<GLint>(textureUnit_)) {
            glActiveTexture(currentActiveTexture);
        }
    }

private:
    GLuint texture_ = 0;
    GLenum textureUnit_ = GL_TEXTURE0;
    GLint previousActiveTexture_ = GL_TEXTURE0;
    GLint previousTextureBinding_ = 0;
    GLint previousMinFilter_ = GL_NEAREST;
    GLint previousMagFilter_ = GL_NEAREST;
    GLint previousSampler_ = 0;
    bool restoreTextureBinding_ = false;
    bool restoreSampler_ = false;
    bool active_ = false;
};
} // namespace

// These shaders are created on the current mirror capture path.

static const char* mt_passthrough_vert_shader = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
layout(location = 2) in vec4 aSourceRect;
out vec2 TexCoord;
out vec4 SourceRect;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    TexCoord = aTexCoord;
    SourceRect = aSourceRect;
})";

static const char* mt_filter_frag_shader = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;
in vec4 SourceRect;
uniform sampler2D screenTexture;
uniform int u_gammaMode;
uniform vec3 u_targetColors[8];
uniform int u_targetColorCount;
uniform vec4 outputColor;
uniform float u_sensitivity;

vec3 SRGBToLinear(vec3 c) {
    bvec3 cutoff = lessThanEqual(c, vec3(0.04045));
    vec3 low = c / 12.92;
    vec3 high = pow((c + 0.055) / 1.055, vec3(2.4));
    return mix(high, low, vec3(cutoff));
}
void main() {
    vec2 srcCoord = SourceRect.xy + TexCoord * SourceRect.zw;
    vec3 screenColor = texture(screenTexture, srcCoord).rgb;
    vec3 screenColorLinear = SRGBToLinear(screenColor);
    
    bool matches = false;
    for (int i = 0; i < u_targetColorCount; i++) {
        vec3 targetColorSRGB = u_targetColors[i];
        vec3 targetColorLinear = SRGBToLinear(targetColorSRGB);

        float dist;
        if (u_gammaMode == 2) {
            dist = distance(screenColor, targetColorLinear);
        } else if (u_gammaMode == 1) {
            dist = distance(screenColorLinear, targetColorLinear);
        } else {
            float distSRGB = distance(screenColor, targetColorSRGB);
            float distLinear = distance(screenColorLinear, targetColorLinear);
            dist = min(distSRGB, distLinear);
        }

        if (dist < u_sensitivity) {
            matches = true;
            break;
        }
    }
    
    if (matches) {
        FragColor = outputColor;
    } else {
        FragColor = vec4(0.0, 0.0, 0.0, 0.0);
    }
})";

static const char* mt_filter_passthrough_frag_shader = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;
in vec4 SourceRect;
uniform sampler2D screenTexture;
uniform int u_gammaMode;
uniform vec3 u_targetColors[8];
uniform int u_targetColorCount;
uniform float u_sensitivity;

vec3 SRGBToLinear(vec3 c) {
    bvec3 cutoff = lessThanEqual(c, vec3(0.04045));
    vec3 low = c / 12.92;
    vec3 high = pow((c + 0.055) / 1.055, vec3(2.4));
    return mix(high, low, vec3(cutoff));
}
void main() {
    vec2 srcCoord = SourceRect.xy + TexCoord * SourceRect.zw;
    vec3 screenColor = texture(screenTexture, srcCoord).rgb;
    vec3 screenColorLinear = SRGBToLinear(screenColor);
    
    bool matches = false;
    for (int i = 0; i < u_targetColorCount; i++) {
        vec3 targetColorSRGB = u_targetColors[i];
        vec3 targetColorLinear = SRGBToLinear(targetColorSRGB);

        float dist;
        if (u_gammaMode == 2) {
            dist = distance(screenColor, targetColorLinear);
        } else if (u_gammaMode == 1) {
            dist = distance(screenColorLinear, targetColorLinear);
        } else {
            float distSRGB = distance(screenColor, targetColorSRGB);
            float distLinear = distance(screenColorLinear, targetColorLinear);
            dist = min(distSRGB, distLinear);
        }

        if (dist < u_sensitivity) {
            matches = true;
            break;
        }
    }
    
    if (matches) {
        FragColor = vec4(screenColor, 1.0);
    } else {
        FragColor = vec4(0.0, 0.0, 0.0, 0.0);
    }
})";

static const char* mt_passthrough_frag_shader = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;
in vec4 SourceRect;
uniform sampler2D screenTexture;
void main() {
    vec2 srcCoord = SourceRect.xy + TexCoord * SourceRect.zw;
    // Force alpha=1 to avoid propagating undefined/junk alpha from game textures.
    vec4 c = texture(screenTexture, srcCoord);
    FragColor = vec4(c.rgb, 1.0);
})";

static const char* mt_background_frag_shader = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;
uniform sampler2D backgroundTexture;
uniform float u_opacity;
void main() {
    vec4 texColor = texture(backgroundTexture, TexCoord);
    FragColor = vec4(texColor.rgb, texColor.a * u_opacity);
})";

static const char* mt_render_frag_shader = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;
uniform sampler2D filterTexture;
uniform int u_borderWidth;
uniform vec4 u_outputColor;
uniform vec4 u_borderColor;
uniform vec2 u_screenPixel;

bool hasBorderSample(vec2 coord, vec2 pixel) {
    return texture(filterTexture, coord + vec2(-pixel.x, -pixel.y)).a > 0.001 ||
           texture(filterTexture, coord + vec2(0.0, -pixel.y)).a > 0.001 ||
           texture(filterTexture, coord + vec2(pixel.x, -pixel.y)).a > 0.001 ||
           texture(filterTexture, coord + vec2(-pixel.x, 0.0)).a > 0.001 ||
           texture(filterTexture, coord + vec2(pixel.x, 0.0)).a > 0.001 ||
           texture(filterTexture, coord + vec2(-pixel.x, pixel.y)).a > 0.001 ||
           texture(filterTexture, coord + vec2(0.0, pixel.y)).a > 0.001 ||
           texture(filterTexture, coord + vec2(pixel.x, pixel.y)).a > 0.001;
}

void main() {
    if (texture(filterTexture, TexCoord).a > 0.001) {
        FragColor = u_outputColor;
        return;
    }
    if (u_borderWidth == 1) {
        if (hasBorderSample(TexCoord, u_screenPixel)) {
            FragColor = u_borderColor;
            return;
        }
        discard;
    }
    float maxA = 0.0;
    for (int x = -u_borderWidth; x <= u_borderWidth; x++) {
        for (int y = -u_borderWidth; y <= u_borderWidth; y++) {
            if (x == 0 && y == 0) continue;
            vec2 offset = vec2(x, y) * u_screenPixel;
            maxA = max(maxA, texture(filterTexture, TexCoord + offset).a);
        }
    }
    if (maxA > 0.001) {
        FragColor = u_borderColor;
    } else {
        discard;
    }
})";

static const char* mt_render_passthrough_frag_shader = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;
uniform sampler2D filterTexture;
uniform int u_borderWidth;
uniform vec4 u_borderColor;
uniform vec2 u_screenPixel;

bool hasBorderSample(vec2 coord, vec2 pixel) {
    return texture(filterTexture, coord + vec2(-pixel.x, -pixel.y)).a > 0.001 ||
           texture(filterTexture, coord + vec2(0.0, -pixel.y)).a > 0.001 ||
           texture(filterTexture, coord + vec2(pixel.x, -pixel.y)).a > 0.001 ||
           texture(filterTexture, coord + vec2(-pixel.x, 0.0)).a > 0.001 ||
           texture(filterTexture, coord + vec2(pixel.x, 0.0)).a > 0.001 ||
           texture(filterTexture, coord + vec2(-pixel.x, pixel.y)).a > 0.001 ||
           texture(filterTexture, coord + vec2(0.0, pixel.y)).a > 0.001 ||
           texture(filterTexture, coord + vec2(pixel.x, pixel.y)).a > 0.001;
}

void main() {
    vec4 texColor = texture(filterTexture, TexCoord);
    if (texColor.a > 0.001) {
        FragColor = vec4(texColor.rgb, 1.0);
        return;
    }
    if (u_borderWidth == 1) {
        if (hasBorderSample(TexCoord, u_screenPixel)) {
            FragColor = u_borderColor;
            return;
        }
        discard;
    }
    float maxA = 0.0;
    for (int x = -u_borderWidth; x <= u_borderWidth; x++) {
        for (int y = -u_borderWidth; y <= u_borderWidth; y++) {
            if (x == 0 && y == 0) continue;
            vec2 offset = vec2(x, y) * u_screenPixel;
            maxA = max(maxA, texture(filterTexture, TexCoord + offset).a);
        }
    }
    if (maxA > 0.001) {
        FragColor = u_borderColor;
    } else {
        discard;
    }
})";

static const char* mt_masked_gradient_frag_shader = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;

#define MAX_STOPS 8
#define ANIM_NONE 0
#define ANIM_ROTATE 1
#define ANIM_SLIDE 2
#define ANIM_WAVE 3
#define ANIM_SPIRAL 4
#define ANIM_FADE 5

uniform sampler2D filterTexture;
uniform int u_borderWidth;
uniform vec4 u_borderColor;
uniform vec2 u_screenPixel;
uniform int u_numStops;
uniform vec4 u_stopColors[MAX_STOPS];
uniform float u_stopPositions[MAX_STOPS];
uniform float u_angle;
uniform float u_time;
uniform int u_animationType;
uniform float u_animationSpeed;
uniform bool u_colorFade;

vec4 getGradientColorSeamless(float t) {
    t = fract(t);

    float lastPos = u_stopPositions[u_numStops - 1];
    float firstPos = u_stopPositions[0];
    float wrapSize = (1.0 - lastPos) + firstPos;

    if (t <= firstPos && wrapSize > 0.001) {
        float wrapT = (firstPos - t) / wrapSize;
        return mix(u_stopColors[0], u_stopColors[u_numStops - 1], wrapT);
    }
    if (t >= lastPos && wrapSize > 0.001) {
        float wrapT = (t - lastPos) / wrapSize;
        return mix(u_stopColors[u_numStops - 1], u_stopColors[0], wrapT);
    }

    vec4 color = u_stopColors[0];
    for (int i = 0; i < u_numStops - 1; i++) {
        if (t >= u_stopPositions[i] && t <= u_stopPositions[i + 1]) {
            float segmentT = (t - u_stopPositions[i]) / max(u_stopPositions[i + 1] - u_stopPositions[i], 0.0001);
            color = mix(u_stopColors[i], u_stopColors[i + 1], segmentT);
            break;
        }
    }
    return color;
}

vec4 getGradientColor(float t, float timeOffset) {
    float adjustedT = t;
    if (u_colorFade) {
        adjustedT = fract(t + timeOffset * 0.1);
    }
    adjustedT = clamp(adjustedT, 0.0, 1.0);

    vec4 color = u_stopColors[0];
    for (int i = 0; i < u_numStops - 1; i++) {
        if (adjustedT >= u_stopPositions[i] && adjustedT <= u_stopPositions[i + 1]) {
            float segmentT = (adjustedT - u_stopPositions[i]) / max(u_stopPositions[i + 1] - u_stopPositions[i], 0.0001);
            color = mix(u_stopColors[i], u_stopColors[i + 1], segmentT);
            break;
        }
    }
    if (adjustedT >= u_stopPositions[u_numStops - 1]) {
        color = u_stopColors[u_numStops - 1];
    }
    return color;
}

vec4 getFadeColor(float timeOffset) {
    float cyclePos = fract(timeOffset * 0.1);

    vec4 color = u_stopColors[0];
    for (int i = 0; i < u_numStops - 1; i++) {
        if (cyclePos >= u_stopPositions[i] && cyclePos <= u_stopPositions[i + 1]) {
            float segmentT = (cyclePos - u_stopPositions[i]) / max(u_stopPositions[i + 1] - u_stopPositions[i], 0.0001);
            color = mix(u_stopColors[i], u_stopColors[i + 1], segmentT);
            break;
        }
    }
    if (cyclePos > u_stopPositions[u_numStops - 1]) {
        float wrapRange = 1.0 - u_stopPositions[u_numStops - 1] + u_stopPositions[0];
        float wrapT = (cyclePos - u_stopPositions[u_numStops - 1]) / max(wrapRange, 0.0001);
        color = mix(u_stopColors[u_numStops - 1], u_stopColors[0], wrapT);
    } else if (cyclePos < u_stopPositions[0]) {
        float wrapRange = 1.0 - u_stopPositions[u_numStops - 1] + u_stopPositions[0];
        float wrapT = (u_stopPositions[0] - cyclePos) / max(wrapRange, 0.0001);
        color = mix(u_stopColors[0], u_stopColors[u_numStops - 1], wrapT);
    }
    return color;
}

vec4 sampleGradientColor() {
    vec2 center = vec2(0.5, 0.5);
    vec2 uv = TexCoord - center;
    float effectiveAngle = u_angle;
    float t = 0.0;
    float timeOffset = u_time * u_animationSpeed;

    if (u_animationType == ANIM_NONE) {
        vec2 dir = vec2(cos(u_angle), sin(u_angle));
        t = clamp(dot(uv, dir) + 0.5, 0.0, 1.0);
        return getGradientColor(t, timeOffset);
    }
    if (u_animationType == ANIM_ROTATE) {
        effectiveAngle = u_angle + timeOffset;
        vec2 dir = vec2(cos(effectiveAngle), sin(effectiveAngle));
        t = clamp(dot(uv, dir) + 0.5, 0.0, 1.0);
        return getGradientColor(t, timeOffset);
    }
    if (u_animationType == ANIM_SLIDE) {
        vec2 dir = vec2(cos(u_angle), sin(u_angle));
        t = dot(uv, dir) + 0.5 + timeOffset * 0.2;
        return getGradientColorSeamless(t);
    }
    if (u_animationType == ANIM_WAVE) {
        vec2 dir = vec2(cos(u_angle), sin(u_angle));
        vec2 perpDir = vec2(-sin(u_angle), cos(u_angle));
        float perpPos = dot(uv, perpDir);
        float wave = sin(perpPos * 8.0 + timeOffset * 2.0) * 0.08;
        t = clamp(dot(uv, dir) + 0.5 + wave, 0.0, 1.0);
        return getGradientColor(t, timeOffset);
    }
    if (u_animationType == ANIM_SPIRAL) {
        float dist = length(uv) * 2.0;
        float angle = atan(uv.y, uv.x);
        t = dist + angle / 6.28318 - timeOffset * 0.3;
        return getGradientColorSeamless(t);
    }
    if (u_animationType == ANIM_FADE) {
        return getFadeColor(timeOffset);
    }

    return getGradientColor(0.0, timeOffset);
}

bool hasBorderSample(vec2 coord, vec2 pixel) {
    return texture(filterTexture, coord + vec2(-pixel.x, -pixel.y)).a > 0.001 ||
           texture(filterTexture, coord + vec2(0.0, -pixel.y)).a > 0.001 ||
           texture(filterTexture, coord + vec2(pixel.x, -pixel.y)).a > 0.001 ||
           texture(filterTexture, coord + vec2(-pixel.x, 0.0)).a > 0.001 ||
           texture(filterTexture, coord + vec2(pixel.x, 0.0)).a > 0.001 ||
           texture(filterTexture, coord + vec2(-pixel.x, pixel.y)).a > 0.001 ||
           texture(filterTexture, coord + vec2(0.0, pixel.y)).a > 0.001 ||
           texture(filterTexture, coord + vec2(pixel.x, pixel.y)).a > 0.001;
}

void main() {
    if (texture(filterTexture, TexCoord).a > 0.001) {
        FragColor = sampleGradientColor();
        return;
    }

    if (u_borderWidth <= 0) {
        discard;
    }

    if (u_borderWidth == 1) {
        if (hasBorderSample(TexCoord, u_screenPixel)) {
            FragColor = u_borderColor;
            return;
        }
        discard;
    }

    for (int x = -u_borderWidth; x <= u_borderWidth; x++) {
        for (int y = -u_borderWidth; y <= u_borderWidth; y++) {
            if (x == 0 && y == 0) continue;
            vec2 offset = vec2(float(x), float(y)) * u_screenPixel;
            if (texture(filterTexture, TexCoord + offset).a > 0.001) {
                FragColor = u_borderColor;
                return;
            }
        }
    }

    discard;
})";

static const char* mt_dilate_horizontal_frag_shader = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;
uniform sampler2D sourceTexture;
uniform int u_borderWidth;
uniform vec2 u_screenPixel;
void main() {
    float maxA = 0.0;
    for (int x = -u_borderWidth; x <= u_borderWidth; x++) {
        vec2 offset = vec2(float(x) * u_screenPixel.x, 0.0);
        maxA = max(maxA, texture(sourceTexture, TexCoord + offset).a);
    }
    FragColor = vec4(maxA, 0.0, 0.0, 1.0);
})";

static const char* mt_dilate_vertical_frag_shader = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;
uniform sampler2D sourceTexture;
uniform sampler2D dilateTexture;
uniform int u_borderWidth;
uniform vec2 u_screenPixel;
uniform vec4 u_outputColor;
uniform vec4 u_borderColor;
void main() {
    float centerAlpha = texture(sourceTexture, TexCoord).a;
    if (centerAlpha > 0.001) {
        FragColor = u_outputColor;
        return;
    }
    float maxA = 0.0;
    for (int y = -u_borderWidth; y <= u_borderWidth; y++) {
        vec2 offset = vec2(0.0, float(y) * u_screenPixel.y);
        maxA = max(maxA, texture(dilateTexture, TexCoord + offset).r);
    }
    if (maxA > 0.001) {
        FragColor = u_borderColor;
    } else {
        FragColor = vec4(0.0, 0.0, 0.0, 0.0);
    }
})";

static const char* mt_dilate_vertical_passthrough_frag_shader = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;
uniform sampler2D sourceTexture;
uniform sampler2D dilateTexture;
uniform int u_borderWidth;
uniform vec2 u_screenPixel;
uniform vec4 u_borderColor;
void main() {
    vec4 centerColor = texture(sourceTexture, TexCoord);
    if (centerColor.a > 0.001) {
        FragColor = vec4(centerColor.rgb, 1.0);
        return;
    }
    float maxA = 0.0;
    for (int y = -u_borderWidth; y <= u_borderWidth; y++) {
        vec2 offset = vec2(0.0, float(y) * u_screenPixel.y);
        maxA = max(maxA, texture(dilateTexture, TexCoord + offset).r);
    }
    if (maxA > 0.001) {
        FragColor = u_borderColor;
    } else {
        FragColor = vec4(0.0, 0.0, 0.0, 0.0);
    }
})";

static const char* mt_static_border_frag_shader = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;
uniform int u_shape;
uniform vec4 u_borderColor;
uniform float u_thickness;
uniform float u_radius;
uniform vec2 u_size;

float sdRoundedBox(vec2 p, vec2 b, float r) {
    float maxR = min(b.x, b.y);
    r = clamp(r, 0.0, maxR);
    vec2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

float sdEllipse(vec2 p, vec2 ab) {
    vec2 pn = p / ab;
    float d = length(pn) - 1.0;
    return d * min(ab.x, ab.y);
}

void main() {
    vec2 uv = TexCoord * 2.0 - 1.0;
    
    float aspectRatio = max(u_size.x, 1.0) / max(u_size.y, 1.0);
    vec2 aspectUV = uv;
    if (aspectRatio > 1.0) {
        aspectUV.x *= aspectRatio;
    } else {
        aspectUV.y /= aspectRatio;
    }
    
    float minSize = max(min(u_size.x, u_size.y), 1.0);
    float borderThickness = u_thickness / minSize * 2.0;
    
    float dist;
    
    if (u_shape == 0) {
        vec2 boxSize = vec2(1.0, 1.0);
        if (aspectRatio > 1.0) {
            boxSize.x = aspectRatio;
        } else {
            boxSize.y = 1.0 / aspectRatio;
        }
        float cornerRadius = u_radius / minSize * 2.0;
        dist = sdRoundedBox(aspectUV, boxSize, cornerRadius);
    } else {
        vec2 ellipseSize = vec2(1.0, 1.0);
        if (aspectRatio > 1.0) {
            ellipseSize.x = aspectRatio;
        } else {
            ellipseSize.y = 1.0 / aspectRatio;
        }
        dist = sdEllipse(aspectUV, ellipseSize);
    }
    
    float innerEdge = 0.0;
    float outerEdge = borderThickness;
    
    float epsilon = 0.01;
    
    if (dist >= innerEdge - epsilon && dist <= outerEdge + epsilon) {
        FragColor = u_borderColor;
    } else {
        discard;
    }
})";

// Local shader program handles for mirror capture.
static GLuint mt_filterProgram = 0;
static GLuint mt_filterPassthroughProgram = 0;
static GLuint mt_passthroughProgram = 0;
static GLuint mt_backgroundProgram = 0;
static GLuint mt_renderProgram = 0;
static GLuint mt_renderPassthroughProgram = 0;
static GLuint mt_maskedGradientProgram = 0;
static GLuint mt_dilateHorizontalProgram = 0;
static GLuint mt_dilateVerticalProgram = 0;
static GLuint mt_dilateVerticalPassthroughProgram = 0;
static GLuint mt_staticBorderProgram = 0;

struct MT_FilterShaderLocs {
    GLint screenTexture = -1, sourceRect = -1;
    GLint gammaMode = -1;
    GLint targetColors = -1;
    GLint targetColorCount = -1;
    GLint outputColor = -1, sensitivity = -1;
};
struct MT_FilterPassthroughShaderLocs {
    GLint screenTexture = -1, sourceRect = -1;
    GLint gammaMode = -1;
    GLint targetColors = -1;
    GLint targetColorCount = -1;
    GLint sensitivity = -1;
};
struct MT_PassthroughShaderLocs {
    GLint screenTexture = -1, sourceRect = -1;
};
struct MT_BackgroundShaderLocs {
    GLint backgroundTexture = -1, opacity = -1;
};
struct MT_RenderShaderLocs {
    GLint filterTexture = -1, borderWidth = -1, outputColor = -1, borderColor = -1, screenPixel = -1;
};
struct MT_RenderPassthroughShaderLocs {
    GLint filterTexture = -1, borderWidth = -1, borderColor = -1, screenPixel = -1;
};
struct MT_MaskedGradientShaderLocs {
    GLint filterTexture = -1, borderWidth = -1, borderColor = -1, screenPixel = -1;
    GLint numStops = -1, stopColors = -1, stopPositions = -1, angle = -1, time = -1;
    GLint animationType = -1, animationSpeed = -1, colorFade = -1;
};
struct MT_DilateHorizontalShaderLocs {
    GLint sourceTexture = -1, borderWidth = -1, screenPixel = -1;
};
struct MT_DilateVerticalShaderLocs {
    GLint sourceTexture = -1, dilateTexture = -1, borderWidth = -1, screenPixel = -1, outputColor = -1, borderColor = -1;
};
struct MT_DilateVerticalPassthroughShaderLocs {
    GLint sourceTexture = -1, dilateTexture = -1, borderWidth = -1, screenPixel = -1, borderColor = -1;
};
struct MT_StaticBorderShaderLocs {
    GLint shape = -1, borderColor = -1, thickness = -1, radius = -1, size = -1;
};

static MT_FilterShaderLocs mt_filterShaderLocs;
static MT_PassthroughShaderLocs mt_passthroughShaderLocs;
static MT_BackgroundShaderLocs mt_backgroundShaderLocs;
static MT_RenderShaderLocs mt_renderShaderLocs;
static MT_RenderPassthroughShaderLocs mt_renderPassthroughShaderLocs;
static MT_MaskedGradientShaderLocs mt_maskedGradientShaderLocs;
static MT_DilateHorizontalShaderLocs mt_dilateHorizontalShaderLocs;
static MT_DilateVerticalShaderLocs mt_dilateVerticalShaderLocs;
static MT_DilateVerticalPassthroughShaderLocs mt_dilateVerticalPassthroughShaderLocs;
static MT_StaticBorderShaderLocs mt_staticBorderShaderLocs;

static MT_FilterPassthroughShaderLocs mt_filterPassthroughShaderLocs;

static GLuint MT_CompileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint ok;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, 512, NULL, log);
        Log("Mirror Thread: Shader compile error: {}", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint MT_CreateShaderProgram(const char* vertSrc, const char* fragSrc) {
    GLuint vs = MT_CompileShader(GL_VERTEX_SHADER, vertSrc);
    GLuint fs = MT_CompileShader(GL_FRAGMENT_SHADER, fragSrc);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(p, 512, NULL, log);
        Log("Mirror Thread: Shader link error: {}", log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

static bool MT_InitializeShaders() {
    LogCategory(Log_Init, "Mirror Thread: Initializing local shaders...");

    mt_filterProgram = MT_CreateShaderProgram(mt_passthrough_vert_shader, mt_filter_frag_shader);
    mt_filterPassthroughProgram = MT_CreateShaderProgram(mt_passthrough_vert_shader, mt_filter_passthrough_frag_shader);
    mt_passthroughProgram = MT_CreateShaderProgram(mt_passthrough_vert_shader, mt_passthrough_frag_shader);
    mt_backgroundProgram = MT_CreateShaderProgram(mt_passthrough_vert_shader, mt_background_frag_shader);
    mt_renderProgram = MT_CreateShaderProgram(mt_passthrough_vert_shader, mt_render_frag_shader);
    mt_renderPassthroughProgram = MT_CreateShaderProgram(mt_passthrough_vert_shader, mt_render_passthrough_frag_shader);
    mt_maskedGradientProgram = MT_CreateShaderProgram(mt_passthrough_vert_shader, mt_masked_gradient_frag_shader);
    mt_dilateHorizontalProgram = MT_CreateShaderProgram(mt_passthrough_vert_shader, mt_dilate_horizontal_frag_shader);
    mt_dilateVerticalProgram = MT_CreateShaderProgram(mt_passthrough_vert_shader, mt_dilate_vertical_frag_shader);
    mt_dilateVerticalPassthroughProgram = MT_CreateShaderProgram(mt_passthrough_vert_shader, mt_dilate_vertical_passthrough_frag_shader);
    mt_staticBorderProgram = MT_CreateShaderProgram(mt_passthrough_vert_shader, mt_static_border_frag_shader);

    if (!mt_filterProgram || !mt_filterPassthroughProgram || !mt_passthroughProgram || !mt_backgroundProgram || !mt_renderProgram ||
        !mt_renderPassthroughProgram || !mt_maskedGradientProgram || !mt_dilateHorizontalProgram || !mt_dilateVerticalProgram ||
        !mt_dilateVerticalPassthroughProgram || !mt_staticBorderProgram) {
        Log("Mirror Thread: FATAL - Failed to create basic shader programs");
        return false;
    }

    mt_filterShaderLocs.screenTexture = glGetUniformLocation(mt_filterProgram, "screenTexture");
    mt_filterShaderLocs.sourceRect = glGetUniformLocation(mt_filterProgram, "u_sourceRect");
    mt_filterShaderLocs.gammaMode = glGetUniformLocation(mt_filterProgram, "u_gammaMode");
    mt_filterShaderLocs.targetColors = glGetUniformLocation(mt_filterProgram, "u_targetColors");
    mt_filterShaderLocs.targetColorCount = glGetUniformLocation(mt_filterProgram, "u_targetColorCount");
    mt_filterShaderLocs.outputColor = glGetUniformLocation(mt_filterProgram, "outputColor");
    mt_filterShaderLocs.sensitivity = glGetUniformLocation(mt_filterProgram, "u_sensitivity");

    mt_filterPassthroughShaderLocs.screenTexture = glGetUniformLocation(mt_filterPassthroughProgram, "screenTexture");
    mt_filterPassthroughShaderLocs.sourceRect = glGetUniformLocation(mt_filterPassthroughProgram, "u_sourceRect");
    mt_filterPassthroughShaderLocs.gammaMode = glGetUniformLocation(mt_filterPassthroughProgram, "u_gammaMode");
    mt_filterPassthroughShaderLocs.targetColors = glGetUniformLocation(mt_filterPassthroughProgram, "u_targetColors");
    mt_filterPassthroughShaderLocs.targetColorCount = glGetUniformLocation(mt_filterPassthroughProgram, "u_targetColorCount");
    mt_filterPassthroughShaderLocs.sensitivity = glGetUniformLocation(mt_filterPassthroughProgram, "u_sensitivity");

    mt_passthroughShaderLocs.screenTexture = glGetUniformLocation(mt_passthroughProgram, "screenTexture");
    mt_passthroughShaderLocs.sourceRect = glGetUniformLocation(mt_passthroughProgram, "u_sourceRect");

    mt_backgroundShaderLocs.backgroundTexture = glGetUniformLocation(mt_backgroundProgram, "backgroundTexture");
    mt_backgroundShaderLocs.opacity = glGetUniformLocation(mt_backgroundProgram, "u_opacity");

    mt_renderShaderLocs.filterTexture = glGetUniformLocation(mt_renderProgram, "filterTexture");
    mt_renderShaderLocs.borderWidth = glGetUniformLocation(mt_renderProgram, "u_borderWidth");
    mt_renderShaderLocs.outputColor = glGetUniformLocation(mt_renderProgram, "u_outputColor");
    mt_renderShaderLocs.borderColor = glGetUniformLocation(mt_renderProgram, "u_borderColor");
    mt_renderShaderLocs.screenPixel = glGetUniformLocation(mt_renderProgram, "u_screenPixel");

    mt_renderPassthroughShaderLocs.filterTexture = glGetUniformLocation(mt_renderPassthroughProgram, "filterTexture");
    mt_renderPassthroughShaderLocs.borderWidth = glGetUniformLocation(mt_renderPassthroughProgram, "u_borderWidth");
    mt_renderPassthroughShaderLocs.borderColor = glGetUniformLocation(mt_renderPassthroughProgram, "u_borderColor");
    mt_renderPassthroughShaderLocs.screenPixel = glGetUniformLocation(mt_renderPassthroughProgram, "u_screenPixel");

    mt_maskedGradientShaderLocs.filterTexture = glGetUniformLocation(mt_maskedGradientProgram, "filterTexture");
    mt_maskedGradientShaderLocs.borderWidth = glGetUniformLocation(mt_maskedGradientProgram, "u_borderWidth");
    mt_maskedGradientShaderLocs.borderColor = glGetUniformLocation(mt_maskedGradientProgram, "u_borderColor");
    mt_maskedGradientShaderLocs.screenPixel = glGetUniformLocation(mt_maskedGradientProgram, "u_screenPixel");
    mt_maskedGradientShaderLocs.numStops = glGetUniformLocation(mt_maskedGradientProgram, "u_numStops");
    mt_maskedGradientShaderLocs.stopColors = glGetUniformLocation(mt_maskedGradientProgram, "u_stopColors");
    mt_maskedGradientShaderLocs.stopPositions = glGetUniformLocation(mt_maskedGradientProgram, "u_stopPositions");
    mt_maskedGradientShaderLocs.angle = glGetUniformLocation(mt_maskedGradientProgram, "u_angle");
    mt_maskedGradientShaderLocs.time = glGetUniformLocation(mt_maskedGradientProgram, "u_time");
    mt_maskedGradientShaderLocs.animationType = glGetUniformLocation(mt_maskedGradientProgram, "u_animationType");
    mt_maskedGradientShaderLocs.animationSpeed = glGetUniformLocation(mt_maskedGradientProgram, "u_animationSpeed");
    mt_maskedGradientShaderLocs.colorFade = glGetUniformLocation(mt_maskedGradientProgram, "u_colorFade");

    mt_dilateHorizontalShaderLocs.sourceTexture = glGetUniformLocation(mt_dilateHorizontalProgram, "sourceTexture");
    mt_dilateHorizontalShaderLocs.borderWidth = glGetUniformLocation(mt_dilateHorizontalProgram, "u_borderWidth");
    mt_dilateHorizontalShaderLocs.screenPixel = glGetUniformLocation(mt_dilateHorizontalProgram, "u_screenPixel");

    mt_dilateVerticalShaderLocs.sourceTexture = glGetUniformLocation(mt_dilateVerticalProgram, "sourceTexture");
    mt_dilateVerticalShaderLocs.dilateTexture = glGetUniformLocation(mt_dilateVerticalProgram, "dilateTexture");
    mt_dilateVerticalShaderLocs.borderWidth = glGetUniformLocation(mt_dilateVerticalProgram, "u_borderWidth");
    mt_dilateVerticalShaderLocs.screenPixel = glGetUniformLocation(mt_dilateVerticalProgram, "u_screenPixel");
    mt_dilateVerticalShaderLocs.outputColor = glGetUniformLocation(mt_dilateVerticalProgram, "u_outputColor");
    mt_dilateVerticalShaderLocs.borderColor = glGetUniformLocation(mt_dilateVerticalProgram, "u_borderColor");

    mt_dilateVerticalPassthroughShaderLocs.sourceTexture = glGetUniformLocation(mt_dilateVerticalPassthroughProgram, "sourceTexture");
    mt_dilateVerticalPassthroughShaderLocs.dilateTexture = glGetUniformLocation(mt_dilateVerticalPassthroughProgram, "dilateTexture");
    mt_dilateVerticalPassthroughShaderLocs.borderWidth = glGetUniformLocation(mt_dilateVerticalPassthroughProgram, "u_borderWidth");
    mt_dilateVerticalPassthroughShaderLocs.screenPixel = glGetUniformLocation(mt_dilateVerticalPassthroughProgram, "u_screenPixel");
    mt_dilateVerticalPassthroughShaderLocs.borderColor = glGetUniformLocation(mt_dilateVerticalPassthroughProgram, "u_borderColor");

    mt_staticBorderShaderLocs.shape = glGetUniformLocation(mt_staticBorderProgram, "u_shape");
    mt_staticBorderShaderLocs.borderColor = glGetUniformLocation(mt_staticBorderProgram, "u_borderColor");
    mt_staticBorderShaderLocs.thickness = glGetUniformLocation(mt_staticBorderProgram, "u_thickness");
    mt_staticBorderShaderLocs.radius = glGetUniformLocation(mt_staticBorderProgram, "u_radius");
    mt_staticBorderShaderLocs.size = glGetUniformLocation(mt_staticBorderProgram, "u_size");

    glUseProgram(mt_filterProgram);
    glUniform1i(mt_filterShaderLocs.screenTexture, 0);
    if (mt_filterShaderLocs.gammaMode >= 0) { glUniform1i(mt_filterShaderLocs.gammaMode, 0); }

    glUseProgram(mt_filterPassthroughProgram);
    glUniform1i(mt_filterPassthroughShaderLocs.screenTexture, 0);
    if (mt_filterPassthroughShaderLocs.gammaMode >= 0) { glUniform1i(mt_filterPassthroughShaderLocs.gammaMode, 0); }

    glUseProgram(mt_passthroughProgram);
    glUniform1i(mt_passthroughShaderLocs.screenTexture, 0);

    glUseProgram(mt_backgroundProgram);
    glUniform1i(mt_backgroundShaderLocs.backgroundTexture, 0);

    glUseProgram(mt_renderProgram);
    glUniform1i(mt_renderShaderLocs.filterTexture, 0);

    glUseProgram(mt_renderPassthroughProgram);
    glUniform1i(mt_renderPassthroughShaderLocs.filterTexture, 0);

    glUseProgram(mt_maskedGradientProgram);
    glUniform1i(mt_maskedGradientShaderLocs.filterTexture, 0);

    glUseProgram(mt_dilateHorizontalProgram);
    glUniform1i(mt_dilateHorizontalShaderLocs.sourceTexture, 0);

    glUseProgram(mt_dilateVerticalProgram);
    glUniform1i(mt_dilateVerticalShaderLocs.sourceTexture, 0);
    glUniform1i(mt_dilateVerticalShaderLocs.dilateTexture, 1);

    glUseProgram(mt_dilateVerticalPassthroughProgram);
    glUniform1i(mt_dilateVerticalPassthroughShaderLocs.sourceTexture, 0);
    glUniform1i(mt_dilateVerticalPassthroughShaderLocs.dilateTexture, 1);

    glUseProgram(0);

    LogCategory(Log_Init, "Mirror Thread: Local shaders initialized successfully");
    return true;
}

static void MT_CleanupShaders() {
    if (mt_filterProgram) {
        glDeleteProgram(mt_filterProgram);
        mt_filterProgram = 0;
    }
    if (mt_filterPassthroughProgram) {
        glDeleteProgram(mt_filterPassthroughProgram);
        mt_filterPassthroughProgram = 0;
    }
    if (mt_passthroughProgram) {
        glDeleteProgram(mt_passthroughProgram);
        mt_passthroughProgram = 0;
    }
    if (mt_backgroundProgram) {
        glDeleteProgram(mt_backgroundProgram);
        mt_backgroundProgram = 0;
    }
    if (mt_renderProgram) {
        glDeleteProgram(mt_renderProgram);
        mt_renderProgram = 0;
    }
    if (mt_renderPassthroughProgram) {
        glDeleteProgram(mt_renderPassthroughProgram);
        mt_renderPassthroughProgram = 0;
    }
    if (mt_maskedGradientProgram) {
        glDeleteProgram(mt_maskedGradientProgram);
        mt_maskedGradientProgram = 0;
    }
    if (mt_dilateHorizontalProgram) {
        glDeleteProgram(mt_dilateHorizontalProgram);
        mt_dilateHorizontalProgram = 0;
    }
    if (mt_dilateVerticalProgram) {
        glDeleteProgram(mt_dilateVerticalProgram);
        mt_dilateVerticalProgram = 0;
    }
    if (mt_dilateVerticalPassthroughProgram) {
        glDeleteProgram(mt_dilateVerticalPassthroughProgram);
        mt_dilateVerticalPassthroughProgram = 0;
    }
    if (mt_staticBorderProgram) {
        glDeleteProgram(mt_staticBorderProgram);
        mt_staticBorderProgram = 0;
    }
}

// Get the most recent copy texture (for OBS/render_thread to use)
GLuint GetGameCopyTexture() {
    int readIndex = g_lastCopyReadIndex.load(std::memory_order_acquire);
    if (readIndex < 0 || readIndex >= 2) return 0;
    return g_copyTextures[readIndex];
}

// These return GUARANTEED COMPLETE frames - no fence wait needed
// Updated after the copy fence signals, so OBS can read without waiting.

GLuint GetReadyGameTexture() {
    int idx = g_readyFrameIndex.load(std::memory_order_acquire);
    if (idx < 0 || idx >= 2) return 0;
    return g_copyTextures[idx];
}

int GetReadyGameWidth() { return g_readyFrameWidth.load(std::memory_order_acquire); }

int GetReadyGameHeight() { return g_readyFrameHeight.load(std::memory_order_acquire); }

// Fallback accessors - return last copy texture info (requires fence wait before use)
GLuint GetFallbackGameTexture() {
    int idx = g_lastCopyReadIndex.load(std::memory_order_acquire);
    if (idx < 0 || idx >= 2) return 0;
    return g_copyTextures[idx];
}

int GetFallbackGameWidth() { return g_lastCopyWidth.load(std::memory_order_acquire); }

int GetFallbackGameHeight() { return g_lastCopyHeight.load(std::memory_order_acquire); }

// This is a guaranteed valid texture (may be 1 frame behind) - no fence wait needed
GLuint GetSafeReadTexture() {
    if (!g_safeReadTextureValid.load(std::memory_order_acquire)) return 0;
    int writeIndex = g_copyTextureWriteIndex.load(std::memory_order_acquire);
    int readIndex = 1 - writeIndex;
    if (g_copyTextures[readIndex] == 0) return 0;
    return g_copyTextures[readIndex];
}

void InitCaptureTexture(int width, int height) {
    // This must be called on the active GL render path with a current context.

    g_copyTextureW = width;
    g_copyTextureH = height;

    glGenFramebuffers(1, &g_copyFBO);

    glGenTextures(2, g_copyTextures);
    for (int i = 0; i < 2; i++) {
        BindTextureDirect(GL_TEXTURE_2D, g_copyTextures[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    BindTextureDirect(GL_TEXTURE_2D, 0);

    g_copyTextureWriteIndex.store(0);
    g_safeReadTextureValid.store(false, std::memory_order_release);

    LogCategory(Log_Init, "InitCaptureTexture: Created FBO and {} textures of {}x{}", 2, width, height);
}

void EnsureCaptureTextureInitialized(int width, int height) {
    if (width <= 0 || height <= 0) { return; }
    if (g_copyFBO != 0 && g_copyTextures[0] != 0 && g_copyTextures[1] != 0) { return; }
    InitCaptureTexture(width, height);
}

void CleanupCaptureTexture() {
    // Cleanup capture resources on the current GL thread.
    {
        GLsync old = g_lastCopyFence.exchange(nullptr, std::memory_order_acq_rel);
        if (old && glIsSync(old)) { glDeleteSync(old); }
        g_lastCopyReadIndex.store(-1, std::memory_order_release);
        g_lastCopyWidth.store(0, std::memory_order_release);
        g_lastCopyHeight.store(0, std::memory_order_release);
        g_readyFrameIndex.store(-1, std::memory_order_release);
        g_readyFrameWidth.store(0, std::memory_order_release);
        g_readyFrameHeight.store(0, std::memory_order_release);
        g_safeReadTextureValid.store(false, std::memory_order_release);
    }

    if (g_copyTextures[0] != 0 || g_copyTextures[1] != 0) {
        glDeleteTextures(2, g_copyTextures);
        g_copyTextures[0] = 0;
        g_copyTextures[1] = 0;
    }

    if (g_copyFBO != 0) {
        glDeleteFramebuffers(1, &g_copyFBO);
        g_copyFBO = 0;
    }

    Log("CleanupCaptureTexture: Cleaned up FBO and textures");
}

void SubmitFrameCapture(GLuint gameTexture, int width, int height) {
    // Called from SwapBuffers to refresh the shared copy textures for same-thread consumers.

    if (g_copyFBO == 0) {
        return;
    }

    GLint prevReadFBO = 0;
    GLint prevDrawFBO = 0;
    GLint prevActiveTexture = 0;
    GLint prevTexture2D = 0;
    GLboolean prevDither = GL_FALSE;
    GLboolean prevFramebufferSRGB = GL_FALSE;
    bool hasFramebufferSRGB = false;

    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFBO);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDrawFBO);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActiveTexture);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTexture2D);

    // Some drivers apply dithering when converting to RGBA8 during blits/writes.
    prevDither = glIsEnabled(GL_DITHER);

    hasFramebufferSRGB = (GLEW_VERSION_3_0 || GLEW_EXT_framebuffer_sRGB || GLEW_ARB_framebuffer_sRGB);
    if (hasFramebufferSRGB) {
        prevFramebufferSRGB = glIsEnabled(GL_FRAMEBUFFER_SRGB);
    }

    auto restoreState = [&]() {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, prevReadFBO);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prevDrawFBO);
        glActiveTexture(prevActiveTexture);
        BindTextureDirect(GL_TEXTURE_2D, prevTexture2D);

        if (prevDither)
            glEnable(GL_DITHER);
        else
            glDisable(GL_DITHER);

        if (hasFramebufferSRGB) {
            if (prevFramebufferSRGB)
                glEnable(GL_FRAMEBUFFER_SRGB);
            else
                glDisable(GL_FRAMEBUFFER_SRGB);
        }
    };

    glDisable(GL_DITHER);
    if (hasFramebufferSRGB) { glDisable(GL_FRAMEBUFFER_SRGB); }

    // Only resize the WRITE texture, not the read texture that other threads may be using
    int writeIndex = g_copyTextureWriteIndex.load(std::memory_order_acquire);
    bool dimensionsChanged = (width != g_copyTextureW || height != g_copyTextureH);

    if (dimensionsChanged) {
        // Note: We do NOT invalidate g_lastCopyReadIndex here - it continues pointing to
        // in an invalid state if blits fail due to race conditions.

        // glTexImage2D replaces the backing storage with undefined content, so any
        // thread reading the "ready" texture would get garbage/black data. This was
        // causing visual freezes on some devices: the render path would keep blitting
        // the stale ready frame (now undefined) instead of showing new content.
        g_readyFrameIndex.store(-1, std::memory_order_release);
        g_readyFrameWidth.store(0, std::memory_order_release);
        g_readyFrameHeight.store(0, std::memory_order_release);
        g_safeReadTextureValid.store(false, std::memory_order_release);

        for (int i = 0; i < 2; i++) {
            BindTextureDirect(GL_TEXTURE_2D, g_copyTextures[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        }
        BindTextureDirect(GL_TEXTURE_2D, 0);

        // Use fence + flush instead of glFinish() to avoid blocking the game thread.
        // cause visible hitches on some GPU/driver combinations (especially iGPUs).
        // A fence only waits for the texture reallocation commands specifically.
        GLsync resizeFence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glFlush(); // Ensure fence and resize commands are submitted to GPU
        if (resizeFence) {
            glClientWaitSync(resizeFence, GL_SYNC_FLUSH_COMMANDS_BIT, 500000000ULL);
            if (glIsSync(resizeFence)) { glDeleteSync(resizeFence); }
        }

        g_copyTextureW = width;
        g_copyTextureH = height;
        LogCategory(Log_TextureOps, "SubmitFrameCapture: Resized copy textures to {}x{}", width, height);
    }

    static GLuint srcFBO = 0;
    if (srcFBO == 0) { glGenFramebuffers(1, &srcFBO); }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFBO);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gameTexture, 0);

    GLenum srcStatus = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
    if (srcStatus != GL_FRAMEBUFFER_COMPLETE) {
        static int s_srcIncompleteLog = 0;
        if ((++s_srcIncompleteLog % 240) == 1) {
            LogCategory(Log_TextureOps,
                        "SubmitFrameCapture: Source FBO incomplete (status {}) gameTex={} size={}x{}",
                        srcStatus, gameTexture, width, height);
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        restoreState();
        return;
    }


    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_copyFBO);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_copyTextures[writeIndex], 0);

    GLenum dstStatus = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    if (dstStatus != GL_FRAMEBUFFER_COMPLETE) {
        static int s_dstIncompleteLog = 0;
        if ((++s_dstIncompleteLog % 240) == 1) {
            LogCategory(Log_TextureOps,
                        "SubmitFrameCapture: Destination FBO incomplete (status {}) writeIdx={} dstTex={} size={}x{}",
                        dstStatus, writeIndex, g_copyTextures[writeIndex], width, height);
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        restoreState();
        return;
    }

    BlitFramebufferDirect(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

    GLsync copyFence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (!copyFence) {
        restoreState();
        return;
    }

    // Flush to ensure commands are submitted and the fence is visible before any sampling.
    glFlush();

    int nextWriteIndex = 1 - writeIndex;
    g_copyTextureWriteIndex.store(nextWriteIndex, std::memory_order_release);

    GLsync oldFence = g_lastCopyFence.exchange(copyFence, std::memory_order_acq_rel);
    if (oldFence && glIsSync(oldFence)) { glDeleteSync(oldFence); }
    g_lastCopyReadIndex.store(writeIndex, std::memory_order_release);
    g_lastCopyWidth.store(width, std::memory_order_release);
    g_lastCopyHeight.store(height, std::memory_order_release);
    g_safeReadTextureValid.store(true, std::memory_order_release);
    g_readyFrameIndex.store(writeIndex, std::memory_order_release);
    g_readyFrameWidth.store(width, std::memory_order_release);
    g_readyFrameHeight.store(height, std::memory_order_release);

    restoreState();
}

static void ComputeMirrorRenderCache(MirrorInstance* inst, const ThreadedMirrorConfig& conf, int gameW, int gameH, int screenW, int screenH,
                                     int finalX, int finalY, int finalW, int finalH, bool writeToBack) {
    auto& cache = writeToBack ? inst->cachedRenderStateBack : inst->cachedRenderState;

    float scaleX = conf.outputSeparateScale ? conf.outputScaleX : conf.outputScale;
    float scaleY = conf.outputSeparateScale ? conf.outputScaleY : conf.outputScale;
    if (cache.isValid && cache.outputScale == conf.outputScale && cache.outputSeparateScale == conf.outputSeparateScale &&
        cache.outputScaleX == conf.outputScaleX && cache.outputScaleY == conf.outputScaleY && cache.outputX == conf.outputX &&
        cache.outputY == conf.outputY && cache.outputRelativeTo == conf.outputRelativeTo && cache.gameW == gameW && cache.gameH == gameH &&
        cache.screenW == screenW && cache.screenH == screenH && cache.finalX == finalX && cache.finalY == finalY &&
        cache.finalW == finalW && cache.finalH == finalH && cache.fbo_w == inst->fbo_w && cache.fbo_h == inst->fbo_h) {
        return;
    }

    int outW = static_cast<int>(inst->fbo_w * scaleX);
    int outH = static_cast<int>(inst->fbo_h * scaleY);

    MirrorConfig tempConf;
    tempConf.output.scale = conf.outputScale;
    tempConf.output.separateScale = conf.outputSeparateScale;
    tempConf.output.scaleX = conf.outputScaleX;
    tempConf.output.scaleY = conf.outputScaleY;
    tempConf.output.x = conf.outputX;
    tempConf.output.y = conf.outputY;
    tempConf.output.relativeTo = conf.outputRelativeTo;

    int screenX, screenY;
    CalculateFinalScreenPos(&tempConf, *inst, gameW, gameH, finalX, finalY, finalW, finalH, screenW, screenH, screenX, screenY);

    float nx1 = (static_cast<float>(screenX) / screenW) * 2.0f - 1.0f;
    float ny2 = 1.0f - (static_cast<float>(screenY) / screenH) * 2.0f;
    float nx2 = (static_cast<float>(screenX + outW) / screenW) * 2.0f - 1.0f;
    float ny1 = 1.0f - (static_cast<float>(screenY + outH) / screenH) * 2.0f;

    float vertices[] = { nx1, ny1, 0, 0, nx2, ny1, 1, 0, nx2, ny2, 1, 1, nx1, ny1, 0, 0, nx2, ny2, 1, 1, nx1, ny2, 0, 1 };
    memcpy(cache.vertices, vertices, sizeof(vertices));

    cache.outputScale = conf.outputScale;
    cache.outputSeparateScale = conf.outputSeparateScale;
    cache.outputScaleX = conf.outputScaleX;
    cache.outputScaleY = conf.outputScaleY;
    cache.outputX = conf.outputX;
    cache.outputY = conf.outputY;
    cache.outputRelativeTo = conf.outputRelativeTo;
    cache.gameW = gameW;
    cache.gameH = gameH;
    cache.screenW = screenW;
    cache.screenH = screenH;
    cache.finalX = finalX;
    cache.finalY = finalY;
    cache.finalW = finalW;
    cache.finalH = finalH;
    cache.fbo_w = inst->fbo_w;
    cache.fbo_h = inst->fbo_h;
    cache.outW = outW;
    cache.outH = outH;
    cache.mirrorScreenX = screenX;
    cache.mirrorScreenY = screenY;
    cache.mirrorScreenW = outW;
    cache.mirrorScreenH = outH;
    cache.isValid = true;
}

struct MT_RenderToBufferStateCache {
    GLuint framebuffer = 0;
    bool framebufferValid = false;
    GLuint program = 0;
    bool programValid = false;
    GLuint texture = 0;
    bool textureValid = false;
    bool blendEnabled = false;
    bool blendValid = false;
    int viewportX = 0;
    int viewportY = 0;
    int viewportW = 0;
    int viewportH = 0;
    bool viewportValid = false;
    float clearColorR = 0.0f;
    float clearColorG = 0.0f;
    float clearColorB = 0.0f;
    float clearColorA = 0.0f;
    bool clearColorValid = false;

    int filterGammaMode = 0;
    bool filterGammaModeValid = false;
    int filterTargetColorCount = 0;
    bool filterTargetColorStateValid = false;
    std::array<float, 24> filterTargetColors{};
    float filterSensitivity = 0.0f;
    bool filterSensitivityValid = false;
    Color filterOutputColor{};
    bool filterOutputColorValid = false;

    int filterPassthroughGammaMode = 0;
    bool filterPassthroughGammaModeValid = false;
    int filterPassthroughTargetColorCount = 0;
    bool filterPassthroughTargetColorStateValid = false;
    std::array<float, 24> filterPassthroughTargetColors{};
    float filterPassthroughSensitivity = 0.0f;
    bool filterPassthroughSensitivityValid = false;

    float backgroundOpacity = 0.0f;
    bool backgroundOpacityValid = false;

    int renderBorderWidth = 0;
    bool renderBorderWidthValid = false;
    Color renderOutputColor{};
    bool renderOutputColorValid = false;
    Color renderBorderColor{};
    bool renderBorderColorValid = false;
    float renderScreenPixelX = 0.0f;
    float renderScreenPixelY = 0.0f;
    bool renderScreenPixelValid = false;

    int renderPassthroughBorderWidth = 0;
    bool renderPassthroughBorderWidthValid = false;
    Color renderPassthroughBorderColor{};
    bool renderPassthroughBorderColorValid = false;
    float renderPassthroughScreenPixelX = 0.0f;
    float renderPassthroughScreenPixelY = 0.0f;
    bool renderPassthroughScreenPixelValid = false;

    int dilateHorizontalBorderWidth = 0;
    bool dilateHorizontalBorderWidthValid = false;
    float dilateHorizontalScreenPixelX = 0.0f;
    float dilateHorizontalScreenPixelY = 0.0f;
    bool dilateHorizontalScreenPixelValid = false;

    int dilateVerticalBorderWidth = 0;
    bool dilateVerticalBorderWidthValid = false;
    float dilateVerticalScreenPixelX = 0.0f;
    float dilateVerticalScreenPixelY = 0.0f;
    bool dilateVerticalScreenPixelValid = false;
    Color dilateVerticalOutputColor{};
    bool dilateVerticalOutputColorValid = false;
    Color dilateVerticalBorderColor{};
    bool dilateVerticalBorderColorValid = false;

    int dilateVerticalPassthroughBorderWidth = 0;
    bool dilateVerticalPassthroughBorderWidthValid = false;
    float dilateVerticalPassthroughScreenPixelX = 0.0f;
    float dilateVerticalPassthroughScreenPixelY = 0.0f;
    bool dilateVerticalPassthroughScreenPixelValid = false;
    Color dilateVerticalPassthroughBorderColor{};
    bool dilateVerticalPassthroughBorderColorValid = false;

    GLuint sourceRectAttribBuffer = 0;
    bool sourceRectAttribBufferValid = false;
};

static void MT_BindFramebufferCached(GLuint framebuffer, MT_RenderToBufferStateCache* stateCache) {
    if (!stateCache || !stateCache->framebufferValid || stateCache->framebuffer != framebuffer) {
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        if (stateCache) {
            stateCache->framebuffer = framebuffer;
            stateCache->framebufferValid = true;
        }
    }
}

static void MT_SetViewportCached(int x, int y, int width, int height, MT_RenderToBufferStateCache* stateCache) {
    if (!stateCache || !stateCache->viewportValid || stateCache->viewportX != x || stateCache->viewportY != y ||
        stateCache->viewportW != width || stateCache->viewportH != height) {
        if (oglViewport) {
            oglViewport(x, y, width, height);
        } else {
            glViewport(x, y, width, height);
        }

        if (stateCache) {
            stateCache->viewportX = x;
            stateCache->viewportY = y;
            stateCache->viewportW = width;
            stateCache->viewportH = height;
            stateCache->viewportValid = true;
        }
    }
}

static void MT_UseProgramCached(GLuint program, MT_RenderToBufferStateCache* stateCache) {
    if (!stateCache || !stateCache->programValid || stateCache->program != program) {
        glUseProgram(program);
        if (stateCache) {
            stateCache->program = program;
            stateCache->programValid = true;
        }
    }
}

static void MT_BindTextureCached(GLuint texture, MT_RenderToBufferStateCache* stateCache) {
    if (!stateCache || !stateCache->textureValid || stateCache->texture != texture) {
        BindTextureDirect(GL_TEXTURE_2D, texture);
        if (stateCache) {
            stateCache->texture = texture;
            stateCache->textureValid = true;
        }
    }
}

static void MT_SetBlendEnabledCached(bool enabled, MT_RenderToBufferStateCache* stateCache) {
    if (!stateCache || !stateCache->blendValid || stateCache->blendEnabled != enabled) {
        if (enabled) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE);
        } else {
            glDisable(GL_BLEND);
        }

        if (stateCache) {
            stateCache->blendEnabled = enabled;
            stateCache->blendValid = true;
        }
    }
}

static void MT_SetClearColorCached(float r, float g, float b, float a, MT_RenderToBufferStateCache* stateCache) {
    if (!stateCache || !stateCache->clearColorValid || stateCache->clearColorR != r || stateCache->clearColorG != g ||
        stateCache->clearColorB != b || stateCache->clearColorA != a) {
        glClearColor(r, g, b, a);
        if (stateCache) {
            stateCache->clearColorR = r;
            stateCache->clearColorG = g;
            stateCache->clearColorB = b;
            stateCache->clearColorA = a;
            stateCache->clearColorValid = true;
        }
    }
}

static bool MT_ColorEquals(const Color& left, const Color& right) {
    return left.r == right.r && left.g == right.g && left.b == right.b && left.a == right.a;
}

static uint64_t MT_HashBytes(uint64_t seed, const void* data, size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) {
        seed ^= static_cast<uint64_t>(bytes[i]);
        seed *= 1099511628211ull;
    }
    return seed;
}

static uint64_t MT_ComputeSourceRectLayoutHash(const ThreadedMirrorConfig& conf) {
    uint64_t hash = 1469598103934665603ull;
    hash = MT_HashBytes(hash, &conf.captureWidth, sizeof(conf.captureWidth));
    hash = MT_HashBytes(hash, &conf.captureHeight, sizeof(conf.captureHeight));

    const size_t inputCount = conf.input.size();
    hash = MT_HashBytes(hash, &inputCount, sizeof(inputCount));
    for (const auto& input : conf.input) {
        hash = MT_HashBytes(hash, &input.x, sizeof(input.x));
        hash = MT_HashBytes(hash, &input.y, sizeof(input.y));
        hash = MT_HashBytes(hash, input.relativeTo.data(), input.relativeTo.size());
    }

    return hash;
}

struct MT_SourceRectCacheEntry {
    uint64_t layoutHash = 0;
    int gameW = 0;
    int gameH = 0;
    std::vector<float> sourceRects;
};

struct MT_SourceRectGpuCacheEntry {
    GLuint instanceVbo = 0;
    size_t capacityBytes = 0;
    uint64_t layoutHash = 0;
    int gameW = 0;
    int gameH = 0;
};

static void MT_DeleteSourceRectGpuCacheEntry(MT_SourceRectGpuCacheEntry& cacheEntry) {
    if (cacheEntry.instanceVbo != 0) {
        glDeleteBuffers(1, &cacheEntry.instanceVbo);
        cacheEntry.instanceVbo = 0;
    }
    cacheEntry.capacityBytes = 0;
    cacheEntry.layoutHash = 0;
    cacheEntry.gameW = 0;
    cacheEntry.gameH = 0;
}

static void MT_BindSourceRectInstanceBufferCached(GLuint instanceVbo, MT_RenderToBufferStateCache* stateCache) {
    if (!stateCache || !stateCache->sourceRectAttribBufferValid || stateCache->sourceRectAttribBuffer != instanceVbo) {
        glBindBuffer(GL_ARRAY_BUFFER, instanceVbo);
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        if (stateCache) {
            stateCache->sourceRectAttribBuffer = instanceVbo;
            stateCache->sourceRectAttribBufferValid = true;
        }
    }
}

// Forward declaration needed for use in MT_GetCachedSourceRects before the full definition below
static void MT_GetRelativeCoordsNormalized(const std::string& anchor, int relX, int relY, int w, int h, int containerW,
                                           int containerH, int& outX, int& outY);

static const std::vector<float>& MT_GetCachedSourceRects(const ThreadedMirrorConfig& conf, int gameW, int gameH) {
    thread_local std::unordered_map<std::string, MT_SourceRectCacheEntry> s_sourceRectCache;

    MT_SourceRectCacheEntry& cacheEntry = s_sourceRectCache[conf.name];
    const size_t requiredFloatCount = conf.input.size() * 4;
    if (cacheEntry.layoutHash == conf.sourceRectLayoutHash && cacheEntry.gameW == gameW && cacheEntry.gameH == gameH &&
        cacheEntry.sourceRects.size() == requiredFloatCount) {
        return cacheEntry.sourceRects;
    }

    cacheEntry.layoutHash = conf.sourceRectLayoutHash;
    cacheEntry.gameW = gameW;
    cacheEntry.gameH = gameH;
    cacheEntry.sourceRects.resize(requiredFloatCount);

    const float sw = static_cast<float>(conf.captureWidth) / gameW;
    const float sh = static_cast<float>(conf.captureHeight) / gameH;
    for (size_t i = 0; i < conf.input.size(); ++i) {
        const auto& r = conf.input[i];
        int capX = 0;
        int capY = 0;
        MT_GetRelativeCoordsNormalized(r.relativeTo, r.x, r.y, conf.captureWidth, conf.captureHeight, gameW, gameH, capX, capY);
        const int capYGl = gameH - capY - conf.captureHeight;

        cacheEntry.sourceRects[i * 4 + 0] = static_cast<float>(capX) / gameW;
        cacheEntry.sourceRects[i * 4 + 1] = static_cast<float>(capYGl) / gameH;
        cacheEntry.sourceRects[i * 4 + 2] = sw;
        cacheEntry.sourceRects[i * 4 + 3] = sh;
    }

    return cacheEntry.sourceRects;
}

static int MT_UploadSourceRectInstances(MT_SourceRectGpuCacheEntry& gpuCache, const ThreadedMirrorConfig& conf, int gameW, int gameH,
                                        MT_RenderToBufferStateCache* stateCache) {
    const size_t instanceCount = conf.input.size();
    if (instanceCount == 0) { return 0; }

    if (gpuCache.instanceVbo == 0) {
        glGenBuffers(1, &gpuCache.instanceVbo);
        gpuCache.capacityBytes = 0;
        gpuCache.layoutHash = 0;
        gpuCache.gameW = 0;
        gpuCache.gameH = 0;
    }

    if (gpuCache.layoutHash != conf.sourceRectLayoutHash || gpuCache.gameW != gameW || gpuCache.gameH != gameH) {
        const std::vector<float>& sourceRects = MT_GetCachedSourceRects(conf, gameW, gameH);
        const size_t requiredBytes = sourceRects.size() * sizeof(float);
        glBindBuffer(GL_ARRAY_BUFFER, gpuCache.instanceVbo);
        if (gpuCache.capacityBytes < requiredBytes) {
            glBufferData(GL_ARRAY_BUFFER, requiredBytes, sourceRects.data(), GL_DYNAMIC_DRAW);
            gpuCache.capacityBytes = requiredBytes;
        } else {
            glBufferSubData(GL_ARRAY_BUFFER, 0, requiredBytes, sourceRects.data());
        }

        gpuCache.layoutHash = conf.sourceRectLayoutHash;
        gpuCache.gameW = gameW;
        gpuCache.gameH = gameH;
    }

    MT_BindSourceRectInstanceBufferCached(gpuCache.instanceVbo, stateCache);

    return static_cast<int>(instanceCount);
}

static bool MT_CanRenderMirrorDirectToFinal(const ThreadedMirrorConfig& conf, bool useRawOutput, bool writeToBack) {
    if (writeToBack) { return false; }
    if (useRawOutput) { return true; }
    if (conf.gradientOutput) { return false; }
    if (conf.borderType == MirrorBorderType::Static) { return true; }
    return conf.borderType == MirrorBorderType::Dynamic && conf.dynamicBorderThickness <= 0;
}

static bool MT_CanCompositeDynamicBorderOnScreen(const ThreadedMirrorConfig& conf, bool useRawOutput, bool writeToBack) {
    return !writeToBack && !useRawOutput && !conf.gradientOutput && conf.borderType == MirrorBorderType::Dynamic &&
           conf.dynamicBorderThickness == 1;
}

static bool MT_ShouldUseSeparableDynamicBorder(const ThreadedMirrorConfig& conf, bool useRawOutput) {
    return !useRawOutput && !conf.gradientOutput && conf.borderType == MirrorBorderType::Dynamic &&
           conf.dynamicBorderThickness > 1;
}

static bool MT_SameThreadMirrorNeedsFinalTarget(const ThreadedMirrorConfig& conf, bool useRawOutput) {
    return !MT_CanCompositeDynamicBorderOnScreen(conf, useRawOutput, false);
}

static bool MT_EnsureTempCaptureTexture(MirrorInstance* inst, int width, int height) {
    if (!inst || width <= 0 || height <= 0) { return false; }

    if (inst->tempCaptureTexture == 0) {
        glGenTextures(1, &inst->tempCaptureTexture);
        inst->tempCaptureTextureW = 0;
        inst->tempCaptureTextureH = 0;
    }

    if (inst->tempCaptureTextureW == width && inst->tempCaptureTextureH == height) { return true; }

    BindTextureDirect(GL_TEXTURE_2D, inst->tempCaptureTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    BindTextureDirect(GL_TEXTURE_2D, 0);

    inst->tempCaptureTextureW = width;
    inst->tempCaptureTextureH = height;
    return true;
}

static bool MT_RenderSeparableDynamicBorder(MirrorInstance* inst, const ThreadedMirrorConfig& conf, GLuint captureTexture,
                                            GLuint captureTempFbo, GLuint captureFinalFbo, GLuint* lastTempTextureId,
                                            GLuint finalTexture, int finalW, int finalH,
                                            MT_RenderToBufferStateCache* stateCache) {
    if (!inst || captureTempFbo == 0 || captureFinalFbo == 0 || finalTexture == 0 || !MT_EnsureTempCaptureTexture(inst, finalW, finalH)) {
        return false;
    }

    const float screenPixelX = 1.0f / static_cast<float>((std::max)(1, finalW));
    const float screenPixelY = 1.0f / static_cast<float>((std::max)(1, finalH));

    if (lastTempTextureId == nullptr || *lastTempTextureId != inst->tempCaptureTexture) {
        glBindFramebuffer(GL_FRAMEBUFFER, captureTempFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, inst->tempCaptureTexture, 0);
        if (lastTempTextureId) { *lastTempTextureId = inst->tempCaptureTexture; }
        if (stateCache) {
            stateCache->framebufferValid = false;
            stateCache->textureValid = false;
        }
    }

    MT_BindFramebufferCached(captureTempFbo, stateCache);
    MT_SetViewportCached(0, 0, finalW, finalH, stateCache);
    MT_SetBlendEnabledCached(false, stateCache);

    glActiveTexture(GL_TEXTURE0);
    BindTextureDirect(GL_TEXTURE_2D, captureTexture);
    MT_UseProgramCached(mt_dilateHorizontalProgram, stateCache);
    if (!stateCache || !stateCache->dilateHorizontalBorderWidthValid ||
        stateCache->dilateHorizontalBorderWidth != conf.dynamicBorderThickness) {
        glUniform1i(mt_dilateHorizontalShaderLocs.borderWidth, conf.dynamicBorderThickness);
        if (stateCache) {
            stateCache->dilateHorizontalBorderWidth = conf.dynamicBorderThickness;
            stateCache->dilateHorizontalBorderWidthValid = true;
        }
    }
    if (!stateCache || !stateCache->dilateHorizontalScreenPixelValid || stateCache->dilateHorizontalScreenPixelX != screenPixelX ||
        stateCache->dilateHorizontalScreenPixelY != screenPixelY) {
        glUniform2f(mt_dilateHorizontalShaderLocs.screenPixel, screenPixelX, screenPixelY);
        if (stateCache) {
            stateCache->dilateHorizontalScreenPixelX = screenPixelX;
            stateCache->dilateHorizontalScreenPixelY = screenPixelY;
            stateCache->dilateHorizontalScreenPixelValid = true;
        }
    }
    glDrawArrays(GL_TRIANGLES, 0, 6);

    MT_BindFramebufferCached(captureFinalFbo, stateCache);

    glActiveTexture(GL_TEXTURE0);
    BindTextureDirect(GL_TEXTURE_2D, captureTexture);
    glActiveTexture(GL_TEXTURE1);
    BindTextureDirect(GL_TEXTURE_2D, inst->tempCaptureTexture);

    {
        MT_ScopedTextureSamplingGuard captureSamplingGuard(captureTexture, GL_NEAREST, GL_NEAREST);

        if (conf.colorPassthrough) {
            MT_UseProgramCached(mt_dilateVerticalPassthroughProgram, stateCache);
            if (!stateCache || !stateCache->dilateVerticalPassthroughBorderWidthValid ||
                stateCache->dilateVerticalPassthroughBorderWidth != conf.dynamicBorderThickness) {
                glUniform1i(mt_dilateVerticalPassthroughShaderLocs.borderWidth, conf.dynamicBorderThickness);
                if (stateCache) {
                    stateCache->dilateVerticalPassthroughBorderWidth = conf.dynamicBorderThickness;
                    stateCache->dilateVerticalPassthroughBorderWidthValid = true;
                }
            }
            if (!stateCache || !stateCache->dilateVerticalPassthroughScreenPixelValid ||
                stateCache->dilateVerticalPassthroughScreenPixelX != screenPixelX ||
                stateCache->dilateVerticalPassthroughScreenPixelY != screenPixelY) {
                glUniform2f(mt_dilateVerticalPassthroughShaderLocs.screenPixel, screenPixelX, screenPixelY);
                if (stateCache) {
                    stateCache->dilateVerticalPassthroughScreenPixelX = screenPixelX;
                    stateCache->dilateVerticalPassthroughScreenPixelY = screenPixelY;
                    stateCache->dilateVerticalPassthroughScreenPixelValid = true;
                }
            }
            if (!stateCache || !stateCache->dilateVerticalPassthroughBorderColorValid ||
                !MT_ColorEquals(stateCache->dilateVerticalPassthroughBorderColor, conf.borderColor)) {
                glUniform4f(mt_dilateVerticalPassthroughShaderLocs.borderColor, conf.borderColor.r, conf.borderColor.g,
                            conf.borderColor.b, conf.borderColor.a);
                if (stateCache) {
                    stateCache->dilateVerticalPassthroughBorderColor = conf.borderColor;
                    stateCache->dilateVerticalPassthroughBorderColorValid = true;
                }
            }
        } else {
            MT_UseProgramCached(mt_dilateVerticalProgram, stateCache);
            if (!stateCache || !stateCache->dilateVerticalBorderWidthValid ||
                stateCache->dilateVerticalBorderWidth != conf.dynamicBorderThickness) {
                glUniform1i(mt_dilateVerticalShaderLocs.borderWidth, conf.dynamicBorderThickness);
                if (stateCache) {
                    stateCache->dilateVerticalBorderWidth = conf.dynamicBorderThickness;
                    stateCache->dilateVerticalBorderWidthValid = true;
                }
            }
            if (!stateCache || !stateCache->dilateVerticalScreenPixelValid || stateCache->dilateVerticalScreenPixelX != screenPixelX ||
                stateCache->dilateVerticalScreenPixelY != screenPixelY) {
                glUniform2f(mt_dilateVerticalShaderLocs.screenPixel, screenPixelX, screenPixelY);
                if (stateCache) {
                    stateCache->dilateVerticalScreenPixelX = screenPixelX;
                    stateCache->dilateVerticalScreenPixelY = screenPixelY;
                    stateCache->dilateVerticalScreenPixelValid = true;
                }
            }
            if (!stateCache || !stateCache->dilateVerticalOutputColorValid ||
                !MT_ColorEquals(stateCache->dilateVerticalOutputColor, conf.outputColor)) {
                glUniform4f(mt_dilateVerticalShaderLocs.outputColor, conf.outputColor.r, conf.outputColor.g, conf.outputColor.b,
                            conf.outputColor.a);
                if (stateCache) {
                    stateCache->dilateVerticalOutputColor = conf.outputColor;
                    stateCache->dilateVerticalOutputColorValid = true;
                }
            }
            if (!stateCache || !stateCache->dilateVerticalBorderColorValid ||
                !MT_ColorEquals(stateCache->dilateVerticalBorderColor, conf.borderColor)) {
                glUniform4f(mt_dilateVerticalShaderLocs.borderColor, conf.borderColor.r, conf.borderColor.g, conf.borderColor.b,
                            conf.borderColor.a);
                if (stateCache) {
                    stateCache->dilateVerticalBorderColor = conf.borderColor;
                    stateCache->dilateVerticalBorderColorValid = true;
                }
            }
        }
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    glActiveTexture(GL_TEXTURE1);
    BindTextureDirect(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    BindTextureDirect(GL_TEXTURE_2D, finalTexture);
    if (stateCache) { stateCache->textureValid = false; }
    return true;
}

static bool RenderMirrorToBuffer(MirrorInstance* inst, const ThreadedMirrorConfig& conf, GLuint validCopyTexture, GLuint captureVAO,
                                 GLuint captureVBO, MT_SourceRectGpuCacheEntry& sourceRectGpuCache, GLuint captureFbo,
                                 GLuint captureTempFbo, GLuint* captureTempTextureId, GLuint captureFinalFbo,
                                 MirrorGammaMode gammaMode, int gameW, int gameH, bool writeToBack,
                                 MT_RenderToBufferStateCache* stateCache = nullptr,
                                 bool fixedStateAlreadyPrepared = false) {
    PROFILE_SCOPE_CAT("Capture Single Mirror", "Mirror Thread");

    const GLuint captureTexture = writeToBack ? inst->fboTextureBack : inst->fboTexture;
    const GLuint finalTexture = writeToBack ? inst->finalTextureBack : inst->finalTexture;
    const int finalW = writeToBack ? inst->final_w_back : inst->final_w;
    const int finalH = writeToBack ? inst->final_h_back : inst->final_h;

    bool useRawOutput = inst->desiredRawOutput.load(std::memory_order_acquire);
    bool useColorPassthrough = conf.colorPassthrough;
    bool useGradientOutput = conf.gradientOutput && !useRawOutput && !useColorPassthrough;
    const bool renderDirectToFinal =
        captureFinalFbo != 0 && finalTexture != 0 && MT_CanRenderMirrorDirectToFinal(conf, useRawOutput, writeToBack);

    const GLuint initialTargetFbo = renderDirectToFinal ? captureFinalFbo : captureFbo;
    const int initialTargetW = renderDirectToFinal ? finalW : inst->fbo_w;
    const int initialTargetH = renderDirectToFinal ? finalH : inst->fbo_h;

    MT_BindFramebufferCached(initialTargetFbo, stateCache);
    MT_SetViewportCached(0, 0, initialTargetW, initialTargetH, stateCache);

    if (!fixedStateAlreadyPrepared) {
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glActiveTexture(GL_TEXTURE0);
        glBindVertexArray(captureVAO);
        glBindBuffer(GL_ARRAY_BUFFER, captureVBO);
    }

    MT_SetClearColorCached(0.0f, 0.0f, 0.0f, (renderDirectToFinal && useRawOutput) ? 1.0f : 0.0f, stateCache);
    glClear(GL_COLOR_BUFFER_BIT);

    MT_BindTextureCached(validCopyTexture, stateCache);

    {
        // Newer Minecraft versions can leave the live source texture with hostile sampler or filter state.
        MT_ScopedTextureSamplingGuard sourceSamplingGuard(validCopyTexture, GL_NEAREST, GL_NEAREST, GL_TEXTURE0, true);

        if (useRawOutput) {
            MT_UseProgramCached(mt_passthroughProgram, stateCache);
        } else if (useColorPassthrough) {
            const int colorCount = (std::min)(static_cast<int>(conf.targetColors.size()), 8);
            std::array<float, 24> targetColorData{};
            for (int i = 0; i < colorCount; ++i) {
                targetColorData[static_cast<size_t>(i) * 3 + 0] = conf.targetColors[i].r;
                targetColorData[static_cast<size_t>(i) * 3 + 1] = conf.targetColors[i].g;
                targetColorData[static_cast<size_t>(i) * 3 + 2] = conf.targetColors[i].b;
            }

            MT_UseProgramCached(mt_filterPassthroughProgram, stateCache);
            if (mt_filterPassthroughShaderLocs.gammaMode >= 0 &&
                (!stateCache || !stateCache->filterPassthroughGammaModeValid ||
                 stateCache->filterPassthroughGammaMode != static_cast<int>(gammaMode))) {
                glUniform1i(mt_filterPassthroughShaderLocs.gammaMode, static_cast<int>(gammaMode));
                if (stateCache) {
                    stateCache->filterPassthroughGammaMode = static_cast<int>(gammaMode);
                    stateCache->filterPassthroughGammaModeValid = true;
                }
            }

            if (!stateCache || !stateCache->filterPassthroughTargetColorStateValid ||
                stateCache->filterPassthroughTargetColorCount != colorCount ||
                !std::equal(targetColorData.begin(), targetColorData.begin() + (static_cast<size_t>(colorCount) * 3),
                            stateCache->filterPassthroughTargetColors.begin())) {
                glUniform1i(mt_filterPassthroughShaderLocs.targetColorCount, colorCount);
                if (colorCount > 0) {
                    glUniform3fv(mt_filterPassthroughShaderLocs.targetColors, colorCount, targetColorData.data());
                }
                if (stateCache) {
                    stateCache->filterPassthroughTargetColorCount = colorCount;
                    stateCache->filterPassthroughTargetColors = targetColorData;
                    stateCache->filterPassthroughTargetColorStateValid = true;
                }
            }

            if (!stateCache || !stateCache->filterPassthroughSensitivityValid ||
                stateCache->filterPassthroughSensitivity != conf.colorSensitivity) {
                glUniform1f(mt_filterPassthroughShaderLocs.sensitivity, conf.colorSensitivity);
                if (stateCache) {
                    stateCache->filterPassthroughSensitivity = conf.colorSensitivity;
                    stateCache->filterPassthroughSensitivityValid = true;
                }
            }
        } else {
            const int colorCount = (std::min)(static_cast<int>(conf.targetColors.size()), 8);
            std::array<float, 24> targetColorData{};
            for (int i = 0; i < colorCount; ++i) {
                targetColorData[static_cast<size_t>(i) * 3 + 0] = conf.targetColors[i].r;
                targetColorData[static_cast<size_t>(i) * 3 + 1] = conf.targetColors[i].g;
                targetColorData[static_cast<size_t>(i) * 3 + 2] = conf.targetColors[i].b;
            }

            MT_UseProgramCached(mt_filterProgram, stateCache);
            if (mt_filterShaderLocs.gammaMode >= 0 &&
                (!stateCache || !stateCache->filterGammaModeValid || stateCache->filterGammaMode != static_cast<int>(gammaMode))) {
                glUniform1i(mt_filterShaderLocs.gammaMode, static_cast<int>(gammaMode));
                if (stateCache) {
                    stateCache->filterGammaMode = static_cast<int>(gammaMode);
                    stateCache->filterGammaModeValid = true;
                }
            }

            if (!stateCache || !stateCache->filterTargetColorStateValid || stateCache->filterTargetColorCount != colorCount ||
                !std::equal(targetColorData.begin(), targetColorData.begin() + (static_cast<size_t>(colorCount) * 3),
                            stateCache->filterTargetColors.begin())) {
                glUniform1i(mt_filterShaderLocs.targetColorCount, colorCount);
                if (colorCount > 0) {
                    glUniform3fv(mt_filterShaderLocs.targetColors, colorCount, targetColorData.data());
                }
                if (stateCache) {
                    stateCache->filterTargetColorCount = colorCount;
                    stateCache->filterTargetColors = targetColorData;
                    stateCache->filterTargetColorStateValid = true;
                }
            }

            if (!stateCache || !stateCache->filterOutputColorValid || !MT_ColorEquals(stateCache->filterOutputColor, conf.outputColor)) {
                glUniform4f(mt_filterShaderLocs.outputColor, conf.outputColor.r, conf.outputColor.g, conf.outputColor.b,
                            conf.outputColor.a);
                if (stateCache) {
                    stateCache->filterOutputColor = conf.outputColor;
                    stateCache->filterOutputColorValid = true;
                }
            }
            if (!stateCache || !stateCache->filterSensitivityValid || stateCache->filterSensitivity != conf.colorSensitivity) {
                glUniform1f(mt_filterShaderLocs.sensitivity, conf.colorSensitivity);
                if (stateCache) {
                    stateCache->filterSensitivity = conf.colorSensitivity;
                    stateCache->filterSensitivityValid = true;
                }
            }
        }

        if (useRawOutput) {
            MT_SetBlendEnabledCached(false, stateCache);
        } else {
            MT_SetBlendEnabledCached(true, stateCache);
        }

        int padding = renderDirectToFinal ? 0 : ((conf.borderType == MirrorBorderType::Dynamic) ? conf.dynamicBorderThickness : 0);
        const int drawWidth = renderDirectToFinal ? finalW : conf.captureWidth;
        const int drawHeight = renderDirectToFinal ? finalH : conf.captureHeight;
        MT_SetViewportCached(padding, padding, drawWidth, drawHeight, stateCache);
        const int instanceCount = MT_UploadSourceRectInstances(sourceRectGpuCache, conf, gameW, gameH, stateCache);
        glDrawArraysInstanced(GL_TRIANGLES, 0, 6, instanceCount);
    }

    MT_SetBlendEnabledCached(false, stateCache);

    // Uses async PBO readback: previous frame's result is harvested (non-blocking), then a
    if (useRawOutput) {
        if (writeToBack) {
            inst->hasFrameContentBack = true;
        } else {
            inst->hasFrameContent = true;
        }
    }

    if (renderDirectToFinal || MT_CanCompositeDynamicBorderOnScreen(conf, useRawOutput, writeToBack)) { return true; }

    // This produces screen-ready content so render thread just needs to blit.
    // Use the mirror-thread-local FBO (framebuffer objects may not be shared across contexts).
    if (captureFinalFbo != 0 && finalTexture != 0) {
        PROFILE_SCOPE_CAT("Apply Border Shader", "Mirror Thread");

        if (MT_ShouldUseSeparableDynamicBorder(conf, useRawOutput)) {
            return MT_RenderSeparableDynamicBorder(inst, conf, captureTexture, captureTempFbo, captureFinalFbo,
                                                  captureTempTextureId, finalTexture, finalW, finalH, stateCache);
        }

        if (useRawOutput) {
            MT_ScopedTextureSamplingGuard captureSamplingGuard(captureTexture, GL_NEAREST, GL_NEAREST);
            MT_BindFramebufferCached(captureFinalFbo, stateCache);
            MT_SetViewportCached(0, 0, finalW, finalH, stateCache);

            MT_BindTextureCached(captureTexture, stateCache);
            MT_UseProgramCached(mt_backgroundProgram, stateCache);
            if (!stateCache || !stateCache->backgroundOpacityValid || stateCache->backgroundOpacity != 1.0f) {
                glUniform1f(mt_backgroundShaderLocs.opacity, 1.0f);
                if (stateCache) {
                    stateCache->backgroundOpacity = 1.0f;
                    stateCache->backgroundOpacityValid = true;
                }
            }
            glDrawArrays(GL_TRIANGLES, 0, 6);
        } else if (useGradientOutput) {
            MT_ScopedTextureSamplingGuard captureSamplingGuard(captureTexture, GL_NEAREST, GL_NEAREST);
            MT_BindFramebufferCached(captureFinalFbo, stateCache);
            MT_SetViewportCached(0, 0, finalW, finalH, stateCache);
            MT_SetClearColorCached(0.0f, 0.0f, 0.0f, 0.0f, stateCache);
            glClear(GL_COLOR_BUFFER_BIT);

            MT_BindTextureCached(captureTexture, stateCache);
            MT_UseProgramCached(mt_maskedGradientProgram, stateCache);

            const int borderWidth = conf.borderType == MirrorBorderType::Dynamic ? conf.dynamicBorderThickness : 0;
            glUniform1i(mt_maskedGradientShaderLocs.borderWidth, borderWidth);
            glUniform4f(mt_maskedGradientShaderLocs.borderColor, conf.borderColor.r, conf.borderColor.g, conf.borderColor.b,
                        conf.borderColor.a);
            glUniform2f(mt_maskedGradientShaderLocs.screenPixel, 1.0f / (std::max)(1, finalW),
                        1.0f / (std::max)(1, finalH));

            const int numStops = (std::min)(static_cast<int>(conf.gradient.gradientStops.size()), MAX_GRADIENT_STOPS);
            float colors[MAX_GRADIENT_STOPS * 4] = {};
            float positions[MAX_GRADIENT_STOPS] = {};
            for (int i = 0; i < numStops; ++i) {
                colors[i * 4 + 0] = conf.gradient.gradientStops[i].color.r;
                colors[i * 4 + 1] = conf.gradient.gradientStops[i].color.g;
                colors[i * 4 + 2] = conf.gradient.gradientStops[i].color.b;
                colors[i * 4 + 3] = conf.gradient.gradientStops[i].color.a;
                positions[i] = conf.gradient.gradientStops[i].position;
            }
            glUniform1i(mt_maskedGradientShaderLocs.numStops, numStops);
            glUniform4fv(mt_maskedGradientShaderLocs.stopColors, numStops, colors);
            glUniform1fv(mt_maskedGradientShaderLocs.stopPositions, numStops, positions);
            glUniform1f(mt_maskedGradientShaderLocs.angle, conf.gradient.gradientAngle * 3.14159265f / 180.0f);

            static auto startTime = std::chrono::steady_clock::now();
            const auto now = std::chrono::steady_clock::now();
            const float timeSeconds = std::chrono::duration<float>(now - startTime).count();
            glUniform1f(mt_maskedGradientShaderLocs.time, timeSeconds);
            glUniform1i(mt_maskedGradientShaderLocs.animationType, static_cast<int>(conf.gradient.gradientAnimation));
            glUniform1f(mt_maskedGradientShaderLocs.animationSpeed, conf.gradient.gradientAnimationSpeed);
            glUniform1i(mt_maskedGradientShaderLocs.colorFade, conf.gradient.gradientColorFade ? 1 : 0);

            glDrawArrays(GL_TRIANGLES, 0, 6);
        } else if (conf.borderType == MirrorBorderType::Static) {
            MT_ScopedTextureSamplingGuard captureSamplingGuard(captureTexture, GL_NEAREST, GL_NEAREST);
            // Static border is rendered later during final mirror composition.
            MT_BindFramebufferCached(captureFinalFbo, stateCache);
            MT_SetViewportCached(0, 0, finalW, finalH, stateCache);

            MT_BindTextureCached(captureTexture, stateCache);
            MT_UseProgramCached(mt_backgroundProgram, stateCache);
            if (!stateCache || !stateCache->backgroundOpacityValid || stateCache->backgroundOpacity != 1.0f) {
                glUniform1f(mt_backgroundShaderLocs.opacity, 1.0f);
                if (stateCache) {
                    stateCache->backgroundOpacity = 1.0f;
                    stateCache->backgroundOpacityValid = true;
                }
            }
            glDrawArrays(GL_TRIANGLES, 0, 6);
        } else {
            MT_ScopedTextureSamplingGuard captureSamplingGuard(captureTexture, GL_NEAREST, GL_NEAREST);
            MT_BindFramebufferCached(captureFinalFbo, stateCache);
            MT_SetViewportCached(0, 0, finalW, finalH, stateCache);
            MT_SetClearColorCached(0.0f, 0.0f, 0.0f, 0.0f, stateCache);
            glClear(GL_COLOR_BUFFER_BIT);

            MT_BindTextureCached(captureTexture, stateCache);
            if (useColorPassthrough) {
                MT_UseProgramCached(mt_renderPassthroughProgram, stateCache);
                if (!stateCache || !stateCache->renderPassthroughBorderWidthValid ||
                    stateCache->renderPassthroughBorderWidth != conf.dynamicBorderThickness) {
                    glUniform1i(mt_renderPassthroughShaderLocs.borderWidth, conf.dynamicBorderThickness);
                    if (stateCache) {
                        stateCache->renderPassthroughBorderWidth = conf.dynamicBorderThickness;
                        stateCache->renderPassthroughBorderWidthValid = true;
                    }
                }
                if (!stateCache || !stateCache->renderPassthroughBorderColorValid ||
                    !MT_ColorEquals(stateCache->renderPassthroughBorderColor, conf.borderColor)) {
                    glUniform4f(mt_renderPassthroughShaderLocs.borderColor, conf.borderColor.r, conf.borderColor.g,
                                conf.borderColor.b, conf.borderColor.a);
                    if (stateCache) {
                        stateCache->renderPassthroughBorderColor = conf.borderColor;
                        stateCache->renderPassthroughBorderColorValid = true;
                    }
                }
                const float screenPixelX = 1.0f / finalW;
                const float screenPixelY = 1.0f / finalH;
                if (!stateCache || !stateCache->renderPassthroughScreenPixelValid ||
                    stateCache->renderPassthroughScreenPixelX != screenPixelX ||
                    stateCache->renderPassthroughScreenPixelY != screenPixelY) {
                    glUniform2f(mt_renderPassthroughShaderLocs.screenPixel, screenPixelX, screenPixelY);
                    if (stateCache) {
                        stateCache->renderPassthroughScreenPixelX = screenPixelX;
                        stateCache->renderPassthroughScreenPixelY = screenPixelY;
                        stateCache->renderPassthroughScreenPixelValid = true;
                    }
                }
            } else {
                MT_UseProgramCached(mt_renderProgram, stateCache);
                if (!stateCache || !stateCache->renderBorderWidthValid || stateCache->renderBorderWidth != conf.dynamicBorderThickness) {
                    glUniform1i(mt_renderShaderLocs.borderWidth, conf.dynamicBorderThickness);
                    if (stateCache) {
                        stateCache->renderBorderWidth = conf.dynamicBorderThickness;
                        stateCache->renderBorderWidthValid = true;
                    }
                }
                if (!stateCache || !stateCache->renderOutputColorValid || !MT_ColorEquals(stateCache->renderOutputColor, conf.outputColor)) {
                    glUniform4f(mt_renderShaderLocs.outputColor, conf.outputColor.r, conf.outputColor.g, conf.outputColor.b,
                                conf.outputColor.a);
                    if (stateCache) {
                        stateCache->renderOutputColor = conf.outputColor;
                        stateCache->renderOutputColorValid = true;
                    }
                }
                if (!stateCache || !stateCache->renderBorderColorValid || !MT_ColorEquals(stateCache->renderBorderColor, conf.borderColor)) {
                    glUniform4f(mt_renderShaderLocs.borderColor, conf.borderColor.r, conf.borderColor.g, conf.borderColor.b,
                                conf.borderColor.a);
                    if (stateCache) {
                        stateCache->renderBorderColor = conf.borderColor;
                        stateCache->renderBorderColorValid = true;
                    }
                }
                const float screenPixelX = 1.0f / finalW;
                const float screenPixelY = 1.0f / finalH;
                if (!stateCache || !stateCache->renderScreenPixelValid || stateCache->renderScreenPixelX != screenPixelX ||
                    stateCache->renderScreenPixelY != screenPixelY) {
                    glUniform2f(mt_renderShaderLocs.screenPixel, screenPixelX, screenPixelY);
                    if (stateCache) {
                        stateCache->renderScreenPixelX = screenPixelX;
                        stateCache->renderScreenPixelY = screenPixelY;
                        stateCache->renderScreenPixelValid = true;
                    }
                }
            }

            glDrawArrays(GL_TRIANGLES, 0, 6);
        }

        // NOTE: Static border is rendered during final mirror compositing.
    }

    return true;
}

// Mirror-thread local FBOs.
// Framebuffer objects are not reliably shared between WGL contexts across all drivers.
struct MT_MirrorFbos {
    GLuint backFbo = 0;
    GLuint tempBackFbo = 0;
    GLuint finalBackFbo = 0;
    GLuint lastBackTex = 0;
    GLuint lastTempTex = 0;
    GLuint lastFinalBackTex = 0;

    // Async PBO for content detection (replaces synchronous glReadPixels)
    // Frame N+1: read back results from PBO (non-blocking) before starting new readback
    GLuint contentDetectionPBO = 0;
    int contentPBOWidth = 0;
    int contentPBOHeight = 0;
    bool contentReadbackPending = false;
    GLsync contentReadbackFence = nullptr; // Fence for the async readback

    GLuint contentDownsampleFbo = 0;
    GLuint contentDownsampleTex = 0;
    int contentDownW = 0;
    int contentDownH = 0;
};

static void MT_ReleaseContentDetectionResources(MT_MirrorFbos& fb) {
    if (fb.contentReadbackFence && glIsSync(fb.contentReadbackFence)) { glDeleteSync(fb.contentReadbackFence); }
    fb.contentReadbackFence = nullptr;
    fb.contentReadbackPending = false;

    if (fb.contentDetectionPBO) {
        glDeleteBuffers(1, &fb.contentDetectionPBO);
        fb.contentDetectionPBO = 0;
    }
    fb.contentPBOWidth = 0;
    fb.contentPBOHeight = 0;

    if (fb.contentDownsampleFbo) {
        glDeleteFramebuffers(1, &fb.contentDownsampleFbo);
        fb.contentDownsampleFbo = 0;
    }
    if (fb.contentDownsampleTex) {
        glDeleteTextures(1, &fb.contentDownsampleTex);
        fb.contentDownsampleTex = 0;
    }
    fb.contentDownW = 0;
    fb.contentDownH = 0;
}

static void MT_DeleteMirrorFbos(MT_MirrorFbos& fb) {
    if (fb.backFbo) { glDeleteFramebuffers(1, &fb.backFbo); }
    if (fb.tempBackFbo) { glDeleteFramebuffers(1, &fb.tempBackFbo); }
    if (fb.finalBackFbo) { glDeleteFramebuffers(1, &fb.finalBackFbo); }
    fb.backFbo = 0;
    fb.tempBackFbo = 0;
    fb.finalBackFbo = 0;
    fb.lastBackTex = 0;
    fb.lastTempTex = 0;
    fb.lastFinalBackTex = 0;

    MT_ReleaseContentDetectionResources(fb);
}

static bool MT_MirrorNeedsContentDetection(const ThreadedMirrorConfig& conf, bool useRawOutput) {
    return !useRawOutput && conf.borderType == MirrorBorderType::Static && conf.staticBorderThickness > 0;
}

static bool MT_HarvestContentReadback(MT_MirrorFbos& fb, bool& outHasContent) {
    if (!fb.contentReadbackPending || !fb.contentReadbackFence) { return false; }

    GLenum fenceStatus = glClientWaitSync(fb.contentReadbackFence, 0, 0);
    if (fenceStatus != GL_ALREADY_SIGNALED && fenceStatus != GL_CONDITION_SATISFIED) { return false; }

    bool hasContent = false;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, fb.contentDetectionPBO);
    const unsigned char* mapped = static_cast<const unsigned char*>(
        glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, fb.contentPBOWidth * fb.contentPBOHeight * 4, GL_MAP_READ_BIT));
    if (mapped) {
        const int w = fb.contentPBOWidth;
        const int h = fb.contentPBOHeight;
        for (int y = 0; y < h && !hasContent; ++y) {
            const unsigned char* row = mapped + (static_cast<size_t>(y) * w * 4);
            for (int x = 0; x < w; ++x) {
                if (row[(static_cast<size_t>(x) * 4) + 3] > 0) {
                    hasContent = true;
                    break;
                }
            }
        }
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    if (glIsSync(fb.contentReadbackFence)) { glDeleteSync(fb.contentReadbackFence); }
    fb.contentReadbackFence = nullptr;
    fb.contentReadbackPending = false;

    outHasContent = hasContent;
    return true;
}

static bool MT_DetectContentSynchronously(MirrorInstance* inst, GLuint sourceFbo, int sourceW, int sourceH) {
    if (!inst || sourceFbo == 0 || sourceW <= 0 || sourceH <= 0) { return false; }

    const size_t requiredBytes = static_cast<size_t>(sourceW) * static_cast<size_t>(sourceH) * 4;
    if (inst->pixelBuffer.size() < requiredBytes) {
        inst->pixelBuffer.resize(requiredBytes);
    }

    GLint previousReadFramebuffer = 0;
    GLint previousPackRowLength = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &previousPackRowLength);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, sourceFbo);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glReadPixels(0, 0, sourceW, sourceH, GL_RGBA, GL_UNSIGNED_BYTE, inst->pixelBuffer.data());
    glPixelStorei(GL_PACK_ROW_LENGTH, previousPackRowLength);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousReadFramebuffer));

    const unsigned char* pixels = inst->pixelBuffer.data();
    for (size_t i = 3; i < requiredBytes; i += 4) {
        if (pixels[i] > 0) { return true; }
    }

    return false;
}

static void MT_QueueContentReadback(MT_MirrorFbos& fb, GLuint sourceFbo, int sourceW, int sourceH,
                                    bool preserveSinglePixels = false) {
    if (sourceFbo == 0 || sourceW <= 0 || sourceH <= 0) {
        MT_ReleaseContentDetectionResources(fb);
        return;
    }

    constexpr int kDetectMax = 64;
    const bool useDownsample = !preserveSinglePixels;
    const int detW = useDownsample ? (std::min)(sourceW, kDetectMax) : sourceW;
    const int detH = useDownsample ? (std::min)(sourceH, kDetectMax) : sourceH;
    if (detW <= 0 || detH <= 0) {
        MT_ReleaseContentDetectionResources(fb);
        return;
    }

    if (useDownsample &&
        ((fb.contentDownsampleFbo == 0) || (fb.contentDownsampleTex == 0) || (fb.contentDownW != detW) || (fb.contentDownH != detH))) {
        if (fb.contentDownsampleFbo == 0) { glGenFramebuffers(1, &fb.contentDownsampleFbo); }
        if (fb.contentDownsampleTex == 0) { glGenTextures(1, &fb.contentDownsampleTex); }
        BindTextureDirect(GL_TEXTURE_2D, fb.contentDownsampleTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, detW, detH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        BindTextureDirect(GL_TEXTURE_2D, 0);

        glBindFramebuffer(GL_FRAMEBUFFER, fb.contentDownsampleFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fb.contentDownsampleTex, 0);
        fb.contentDownW = detW;
        fb.contentDownH = detH;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    } else if (!useDownsample) {
        if (fb.contentDownsampleFbo) {
            glDeleteFramebuffers(1, &fb.contentDownsampleFbo);
            fb.contentDownsampleFbo = 0;
        }
        if (fb.contentDownsampleTex) {
            glDeleteTextures(1, &fb.contentDownsampleTex);
            fb.contentDownsampleTex = 0;
        }
        fb.contentDownW = 0;
        fb.contentDownH = 0;
    }

    if (fb.contentDetectionPBO == 0 || fb.contentPBOWidth != detW || fb.contentPBOHeight != detH) {
        if (fb.contentDetectionPBO == 0) { glGenBuffers(1, &fb.contentDetectionPBO); }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, fb.contentDetectionPBO);
        glBufferData(GL_PIXEL_PACK_BUFFER, detW * detH * 4, nullptr, GL_STREAM_READ);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        fb.contentPBOWidth = detW;
        fb.contentPBOHeight = detH;
    }

    if (fb.contentReadbackFence && glIsSync(fb.contentReadbackFence)) { glDeleteSync(fb.contentReadbackFence); }
    fb.contentReadbackFence = nullptr;

    if (useDownsample) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, sourceFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb.contentDownsampleFbo);
        BlitFramebufferDirect(0, 0, sourceW, sourceH, 0, 0, detW, detH, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fb.contentDownsampleFbo);
    } else {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, sourceFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    }

    GLint previousPackRowLength = 0;
    glGetIntegerv(GL_PACK_ROW_LENGTH, &previousPackRowLength);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, fb.contentDetectionPBO);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glReadPixels(0, 0, detW, detH, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glPixelStorei(GL_PACK_ROW_LENGTH, previousPackRowLength);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

    fb.contentReadbackFence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    fb.contentReadbackPending = (fb.contentReadbackFence != nullptr);
}

static std::unordered_map<std::string, MT_MirrorFbos> g_sameThreadMirrorFbos;
static std::unordered_map<std::string, MT_SourceRectGpuCacheEntry> g_sameThreadSourceRectGpuCaches;
static GLuint g_sameThreadCaptureVAO = 0;
static GLuint g_sameThreadCaptureVBO = 0;

void CleanupMirrorCaptureGpuResources() {
    for (auto& [name, fb] : g_sameThreadMirrorFbos) {
        (void)name;
        MT_DeleteMirrorFbos(fb);
    }
    g_sameThreadMirrorFbos.clear();

    for (auto& [name, cacheEntry] : g_sameThreadSourceRectGpuCaches) {
        (void)name;
        MT_DeleteSourceRectGpuCacheEntry(cacheEntry);
    }
    g_sameThreadSourceRectGpuCaches.clear();

    if (g_sameThreadCaptureVAO != 0) {
        glDeleteVertexArrays(1, &g_sameThreadCaptureVAO);
        g_sameThreadCaptureVAO = 0;
    }
    if (g_sameThreadCaptureVBO != 0) {
        glDeleteBuffers(1, &g_sameThreadCaptureVBO);
        g_sameThreadCaptureVBO = 0;
    }

    MT_CleanupShaders();
}

static void EnsureSameThreadMirrorCaptureResources() {
    if (g_sameThreadCaptureVAO != 0 && g_sameThreadCaptureVBO != 0) { return; }

    glGenVertexArrays(1, &g_sameThreadCaptureVAO);
    glGenBuffers(1, &g_sameThreadCaptureVBO);
    glBindVertexArray(g_sameThreadCaptureVAO);
    glBindBuffer(GL_ARRAY_BUFFER, g_sameThreadCaptureVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 24, nullptr, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(2);
    glVertexAttribDivisor(2, 1);

    static const float verts[] = { -1, -1, 0, 0, 1, -1, 1, 0, 1, 1, 1, 1, -1, -1, 0, 0, 1, 1, 1, 1, -1, 1, 0, 1 };
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
}

static std::string MT_NormalizeCaptureAnchor(const std::string& relativeTo) {
    if (relativeTo.length() > 8 && relativeTo.substr(relativeTo.length() - 8) == "Viewport") {
        return relativeTo.substr(0, relativeTo.length() - 8);
    }
    if (relativeTo.length() > 6 && relativeTo.substr(relativeTo.length() - 6) == "Screen") {
        return relativeTo.substr(0, relativeTo.length() - 6);
    }
    return relativeTo;
}

static std::vector<MirrorCaptureConfig> MT_NormalizeCaptureInputs(const std::vector<MirrorCaptureConfig>& inputRegions) {
    std::vector<MirrorCaptureConfig> normalized = inputRegions;
    for (auto& region : normalized) {
        region.relativeTo = MT_NormalizeCaptureAnchor(region.relativeTo);
    }
    return normalized;
}

static void MT_CopyNormalizedCaptureInputs(const std::vector<MirrorCaptureConfig>& inputRegions,
                                           std::vector<MirrorCaptureConfig>& outRegions) {
    outRegions.resize(inputRegions.size());
    for (size_t i = 0; i < inputRegions.size(); ++i) {
        outRegions[i] = inputRegions[i];
        outRegions[i].relativeTo = MT_NormalizeCaptureAnchor(inputRegions[i].relativeTo);
    }
}

static void MT_GetRelativeCoordsNormalized(const std::string& anchor, int relX, int relY, int w, int h, int containerW,
                                           int containerH, int& outX, int& outY) {
    char firstChar = anchor.empty() ? '\0' : anchor[0];

    if (firstChar == 't') {
        outY = relY;
        outX = (anchor == "topLeft") ? relX : containerW - w - relX;
    } else if (firstChar == 'c') {
        outX = (containerW - w) / 2 + relX;
        outY = (containerH - h) / 2 + relY;
    } else if (firstChar == 'p') {
        const int pieYTop = 220;
        const int pieXLeft = 92;
        const int pieXRight = 36;
        int baseX = (anchor == "pieLeft") ? containerW - pieXLeft : containerW - pieXRight;
        outX = baseX + relX;
        outY = containerH - pieYTop + relY;
    } else {
        outY = containerH - h - relY;
        outX = (anchor == "bottomRight") ? containerW - w - relX : relX;
    }
}

static void PopulateThreadedMirrorConfig(ThreadedMirrorConfig& conf, const MirrorConfig& mirror) {
    conf.name = mirror.name;
    conf.captureWidth = mirror.captureWidth;
    conf.captureHeight = mirror.captureHeight;
    conf.borderType = mirror.border.type;
    conf.dynamicBorderThickness = mirror.border.dynamicThickness;
    conf.staticBorderShape = mirror.border.staticShape;
    conf.staticBorderColor = mirror.border.staticColor;
    conf.staticBorderThickness = mirror.border.staticThickness;
    conf.staticBorderRadius = mirror.border.staticRadius;
    conf.staticBorderOffsetX = mirror.border.staticOffsetX;
    conf.staticBorderOffsetY = mirror.border.staticOffsetY;
    conf.staticBorderWidth = mirror.border.staticWidth;
    conf.staticBorderHeight = mirror.border.staticHeight;
    conf.fps = mirror.fps;
    conf.rawOutput = mirror.rawOutput;
    conf.colorPassthrough = mirror.colorPassthrough;
    conf.gradientOutput = mirror.gradientOutput;
    conf.gradient = mirror.gradient;
    conf.targetColors = mirror.colors.targetColors;
    conf.outputColor = mirror.colors.output;
    conf.borderColor = mirror.colors.border;
    conf.colorSensitivity = mirror.colorSensitivity;
    MT_CopyNormalizedCaptureInputs(mirror.input, conf.input);
    conf.outputScale = mirror.output.scale;
    conf.outputSeparateScale = mirror.output.separateScale;
    conf.outputScaleX = mirror.output.scaleX;
    conf.outputScaleY = mirror.output.scaleY;
    conf.outputX = mirror.output.x;
    conf.outputY = mirror.output.y;
    conf.outputRelativeTo = mirror.output.relativeTo;
    conf.sourceRectLayoutHash = MT_ComputeSourceRectLayoutHash(conf);
}

void BuildThreadedMirrorConfigs(const std::vector<MirrorConfig>& activeMirrors, std::vector<ThreadedMirrorConfig>& outConfigs) {
    outConfigs.resize(activeMirrors.size());

    for (size_t i = 0; i < activeMirrors.size(); ++i) {
        PopulateThreadedMirrorConfig(outConfigs[i], activeMirrors[i]);
    }
}

static void SwapMirrorInstanceBuffers(MirrorInstance& inst, const std::chrono::steady_clock::time_point& updateTime) {
    std::swap(inst.fbo, inst.fboBack);
    std::swap(inst.fboTexture, inst.fboTextureBack);
    std::swap(inst.capturedAsRawOutput, inst.capturedAsRawOutputBack);
    std::swap(inst.cachedRenderState, inst.cachedRenderStateBack);
    std::swap(inst.finalFbo, inst.finalFboBack);
    std::swap(inst.finalTexture, inst.finalTextureBack);
    std::swap(inst.final_w, inst.final_w_back);
    std::swap(inst.final_h, inst.final_h_back);
    std::swap(inst.hasFrameContent, inst.hasFrameContentBack);
    std::swap(inst.gpuFence, inst.gpuFenceBack);

    inst.hasValidContent = true;
    inst.captureReady.store(false, std::memory_order_release);
    inst.lastUpdateTime = updateTime;
}

// Call this from the active GL render path each frame.
void SwapMirrorBuffers() {
    std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex); // Write lock - swapping buffers
    auto now = std::chrono::steady_clock::now();

    for (auto& [name, inst] : g_mirrorInstances) {
        if (inst.captureReady.load(std::memory_order_acquire)) {
            SwapMirrorInstanceBuffers(inst, now);
        }
    }
}

bool RenderMirrorCapturesOnCurrentThread(const std::vector<ThreadedMirrorConfig>& activeMirrorConfigs, GLuint sourceTexture, int gameW,
                                         int gameH, int screenW, int screenH, int finalX, int finalY, int finalW, int finalH) {
    if (activeMirrorConfigs.empty() || sourceTexture == 0 || gameW <= 0 || gameH <= 0) { return false; }

    if (mt_filterProgram == 0 || mt_renderProgram == 0 || mt_backgroundProgram == 0) {
        if (!MT_InitializeShaders()) { return false; }
    }
    EnsureSameThreadMirrorCaptureResources();

    MirrorGammaMode gammaMode = GetGlobalMirrorGammaMode();
    bool renderedAny = false;
    MT_RenderToBufferStateCache stateCache{};

    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(g_sameThreadCaptureVAO);
    glBindBuffer(GL_ARRAY_BUFFER, g_sameThreadCaptureVBO);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    auto now = std::chrono::steady_clock::now();
    std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex);

    for (const auto& conf : activeMirrorConfigs) {
        if (conf.input.empty() || conf.captureWidth <= 0 || conf.captureHeight <= 0) { continue; }

        auto it = g_mirrorInstances.find(conf.name);
        if (it == g_mirrorInstances.end()) { continue; }

        MirrorInstance* inst = &it->second;
        if (!MirrorUsesEveryFrameUpdates(conf.fps) && inst->forceUpdateFrames <= 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - inst->lastUpdateTime).count();
            if (elapsed < (1000 / conf.fps)) { continue; }
        }

        inst->desiredRawOutput.store(conf.rawOutput, std::memory_order_relaxed);

        const bool needsFinalTarget = MT_SameThreadMirrorNeedsFinalTarget(conf, conf.rawOutput);

        {
            PROFILE_SCOPE_CAT("Prepare Mirror Capture Targets", "Rendering");
            int borderPadding = (!conf.rawOutput && conf.borderType == MirrorBorderType::Dynamic) ? conf.dynamicBorderThickness : 0;
            int requiredFboW = conf.captureWidth + 2 * borderPadding;
            int requiredFboH = conf.captureHeight + 2 * borderPadding;

            if (inst->fbo_w != requiredFboW || inst->fbo_h != requiredFboH) {
                inst->fbo_w = requiredFboW;
                inst->fbo_h = requiredFboH;
                inst->forceUpdateFrames = 3;
                inst->cachedRenderState.isValid = false;

                BindTextureDirect(GL_TEXTURE_2D, inst->fboTexture);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, inst->fbo_w, inst->fbo_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
                BindTextureDirect(GL_TEXTURE_2D, 0);
            }

            if (needsFinalTarget) {
                float finalScaleX = conf.outputSeparateScale ? conf.outputScaleX : conf.outputScale;
                float finalScaleY = conf.outputSeparateScale ? conf.outputScaleY : conf.outputScale;
                int requiredFinalW = static_cast<int>(inst->fbo_w * finalScaleX);
                int requiredFinalH = static_cast<int>(inst->fbo_h * finalScaleY);
                if (inst->final_w != requiredFinalW || inst->final_h != requiredFinalH) {
                    BindTextureDirect(GL_TEXTURE_2D, inst->finalTexture);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, requiredFinalW, requiredFinalH, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
                    BindTextureDirect(GL_TEXTURE_2D, 0);

                    inst->final_w = requiredFinalW;
                    inst->final_h = requiredFinalH;
                    inst->final_w_back = requiredFinalW;
                    inst->final_h_back = requiredFinalH;
                    inst->cachedRenderState.isValid = false;
                    inst->cachedRenderStateBack.isValid = false;
                }
            }
        }

        MT_MirrorFbos& fb = g_sameThreadMirrorFbos[conf.name];
        {
            PROFILE_SCOPE_CAT("Prepare Mirror Capture FBOs", "Rendering");
            if (fb.backFbo == 0) { glGenFramebuffers(1, &fb.backFbo); }
            if (fb.tempBackFbo == 0) { glGenFramebuffers(1, &fb.tempBackFbo); }
            if (fb.finalBackFbo == 0) { glGenFramebuffers(1, &fb.finalBackFbo); }
        }

        const bool needsContentDetection = MT_MirrorNeedsContentDetection(conf, conf.rawOutput);
        if (!needsContentDetection) {
            PROFILE_SCOPE_CAT("Mirror Content Detection", "Rendering");
            MT_ReleaseContentDetectionResources(fb);
            inst->hasFrameContent = true;
        }

        {
            PROFILE_SCOPE_CAT("Attach Mirror Capture Textures", "Rendering");
            if (fb.lastBackTex != inst->fboTexture) {
                glBindFramebuffer(GL_FRAMEBUFFER, fb.backFbo);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, inst->fboTexture, 0);
                fb.lastBackTex = inst->fboTexture;
            }
            if (needsFinalTarget && fb.lastFinalBackTex != inst->finalTexture) {
                glBindFramebuffer(GL_FRAMEBUFFER, fb.finalBackFbo);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, inst->finalTexture, 0);
                fb.lastFinalBackTex = inst->finalTexture;
            }
        }

        {
            PROFILE_SCOPE_CAT("Capture Mirror Into Buffer", "Rendering");
            const GLuint captureFinalFbo = needsFinalTarget ? fb.finalBackFbo : 0;
            RenderMirrorToBuffer(inst, conf, sourceTexture, g_sameThreadCaptureVAO, g_sameThreadCaptureVBO,
                                 g_sameThreadSourceRectGpuCaches[conf.name], fb.backFbo, fb.tempBackFbo, &fb.lastTempTex,
                                 captureFinalFbo, gammaMode, gameW, gameH, false, &stateCache, true);
        }

        {
            PROFILE_SCOPE_CAT("Finalize Mirror Capture", "Rendering");
            if (needsContentDetection) {
                MT_ReleaseContentDetectionResources(fb);
                GLuint contentSourceFbo = fb.backFbo;
                int contentSourceW = inst->fbo_w;
                int contentSourceH = inst->fbo_h;
                if (MT_CanRenderMirrorDirectToFinal(conf, conf.rawOutput, false) && fb.finalBackFbo != 0 && inst->final_w > 0 &&
                    inst->final_h > 0) {
                    contentSourceFbo = fb.finalBackFbo;
                    contentSourceW = inst->final_w;
                    contentSourceH = inst->final_h;
                }
                inst->hasFrameContent = MT_DetectContentSynchronously(inst, contentSourceFbo, contentSourceW, contentSourceH);
            }
            ComputeMirrorRenderCache(inst, conf, gameW, gameH, screenW, screenH, finalX, finalY, finalW, finalH, false);
            inst->capturedAsRawOutput = conf.rawOutput;
            if (inst->gpuFence && glIsSync(inst->gpuFence)) { glDeleteSync(inst->gpuFence); }
            inst->gpuFence = nullptr;
            inst->hasValidContent = true;
            inst->captureReady.store(false, std::memory_order_release);
            inst->lastUpdateTime = now;
            if (inst->forceUpdateFrames > 0) { inst->forceUpdateFrames--; }
        }
        renderedAny = true;
    }

    return renderedAny;
}

// Update capture configs from main thread (call when active mirrors change)
void UpdateMirrorCaptureConfigs(const std::vector<MirrorConfig>& activeMirrors) {
    std::vector<ThreadedMirrorConfig> configs;
    BuildThreadedMirrorConfigs(activeMirrors, configs);

    // Compute summaries from the local vector (avoid reading g_threadedMirrorConfigs without its mutex).
    const int mirrorCount = static_cast<int>(configs.size());
    int maxFps = 0;
    bool unlimited = false;
    for (const auto& c : configs) {
        if (c.fps <= 0) {
            unlimited = true;
            break;
        }
        maxFps = (std::max)(maxFps, c.fps);
    }

    // Clear captureReady for all mirrors to allow capture thread to start fresh
    // (captureReady would stay true if main thread never consumed the capture)
    {
        std::unique_lock<std::shared_mutex> clearLock(g_mirrorInstancesMutex);
        for (auto& [name, inst] : g_mirrorInstances) {
            inst.captureReady.store(false, std::memory_order_release);
            inst.cachedRenderState.isValid = false;
            inst.cachedRenderStateBack.isValid = false;
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_threadedMirrorConfigMutex);
        g_threadedMirrorConfigs = std::move(configs);
    }

    g_activeMirrorCaptureCount.store(mirrorCount, std::memory_order_release);

    g_activeMirrorCaptureMaxFps.store(unlimited ? 0 : maxFps, std::memory_order_release);

}

void UpdateMirrorFPS(const std::string& mirrorName, int fps) {
    std::lock_guard<std::mutex> lock(g_threadedMirrorConfigMutex);
    for (auto& conf : g_threadedMirrorConfigs) {
        if (conf.name == mirrorName) {
            conf.fps = fps;
            break;
        }
    }
    int maxFps = 0;
    bool unlimited = false;
    for (const auto& c : g_threadedMirrorConfigs) {
        if (c.fps <= 0) {
            unlimited = true;
            break;
        }
        maxFps = (std::max)(maxFps, c.fps);
    }
    g_activeMirrorCaptureMaxFps.store(unlimited ? 0 : maxFps, std::memory_order_release);
}

void UpdateMirrorOutputPosition(const std::string& mirrorName, int x, int y, float scale, bool separateScale, float scaleX, float scaleY,
                                const std::string& relativeTo) {
    // Update the threaded config
    {
        std::lock_guard<std::mutex> lock(g_threadedMirrorConfigMutex);
        for (auto& conf : g_threadedMirrorConfigs) {
            if (conf.name == mirrorName) {
                conf.outputX = x;
                conf.outputY = y;
                conf.outputScale = scale;
                conf.outputSeparateScale = separateScale;
                conf.outputScaleX = scaleX;
                conf.outputScaleY = scaleY;
                conf.outputRelativeTo = relativeTo;
                break;
            }
        }
    }
    // This ensures the active render path recalculates positions immediately.
    {
        std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex);
        auto it = g_mirrorInstances.find(mirrorName);
        if (it != g_mirrorInstances.end()) {
            // Front cache: recompute immediately.
            // Back cache: recompute on the next capture pass.
            it->second.cachedRenderState.isValid = false;
            it->second.cachedRenderStateBack.isValid = false;
        }
    }
}

void UpdateMirrorGroupOutputPosition(const std::vector<std::string>& mirrorIds, int x, int y, float scale, bool separateScale, float scaleX,
                                     float scaleY, const std::string& relativeTo) {
    (void)scale;
    (void)separateScale;
    (void)scaleX;
    (void)scaleY;

    // Update the threaded config for all mirrors in the group.
    // NOTE: Group moves only affect placement.
    {
        std::lock_guard<std::mutex> lock(g_threadedMirrorConfigMutex);
        for (auto& conf : g_threadedMirrorConfigs) {
            if (std::find(mirrorIds.begin(), mirrorIds.end(), conf.name) != mirrorIds.end()) {
                conf.outputX = x;
                conf.outputY = y;
                conf.outputRelativeTo = relativeTo;
            }
        }
    }

    {
        std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex);
        for (const auto& mirrorName : mirrorIds) {
            auto it = g_mirrorInstances.find(mirrorName);
            if (it != g_mirrorInstances.end()) {
                it->second.cachedRenderState.isValid = false;
                it->second.cachedRenderStateBack.isValid = false;
            }
        }
    }
}

void UpdateMirrorInputRegions(const std::string& mirrorName, const std::vector<MirrorCaptureConfig>& inputRegions) {
    std::lock_guard<std::mutex> lock(g_threadedMirrorConfigMutex);
    for (auto& conf : g_threadedMirrorConfigs) {
        if (conf.name == mirrorName) {
            conf.input = MT_NormalizeCaptureInputs(inputRegions);
            conf.sourceRectLayoutHash = MT_ComputeSourceRectLayoutHash(conf);
            break;
        }
    }
}

void UpdateMirrorCaptureSettings(const std::string& mirrorName, int captureWidth, int captureHeight, const MirrorBorderConfig& border,
                                 const MirrorColors& colors, float colorSensitivity, bool rawOutput, bool colorPassthrough,
                                 bool gradientOutput, const GradientConfig& gradient) {
    std::lock_guard<std::mutex> lock(g_threadedMirrorConfigMutex);
    for (auto& conf : g_threadedMirrorConfigs) {
        if (conf.name == mirrorName) {
            conf.captureWidth = captureWidth;
            conf.captureHeight = captureHeight;

            conf.borderType = border.type;
            conf.dynamicBorderThickness = border.dynamicThickness;
            conf.staticBorderShape = border.staticShape;
            conf.staticBorderColor = border.staticColor;
            conf.staticBorderThickness = border.staticThickness;
            conf.staticBorderRadius = border.staticRadius;
            conf.staticBorderOffsetX = border.staticOffsetX;
            conf.staticBorderOffsetY = border.staticOffsetY;
            conf.staticBorderWidth = border.staticWidth;
            conf.staticBorderHeight = border.staticHeight;

            conf.targetColors = colors.targetColors;
            conf.outputColor = colors.output;
            conf.borderColor = colors.border;
            conf.colorSensitivity = colorSensitivity;
            conf.rawOutput = rawOutput;
            conf.colorPassthrough = colorPassthrough;
            conf.gradientOutput = gradientOutput;
            conf.gradient = gradient;
            conf.sourceRectLayoutHash = MT_ComputeSourceRectLayoutHash(conf);
            break;
        }
    }
}

void InvalidateMirrorTextureCaches(const std::vector<std::string>& mirrorNames) {
    if (mirrorNames.empty()) return;

    std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex);
    for (const auto& mirrorName : mirrorNames) {
        auto it = g_mirrorInstances.find(mirrorName);
        if (it == g_mirrorInstances.end()) continue;

        MirrorInstance& inst = it->second;
        inst.captureReady.store(false, std::memory_order_release);
        inst.hasValidContent = false;
        inst.hasFrameContent = false;
        inst.hasFrameContentBack = false;
        inst.capturedAsRawOutput = false;
        inst.capturedAsRawOutputBack = false;
        inst.cachedRenderState.isValid = false;
        inst.cachedRenderStateBack.isValid = false;

        // Force full reallocation/rebuild on next activation so stale mirror/static-border visuals cannot survive.
        inst.fbo_w = 0;
        inst.fbo_h = 0;
        inst.final_w = 0;
        inst.final_h = 0;
        inst.final_w_back = 0;
        inst.final_h_back = 0;

        inst.forceUpdateFrames = 3;
        for (const auto& mirrorName : mirrorNames) {
            auto sameThreadFboIt = g_sameThreadMirrorFbos.find(mirrorName);
            if (sameThreadFboIt != g_sameThreadMirrorFbos.end()) {
                MT_DeleteMirrorFbos(sameThreadFboIt->second);
                g_sameThreadMirrorFbos.erase(sameThreadFboIt);
            }

            auto sameThreadCacheIt = g_sameThreadSourceRectGpuCaches.find(mirrorName);
            if (sameThreadCacheIt != g_sameThreadSourceRectGpuCaches.end()) {
                MT_DeleteSourceRectGpuCacheEntry(sameThreadCacheIt->second);
                g_sameThreadSourceRectGpuCaches.erase(sameThreadCacheIt);
            }

            auto instIt = g_mirrorInstances.find(mirrorName);
            if (instIt != g_mirrorInstances.end() && instIt->second.tempCaptureTexture != 0) {
                glDeleteTextures(1, &instIt->second.tempCaptureTexture);
                instIt->second.tempCaptureTexture = 0;
                instIt->second.tempCaptureTextureW = 0;
                instIt->second.tempCaptureTextureH = 0;
            }
        }
    }
}



