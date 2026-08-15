#include "hook_chain.h"

#include "MinHook.h"
#include "common/utils.h"

#include <Psapi.h>
#include <winver.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "Version.lib")


extern Config g_config;

typedef BOOL(WINAPI* WGLSWAPBUFFERS)(HDC);
extern WGLSWAPBUFFERS owglSwapBuffers;
extern std::atomic<WGLSWAPBUFFERS> g_owglSwapBuffersThirdParty;
extern std::atomic<void*> g_wglSwapBuffersThirdPartyHookTarget;
extern BOOL WINAPI hkwglSwapBuffers(HDC hDc);
extern BOOL WINAPI hkwglSwapBuffers_ThirdParty(HDC hDc);


namespace {

struct HookChainOwnerInfo {
    std::wstring path;
    std::wstring name;
    std::wstring company;
    std::wstring product;
    std::wstring description;
    uintptr_t base = 0;
    size_t size = 0;
};

std::mutex g_wglSwapBuffersThirdPartyHookMutex;
std::atomic<void*> g_lastSkippedWglSwapBuffersStart{ nullptr };
std::atomic<void*> g_lastSkippedWglSwapBuffersTarget{ nullptr };

struct ScopedModulePin {
    HMODULE module = NULL;

    ScopedModulePin() = default;
    ScopedModulePin(const ScopedModulePin&) = delete;
    ScopedModulePin& operator=(const ScopedModulePin&) = delete;

    ~ScopedModulePin() {
        if (module) {
            FreeLibrary(module);
        }
    }

    bool PinForAddress(const void* addr) {
        if (!addr) return false;
        if (module) {
            FreeLibrary(module);
            module = NULL;
        }

        return GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCSTR>(addr), &module) && module;
    }

    explicit operator bool() const { return module != NULL; }
};

static std::string PtrToHex(const void* p) {
    std::ostringstream ss;
    ss << "0x" << std::hex << std::uppercase << (uintptr_t)p;
    return ss.str();
}

static bool IsReadableCodePtr(const void* addr) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(addr, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    DWORD prot = mbi.Protect & 0xFF;
    return prot == PAGE_EXECUTE || prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY ||
           prot == PAGE_READONLY || prot == PAGE_READWRITE || prot == PAGE_WRITECOPY;
}

static bool IsAbsoluteJumpStub(const uint8_t* bytes) {
    if (!bytes) return false;
    return (bytes[0] == 0xEB) || (bytes[0] == 0xE9) || (bytes[0] == 0xFF && bytes[1] == 0x25) ||
           (bytes[0] == 0x48 && bytes[1] == 0xB8 && bytes[10] == 0xFF && bytes[11] == 0xE0) ||
           (bytes[0] == 0x49 && bytes[1] == 0xBB && bytes[10] == 0x41 && bytes[11] == 0xFF && bytes[12] == 0xE3);
}

static std::wstring ToLowerWide(std::wstring value) {
    for (wchar_t& ch : value) ch = static_cast<wchar_t>(towlower(ch));
    return value;
}

static bool ContainsAnySubstring(const std::wstring& haystack, std::initializer_list<const wchar_t*> needles) {
    for (const wchar_t* needle : needles) {
        if (needle && haystack.find(needle) != std::wstring::npos) {
            return true;
        }
    }
    return false;
}

