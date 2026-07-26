#include "font_assets.h"

#include "common/utils.h"
#include "platform/resource.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <system_error>

namespace {

std::vector<SystemFontAsset> s_systemFontAssets;
std::once_flag s_systemFontAssetsOnce;

std::filesystem::path Utf8Path(std::string_view path) {
    return std::filesystem::path(Utf8ToWide(std::string(path)));
}

std::wstring TrimWide(std::wstring value) {
    const auto isSpace = [](wchar_t ch) { return std::iswspace(static_cast<wint_t>(ch)) != 0; };

    while (!value.empty() && isSpace(value.front())) {
        value.erase(value.begin());
    }
    while (!value.empty() && isSpace(value.back())) {
        value.pop_back();
    }

    return value;
}

bool EqualsIgnoreCaseWide(std::wstring_view left, std::wstring_view right) {
    if (left.size() != right.size()) {
        return false;
    }

    for (size_t index = 0; index < left.size(); ++index) {
        if (std::towlower(static_cast<wint_t>(left[index])) != std::towlower(static_cast<wint_t>(right[index]))) {
            return false;
        }
    }

    return true;
}

bool EndsWithIgnoreCaseWide(std::wstring_view value, std::wstring_view suffix) {
    if (suffix.size() > value.size()) {
        return false;
    }

    return EqualsIgnoreCaseWide(value.substr(value.size() - suffix.size()), suffix);
}

std::wstring TitleCaseWide(std::wstring value) {
    bool startOfWord = true;

    for (wchar_t& ch : value) {
        if (std::iswalnum(static_cast<wint_t>(ch)) != 0) {
            ch = startOfWord
                ? static_cast<wchar_t>(std::towupper(static_cast<wint_t>(ch)))
                : static_cast<wchar_t>(std::towlower(static_cast<wint_t>(ch)));
            startOfWord = false;
            continue;
        }

        if (ch == L'_' || ch == L'/') {
            ch = L' ';
        }
        startOfWord = true;
    }

    return value;
}

std::wstring NormalizeDisplayWide(std::wstring value) {
    value = TrimWide(std::move(value));
    if (value.empty()) {
        return value;
    }

    std::wstring compacted;
    compacted.reserve(value.size());
    bool previousWasSpace = false;
    for (wchar_t ch : value) {
        const bool isSpace = std::iswspace(static_cast<wint_t>(ch)) != 0;
        if (isSpace) {
            if (!previousWasSpace) {
                compacted.push_back(L' ');
            }
            previousWasSpace = true;
        } else {
            compacted.push_back(ch);
            previousWasSpace = false;
        }
    }

    return TitleCaseWide(TrimWide(std::move(compacted)));
}

bool ReadFileBytes(const std::filesystem::path& path, std::vector<uint8_t>& outBytes) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input.is_open()) {
        return false;
    }

    const std::streamoff size = input.tellg();
    if (size <= 0) {
        return false;
    }

    input.seekg(0, std::ios::beg);
    outBytes.resize(static_cast<size_t>(size));
    input.read(reinterpret_cast<char*>(outBytes.data()), static_cast<std::streamsize>(outBytes.size()));
    return input.good() || static_cast<size_t>(input.gcount()) == outBytes.size();
}

bool TryReadBigEndianU16(const std::vector<uint8_t>& bytes, size_t offset, uint16_t& outValue) {
    if (offset + 2 > bytes.size()) {
        return false;
    }

    outValue = static_cast<uint16_t>((static_cast<uint16_t>(bytes[offset]) << 8) | static_cast<uint16_t>(bytes[offset + 1]));
    return true;
}

bool TryReadBigEndianU32(const std::vector<uint8_t>& bytes, size_t offset, uint32_t& outValue) {
    if (offset + 4 > bytes.size()) {
        return false;
    }

    outValue = (static_cast<uint32_t>(bytes[offset]) << 24) |
               (static_cast<uint32_t>(bytes[offset + 1]) << 16) |
               (static_cast<uint32_t>(bytes[offset + 2]) << 8) |
               static_cast<uint32_t>(bytes[offset + 3]);
    return true;
}

