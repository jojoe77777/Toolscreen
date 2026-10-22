// A stand-in for a third-party overlay (Discord, Steam, recorders) for the in-game tests. It detours
// opengl32!wglSwapBuffers with its own MinHook instance, so Toolscreen's hook-chain code sees the export
// prologue jump into a foreign module and chains behind it, as it would with a real overlay.

#include <windows.h>

#include <atomic>

#include "MinHook.h"

namespace {

using WglSwapBuffersFn = BOOL(WINAPI*)(HDC);

void* g_target = nullptr;
WglSwapBuffersFn g_original = nullptr;
std::atomic<long> g_calls{ 0 };

} // namespace

extern "C" {

// The detour Toolscreen chains behind. Its address is what Toolscreen reports as the third-party hook target.
__declspec(dllexport) __declspec(noinline) BOOL WINAPI GameTestOverlaySwapBuffers(HDC dc) {
    g_calls.fetch_add(1, std::memory_order_relaxed);
    return g_original ? g_original(dc) : FALSE;
}

__declspec(dllexport) BOOL GameTestOverlayInstall() {
    HMODULE opengl32 = GetModuleHandleW(L"opengl32.dll");
    if (!opengl32) return FALSE;
    g_target = reinterpret_cast<void*>(GetProcAddress(opengl32, "wglSwapBuffers"));
    if (!g_target) return FALSE;
    if (MH_Initialize() != MH_OK) return FALSE;
    if (MH_CreateHook(g_target, reinterpret_cast<void*>(&GameTestOverlaySwapBuffers), reinterpret_cast<void**>(&g_original)) != MH_OK) {
        MH_Uninitialize();
        return FALSE;
    }
    if (MH_EnableHook(g_target) != MH_OK) {
        MH_RemoveHook(g_target);
        MH_Uninitialize();
        return FALSE;
    }
    return TRUE;
}

// Restores the export prologue. The caller must let in-flight frames finish before unloading the DLL.
__declspec(dllexport) BOOL GameTestOverlayRemove() {
    if (!g_target) return FALSE;
    const bool ok = MH_DisableHook(g_target) == MH_OK && MH_RemoveHook(g_target) == MH_OK;
    MH_Uninitialize();
    g_target = nullptr;
    return ok ? TRUE : FALSE;
}

__declspec(dllexport) LONG GameTestOverlayCallCount() { return g_calls.load(std::memory_order_relaxed); }

} // extern "C"
