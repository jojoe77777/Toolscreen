#include "fake_cursor.h"
#include "gui/gui.h"
#include "common/utils.h"
#include "common/gl_overlay.h"
#include <filesystem>
#include <shared_mutex>
#include <string>
#include <vector>
#include <windows.h>

void Log(const std::string& msg);

static std::wstring ResolveCwdPath(const std::wstring& relPath) {
    std::filesystem::path cwdPath = std::filesystem::current_path();
    std::filesystem::path fullPath = cwdPath / relPath;
    return fullPath.wstring();
}

namespace CursorTextures {

static void DestroyCursorOrIcon(HCURSOR handle, UINT loadType) {
    if (!handle) return;
    if (loadType == IMAGE_ICON) {
        DestroyIcon(reinterpret_cast<HICON>(handle));
    } else {
        DestroyCursor(handle);
    }
}

struct CursorDef {
    std::string name;
    std::wstring path;
    UINT loadType;
};

static const std::vector<CursorDef> SYSTEM_CURSORS = { { "Arrow", L"C:/Windows/Cursors/aero_arrow.cur", IMAGE_CURSOR },
                                                       { "Cross (Inverted, small)", L"C:/Windows/Cursors/cross_i.cur", IMAGE_CURSOR },
                                                       { "Cross (Inverted, medium)", L"C:/Windows/Cursors/cross_im.cur", IMAGE_CURSOR },
                                                       { "Cross (Inverted, large)", L"C:/Windows/Cursors/cross_il.cur", IMAGE_CURSOR },
                                                       { "Cross (Inverted, no outline)", L"C:/Windows/Cursors/cross_l.cur", IMAGE_CURSOR },
                                                       { "Cross (Small)", L"C:/Windows/Cursors/cross_r.cur", IMAGE_CURSOR },
                                                       { "Cross (Medium)", L"C:/Windows/Cursors/cross_rm.cur", IMAGE_CURSOR },
                                                       { "Cross (Large)", L"C:/Windows/Cursors/cross_rl.cur", IMAGE_CURSOR } };

static std::vector<CursorDef> AVAILABLE_CURSORS;
static bool g_cursorDefsInitialized = false;
static std::mutex g_cursorDefsMutex;

static void ScanCursorDefinitionsLocked() {
    LogCategory(Log_CursorTextures, "[CursorTextures] ScanCursorDefinitionsLocked starting...");

    AVAILABLE_CURSORS = SYSTEM_CURSORS;
    LogCategory(Log_CursorTextures, "[CursorTextures] Loaded {} system cursor definitions", SYSTEM_CURSORS.size());

    int validSystemCursors = 0;
    for (const auto& cursor : SYSTEM_CURSORS) {
        if (std::filesystem::exists(cursor.path)) {
            validSystemCursors++;
        } else {
            LogCategory(Log_CursorTextures, "[CursorTextures] WARNING: System cursor not found: {}", WideToUtf8(cursor.path));
        }
    }
    LogCategory(Log_CursorTextures, "[CursorTextures] Verified {}/{} system cursors exist on disk",
        validSystemCursors, SYSTEM_CURSORS.size());

    try {
        std::wstring toolscreenPath = GetToolscreenPath();
        if (toolscreenPath.empty()) {
            LogCategory(Log_CursorTextures, "[CursorTextures] ERROR: Failed to get toolscreen path - custom cursors will not be available");
            return;
        }

        std::filesystem::path cursorsPath = std::filesystem::path(toolscreenPath) / "cursors";
        LogCategory(Log_CursorTextures, "[CursorTextures] Scanning for custom cursors at: {}", cursorsPath.string());

        if (!std::filesystem::exists(cursorsPath)) {
            LogCategory(Log_CursorTextures, "[CursorTextures] Custom cursors folder does not exist: {}", cursorsPath.string());
            LogCategory(Log_CursorTextures, "[CursorTextures] To add custom cursors, create this folder and add .cur or .ico files");
        } else if (!std::filesystem::is_directory(cursorsPath)) {
            LogCategory(Log_CursorTextures, "[CursorTextures] ERROR: Cursors path exists but is not a directory: {}", cursorsPath.string());
        } else {
            int customCursorsFound = 0;
            int filesSkipped = 0;
            for (const auto& entry : std::filesystem::directory_iterator(cursorsPath)) {
                if (entry.is_regular_file()) {
                    auto ext = entry.path().extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

                    if (ext == ".cur" || ext == ".ico") {
                        std::string filename = entry.path().filename().stem().string();
                        std::wstring filepath = entry.path().wstring();

                        UINT loadType = (ext == ".ico") ? IMAGE_ICON : IMAGE_CURSOR;

                        AVAILABLE_CURSORS.push_back({ filename, filepath, loadType });
                        LogCategory(Log_CursorTextures, "[CursorTextures] Found custom cursor: {} ({})", filename, ext);
                        customCursorsFound++;
                    } else {
                        filesSkipped++;
                    }
                }
            }
            LogCategory(Log_CursorTextures, "[CursorTextures] Found {} custom cursor(s), skipped {} non-cursor file(s)",
                customCursorsFound, filesSkipped);
        }
    } catch (const std::filesystem::filesystem_error& e) {
        LogCategory(Log_CursorTextures, "[CursorTextures] ERROR: Filesystem error scanning cursors folder: {}", e.what());
        LogCategory(Log_CursorTextures, "[CursorTextures] Error code: {} - {}", e.code().value(), e.code().message());
    } catch (const std::exception& e) {
        LogCategory(Log_CursorTextures, "[CursorTextures] ERROR: Exception scanning cursors folder: {}", e.what());
    } catch (...) { LogCategory(Log_CursorTextures, "[CursorTextures] ERROR: Unknown exception scanning cursors folder"); }

    LogCategory(Log_CursorTextures, "[CursorTextures] ScanCursorDefinitionsLocked complete. Total cursors available: {}",
        AVAILABLE_CURSORS.size());
}

void InitializeCursorDefinitions() {
    std::lock_guard<std::mutex> lock(g_cursorDefsMutex);
    if (g_cursorDefsInitialized) return;

    LogCategory(Log_CursorTextures, "[CursorTextures] InitializeCursorDefinitions starting...");
    ScanCursorDefinitionsLocked();
    g_cursorDefsInitialized = true;
}

void RefreshCursorDefinitions() {
    std::lock_guard<std::mutex> lock(g_cursorDefsMutex);
    LogCategory(Log_CursorTextures, "[CursorTextures] RefreshCursorDefinitions starting...");
    ScanCursorDefinitionsLocked();
    g_cursorDefsInitialized = true;
}

// Global cursor list and mutex
std::vector<CursorData> g_cursorList;
std::mutex g_cursorListMutex;

static void ComputeCursorContentBounds(CursorData& outData, const std::vector<unsigned char>& pixels,
                                       const std::vector<unsigned char>* invertPixels, int width, int height) {
    int minX = width;
    int minY = height;
    int maxX = -1;
    int maxY = -1;

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int idx = (y * width + x) * 4;
            const unsigned char alpha = pixels[idx + 3];
            const unsigned char invertAlpha = invertPixels ? (*invertPixels)[idx + 3] : 0;
            if (alpha == 0 && invertAlpha == 0) {
                continue;
            }

            minX = (std::min)(minX, x);
            minY = (std::min)(minY, y);
            maxX = (std::max)(maxX, x);
            maxY = (std::max)(maxY, y);
        }
    }

    if (maxX < minX || maxY < minY) {
        outData.contentLeft = 0;
        outData.contentTop = 0;
        outData.contentRight = width;
        outData.contentBottom = height;
        return;
    }

    outData.contentLeft = minX;
    outData.contentTop = minY;
    outData.contentRight = maxX + 1;
    outData.contentBottom = maxY + 1;
}