std::wstring DecodeSfntNameString(const std::vector<uint8_t>& bytes, size_t offset, uint16_t length, uint16_t platformId) {
    if (offset + length > bytes.size()) {
        return {};
    }

    std::wstring decoded;
    if (platformId == 0 || platformId == 3) {
        if ((length % 2) != 0) {
            return {};
        }

        decoded.reserve(length / 2);
        for (size_t index = 0; index < length; index += 2) {
            const wchar_t ch = static_cast<wchar_t>((static_cast<uint16_t>(bytes[offset + index]) << 8) |
                                                    static_cast<uint16_t>(bytes[offset + index + 1]));
            if (ch != L'\0') {
                decoded.push_back(ch);
            }
        }
    } else {
        decoded.reserve(length);
        for (size_t index = 0; index < length; ++index) {
            const uint8_t ch = bytes[offset + index];
            if (ch != 0) {
                decoded.push_back(static_cast<wchar_t>(ch));
            }
        }
    }

    return TrimWide(std::move(decoded));
}

struct SfntNameCandidate {
    std::wstring value;
    int score = -1;
};

struct ParsedSfntNames {
    std::wstring family;
    std::wstring subfamily;
    std::wstring fullName;
    std::wstring preferredFamily;
    std::wstring preferredSubfamily;
};

void UpdateSfntNameCandidate(SfntNameCandidate& candidate, const std::wstring& value, int score) {
    if (value.empty() || score < candidate.score) {
        return;
    }

    candidate.value = value;
    candidate.score = score;
}

int ComputeSfntNameScore(uint16_t platformId, uint16_t encodingId, uint16_t languageId) {
    int score = 0;
    if (platformId == 3) {
        score += 100;
    } else if (platformId == 0) {
        score += 80;
    } else if (platformId == 1) {
        score += 40;
    }

    if (languageId == 0x0409) {
        score += 20;
    } else if (languageId == 0) {
        score += 10;
    }

    if (encodingId == 1 || encodingId == 10 || encodingId == 0) {
        score += 5;
    }

    return score;
}

std::optional<ParsedSfntNames> TryReadSfntNames(const std::filesystem::path& path) {
    std::vector<uint8_t> bytes;
    if (!ReadFileBytes(path, bytes) || bytes.size() < 12) {
        return std::nullopt;
    }

    uint16_t numTables = 0;
    if (!TryReadBigEndianU16(bytes, 4, numTables)) {
        return std::nullopt;
    }

    uint32_t nameTableOffset = 0;
    uint32_t nameTableLength = 0;
    bool foundNameTable = false;
    for (uint16_t tableIndex = 0; tableIndex < numTables; ++tableIndex) {
        const size_t recordOffset = 12u + static_cast<size_t>(tableIndex) * 16u;
        uint32_t tag = 0;
        uint32_t offset = 0;
        uint32_t length = 0;
        if (!TryReadBigEndianU32(bytes, recordOffset, tag) ||
            !TryReadBigEndianU32(bytes, recordOffset + 8, offset) ||
            !TryReadBigEndianU32(bytes, recordOffset + 12, length)) {
            return std::nullopt;
        }

        if (tag == 0x6E616D65u) {
            nameTableOffset = offset;
            nameTableLength = length;
            foundNameTable = true;
            break;
        }
    }

    if (!foundNameTable || static_cast<size_t>(nameTableOffset) + static_cast<size_t>(nameTableLength) > bytes.size() || nameTableLength < 6) {
        return std::nullopt;
    }

    uint16_t recordCount = 0;
    uint16_t stringStorageOffset = 0;
    if (!TryReadBigEndianU16(bytes, nameTableOffset + 2, recordCount) ||
        !TryReadBigEndianU16(bytes, nameTableOffset + 4, stringStorageOffset)) {
        return std::nullopt;
    }

    const size_t recordsOffset = static_cast<size_t>(nameTableOffset) + 6u;
    const size_t stringsOffset = static_cast<size_t>(nameTableOffset) + static_cast<size_t>(stringStorageOffset);
    SfntNameCandidate family;
    SfntNameCandidate subfamily;
    SfntNameCandidate fullName;
    SfntNameCandidate preferredFamily;
    SfntNameCandidate preferredSubfamily;

    for (uint16_t recordIndex = 0; recordIndex < recordCount; ++recordIndex) {
        const size_t recordOffset = recordsOffset + static_cast<size_t>(recordIndex) * 12u;
        uint16_t platformId = 0;
        uint16_t encodingId = 0;
        uint16_t languageId = 0;
        uint16_t nameId = 0;
        uint16_t length = 0;
        uint16_t offset = 0;
        if (!TryReadBigEndianU16(bytes, recordOffset, platformId) ||
            !TryReadBigEndianU16(bytes, recordOffset + 2, encodingId) ||
            !TryReadBigEndianU16(bytes, recordOffset + 4, languageId) ||
            !TryReadBigEndianU16(bytes, recordOffset + 6, nameId) ||
            !TryReadBigEndianU16(bytes, recordOffset + 8, length) ||
            !TryReadBigEndianU16(bytes, recordOffset + 10, offset)) {
            return std::nullopt;
        }

        if (nameId != 1 && nameId != 2 && nameId != 4 && nameId != 16 && nameId != 17) {
            continue;
        }

        const std::wstring value = DecodeSfntNameString(bytes, stringsOffset + offset, length, platformId);
        if (value.empty()) {
            continue;
        }

        const int score = ComputeSfntNameScore(platformId, encodingId, languageId);
        switch (nameId) {
            case 1:
                UpdateSfntNameCandidate(family, value, score);
                break;
            case 2:
                UpdateSfntNameCandidate(subfamily, value, score);
                break;
            case 4:
                UpdateSfntNameCandidate(fullName, value, score);
                break;
            case 16:
                UpdateSfntNameCandidate(preferredFamily, value, score);
                break;
            case 17:
                UpdateSfntNameCandidate(preferredSubfamily, value, score);
                break;
        }
    }

    if (family.value.empty() && preferredFamily.value.empty() && fullName.value.empty()) {
        return std::nullopt;
    }

    return ParsedSfntNames{ family.value, subfamily.value, fullName.value, preferredFamily.value, preferredSubfamily.value };
}

