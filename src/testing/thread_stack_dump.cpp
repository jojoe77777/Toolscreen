#include "thread_stack_dump.h"

#include <windows.h>

#include <dbghelp.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace GameTest {
namespace {

constexpr size_t kMaxFrames = 48;

struct ThreadSample {
    DWORD threadId = 0;
    std::vector<DWORD64> frames;
};

std::string ModuleNameForAddress(DWORD64 address, DWORD64& outBase) {
    HMODULE module = NULL;
    outBase = 0;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module) ||
        module == NULL) {
        return {};
    }
    outBase = reinterpret_cast<DWORD64>(module);
    char path[MAX_PATH] = {};
    GetModuleFileNameA(module, path, MAX_PATH);
    const char* name = strrchr(path, '\\');
    return name ? name + 1 : path;
}

bool IsInterestingModule(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const char* const kInteresting[] = { "toolscreen", "vulkan-1", "graphics-hook", "nvoglv", "nvgpucomp", "amdvlk", "igvk" };
    for (const char* entry : kInteresting) {
        if (name.find(entry) != std::string::npos) return true;
    }
    return name.rfind("nv", 0) == 0 && name.find("vk") != std::string::npos;
}

#if defined(_M_X64)
// Walks a suspended thread with RtlVirtualUnwind. It allocates nothing, so it cannot deadlock on a heap or
// loader lock that the suspended thread holds. Stops at the first frame without unwind data (e.g. JIT code).
void WalkSuspendedThread(HANDLE thread, ThreadSample& sample) {
    CONTEXT context = {};
    context.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread, &context)) return;

    sample.frames.reserve(kMaxFrames);
    for (size_t i = 0; i < kMaxFrames && context.Rip != 0; ++i) {
        sample.frames.push_back(context.Rip);
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
        if (!function) {
            if (i != 0) break;
            // A leaf function at the top of the stack: its return address is at RSP.
            context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
            context.Rsp += 8;
            continue;
        }
        PVOID handlerData = nullptr;
        DWORD64 establisherFrame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData, &establisherFrame, nullptr);
    }
}
#endif

std::once_flag s_symbolsOnce;

void EnsureSymbols() {
    std::call_once(s_symbolsOnce, [] {
        // Search beside Toolscreen.dll, where the test script stages Toolscreen.pdb.
        char modulePath[MAX_PATH] = {};
        HMODULE self = NULL;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&EnsureSymbols), &self);
        GetModuleFileNameA(self, modulePath, MAX_PATH);
        if (char* slash = strrchr(modulePath, '\\')) *slash = '\0';

        SymSetOptions(SymGetOptions() | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS);
        if (!SymInitialize(GetCurrentProcess(), modulePath, TRUE)) {
            SymSetSearchPath(GetCurrentProcess(), modulePath);
            SymRefreshModuleList(GetCurrentProcess());
        }
    });
}

std::string Symbolize(DWORD64 address) {
    std::ostringstream out;
    DWORD64 moduleBase = 0;
    const std::string module = ModuleNameForAddress(address, moduleBase);
    out << (module.empty() ? "?" : module) << "+0x" << std::hex << (address - moduleBase);

    char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
    auto* symbol = reinterpret_cast<PSYMBOL_INFO>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;
    DWORD64 displacement = 0;
    if (SymFromAddr(GetCurrentProcess(), address, &displacement, symbol)) {
        out << " " << symbol->Name << "+0x" << std::hex << displacement;
        IMAGEHLP_LINE64 line = {};
        line.SizeOfStruct = sizeof(line);
        DWORD lineDisplacement = 0;
        if (SymGetLineFromAddr64(GetCurrentProcess(), address, &lineDisplacement, &line)) {
            const char* file = strrchr(line.FileName, '\\');
            out << " [" << (file ? file + 1 : line.FileName) << ":" << std::dec << line.LineNumber << "]";
        }
    }
    return out.str();
}

} // namespace

std::string DumpInterestingThreadStacks() {
#if !defined(_M_X64)
    return "Thread stack dumps are only implemented for x64.";
#else
    std::vector<DWORD> threadIds;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return "CreateToolhelp32Snapshot failed.";
    THREADENTRY32 entry = { sizeof(entry) };
    const DWORD pid = GetCurrentProcessId();
    const DWORD self = GetCurrentThreadId();
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID == pid && entry.th32ThreadID != self) threadIds.push_back(entry.th32ThreadID);
    }
    CloseHandle(snapshot);

    // Sample every thread first (each suspended only while its own frames are recorded), then symbolize.
    std::vector<ThreadSample> samples;
    samples.reserve(threadIds.size());
    for (DWORD threadId : threadIds) {
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, threadId);
        if (!thread) continue;
        ThreadSample sample;
        sample.threadId = threadId;
        if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
            WalkSuspendedThread(thread, sample);
            ResumeThread(thread);
        }
        CloseHandle(thread);
        samples.push_back(std::move(sample));
    }

    EnsureSymbols();
    std::ostringstream report;
    size_t reported = 0;
    for (const ThreadSample& sample : samples) {
        const bool interesting = std::any_of(sample.frames.begin(), sample.frames.end(), [](DWORD64 frame) {
            DWORD64 base = 0;
            return IsInterestingModule(ModuleNameForAddress(frame, base));
        });
        if (!interesting) continue;
        ++reported;
        report << "Thread " << sample.threadId << ":\n";
        for (size_t i = 0; i < sample.frames.size(); ++i) {
            report << "  [" << i << "] " << Symbolize(sample.frames[i]) << "\n";
        }
    }
    if (reported == 0) report << "No thread had a frame in an interesting module.\n";
    return report.str();
#endif
}

} // namespace GameTest
