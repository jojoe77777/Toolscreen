#include "config_file_safety.h"

#include "common/profiler.h"
#include "common/utils.h"
#include "third_party/toml.hpp"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <sstream>

namespace {

constexpr size_t kMaxConfigSnapshots = 50;
constexpr auto kStaleTempFileAge = std::chrono::minutes(10);

std::mutex g_recoveryNoticesMutex;
std::vector<ConfigRecoveryNotice> g_recoveryNotices;

bool IsTransientFileError(const DWORD errorCode) {
    return errorCode == ERROR_ACCESS_DENIED ||
           errorCode == ERROR_LOCK_VIOLATION ||
           errorCode == ERROR_SHARING_VIOLATION;
}

template <typename Operation>
bool RetryFileOperation(Operation&& operation) {
    constexpr int kMaxAttempts = 60;
    constexpr DWORD kRetryDelayMs = 5;

    DWORD lastError = ERROR_SUCCESS;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        if (operation()) {
            return true;
        }

        lastError = GetLastError();
        if (!IsTransientFileError(lastError) || attempt == (kMaxAttempts - 1)) {
            SetLastError(lastError);
            return false;
        }

        Sleep(kRetryDelayMs);
    }

    SetLastError(lastError);
    return false;
}

bool DeletePathIfPresentWithRetries(const std::wstring& path) {
    return RetryFileOperation([&]() {
        if (DeleteFileW(path.c_str())) {
            return true;
        }

        const DWORD errorCode = GetLastError();
        if (errorCode == ERROR_FILE_NOT_FOUND || errorCode == ERROR_PATH_NOT_FOUND) {
            SetLastError(ERROR_SUCCESS);
            return true;
        }

        SetLastError(errorCode);
        return false;
    });
}

bool ReplacePathAtomically(const std::wstring& tempPath, const std::wstring& finalPath) {
    if (MovePathWithRetries(tempPath, finalPath, MOVEFILE_REPLACE_EXISTING)) {
        return true;
    }

    if (!DeletePathIfPresentWithRetries(finalPath)) {
        return false;
    }

    return MovePathWithRetries(tempPath, finalPath, 0);
}

// WriteFile + FlushFileBuffers, so the bytes are on disk before the rename that publishes them. Without the flush a
// power cut shortly after a save can leave a renamed file whose data blocks were never written (zero-length or NULs).
bool WriteBytesDurably(const std::wstring& path, const std::string& bytes) {
    HANDLE file = INVALID_HANDLE_VALUE;
    const bool opened = RetryFileOperation([&]() {
        file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        return file != INVALID_HANDLE_VALUE;
    });
    if (!opened) {
        return false;
    }

    bool ok = true;
    size_t offset = 0;
    while (ok && offset < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes.size() - offset, 1u << 20));
        DWORD written = 0;
        ok = WriteFile(file, bytes.data() + offset, chunk, &written, nullptr) && written == chunk;
        offset += written;
    }
    if (ok) {
        ok = FlushFileBuffers(file) == TRUE;
    }

    const DWORD lastError = GetLastError();
    CloseHandle(file);
    SetLastError(lastError);
    return ok;
}

bool WriteBytesAtomically(const std::wstring& path, const std::string& bytes) {
    const std::wstring tempPath = MakeTempSiblingPath(path, L".tmp-");
    if (!WriteBytesDurably(tempPath, bytes) || !ReplacePathAtomically(tempPath, path)) {
        const DWORD lastError = GetLastError();
        std::error_code cleanupError;
        std::filesystem::remove(std::filesystem::path(tempPath), cleanupError);
        SetLastError(lastError);
        return false;
    }
    return true;
}

enum class ReadResult { Ok, Missing, Failed };

