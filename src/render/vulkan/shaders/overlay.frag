#version 450

layout(set = 0, binding = 0) uniform sampler2D sourceTexture;

layout(location = 0) in vec4 fragmentColor;
layout(location = 1) in vec2 fragmentUv;
layout(location = 0) out vec4 outputColor;

layout(constant_id = 0) const int keyCount = 0;
layout(constant_id = 1) const float key0R = 0.0;
layout(constant_id = 2) const float key0G = 0.0;
layout(constant_id = 3) const float key0B = 0.0;
layout(constant_id = 4) const float key1R = 0.0;
layout(constant_id = 5) const float key1G = 0.0;
layout(constant_id = 6) const float key1B = 0.0;
layout(constant_id = 7) const float key2R = 0.0;
layout(constant_id = 8) const float key2G = 0.0;
layout(constant_id = 9) const float key2B = 0.0;
layout(constant_id = 10) const float key3R = 0.0;
layout(constant_id = 11) const float key3G = 0.0;
layout(constant_id = 12) const float key3B = 0.0;
layout(constant_id = 13) const float key4R = 0.0;
layout(constant_id = 14) const float key4G = 0.0;
layout(constant_id = 15) const float key4B = 0.0;
layout(constant_id = 16) const float key5R = 0.0;
layout(constant_id = 17) const float key5G = 0.0;
layout(constant_id = 18) const float key5B = 0.0;
layout(constant_id = 19) const float key6R = 0.0;
layout(constant_id = 20) const float key6G = 0.0;
layout(constant_id = 21) const float key6B = 0.0;
layout(constant_id = 22) const float key7R = 0.0;
layout(constant_id = 23) const float key7G = 0.0;
layout(constant_id = 24) const float key7B = 0.0;
layout(constant_id = 25) const float sensitivity0 = 0.0;
layout(constant_id = 26) const float sensitivity1 = 0.0;
layout(constant_id = 27) const float sensitivity2 = 0.0;
layout(constant_id = 28) const float sensitivity3 = 0.0;
layout(constant_id = 29) const float sensitivity4 = 0.0;
layout(constant_id = 30) const float sensitivity5 = 0.0;
layout(constant_id = 31) const float sensitivity6 = 0.0;
layout(constant_id = 32) const float sensitivity7 = 0.0;

vec3 keyAt(int index) {
    if (index == 0) return vec3(key0R, key0G, key0B);
    if (index == 1) return vec3(key1R, key1G, key1B);
    if (index == 2) return vec3(key2R, key2G, key2B);
    if (index == 3) return vec3(key3R, key3G, key3B);
    if (index == 4) return vec3(key4R, key4G, key4B);
    if (index == 5) return vec3(key5R, key5G, key5B);
    if (index == 6) return vec3(key6R, key6G, key6B);
    return vec3(key7R, key7G, key7B);
}

float sensitivityAt(int index) {
    if (index == 0) return sensitivity0;
    if (index == 1) return sensitivity1;
    if (index == 2) return sensitivity2;
    if (index == 3) return sensitivity3;
    if (index == 4) return sensitivity4;
    if (index == 5) return sensitivity5;
    if (index == 6) return sensitivity6;
    return sensitivity7;
}

void main() {
    vec4 sampled = texture(sourceTexture, fragmentUv);
    for (int index = 0; index < keyCount; ++index) {
        if (distance(sampled.rgb, keyAt(index)) <= sensitivityAt(index)) {
            discard;
        }
    }
    outputColor = sampled * fragmentColor;
}