static std::wstring BuildSystemCursorCacheKey(LPCWSTR systemCursorId) {
    if (systemCursorId == IDC_ARROW) {
        return L"<system:idc_arrow>";
    }

    if (IS_INTRESOURCE(systemCursorId)) {
        std::wstring key = L"<system:resource:";
        key += std::to_wstring(reinterpret_cast<ULONG_PTR>(systemCursorId));
        key += L">";
        return key;
    }

    std::wstring key = L"<system:name:";
    key += systemCursorId;
    key += L">";
    return key;
}

static const std::vector<int> STANDARD_SIZES = {
    16, 20, 24, 28, 32, 40, 48, 56, 64, 72, 80, 96, 112, 128, 144, 160, 192, 224, 256, 288, 320
};

static bool LoadSingleCursor(const std::wstring& path, UINT loadType, int size, CursorData& outData) {
    if (path.empty()) {
        LogCategory(Log_CursorTextures, "[CursorTextures] ERROR: LoadSingleCursor called with empty path");
        return false;
    }
    if (size <= 0 || size > 512) {
        LogCategory(Log_CursorTextures, "[CursorTextures] ERROR: LoadSingleCursor called with invalid size: {}", size);
        return false;
    }

    std::wstring resolvedPath = path;
    try {
        if (!std::filesystem::path(path).is_absolute()) { resolvedPath = ResolveCwdPath(path); }
    } catch (const std::exception& e) {
        LogCategory(Log_CursorTextures, "[CursorTextures] ERROR: Failed to resolve path: {}", e.what());
        return false;
    }

    std::string pathStr = WideToUtf8(resolvedPath);
    LogCategory(Log_CursorTextures, "[CursorTextures] Loading cursor: {} at size {} (type: {})",
        pathStr, size, loadType == IMAGE_ICON ? "ICON" : "CURSOR");

    if (!std::filesystem::exists(resolvedPath)) {
        LogCategory(Log_CursorTextures, "[CursorTextures] ERROR: Cursor file does not exist: {}", pathStr);
        return false;
    }

    outData.filePath = path;
    outData.size = size;
    outData.loadType = loadType;
    outData.ownsHandle = true;

    HCURSOR hCursor = (HCURSOR)LoadImageW(NULL, resolvedPath.c_str(), loadType, size, size, LR_LOADFROMFILE | LR_DEFAULTSIZE);
    if (!hCursor) {
        DWORD err = GetLastError();
        std::string errMsg;
        switch (err) {
        case ERROR_FILE_NOT_FOUND:
            errMsg = "File not found";
            break;
        case ERROR_PATH_NOT_FOUND:
            errMsg = "Path not found";
            break;
        case ERROR_ACCESS_DENIED:
            errMsg = "Access denied";
            break;
        case ERROR_INVALID_PARAMETER:
            errMsg = "Invalid parameter";
            break;
        case ERROR_NOT_ENOUGH_MEMORY:
            errMsg = "Not enough memory";
            break;
        case ERROR_RESOURCE_TYPE_NOT_FOUND:
            errMsg = "Resource type not found (invalid cursor/icon format?)";
            break;
        default:
            errMsg = "Unknown error";
            break;
        }
        LogCategory(Log_CursorTextures,
                    "[CursorTextures] ERROR: LoadImageW failed for '{}' - Error {}: {}", pathStr, err, errMsg);
        return false;
    }

    // (Without this, Windows may pick the nearest embedded variant.)
    if (size > 0) {
        HANDLE scaled = CopyImage(reinterpret_cast<HANDLE>(hCursor), loadType, size, size, 0);
        if (scaled) {
            HCURSOR hScaled = reinterpret_cast<HCURSOR>(scaled);
            if (hScaled != hCursor) {
                DestroyCursorOrIcon(hCursor, loadType);
                hCursor = hScaled;
            }
        } else {
            LogCategory(Log_CursorTextures, "[CursorTextures] WARNING: CopyImage failed to force size to {}px for {} (err={})",
                size, pathStr, GetLastError());
        }
    }

    outData.hCursor = hCursor;

    ICONINFOEXW iconInfoEx = { 0 };
    iconInfoEx.cbSize = sizeof(ICONINFOEXW);
    bool hasIconInfoEx = GetIconInfoExW(hCursor, &iconInfoEx);

    if (!hasIconInfoEx) {
        DWORD err = GetLastError();
        LogCategory(Log_CursorTextures, "[CursorTextures] ERROR: GetIconInfoExW failed with error {}", err);
        DestroyCursorOrIcon(hCursor, loadType);
        outData.hCursor = nullptr;
        return false;
    }

    BITMAP bmp;
    bool isMonochrome = (iconInfoEx.hbmColor == NULL);
    LogCategory(Log_CursorTextures, "[CursorTextures] Cursor type: {}", isMonochrome ? "monochrome" : "color");

    if (isMonochrome) {
        if (!iconInfoEx.hbmMask) {
            LogCategory(Log_CursorTextures, "[CursorTextures] ERROR: Monochrome cursor has no mask bitmap");
            DestroyCursorOrIcon(hCursor, loadType);
            outData.hCursor = nullptr;
            return false;
        }
        if (!GetObject(iconInfoEx.hbmMask, sizeof(BITMAP), &bmp)) {
            DWORD err = GetLastError();
            LogCategory(Log_CursorTextures, "[CursorTextures] ERROR: GetObject for mask bitmap failed with error {}", err);
            DeleteObject(iconInfoEx.hbmMask);
            DestroyCursorOrIcon(hCursor, loadType);
            outData.hCursor = nullptr;
            return false;
        }
    } else {
        if (!GetObject(iconInfoEx.hbmColor, sizeof(BITMAP), &bmp)) {
            DWORD err = GetLastError();
            LogCategory(Log_CursorTextures, "[CursorTextures] ERROR: GetObject for color bitmap failed with error {}", err);
            if (iconInfoEx.hbmMask) DeleteObject(iconInfoEx.hbmMask);
            if (iconInfoEx.hbmColor) DeleteObject(iconInfoEx.hbmColor);
            DestroyCursorOrIcon(hCursor, loadType);
            outData.hCursor = nullptr;
            return false;
        }
    }

    int width = bmp.bmWidth;
    int height = isMonochrome ? bmp.bmHeight / 2 : bmp.bmHeight;

    if (width <= 0 || height <= 0 || width > 1024 || height > 1024) {
        LogCategory(Log_CursorTextures,
                    "[CursorTextures] ERROR: Invalid bitmap dimensions: {}x{}", width, height);
        if (iconInfoEx.hbmMask) DeleteObject(iconInfoEx.hbmMask);
        if (iconInfoEx.hbmColor) DeleteObject(iconInfoEx.hbmColor);
        DestroyCursorOrIcon(hCursor, loadType);
        outData.hCursor = nullptr;
        return false;
    }

    LogCategory(Log_CursorTextures, "[CursorTextures] Bitmap size: {}x{}, hotspot: ({}, {})",
        width, height, iconInfoEx.xHotspot, iconInfoEx.yHotspot);

    outData.bitmapWidth = width;
    outData.bitmapHeight = height;
    outData.hotspotX = iconInfoEx.xHotspot;
    outData.hotspotY = iconInfoEx.yHotspot;

    HDC hdcScreen = GetDC(NULL);
    if (!hdcScreen) {
        DWORD err = GetLastError();
        LogCategory(Log_CursorTextures, "[CursorTextures] ERROR: GetDC(NULL) failed with error {}", err);
        if (iconInfoEx.hbmMask) DeleteObject(iconInfoEx.hbmMask);
        if (iconInfoEx.hbmColor) DeleteObject(iconInfoEx.hbmColor);
        DestroyCursorOrIcon(hCursor, loadType);
        outData.hCursor = nullptr;
        return false;
    }

    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    if (!hdcMem) {
        DWORD err = GetLastError();
        LogCategory(Log_CursorTextures, "[CursorTextures] ERROR: CreateCompatibleDC failed with error {}", err);
        ReleaseDC(NULL, hdcScreen);
        if (iconInfoEx.hbmMask) DeleteObject(iconInfoEx.hbmMask);
        if (iconInfoEx.hbmColor) DeleteObject(iconInfoEx.hbmColor);
        DestroyCursorOrIcon(hCursor, loadType);
        outData.hCursor = nullptr;
        return false;
    }

    std::vector<unsigned char> pixels(width * height * 4);

    BITMAPINFO bmi = { 0 };
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    if (isMonochrome) {
        HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, iconInfoEx.hbmMask);

        std::vector<unsigned char> maskData(width * bmp.bmHeight * 4);
        BITMAPINFO maskBmi = bmi;
        maskBmi.bmiHeader.biHeight = -bmp.bmHeight;
        GetDIBits(hdcMem, iconInfoEx.hbmMask, 0, bmp.bmHeight, maskData.data(), &maskBmi, DIB_RGB_COLORS);

        std::vector<unsigned char> invertPixels(width * height * 4, 0);
        bool hasInverted = false;

        // Windows cursor mask logic:
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                int idx = (y * width + x) * 4;
                int andIdx = (y * width + x) * 4;
                int xorIdx = ((y + height) * width + x) * 4;

                unsigned char andValue = maskData[andIdx];
                unsigned char xorValue = maskData[xorIdx];

                // Windows monochrome cursor mask logic (complete specification):

                bool andBit = (andValue > 128);
                bool xorBit = (xorValue > 128);

                if (andBit && !xorBit) {
                    pixels[idx + 0] = 0;
                    pixels[idx + 1] = 0;
                    pixels[idx + 2] = 0;
                    pixels[idx + 3] = 0;
                } else if (!andBit && !xorBit) {
                    pixels[idx + 0] = 0;
                    pixels[idx + 1] = 0;
                    pixels[idx + 2] = 0;
                    pixels[idx + 3] = 255;
                } else if (andBit && xorBit) {
                    pixels[idx + 0] = 0;
                    pixels[idx + 1] = 0;
                    pixels[idx + 2] = 0;
                    pixels[idx + 3] = 0;

                    invertPixels[idx + 0] = 255;
                    invertPixels[idx + 1] = 255;
                    invertPixels[idx + 2] = 255;
                    invertPixels[idx + 3] = 255;
                    hasInverted = true;
                } else {
                    pixels[idx + 0] = 255;
                    pixels[idx + 1] = 255;
                    pixels[idx + 2] = 255;
                    pixels[idx + 3] = 255;
                }
            }
        }

        outData.hasInvertedPixels = hasInverted;

        if (hasInverted) {
            while (glGetError() != GL_NO_ERROR) {}

            glGenTextures(1, &outData.invertMaskTexture);
            if (outData.invertMaskTexture == 0) {
                LogCategory(Log_CursorTextures, "[CursorTextures] WARNING: Failed to create invert mask texture - glGenTextures returned 0");
                outData.hasInvertedPixels = false;
            } else {
                BindTextureDirect(GL_TEXTURE_2D, outData.invertMaskTexture);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, width, height, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, invertPixels.data());

                GLenum glErr = glGetError();
                if (glErr != GL_NO_ERROR) {
                    LogCategory(Log_CursorTextures,
                                "[CursorTextures] WARNING: OpenGL error creating invert mask texture: {}", glErr);
                    glDeleteTextures(1, &outData.invertMaskTexture);
                    outData.invertMaskTexture = 0;
                    outData.hasInvertedPixels = false;
                } else {
                    LogCategory(Log_CursorTextures, "[CursorTextures] Created invert mask texture ID {}",
                        outData.invertMaskTexture);
                }
                BindTextureDirect(GL_TEXTURE_2D, 0);
            }
        }

        SelectObject(hdcMem, hbmOld);
    } else {
        HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, iconInfoEx.hbmColor);
        GetDIBits(hdcMem, iconInfoEx.hbmColor, 0, height, pixels.data(), &bmi, DIB_RGB_COLORS);

        bool hasAlpha = false;
        if (bmp.bmBitsPixel == 32) {
            for (int i = 0; i < width * height; ++i) {
                if (pixels[i * 4 + 3] != 0) {
                    hasAlpha = true;
                    break;
                }
            }
        }

        if (!hasAlpha && iconInfoEx.hbmMask) {
            std::vector<unsigned char> maskPixels(width * height * 4);
            SelectObject(hdcMem, iconInfoEx.hbmMask);

            BITMAPINFO maskBmi = { 0 };
            maskBmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            maskBmi.bmiHeader.biWidth = width;
            maskBmi.bmiHeader.biHeight = -height;
            maskBmi.bmiHeader.biPlanes = 1;
            maskBmi.bmiHeader.biBitCount = 32;
            maskBmi.bmiHeader.biCompression = BI_RGB;

            GetDIBits(hdcMem, iconInfoEx.hbmMask, 0, height, maskPixels.data(), &maskBmi, DIB_RGB_COLORS);

            for (int i = 0; i < width * height; ++i) {
                unsigned char maskValue = maskPixels[i * 4];
                pixels[i * 4 + 3] = 255 - maskValue;
            }
        } else if (!hasAlpha) {
            for (int i = 0; i < width * height; ++i) { pixels[i * 4 + 3] = 255; }
        }

        SelectObject(hdcMem, hbmOld);
    }
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
    if (iconInfoEx.hbmColor) DeleteObject(iconInfoEx.hbmColor);
    if (iconInfoEx.hbmMask) DeleteObject(iconInfoEx.hbmMask);

    while (glGetError() != GL_NO_ERROR) {}

    glGenTextures(1, &outData.texture);
    if (outData.texture == 0) {
        LogCategory(Log_CursorTextures, "[CursorTextures] ERROR: glGenTextures returned 0 - OpenGL context may not be valid");
        DestroyCursorOrIcon(outData.hCursor, outData.loadType);
        outData.hCursor = nullptr;
        return false;
    }

    BindTextureDirect(GL_TEXTURE_2D, outData.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, pixels.data());

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        std::string errStr;
        switch (err) {
        case GL_INVALID_ENUM:
            errStr = "GL_INVALID_ENUM";
            break;
        case GL_INVALID_VALUE:
            errStr = "GL_INVALID_VALUE";
            break;
        case GL_INVALID_OPERATION:
            errStr = "GL_INVALID_OPERATION";
            break;
        case GL_OUT_OF_MEMORY:
            errStr = "GL_OUT_OF_MEMORY";
            break;
        default:
            errStr = "Unknown (" + std::to_string(err) + ")";
            break;
        }
        LogCategory(Log_CursorTextures, "[CursorTextures] ERROR: OpenGL error during texture creation: {}", errStr);
        glDeleteTextures(1, &outData.texture);
        outData.texture = 0;
        if (outData.invertMaskTexture) {
            glDeleteTextures(1, &outData.invertMaskTexture);
            outData.invertMaskTexture = 0;
        }
        DestroyCursorOrIcon(outData.hCursor, outData.loadType);
        outData.hCursor = nullptr;
        return false;
    }

    BindTextureDirect(GL_TEXTURE_2D, 0);

    LogCategory(Log_CursorTextures, "[CursorTextures] Successfully created texture ID {} ({}x{}) for {}",
        outData.texture, width, height, WideToUtf8(path));
    return true;
}

