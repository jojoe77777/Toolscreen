#pragma once

// Thread-safe input queue for Dear ImGui.
//
// Producer: Win32 window thread (SubclassedWndProc / input_hook.cpp)
// Consumer: render thread (render_thread.cpp) where ImGui context lives.
//
// Goal: avoid *any* ImGui calls from non-render threads.

#include <Windows.h>

#include <atomic>

// Published by the render thread once per frame (after building the UI).
// Safe to read from any thread.
extern std::atomic<bool> g_imguiWantCaptureMouse;
extern std::atomic<bool> g_imguiWantCaptureKeyboard;
extern std::atomic<bool> g_imguiAnyItemActive;

// Enqueue relevant Win32 messages for ImGui.
// Returns true if the message was recognized and queued (or intentionally dropped due to overflow).
bool ImGuiInputQueue_EnqueueWin32Message(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Enqueue a focus event (usable even when GUI is closed).
void ImGuiInputQueue_EnqueueFocus(bool focused);

// Clear any queued events (e.g. when toggling GUI).
void ImGuiInputQueue_Clear();

// Reset mouse capture bookkeeping (call when GUI closes).
void ImGuiInputQueue_ResetMouseCapture(HWND hWnd);

// Drain queued events into the current ImGui context.
// Must be called on the thread that owns the ImGui context, before ImGui::NewFrame().
void ImGuiInputQueue_DrainToImGui();

// Publish capture state atomics from the current ImGui context.
// Must be called on the thread that owns the ImGui context.
void ImGuiInputQueue_PublishCaptureState();
