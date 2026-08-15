#pragma once

#ifndef GLEW_STATIC
#define GLEW_STATIC
#endif
#include <GL/glew.h>
#include <mutex>
#include <string>
#include <vector>
#include <windows.h>

namespace CursorTextures {
struct CursorData {
    HCURSOR hCursor = nullptr;    // Windows cursor handle
    int size = 0;
    std::wstring filePath;
    GLuint texture = 0;
    GLuint invertMaskTexture = 0;
    int hotspotX = 0;
    int hotspotY = 0;
    int bitmapWidth = 32;
    int bitmapHeight = 32;
    int contentLeft = 0;
    int contentTop = 0;
    int contentRight = 0;
    int contentBottom = 0;
    bool hasInvertedPixels = false;
    bool ownsHandle = true;
    UINT loadType = IMAGE_CURSOR;
    // Backend-neutral decoded pixels. OpenGL keeps using texture/invertMaskTexture;
    // Vulkan uploads these snapshots through its existing per-frame texture ring.
    std::vector<unsigned char> rgbaPixels;
    std::vector<unsigned char> invertRgbaPixels;
};

extern std::vector<CursorData> g_cursorList;
extern std::mutex g_cursorListMutex;

void LoadCursorTextures();

const CursorData* LoadOrFindCursor(const std::wstring& path, UINT loadType, int size);

const CursorData* FindCursor(const std::wstring& path, int size);

const CursorData* FindCursorByHandle(HCURSOR hCursor);

const CursorData* LoadOrFindCursorFromHandle(HCURSOR hCursor);
bool CopyCursorDataFromHandle(HCURSOR hCursor, CursorData& outData);

const CursorData* LoadOrFindSystemCursor(LPCWSTR systemCursorId);

void Cleanup();

const CursorData* GetSelectedCursor(const std::string& gameState, int size = 64);

bool GetCursorPathByName(const std::string& cursorName, std::wstring& outPath, UINT& outLoadType);

bool IsCursorFileValid(const std::string& cursorName);

void InitializeCursorDefinitions();
void RefreshCursorDefinitions();

std::vector<std::string> GetAvailableCursorNames();
}

void RenderFakeCursor(HWND hwnd, int windowWidth, int windowHeight);
void RenderFakeCursorToCurrentTarget(HWND hwnd,
                                     int fullWidth,
                                     int fullHeight,
                                     int targetX,
                                     int targetY,
                                     int targetWidth,
                                     int targetHeight,
                                     int sourceWidth,
                                     int sourceHeight);