void LoadCursorTextures() {
    std::lock_guard<std::mutex> lock(g_cursorListMutex);

    InitializeCursorDefinitions();

    std::vector<CursorDef> cursorDefs;
    {
        std::lock_guard<std::mutex> defsLock(g_cursorDefsMutex);
        cursorDefs = AVAILABLE_CURSORS;
    }

    LogCategory(Log_CursorTextures, "[CursorTextures] LoadCursorTextures called - loading initial cursors at default size (64px)");

    int totalLoaded = 0;
    const int defaultSize = 64;

    for (const auto& cursorDef : cursorDefs) {
        CursorData cursorData;
        if (LoadSingleCursor(cursorDef.path, cursorDef.loadType, defaultSize, cursorData)) {
            g_cursorList.push_back(cursorData);
            LogCategory(Log_CursorTextures, "[CursorTextures] Loaded {} at size {}",
                WideToUtf8(cursorDef.path), defaultSize);
            totalLoaded++;
        } else {
            LogCategory(Log_CursorTextures, "[CursorTextures] Failed to load {} at size {}",
                WideToUtf8(cursorDef.path), defaultSize);
        }
    }

    LogCategory(Log_CursorTextures, "[CursorTextures] Finished loading {} default cursor variants");
}

// NOTE: Caller must NOT hold g_cursorListMutex when calling this function
const CursorData* LoadOrFindCursor(const std::wstring& path, UINT loadType, int size) {
    std::string pathStr = WideToUtf8(path);

    if (path.empty()) {
        LogCategory(Log_CursorTextures, "[CursorTextures] ERROR: LoadOrFindCursor called with empty path");
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(g_cursorListMutex);
        for (const auto& cursor : g_cursorList) {
            if (cursor.filePath == path && cursor.size == size) {
                return &cursor;
            }
        }
    }

    LogCategory(Log_CursorTextures, "[CursorTextures] Loading cursor on-demand: {} at size {}", pathStr, size);
    CursorData newCursorData;
    if (LoadSingleCursor(path, loadType, size, newCursorData)) {
        std::lock_guard<std::mutex> lock(g_cursorListMutex);
        g_cursorList.push_back(newCursorData);
        LogCategory(Log_CursorTextures,
                    "[CursorTextures] Successfully loaded on-demand cursor. Total loaded: {}", g_cursorList.size());
        return &g_cursorList.back();
    } else {
        LogCategory(Log_CursorTextures, "[CursorTextures] ERROR: Failed to load cursor on-demand: {}", pathStr);
        return nullptr;
    }
}

