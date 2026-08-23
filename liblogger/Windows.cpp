#include "MinHook.h"
#include "base64.h"
#include <jni.h>
#include <Windows.h>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <TlHelp32.h>
#include <WinTrust.h>
#include <SoftPub.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <unordered_set>
#include <mscat.h>

#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "Crypt32.lib")
#pragma comment(lib, "user32.lib")

void LogToMinecraft(const std::string& message);
void LogErrorToMinecraft(const std::string& eventName, const std::string& errorMessage);

// Global cache for seen module hashes to skip expensive detection
std::mutex g_seenHashesMutex;
std::unordered_set<std::string> g_seenHashes;

#ifndef LIBLOGGER_VERSION_STR
#define LIBLOGGER_VERSION_STR "1.1.0"
#endif

// Version number
const std::string LIBLOGGER_VERSION = LIBLOGGER_VERSION_STR;

// JNI is not safe to enter merely because jvm.dll exports
// JNI_GetCreatedJavaVMs. During early VM bootstrap, attaching a native thread
// can crash inside the VM (notably with ZGC). All producers therefore enqueue
// their messages and a single worker begins using JNI only after the game has
// created a visible window.
std::mutex g_jniLogMutex;
std::condition_variable g_jniLogCondition;
std::deque<std::string> g_jniLogQueue;
std::atomic<bool> g_shuttingDown{false};

// Callback function for EnumWindows
BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    DWORD windowProcessId = 0;
    GetWindowThreadProcessId(hwnd, &windowProcessId);
    
    if (windowProcessId == GetCurrentProcessId() && IsWindowVisible(hwnd)) {
        *reinterpret_cast<bool*>(lParam) = true;
        return FALSE; // Stop enumeration
    }
    return TRUE; // Continue enumeration
}

// Helper function to wait for our process to have a visible window
bool WaitForProcessWindow() {
    const int retryDelayMs = 100;
    
    while (!g_shuttingDown.load(std::memory_order_acquire)) {
        bool hasWindow = false;
        EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&hasWindow));
        
        if (hasWindow) {
            return true;
        }
        
        Sleep(retryDelayMs);
    }
    
    return false;
}

// Helper to dynamically load JNI_GetCreatedJavaVMs to avoid linking jvm.lib
typedef jint (JNICALL *PtrJNI_GetCreatedJavaVMs)(JavaVM **, jsize, jsize *);

jint DynamicGetCreatedJavaVMs(JavaVM **vmBuf, jsize bufLen, jsize *nVMs) {
    HMODULE hJvm = GetModuleHandle(TEXT("jvm.dll"));
    if (hJvm == NULL) {
        // Fallback: try just "jvm" in case of different naming conventions
        hJvm = GetModuleHandle(TEXT("jvm"));
    }
    
    if (hJvm == NULL) {
        return JNI_ERR;
    }

    PtrJNI_GetCreatedJavaVMs ptr = (PtrJNI_GetCreatedJavaVMs)GetProcAddress(hJvm, "JNI_GetCreatedJavaVMs");
    if (ptr == NULL) {
        return JNI_ERR;
    }

    return ptr(vmBuf, bufLen, nVMs);
}

// Function to attach thread with custom name via JNI
// This must be called BEFORE any other JNI operations on the thread
// Returns the JNIEnv for the attached thread, or nullptr on failure
JNIEnv* AttachThreadWithName(const std::string& threadName) {
    try {
        JavaVM* jvm = nullptr;
        JNIEnv* env = nullptr;
        
        // Get JVM
        jsize vm_count = 0;
        if (DynamicGetCreatedJavaVMs(&jvm, 1, &vm_count) != JNI_OK || vm_count == 0) {
            return nullptr;
        }
        
        // Check if already attached
        jint getEnvResult = jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
        
        if (getEnvResult == JNI_EDETACHED) {
            // Not attached yet, attach now with custom name
            JavaVMAttachArgs args;
            args.version = JNI_VERSION_1_6;
            args.name = const_cast<char*>(threadName.c_str());
            args.group = nullptr;
            
            if (jvm->AttachCurrentThreadAsDaemon(reinterpret_cast<void**>(&env), &args) != JNI_OK) {
                return nullptr;
            }

            return env;
        } else if (getEnvResult == JNI_OK) {
            // Already attached, try to rename
            jclass threadClass = env->FindClass("java/lang/Thread");
            if (threadClass) {
                jmethodID currentThreadMethod = env->GetStaticMethodID(threadClass, "currentThread", "()Ljava/lang/Thread;");
                if (currentThreadMethod) {
                    jobject currentThread = env->CallStaticObjectMethod(threadClass, currentThreadMethod);
                    if (currentThread) {
                        jmethodID setNameMethod = env->GetMethodID(threadClass, "setName", "(Ljava/lang/String;)V");
                        if (setNameMethod) {
                            jstring jThreadName = env->NewStringUTF(threadName.c_str());
                            env->CallVoidMethod(currentThread, setNameMethod, jThreadName);
                            env->DeleteLocalRef(jThreadName);
                        }
                        env->DeleteLocalRef(currentThread);
                    }
                }
                env->DeleteLocalRef(threadClass);
            }
            return env;
        }
        
        return nullptr;
    } catch (...) {
        return nullptr;
    }
}

