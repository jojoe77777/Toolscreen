#include "gui_internal.h"

#include "common/utils.h"
#include "render/render.h"
#include "render/render_backend.h"
#include "render/vulkan/vulkan_renderer.h"
#include "third_party/stb_image.h"

#include <GL/glew.h>
#include <algorithm>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>
#include <unordered_map>
#include <winhttp.h>

#pragma comment(lib, "Winhttp.lib")

struct SupporterTierTextureEntry {
    GLuint textureId = 0;
    int width = 0;
    int height = 0;
};

struct DecodedSupporterTierImage {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> rgbaPixels;
};

std::shared_mutex g_supportersMutex;
std::vector<SupporterRoleEntry> g_supporterRoles;
std::atomic<bool> g_supportersLoaded{ false };
std::atomic<bool> g_supportersFetchEverFailed{ false };
static std::atomic<bool> g_supportersFetchStarted{ false };
std::atomic<bool> g_supporterTierTexturesDirty{ false };
static std::mutex g_supporterTierTexturesMutex;
static std::unordered_map<std::string, SupporterTierTextureEntry> g_supporterTierTextures;

static bool HttpGetToString(const std::wstring& url, std::string& outBody, std::string& outError) {
    URL_COMPONENTS urlComp{};
    urlComp.dwStructSize = sizeof(urlComp);
    urlComp.dwSchemeLength = (DWORD)-1;
    urlComp.dwHostNameLength = (DWORD)-1;
    urlComp.dwUrlPathLength = (DWORD)-1;
    urlComp.dwExtraInfoLength = (DWORD)-1;

    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &urlComp)) {
        outError = "WinHttpCrackUrl failed";
        return false;
    }

    std::wstring host(urlComp.lpszHostName, urlComp.dwHostNameLength);
    std::wstring path(urlComp.lpszUrlPath ? std::wstring(urlComp.lpszUrlPath, urlComp.dwUrlPathLength) : L"/");
    if (urlComp.lpszExtraInfo && urlComp.dwExtraInfoLength > 0) { path.append(urlComp.lpszExtraInfo, urlComp.dwExtraInfoLength); }

    HINTERNET hSession = WinHttpOpen(L"Toolscreen/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        outError = "WinHttpOpen failed";
        return false;
    }

    WinHttpSetTimeouts(hSession, 5000, 5000, 10000, 10000);

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), urlComp.nPort, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        outError = "WinHttpConnect failed";
        return false;
    }

    DWORD requestFlags = 0;
    if (urlComp.nScheme == INTERNET_SCHEME_HTTPS) { requestFlags |= WINHTTP_FLAG_SECURE; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            requestFlags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        outError = "WinHttpOpenRequest failed";
        return false;
    }

    bool ok = false;
    do {
        if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            outError = "WinHttpSendRequest failed";
            break;
        }

        if (!WinHttpReceiveResponse(hRequest, nullptr)) {
            outError = "WinHttpReceiveResponse failed";
            break;
        }

        DWORD statusCode = 0;
        DWORD statusCodeSize = sizeof(statusCode);
        if (!WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode,
                                 &statusCodeSize, WINHTTP_NO_HEADER_INDEX)) {
            outError = "WinHttpQueryHeaders(status) failed";
            break;
        }

        if (statusCode != 200) {
            outError = "HTTP status " + std::to_string(statusCode);
            break;
        }

        outBody.clear();
        while (true) {
            DWORD bytesAvailable = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &bytesAvailable)) {
                outError = "WinHttpQueryDataAvailable failed";
                break;
            }
            if (bytesAvailable == 0) {
                ok = true;
                break;
            }

            std::string chunk;
            chunk.resize(bytesAvailable);
            DWORD bytesRead = 0;
            if (!WinHttpReadData(hRequest, chunk.data(), bytesAvailable, &bytesRead)) {
                outError = "WinHttpReadData failed";
                break;
            }
            chunk.resize(bytesRead);
            outBody += chunk;
        }
    } while (false);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return ok;
}

