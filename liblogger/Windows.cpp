#ifndef NOMINMAX
#define NOMINMAX
#endif
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
#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <type_traits>
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

static constexpr const wchar_t* kBlockedSignerVendors[] = {
    L"Overwolf",
};

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
                            if (jThreadName) {
                                env->CallVoidMethod(currentThread, setNameMethod, jThreadName);
                                env->DeleteLocalRef(jThreadName);
                            }
                        }
                        env->DeleteLocalRef(currentThread);
                    }
                }
                env->DeleteLocalRef(threadClass);
            }
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
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
    
    std::wstring wstrTo(static_cast<size_t>(size_needed), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, str, -1, wstrTo.data(), size_needed) == 0) {
        return L"";
    }
    wstrTo.resize(static_cast<size_t>(size_needed - 1));
    return wstrTo;
}

std::string ConvertWcharToChar(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size_needed <= 1) return "";
    
    std::string strTo(static_cast<size_t>(size_needed), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, strTo.data(), size_needed, nullptr, nullptr) == 0) {
        return "";
    }
    strTo.resize(static_cast<size_t>(size_needed - 1));
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
    while (true) {
        if (!ReadFile(hFile.get(), buffer, sizeof(buffer), &bytesRead, nullptr)) {
            return L"[Hash Failed: File Read]";
        }
        if (bytesRead == 0) {
            break;
        }
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
                !pSgnr->pChainContext->rgpChain || !pSgnr->pChainContext->rgpChain[0] ||
                pSgnr->pChainContext->rgpChain[0]->cElement == 0 ||
                !pSgnr->pChainContext->rgpChain[0]->rgpElement ||
                !pSgnr->pChainContext->rgpChain[0]->rgpElement[0]) {
                continue;
            }

            PCCERT_CONTEXT pCertContext = pSgnr->pChainContext->rgpChain[0]->rgpElement[0]->pCertContext;
            if (!pCertContext) {
                continue;
            }

            DWORD nameLength = CertGetNameStringW(
                pCertContext,
                CERT_NAME_SIMPLE_DISPLAY_TYPE,
                0,
                nullptr,
                nullptr,
                0);
            if (nameLength > 1) {
                std::vector<wchar_t> nameBuffer(nameLength);
                if (CertGetNameStringW(
                        pCertContext,
                        CERT_NAME_SIMPLE_DISPLAY_TYPE,
                        0,
                        nullptr,
                        nameBuffer.data(),
                        nameLength) <= 1) {
                    continue;
                }
                if (!signerNames.empty()) {
                    signerNames += L"; ";
                }
                signerNames += nameBuffer.data();
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

    auto isNoSignatureStatus = [](LONG status) {
        return status == TRUST_E_NOSIGNATURE ||
            status == TRUST_E_SUBJECT_FORM_UNKNOWN ||
            status == TRUST_E_PROVIDER_UNKNOWN;
    };

    auto initializeFileTrustData = [](WINTRUST_DATA& trustData, WINTRUST_FILE_INFO& trustFileInfo) {
        trustData = {};
        trustData.cbStruct = sizeof(WINTRUST_DATA);
        trustData.dwUIChoice = WTD_UI_NONE;
        trustData.fdwRevocationChecks = WTD_REVOKE_NONE;
        trustData.dwUnionChoice = WTD_CHOICE_FILE;
        trustData.pFile = &trustFileInfo;
        trustData.dwStateAction = WTD_STATEACTION_VERIFY;
    };

    // Verify the primary embedded signature and discover any secondary
    // signatures. Each secondary signature is then verified independently so
    // vendor matching cannot be bypassed with a dual-signed file.
    WINTRUST_FILE_INFO fileInfo = { sizeof(WINTRUST_FILE_INFO), filePath };
    WINTRUST_SIGNATURE_SETTINGS signatureSettings = {};
    signatureSettings.cbStruct = sizeof(signatureSettings);
    signatureSettings.dwFlags = WSS_GET_SECONDARY_SIG_COUNT | WSS_VERIFY_SPECIFIC;
    signatureSettings.dwIndex = 0;

    WINTRUST_DATA winTrustData = {};
    initializeFileTrustData(winTrustData, fileInfo);
    winTrustData.pSignatureSettings = &signatureSettings;

    lStatus = WinVerifyTrust(nullptr, &policyGUID, &winTrustData);
    const DWORD secondarySignatureCount = signatureSettings.cSecondarySigs;
    bool foundEmbeddedSignature = !isNoSignatureStatus(lStatus);
    std::wstring embeddedSignerNames;

    if (lStatus == ERROR_SUCCESS) {
        embeddedSignerNames = extractSignerName(winTrustData, false);
    }
    cleanupWinTrust(winTrustData);

    for (DWORD signatureIndex = 1; signatureIndex <= secondarySignatureCount; ++signatureIndex) {
        WINTRUST_SIGNATURE_SETTINGS secondarySettings = {};
        secondarySettings.cbStruct = sizeof(secondarySettings);
        secondarySettings.dwIndex = signatureIndex;
        secondarySettings.dwFlags = WSS_VERIFY_SPECIFIC;

        WINTRUST_DATA secondaryTrustData = {};
        initializeFileTrustData(secondaryTrustData, fileInfo);
        secondaryTrustData.pSignatureSettings = &secondarySettings;

        LONG secondaryStatus = WinVerifyTrust(nullptr, &policyGUID, &secondaryTrustData);
        foundEmbeddedSignature = foundEmbeddedSignature || !isNoSignatureStatus(secondaryStatus);
        if (secondaryStatus == ERROR_SUCCESS) {
            std::wstring signerNames = extractSignerName(secondaryTrustData, false);
            if (!embeddedSignerNames.empty() && !signerNames.empty()) {
                embeddedSignerNames += L"; ";
            }
            embeddedSignerNames += signerNames;
        }
        cleanupWinTrust(secondaryTrustData);
    }

    if (!embeddedSignerNames.empty()) {
        return embeddedSignerNames;
    }
    if (foundEmbeddedSignature) {
        return L"[Invalid Signature]";
    }

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

    DWORD hashSize = 0;
    if (!CryptCATAdminCalcHashFromFileHandle2(hCatAdmin, hFile.get(), &hashSize, nullptr, 0) || hashSize == 0) {
        CryptCATAdminReleaseContext(hCatAdmin, 0);
        return L"[Unsigned]";
    }

    std::vector<BYTE> hash(hashSize);
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

    static constexpr wchar_t kHexDigits[] = L"0123456789ABCDEF";
    std::wstring memberTag;
    memberTag.reserve(hash.size() * 2);
    for (BYTE byte : hash) {
        memberTag.push_back(kHexDigits[byte >> 4]);
        memberTag.push_back(kHexDigits[byte & 0x0F]);
    }

    // Verify the file's membership in the catalog as well as the catalog's
    // signature. Verifying the catalog file alone does not authenticate this
    // particular module.
    WINTRUST_CATALOG_INFO catalogTrustInfo = {};
    catalogTrustInfo.cbStruct = sizeof(catalogTrustInfo);
    catalogTrustInfo.pcwszCatalogFilePath = catInfo.wszCatalogFile;
    catalogTrustInfo.pcwszMemberTag = memberTag.c_str();
    catalogTrustInfo.pcwszMemberFilePath = filePath;
    catalogTrustInfo.hMemberFile = hFile.get();
    catalogTrustInfo.pbCalculatedFileHash = hash.data();
    catalogTrustInfo.cbCalculatedFileHash = hashSize;

    WINTRUST_DATA wtdCat = {};
    wtdCat.cbStruct = sizeof(wtdCat);
    wtdCat.dwUIChoice = WTD_UI_NONE;
    wtdCat.fdwRevocationChecks = WTD_REVOKE_NONE;
    wtdCat.dwUnionChoice = WTD_CHOICE_CATALOG;
    wtdCat.pCatalog = &catalogTrustInfo;
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
    const size_t needleLength = wcslen(needle);
    if (needleLength == 0 || value.size() < needleLength ||
        needleLength > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return false;
    }

    const int compareLength = static_cast<int>(needleLength);
    for (size_t offset = 0; offset <= value.size() - needleLength; ++offset) {
        if (CompareStringOrdinal(value.data() + offset, compareLength, needle, compareLength, TRUE) == CSTR_EQUAL) {
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

bool ShouldBlockLoadLibrary(LPCWSTR requestedPath, std::wstring& blockedPath) {
    std::wstring resolvedPath = ResolveLoadLibraryPath(requestedPath);
    if (resolvedPath.empty()) {
        return false;
    }

    // GetSignerName returns a signer only after WinVerifyTrust accepts the
    // embedded or catalog signature. Invalid and unsigned files stay allowed.
    std::wstring signerName = GetSignerName(resolvedPath.c_str());
    bool isBlockedVendor = false;
    for (const wchar_t* blockedVendor : kBlockedSignerVendors) {
        if (ContainsOrdinalIgnoreCase(signerName, blockedVendor)) {
            isBlockedVendor = true;
            break;
        }
    }
    if (!isBlockedVendor) {
        return false;
    }

    blockedPath = std::move(resolvedPath);
    return true;
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

bool IsBlockedLoadLibraryRequest(LPCWSTR requestedPath, std::wstring& blockedPath) {
    if (g_isCheckingLoadSignature) {
        return false;
    }

    g_isCheckingLoadSignature = true;
    bool shouldBlock = false;
    try {
        shouldBlock = ShouldBlockLoadLibrary(requestedPath, blockedPath);
    } catch (...) {
        // Signature inspection is fail-open so unrelated DLL loads are not
        // broken by path, trust-provider, or allocation failures.
    }
    g_isCheckingLoadSignature = false;
    return shouldBlock;
}

bool IsBlockedLoadLibraryRequest(LPCSTR requestedPath, std::wstring& blockedPath) {
    if (g_isCheckingLoadSignature) {
        return false;
    }

    g_isCheckingLoadSignature = true;
    bool shouldBlock = false;
    try {
        std::wstring widePath = ConvertAnsiToWchar(requestedPath);
        shouldBlock = !widePath.empty() && ShouldBlockLoadLibrary(widePath.c_str(), blockedPath);
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
    if (fileSize < 0) {
        throw std::runtime_error("PE Read Error: Failed to get file size");
    }
    file.seekg(0, std::ios::beg);
    if (fileSize == 0) {
        throw std::runtime_error("PE Read Error: File is empty");
    }
    if (static_cast<unsigned long long>(fileSize) > std::numeric_limits<DWORD>::max()) {
        throw std::runtime_error("PE Read Error: File is too large");
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

    IMAGE_DOS_HEADER dosHeader = {};
    std::memcpy(&dosHeader, buffer.data(), sizeof(dosHeader));
    if (dosHeader.e_magic != IMAGE_DOS_SIGNATURE) {
        throw std::runtime_error("PE Format Error: DOS signature mismatch (not an 'MZ' file)");
    }

    if (dosHeader.e_lfanew < 0) {
        throw std::runtime_error("PE Format Error: Invalid NT header offset or file too small");
    }

    const size_t ntOffset = static_cast<size_t>(dosHeader.e_lfanew);
    if (ntOffset > buffer.size() ||
        sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) > buffer.size() - ntOffset) {
        throw std::runtime_error("PE Format Error: Invalid NT header offset or file too small");
    }

    DWORD signature = 0;
    std::memcpy(&signature, buffer.data() + ntOffset, sizeof(signature));
    if (signature != IMAGE_NT_SIGNATURE) {
        throw std::runtime_error("PE Format Error: Invalid PE signature");
    }

    return buffer;
}

bool HasBytes(const std::vector<char>& buffer, size_t offset, size_t byteCount) {
    return offset <= buffer.size() && byteCount <= buffer.size() - offset;
}

template <typename T>
bool ReadPeStructure(const std::vector<char>& buffer, size_t offset, T& value) {
    if (!HasBytes(buffer, offset, sizeof(T))) {
        return false;
    }
    std::memcpy(&value, buffer.data() + offset, sizeof(T));
    return true;
}

bool RvaToOffset(
    const std::vector<char>& buffer,
    size_t sectionTableOffset,
    WORD numberOfSections,
    DWORD sizeOfHeaders,
    DWORD rva,
    size_t requiredBytes,
    size_t& fileOffset,
    size_t& availableBytes) {
    if (rva < sizeOfHeaders) {
        const size_t headerOffset = static_cast<size_t>(rva);
        if (!HasBytes(buffer, headerOffset, requiredBytes)) {
            return false;
        }
        fileOffset = headerOffset;
        availableBytes = std::min(buffer.size() - headerOffset, static_cast<size_t>(sizeOfHeaders - rva));
        return requiredBytes <= availableBytes;
    }

    for (WORD sectionIndex = 0; sectionIndex < numberOfSections; ++sectionIndex) {
        IMAGE_SECTION_HEADER section = {};
        const size_t sectionOffset = sectionTableOffset + static_cast<size_t>(sectionIndex) * sizeof(section);
        if (!ReadPeStructure(buffer, sectionOffset, section)) {
            return false;
        }

        const uint64_t sectionStart = section.VirtualAddress;
        const uint64_t sectionSpan = std::max(section.Misc.VirtualSize, section.SizeOfRawData);
        const uint64_t sectionEnd = sectionStart + sectionSpan;
        if (rva < sectionStart || static_cast<uint64_t>(rva) >= sectionEnd) {
            continue;
        }

        const uint64_t sectionDelta = static_cast<uint64_t>(rva) - sectionStart;
        if (sectionDelta >= section.SizeOfRawData) {
            return false;
        }

        const uint64_t rawOffset = static_cast<uint64_t>(section.PointerToRawData) + sectionDelta;
        const uint64_t rawAvailable = static_cast<uint64_t>(section.SizeOfRawData) - sectionDelta;
        if (rawOffset > buffer.size()) {
            return false;
        }

        fileOffset = static_cast<size_t>(rawOffset);
        availableBytes = std::min(buffer.size() - fileOffset, static_cast<size_t>(rawAvailable));
        return requiredBytes <= availableBytes;
    }
    return false;
}

std::wstring GetImportedModules(const std::vector<char>& buffer) {
    try {
        IMAGE_DOS_HEADER dosHeader = {};
        if (!ReadPeStructure(buffer, 0, dosHeader) || dosHeader.e_lfanew < 0) {
            return L"[PE Error: Invalid DOS Header]";
        }

        const size_t ntOffset = static_cast<size_t>(dosHeader.e_lfanew);
        IMAGE_FILE_HEADER fileHeader = {};
        if (!ReadPeStructure(buffer, ntOffset + sizeof(DWORD), fileHeader)) {
            return L"[PE Error: Invalid File Header]";
        }

        const size_t optionalHeaderOffset = ntOffset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
        if (!HasBytes(buffer, optionalHeaderOffset, fileHeader.SizeOfOptionalHeader)) {
            return L"[PE Error: Invalid Optional Header]";
        }

        WORD optionalMagic = 0;
        if (!ReadPeStructure(buffer, optionalHeaderOffset, optionalMagic)) {
            return L"[PE Error: Invalid Optional Header]";
        }

        IMAGE_DATA_DIRECTORY importDirectory = {};
        DWORD sizeOfHeaders = 0;
        DWORD numberOfRvaAndSizes = 0;
        size_t dataDirectoryOffset = 0;
        if (optionalMagic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
            if (fileHeader.SizeOfOptionalHeader < offsetof(IMAGE_OPTIONAL_HEADER64, DataDirectory) ||
                !ReadPeStructure(
                    buffer,
                    optionalHeaderOffset + offsetof(IMAGE_OPTIONAL_HEADER64, SizeOfHeaders),
                    sizeOfHeaders) ||
                !ReadPeStructure(
                    buffer,
                    optionalHeaderOffset + offsetof(IMAGE_OPTIONAL_HEADER64, NumberOfRvaAndSizes),
                    numberOfRvaAndSizes)) {
                return L"[PE Error: Invalid PE32+ Optional Header]";
            }
            dataDirectoryOffset = offsetof(IMAGE_OPTIONAL_HEADER64, DataDirectory);
        } else if (optionalMagic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
            if (fileHeader.SizeOfOptionalHeader < offsetof(IMAGE_OPTIONAL_HEADER32, DataDirectory) ||
                !ReadPeStructure(
                    buffer,
                    optionalHeaderOffset + offsetof(IMAGE_OPTIONAL_HEADER32, SizeOfHeaders),
                    sizeOfHeaders) ||
                !ReadPeStructure(
                    buffer,
                    optionalHeaderOffset + offsetof(IMAGE_OPTIONAL_HEADER32, NumberOfRvaAndSizes),
                    numberOfRvaAndSizes)) {
                return L"[PE Error: Invalid PE32 Optional Header]";
            }
            dataDirectoryOffset = offsetof(IMAGE_OPTIONAL_HEADER32, DataDirectory);
        } else {
            return L"[PE Error: Unsupported Optional Header]";
        }

        if (numberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT) {
            return L"";
        }

        const size_t importDirectoryOffset =
            dataDirectoryOffset + IMAGE_DIRECTORY_ENTRY_IMPORT * sizeof(IMAGE_DATA_DIRECTORY);
        if (importDirectoryOffset + sizeof(importDirectory) > fileHeader.SizeOfOptionalHeader ||
            !ReadPeStructure(buffer, optionalHeaderOffset + importDirectoryOffset, importDirectory)) {
            return L"[PE Error: Invalid Import Data Directory]";
        }

        if (importDirectory.VirtualAddress == 0) {
            return L"";
        }

        const size_t sectionTableOffset = optionalHeaderOffset + fileHeader.SizeOfOptionalHeader;
        const size_t sectionTableSize = static_cast<size_t>(fileHeader.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER);
        if (!HasBytes(buffer, sectionTableOffset, sectionTableSize)) {
            return L"[PE Error: Invalid Section Table]";
        }

        size_t importOffset = 0;
        size_t importBytesAvailable = 0;
        if (!RvaToOffset(
                buffer,
                sectionTableOffset,
                fileHeader.NumberOfSections,
                sizeOfHeaders,
                importDirectory.VirtualAddress,
                sizeof(IMAGE_IMPORT_DESCRIPTOR),
                importOffset,
                importBytesAvailable)) {
            return L"[PE Error: Invalid Import Directory RVA]";
        }

        if (importDirectory.Size != 0) {
            importBytesAvailable = std::min(importBytesAvailable, static_cast<size_t>(importDirectory.Size));
        }

        std::wstringstream result;
        bool firstModule = true;
        for (size_t descriptorOffset = 0;
             descriptorOffset <= importBytesAvailable - sizeof(IMAGE_IMPORT_DESCRIPTOR);
             descriptorOffset += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
            IMAGE_IMPORT_DESCRIPTOR importDescriptor = {};
            if (!ReadPeStructure(buffer, importOffset + descriptorOffset, importDescriptor)) {
                return L"[PE Error: Truncated Import Directory]";
            }
            if (importDescriptor.Name == 0) {
                return result.str();
            }

            size_t nameOffset = 0;
            size_t nameBytesAvailable = 0;
            if (!RvaToOffset(
                    buffer,
                    sectionTableOffset,
                    fileHeader.NumberOfSections,
                    sizeOfHeaders,
                    importDescriptor.Name,
                    1,
                    nameOffset,
                    nameBytesAvailable)) {
                continue;
            }

            const void* terminator = std::memchr(buffer.data() + nameOffset, '\0', nameBytesAvailable);
            if (!terminator) {
                return L"[PE Error: Unterminated Import Name]";
            }

            const char* nameStart = buffer.data() + nameOffset;
            const size_t nameLength = static_cast<const char*>(terminator) - nameStart;
            if (!firstModule) {
                result << L", ";
            }
            result << ConvertCharToWchar(std::string(nameStart, nameLength).c_str());
            firstModule = false;
        }

        return L"[PE Error: Unterminated Import Directory]";
    } catch (const std::exception& e) {
        return ConvertCharToWchar(e.what());
    } catch (...) {
        return L"[Unknown Exception in GetImportedModules]";
    }
}

std::wstring GetFileTimestamp(LPCWSTR filePath, bool isCreationTime) {
    ScopedHandle hFile(CreateFileW(
        filePath,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr));
    if (hFile.get() == INVALID_HANDLE_VALUE) {
        return L"[Error Reading Time]";
    }

    FILETIME ft = {}, ftLocal = {};
    SYSTEMTIME st = {};
    if (!GetFileTime(hFile.get(), isCreationTime ? &ft : nullptr, nullptr, isCreationTime ? nullptr : &ft) ||
        !FileTimeToLocalFileTime(&ft, &ftLocal) ||
        !FileTimeToSystemTime(&ftLocal, &st)) {
        return L"[Error Reading Time]";
    }

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

// Log module info to Minecraft console.
// Format: <eventName> <base64_dllPath> <fileHash> <base64_signerName> <base64_imports>
void LogModuleToMinecraft(const ModuleInfo& info, const std::string& eventName = "moduleLoaded") {
    try {
        // Convert to strings and encode
        std::string encodedPath = Base64Encode(ConvertWcharToChar(info.path));
        std::string hash = ConvertWcharToChar(info.hash);
        std::string encodedSigner = Base64Encode(ConvertWcharToChar(info.signerName));
        std::string encodedImports = EncodeImportsList(ConvertWcharToChar(info.importedModules));

        std::string formattedMessage = eventName + " " + encodedPath + " " + hash + " " + encodedSigner + " " + encodedImports;
        
        LogToMinecraft(formattedMessage);
    } catch (const std::exception& e) {
        LogErrorToMinecraft("ModuleLogError", std::string("LogModuleToMinecraft Error: ") + e.what());
    } catch (...) {
        LogErrorToMinecraft("ModuleLogError", "LogModuleToMinecraft Error: Unknown exception");
    }
}

bool IsValidSha512(const std::wstring& hash) {
    return hash.size() == 128 && std::all_of(hash.begin(), hash.end(), [](wchar_t character) {
        return (character >= L'0' && character <= L'9') ||
            (character >= L'a' && character <= L'f') ||
            (character >= L'A' && character <= L'F');
    });
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
                    
                    std::string hash = ConvertWcharToChar(info.hash);

                    // Add to seen hashes cache
                    if (IsValidSha512(info.hash)) {
                        std::lock_guard<std::mutex> lock(g_seenHashesMutex);
                        g_seenHashes.insert(hash);
                    }

                    LogModuleToMinecraft(info);
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

std::wstring GetLoadedModulePath(HMODULE hModule) {
    std::vector<wchar_t> pathBuffer(MAX_PATH);
    while (pathBuffer.size() <= 32768) {
        DWORD length = GetModuleFileNameW(hModule, pathBuffer.data(), static_cast<DWORD>(pathBuffer.size()));
        if (length == 0) {
            return L"";
        }
        if (length < pathBuffer.size()) {
            return std::wstring(pathBuffer.data(), length);
        }
        pathBuffer.resize(pathBuffer.size() * 2);
    }
    return L"";
}

void HandleLoadedModule(HMODULE hModule) noexcept {
    if (!hModule) return;

    try {
        std::wstring loadedPath = GetLoadedModulePath(hModule);
        if (loadedPath.empty()) {
            LogErrorToMinecraft("ModuleAnalysisError", "Could not get module file name");
            return;
        }

        // Defer expensive module analysis to a background thread.
        std::thread([path = std::move(loadedPath)]() {
            try {
                std::wstring hashW = CalculateSHA512(path.c_str());
                std::string hash = ConvertWcharToChar(hashW);

                // Hash failures must not deduplicate unrelated modules under a
                // shared error string.
                if (IsValidSha512(hashW)) {
                    std::lock_guard<std::mutex> lock(g_seenHashesMutex);
                    if (g_seenHashes.count(hash) > 0) {
                        return;
                    }
                    g_seenHashes.insert(hash);
                }

                ModuleInfo info = AnalyzeModule(path);
                LogModuleToMinecraft(info);
            } catch (const std::exception& e) {
                LogErrorToMinecraft("ModuleAnalysisError", std::string(e.what()));
            } catch (...) {
                LogErrorToMinecraft("ModuleAnalysisError", "Unknown exception");
            }
        }).detach();
    } catch (const std::exception& e) {
        try {
            LogErrorToMinecraft("ModuleAnalysisError", std::string(e.what()));
        } catch (...) {
        }
    } catch (...) {
        try {
            LogErrorToMinecraft("ModuleAnalysisError", "Unknown exception");
        } catch (...) {
        }
    }
}

void HandleBlockedModule(const std::wstring& blockedPath) noexcept {
    try {
        std::thread([path = blockedPath]() {
            try {
                ModuleInfo info = AnalyzeModule(path);
                LogModuleToMinecraft(info, "moduleBlocked");
            } catch (const std::exception& e) {
                LogErrorToMinecraft("BlockedModuleAnalysisError", std::string(e.what()));
            } catch (...) {
                LogErrorToMinecraft("BlockedModuleAnalysisError", "Unknown exception");
            }
        }).detach();
    } catch (const std::exception& e) {
        try {
            LogErrorToMinecraft("BlockedModuleAnalysisError", std::string(e.what()));
        } catch (...) {
        }
    } catch (...) {
        try {
            LogErrorToMinecraft("BlockedModuleAnalysisError", "Unknown exception");
        } catch (...) {
        }
    }
}

HMODULE WINAPI DetourLoadLibraryW(LPCWSTR lpLibFileName) {
    std::wstring blockedPath;
    if (IsBlockedLoadLibraryRequest(lpLibFileName, blockedPath)) {
        HandleBlockedModule(blockedPath);
        SetLastError(ERROR_ACCESS_DENIED);
        return nullptr;
    }

    HMODULE hModule = pOriginalLoadLibraryW(lpLibFileName);
    HandleLoadedModule(hModule);
    return hModule;
}

HMODULE WINAPI DetourLoadLibraryA(LPCSTR lpLibFileName) {
    std::wstring blockedPath;
    if (IsBlockedLoadLibraryRequest(lpLibFileName, blockedPath)) {
        HandleBlockedModule(blockedPath);
        SetLastError(ERROR_ACCESS_DENIED);
        return nullptr;
    }

    HMODULE hModule = pOriginalLoadLibraryA(lpLibFileName);
    HandleLoadedModule(hModule);
    return hModule;
}

HMODULE WINAPI DetourLoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    std::wstring blockedPath;
    if (IsBlockedLoadLibraryRequest(lpLibFileName, blockedPath)) {
        HandleBlockedModule(blockedPath);
        SetLastError(ERROR_ACCESS_DENIED);
        return nullptr;
    }

    HMODULE hModule = pOriginalLoadLibraryExW(lpLibFileName, hFile, dwFlags);
    HandleLoadedModule(hModule);
    return hModule;
}

HMODULE WINAPI DetourLoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    std::wstring blockedPath;
    if (IsBlockedLoadLibraryRequest(lpLibFileName, blockedPath)) {
        HandleBlockedModule(blockedPath);
        SetLastError(ERROR_ACCESS_DENIED);
        return nullptr;
    }

    HMODULE hModule = pOriginalLoadLibraryExA(lpLibFileName, hFile, dwFlags);
    HandleLoadedModule(hModule);
    return hModule;
}

void RunBackgroundWorkers() {
    try {
        std::thread(RunInitialScanOptimized).detach();
    } catch (const std::exception& e) {
        LogErrorToMinecraft("InitialScanFatalError", std::string("Failed to start initial scan: ") + e.what());
    } catch (...) {
        LogErrorToMinecraft("InitialScanFatalError", "Failed to start initial scan: Unknown exception");
    }
    RunJniLogWorker();
}

DWORD WINAPI BackgroundWorkerThreadProc(LPVOID) {
    RunBackgroundWorkers();
    return 0;
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

        // CreateThread returns without waiting for the new thread to run. Its
        // entry point starts after DllMain releases the loader lock.
        HANDLE backgroundThread = CreateThread(nullptr, 0, BackgroundWorkerThreadProc, nullptr, 0, nullptr);
        if (!backgroundThread) {
            MH_DisableHook(MH_ALL_HOOKS);
            MH_Uninitialize();
            return FALSE;
        }
        CloseHandle(backgroundThread);

    } else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        g_shuttingDown.store(true, std::memory_order_release);
        g_jniLogCondition.notify_all();
        // The OS is already tearing down the process when lpReserved is set;
        // avoid loader-lock work that cannot provide any useful cleanup.
        if (lpReserved == nullptr) {
            MH_DisableHook(MH_ALL_HOOKS);
            MH_Uninitialize();
        }
    }
    return TRUE;
}