std::string Base64Encode(const std::string& input) {
    return macaron::Base64::Encode(input);
}

std::wstring ConvertCharToWchar(const char* str) {
    if (!str) return L"";
    
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, str, -1, nullptr, 0);
    if (size_needed <= 1) return L"";
    
    std::wstring wstrTo(size_needed - 1, 0); // size_needed includes null terminator
    MultiByteToWideChar(CP_UTF8, 0, str, -1, &wstrTo[0], size_needed);
    return wstrTo;
}

std::string ConvertWcharToChar(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size_needed <= 1) return "";
    
    std::string strTo(size_needed - 1, 0); // size_needed includes null terminator
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &strTo[0], size_needed, nullptr, nullptr);
    return strTo;
}

// Global variables
typedef HMODULE(WINAPI* LoadLibraryW_t)(LPCWSTR lpLibFileName);
LoadLibraryW_t pOriginalLoadLibraryW = nullptr;

typedef HMODULE(WINAPI* LoadLibraryA_t)(LPCSTR lpLibFileName);
LoadLibraryA_t pOriginalLoadLibraryA = nullptr;

typedef HMODULE(WINAPI* LoadLibraryExW_t)(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);
LoadLibraryExW_t pOriginalLoadLibraryExW = nullptr;

typedef HMODULE(WINAPI* LoadLibraryExA_t)(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);
LoadLibraryExA_t pOriginalLoadLibraryExA = nullptr;

// Trust verification may load supporting Windows DLLs. Let those nested loads
// pass through so the LoadLibrary hooks do not recursively verify themselves.
thread_local bool g_isCheckingLoadSignature = false;

// Structure to hold module information
struct ModuleInfo {
    std::wstring path;
    std::wstring hash;
    std::wstring signerName;
    std::wstring creationTime;
    std::wstring modifiedTime;
    std::wstring importedModules;
};

// RAII Handle wrappers
struct HandleDeleter {
    void operator()(HANDLE h) const {
        if (h != NULL && h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
        }
    }
};
using ScopedHandle = std::unique_ptr<void, HandleDeleter>;

struct BcryptAlgDeleter {
    void operator()(BCRYPT_ALG_HANDLE h) const {
        if (h) {
            BCryptCloseAlgorithmProvider(h, 0);
        }
    }
};
using ScopedBcryptAlgHandle = std::unique_ptr<std::remove_pointer<BCRYPT_ALG_HANDLE>::type, BcryptAlgDeleter>;

struct BcryptHashDeleter {
    void operator()(BCRYPT_HASH_HANDLE h) const {
        if (h) {
            BCryptDestroyHash(h);
        }
    }
};
using ScopedBcryptHashHandle = std::unique_ptr<std::remove_pointer<BCRYPT_HASH_HANDLE>::type, BcryptHashDeleter>;