const CursorData* FindCursor(const std::wstring& path, int size) {
    if (path.empty()) {
        LogCategory(Log_CursorTextures, "[CursorTextures] ERROR: FindCursor called with empty path");
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(g_cursorListMutex);
        for (const auto& cursor : g_cursorList) {
            if (cursor.filePath == path && cursor.size == size) { return &cursor; }
        }
    }

    UINT loadType = IMAGE_CURSOR;
    try {
        std::filesystem::path fsPath(path);
        std::string ext = fsPath.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".ico") {
            loadType = IMAGE_ICON;
        } else if (ext != ".cur" && ext != ".ani") {
            LogCategory(Log_CursorTextures, "[CursorTextures] WARNING: Unexpected cursor file extension: {}, treating as cursor", ext);
        }
    } catch (const std::exception& e) {
        LogCategory(Log_CursorTextures,
                    "[CursorTextures] WARNING: Failed to parse path extension: {}, defaulting to IMAGE_CURSOR", e.what());
    }

    return LoadOrFindCursor(path, loadType, size);
}

const CursorData* FindCursorByHandle(HCURSOR hCursor) {
    std::lock_guard<std::mutex> lock(g_cursorListMutex);

    for (const auto& cursor : g_cursorList) {
        if (cursor.hCursor == hCursor) { return &cursor; }
    }
    return nullptr;
}