ReadResult ReadFileBytes(const std::wstring& path, std::string& out) {
    out.clear();
    HANDLE file = INVALID_HANDLE_VALUE;
    const bool opened = RetryFileOperation([&]() {
        file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        return file != INVALID_HANDLE_VALUE;
    });
    if (!opened) {
        const DWORD error = GetLastError();
        return (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) ? ReadResult::Missing : ReadResult::Failed;
    }

    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) == TRUE && size.QuadPart >= 0 && size.QuadPart < (256ll << 20);
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        size_t offset = 0;
        while (ok && offset < out.size()) {
            DWORD read = 0;
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(out.size() - offset, 1u << 20));
            ok = ReadFile(file, out.data() + offset, chunk, &read, nullptr) == TRUE && read > 0;
            offset += read;
        }
    }
    CloseHandle(file);
    return ok ? ReadResult::Ok : ReadResult::Failed;
}

bool IsBlank(const std::string& source) {
    return std::all_of(source.begin(), source.end(), [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; });
}

bool IsValidUtf8(const std::string& source) {
    size_t i = 0;
    while (i < source.size()) {
        const unsigned char lead = static_cast<unsigned char>(source[i]);
        size_t length = 0;
        unsigned int codePoint = 0;
        if (lead < 0x80) {
            ++i;
            continue;
        } else if ((lead & 0xE0) == 0xC0) {
            length = 2;
            codePoint = lead & 0x1F;
        } else if ((lead & 0xF0) == 0xE0) {
            length = 3;
            codePoint = lead & 0x0F;
        } else if ((lead & 0xF8) == 0xF0) {
            length = 4;
            codePoint = lead & 0x07;
        } else {
            return false;
        }
        if (i + length > source.size()) return false;
        for (size_t k = 1; k < length; ++k) {
            const unsigned char next = static_cast<unsigned char>(source[i + k]);
            if ((next & 0xC0) != 0x80) return false;
            codePoint = (codePoint << 6) | (next & 0x3F);
        }
        // Reject overlong encodings, UTF-16 surrogates and values past U+10FFFF.
        static const unsigned int kMinimumForLength[] = { 0, 0, 0x80, 0x800, 0x10000 };
        if (codePoint < kMinimumForLength[length] || codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF)) {
            return false;
        }
        i += length;
    }
    return true;
}

// Returns why `source` cannot be a file anyone wrote by hand, or an empty string when it is plausible text.
// Empty files, NUL bytes, invalid UTF-8 and binary control bytes come from torn writes, disk damage or other
// programs, never from a person editing the file, so they are safe to replace without asking.
std::string DescribeDiskDamage(const std::string& source) {
    if (IsBlank(source)) {
        return "file is empty";
    }
    if (source.find('\0') != std::string::npos) {
        return "file contains NUL bytes";
    }
    if (!IsValidUtf8(source)) {
        return "file is not valid UTF-8 text";
    }
    const bool hasControlBytes = std::any_of(source.begin(), source.end(), [](unsigned char c) {
        return (c < 0x20 && c != '\t' && c != '\r' && c != '\n') || c == 0x7F;
    });
    if (hasControlBytes) {
        return "file contains binary data";
    }
    return std::string();
}

bool LooksLikeDiskDamage(const std::string& source) {
    return !DescribeDiskDamage(source).empty();
}

std::wstring FileNameOf(const std::wstring& path) {
    return std::filesystem::path(path).filename().wstring();
}

std::string DisplayNameFor(const std::wstring& path) {
    if (!g_toolscreenPath.empty()) {
        std::error_code error;
        const std::filesystem::path relative = std::filesystem::relative(std::filesystem::path(path), std::filesystem::path(g_toolscreenPath), error);
        if (!error && !relative.empty() && relative.native().rfind(L"..", 0) != 0) {
            return WideToUtf8(relative.wstring());
        }
    }
    return WideToUtf8(FileNameOf(path));
}

std::wstring LocalTimestampForFileName() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t buffer[32];
    swprintf_s(buffer, L"%04u%02u%02u-%02u%02u%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    return buffer;
}

