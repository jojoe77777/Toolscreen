#pragma once

#include "third_party/toml.hpp"

#include <functional>
#include <ostream>
#include <string>
#include <vector>

// Writes to a temp file next to `path`, flushes it to disk, then replaces `path` with it, so a crash or power cut
// mid-save cannot leave a truncated file. The new contents must parse as TOML before anything on disk changes, and
// the previous contents of `path` are kept as `<path>.bak` when they were intact. Skips the write when `path`
// already holds the same bytes.
bool WriteTomlFileSafely(const std::wstring& path, const std::function<bool(std::ostream&)>& writeContents);

std::wstring GetTomlBackupPath(const std::wstring& path);

// MoveFileExW that retries while antivirus, indexers or another instance briefly hold the file open.
bool MovePathWithRetries(const std::wstring& fromPath, const std::wstring& toPath, unsigned long flags);

std::wstring MakeTempSiblingPath(const std::wstring& finalPath, const wchar_t* suffix);

// Returns true when `source` is usable; otherwise returns false and sets `error` to the reason.
using TomlSourceAcceptor = std::function<bool(const std::string& source, std::string& error)>;

enum class TomlFileLoadStatus {
    Loaded,        // `path` was intact.
    Recovered,     // `path` was damaged; a backup was accepted and written back to `path`.
    Missing,       // `path` does not exist, or was blank or binary with no usable backup and has been moved aside.
    Unrecoverable, // `path` holds content `accept` rejected and no backup was usable; `path` is left untouched.
};

// Reads `path` and passes its contents to `accept`. A file that is blank, binary (NUL or control bytes, invalid
// UTF-8), or that `accept` rejects counts as damaged: it is renamed to `<name>.corrupt-<timestamp>` and
// `<path>.bak`, then each of `extraBackups` in order, is offered to `accept`. The first accepted backup is restored
// to `path`. With no usable backup, a blank or binary file is moved aside and reported as Missing, while a file
// that still holds readable text (most often a hand-edit typo) stays in place so the user can fix it.
TomlFileLoadStatus LoadTomlFileWithRecovery(const std::wstring& path, const std::vector<std::wstring>& extraBackups,
                                            const TomlSourceAcceptor& accept, std::string* outError = nullptr);

bool ParseTomlSource(const std::string& source, toml::table& outTable, std::string& error);

// Deletes `*.tmp-*` and `*.rename-*` leftovers in `directory` from saves interrupted by a crash. Files younger than
// a few minutes are kept because another game instance may still be writing them.
void RemoveStaleConfigTempFiles(const std::wstring& directory);

// Snapshots config.toml into `<toolscreen>/backups/config_<unix time>.toml` when its content is intact and differs
// from the newest snapshot, keeping the 50 newest.
void BackupConfigFile();

// Snapshot files from BackupConfigFile, newest first.
std::vector<std::wstring> ListConfigBackupsNewestFirst();

struct ConfigRecoveryNotice {
    std::string fileName;      // File that was damaged, relative to the toolscreen folder.
    std::string restoredFrom;  // Backup file name that replaced it; empty when nothing could be restored.
    std::string movedAsideAs;  // Name the damaged file now has; empty when it was left in place.
};

std::vector<ConfigRecoveryNotice> GetConfigRecoveryNotices();
void ClearConfigRecoveryNotices();
