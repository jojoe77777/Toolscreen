#include <windows.h>
#include <ShlObj.h>
#include <Shlwapi.h>

#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

static std::wstring Utf8ToWide(const std::string& utf8_string) {
    if (utf8_string.empty()) return std::wstring();
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &utf8_string[0], (int)utf8_string.size(), NULL, 0);
    std::wstring wstr_to(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &utf8_string[0], (int)utf8_string.size(), &wstr_to[0], size_needed);
    return wstr_to;
}

static std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

std::string SanitizePathForDisplay(const std::string& path);
std::wstring SanitizePathForDisplay(const std::wstring& path);

#include "common/path_sanitize.inl"

namespace {

int g_failures = 0;

void CheckStrEq(const std::string& actual, const std::string& expected, const std::string& label) {
    if (actual != expected) {
        std::cerr << "  ASSERT FAILED: " << label << " expected \"" << expected << "\" got \"" << actual << "\"\n";
        ++g_failures;
    }
}

void HomePrefixCollapsesToTilde() {
    auto out = SanitizePathForDisplayImpl(L"C:\\Users\\testUsername\\AppData\\foo\\image.png", L"C:\\Users\\testUsername");
    CheckStrEq(WideToUtf8(out), "~\\AppData\\foo\\image.png", "home prefix");
}

void HomePrefixExactMatchCollapses() {
    auto out = SanitizePathForDisplayImpl(L"C:\\Users\\testUsername", L"C:\\Users\\testUsername");
    CheckStrEq(WideToUtf8(out), "~", "home prefix exact");
}

void MidStringUsersOccurrenceMasked() {
    auto out = SanitizePathForDisplayImpl(L"failed to open C:\\Users\\testUsername\\config.toml here", L"");
    CheckStrEq(WideToUtf8(out), "failed to open ~\\config.toml here", "mid-string occurrence");
}

void CaseInsensitiveOnDriveAndUsers() {
    auto out = SanitizePathForDisplayImpl(L"c:\\users\\testUsername\\AppData\\img.png", L"C:\\Users\\testUsername");
    CheckStrEq(WideToUtf8(out), "~\\AppData\\img.png", "case-insensitive prefix");

    auto out2 = SanitizePathForDisplayImpl(L"see C:\\USERS\\testUsername\\a.txt", L"");
    CheckStrEq(WideToUtf8(out2), "see ~\\a.txt", "case-insensitive mid-string");
}

void NonHomePathUntouched() {
    auto out = SanitizePathForDisplayImpl(L"D:\\Games\\Minecraft\\saves\\world", L"C:\\Users\\testUsername");
    CheckStrEq(WideToUtf8(out), "D:\\Games\\Minecraft\\saves\\world", "non-home passthrough");
}

void EmptyStringPassthrough() {
    auto out = SanitizePathForDisplayImpl(L"", L"C:\\Users\\testUsername");
    CheckStrEq(WideToUtf8(out), "", "empty string");
}

void HomePrefixDoesNotPartialMatchSegment() {
    auto out = SanitizePathForDisplayImpl(L"C:\\Users\\testUsernameLong\\file.png", L"C:\\Users\\zzz");
    CheckStrEq(WideToUtf8(out), "~\\file.png", "different user segment still masked by Users rule");
}

void Utf8OverloadUsesResolvedHome() {
    auto out = SanitizePathForDisplay(std::string("C:\\Users\\testUsername\\AppData\\image.png"));
    CheckStrEq(out, "~\\AppData\\image.png", "utf8 overload resolves USERPROFILE");
}

void FileNameFromWindowsPath() {
    CheckStrEq(FileNameForDisplay("C:\\Users\\testUsername\\AppData\\foo\\image.png"), "image.png", "windows path filename");
}

void FileNameFromForwardSlashPath() {
    CheckStrEq(FileNameForDisplay("assets/images/icons/cursor.png"), "cursor.png", "forward-slash filename");
}

void FileNameNoSeparatorPassthrough() {
    CheckStrEq(FileNameForDisplay("image.png"), "image.png", "bare filename passthrough");
}

void FileNameEmptyPassthrough() {
    CheckStrEq(FileNameForDisplay(""), "", "empty filename");
}

struct TestCase {
    const char* name;
    std::function<void()> run;
};

const std::vector<TestCase>& Registry() {
    static const std::vector<TestCase> cases = {
        {"home_prefix_collapses_to_tilde", &HomePrefixCollapsesToTilde},
        {"home_prefix_exact_match_collapses", &HomePrefixExactMatchCollapses},
        {"mid_string_users_occurrence_masked", &MidStringUsersOccurrenceMasked},
        {"case_insensitive_on_drive_and_users", &CaseInsensitiveOnDriveAndUsers},
        {"non_home_path_untouched", &NonHomePathUntouched},
        {"empty_string_passthrough", &EmptyStringPassthrough},
        {"home_prefix_does_not_partial_match_segment", &HomePrefixDoesNotPartialMatchSegment},
        {"utf8_overload_uses_resolved_home", &Utf8OverloadUsesResolvedHome},
        {"filename_from_windows_path", &FileNameFromWindowsPath},
        {"filename_from_forward_slash_path", &FileNameFromForwardSlashPath},
        {"filename_no_separator_passthrough", &FileNameNoSeparatorPassthrough},
        {"filename_empty_passthrough", &FileNameEmptyPassthrough},
    };
    return cases;
}

int RunNamed(const std::string& name) {
    for (const auto& testCase : Registry()) {
        if (name == testCase.name) {
            g_failures = 0;
            std::cout << "RUN " << name << '\n';
            testCase.run();
            if (g_failures == 0) {
                std::cout << "PASS " << name << '\n';
                return 0;
            }
            std::cerr << "FAIL " << name << " (" << g_failures << " assertion(s))\n";
            return 1;
        }
    }
    std::cerr << "Unknown test case: " << name << '\n';
    return 2;
}

int RunAll() {
    int failed = 0;
    for (const auto& testCase : Registry()) {
        if (RunNamed(testCase.name) != 0) ++failed;
    }
    return failed == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    _putenv_s("USERPROFILE", "C:\\Users\\testUsername");

    if (argc == 1 || (argc == 2 && std::strcmp(argv[1], "--run-all") == 0)) {
        return RunAll();
    }
    if (argc == 2 && std::strcmp(argv[1], "--list") == 0) {
        for (const auto& testCase : Registry()) std::cout << testCase.name << '\n';
        return 0;
    }
    if (argc == 3 && std::strcmp(argv[1], "--run") == 0) {
        return RunNamed(argv[2]);
    }
    std::cerr << "Usage: " << argv[0] << " [--run <case> | --run-all | --list]\n";
    return 2;
}