static std::wstring GetFileVersionStringValue(const std::wstring& filePath, const wchar_t* key) {
    if (filePath.empty() || !key) return L"";

    DWORD handle = 0;
    DWORD sz = GetFileVersionInfoSizeW(filePath.c_str(), &handle);
    if (sz == 0) return L"";

    std::vector<uint8_t> data(sz);
    if (!GetFileVersionInfoW(filePath.c_str(), 0, sz, data.data())) return L"";

    struct LANGANDCODEPAGE {
        WORD wLanguage;
        WORD wCodePage;
    };

    LANGANDCODEPAGE* lpTranslate = nullptr;
    UINT cbTranslate = 0;
    if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", (LPVOID*)&lpTranslate, &cbTranslate) && lpTranslate &&
        cbTranslate >= sizeof(LANGANDCODEPAGE)) {
        wchar_t subBlock[256];
        swprintf_s(subBlock, L"\\StringFileInfo\\%04x%04x\\%s", lpTranslate[0].wLanguage, lpTranslate[0].wCodePage, key);
        LPVOID buf = nullptr;
        UINT bufLen = 0;
        if (VerQueryValueW(data.data(), subBlock, &buf, &bufLen) && buf && bufLen > 0) {
            return std::wstring(reinterpret_cast<const wchar_t*>(buf));
        }
    }

    {
        wchar_t subBlock[256];
        swprintf_s(subBlock, L"\\StringFileInfo\\040904B0\\%s", key);
        LPVOID buf = nullptr;
        UINT bufLen = 0;
        if (VerQueryValueW(data.data(), subBlock, &buf, &bufLen) && buf && bufLen > 0) {
            return std::wstring(reinterpret_cast<const wchar_t*>(buf));
        }
    }
    return L"";
}

static bool GetOwnerInfoForAddress(const void* addr, HookChainOwnerInfo& out) {
    out = HookChainOwnerInfo{};
    if (!addr) return false;

    HMODULE hMod = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &hMod) ||
        !hMod) {
        return false;
    }

    WCHAR pathBuf[MAX_PATH] = { 0 };
    DWORD n = GetModuleFileNameW(hMod, pathBuf, (DWORD)std::size(pathBuf));
    if (n > 0) {
        out.path.assign(pathBuf, pathBuf + n);
        try {
            out.name = std::filesystem::path(out.path).filename().wstring();
        } catch (...) {
            out.name = out.path;
        }
        out.company = GetFileVersionStringValue(out.path, L"CompanyName");
        out.product = GetFileVersionStringValue(out.path, L"ProductName");
        out.description = GetFileVersionStringValue(out.path, L"FileDescription");
    }

    MODULEINFO mi{};
    if (GetModuleInformation(GetCurrentProcess(), hMod, &mi, sizeof(mi))) {
        out.base = (uintptr_t)mi.lpBaseOfDll;
        out.size = (size_t)mi.SizeOfImage;
    } else {
        out.base = (uintptr_t)hMod;
    }

    return true;
}

static bool IsToolscreenHookOwner(const HookChainOwnerInfo& ownerInfo) {
    const std::wstring ownerNameLower = ToLowerWide(ownerInfo.name);
    const std::wstring ownerPathLower = ToLowerWide(ownerInfo.path);

    return ownerNameLower == L"toolscreen.dll" || ContainsAnySubstring(ownerPathLower, { L"\\toolscreen.dll" });
}

static bool IsAddressInSet(const void* addr, std::initializer_list<const void*> addrs) {
    for (const void* candidate : addrs) {
        if (candidate && addr == candidate) {
            return true;
        }
    }
    return false;
}

static bool TryResolveJumpTarget(void* cur, void*& next) {
    next = nullptr;
    if (!cur || !IsReadableCodePtr(cur)) return false;

    const uint8_t* b = reinterpret_cast<const uint8_t*>(cur);

    if (b[0] == 0xEB) {
        int8_t rel = *reinterpret_cast<const int8_t*>(b + 1);
        next = const_cast<uint8_t*>(b + 2 + rel);
    } else if (b[0] == 0xE9) {
        int32_t rel = *reinterpret_cast<const int32_t*>(b + 1);
        next = const_cast<uint8_t*>(b + 5 + rel);
    } else if (b[0] == 0xFF && b[1] == 0x25) {
        int32_t disp = *reinterpret_cast<const int32_t*>(b + 2);
        const uint8_t* ripNext = b + 6;
        const uint8_t* slot = ripNext + disp;
        if (!IsReadableCodePtr(slot)) return false;
        next = *reinterpret_cast<void* const*>(slot);
    } else if (b[0] == 0x48 && b[1] == 0xB8 && b[10] == 0xFF && b[11] == 0xE0) {
        next = *reinterpret_cast<void* const*>(b + 2);
    } else if (b[0] == 0x49 && b[1] == 0xBB && b[10] == 0x41 && b[11] == 0xFF && b[12] == 0xE3) {
        next = *reinterpret_cast<void* const*>(b + 2);
    }

    return next != nullptr;
}

