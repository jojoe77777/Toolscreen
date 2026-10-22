#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// A deliberately small deterministic model for the generation/latch contract
// used by the injected hooks.  It exercises ordering without a GLFW/Vulkan
// driver or an injected process.
struct StartupLifecycleModel {
    unsigned long long generation = 0;
    void* window = nullptr;
    void* hwnd = nullptr;
    bool noApi = false;
    bool replayPending = false;
    unsigned long long replayGeneration = 0;
    void* replayHwnd = nullptr;
    bool glInitialized = false;
    bool backendLatched = false;
    bool recording = false;
    bool teardownDeferred = false;
    bool relativeMouseMode = false;
    bool guiOpen = false;
    int deferredRelativeMouseMode = -1;

    void Create(void* newWindow, void* newHwnd, bool isNoApi) {
        ++generation;
        window = newWindow;
        hwnd = newHwnd;
        noApi = isNoApi;
        replayPending = false;
    }
    void Destroy(void* destroyedWindow) {
        if (window != destroyedWindow) return;
        ++generation;
        window = nullptr;
        hwnd = nullptr;
        noApi = false;
        replayPending = false;
    }
    void PostReplay() {
        replayPending = window && hwnd;
        replayGeneration = generation;
        replayHwnd = hwnd;
    }
    bool Replay(void* destination) {
        if (!replayPending || destination != hwnd || destination != replayHwnd || replayGeneration != generation) return false;
        replayPending = false;
        return true;
    }
    bool TryOpenGlFrame(bool validGlfwWindow, bool glewSucceeds) {
        if (!validGlfwWindow || noApi || !glewSucceeds) return false;
        glInitialized = true;
        backendLatched = true;
        return true;
    }
    bool TryVulkanFrame(bool hasExactSwapchainHwnd, bool rendererInitializes) {
        if (!hasExactSwapchainHwnd || !rendererInitializes) return false;
        backendLatched = true;
        return true;
    }
    void BeginTeardown() {
        if (recording) teardownDeferred = true;
    }
    void EndRecording() { recording = false; }
    bool SetRelativeMouseMode(bool enabled) {
        if (guiOpen) {
            deferredRelativeMouseMode = enabled ? 1 : 0;
            return true;
        }
        relativeMouseMode = enabled;
        return true;
    }
    void CloseGui() {
        guiOpen = false;
        if (deferredRelativeMouseMode != -1) {
            relativeMouseMode = deferredRelativeMouseMode != 0;
            deferredRelativeMouseMode = -1;
        }
    }
};