std::wstring CalculateSHA512(LPCWSTR filePath) {
    BCRYPT_ALG_HANDLE hAlgRaw = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlgRaw, BCRYPT_SHA512_ALGORITHM, nullptr, 0))) {
        return L"[Hash Failed: Algorithm Provider]";
    }
    ScopedBcryptAlgHandle hAlg(hAlgRaw);

    BCRYPT_HASH_HANDLE hHashRaw = nullptr;
    if (!BCRYPT_SUCCESS(BCryptCreateHash(hAlg.get(), &hHashRaw, nullptr, 0, nullptr, 0, 0))) {
        return L"[Hash Failed: Create Hash]";
    }
    ScopedBcryptHashHandle hHash(hHashRaw);

    ScopedHandle hFile(CreateFileW(filePath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (hFile.get() == INVALID_HANDLE_VALUE) {
        return L"[Hash Failed: File Open]";
    }

    BYTE buffer[8192]; // Increased buffer size for better performance
    DWORD bytesRead = 0;
    while (ReadFile(hFile.get(), buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0) {
        if (!BCRYPT_SUCCESS(BCryptHashData(hHash.get(), buffer, bytesRead, 0))) {
            return L"[Hash Failed: Hashing Data]";
        }
    }

    DWORD hashSize = 0;
    DWORD cbData = 0;
    if (!BCRYPT_SUCCESS(BCryptGetProperty(hAlg.get(), BCRYPT_HASH_LENGTH, reinterpret_cast<PBYTE>(&hashSize), sizeof(DWORD), &cbData, 0))) {
        return L"[Hash Failed: Get Property]";
    }

    std::vector<BYTE> hashBuffer(hashSize);
    if (!BCRYPT_SUCCESS(BCryptFinishHash(hHash.get(), hashBuffer.data(), hashSize, 0))) {
        return L"[Hash Failed: Finish Hash]";
    }

    static constexpr wchar_t kHexDigits[] = L"0123456789abcdef";
    std::wstring result;
    result.reserve(hashBuffer.size() * 2);
    for (BYTE byte : hashBuffer) {
        result.push_back(kHexDigits[byte >> 4]);
        result.push_back(kHexDigits[byte & 0x0F]);
    }
    return result;
}

std::wstring GetSignerName(LPCWSTR filePath) {
    LONG lStatus;
    GUID policyGUID = WINTRUST_ACTION_GENERIC_VERIFY_V2;

    auto extractSignerName = [](WINTRUST_DATA& trustData, bool isCatalog) -> std::wstring {
        CRYPT_PROVIDER_DATA* pProvData = WTHelperProvDataFromStateData(trustData.hWVTStateData);
        if (!pProvData || pProvData->csSigners == 0) {
            return isCatalog ? L"[Signed via Catalog, No Signer Info]" : L"[Signed, No Signer Info]";
        }

        std::wstring signerNames;
        for (DWORD signerIndex = 0; signerIndex < pProvData->csSigners; ++signerIndex) {
            CRYPT_PROVIDER_SGNR* pSgnr = WTHelperGetProvSignerFromChain(pProvData, signerIndex, FALSE, 0);
            if (!pSgnr || !pSgnr->pChainContext || pSgnr->pChainContext->cChain == 0 ||
                pSgnr->pChainContext->rgpChain[0]->cElement == 0) {
                continue;
            }

            PCCERT_CONTEXT pCertContext = pSgnr->pChainContext->rgpChain[0]->rgpElement[0]->pCertContext;
            if (!pCertContext) {
                continue;
            }

            WCHAR szName[256];
            if (CertGetNameStringW(pCertContext, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, szName, 256) > 1) {
                if (!signerNames.empty()) {
                    signerNames += L"; ";
                }
                signerNames += szName;
                if (isCatalog) {
                    signerNames += L" (Catalog)";
                }
            }
        }

        if (!signerNames.empty()) {
            return signerNames;
        }
        return isCatalog ? L"[Signed via Catalog, Signer Not Found]" : L"[Signed, Signer Not Found]";
    };

    auto cleanupWinTrust = [&policyGUID](WINTRUST_DATA& wtd) {
        wtd.dwStateAction = WTD_STATEACTION_CLOSE;
        WinVerifyTrust(nullptr, &policyGUID, &wtd);
    };

    // Try direct signature first
    WINTRUST_FILE_INFO fileInfo = { sizeof(WINTRUST_FILE_INFO), filePath };
    WINTRUST_DATA winTrustData = {};
    winTrustData.cbStruct = sizeof(WINTRUST_DATA);
    winTrustData.dwUIChoice = WTD_UI_NONE;
    winTrustData.fdwRevocationChecks = WTD_REVOKE_NONE;
    winTrustData.dwUnionChoice = WTD_CHOICE_FILE;
    winTrustData.pFile = &fileInfo;
    winTrustData.dwStateAction = WTD_STATEACTION_VERIFY;

    lStatus = WinVerifyTrust(nullptr, &policyGUID, &winTrustData);

    if (lStatus == ERROR_SUCCESS) {
        std::wstring result = extractSignerName(winTrustData, false);
        cleanupWinTrust(winTrustData);
        return result;
    }

    if (lStatus != TRUST_E_NOSIGNATURE) {
        cleanupWinTrust(winTrustData);
        return L"[Invalid Signature]";
    }

    cleanupWinTrust(winTrustData);

    // Try catalog signature
    HCATADMIN hCatAdmin = nullptr;
    if (!CryptCATAdminAcquireContext2(&hCatAdmin, nullptr, nullptr, nullptr, 0)) {
        return L"[Unsigned]";
    }

    ScopedHandle hFile(CreateFileW(filePath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
    if (hFile.get() == INVALID_HANDLE_VALUE) {
        CryptCATAdminReleaseContext(hCatAdmin, 0);
        return L"[Unsigned]";
    }

    std::vector<BYTE> hash(1024);
    DWORD hashSize = static_cast<DWORD>(hash.size());
    if (!CryptCATAdminCalcHashFromFileHandle2(hCatAdmin, hFile.get(), &hashSize, hash.data(), 0)) {
        CryptCATAdminReleaseContext(hCatAdmin, 0);
        return L"[Unsigned]";
    }
    hash.resize(hashSize);

    HCATINFO hCatInfo = CryptCATAdminEnumCatalogFromHash(hCatAdmin, hash.data(), hashSize, 0, nullptr);
    if (!hCatInfo) {
        CryptCATAdminReleaseContext(hCatAdmin, 0);
        return L"[Unsigned]";
    }

    CATALOG_INFO catInfo = { sizeof(CATALOG_INFO) };
    if (!CryptCATCatalogInfoFromContext(hCatInfo, &catInfo, 0)) {
        CryptCATAdminReleaseCatalogContext(hCatAdmin, hCatInfo, 0);
        CryptCATAdminReleaseContext(hCatAdmin, 0);
        return L"[Unsigned]";
    }

    // Verify catalog signature
    WINTRUST_FILE_INFO catFileInfo = { sizeof(WINTRUST_FILE_INFO), catInfo.wszCatalogFile };
    WINTRUST_DATA wtdCat = {};
    wtdCat.cbStruct = sizeof(wtdCat);
    wtdCat.dwUIChoice = WTD_UI_NONE;
    wtdCat.fdwRevocationChecks = WTD_REVOKE_NONE;
    wtdCat.dwUnionChoice = WTD_CHOICE_FILE;
    wtdCat.pFile = &catFileInfo;
    wtdCat.dwStateAction = WTD_STATEACTION_VERIFY;

    lStatus = WinVerifyTrust(nullptr, &policyGUID, &wtdCat);

    std::wstring signerName = (lStatus == ERROR_SUCCESS) 
        ? extractSignerName(wtdCat, true) 
        : L"[Unsigned]";
    
    cleanupWinTrust(wtdCat);
    CryptCATAdminReleaseCatalogContext(hCatAdmin, hCatInfo, 0);
    CryptCATAdminReleaseContext(hCatAdmin, 0);

    return signerName;
}

bool ContainsOrdinalIgnoreCase(const std::wstring& value, const wchar_t* needle) {
    const int needleLength = static_cast<int>(wcslen(needle));
    if (needleLength == 0 || value.size() < static_cast<size_t>(needleLength)) {
        return false;
    }

    for (size_t offset = 0; offset + needleLength <= value.size(); ++offset) {
        if (CompareStringOrdinal(value.data() + offset, needleLength, needle, needleLength, TRUE) == CSTR_EQUAL) {
            return true;
        }
    }
    return false;
}

std::wstring ResolveLoadLibraryPath(LPCWSTR requestedPath) {
    if (!requestedPath || requestedPath[0] == L'\0') {
        return L"";
    }

    std::vector<wchar_t> resolvedPath(MAX_PATH);
    while (true) {
        DWORD length = SearchPathW(
            nullptr,
            requestedPath,
            L".dll",
            static_cast<DWORD>(resolvedPath.size()),
            resolvedPath.data(),
            nullptr);
        if (length == 0) {
            return L"";
        }
        if (length < resolvedPath.size()) {
            return std::wstring(resolvedPath.data(), length);
        }
        resolvedPath.resize(static_cast<size_t>(length) + 1);
    }
}

bool ShouldBlockLoadLibrary(LPCWSTR requestedPath) {
    std::wstring resolvedPath = ResolveLoadLibraryPath(requestedPath);
    if (resolvedPath.empty()) {
        return false;
    }

    // GetSignerName returns a signer only after WinVerifyTrust accepts the
    // embedded or catalog signature. Invalid and unsigned files stay allowed.
    return ContainsOrdinalIgnoreCase(GetSignerName(resolvedPath.c_str()), L"Overwolf");
}

std::wstring ConvertAnsiToWchar(LPCSTR value) {
    if (!value) {
        return L"";
    }

    int requiredLength = MultiByteToWideChar(CP_ACP, 0, value, -1, nullptr, 0);
    if (requiredLength <= 1) {
        return L"";
    }

    std::wstring converted(static_cast<size_t>(requiredLength), L'\0');
    if (MultiByteToWideChar(CP_ACP, 0, value, -1, converted.data(), requiredLength) == 0) {
        return L"";
    }
    converted.resize(static_cast<size_t>(requiredLength - 1));
    return converted;
}

bool IsBlockedLoadLibraryRequest(LPCWSTR requestedPath) {
    if (g_isCheckingLoadSignature) {
        return false;
    }

    g_isCheckingLoadSignature = true;
    bool shouldBlock = false;
    try {
        shouldBlock = ShouldBlockLoadLibrary(requestedPath);
    } catch (...) {
        // Signature inspection is fail-open so unrelated DLL loads are not
        // broken by path, trust-provider, or allocation failures.
    }
    g_isCheckingLoadSignature = false;
    return shouldBlock;
}

bool IsBlockedLoadLibraryRequest(LPCSTR requestedPath) {
    if (g_isCheckingLoadSignature) {
        return false;
    }

    g_isCheckingLoadSignature = true;
    bool shouldBlock = false;
    try {
        std::wstring widePath = ConvertAnsiToWchar(requestedPath);
        shouldBlock = !widePath.empty() && ShouldBlockLoadLibrary(widePath.c_str());
    } catch (...) {
        // Keep the same fail-open behavior as the wide-character hook.
    }
    g_isCheckingLoadSignature = false;
    return shouldBlock;
}

std::vector<char> ReadPeFile(LPCWSTR filePath) {
    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        throw std::runtime_error("PE Read Error: Could not open file");
    }

    std::streamsize fileSize = file.tellg();
    if (fileSize == -1) {
        throw std::runtime_error("PE Read Error: Failed to get file size");
    }
    file.seekg(0, std::ios::beg);
    if (fileSize == 0) {
        throw std::runtime_error("PE Read Error: File is empty");
    }

    std::vector<char> buffer;
    try {
        buffer.resize(static_cast<size_t>(fileSize));
    } catch (const std::bad_alloc&) {
        throw std::runtime_error("PE Read Error: Not enough memory to allocate buffer for file");
    } catch (const std::length_error&) {
        throw std::runtime_error("PE Read Error: File size exceeds maximum allocation size");
    }

    if (!file.read(buffer.data(), fileSize)) {
        throw std::runtime_error("PE Read Error: Could not read file content");
    }

    if (static_cast<size_t>(fileSize) < sizeof(IMAGE_DOS_HEADER)) {
        throw std::runtime_error("PE Format Error: File too small for DOS header");
    }

    PIMAGE_DOS_HEADER dosHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(buffer.data());
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
        throw std::runtime_error("PE Format Error: DOS signature mismatch (not an 'MZ' file)");
    }

    if (static_cast<size_t>(fileSize) < dosHeader->e_lfanew + sizeof(IMAGE_NT_HEADERS)) {
        throw std::runtime_error("PE Format Error: Invalid NT header offset or file too small");
    }

    PIMAGE_NT_HEADERS ntHeader = reinterpret_cast<PIMAGE_NT_HEADERS>(buffer.data() + dosHeader->e_lfanew);
    if (ntHeader->Signature != IMAGE_NT_SIGNATURE) {
        throw std::runtime_error("PE Format Error: Invalid PE signature");
    }

    return buffer;
}

DWORD RvaToOffset(PIMAGE_NT_HEADERS ntHeader, DWORD rva, DWORD fileSize) {
    PIMAGE_SECTION_HEADER sectionHeader = IMAGE_FIRST_SECTION(ntHeader);
    for (WORD i = 0; i < ntHeader->FileHeader.NumberOfSections; i++, sectionHeader++) {
        if (rva >= sectionHeader->VirtualAddress && rva < sectionHeader->VirtualAddress + sectionHeader->Misc.VirtualSize) {
            DWORD offset = (rva - sectionHeader->VirtualAddress) + sectionHeader->PointerToRawData;
            return (offset < fileSize) ? offset : 0;
        }
    }
    return 0;
}

std::wstring GetImportedModules(const std::vector<char>& buffer) {
    try {
        PIMAGE_NT_HEADERS ntHeader = (PIMAGE_NT_HEADERS)(buffer.data() + ((PIMAGE_DOS_HEADER)buffer.data())->e_lfanew);
        PIMAGE_DATA_DIRECTORY importDataDir;
        if (ntHeader->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
            importDataDir = &((PIMAGE_NT_HEADERS64)ntHeader)->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        } else {
            importDataDir = &ntHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        }

        if (importDataDir->VirtualAddress == 0) {
            return L"";
        }

        DWORD importDirOffset = RvaToOffset(ntHeader, importDataDir->VirtualAddress, static_cast<DWORD>(buffer.size()));
        if (importDirOffset == 0) {
            return L"[PE Error: Invalid Import Directory RVA]";
        }

        PIMAGE_IMPORT_DESCRIPTOR importDesc = (PIMAGE_IMPORT_DESCRIPTOR)(buffer.data() + importDirOffset);
        std::wstringstream result;
        bool firstModule = true;
        while (importDesc->Name != 0) {
            DWORD nameOffset = RvaToOffset(ntHeader, importDesc->Name, static_cast<DWORD>(buffer.size()));
            if (nameOffset == 0) {
                importDesc++;
                continue;
            }
            if (!firstModule) {
                result << L", ";
            }
            std::string s((const char*)(buffer.data() + nameOffset));
            result << ConvertCharToWchar(s.c_str());
            firstModule = false;
            importDesc++;
        }
        return result.str();
    } catch (const std::exception& e) {
        std::string err(e.what());
        return ConvertCharToWchar(err.c_str());
    }
}

std::wstring GetFileTimestamp(LPCWSTR filePath, bool isCreationTime) {
    ScopedHandle hFile(CreateFileW(filePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL));
    if (hFile.get() == INVALID_HANDLE_VALUE) {
        return L"[Error Reading Time]";
    }

    FILETIME ft, ftLocal;
    SYSTEMTIME st;
    if (!GetFileTime(hFile.get(), isCreationTime ? &ft : NULL, NULL, isCreationTime ? NULL : &ft)) {
        return L"[Error Reading Time]";
    }

    FileTimeToLocalFileTime(&ft, &ftLocal);
    FileTimeToSystemTime(&ftLocal, &st);
    std::wstringstream ss;
    ss << st.wYear << L"-" << std::setfill(L'0') << std::setw(2) << st.wMonth << L"-" << std::setw(2) << st.wDay
        << L" " << std::setw(2) << st.wHour << L":" << std::setw(2) << st.wMinute << L":" << std::setw(2) << st.wSecond;
    return ss.str();
}

ModuleInfo AnalyzeModule(const std::wstring& modulePath) {
    ModuleInfo info;
    info.path = modulePath;
    info.hash = CalculateSHA512(modulePath.c_str());
    info.signerName = GetSignerName(modulePath.c_str());
    info.creationTime = GetFileTimestamp(modulePath.c_str(), true);
    info.modifiedTime = GetFileTimestamp(modulePath.c_str(), false);
    try {
        std::vector<char> buffer = ReadPeFile(modulePath.c_str());
        info.importedModules = GetImportedModules(buffer);
    } catch (const std::exception& e) {
        std::string err(e.what());
        info.importedModules = ConvertCharToWchar(err.c_str());
    } catch (...) {
        info.importedModules = L"[Unknown Exception in AnalyzeModule]";
    }
    return info;
}

void WriteToMinecraft(JNIEnv* env, const std::string& message) {
    try {
        jclass systemClass = env->FindClass("java/lang/System");
        if (!systemClass) {
            env->ExceptionClear();
            return;
        }

        jfieldID outFieldID = env->GetStaticFieldID(systemClass, "out", "Ljava/io/PrintStream;");
        if (!outFieldID) {
            env->ExceptionClear();
            env->DeleteLocalRef(systemClass);
            return;
        }

        jobject printStream = env->GetStaticObjectField(systemClass, outFieldID);
        if (!printStream) {
            env->ExceptionClear();
            env->DeleteLocalRef(systemClass);
            return;
        }

        jclass printStreamClass = env->GetObjectClass(printStream);
        if (!printStreamClass) {
            env->ExceptionClear();
            env->DeleteLocalRef(printStream);
            env->DeleteLocalRef(systemClass);
            return;
        }

        jmethodID printlnMethod = env->GetMethodID(printStreamClass, "println", "(Ljava/lang/String;)V");
        if (!printlnMethod) {
            env->ExceptionClear();
            env->DeleteLocalRef(printStreamClass);
            env->DeleteLocalRef(printStream);
            env->DeleteLocalRef(systemClass);
            return;
        }

        jstring logMessage = env->NewStringUTF(message.c_str());
        if (logMessage) {
            env->CallVoidMethod(printStream, printlnMethod, logMessage);
            env->ExceptionClear();
            env->DeleteLocalRef(logMessage);
        } else {
            env->ExceptionClear();
        }
        env->DeleteLocalRef(printStreamClass);
        env->DeleteLocalRef(printStream);
        env->DeleteLocalRef(systemClass);
    } catch (...) {
        // Silently fail to avoid infinite recursion
    }
}

void LogToMinecraft(const std::string& message) {
    {
        std::lock_guard<std::mutex> lock(g_jniLogMutex);
        if (g_shuttingDown.load(std::memory_order_relaxed)) {
            return;
        }
        g_jniLogQueue.push_back(message);
    }
    g_jniLogCondition.notify_one();
}

void RunJniLogWorker() {
    if (!WaitForProcessWindow()) {
        return;
    }

    JNIEnv* env = nullptr;
    while (!g_shuttingDown.load(std::memory_order_acquire) && env == nullptr) {
        env = AttachThreadWithName("LibLogger");
        if (env == nullptr) {
            Sleep(100);
        }
    }

    while (env != nullptr) {
        std::string message;
        {
            std::unique_lock<std::mutex> lock(g_jniLogMutex);
            g_jniLogCondition.wait(lock, [] {
                return g_shuttingDown.load(std::memory_order_acquire) || !g_jniLogQueue.empty();
            });
            if (g_shuttingDown.load(std::memory_order_relaxed) && g_jniLogQueue.empty()) {
                break;
            }
            message = std::move(g_jniLogQueue.front());
            g_jniLogQueue.pop_front();
        }
        WriteToMinecraft(env, message);
    }
}

// Log error to Minecraft console
// Format: securityEvent <eventName> <base64_eventData>
void LogErrorToMinecraft(const std::string& eventName, const std::string& errorMessage) {
    std::string encodedEventName = Base64Encode(eventName);
    std::string encodedEventData = Base64Encode(errorMessage);
    std::string formattedMessage = "securityEvent " + encodedEventName + " " + encodedEventData;
    LogToMinecraft(formattedMessage);
}

std::string EncodeImportsList(const std::string& imports) {
    if (imports.empty()) return "";
    
    std::string result;
    std::stringstream ss(imports);
    std::string item;
    
    while (std::getline(ss, item, ',')) {
        // Trim whitespace
        size_t start = item.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        
        size_t end = item.find_last_not_of(" \t");
        item = item.substr(start, end - start + 1);
        
        if (!result.empty()) result += ",";
        result += Base64Encode(item);
    }
    
    return result;
}

// Log module info to Minecraft console
// Format: moduleLoaded <base64_dllPath> <fileHash> <base64_signerName> <base64_imports>
void LogModuleToMinecraft(const ModuleInfo& info) {
    try {
        // Convert to strings and encode
        std::string encodedPath = Base64Encode(ConvertWcharToChar(info.path));
        std::string hash = ConvertWcharToChar(info.hash);
        std::string encodedSigner = Base64Encode(ConvertWcharToChar(info.signerName));
        std::string encodedImports = EncodeImportsList(ConvertWcharToChar(info.importedModules));

        // Format: moduleLoaded <base64_path> <hash> <base64_signer> <base64_imports>
        std::string formattedMessage = "moduleLoaded " + encodedPath + " " + hash + " " + encodedSigner + " " + encodedImports;
        
        LogToMinecraft(formattedMessage);
    } catch (const std::exception& e) {
        LogErrorToMinecraft("ModuleLogError", std::string("LogModuleToMinecraft Error: ") + e.what());
    } catch (...) {
        LogErrorToMinecraft("ModuleLogError", "LogModuleToMinecraft Error: Unknown exception");
    }
}

void RunInitialScanOptimized() {
    try {
        // Wait for our process to have a window
        if (!WaitForProcessWindow()) {
            LogErrorToMinecraft("InitialScanError", "Process window not available after timeout");
            return;
        }
        
        // Log version number to Minecraft
        LogToMinecraft("Running LibLogger v" + LIBLOGGER_VERSION + " for verification purposes");

        // Enumerate all modules
        ScopedHandle hModuleSnap(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId()));
        if (hModuleSnap.get() == INVALID_HANDLE_VALUE) {
            LogErrorToMinecraft("InitialScanError", "Could not create module snapshot");
            return;
        }

        MODULEENTRY32W me32 = { sizeof(MODULEENTRY32W) };
        
        if (Module32FirstW(hModuleSnap.get(), &me32)) {
            do {
                try {
                    
                    // Analyze module and log immediately using JNI references
                    ModuleInfo info = AnalyzeModule(me32.szExePath);
                    
                    std::string encodedPath = Base64Encode(ConvertWcharToChar(info.path));
                    std::string hash = ConvertWcharToChar(info.hash);
                    std::string encodedSigner = Base64Encode(ConvertWcharToChar(info.signerName));
                    std::string encodedImports = EncodeImportsList(ConvertWcharToChar(info.importedModules));

                    // Add to seen hashes cache
                    {
                        std::lock_guard<std::mutex> lock(g_seenHashesMutex);
                        g_seenHashes.insert(hash);
                    }

                    // Format: moduleLoaded <base64_path> <hash> <base64_signer> <base64_imports>
                    std::string formattedMessage = "moduleLoaded " + encodedPath + " " + hash + " " + encodedSigner + " " + encodedImports;
                    
                    LogToMinecraft(formattedMessage);
                } catch (const std::exception& e) {
                    LogErrorToMinecraft("ModuleAnalysisError", std::string("Initial Scan Module Analysis Error: ") + e.what());
                } catch (...) {
                    LogErrorToMinecraft("ModuleAnalysisError", "Initial Scan Module Analysis Error: Unknown exception");
                }
                
            } while (Module32NextW(hModuleSnap.get(), &me32));
        } else {
            LogErrorToMinecraft("InitialScanError", "Could not enumerate first module");
        }

    } catch (const std::exception& e) {
        LogErrorToMinecraft("InitialScanFatalError", std::string("Initial Scan Fatal Error: ") + e.what());
    } catch (...) {
        LogErrorToMinecraft("InitialScanFatalError", "Initial Scan Fatal Error: Unknown exception");
    }
}

void HandleLoadedModule(HMODULE hModule) {
    if (!hModule) return;
    
    WCHAR loadedPath[MAX_PATH];
    if (GetModuleFileNameW(hModule, loadedPath, MAX_PATH) == 0) {
        std::thread([]() {
            LogErrorToMinecraft("ModuleAnalysisError", "Could not get module file name");
        }).detach();
        return;
    }
    
    // Defer expensive module analysis to background thread
    std::thread([path = std::wstring(loadedPath)]() {
        try {
            // Calculate hash first (cheap operation) to check if we've seen this module before
            std::wstring hashW = CalculateSHA512(path.c_str());
            std::string hash = ConvertWcharToChar(hashW);
            
            // Check if we've already seen this hash
            {
                std::lock_guard<std::mutex> lock(g_seenHashesMutex);
                if (g_seenHashes.count(hash) > 0) {
                    // Already seen this module, skip expensive detection
                    return;
                }
                g_seenHashes.insert(hash);
            }
            
            // New module - do full analysis
            ModuleInfo info = AnalyzeModule(path);
            LogModuleToMinecraft(info);
        } catch (const std::exception& e) {
            LogErrorToMinecraft("ModuleAnalysisError", std::string(e.what()));
        } catch (...) {
            LogErrorToMinecraft("ModuleAnalysisError", "Unknown exception");
        }
    }).detach();
}

HMODULE WINAPI DetourLoadLibraryW(LPCWSTR lpLibFileName) {
    if (IsBlockedLoadLibraryRequest(lpLibFileName)) {
        SetLastError(ERROR_ACCESS_DENIED);
        return nullptr;
    }

    HMODULE hModule = pOriginalLoadLibraryW(lpLibFileName);
    HandleLoadedModule(hModule);
    return hModule;
}

HMODULE WINAPI DetourLoadLibraryA(LPCSTR lpLibFileName) {
    if (IsBlockedLoadLibraryRequest(lpLibFileName)) {
        SetLastError(ERROR_ACCESS_DENIED);
        return nullptr;
    }

    HMODULE hModule = pOriginalLoadLibraryA(lpLibFileName);
    HandleLoadedModule(hModule);
    return hModule;
}

HMODULE WINAPI DetourLoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    if (IsBlockedLoadLibraryRequest(lpLibFileName)) {
        SetLastError(ERROR_ACCESS_DENIED);
        return nullptr;
    }

    HMODULE hModule = pOriginalLoadLibraryExW(lpLibFileName, hFile, dwFlags);
    HandleLoadedModule(hModule);
    return hModule;
}

