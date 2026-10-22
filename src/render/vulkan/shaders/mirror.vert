#version 450

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inUv;
layout(location = 2) in vec4 inColor;

layout(push_constant) uniform PushConstants {
    vec2 scale;
    vec2 translate;
} pushConstants;

layout(location = 0) out vec4 fragmentColor;
layout(location = 1) out vec2 fragmentUv;

void main() {
    fragmentColor = inColor;
    fragmentUv = inUv;
    gl_Position = vec4(inPosition * pushConstants.scale + pushConstants.translate, 0.0, 1.0);
}
