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
layout(constant_id = 43) const int gradientStopCount = 0;
layout(constant_id = 44) const float gradientColor0R = 0.0;
layout(constant_id = 45) const float gradientColor0G = 0.0;
layout(constant_id = 46) const float gradientColor0B = 0.0;
layout(constant_id = 47) const float gradientColor0A = 1.0;
layout(constant_id = 48) const float gradientColor1R = 0.0;
layout(constant_id = 49) const float gradientColor1G = 0.0;
layout(constant_id = 50) const float gradientColor1B = 0.0;
layout(constant_id = 51) const float gradientColor1A = 1.0;
layout(constant_id = 52) const float gradientColor2R = 0.0;
layout(constant_id = 53) const float gradientColor2G = 0.0;
layout(constant_id = 54) const float gradientColor2B = 0.0;
layout(constant_id = 55) const float gradientColor2A = 1.0;
layout(constant_id = 56) const float gradientColor3R = 0.0;
layout(constant_id = 57) const float gradientColor3G = 0.0;
layout(constant_id = 58) const float gradientColor3B = 0.0;
layout(constant_id = 59) const float gradientColor3A = 1.0;
layout(constant_id = 60) const float gradientColor4R = 0.0;
layout(constant_id = 61) const float gradientColor4G = 0.0;
layout(constant_id = 62) const float gradientColor4B = 0.0;
layout(constant_id = 63) const float gradientColor4A = 1.0;
layout(constant_id = 64) const float gradientColor5R = 0.0;
layout(constant_id = 65) const float gradientColor5G = 0.0;
layout(constant_id = 66) const float gradientColor5B = 0.0;
layout(constant_id = 67) const float gradientColor5A = 1.0;
layout(constant_id = 68) const float gradientColor6R = 0.0;
layout(constant_id = 69) const float gradientColor6G = 0.0;
layout(constant_id = 70) const float gradientColor6B = 0.0;
layout(constant_id = 71) const float gradientColor6A = 1.0;
layout(constant_id = 72) const float gradientColor7R = 0.0;
layout(constant_id = 73) const float gradientColor7G = 0.0;
layout(constant_id = 74) const float gradientColor7B = 0.0;
layout(constant_id = 75) const float gradientColor7A = 1.0;
layout(constant_id = 76) const float gradientPosition0 = 0.0;
layout(constant_id = 77) const float gradientPosition1 = 1.0;
layout(constant_id = 78) const float gradientPosition2 = 1.0;
layout(constant_id = 79) const float gradientPosition3 = 1.0;
layout(constant_id = 80) const float gradientPosition4 = 1.0;
layout(constant_id = 81) const float gradientPosition5 = 1.0;
layout(constant_id = 82) const float gradientPosition6 = 1.0;
layout(constant_id = 83) const float gradientPosition7 = 1.0;
layout(constant_id = 84) const float gradientAngle = 0.0;
layout(constant_id = 85) const int gradientAnimationType = 0;
layout(constant_id = 86) const float gradientAnimationSpeed = 1.0;
layout(constant_id = 87) const int gradientColorFade = 0;
layout(constant_id = 88) const int staticBorderMode = 0;
layout(constant_id = 89) const int staticBorderShape = 0;

layout(push_constant) uniform FragmentPushConstants {
    layout(offset = 16) float gradientTime;
    vec4 staticBorderColor;
    float staticBorderThickness;
    float staticBorderRadius;
    vec2 staticBorderSize;
    vec2 staticBorderQuadSize;
} pushConstants;