HMODULE WINAPI DetourLoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    if (IsBlockedLoadLibraryRequest(lpLibFileName)) {
        SetLastError(ERROR_ACCESS_DENIED);
        return nullptr;
    }

    HMODULE hModule = pOriginalLoadLibraryExA(lpLibFileName, hFile, dwFlags);
    HandleLoadedModule(hModule);
    return hModule;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);

        // Initialize MinHook
        if (MH_Initialize() != MH_OK) return FALSE;

        // Create hooks for every public LoadLibrary entry point.
        if (MH_CreateHook(&LoadLibraryW, &DetourLoadLibraryW, reinterpret_cast<LPVOID*>(&pOriginalLoadLibraryW)) != MH_OK ||
            MH_CreateHook(&LoadLibraryA, &DetourLoadLibraryA, reinterpret_cast<LPVOID*>(&pOriginalLoadLibraryA)) != MH_OK ||
            MH_CreateHook(&LoadLibraryExW, &DetourLoadLibraryExW, reinterpret_cast<LPVOID*>(&pOriginalLoadLibraryExW)) != MH_OK ||
            MH_CreateHook(&LoadLibraryExA, &DetourLoadLibraryExA, reinterpret_cast<LPVOID*>(&pOriginalLoadLibraryExA)) != MH_OK) {
            MH_Uninitialize();
            return FALSE;
        }

        // Enable all hooks
        if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
            MH_Uninitialize();
            return FALSE;
        }

        // Run initial scan in a separate thread (don't check in DllMain - can cause deadlock)
        std::thread(RunJniLogWorker).detach();
        std::thread([]() {
            RunInitialScanOptimized();
        }).detach();

    } else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        g_shuttingDown.store(true, std::memory_order_release);
        g_jniLogCondition.notify_all();
        // Cleanup
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
    }
    return TRUE;
}
