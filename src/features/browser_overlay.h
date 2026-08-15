#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "gui/gui.h"
#include <GL/glew.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct BrowserOverlayTextureFrame {
    GLuint textureId = 0;
    int textureWidth = 0;
    int textureHeight = 0;
};

struct BrowserOverlayPixelFrame {
    std::shared_ptr<const std::vector<unsigned char>> pixels;
    int width = 0;
    int height = 0;
    uint64_t generation = 0;
};

void StartBrowserOverlayThread();
void StopBrowserOverlayThread();
void CleanupBrowserOverlayCache();
void RemoveBrowserOverlayFromCache(const std::string& overlayId);
void RequestBrowserOverlayRefresh(const std::string& overlayId);
const BrowserOverlayConfig* FindBrowserOverlayConfig(const std::string& overlayId);
const BrowserOverlayConfig* FindBrowserOverlayConfigIn(const std::string& overlayId, const Config& config);
bool StageBrowserOverlayTestFrame(const BrowserOverlayConfig& config, const std::vector<unsigned char>& rgbaPixels, int width, int height);
bool PrepareBrowserOverlayTexture(const BrowserOverlayConfig& config, BrowserOverlayTextureFrame& outFrame);
bool AcquireBrowserOverlayPixelFrame(const BrowserOverlayConfig& config, BrowserOverlayPixelFrame& outFrame);
