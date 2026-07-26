#pragma once

#include <atomic>
#include <Windows.h>
#include <string>

extern WNDPROC g_originalWndProc;
extern std::atomic<HWND> g_subclassedHwnd;

struct InputHandlerResult {
    bool consumed;
    LRESULT result;
};

// Custom message: treat payload as WM_CHAR/WM_SYSCHAR without running HandleCharRebinding.
inline constexpr UINT WM_TOOLSCREEN_CHAR_NO_REBIND = WM_APP + 0x2A1;
// Custom messages: treat payload as WM_KEYDOWN/WM_KEYUP without running HandleKeyRebinding.
inline constexpr UINT WM_TOOLSCREEN_KEYDOWN_NO_REBIND = WM_APP + 0x2A2;
inline constexpr UINT WM_TOOLSCREEN_KEYUP_NO_REBIND = WM_APP + 0x2A3;
inline constexpr UINT WM_TOOLSCREEN_APPLY_FOCUS_REGAIN_SIZE = WM_APP + 0x2A4;
inline constexpr UINT WM_TOOLSCREEN_LOCAL_KEY_REPEAT = WM_APP + 0x2A5;

InputHandlerResult HandleMouseMoveViewportOffset(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM& lParam);

InputHandlerResult HandleShutdownCheck(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

InputHandlerResult HandleWindowValidation(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

InputHandlerResult HandleToolscreenQueryMessages(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

InputHandlerResult HandleNonFullscreenCheck(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

void HandleCharLogging(UINT uMsg, WPARAM wParam, LPARAM lParam);

InputHandlerResult HandleAltF4(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

InputHandlerResult HandleConfigLoadFailure(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

InputHandlerResult HandleSetCursor(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, const std::string& gameState);

InputHandlerResult HandleDestroy(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

InputHandlerResult HandleImGuiInput(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

InputHandlerResult HandleGuiToggle(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

InputHandlerResult HandleBorderlessToggle(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

InputHandlerResult HandleImageOverlaysToggle(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
InputHandlerResult HandleWindowOverlaysToggle(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
InputHandlerResult HandleNinjabrainOverlayToggle(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
InputHandlerResult HandleKeyRebindsToggle(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

InputHandlerResult HandleWindowOverlayKeyboard(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

InputHandlerResult HandleWindowOverlayMouse(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

// Block all input when GUI is open
InputHandlerResult HandleGuiInputBlocking(UINT uMsg);

InputHandlerResult HandleActivate(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

InputHandlerResult HandleHotkeys(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, const std::string& currentModeId,
                                 const std::string& gameState);

InputHandlerResult HandleMouseCoordinateTranslationPhase(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM& lParam);

InputHandlerResult HandleKeyRebinding(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

InputHandlerResult HandleCustomKeyNoRebind(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

InputHandlerResult HandleCustomCharNoRebind(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

InputHandlerResult HandleCharRebinding(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

void ResetLocalKeyRepeatState(HWND hWnd);

void ReleaseActiveLowLevelRebindKeys(HWND hWnd);

void ReleaseHeldPassthroughRebindSources(HWND hWnd);

bool IsKeyCurrentlyLowLevelSuppressed(DWORD vk);

#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
void ClearLowLevelSuppressedKeysForTest();
void ResetSyntheticRebindKeyEventsForTest();
size_t GetSyntheticRebindKeyEventCountForTest();
bool GetSyntheticRebindKeyEventForTest(size_t index, UINT& outScanCodeWithFlags, bool& outKeyDown);
size_t GetActiveSyntheticRebindOutputCountForTest();
void ResetExactKeyboardMessageStateForTest();
size_t GetUnreboundKeyDownCountForTest();
void ResetHotkeyRuntimeStateForTest();
void ResetLowLevelExactModifierStateForTest();
void SetLowLevelExactModifierDownForTest(DWORD vk, bool isDown);
void SetPhysicalModifierDownForTest(DWORD vk, bool isDown);
void ResetPhysicalModifierStateForTest();
void QueueSuppressedLowLevelKeyForTest(DWORD vk, UINT scanCodeWithFlags, bool isSystemKey);
void QueueLowLevelExactModifierKeydownForTest(DWORD vk);
void QueueLowLevelExactModifierKeyupForTest(DWORD vk);
DWORD ResolveTrackedKeyboardVkFromMessageForTest(UINT uMsg, WPARAM wParam, LPARAM lParam);
bool IsDeepSuppressionEligibleSourceVkForTest(DWORD vk);
bool ShouldDeepSuppressRebindForTest(DWORD fromKey, DWORD toKey);
#endif

LRESULT CALLBACK SubclassedWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);