static void* ResolveFirstAllowedSwapBuffersHookTarget(void* start, bool allowDirectStart, std::initializer_list<const void*> excludedTargets) {
    if (!start) return nullptr;

    void* cur = start;
    for (int depth = 0; depth < 16; depth++) {
        if (!cur) return nullptr;

        if (allowDirectStart || depth > 0) {
            if (!IsAddressInSet(cur, excludedTargets) && HookChain::IsAllowedSwapBuffersThirdPartyHookAddress(cur)) {
                return cur;
            }
        }

        if (!IsReadableCodePtr(cur)) return nullptr;
        if (!IsAbsoluteJumpStub(reinterpret_cast<const uint8_t*>(cur))) return nullptr;

        void* next = nullptr;
        if (!TryResolveJumpTarget(cur, next)) return nullptr;

        if (IsAddressInSet(next, excludedTargets)) {
            return nullptr;
        }

        cur = next;
    }

    return nullptr;
}

static void LogSkippedDisallowedHookTarget(const char* apiName, void* startAddress, void* skippedTarget) {
    if (!apiName || !startAddress || !skippedTarget) return;

    if (HookChain::IsAllowedSwapBuffersThirdPartyHookAddress(skippedTarget)) return;

    void* lastStart = g_lastSkippedWglSwapBuffersStart.load(std::memory_order_acquire);
    void* lastTarget = g_lastSkippedWglSwapBuffersTarget.load(std::memory_order_acquire);
    if (lastStart == startAddress && lastTarget == skippedTarget) {
        return;
    }

    g_lastSkippedWglSwapBuffersStart.store(startAddress, std::memory_order_release);
    g_lastSkippedWglSwapBuffersTarget.store(skippedTarget, std::memory_order_release);

    const char* action = "skipping unchainable hook target";
    HookChainOwnerInfo ownerInfo{};
    if (GetOwnerInfoForAddress(skippedTarget, ownerInfo) && IsToolscreenHookOwner(ownerInfo)) {
        action = "Skipping chaining our own hook";
    }

    LogCategory("hookchain",
                std::string("[") + apiName + "] " + action + " start=" +
                    HookChain::DescribeAddressWithOwner(startAddress) + " target=" + HookChain::DescribeAddressWithOwner(skippedTarget));
}

static void LogSkippedStaleHookTarget(const char* apiName, void* target, const char* reason) {
    if (!apiName || !target) return;

    LogCategory("hookchain",
                std::string("[") + apiName + "] skipping stale third-party hook target " + HookChain::DescribeAddressWithOwner(target) +
                    " reason=" + (reason ? reason : "unknown"));
}

static bool IsLiveThirdPartyWglSwapBuffersHookTarget(void* target) {
    if (!target) return false;

    ScopedModulePin pinnedModule;
    if (!pinnedModule.PinForAddress(target)) {
        return false;
    }

    return IsReadableCodePtr(target);
}

static void ClearStaleThirdPartyWglSwapBuffersHookState(void* staleTarget, const char* reason) {
    void* expected = staleTarget;
    if (!g_wglSwapBuffersThirdPartyHookTarget.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel)) {
        return;
    }

    g_owglSwapBuffersThirdParty.store(nullptr, std::memory_order_release);
    g_lastSkippedWglSwapBuffersStart.store(nullptr, std::memory_order_release);
    g_lastSkippedWglSwapBuffersTarget.store(nullptr, std::memory_order_release);
    LogSkippedStaleHookTarget("wglSwapBuffers", staleTarget, reason);
}