static bool ParseSupportersJson(const std::string& body, std::vector<SupporterRoleEntry>& outRoles, std::string& outError) {
    outRoles.clear();

    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(body);
    } catch (const std::exception& e) {
        outError = std::string("JSON parse failed: ") + e.what();
        return false;
    }

    if (!parsed.is_array()) {
        outError = "JSON root must be an array";
        return false;
    }

    for (const auto& roleJson : parsed) {
        if (!roleJson.is_object()) continue;

        SupporterRoleEntry role;
        role.name = roleJson.value("name", "");
        role.imageUrl = (roleJson.contains("image") && roleJson["image"].is_string()) ? roleJson["image"].get<std::string>() : "";

        const std::string colorString = roleJson.value("color", "#FFFFFF");
        ParseColorString(colorString, role.color);

        if (roleJson.contains("members") && roleJson["members"].is_array()) {
            for (const auto& member : roleJson["members"]) {
                if (member.is_string()) { role.members.push_back(member.get<std::string>()); }
            }
        }

        if (!role.name.empty()) { outRoles.push_back(std::move(role)); }
    }

    return true;
}

static bool DecodeSupporterTierImage(const std::string& encodedBytes, DecodedSupporterTierImage& outImage, std::string& outError) {
    outImage = {};
    if (encodedBytes.empty()) {
        outError = "Image response body is empty";
        return false;
    }

    stbi_set_flip_vertically_on_load_thread(0);

    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(encodedBytes.data()),
                                                  static_cast<int>(encodedBytes.size()), &width, &height, &channels, 4);
    if (!pixels || width <= 0 || height <= 0) {
        outError = std::string("Failed to decode image: ") + (stbi_failure_reason() ? stbi_failure_reason() : "unknown");
        if (pixels) { stbi_image_free(pixels); }
        return false;
    }

    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(4);
    outImage.width = width;
    outImage.height = height;
    outImage.rgbaPixels.assign(pixels, pixels + pixelCount);
    stbi_image_free(pixels);
    return true;
}

static void PopulateSupporterTierImages(std::vector<SupporterRoleEntry>& roles) {
    std::unordered_map<std::string, DecodedSupporterTierImage> decodeCache;

    for (auto& role : roles) {
        role.tierIconWidth = 0;
        role.tierIconHeight = 0;
        role.tierIconGeneration = 0;
        role.tierIconPixels.clear();

        if (role.imageUrl.empty()) { continue; }

        auto cached = decodeCache.find(role.imageUrl);
        if (cached != decodeCache.end()) {
            if (cached->second.width > 0 && cached->second.height > 0 && !cached->second.rgbaPixels.empty()) {
                role.tierIconWidth = cached->second.width;
                role.tierIconHeight = cached->second.height;
                role.tierIconPixels = cached->second.rgbaPixels;
                uint64_t hash = 1469598103934665603ull;
                for (const unsigned char value : role.tierIconPixels) {
                    hash ^= value;
                    hash *= 1099511628211ull;
                }
                role.tierIconGeneration = hash != 0 ? hash : 1;
            }
            continue;
        }

        std::string imageBody;
        std::string fetchError;
        if (!HttpGetToString(Utf8ToWide(role.imageUrl), imageBody, fetchError)) {
            Log("Supporters metadata: failed to fetch tier image '" + role.imageUrl + "': " + fetchError);
            decodeCache.emplace(role.imageUrl, DecodedSupporterTierImage{});
            continue;
        }

        DecodedSupporterTierImage decoded;
        std::string decodeError;
        if (!DecodeSupporterTierImage(imageBody, decoded, decodeError)) {
            Log("Supporters metadata: failed to decode tier image '" + role.imageUrl + "': " + decodeError);
            decodeCache.emplace(role.imageUrl, DecodedSupporterTierImage{});
            continue;
        }

        auto inserted = decodeCache.emplace(role.imageUrl, std::move(decoded));
        const DecodedSupporterTierImage& cachedDecoded = inserted.first->second;

        role.tierIconWidth = cachedDecoded.width;
        role.tierIconHeight = cachedDecoded.height;
        role.tierIconPixels = cachedDecoded.rgbaPixels;
        uint64_t hash = 1469598103934665603ull;
        for (const unsigned char value : role.tierIconPixels) {
            hash ^= value;
            hash *= 1099511628211ull;
        }
        role.tierIconGeneration = hash != 0 ? hash : 1;
    }
}