// Does NOT take ownership of hCursor - caller keeps it
static bool CreateTextureFromHandle(HCURSOR hCursor, CursorData& outData) {
    if (!hCursor) { return false; }

    ICONINFOEXW iconInfoEx = { 0 };
    iconInfoEx.cbSize = sizeof(ICONINFOEXW);
    if (!GetIconInfoExW(hCursor, &iconInfoEx)) { return false; }

    BITMAP bmp;
    bool isMonochrome = (iconInfoEx.hbmColor == NULL);

    if (isMonochrome) {
        if (!iconInfoEx.hbmMask || !GetObject(iconInfoEx.hbmMask, sizeof(BITMAP), &bmp)) {
            if (iconInfoEx.hbmMask) DeleteObject(iconInfoEx.hbmMask);
            return false;
        }
    } else {
        if (!GetObject(iconInfoEx.hbmColor, sizeof(BITMAP), &bmp)) {
            if (iconInfoEx.hbmMask) DeleteObject(iconInfoEx.hbmMask);
            if (iconInfoEx.hbmColor) DeleteObject(iconInfoEx.hbmColor);
            return false;
        }
    }

    int width = bmp.bmWidth;
    int height = isMonochrome ? bmp.bmHeight / 2 : bmp.bmHeight;

    if (width <= 0 || height <= 0 || width > 1024 || height > 1024) {
        if (iconInfoEx.hbmMask) DeleteObject(iconInfoEx.hbmMask);
        if (iconInfoEx.hbmColor) DeleteObject(iconInfoEx.hbmColor);
        return false;
    }

    outData.hCursor = hCursor;
    outData.filePath = L"<system>";
    outData.size = 0;
    outData.loadType = IMAGE_CURSOR;
    outData.ownsHandle = false;
    outData.bitmapWidth = width;
    outData.bitmapHeight = height;
    outData.hotspotX = iconInfoEx.xHotspot;
    outData.hotspotY = iconInfoEx.yHotspot;

    HDC hdcScreen = GetDC(NULL);
    if (!hdcScreen) {
        if (iconInfoEx.hbmMask) DeleteObject(iconInfoEx.hbmMask);
        if (iconInfoEx.hbmColor) DeleteObject(iconInfoEx.hbmColor);
        return false;
    }

    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    if (!hdcMem) {
        ReleaseDC(NULL, hdcScreen);
        if (iconInfoEx.hbmMask) DeleteObject(iconInfoEx.hbmMask);
        if (iconInfoEx.hbmColor) DeleteObject(iconInfoEx.hbmColor);
        return false;
    }

    std::vector<unsigned char> pixels(width * height * 4);
    std::vector<unsigned char> invertPixels;

    BITMAPINFO bmi = { 0 };
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    if (isMonochrome) {
        HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, iconInfoEx.hbmMask);
        std::vector<unsigned char> maskData(width * bmp.bmHeight * 4);
        BITMAPINFO maskBmi = bmi;
        maskBmi.bmiHeader.biHeight = -bmp.bmHeight;
        GetDIBits(hdcMem, iconInfoEx.hbmMask, 0, bmp.bmHeight, maskData.data(), &maskBmi, DIB_RGB_COLORS);

        invertPixels.assign(width * height * 4, 0);
        bool hasInverted = false;

        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                int idx = (y * width + x) * 4;
                int andIdx = (y * width + x) * 4;
                int xorIdx = ((y + height) * width + x) * 4;
                unsigned char andValue = maskData[andIdx];
                unsigned char xorValue = maskData[xorIdx];
                bool andBit = (andValue > 128);
                bool xorBit = (xorValue > 128);

                if (andBit && !xorBit) {
                    pixels[idx + 0] = pixels[idx + 1] = pixels[idx + 2] = pixels[idx + 3] = 0;
                } else if (!andBit && !xorBit) {
                    pixels[idx + 0] = pixels[idx + 1] = pixels[idx + 2] = 0;
                    pixels[idx + 3] = 255;
                } else if (andBit && xorBit) {
                    pixels[idx + 0] = pixels[idx + 1] = pixels[idx + 2] = pixels[idx + 3] = 0;
                    invertPixels[idx + 0] = invertPixels[idx + 1] = invertPixels[idx + 2] = invertPixels[idx + 3] = 255;
                    hasInverted = true;
                } else {
                    pixels[idx + 0] = pixels[idx + 1] = pixels[idx + 2] = pixels[idx + 3] = 255;
                }
            }
        }

        outData.hasInvertedPixels = hasInverted;
        if (hasInverted) {
            while (glGetError() != GL_NO_ERROR) {}
            glGenTextures(1, &outData.invertMaskTexture);
            if (outData.invertMaskTexture != 0) {
                BindTextureDirect(GL_TEXTURE_2D, outData.invertMaskTexture);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, width, height, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, invertPixels.data());
                BindTextureDirect(GL_TEXTURE_2D, 0);
            }
        }
        SelectObject(hdcMem, hbmOld);
    } else {
        HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, iconInfoEx.hbmColor);
        GetDIBits(hdcMem, iconInfoEx.hbmColor, 0, height, pixels.data(), &bmi, DIB_RGB_COLORS);

        bool hasAlpha = false;
        if (bmp.bmBitsPixel == 32) {
            for (int i = 0; i < width * height; ++i) {
                if (pixels[i * 4 + 3] != 0) {
                    hasAlpha = true;
                    break;
                }
            }
        }

        if (!hasAlpha && iconInfoEx.hbmMask) {
            std::vector<unsigned char> maskPixels(width * height * 4);
            SelectObject(hdcMem, iconInfoEx.hbmMask);
            BITMAPINFO maskBmi = { 0 };
            maskBmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            maskBmi.bmiHeader.biWidth = width;
            maskBmi.bmiHeader.biHeight = -height;
            maskBmi.bmiHeader.biPlanes = 1;
            maskBmi.bmiHeader.biBitCount = 32;
            maskBmi.bmiHeader.biCompression = BI_RGB;
            GetDIBits(hdcMem, iconInfoEx.hbmMask, 0, height, maskPixels.data(), &maskBmi, DIB_RGB_COLORS);
            for (int i = 0; i < width * height; ++i) { pixels[i * 4 + 3] = 255 - maskPixels[i * 4]; }
        } else if (!hasAlpha) {
            for (int i = 0; i < width * height; ++i) { pixels[i * 4 + 3] = 255; }
        }
        SelectObject(hdcMem, hbmOld);
    }

    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
    if (iconInfoEx.hbmColor) DeleteObject(iconInfoEx.hbmColor);
    if (iconInfoEx.hbmMask) DeleteObject(iconInfoEx.hbmMask);

    ComputeCursorContentBounds(outData, pixels, outData.hasInvertedPixels ? &invertPixels : nullptr, width, height);

    while (glGetError() != GL_NO_ERROR) {}
    glGenTextures(1, &outData.texture);
    if (outData.texture == 0) { return false; }

    BindTextureDirect(GL_TEXTURE_2D, outData.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, pixels.data());

    if (glGetError() != GL_NO_ERROR) {
        glDeleteTextures(1, &outData.texture);
        outData.texture = 0;
        if (outData.invertMaskTexture) {
            glDeleteTextures(1, &outData.invertMaskTexture);
            outData.invertMaskTexture = 0;
        }
        return false;
    }

    BindTextureDirect(GL_TEXTURE_2D, 0);
    return true;
}