static void* ResolveAbsoluteJumpTarget(void* p) {
    if (!p) return nullptr;

    void* cur = p;
    for (int depth = 0; depth < 8; depth++) {
        void* next = nullptr;

        if (!TryResolveJumpTarget(cur, next)) return nullptr;
        if (!IsReadableCodePtr(next)) return next;
        if (!IsAbsoluteJumpStub(reinterpret_cast<const uint8_t*>(next))) return next;

        cur = next;
    }

    return nullptr;
}

static void* TraceAbsoluteJumpTarget(void* p, std::vector<std::string>& outTraceLines) {
    outTraceLines.clear();
    if (!p) return nullptr;

    void* cur = p;
    for (int depth = 0; depth < 16; depth++) {
        if (!IsReadableCodePtr(cur)) {
            outTraceLines.push_back("depth=" + std::to_string(depth) + " unreadable @" + PtrToHex(cur));
            return nullptr;
        }

        const uint8_t* b = reinterpret_cast<const uint8_t*>(cur);
        void* next = nullptr;
        const char* kind = nullptr;

        if (b[0] == 0xEB) {
            int8_t rel = *reinterpret_cast<const int8_t*>(b + 1);
            next = const_cast<uint8_t*>(b + 2 + rel);
            kind = "jmp rel8";
        } else if (b[0] == 0xE9) {
            int32_t rel = *reinterpret_cast<const int32_t*>(b + 1);
            next = const_cast<uint8_t*>(b + 5 + rel);
            kind = "jmp rel32";
        } else if (b[0] == 0xFF && b[1] == 0x25) {
            int32_t disp = *reinterpret_cast<const int32_t*>(b + 2);
            const uint8_t* ripNext = b + 6;
            const uint8_t* slot = ripNext + disp;
            if (!IsReadableCodePtr(slot)) {
                outTraceLines.push_back(std::string("depth=") + std::to_string(depth) + " rip-slot unreadable @" + PtrToHex(slot));
                return nullptr;
            }
            next = *reinterpret_cast<void* const*>(slot);
            kind = "jmp [rip+disp32]";
        } else if (b[0] == 0x48 && b[1] == 0xB8 && b[10] == 0xFF && b[11] == 0xE0) {
            next = *reinterpret_cast<void* const*>(b + 2);
            kind = "mov rax, imm64; jmp rax";
        } else if (b[0] == 0x49 && b[1] == 0xBB && b[10] == 0x41 && b[11] == 0xFF && b[12] == 0xE3) {
            next = *reinterpret_cast<void* const*>(b + 2);
            kind = "mov r11, imm64; jmp r11";
        }

        if (!next || !kind) {
            outTraceLines.push_back(std::string("depth=") + std::to_string(depth) + " no-jump-pattern @" + HookChain::DescribeAddressWithOwner(cur));
            return nullptr;
        }

        outTraceLines.push_back(std::string("depth=") + std::to_string(depth) + " " + kind + " " + HookChain::DescribeAddressWithOwner(cur) +
                                " -> " + HookChain::DescribeAddressWithOwner(next));

        if (!IsReadableCodePtr(next)) return next;
        if (!IsAbsoluteJumpStub(reinterpret_cast<const uint8_t*>(next))) return next;
        cur = next;
    }

    outTraceLines.push_back("max-depth reached starting @" + HookChain::DescribeAddressWithOwner(p));
    return nullptr;
}

static void LogHookChainDetails(const char* apiName, void* startAddress, void* resolvedHookTarget, const char* reason) {
    if (!apiName) apiName = "(unknown api)";
    if (!reason) reason = "(unspecified)";

    const char* mode = "LatestHook";

    LogCategory("hookchain",
                std::string("[") + apiName + "] chain-detect reason=" + reason + " nextTarget=" + mode + " start=" +
                    HookChain::DescribeAddressWithOwner(startAddress) + " hookTarget=" + HookChain::DescribeAddressWithOwner(resolvedHookTarget));

    std::vector<std::string> trace;
    (void)TraceAbsoluteJumpTarget(startAddress, trace);
    for (const auto& line : trace) {
        LogCategory("hookchain", std::string("[") + apiName + "] " + line);
    }
}