bool IsRegularStyleName(std::wstring_view styleName) {
    return EqualsIgnoreCaseWide(styleName, L"regular") ||
           EqualsIgnoreCaseWide(styleName, L"normal") ||
           EqualsIgnoreCaseWide(styleName, L"roman") ||
           EqualsIgnoreCaseWide(styleName, L"book");
}

std::wstring SimplifyFontFamilyName(std::wstring familyName) {
    familyName = TrimWide(std::move(familyName));
    if (EndsWithIgnoreCaseWide(familyName, L" MT")) {
        familyName.resize(familyName.size() - 3);
        familyName = TrimWide(std::move(familyName));
    }

    return familyName;
}

std::string BuildDisplayNameFromSfntNames(const ParsedSfntNames& names) {
    std::wstring familyName = names.preferredFamily.empty() ? names.family : names.preferredFamily;
    std::wstring subfamilyName = names.preferredSubfamily.empty() ? names.subfamily : names.preferredSubfamily;
    std::wstring displayName;

    familyName = SimplifyFontFamilyName(std::move(familyName));
    if (!familyName.empty()) {
        displayName = familyName;
        subfamilyName = TrimWide(std::move(subfamilyName));
        if (!subfamilyName.empty() && !IsRegularStyleName(subfamilyName)) {
            displayName += L" ";
            displayName += subfamilyName;
        }
    } else {
        displayName = names.fullName;
    }

    displayName = NormalizeDisplayWide(std::move(displayName));
    return displayName.empty() ? std::string() : WideToUtf8(displayName);
}

std::string NormalizePathForComparison(const std::string& path) {
    if (path.empty()) {
        return path;
    }

    std::string normalized = path;
    try {
        normalized = WideToUtf8(Utf8Path(path).lexically_normal().wstring());
    } catch (const std::exception&) {
        normalized = path;
    }

    std::replace(normalized.begin(), normalized.end(), '/', '\\');
    while (normalized.size() >= 2 && normalized[0] == '.' && normalized[1] == '\\') {
        normalized.erase(0, 2);
    }
    while (normalized.size() > 1 && normalized.back() == '\\') {
        normalized.pop_back();
    }

    return normalized;
}

std::wstring GetWindowsFontsDirectory() {
    wchar_t windowsDirectory[MAX_PATH] = {};
    const UINT length = GetWindowsDirectoryW(windowsDirectory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return L"C:\\Windows\\Fonts";
    }

    return (std::filesystem::path(windowsDirectory) / "Fonts").wstring();
}

bool HasTtfExtension(const std::filesystem::path& path) {
    return EqualsIgnoreCase(WideToUtf8(path.extension().wstring()), ".ttf");
}

std::string BuildSystemFontDisplayName(const std::filesystem::path& path) {
    if (const std::optional<ParsedSfntNames> parsedNames = TryReadSfntNames(path)) {
        const std::string parsedDisplayName = BuildDisplayNameFromSfntNames(*parsedNames);
        if (!parsedDisplayName.empty()) {
            return parsedDisplayName;
        }
    }

    const std::wstring stem = NormalizeDisplayWide(path.stem().wstring());
    if (!stem.empty()) {
        return WideToUtf8(stem);
    }

    return WideToUtf8(NormalizeDisplayWide(path.filename().wstring()));
}

