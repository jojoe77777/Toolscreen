#pragma once

#include <GL/glew.h>
#include <mutex>
#include <string>
#include <vector>
#include <windows.h>

// Unified Cursor Texture System
namespace CursorTextures {
struct CursorData {
    HCURSOR hCursor = nullptr;    // Windows cursor handle
    int size = 0;                 // Requested cursor size (pixels)
    std::wstring filePath;        // Source file path
    GLuint texture = 0;           // Main cursor texture
    GLuint invertMaskTexture = 0; // Mask for inverted pixels (XOR blending)
    int hotspotX = 0;             // Hotspot offset in pixels
    int hotspotY = 0;
    int bitmapWidth = 32;           // Actual bitmap width after loading
    int bitmapHeight = 32;          // Actual bitmap height after loading
    bool hasInvertedPixels = false; // Whether cursor has inverted regions
    UINT loadType = IMAGE_CURSOR;   // IMAGE_CURSOR or IMAGE_ICON
};

// Global list of all loaded cursors
extern std::vector<CursorData> g_cursorList;
extern std::mutex g_cursorListMutex;

// Load and create HCURSOR handles + textures for predefined cursors at default size (64px)
// Additional sizes are loaded on-demand when requested
// Should be called once during program initialization
void LoadCursorTextures();

// Load or find a cursor at a specific size (on-demand loading)
// If cursor at size doesn't exist, loads it immediately
// Returns pointer to CursorData or nullptr if load failed
const CursorData* LoadOrFindCursor(const std::wstring& path, UINT loadType, int size);

// Find cursor in g_cursorList by path and size
// Returns pointer to CursorData or nullptr if not found
const CursorData* FindCursor(const std::wstring& path, int size);

// Find cursor in g_cursorList by HCURSOR handle
// Returns pointer to CursorData or nullptr if not found
const CursorData* FindCursorByHandle(HCURSOR hCursor);

// Load or find a cursor from an existing HCURSOR handle (for system cursors)
// If cursor handle is already in the list, returns it
// Otherwise creates a new texture from the handle and adds it to the list
// Returns pointer to CursorData or nullptr if load failed
const CursorData* LoadOrFindCursorFromHandle(HCURSOR hCursor);

// Cleanup all cursor handles and textures
void Cleanup();

// Get the selected cursor for the current game state
// Returns CursorData for the configured cursor, or first available as fallback
// gameState should be "title", "wall", or "ingame"
const CursorData* GetSelectedCursor(const std::string& gameState, int size = 64);

// Helper to get cursor file path and type by cursor name
// Returns true if found, false if unknown name (uses first available as fallback)
bool GetCursorPathByName(const std::string& cursorName, std::wstring& outPath, UINT& outLoadType);

// Check if a cursor file exists at the given path
// Returns true if file exists, false otherwise
bool IsCursorFileValid(const std::string& cursorName);

// Initialize cursor definitions (called automatically during LoadCursorTextures)
// Scans the cursors folder for .cur and .ico files and adds them to the cursor list
void InitializeCursorDefinitions();

// Get the list of available cursor definitions
// Returns vector of cursor names/paths that can be displayed in UI
std::vector<std::string> GetAvailableCursorNames();
} // namespace CursorTextures

// Render fake cursor overlay on the game window
void RenderFakeCursor(HWND hwnd, int windowWidth, int windowHeight);
