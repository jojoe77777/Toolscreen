#version 450

layout(set = 0, binding = 0) uniform sampler2D sourceTexture;

layout(location = 0) in vec4 fragmentColor;
layout(location = 1) in vec2 fragmentUv;
layout(location = 0) out vec4 outputColor;

layout(constant_id = 0) const int targetCount = 0;
layout(constant_id = 1) const float sensitivity = 0.001;
layout(constant_id = 2) const int colorPassthrough = 0;
layout(constant_id = 3) const int dynamicBorderWidth = 0;
layout(constant_id = 4) const float sourceTexelX = 0.0;
layout(constant_id = 5) const float sourceTexelY = 0.0;
layout(constant_id = 6) const float cropMinU = 0.0;
layout(constant_id = 7) const float cropMinV = 0.0;
layout(constant_id = 8) const float cropMaxU = 1.0;
layout(constant_id = 9) const float cropMaxV = 1.0;
layout(constant_id = 10) const float outputR = 1.0;
layout(constant_id = 11) const float outputG = 1.0;
layout(constant_id = 12) const float outputB = 1.0;
layout(constant_id = 13) const float outputA = 1.0;
layout(constant_id = 14) const float borderR = 0.0;
layout(constant_id = 15) const float borderG = 0.0;
layout(constant_id = 16) const float borderB = 0.0;
layout(constant_id = 17) const float borderA = 1.0;
layout(constant_id = 18) const float target0R = 0.0;
layout(constant_id = 19) const float target0G = 0.0;
layout(constant_id = 20) const float target0B = 0.0;
layout(constant_id = 21) const float target1R = 0.0;
layout(constant_id = 22) const float target1G = 0.0;
layout(constant_id = 23) const float target1B = 0.0;
layout(constant_id = 24) const float target2R = 0.0;
layout(constant_id = 25) const float target2G = 0.0;
layout(constant_id = 26) const float target2B = 0.0;
layout(constant_id = 27) const float target3R = 0.0;
layout(constant_id = 28) const float target3G = 0.0;
layout(constant_id = 29) const float target3B = 0.0;
layout(constant_id = 30) const float target4R = 0.0;
layout(constant_id = 31) const float target4G = 0.0;
layout(constant_id = 32) const float target4B = 0.0;
layout(constant_id = 33) const float target5R = 0.0;
layout(constant_id = 34) const float target5G = 0.0;
layout(constant_id = 35) const float target5B = 0.0;
layout(constant_id = 36) const float target6R = 0.0;
layout(constant_id = 37) const float target6G = 0.0;
layout(constant_id = 38) const float target6B = 0.0;
layout(constant_id = 39) const float target7R = 0.0;
layout(constant_id = 40) const float target7G = 0.0;
layout(constant_id = 41) const float target7B = 0.0;
layout(constant_id = 42) const int gammaMode = 0;

vec3 srgbToLinear(vec3 color) {
    bvec3 cutoff = lessThanEqual(color, vec3(0.04045));
    vec3 low = color / 12.92;
    vec3 high = pow((color + 0.055) / 1.055, vec3(2.4));
    return mix(high, low, vec3(cutoff));
}

vec3 targetAt(int index) {
    if (index == 0) return vec3(target0R, target0G, target0B);
    if (index == 1) return vec3(target1R, target1G, target1B);
    if (index == 2) return vec3(target2R, target2G, target2B);
    if (index == 3) return vec3(target3R, target3G, target3B);
    if (index == 4) return vec3(target4R, target4G, target4B);
    if (index == 5) return vec3(target5R, target5G, target5B);
    if (index == 6) return vec3(target6R, target6G, target6B);
    return vec3(target7R, target7G, target7B);
}

bool insideCrop(vec2 uv) {
    float minV = min(cropMinV, cropMaxV);
    float maxV = max(cropMinV, cropMaxV);
    return uv.x >= cropMinU && uv.x <= cropMaxU && uv.y >= minV && uv.y <= maxV;
}

bool matchesAt(vec2 uv) {
    if (!insideCrop(uv)) return false;
    vec3 sampled = texture(sourceTexture, uv).rgb;
    vec3 sampledLinear = srgbToLinear(sampled);
    for (int index = 0; index < targetCount; ++index) {
        vec3 target = targetAt(index);
        vec3 targetLinear = srgbToLinear(target);
        float difference;
        if (gammaMode == 2) {
            difference = distance(sampled, targetLinear);
        } else if (gammaMode == 1) {
            difference = distance(sampledLinear, targetLinear);
        } else {
            difference = min(distance(sampled, target),
                             distance(sampledLinear, targetLinear));
        }
        if (difference < sensitivity) return true;
    }
    return false;
}

void main() {
    if (matchesAt(fragmentUv)) {
        if (colorPassthrough != 0) {
            vec4 sampled = texture(sourceTexture, fragmentUv);
            outputColor = vec4(sampled.rgb, sampled.a * outputA);
        } else {
            // The vertex color carries either the configured solid output
            // color or the evaluated mirror gradient. The specialization
            // color remains as a multiplier so solid and gradient mirrors
            // share one cached pipeline shape.
            outputColor = vec4(outputR, outputG, outputB, outputA) * fragmentColor;
        }
        return;
    }

    const int maxBorderWidth = 16;
    int width = min(dynamicBorderWidth, maxBorderWidth);
    if (width > 0) {
        for (int y = -maxBorderWidth; y <= maxBorderWidth; ++y) {
            for (int x = -maxBorderWidth; x <= maxBorderWidth; ++x) {
                if (abs(x) > width || abs(y) > width) continue;
                if (matchesAt(fragmentUv + vec2(float(x) * sourceTexelX, float(y) * sourceTexelY))) {
                    outputColor = vec4(borderR, borderG, borderB, borderA);
                    return;
                }
            }
        }
    }
    discard;
}