std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

void PopulateSystemFontAssets() {
    const std::filesystem::path fontsDirectory(GetWindowsFontsDirectory());
    std::error_code iteratorError;
    std::vector<SystemFontAsset> assets;

    for (std::filesystem::directory_iterator it(fontsDirectory, iteratorError), end; !iteratorError && it != end; it.increment(iteratorError)) {
        const std::filesystem::directory_entry& entry = *it;
        std::error_code statusError;
        if (!entry.is_regular_file(statusError) || statusError) {
            continue;
        }

        if (!HasTtfExtension(entry.path())) {
            continue;
        }

        assets.push_back({ BuildSystemFontDisplayName(entry.path()), WideToUtf8(entry.path().wstring()) });
    }

    if (iteratorError) {
        Log("WARNING: Failed to enumerate Windows fonts from {}: {}", WideToUtf8(fontsDirectory.wstring()), iteratorError.message());
    }

    std::sort(assets.begin(), assets.end(), [](const SystemFontAsset& left, const SystemFontAsset& right) {
        const std::string leftLabel = ToLowerAscii(left.displayName);
        const std::string rightLabel = ToLowerAscii(right.displayName);
        if (leftLabel != rightLabel) {
            return leftLabel < rightLabel;
        }

        return ToLowerAscii(left.path) < ToLowerAscii(right.path);
    });

    assets.erase(std::unique(assets.begin(), assets.end(), [](const SystemFontAsset& left, const SystemFontAsset& right) {
                     return PathsEqualIgnoreCase(left.path, right.path);
                 }),
                 assets.end());

    s_systemFontAssets = std::move(assets);
    Log("Loaded {} system .ttf fonts from {}.", s_systemFontAssets.size(), WideToUtf8(fontsDirectory.wstring()));
}

void EnsureSystemFontAssetsLoaded() {
    std::call_once(s_systemFontAssetsOnce, []() {
        PopulateSystemFontAssets();
    });
}

} // namespace

bool PathsEqualIgnoreCase(const std::string& left, const std::string& right) {
    return EqualsIgnoreCase(NormalizePathForComparison(left), NormalizePathForComparison(right));
}

namespace {

std::string BuildAbsoluteBundledFontPath(const std::filesystem::path& rootPath, const BundledFontAsset& asset) {
    return WideToUtf8((rootPath / Utf8Path(asset.relativePath)).wstring());
}

bool MatchesBundledFontPath(const BundledFontAsset& asset, const std::string& path, const std::wstring& toolscreenPath) {
    if (path.empty()) {
        return false;
    }

    if (PathsEqualIgnoreCase(path, asset.relativePath)) {
        return true;
    }

    const std::string filename = Utf8Path(asset.relativePath).filename().string();
    if (!filename.empty() && PathsEqualIgnoreCase(path, filename)) {
        return true;
    }

    if (!toolscreenPath.empty()) {
        const std::filesystem::path rootPath(toolscreenPath);
        if (PathsEqualIgnoreCase(path, BuildAbsoluteBundledFontPath(rootPath, asset))) {
            return true;
        }

        if (std::string_view(asset.id) == "minecraft") {
            const std::string legacyRootPath = WideToUtf8((rootPath / "Minecraft.ttf").wstring());
            if (PathsEqualIgnoreCase(path, legacyRootPath)) {
                return true;
            }
        }
    }

    return false;
}

bool WriteEmbeddedResourceToFile(WORD resourceId, const std::filesystem::path& destination, const void* moduleAnchor,
                                 bool overwriteExisting) {
    if (moduleAnchor == nullptr) {
        return false;
    }

    std::error_code directoryError;
    std::filesystem::create_directories(destination.parent_path(), directoryError);
    if (directoryError) {
        Log("WARNING: Failed to create bundled font directory: {}", WideToUtf8(destination.parent_path().wstring()));
        return false;
    }

    HMODULE moduleHandle = nullptr;
    const BOOL gotModule = GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(moduleAnchor), &moduleHandle);
    if (gotModule != TRUE || moduleHandle == nullptr) {
        Log("WARNING: Failed to resolve module handle while staging bundled font resource {}.", resourceId);
        return false;
    }