// Renames the damaged file out of the way so restoring a backup does not destroy it.
std::wstring MoveDamagedFileAside(const std::wstring& path) {
    const std::wstring base = path + L".corrupt-" + LocalTimestampForFileName();
    std::wstring target = base;
    for (int suffix = 2; GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES && suffix < 100; ++suffix) {
        target = base + L"-" + std::to_wstring(suffix);
    }

    if (MovePathWithRetries(path, target, 0)) {
        return target;
    }
    Log("WARNING: Could not move damaged " + DisplayNameFor(path) + " aside (error " + std::to_string(GetLastError()) + ").");
    return std::wstring();
}

void AddRecoveryNotice(ConfigRecoveryNotice notice) {
    std::lock_guard<std::mutex> lock(g_recoveryNoticesMutex);
    for (const auto& existing : g_recoveryNotices) {
        if (existing.fileName == notice.fileName && existing.restoredFrom == notice.restoredFrom) {
            return;
        }
    }
    g_recoveryNotices.push_back(std::move(notice));
}

bool TryAccept(const TomlSourceAcceptor& accept, const std::string& source, std::string& error) {
    if (std::string damage = DescribeDiskDamage(source); !damage.empty()) {
        error = std::move(damage);
        return false;
    }
    try {
        return accept(source, error);
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

std::wstring GetConfigBackupDirectory() {
    return g_toolscreenPath + L"\\backups";
}

long long SnapshotTimestampFromFileName(const std::wstring& fileName) {
    // config_<unix seconds>.toml
    constexpr size_t kPrefixLength = 7;
    if (fileName.size() <= kPrefixLength) {
        return -1;
    }
    try {
        return std::stoll(fileName.substr(kPrefixLength));
    } catch (...) {
        return -1;
    }
}

} // namespace

std::wstring MakeTempSiblingPath(const std::wstring& finalPath, const wchar_t* suffix) {
    const std::filesystem::path final(finalPath);
    const std::wstring filename = final.filename().wstring();
    return (final.parent_path() /
            (filename + suffix + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())))
        .wstring();
}

bool MovePathWithRetries(const std::wstring& fromPath, const std::wstring& toPath, const unsigned long flags) {
    return RetryFileOperation([&]() {
        return MoveFileExW(fromPath.c_str(), toPath.c_str(), flags | MOVEFILE_WRITE_THROUGH);
    });
}