static void LogIatHookChainDetails(const char* apiName, HMODULE importingModule, void* thunkTarget, void* expectedExport) {
    if (!apiName) apiName = "(unknown api)";
    std::string importerDesc = importingModule ? HookChain::DescribeAddressWithOwner(importingModule) : std::string("(null)");
    LogCategory("hookchain",
                std::string("[") + apiName + "] IAT chain-detect importingModule=" + importerDesc + " iatTarget=" +
                    HookChain::DescribeAddressWithOwner(thunkTarget) + " expectedExport=" + HookChain::DescribeAddressWithOwner(expectedExport));
}

static bool TryInstallThirdPartyWglSwapBuffersHook(void* jumpTarget, const char* what) {
    void* currentTarget = g_wglSwapBuffersThirdPartyHookTarget.load(std::memory_order_acquire);
    if (jumpTarget == currentTarget) {
        return true;
    }

    if (currentTarget) {
        LogCategory("hookchain",
                    std::string("[wglSwapBuffers] keeping existing third-party hook target ") +
                        HookChain::DescribeAddressWithOwner(currentTarget) + " instead of switching to " +
                        HookChain::DescribeAddressWithOwner(jumpTarget));
        return false;
    }

    ScopedModulePin pinnedModule;
    if (!pinnedModule.PinForAddress(jumpTarget)) {
        LogSkippedStaleHookTarget("wglSwapBuffers", jumpTarget, "owner module unloaded before install");
        return false;
    }

    if (!IsReadableCodePtr(jumpTarget)) {
        LogSkippedStaleHookTarget("wglSwapBuffers", jumpTarget, "target page not readable during install");
        return false;
    }

    WGLSWAPBUFFERS trampoline = nullptr;
    if (!HookChain::TryCreateAndEnableHook(jumpTarget, reinterpret_cast<void*>(&hkwglSwapBuffers_ThirdParty),
                                           reinterpret_cast<void**>(&trampoline), what)) {
        return false;
    }
    g_owglSwapBuffersThirdParty.store(trampoline, std::memory_order_release);

    g_wglSwapBuffersThirdPartyHookTarget.store(jumpTarget, std::memory_order_release);
    g_lastSkippedWglSwapBuffersStart.store(nullptr, std::memory_order_release);
    g_lastSkippedWglSwapBuffersTarget.store(nullptr, std::memory_order_release);
    return true;
}