const CursorData* LoadOrFindCursorFromHandle(HCURSOR hCursor) {
    if (!hCursor) { return nullptr; }

    {
        std::lock_guard<std::mutex> lock(g_cursorListMutex);
        for (const auto& cursor : g_cursorList) {
            if (cursor.hCursor == hCursor) { return &cursor; }
        }
    }

    CursorData newData;
    if (!CreateTextureFromHandle(hCursor, newData)) { return nullptr; }

    std::lock_guard<std::mutex> lock(g_cursorListMutex);
    g_cursorList.push_back(newData);
    return &g_cursorList.back();
}

const CursorData* LoadOrFindSystemCursor(LPCWSTR systemCursorId) {
    if (!systemCursorId || wglGetCurrentContext() == nullptr) {
        return nullptr;
    }

    const std::wstring cacheKey = BuildSystemCursorCacheKey(systemCursorId);
    {
        std::lock_guard<std::mutex> lock(g_cursorListMutex);
        for (const auto& cursor : g_cursorList) {
            if (cursor.filePath == cacheKey) {
                return &cursor;
            }
        }
    }

    HCURSOR sharedCursor = LoadCursorW(nullptr, systemCursorId);
    if (!sharedCursor) {
        return nullptr;
    }

    HCURSOR ownedCursor = reinterpret_cast<HCURSOR>(CopyImage(reinterpret_cast<HANDLE>(sharedCursor), IMAGE_CURSOR, 0, 0, LR_DEFAULTSIZE));
    if (!ownedCursor) {
        return nullptr;
    }

    CursorData newData;
    if (!CreateTextureFromHandle(ownedCursor, newData)) {
        DestroyCursor(ownedCursor);
        return nullptr;
    }

    newData.hCursor = ownedCursor;
    newData.filePath = cacheKey;
    newData.size = 0;
    newData.loadType = IMAGE_CURSOR;
    newData.ownsHandle = true;

    std::lock_guard<std::mutex> lock(g_cursorListMutex);
    g_cursorList.push_back(newData);
    return &g_cursorList.back();
}

const CursorData* GetSelectedCursor(const std::string& gameState, int size) {
    // Get cursor name and size for the current game state from config snapshot (thread-safe)
    auto cfgSnap = GetConfigSnapshot();
    if (!cfgSnap) return nullptr;

    std::string selectedCursorName = "";
    int selectedSize = size;

    if (!cfgSnap->cursors.enabled) {
        return nullptr;
    }

    if (gameState == "title") {
        selectedCursorName = cfgSnap->cursors.title.cursorName;
        selectedSize = cfgSnap->cursors.title.cursorSize;
    } else if (gameState == "wall") {
        selectedCursorName = cfgSnap->cursors.wall.cursorName;
        selectedSize = cfgSnap->cursors.wall.cursorSize;
    } else {
        selectedCursorName = cfgSnap->cursors.ingame.cursorName;
        selectedSize = cfgSnap->cursors.ingame.cursorSize;
    }

    std::wstring cursorPath;
    UINT loadType = IMAGE_CURSOR;
    GetCursorPathByName(selectedCursorName, cursorPath, loadType);

    const CursorData* cursorData = FindCursor(cursorPath, selectedSize);
    if (cursorData) { return cursorData; }

    Log("[GetSelectedCursor] Cursor '{}' not found at size {}, trying fallback", selectedCursorName, selectedSize);

    {
        std::lock_guard<std::mutex> lock(g_cursorListMutex);
        for (const auto& cursor : g_cursorList) {
            if (cursor.size == selectedSize && cursor.texture != 0) {
                Log("[GetSelectedCursor] Fallback: using cursor from {}", WideToUtf8(cursor.filePath));
                return &cursor;
            }
        }
        for (const auto& cursor : g_cursorList) {
            if (cursor.texture != 0) {
                Log("[GetSelectedCursor] Fallback: using cursor from {} at size {}",
                    WideToUtf8(cursor.filePath), std::to_string(cursor.size));
                return &cursor;
            }
        }
    }

    Log("[GetSelectedCursor] No fallback cursor available, rendering nothing");
    return nullptr;
}