bool ParseTomlSource(const std::string& source, toml::table& outTable, std::string& error) {
#if TOML_EXCEPTIONS
    try {
        outTable = toml::parse(source);
        return true;
    } catch (const toml::parse_error& e) {
        error = std::string(e.description()) + " (line " + std::to_string(e.source().begin.line) + ", column " +
                std::to_string(e.source().begin.column) + ")";
        return false;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
#else
    toml::parse_result result = toml::parse(source);
    if (!result) {
        error = std::string(result.error().description()) + " (line " + std::to_string(result.error().source().begin.line) +
                ", column " + std::to_string(result.error().source().begin.column) + ")";
        return false;
    }
    outTable = std::move(result).table();
    return true;
#endif
}

std::wstring GetTomlBackupPath(const std::wstring& path) {
    return path + L".bak";
}

// Integrity check shared by the save and backup paths: not blank, no NUL bytes, parses as TOML.
static bool IsTomlSourceIntact(const std::string& source, std::string* outError = nullptr) {
    std::string error;
    const auto parses = [](const std::string& s, std::string& e) {
        toml::table tbl;
        return ParseTomlSource(s, tbl, e);
    };
    const bool intact = TryAccept(parses, source, error);
    if (!intact && outError) {
        *outError = error;
    }
    return intact;
}

bool WriteTomlFileSafely(const std::wstring& path, const std::function<bool(std::ostream&)>& writeContents) {
    std::ostringstream out;
    if (!writeContents(out) || !out.good()) {
        return false;
    }
    const std::string contents = out.str();

    // A serializer bug must not replace a loadable file with one the next launch cannot read.
    std::string validationError;
    if (!IsTomlSourceIntact(contents, &validationError)) {
        Log("ERROR: Refusing to save " + DisplayNameFor(path) + ": generated TOML is invalid (" + validationError + ").");
        return false;
    }

    std::string previous;
    if (ReadFileBytes(path, previous) == ReadResult::Ok) {
        if (previous == contents) {
            return true;
        }
        // Only an intact previous version is worth keeping. A damaged one would push the last good copy out of .bak.
        if (IsTomlSourceIntact(previous) && !WriteBytesAtomically(GetTomlBackupPath(path), previous)) {
            Log("WARNING: Could not update backup " + DisplayNameFor(GetTomlBackupPath(path)) + " (error " +
                std::to_string(GetLastError()) + ").");
        }
    }

    return WriteBytesAtomically(path, contents);
}

TomlFileLoadStatus LoadTomlFileWithRecovery(const std::wstring& path, const std::vector<std::wstring>& extraBackups,
                                            const TomlSourceAcceptor& accept, std::string* outError) {
    std::string source;
    const ReadResult readResult = ReadFileBytes(path, source);
    if (readResult == ReadResult::Missing) {
        return TomlFileLoadStatus::Missing;
    }

    std::string primaryError;
    if (readResult == ReadResult::Failed) {
        primaryError = "could not read file (error " + std::to_string(GetLastError()) + ")";
        // Do not treat a file we could not open (locked by another program) as damaged and move it aside.
        if (outError) *outError = primaryError;
        return TomlFileLoadStatus::Unrecoverable;
    }

    if (TryAccept(accept, source, primaryError)) {
        return TomlFileLoadStatus::Loaded;
    }

    const std::string displayName = DisplayNameFor(path);
    Log("WARNING: " + displayName + " is damaged or invalid: " + primaryError + ". Looking for a backup to restore.");

    std::vector<std::wstring> candidates;
    candidates.push_back(GetTomlBackupPath(path));
    candidates.insert(candidates.end(), extraBackups.begin(), extraBackups.end());

    for (const auto& candidate : candidates) {
        std::string backupSource;
        if (ReadFileBytes(candidate, backupSource) != ReadResult::Ok) {
            continue;
        }
        std::string backupError;
        if (!TryAccept(accept, backupSource, backupError)) {
            Log("WARNING: Backup " + DisplayNameFor(candidate) + " is not usable either: " + backupError);
            continue;
        }

        const std::wstring movedAside = MoveDamagedFileAside(path);
        if (movedAside.empty()) {
            // Restoring now would overwrite the only copy of the user's file.
            break;
        }
        if (!WriteBytesAtomically(path, backupSource)) {
            Log("ERROR: Could not restore " + displayName + " from " + DisplayNameFor(candidate) + " (error " +
                std::to_string(GetLastError()) + ").");
            MovePathWithRetries(movedAside, path, 0);
            break;
        }

        Log("Restored " + displayName + " from backup " + DisplayNameFor(candidate) + ". The damaged file was kept as " +
            DisplayNameFor(movedAside) + ".");
        AddRecoveryNotice({ displayName, WideToUtf8(FileNameOf(candidate)), WideToUtf8(FileNameOf(movedAside)) });
        return TomlFileLoadStatus::Recovered;
    }

    if (LooksLikeDiskDamage(source)) {
        // Nothing in an empty or binary file is worth keeping in place; let the caller start fresh.
        const std::wstring movedAside = MoveDamagedFileAside(path);
        if (!movedAside.empty()) {
            Log("WARNING: No usable backup for " + displayName + "; moved the damaged file aside as " + DisplayNameFor(movedAside) + ".");
            AddRecoveryNotice({ displayName, std::string(), WideToUtf8(FileNameOf(movedAside)) });
            return TomlFileLoadStatus::Missing;
        }
    }

    if (outError) *outError = primaryError;
    return TomlFileLoadStatus::Unrecoverable;
}

void RemoveStaleConfigTempFiles(const std::wstring& directory) {
    std::error_code error;
    const auto now = std::filesystem::file_time_type::clock::now();
    for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(directory), error)) {
        if (error) {
            break;
        }
        std::error_code entryError;
        if (!entry.is_regular_file(entryError) || entryError) {
            continue;
        }

        const std::wstring name = entry.path().filename().wstring();
        if (name.find(L".toml.tmp-") == std::wstring::npos && name.find(L".toml.rename-") == std::wstring::npos) {
            continue;
        }

        const auto modified = entry.last_write_time(entryError);
        if (entryError || now - modified < kStaleTempFileAge) {
            continue;
        }

        if (std::filesystem::remove(entry.path(), entryError)) {
            Log("Removed leftover temp file from an interrupted save: " + DisplayNameFor(entry.path().wstring()));
        }
    }
}