static void* FindIatImportedFunctionTarget(HMODULE importingModule, const char* importedDllNameLower, const char* funcName) {
    if (!importingModule || !importedDllNameLower || !funcName) return nullptr;

    MODULEINFO moduleInfo{};
    if (!GetModuleInformation(GetCurrentProcess(), importingModule, &moduleInfo,
                              sizeof(moduleInfo)) ||
        moduleInfo.lpBaseOfDll != importingModule ||
        moduleInfo.SizeOfImage < sizeof(IMAGE_DOS_HEADER)) {
        return nullptr;
    }
    uint8_t* base = reinterpret_cast<uint8_t*>(importingModule);
    const size_t imageSize = moduleInfo.SizeOfImage;
    const auto contains = [imageSize](size_t offset, size_t size) {
        return offset <= imageSize && size <= imageSize - offset;
    };
    auto* dos = reinterpret_cast<PIMAGE_DOS_HEADER>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        !contains(static_cast<size_t>(dos->e_lfanew),
                  sizeof(IMAGE_NT_HEADERS))) {
        return nullptr;
    }
    auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

    const IMAGE_DATA_DIRECTORY& impDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!impDir.VirtualAddress || !impDir.Size ||
        !contains(impDir.VirtualAddress, impDir.Size)) {
        return nullptr;
    }

    auto* desc = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(base + impDir.VirtualAddress);
    const size_t descriptorCount =
        impDir.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
    for (size_t descriptorIndex = 0;
         descriptorIndex < descriptorCount && desc[descriptorIndex].Name != 0;
         ++descriptorIndex) {
        const IMAGE_IMPORT_DESCRIPTOR& current = desc[descriptorIndex];
        if (!contains(current.Name, 1)) continue;
        const char* dllName =
            reinterpret_cast<const char*>(base + current.Name);
        const size_t dllNameCapacity = imageSize - current.Name;
        const size_t dllNameLength = strnlen_s(dllName, dllNameCapacity);
        if (dllNameLength == dllNameCapacity) continue;

        std::string dllLower(dllName, dllNameLength);
        for (char& c : dllLower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (dllLower != importedDllNameLower) continue;

        const DWORD originalThunkRva =
            current.OriginalFirstThunk ? current.OriginalFirstThunk
                                       : current.FirstThunk;
        if (!originalThunkRva || !current.FirstThunk ||
            !contains(originalThunkRva, sizeof(IMAGE_THUNK_DATA)) ||
            !contains(current.FirstThunk, sizeof(IMAGE_THUNK_DATA))) {
            continue;
        }
        auto* oft =
            reinterpret_cast<PIMAGE_THUNK_DATA>(base + originalThunkRva);
        auto* ft = reinterpret_cast<PIMAGE_THUNK_DATA>(
            base + current.FirstThunk);
        const size_t thunkCount =
            (std::min)(imageSize - originalThunkRva,
                       imageSize - current.FirstThunk) /
            sizeof(IMAGE_THUNK_DATA);
        for (size_t thunkIndex = 0;
             thunkIndex < thunkCount &&
             oft[thunkIndex].u1.AddressOfData != 0;
             ++thunkIndex) {
            if (IMAGE_SNAP_BY_ORDINAL(oft[thunkIndex].u1.Ordinal)) continue;
            const size_t nameRva =
                static_cast<size_t>(oft[thunkIndex].u1.AddressOfData);
            if (!contains(nameRva, sizeof(IMAGE_IMPORT_BY_NAME))) continue;
            auto* ibn =
                reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(base + nameRva);
            const size_t nameOffset =
                nameRva + offsetof(IMAGE_IMPORT_BY_NAME, Name);
            const size_t nameCapacity = imageSize - nameOffset;
            const char* importedName =
                reinterpret_cast<const char*>(ibn->Name);
            const size_t nameLength =
                strnlen_s(importedName, nameCapacity);
            if (nameLength == nameCapacity) continue;
            if (strcmp(importedName, funcName) == 0) {
                return reinterpret_cast<void*>(
                    ft[thunkIndex].u1.Function);
            }
        }
    }
    return nullptr;
}

