#include "render_backend.h"

#include "common/utils.h"
#include "render/render.h"
#include "render/vulkan/vulkan_renderer.h"

namespace {
std::atomic<RenderBackend> g_renderBackend{ RenderBackend::Unknown };
}

RenderBackend GetRenderBackend() {
    return g_renderBackend.load(std::memory_order_acquire);
}

bool TryLatchRenderBackend(RenderBackend backend) {
    if (backend == RenderBackend::Unknown) { return false; }
    RenderBackend expected = RenderBackend::Unknown;
    if (g_renderBackend.compare_exchange_strong(expected, backend, std::memory_order_acq_rel)) {
        LogCategory("init", std::string("[RENDER] Latched backend after first real frame: ") + GetRenderBackendName(backend));
        return true;
    }
    return expected == backend;
}

bool IsRenderBackendReady() {
    switch (GetRenderBackend()) {
    case RenderBackend::OpenGL:
        return g_glInitialized.load(std::memory_order_acquire);
    case RenderBackend::Vulkan:
        return VulkanRenderer::IsReady();
    default:
        // GUI integration tests initialize the established GL renderer without
        // driving a platform SwapBuffers call. In production g_glInitialized
        // can only become true from inside the already-latched OpenGL frame.
        return g_glInitialized.load(std::memory_order_acquire);
    }
}

const char* GetRenderBackendName(RenderBackend backend) {
    switch (backend) {
    case RenderBackend::OpenGL: return "OpenGL";
    case RenderBackend::Vulkan: return "Vulkan";
    default: return "Unknown";
    }
}
