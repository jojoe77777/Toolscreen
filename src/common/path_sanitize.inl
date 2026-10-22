namespace {

wchar_t NormalizeSlash(wchar_t ch) { return ch == L'/' ? L'\\' : ch; }

bool EqualsFoldedSlash(wchar_t a, wchar_t b) {
    return towlower(NormalizeSlash(a)) == towlower(NormalizeSlash(b));
}

bool MatchesPrefixFolded(const std::wstring& path, size_t at, const std::wstring& prefix) {
    if (at + prefix.size() > path.size()) { return false; }
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (!EqualsFoldedSlash(path[at + i], prefix[i])) { return false; }
    }
    return true;
}

std::wstring ResolveUserHomeDir() {
    std::wstring home;

    size_t required = 0;
    wchar_t envValue[MAX_PATH];
    if (_wgetenv_s(&required, envValue, MAX_PATH, L"USERPROFILE") == 0 && required > 1) {
        home.assign(envValue, required - 1);
    }

    if (home.empty()) {
        PWSTR profilePath = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &profilePath)) && profilePath) {
            home.assign(profilePath);
        }
        if (profilePath) { CoTaskMemFree(profilePath); }
    }

    while (!home.empty() && (home.back() == L'\\' || home.back() == L'/')) { home.pop_back(); }
    return home;
}

const std::wstring& CachedUserHomeDir() {
    static const std::wstring home = ResolveUserHomeDir();
    return home;
}

std::wstring SanitizePathForDisplayImpl(const std::wstring& path, const std::wstring& homeDir) {
    if (path.empty()) { return path; }

    static const std::wstring kUsersSegment = L"\\Users\\";

    std::wstring result;
    result.reserve(path.size());

    size_t i = 0;
    while (i < path.size()) {
        if (!homeDir.empty() && MatchesPrefixFolded(path, i, homeDir)) {
            const size_t after = i + homeDir.size();
            if (after == path.size() || path[after] == L'\\' || path[after] == L'/') {
                result.push_back(L'~');
                i = after;
                continue;
            }
        }

        if (i + 1 < path.size() && path[i + 1] == L':' && iswalpha(path[i]) &&
            MatchesPrefixFolded(path, i + 2, kUsersSegment)) {
            size_t cursor = i + 2 + kUsersSegment.size();
            while (cursor < path.size() && path[cursor] != L'\\' && path[cursor] != L'/') { ++cursor; }
            result.push_back(L'~');
            i = cursor;
            continue;
        }

        result.push_back(path[i]);
        ++i;
    }

    return result;
}

} // namespace

std::wstring SanitizePathForDisplay(const std::wstring& path) {
    return SanitizePathForDisplayImpl(path, CachedUserHomeDir());
}

std::string SanitizePathForDisplay(const std::string& path) {
    return WideToUtf8(SanitizePathForDisplay(Utf8ToWide(path)));
}

std::string FileNameForDisplay(const std::string& path) {
    if (path.empty()) { return path; }
    const size_t pos = path.find_last_of("/\\");
    if (pos == std::string::npos) { return path; }
    return path.substr(pos + 1);
}