    HRSRC resourceHandle = FindResourceW(moduleHandle, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (resourceHandle == nullptr) {
        Log("WARNING: Failed to find bundled font resource {}.", resourceId);
        return false;
    }

    HGLOBAL resourceDataHandle = LoadResource(moduleHandle, resourceHandle);
    if (resourceDataHandle == nullptr) {
        Log("WARNING: Failed to load bundled font resource {}.", resourceId);
        return false;
    }

    const DWORD resourceSize = SizeofResource(moduleHandle, resourceHandle);
    const void* resourceData = LockResource(resourceDataHandle);
    if (resourceData == nullptr || resourceSize == 0) {
        Log("WARNING: Bundled font resource was empty: {}.", resourceId);
        return false;
    }

    const DWORD creationDisposition = overwriteExisting ? CREATE_ALWAYS : CREATE_NEW;
    HANDLE fileHandle = CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr, creationDisposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fileHandle == INVALID_HANDLE_VALUE) {
        const DWORD lastError = GetLastError();
        if (!overwriteExisting && lastError == ERROR_FILE_EXISTS) {
            return true;
        }

        Log("WARNING: Failed to open bundled font path for writing: {} (error {}).",
            WideToUtf8(destination.wstring()), lastError);
        return false;
    }

    DWORD written = 0;
    const BOOL wroteFile = WriteFile(fileHandle, resourceData, resourceSize, &written, nullptr);
    CloseHandle(fileHandle);
    if (wroteFile != TRUE || written != resourceSize) {
        Log("WARNING: Failed to stage bundled font resource to: {}", WideToUtf8(destination.wstring()));
        return false;
    }

    return true;
}

} // namespace

const std::vector<BundledFontAsset>& GetBundledFontAssets() {
    static const std::vector<BundledFontAsset> assets = {
        { "open-sans", "font.preset.open_sans", "fonts/OpenSans-Regular.ttf", IDR_OPENSANS_FONT },
        { "minecraft", "font.preset.minecraft", "fonts/Minecraft.ttf", IDR_MINECRAFT_FONT },
        { "monocraft", "font.preset.monocraft", "fonts/Monocraft.ttf", IDR_MONOCRAFT_FONT },
    };

    return assets;
}

const std::vector<SystemFontAsset>& GetSystemFontAssets() {
    EnsureSystemFontAssetsLoaded();
    return s_systemFontAssets;
}

void LoadSystemFontAssets() {
    EnsureSystemFontAssetsLoaded();
}

const BundledFontAsset* FindBundledFontAssetById(std::string_view id) {
    for (const BundledFontAsset& asset : GetBundledFontAssets()) {
        if (std::string_view(asset.id) == id) {
            return &asset;
        }
    }

    return nullptr;
}

const BundledFontAsset* FindBundledFontAssetByPath(const std::string& path, const std::wstring& toolscreenPath) {
    for (const BundledFontAsset& asset : GetBundledFontAssets()) {
        if (MatchesBundledFontPath(asset, path, toolscreenPath)) {
            return &asset;
        }
    }

    return nullptr;
}

std::string ResolveToolscreenRelativePath(const std::string& path, const std::wstring& toolscreenPath) {
    if (path.empty()) {
        return path;
    }

    const std::filesystem::path configuredPath = Utf8Path(path);
    if (configuredPath.is_absolute() || toolscreenPath.empty()) {
        return path;
    }

    return WideToUtf8((std::filesystem::path(toolscreenPath) / configuredPath).wstring());
}

std::string NormalizeBundledFontPath(const std::string& path, const std::wstring& toolscreenPath) {
    const BundledFontAsset* asset = FindBundledFontAssetByPath(path, toolscreenPath);
    if (asset == nullptr) {
        return path;
    }

    return asset->relativePath;
}

bool NormalizeBundledFontPathInPlace(std::string& path, const std::wstring& toolscreenPath) {
    const std::string normalizedPath = NormalizeBundledFontPath(path, toolscreenPath);
    if (normalizedPath == path) {
        return false;
    }

    path = normalizedPath;
    return true;
}

bool ExtractBundledFontAsset(const BundledFontAsset& asset, const std::filesystem::path& rootPath, const void* moduleAnchor,
                             bool overwriteExisting) {
    if (rootPath.empty()) {
        return false;
    }

    std::error_code existsError;
    const std::filesystem::path destination = rootPath / Utf8Path(asset.relativePath);
    if (!overwriteExisting && std::filesystem::exists(destination, existsError) && !existsError) {
        return true;
    }

    return WriteEmbeddedResourceToFile(asset.resourceId, destination, moduleAnchor, overwriteExisting);
}

void ExtractBundledFontAssets(const std::filesystem::path& rootPath, const void* moduleAnchor, bool overwriteExisting) {
    for (const BundledFontAsset& asset : GetBundledFontAssets()) {
        ExtractBundledFontAsset(asset, rootPath, moduleAnchor, overwriteExisting);
    }
}