bool GetCursorPathByName(const std::string& cursorName, std::wstring& outPath, UINT& outLoadType) {
    if (!g_cursorDefsInitialized) { InitializeCursorDefinitions(); }

    std::lock_guard<std::mutex> lock(g_cursorDefsMutex);
    const CursorDef* selectedDef = nullptr;
    for (const auto& def : AVAILABLE_CURSORS) {
        if (def.name == cursorName) {
            selectedDef = &def;
            break;
        }
    }

    if (selectedDef) {
        outPath = selectedDef->path;
        outLoadType = selectedDef->loadType;
        return true;
    } else {
        LogCategory(Log_CursorTextures, "[CursorTextures] WARNING: Unknown cursor name '{}'", cursorName);
        LogCategory(Log_CursorTextures, "[CursorTextures] Available cursors: {}", AVAILABLE_CURSORS.size());
        for (const auto& def : AVAILABLE_CURSORS) { LogCategory(Log_CursorTextures, "[CursorTextures]   - {}", def.name); }

        if (!AVAILABLE_CURSORS.empty()) {
            outPath = AVAILABLE_CURSORS[0].path;
            outLoadType = AVAILABLE_CURSORS[0].loadType;
            LogCategory(Log_CursorTextures, "[CursorTextures] Using first available cursor as fallback: {}", AVAILABLE_CURSORS[0].name);
            return false;
        }

        outPath = L"";
        outLoadType = IMAGE_CURSOR;
        LogCategory(Log_CursorTextures, "[CursorTextures] ERROR: No cursors available for fallback");
        return false;
    }
}

bool IsCursorFileValid(const std::string& cursorName) {
    if (!g_cursorDefsInitialized) { InitializeCursorDefinitions(); }

    std::lock_guard<std::mutex> lock(g_cursorDefsMutex);

    if (cursorName.empty()) {
        LogCategory(Log_CursorTextures, "[CursorTextures] IsCursorFileValid: Empty cursor name provided");
        return false;
    }

    const CursorDef* selectedDef = nullptr;
    for (const auto& def : AVAILABLE_CURSORS) {
        if (def.name == cursorName) {
            selectedDef = &def;
            break;
        }
    }

    if (!selectedDef) {
        LogCategory(Log_CursorTextures, "[CursorTextures] IsCursorFileValid: Cursor '{}' not found in definitions", cursorName);
        return false;
    }

    std::wstring resolvedPath = selectedDef->path;
    try {
        if (!std::filesystem::path(selectedDef->path).is_absolute()) { resolvedPath = ResolveCwdPath(selectedDef->path); }
    } catch (const std::exception& e) {
        LogCategory(Log_CursorTextures,
                    "[CursorTextures] IsCursorFileValid: Failed to resolve path for '{}': {}", cursorName, e.what());
        return false;
    }

    bool exists = std::filesystem::exists(resolvedPath);
    if (!exists) {
        LogCategory(Log_CursorTextures, "[CursorTextures] IsCursorFileValid: Cursor file does not exist: {}", WideToUtf8(resolvedPath));
    }
    return exists;
}

void Cleanup() {
    std::lock_guard<std::mutex> lock(g_cursorListMutex);

    LogCategory(Log_CursorTextures,
                "[CursorTextures] Cleanup: Starting cleanup of {} cursor entries", g_cursorList.size());

    int texturesDeleted = 0;
    int invertMasksDeleted = 0;
    int cursorsDestroyed = 0;

    for (auto& cursor : g_cursorList) {
        if (cursor.texture) {
            glDeleteTextures(1, &cursor.texture);
            cursor.texture = 0;
            texturesDeleted++;
        }
        if (cursor.invertMaskTexture) {
            glDeleteTextures(1, &cursor.invertMaskTexture);
            cursor.invertMaskTexture = 0;
            invertMasksDeleted++;
        }
        if (cursor.hCursor && cursor.ownsHandle) {
            DestroyCursorOrIcon(cursor.hCursor, cursor.loadType);
            cursor.hCursor = nullptr;
            cursorsDestroyed++;
        } else if (cursor.hCursor) {
            cursor.hCursor = nullptr;
        }
    }

    g_cursorList.clear();
    LogCategory(Log_CursorTextures, "[CursorTextures] Cleanup complete: {} textures, {} invert masks, {} cursor handles",
        texturesDeleted, invertMasksDeleted, cursorsDestroyed);
}

std::vector<std::string> GetAvailableCursorNames() {
    if (!g_cursorDefsInitialized) { InitializeCursorDefinitions(); }

    std::lock_guard<std::mutex> lock(g_cursorDefsMutex);
    std::vector<std::string> names;
    for (const auto& cursor : AVAILABLE_CURSORS) { names.push_back(cursor.name); }
    return names;
}

}

static int s_fakeCursorLogCounter = 0;
static const int FAKE_CURSOR_LOG_INTERVAL = 300;

static gloverlay::QuadBatch s_fcBatch;

