#pragma once

#include <atomic>
#include <cstdint>

enum class RenderBackend : uint8_t {
    Unknown = 0,
    OpenGL,
    Vulkan,
};

RenderBackend GetRenderBackend();
bool TryLatchRenderBackend(RenderBackend backend);
bool IsRenderBackendReady();
const char* GetRenderBackendName(RenderBackend backend);