bool EnsureSupporterTierTexture(const SupporterRoleEntry& role, uintptr_t& outTextureId, int& outWidth, int& outHeight) {
    outTextureId = 0;
    outWidth = 0;
    outHeight = 0;

    if (role.imageUrl.empty() || role.tierIconWidth <= 0 || role.tierIconHeight <= 0 || role.tierIconPixels.empty()) { return false; }

    if (GetRenderBackend() == RenderBackend::Vulkan) {
        if (!VulkanRenderer::GetGuiRgbaTexture(
                "supporter:" + role.imageUrl, role.tierIconPixels.data(),
                role.tierIconWidth, role.tierIconHeight,
                role.tierIconGeneration, outTextureId)) {
            return false;
        }
        outWidth = role.tierIconWidth;
        outHeight = role.tierIconHeight;
        return true;
    }

    std::lock_guard<std::mutex> lock(g_supporterTierTexturesMutex);

    auto it = g_supporterTierTextures.find(role.imageUrl);
    if (it != g_supporterTierTextures.end() && it->second.textureId != 0) {
        outTextureId = static_cast<uintptr_t>(it->second.textureId);
        outWidth = it->second.width;
        outHeight = it->second.height;
        return true;
    }

    if (wglGetCurrentContext() == nullptr) { return false; }

    SupporterTierTextureEntry entry;
    glGenTextures(1, &entry.textureId);
    if (entry.textureId == 0) { return false; }

    BindTextureDirect(GL_TEXTURE_2D, entry.textureId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, role.tierIconWidth, role.tierIconHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 role.tierIconPixels.data());
    BindTextureDirect(GL_TEXTURE_2D, 0);

    entry.width = role.tierIconWidth;
    entry.height = role.tierIconHeight;

    g_supporterTierTextures[role.imageUrl] = entry;

    outTextureId = static_cast<uintptr_t>(entry.textureId);
    outWidth = entry.width;
    outHeight = entry.height;
    return true;
}

void ClearSupporterTierTextureCache() {
    std::lock_guard<std::mutex> lock(g_supporterTierTexturesMutex);

    if (wglGetCurrentContext() != nullptr) {
        for (auto& it : g_supporterTierTextures) {
            if (it.second.textureId != 0) {
                glDeleteTextures(1, &it.second.textureId);
                it.second.textureId = 0;
            }
        }
    }

    g_supporterTierTextures.clear();
}

void StartSupportersFetch() {
    bool expected = false;
    if (!g_supportersFetchStarted.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) { return; }
    Log("Supporters metadata: starting background fetch thread.");

    std::thread([]() {
        static const std::wstring kMembershipUrl =
            L"https://raw.githubusercontent.com/jojoe77777/toolscreen-meta/refs/heads/main/membership.json";
        static constexpr int kMinRetryDelaySeconds = 30;
        static constexpr int kMaxRetryDelaySeconds = 3600;

        Log("Supporters metadata: fetch thread running.");

        int retryDelaySeconds = kMinRetryDelaySeconds;
        while (true) {
            std::string fetchError;

            try {
                std::string body;
                if (HttpGetToString(kMembershipUrl, body, fetchError)) {
                    std::vector<SupporterRoleEntry> parsedRoles;
                    std::string parseError;
                    if (ParseSupportersJson(body, parsedRoles, parseError)) {
                        PopulateSupporterTierImages(parsedRoles);

                        {
                            std::unique_lock<std::shared_mutex> writeLock(g_supportersMutex);
                            g_supporterRoles = std::move(parsedRoles);
                        }

                        g_supporterTierTexturesDirty.store(true, std::memory_order_release);
                        g_supportersLoaded.store(true, std::memory_order_release);
                        g_supportersFetchEverFailed.store(false, std::memory_order_release);
                        Log("Loaded supporters metadata.");
                        return;
                    }

                    fetchError = parseError;
                }
            } catch (const std::exception& e) {
                fetchError = std::string("Unexpected exception: ") + e.what();
            } catch (...) {
                fetchError = "Unexpected unknown exception";
            }

            if (fetchError.empty()) { fetchError = "Unknown fetch failure"; }
            g_supportersFetchEverFailed.store(true, std::memory_order_release);
            Log("Supporters metadata fetch failed; retrying in " + std::to_string(retryDelaySeconds) + "s. Reason: " + fetchError);

            std::this_thread::sleep_for(std::chrono::seconds(retryDelaySeconds));
            retryDelaySeconds = (std::min)(retryDelaySeconds * 2, kMaxRetryDelaySeconds);
        }
    }).detach();
}