static void RenderFakeCursorInternal(HWND hwnd,
                                     int fullWidth,
                                     int fullHeight,
                                     int targetX,
                                     int targetY,
                                     int targetWidth,
                                     int targetHeight,
                                     int sourceWidth,
                                     int sourceHeight,
                                     bool bindDefaultFramebuffer) {
    if (!hwnd || fullWidth <= 0 || fullHeight <= 0 || targetWidth <= 0 || targetHeight <= 0) { return; }

    s_fakeCursorLogCounter++;
    bool shouldLog = (s_fakeCursorLogCounter % FAKE_CURSOR_LOG_INTERVAL == 0);

    CURSORINFO cursorInfo = { 0 };
    cursorInfo.cbSize = sizeof(CURSORINFO);
    if (!GetCursorInfo(&cursorInfo)) {
        if (shouldLog) {
            Log("[FakeCursor] GetCursorInfo failed with error {}", GetLastError());
        }
        return;
    }
    if (!cursorInfo.hCursor) { return; }
    if (!(cursorInfo.flags & CURSOR_SHOWING)) { return; }

    const CursorTextures::CursorData* cursorData = CursorTextures::LoadOrFindCursorFromHandle(cursorInfo.hCursor);

    if (!cursorData) {
        if (shouldLog) {
            Log("[FakeCursor] Cursor handle 0x{:X} could not be resolved to a texture",
                reinterpret_cast<uintptr_t>(cursorInfo.hCursor));
        }
        return;
    }

    RECT gameClientRectInScreen;
    if (!GetWindowClientRectInScreen(hwnd, gameClientRectInScreen)) {
        if (shouldLog) {
            Log("[FakeCursor] GetWindowClientRectInScreen failed with error {}", GetLastError());
        }
        return;
    }

    const int actualGameWindowWidth = gameClientRectInScreen.right - gameClientRectInScreen.left;
    const int actualGameWindowHeight = gameClientRectInScreen.bottom - gameClientRectInScreen.top;
    if (actualGameWindowWidth <= 0 || actualGameWindowHeight <= 0) { return; }

    if (cursorInfo.ptScreenPos.x < gameClientRectInScreen.left || cursorInfo.ptScreenPos.x >= gameClientRectInScreen.right ||
        cursorInfo.ptScreenPos.y < gameClientRectInScreen.top || cursorInfo.ptScreenPos.y >= gameClientRectInScreen.bottom) {
        return;
    }

    const bool projectAcrossWholeWindow = sourceWidth > 0 && sourceHeight > 0;
    const int projectionX = projectAcrossWholeWindow ? 0 : targetX;
    const int projectionY = projectAcrossWholeWindow ? 0 : targetY;
    const int projectionWidth = projectAcrossWholeWindow ? fullWidth : targetWidth;
    const int projectionHeight = projectAcrossWholeWindow ? fullHeight : targetHeight;

    if (projectionWidth <= 0 || projectionHeight <= 0) { return; }

    // The bitmap is already at the user's desired cursor size (includes Windows cursor scaling)

    float offset = cursorData->loadType == IMAGE_CURSOR ? 1.5f : 1.0f;

    int systemCursorWidth = cursorData->bitmapWidth;
    int systemCursorHeight = cursorData->bitmapHeight;
    if (systemCursorWidth <= 0 || systemCursorHeight <= 0) { return; }

    int scaledCursorWidth = (systemCursorWidth * projectionWidth) / actualGameWindowWidth;
    int scaledCursorHeight = (systemCursorHeight * projectionHeight) / actualGameWindowHeight;

    int scaledHotspotX = static_cast<int>((cursorData->hotspotX * scaledCursorWidth * offset) / systemCursorWidth);
    int scaledHotspotY = static_cast<int>((cursorData->hotspotY * scaledCursorHeight * offset) / systemCursorHeight);

    int renderWidth = static_cast<int>(scaledCursorWidth * offset);
    int renderHeight = static_cast<int>(scaledCursorHeight * offset);

    const double projectedCursorX = (static_cast<double>(cursorInfo.ptScreenPos.x - gameClientRectInScreen.left) * projectionWidth) /
                                    static_cast<double>(actualGameWindowWidth);
    const double projectedCursorY = (static_cast<double>(cursorInfo.ptScreenPos.y - gameClientRectInScreen.top) * projectionHeight) /
                                    static_cast<double>(actualGameWindowHeight);

    int cursorX = projectionX + static_cast<int>(projectedCursorX) - scaledHotspotX;
    int cursorY = projectionY + static_cast<int>(projectedCursorY) - scaledHotspotY;

    if (renderWidth > 0 && renderHeight > 0 && renderWidth < 512 && renderHeight < 512) {
        if (!s_fcBatch.Ensure()) { return; }

        const float invHalfW = 2.0f / static_cast<float>(fullWidth);
        const float invHalfH = 2.0f / static_cast<float>(fullHeight);
        auto DrawCursorQuad = [&](int x, int y) {
            const float x0 = static_cast<float>(x) * invHalfW - 1.0f;
            const float y0 = 1.0f - static_cast<float>(y) * invHalfH;
            const float x1 = static_cast<float>(x + renderWidth) * invHalfW - 1.0f;
            const float y1 = 1.0f - static_cast<float>(y + renderHeight) * invHalfH;
            const float quad[48] = {
                x0, y0, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                x1, y0, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                x1, y1, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                x0, y0, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                x1, y1, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                x0, y1, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
            };
            s_fcBatch.Draw(quad, 6);
        };

        gloverlay::ScopedState glState;
        glDisable(GL_STENCIL_TEST);
        if (bindDefaultFramebuffer) { glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0); }

        BindTextureDirect(GL_TEXTURE_2D, cursorData->texture);
        DrawCursorQuad(cursorX, cursorY);

        if (cursorData->hasInvertedPixels && cursorData->invertMaskTexture != 0) {
            BindTextureDirect(GL_TEXTURE_2D, cursorData->invertMaskTexture);
            glBlendFunc(GL_ONE_MINUS_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA);
            DrawCursorQuad(cursorX, cursorY);
        }
    }
}

void RenderFakeCursor(HWND hwnd, int windowWidth, int windowHeight) {
    RenderFakeCursorInternal(hwnd, windowWidth, windowHeight, 0, 0, windowWidth, windowHeight, 0, 0, true);
}

void RenderFakeCursorToCurrentTarget(HWND hwnd,
                                     int fullWidth,
                                     int fullHeight,
                                     int targetX,
                                     int targetY,
                                     int targetWidth,
                                     int targetHeight,
                                     int sourceWidth,
                                     int sourceHeight) {
    RenderFakeCursorInternal(hwnd, fullWidth, fullHeight, targetX, targetY, targetWidth, targetHeight, sourceWidth,
                             sourceHeight, false);
}