std::vector<std::wstring> ListConfigBackupsNewestFirst() {
    std::vector<std::pair<long long, std::wstring>> snapshots;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(GetConfigBackupDirectory()), error)) {
        if (error) {
            break;
        }
        std::error_code entryError;
        if (!entry.is_regular_file(entryError) || entryError) {
            continue;
        }
        const std::wstring name = entry.path().filename().wstring();
        if (name.rfind(L"config_", 0) != 0 || entry.path().extension() != L".toml") {
            continue;
        }
        snapshots.emplace_back(SnapshotTimestampFromFileName(name), entry.path().wstring());
    }

    std::sort(snapshots.begin(), snapshots.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first > b.first;
        return a.second > b.second;
    });

    std::vector<std::wstring> paths;
    paths.reserve(snapshots.size());
    for (auto& snapshot : snapshots) {
        paths.push_back(std::move(snapshot.second));
    }
    return paths;
}

void BackupConfigFile() {
    PROFILE_SCOPE_CAT("Config Backup", "IO Operations");

    if (g_toolscreenPath.empty()) {
        Log("Cannot backup config, toolscreen path is not available.");
        return;
    }

    const std::wstring configPath = g_toolscreenPath + L"\\config.toml";
    std::string contents;
    if (ReadFileBytes(configPath, contents) != ReadResult::Ok) {
        Log("Config file could not be read, skipping backup.");
        return;
    }
    if (!IsTomlSourceIntact(contents)) {
        Log("Config file is not valid TOML, skipping backup so older snapshots are kept.");
        return;
    }

    const std::wstring backupDir = GetConfigBackupDirectory();
    CreateDirectoryW(backupDir.c_str(), NULL);

    std::vector<std::wstring> snapshots = ListConfigBackupsNewestFirst();
    if (!snapshots.empty()) {
        std::string newest;
        if (ReadFileBytes(snapshots.front(), newest) == ReadResult::Ok && newest == contents) {
            return;
        }
    }

    const auto timestamp =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const std::wstring backupPath = backupDir + L"\\config_" + std::to_wstring(timestamp) + L".toml";
    if (!WriteBytesAtomically(backupPath, contents)) {
        Log("Failed to backup config file. Error code: " + std::to_string(GetLastError()));
        return;
    }
    Log("Config backed up to: " + WideToUtf8(backupPath));

    snapshots = ListConfigBackupsNewestFirst();
    for (size_t i = kMaxConfigSnapshots; i < snapshots.size(); ++i) {
        if (DeleteFileW(snapshots[i].c_str())) {
            Log("Deleted old backup: " + WideToUtf8(snapshots[i]));
        } else {
            Log("Failed to delete old backup: " + WideToUtf8(snapshots[i]));
        }
    }
}

std::vector<ConfigRecoveryNotice> GetConfigRecoveryNotices() {
    std::lock_guard<std::mutex> lock(g_recoveryNoticesMutex);
    return g_recoveryNotices;
}

void ClearConfigRecoveryNotices() {
    std::lock_guard<std::mutex> lock(g_recoveryNoticesMutex);
    g_recoveryNotices.clear();
}