float sdRoundedBox(vec2 p, vec2 b, float r) {
    float maxR = min(b.x, b.y);
    r = clamp(r, 0.0, maxR);
    vec2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

float sdEllipse(vec2 p, vec2 ab) {
    vec2 pn = p / ab;
    float len = length(pn);
    if (len < 0.0001) return -min(ab.x, ab.y);

    float d = len - 1.0;
    vec2 grad = pn / (ab * len);
    float gradLen = length(grad);
    return d / gradLen;
}

vec4 gradientColorAt(int index) {
    if (index == 0) return vec4(gradientColor0R, gradientColor0G, gradientColor0B, gradientColor0A);
    if (index == 1) return vec4(gradientColor1R, gradientColor1G, gradientColor1B, gradientColor1A);
    if (index == 2) return vec4(gradientColor2R, gradientColor2G, gradientColor2B, gradientColor2A);
    if (index == 3) return vec4(gradientColor3R, gradientColor3G, gradientColor3B, gradientColor3A);
    if (index == 4) return vec4(gradientColor4R, gradientColor4G, gradientColor4B, gradientColor4A);
    if (index == 5) return vec4(gradientColor5R, gradientColor5G, gradientColor5B, gradientColor5A);
    if (index == 6) return vec4(gradientColor6R, gradientColor6G, gradientColor6B, gradientColor6A);
    return vec4(gradientColor7R, gradientColor7G, gradientColor7B, gradientColor7A);
}

float gradientPositionAt(int index) {
    if (index == 0) return gradientPosition0;
    if (index == 1) return gradientPosition1;
    if (index == 2) return gradientPosition2;
    if (index == 3) return gradientPosition3;
    if (index == 4) return gradientPosition4;
    if (index == 5) return gradientPosition5;
    if (index == 6) return gradientPosition6;
    return gradientPosition7;
}

vec4 getGradientColorSeamless(float t) {
    t = fract(t);
    float lastPosition = gradientPositionAt(gradientStopCount - 1);
    float firstPosition = gradientPositionAt(0);
    float wrapSize = (1.0 - lastPosition) + firstPosition;
    if (t <= firstPosition && wrapSize > 0.001) {
        return mix(gradientColorAt(0), gradientColorAt(gradientStopCount - 1),
                   (firstPosition - t) / wrapSize);
    }
    if (t >= lastPosition && wrapSize > 0.001) {
        return mix(gradientColorAt(gradientStopCount - 1), gradientColorAt(0),
                   (t - lastPosition) / wrapSize);
    }
    vec4 color = gradientColorAt(0);
    for (int index = 0; index < gradientStopCount - 1; ++index) {
        float left = gradientPositionAt(index);
        float right = gradientPositionAt(index + 1);
        if (t >= left && t <= right) {
            color = mix(gradientColorAt(index), gradientColorAt(index + 1),
                        (t - left) / max(right - left, 0.0001));
            break;
        }
    }
    return color;
}

vec4 getGradientColor(float t, float timeOffset) {
    float adjusted = gradientColorFade != 0 ? fract(t + timeOffset * 0.1) : t;
    adjusted = clamp(adjusted, 0.0, 1.0);
    vec4 color = gradientColorAt(0);
    for (int index = 0; index < gradientStopCount - 1; ++index) {
        float left = gradientPositionAt(index);
        float right = gradientPositionAt(index + 1);
        if (adjusted >= left && adjusted <= right) {
            color = mix(gradientColorAt(index), gradientColorAt(index + 1),
                        (adjusted - left) / max(right - left, 0.0001));
            break;
        }
    }
    if (adjusted >= gradientPositionAt(gradientStopCount - 1)) {
        color = gradientColorAt(gradientStopCount - 1);
    }
    return color;
}

vec4 getFadeColor(float timeOffset) {
    float cyclePosition = fract(timeOffset * 0.1);
    vec4 color = gradientColorAt(0);
    for (int index = 0; index < gradientStopCount - 1; ++index) {
        float left = gradientPositionAt(index);
        float right = gradientPositionAt(index + 1);
        if (cyclePosition >= left && cyclePosition <= right) {
            color = mix(gradientColorAt(index), gradientColorAt(index + 1),
                        (cyclePosition - left) / max(right - left, 0.0001));
            break;
        }
    }
    float firstPosition = gradientPositionAt(0);
    float lastPosition = gradientPositionAt(gradientStopCount - 1);
    float wrapRange = 1.0 - lastPosition + firstPosition;
    if (cyclePosition > lastPosition) {
        color = mix(gradientColorAt(gradientStopCount - 1), gradientColorAt(0),
                    (cyclePosition - lastPosition) / max(wrapRange, 0.0001));
    } else if (cyclePosition < firstPosition) {
        color = mix(gradientColorAt(0), gradientColorAt(gradientStopCount - 1),
                    (firstPosition - cyclePosition) / max(wrapRange, 0.0001));
    }
    return color;
}

vec4 sampleGradientColor() {
    vec2 cropSize = max(vec2(cropMaxU - cropMinU, cropMaxV - cropMinV), vec2(0.0001));
    vec2 gradientUv = (fragmentUv - vec2(cropMinU, cropMinV)) / cropSize;
    vec2 uv = gradientUv - vec2(0.5);
    float timeOffset = pushConstants.gradientTime * gradientAnimationSpeed;
    if (gradientAnimationType == 0) {
        vec2 direction = vec2(cos(gradientAngle), sin(gradientAngle));
        return getGradientColor(clamp(dot(uv, direction) + 0.5, 0.0, 1.0), timeOffset);
    }
    if (gradientAnimationType == 1) {
        float effectiveAngle = gradientAngle + timeOffset;
        vec2 direction = vec2(cos(effectiveAngle), sin(effectiveAngle));
        return getGradientColor(clamp(dot(uv, direction) + 0.5, 0.0, 1.0), timeOffset);
    }
    if (gradientAnimationType == 2) {
        vec2 direction = vec2(cos(gradientAngle), sin(gradientAngle));
        return getGradientColorSeamless(dot(uv, direction) + 0.5 + timeOffset * 0.2);
    }
    if (gradientAnimationType == 3) {
        vec2 direction = vec2(cos(gradientAngle), sin(gradientAngle));
        vec2 perpendicular = vec2(-sin(gradientAngle), cos(gradientAngle));
        float wave = sin(dot(uv, perpendicular) * 8.0 + timeOffset * 2.0) * 0.08;
        return getGradientColor(clamp(dot(uv, direction) + 0.5 + wave, 0.0, 1.0), timeOffset);
    }
    if (gradientAnimationType == 4) {
        float t = length(uv) * 2.0 + atan(uv.y, uv.x) / 6.28318 - timeOffset * 0.3;
        return getGradientColorSeamless(t);
    }
    if (gradientAnimationType == 5) return getFadeColor(timeOffset);
    return getGradientColor(0.0, timeOffset);
}

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
    if (staticBorderMode != 0) {
        vec2 pixelPos = fragmentUv * pushConstants.staticBorderQuadSize;
        vec2 centeredPixelPos =
            pixelPos - pushConstants.staticBorderQuadSize * 0.5;
        vec2 halfSize =
            max(pushConstants.staticBorderSize * 0.5, vec2(1.0));
        float dist = staticBorderShape == 0
            ? sdRoundedBox(
                  centeredPixelPos, halfSize,
                  pushConstants.staticBorderRadius)
            : sdEllipse(centeredPixelPos, halfSize);
        const float epsilon = 0.5;
        if (dist >= -epsilon &&
            dist <= pushConstants.staticBorderThickness + epsilon) {
            outputColor = pushConstants.staticBorderColor;
            return;
        }
        discard;
    }

    if (matchesAt(fragmentUv)) {
        if (colorPassthrough != 0) {
            vec4 sampled = texture(sourceTexture, fragmentUv);
            outputColor = vec4(sampled.rgb, sampled.a * outputA);
        } else if (gradientStopCount >= 2) {
            vec4 gradient = sampleGradientColor();
            outputColor = vec4(gradient.rgb, gradient.a * outputA);
        } else {
            // The vertex color carries either the configured solid output
            // color or the evaluated mirror gradient. The specialization
            // color remains as a multiplier so solid and gradient mirrors
            // share one cached pipeline shape.
            outputColor = vec4(outputR, outputG, outputB, outputA) * fragmentColor;
        }
        return;
    }

    int width = max(dynamicBorderWidth, 0);
    if (width > 0) {
        for (int y = -width; y <= width; ++y) {
            for (int x = -width; x <= width; ++x) {
                if (matchesAt(fragmentUv + vec2(float(x) * sourceTexelX, float(y) * sourceTexelY))) {
                    outputColor = vec4(borderR, borderG, borderB, borderA);
                    return;
                }
            }
        }
    }
    discard;
}
