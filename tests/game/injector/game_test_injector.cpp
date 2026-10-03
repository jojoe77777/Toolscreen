// Injects DLLs into a running game process the same way the EasyInjectBundled watcher does
// (VirtualAllocEx + WriteProcessMemory + CreateRemoteThread(LoadLibraryW), 10 s wait, 100 ms gap),
// for the in-game Toolscreen tests. Unlike the watcher it checks each remote LoadLibraryW result.
//
// Usage: toolscreen_game_test_injector.exe --pid <pid> --dll <path> [--dll <path> ...]

#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

namespace {

constexpr DWORD kRemoteThreadTimeoutMs = 10000;
constexpr DWORD kDelayBetweenDllsMs = 100;

bool InjectDll(HANDLE process, const std::wstring& dllPath) {
    const SIZE_T pathBytes = (dllPath.size() + 1) * sizeof(wchar_t);
    LPVOID remoteMem = VirtualAllocEx(process, nullptr, pathBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) {
        std::fwprintf(stderr, L"VirtualAllocEx failed (%lu) for %ls\n", GetLastError(), dllPath.c_str());
        return false;
    }

    bool ok = false;
    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remoteMem, dllPath.c_str(), pathBytes, &written) || written != pathBytes) {
        std::fwprintf(stderr, L"WriteProcessMemory failed (%lu) for %ls\n", GetLastError(), dllPath.c_str());
    } else {
        const FARPROC loadLibrary = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
        DWORD threadId = 0;
        HANDLE thread = CreateRemoteThread(process, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLibrary), remoteMem, 0,
                                           &threadId);
        if (!thread) {
            std::fwprintf(stderr, L"CreateRemoteThread failed (%lu) for %ls\n", GetLastError(), dllPath.c_str());
        } else {
            const DWORD waitResult = WaitForSingleObject(thread, kRemoteThreadTimeoutMs);
            DWORD exitCode = 0;
            GetExitCodeThread(thread, &exitCode);
            CloseHandle(thread);
            if (waitResult != WAIT_OBJECT_0) {
                std::fwprintf(stderr, L"LoadLibraryW did not finish within %lu ms for %ls\n", kRemoteThreadTimeoutMs, dllPath.c_str());
            } else if (exitCode == 0) {
                // The exit code is the low 32 bits of the HMODULE; zero means LoadLibraryW failed.
                std::fwprintf(stderr, L"LoadLibraryW returned NULL in the target for %ls\n", dllPath.c_str());
            } else {
                std::fwprintf(stdout, L"Injected %ls\n", dllPath.c_str());
                ok = true;
            }
        }
    }

    VirtualFreeEx(process, remoteMem, 0, MEM_RELEASE);
    return ok;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    DWORD pid = 0;
    std::vector<std::wstring> dlls;
    for (int i = 1; i < argc; ++i) {
        if (std::wcscmp(argv[i], L"--pid") == 0 && i + 1 < argc) {
            pid = static_cast<DWORD>(std::wcstoul(argv[++i], nullptr, 10));
        } else if (std::wcscmp(argv[i], L"--dll") == 0 && i + 1 < argc) {
            wchar_t fullPath[MAX_PATH * 4] = {};
            if (GetFullPathNameW(argv[++i], static_cast<DWORD>(std::size(fullPath)), fullPath, nullptr) == 0) {
                std::fwprintf(stderr, L"Cannot resolve path %ls\n", argv[i]);
                return 2;
            }
            dlls.emplace_back(fullPath);
        } else {
            std::fwprintf(stderr, L"Usage: %ls --pid <pid> --dll <path> [--dll <path> ...]\n", argv[0]);
            return 2;
        }
    }
    if (pid == 0 || dlls.empty()) {
        std::fwprintf(stderr, L"Usage: %ls --pid <pid> --dll <path> [--dll <path> ...]\n", argv[0]);
        return 2;
    }

    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                                     PROCESS_VM_READ,
                                 FALSE, pid);
    if (!process) {
        std::fwprintf(stderr, L"OpenProcess(%lu) failed (%lu)\n", pid, GetLastError());
        return 1;
    }

    int failures = 0;
    for (const auto& dll : dlls) {
        if (!InjectDll(process, dll)) { ++failures; }
        Sleep(kDelayBetweenDllsMs);
    }
    CloseHandle(process);
    return failures == 0 ? 0 : 1;
}