static void RefreshThirdPartyWglSwapBuffersHookChain() {
    if (g_config.disableHookChaining) return;

    HMODULE hOpenGL32 = GetModuleHandle(L"opengl32.dll");
    if (!hOpenGL32) return;

    void* exportSwap = reinterpret_cast<void*>(GetProcAddress(hOpenGL32, "wglSwapBuffers"));
    if (!exportSwap) return;

    void* observedTarget = ResolveAbsoluteJumpTarget(exportSwap);
    const bool sawDisallowedOuterHook = observedTarget && !HookChain::IsAllowedSwapBuffersThirdPartyHookAddress(observedTarget);
    if (sawDisallowedOuterHook) {
        LogSkippedDisallowedHookTarget("wglSwapBuffers", exportSwap, observedTarget);
    }

    void* jumpTarget = ResolveFirstAllowedSwapBuffersHookTarget(
        exportSwap,
        false,
        { reinterpret_cast<void*>(&hkwglSwapBuffers_ThirdParty) });
    if (!jumpTarget) {
        void* cur = exportSwap;
        for (int depth = 0; depth < 16 && cur; depth++) {
            if (depth > 0 && cur != reinterpret_cast<void*>(&hkwglSwapBuffers_ThirdParty) &&
                HookChain::IsAllowedSwapBuffersThirdPartyHookAddress(cur)) {
                jumpTarget = cur;
                break;
            }

            if (!IsReadableCodePtr(cur) || !IsAbsoluteJumpStub(reinterpret_cast<const uint8_t*>(cur))) {
                break;
            }

            void* next = nullptr;
            if (!TryResolveJumpTarget(cur, next) || next == reinterpret_cast<void*>(&hkwglSwapBuffers_ThirdParty)) {
                break;
            }

            cur = next;
        }
    }
    if (!jumpTarget && observedTarget && HookChain::IsAllowedSwapBuffersThirdPartyHookAddress(observedTarget)) {
        jumpTarget = observedTarget;
    }
    if (!jumpTarget) return;

    if (jumpTarget == reinterpret_cast<void*>(&hkwglSwapBuffers_ThirdParty)) {
        return;
    }

    if (jumpTarget == reinterpret_cast<void*>(&hkwglSwapBuffers) && !sawDisallowedOuterHook) {
        return;
    }

    if (jumpTarget == g_wglSwapBuffersThirdPartyHookTarget.load(std::memory_order_acquire)) {
        return;
    }

    if (TryInstallThirdPartyWglSwapBuffersHook(jumpTarget, "wglSwapBuffers (third-party chain)")) {
        LogHookChainDetails("wglSwapBuffers", exportSwap, jumpTarget,
                            sawDisallowedOuterHook && jumpTarget == observedTarget ? "export detour (transport fallback)"
                                                                                   : "export detour (prolog)");
        Log("Chained wglSwapBuffers through third-party detour target at " + HookChain::DescribeAddressWithOwner(jumpTarget));
    }
}

static void RefreshThirdPartyWglSwapBuffersIatHookChain() {
    if (g_config.disableHookChaining) return;

    HMODULE opengl32 = GetModuleHandle(L"opengl32.dll");
    if (!opengl32) return;

    void* exportSwap = reinterpret_cast<void*>(GetProcAddress(opengl32, "wglSwapBuffers"));
    if (!exportSwap) return;

    HMODULE mods[1024];
    DWORD cbNeeded = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &cbNeeded)) return;

    const DWORD count = (std::min<DWORD>)(
        cbNeeded / sizeof(HMODULE),
        static_cast<DWORD>(sizeof(mods) / sizeof(mods[0])));
    for (DWORD i = 0; i < count; i++) {
        HMODULE m = mods[i];
        if (!m) continue;

        if (m == opengl32) continue;

        ScopedModulePin pinnedImporter;
        if (!pinnedImporter.PinForAddress(m)) continue;
        void* thunkTarget = FindIatImportedFunctionTarget(
            pinnedImporter.module, "opengl32.dll", "wglSwapBuffers");
        if (!thunkTarget) continue;

        if (!HookChain::IsAllowedSwapBuffersThirdPartyHookAddress(thunkTarget)) {
            void* observedTarget = ResolveAbsoluteJumpTarget(thunkTarget);
            if (observedTarget && !HookChain::IsAllowedSwapBuffersThirdPartyHookAddress(observedTarget)) {
                LogSkippedDisallowedHookTarget("wglSwapBuffers", thunkTarget, observedTarget);
            } else {
                LogSkippedDisallowedHookTarget("wglSwapBuffers", thunkTarget, thunkTarget);
            }
        }

        void* allowedTarget = ResolveFirstAllowedSwapBuffersHookTarget(
            thunkTarget,
            true,
            { reinterpret_cast<void*>(&hkwglSwapBuffers_ThirdParty) });
        if (!allowedTarget && HookChain::IsAllowedSwapBuffersThirdPartyHookAddress(thunkTarget)) {
            allowedTarget = thunkTarget;
        }
        if (allowedTarget) {
            thunkTarget = allowedTarget;
        } else {
            void* observedTarget = ResolveAbsoluteJumpTarget(thunkTarget);
            if (observedTarget && HookChain::IsAllowedSwapBuffersThirdPartyHookAddress(observedTarget)) {
                thunkTarget = observedTarget;
            } else if (!HookChain::IsAllowedSwapBuffersThirdPartyHookAddress(thunkTarget)) {
                continue;
            }
        }

        if (thunkTarget == exportSwap || thunkTarget == reinterpret_cast<void*>(&hkwglSwapBuffers_ThirdParty)) {
            continue;
        }

        if (thunkTarget == g_wglSwapBuffersThirdPartyHookTarget.load(std::memory_order_acquire)) {
            return;
        }

        if (TryInstallThirdPartyWglSwapBuffersHook(thunkTarget, "wglSwapBuffers (IAT third-party chain)")) {
            LogIatHookChainDetails("wglSwapBuffers", m, thunkTarget, exportSwap);
            Log("Chained wglSwapBuffers via IAT target at " + HookChain::DescribeAddressWithOwner(thunkTarget));
            return;
        }
    }
}

}