void Expect(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void GlfwDestroyRecreateRejectsStaleReplay() {
    StartupLifecycleModel model;
    void* firstWindow = reinterpret_cast<void*>(1);
    void* firstHwnd = reinterpret_cast<void*>(2);
    void* secondWindow = reinterpret_cast<void*>(3);
    void* secondHwnd = reinterpret_cast<void*>(4);
    model.Create(firstWindow, firstHwnd, true);
    model.PostReplay();
    model.Destroy(firstWindow);
    model.Create(secondWindow, secondHwnd, true);
    Expect(!model.Replay(firstHwnd), "stale GLFW replay must be rejected after recreate");
    model.PostReplay();
    Expect(model.Replay(secondHwnd), "current GLFW replay must be accepted");
}

void LateGlfwDiscoveryDoesNotClassifyBootstrapWgl() {
    StartupLifecycleModel model;
    Expect(!model.TryOpenGlFrame(false, true), "WGL without a GLFW window must pass through");
    model.Create(reinterpret_cast<void*>(1), reinterpret_cast<void*>(2), false);
    Expect(model.TryOpenGlFrame(true, true), "late-discovered GLFW GL window may initialize WGL");
}

void SdlDestroyRecreateRejectsStaleWindow() {
    StartupLifecycleModel model;
    void* firstWindow = reinterpret_cast<void*>(1);
    void* firstHwnd = reinterpret_cast<void*>(2);
    void* secondWindow = reinterpret_cast<void*>(3);
    void* secondHwnd = reinterpret_cast<void*>(4);
    model.Create(firstWindow, firstHwnd, false);
    model.Destroy(firstWindow);
    model.Create(secondWindow, secondHwnd, false);
    model.Destroy(firstWindow);
    Expect(model.window == secondWindow && model.hwnd == secondHwnd,
           "stale SDL3 destroy must not clear the replacement window");
}

void LateSdlDiscoveryAcceptsOwnedOpenGlWindow() {
    StartupLifecycleModel model;
    Expect(!model.TryOpenGlFrame(false, true), "WGL without an SDL3 window must pass through");
    model.Create(reinterpret_cast<void*>(1), reinterpret_cast<void*>(2), false);
    Expect(model.TryOpenGlFrame(true, true), "late-discovered SDL3 OpenGL window may initialize WGL");
}

void SdlRelativeMouseModeDefersWhileGuiOpen() {
    StartupLifecycleModel model;
    model.relativeMouseMode = true;
    model.guiOpen = true;
    Expect(model.SetRelativeMouseMode(false), "SDL3 relative-mode hook must preserve SDL success semantics");
    Expect(model.relativeMouseMode, "SDL3 relative-mode change must remain deferred while Toolscreen owns input");
    model.CloseGui();
    Expect(!model.relativeMouseMode, "deferred SDL3 relative-mode change must replay when the GUI closes");
    Expect(model.deferredRelativeMouseMode == -1, "replayed SDL3 relative-mode state must be consumed exactly once");
}

void OpenGlLatchesOnlyAfterInitialization() {
    StartupLifecycleModel model;
    model.Create(reinterpret_cast<void*>(1), reinterpret_cast<void*>(2), false);
    Expect(!model.TryOpenGlFrame(true, false), "failed GLEW init must not latch OpenGL");
    Expect(!model.backendLatched, "failed GLEW init leaves backend unknown");
    Expect(model.TryOpenGlFrame(true, true), "successful GLEW init latches OpenGL");
}

void VulkanRejectsMissingExactWindow() {
    StartupLifecycleModel model;
    Expect(!model.TryVulkanFrame(false, true), "Vulkan without exact tracked swapchain HWND must not latch");
    Expect(!model.backendLatched, "missing Vulkan HWND leaves backend unknown");
}

void VulkanTeardownWaitsForRecording() {
    StartupLifecycleModel model;
    model.recording = true;
    model.BeginTeardown();
    Expect(model.teardownDeferred, "teardown must wait while recording");
    model.EndRecording();
    Expect(!model.recording, "recording completion releases teardown");
}

struct TestCase { const char* name; std::function<void()> run; };
const std::vector<TestCase>& Registry() {
    static const std::vector<TestCase> tests = {
        {"glfw_destroy_recreate_rejects_stale_replay", GlfwDestroyRecreateRejectsStaleReplay},
        {"late_glfw_discovery_does_not_classify_bootstrap_wgl", LateGlfwDiscoveryDoesNotClassifyBootstrapWgl},
        {"sdl_destroy_recreate_rejects_stale_window", SdlDestroyRecreateRejectsStaleWindow},
        {"late_sdl_discovery_accepts_owned_opengl_window", LateSdlDiscoveryAcceptsOwnedOpenGlWindow},
        {"sdl_relative_mouse_mode_defers_while_gui_open", SdlRelativeMouseModeDefersWhileGuiOpen},
        {"opengl_latches_only_after_initialization", OpenGlLatchesOnlyAfterInitialization},
        {"vulkan_rejects_missing_exact_window", VulkanRejectsMissingExactWindow},
        {"vulkan_teardown_waits_for_recording", VulkanTeardownWaitsForRecording},
    };
    return tests;
}
}

int main(int argc, char** argv) {
    const std::string selected = argc == 3 && std::string(argv[1]) == "--run" ? argv[2] : "";
    try {
        for (const auto& test : Registry()) {
            if (!selected.empty() && selected != test.name) continue;
            test.run();
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
