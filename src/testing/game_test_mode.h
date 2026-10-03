#pragma once

#include <windows.h>

// In-game test mode. scripts/game_tests/run_game_tests.ps1 launches a Fabric dev client with
// TOOLSCREEN_GAME_TEST_RESULTS set, injects Toolscreen the way the launcher does, and reads the
// JSON-lines results this module writes. Without that variable every entry point is a no-op.
namespace GameTest {

// Reads the test-mode environment variables. Safe to call from DllMain.
void InitializeFromEnvironment();
bool IsEnabled();

// Starts the test runner thread once the game-window hooks are installed.
void StartRunner(HWND gameWindow);

// Called at the start of every hooked frame on the render thread (wglSwapBuffers / vkQueuePresentKHR).
// Counts frames and runs work the tests queued for the render thread.
void OnRenderThreadFrame();

void Shutdown();

} // namespace GameTest