namespace HookChain {

bool IsAllowedSwapBuffersThirdPartyHookAddress(const void* addr) {
    HookChainOwnerInfo ownerInfo{};
    if (!GetOwnerInfoForAddress(addr, ownerInfo)) return false;
    return !IsToolscreenHookOwner(ownerInfo);
}

bool TryCreateAndEnableHook(void* target, void* detour, void** outOriginal, const char* what) {
    if (!target) return false;

    MH_STATUS st = MH_CreateHook(target, detour, outOriginal);
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) {
        Log(std::string("ERROR: Failed to create ") + (what ? what : "(hook)") + " hook (status " + std::to_string((int)st) + ")");
        return false;
    }

    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) {
        if ((int)st == 11) {
            MH_RemoveHook(target);
            Log(std::string("INFO: Skipping ") + (what ? what : "(hook)") +
                " hook because the target is not safely patchable by MinHook (status " + std::to_string((int)st) + ")");
            return false;
        }
        Log(std::string("ERROR: Failed to enable ") + (what ? what : "(hook)") + " hook (status " + std::to_string((int)st) + ")");
        return false;
    }
    return true;
}

std::string DescribeAddressWithOwner(const void* addr) {
    if (!addr) return std::string("(null)");

    HookChainOwnerInfo info;
    if (!GetOwnerInfoForAddress(addr, info)) {
        return PtrToHex(addr) + " (unknown module)";
    }

    std::ostringstream ss;
    ss << PtrToHex(addr);
    if (!info.name.empty()) {
        ss << " (" << WideToUtf8(info.name);
        if (info.base) {
            ss << "+0x" << std::hex << std::uppercase << ((uintptr_t)addr - info.base);
        }
        ss << ")";
    }
    if (!info.description.empty()) ss << " \"" << WideToUtf8(info.description) << "\"";
    if (!info.product.empty()) ss << " product=\"" << WideToUtf8(info.product) << "\"";
    if (!info.company.empty()) ss << " company=\"" << WideToUtf8(info.company) << "\"";
    if (!info.path.empty()) ss << " path=\"" << WideToUtf8(info.path) << "\"";
    return ss.str();
}

void RefreshAllThirdPartyHookChains() {
    std::lock_guard<std::mutex> lock(g_wglSwapBuffersThirdPartyHookMutex);

    void* currentTarget = g_wglSwapBuffersThirdPartyHookTarget.load(std::memory_order_acquire);
    if (currentTarget) {
        if (IsLiveThirdPartyWglSwapBuffersHookTarget(currentTarget)) {
            return;
        }

        ClearStaleThirdPartyWglSwapBuffersHookState(currentTarget, "installed target no longer live during refresh");
    }

    RefreshThirdPartyWglSwapBuffersHookChain();
    if (!g_wglSwapBuffersThirdPartyHookTarget.load(std::memory_order_acquire)) {
        RefreshThirdPartyWglSwapBuffersIatHookChain();
    }
}

}


