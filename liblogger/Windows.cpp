#include "MinHook.h"
#include "base64.h"
#include <jni.h>
#include <jvmti.h>
#include <Windows.h>
#include <atomic>
#include <string>
#include <string_view>
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
#include <unordered_set>
#include <mscat.h>

#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "Crypt32.lib")
#pragma comment(lib, "user32.lib")

void LogToMinecraft(const std::string& message);
void LogErrorToMinecraft(const std::string& eventName, const std::string& errorMessage);

// Global flag to check if Fairplay was detected
std::atomic<bool> g_FairplayDetected(false);

// Global cache for seen module hashes to skip expensive detection
std::mutex g_seenHashesMutex;
std::unordered_set<std::string> g_seenHashes;

#ifndef LIBLOGGER_VERSION_STR
#define LIBLOGGER_VERSION_STR "1.0.2"
#endif

// Version number
const std::string LIBLOGGER_VERSION = LIBLOGGER_VERSION_STR;

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
    
    while (true) {
        bool hasWindow = false;
        EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&hasWindow));
        
        if (hasWindow) {
            return true;
        }
        
        Sleep(retryDelayMs);
    }
    
    return false;
}

// Check if Fairplay class is loaded using JVMTI (only called once)
// Must use JVMTI because FindClass won't work across different classloaders
bool IsFairplayLoaded(JavaVM* jvm) {
    jvmtiEnv* jvmti = nullptr;
    if (jvm->GetEnv(reinterpret_cast<void**>(&jvmti), JVMTI_VERSION_1_0) != JNI_OK || jvmti == nullptr) {
        return false;
    }
    
    jint classCount = 0;
    jclass* classes = nullptr;
    jvmtiError error = jvmti->GetLoadedClasses(&classCount, &classes);
    
    if (error != JVMTI_ERROR_NONE) {
        return false;
    }
    
    static constexpr std::string_view targetSignature = "Lexersolver/mcsrfairplay/natives/NativeCallback;";
    
    bool found = false;
    
    // Check all loaded classes
    for (jint i = 0; i < classCount && !found; i++) {
        char* signature = nullptr;
        error = jvmti->GetClassSignature(classes[i], &signature, nullptr);
        
        if (error == JVMTI_ERROR_NONE && signature != nullptr) {
            if (signature == targetSignature) {
                found = true;
            }
            jvmti->Deallocate(reinterpret_cast<unsigned char*>(signature));
        }
    }    
    jvmti->Deallocate(reinterpret_cast<unsigned char*>(classes));
    return found;
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

        CRYPT_PROVIDER_SGNR* pSgnr = WTHelperGetProvSignerFromChain(pProvData, 0, FALSE, 0);
        if (!pSgnr || !pSgnr->pChainContext || pSgnr->pChainContext->cChain == 0 || 
            pSgnr->pChainContext->rgpChain[0]->cElement == 0) {
            return isCatalog ? L"[Signed via Catalog, Signer Not Found]" : L"[Signed, Signer Not Found]";
        }

        PCCERT_CONTEXT pCertContext = pSgnr->pChainContext->rgpChain[0]->rgpElement[0]->pCertContext;
        if (!pCertContext) {
            return isCatalog ? L"[Signed via Catalog, Cert Not Found]" : L"[Signed, Cert Not Found]";
        }

        WCHAR szName[256];
        if (CertGetNameStringW(pCertContext, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, szName, 256) > 1) {
            return isCatalog ? std::wstring(szName) + L" (Catalog)" : std::wstring(szName);
        }
        return isCatalog ? L"[Signed via Catalog, No Subject]" : L"[Signed, No Subject]";
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

void BackOff() {
    // Log to Minecraft (ignore exceptions)
    try {
        LogToMinecraft("Skipping LibLogger due to Fairplay detection");
    } catch (...) {}

    // Disable hooks and uninitialize (ignore exceptions)
    try {
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
    } catch (...) {}

    // Get handle to our DLL and unload it (ignore exceptions)
    try {
        HMODULE hOurModule = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&BackOff), &hOurModule);
        if (hOurModule) {
            FreeLibraryAndExitThread(hOurModule, 0);
        }
    } catch (...) {}
}

void LogToMinecraft(const std::string& message) {
    try {
        JavaVM* jvm = nullptr;
        JNIEnv* env = nullptr;
        
        // Get JVM
        jsize vm_count = 0;
        if (DynamicGetCreatedJavaVMs(&jvm, 1, &vm_count) != JNI_OK || vm_count == 0) {
            return;
        }
        
        // Try to get env first without attaching
        jint getEnvResult = jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
        bool needsDetach = false;
        
        if (getEnvResult == JNI_EDETACHED) {
            if (jvm->AttachCurrentThread(reinterpret_cast<void**>(&env), nullptr) != JNI_OK) {
                return;
            }
            needsDetach = true;
        } else if (getEnvResult != JNI_OK) {
            return;
        }

        jclass systemClass = env->FindClass("java/lang/System");
        if (!systemClass) {
            if (needsDetach) jvm->DetachCurrentThread();
            return;
        }

        jfieldID outFieldID = env->GetStaticFieldID(systemClass, "out", "Ljava/io/PrintStream;");
        if (!outFieldID) {
            env->DeleteLocalRef(systemClass);
            if (needsDetach) jvm->DetachCurrentThread();
            return;
        }

        jobject printStream = env->GetStaticObjectField(systemClass, outFieldID);
        if (!printStream) {
            env->DeleteLocalRef(systemClass);
            if (needsDetach) jvm->DetachCurrentThread();
            return;
        }

        jclass printStreamClass = env->GetObjectClass(printStream);
        if (!printStreamClass) {
            env->DeleteLocalRef(printStream);
            env->DeleteLocalRef(systemClass);
            if (needsDetach) jvm->DetachCurrentThread();
            return;
        }

        jmethodID printlnMethod = env->GetMethodID(printStreamClass, "println", "(Ljava/lang/String;)V");
        if (!printlnMethod) {
            env->DeleteLocalRef(printStreamClass);
            env->DeleteLocalRef(printStream);
            env->DeleteLocalRef(systemClass);
            if (needsDetach) jvm->DetachCurrentThread();
            return;
        }

        jstring logMessage = env->NewStringUTF(message.c_str());
        env->CallVoidMethod(printStream, printlnMethod, logMessage);
        
        env->DeleteLocalRef(logMessage);
        env->DeleteLocalRef(printStreamClass);
        env->DeleteLocalRef(printStream);
        env->DeleteLocalRef(systemClass);
        if (needsDetach) jvm->DetachCurrentThread();
    } catch (...) {
        // Silently fail to avoid infinite recursion
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
        // Check if Fairplay was detected
        if (g_FairplayDetected) {
            return;
        }
        
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
        
        // Get JVM
        JavaVM* jvm = nullptr;
        jsize vm_count = 0;
        if (DynamicGetCreatedJavaVMs(&jvm, 1, &vm_count) != JNI_OK || vm_count == 0) {
            LogErrorToMinecraft("InitialScanError", "JVM not available");
            return;
        }
        
        // Check for Fairplay using JVMTI once before proceeding
        if (IsFairplayLoaded(jvm)) {
            g_FairplayDetected = true;
            BackOff();
            return;
        }
        
        // Log version number to Minecraft
        LogToMinecraft("Running LibLogger v" + LIBLOGGER_VERSION + " for verification purposes");
        
        // Try to get env first without attaching (in case we're already on a JVM thread)
        JNIEnv* env = nullptr;
        jint getEnvResult = jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
        bool needsDetach = false;
        
        if (getEnvResult == JNI_EDETACHED) {
            // Not attached yet, attach now
            if (jvm->AttachCurrentThread(reinterpret_cast<void**>(&env), nullptr) != JNI_OK) {
                LogErrorToMinecraft("InitialScanError", "Could not attach thread");
                return;
            }
            needsDetach = true;
        } else if (getEnvResult != JNI_OK) {
            LogErrorToMinecraft("InitialScanError", "Could not get JNI environment");
            return;
        }

        // Get System.out for logging
        jclass systemClass = env->FindClass("java/lang/System");
        if (!systemClass) {
            if (needsDetach) jvm->DetachCurrentThread();
            return;
        }

        jfieldID outFieldID = env->GetStaticFieldID(systemClass, "out", "Ljava/io/PrintStream;");
        if (!outFieldID) {
            env->DeleteLocalRef(systemClass);
            if (needsDetach) jvm->DetachCurrentThread();
            return;
        }

        jobject printStream = env->GetStaticObjectField(systemClass, outFieldID);
        if (!printStream) {
            env->DeleteLocalRef(systemClass);
            if (needsDetach) jvm->DetachCurrentThread();
            return;
        }

        jclass printStreamClass = env->GetObjectClass(printStream);
        if (!printStreamClass) {
            env->DeleteLocalRef(printStream);
            env->DeleteLocalRef(systemClass);
            if (needsDetach) jvm->DetachCurrentThread();
            return;
        }

        jmethodID printlnMethod = env->GetMethodID(printStreamClass, "println", "(Ljava/lang/String;)V");
        if (!printlnMethod) {
            env->DeleteLocalRef(printStreamClass);
            env->DeleteLocalRef(printStream);
            env->DeleteLocalRef(systemClass);
            if (needsDetach) jvm->DetachCurrentThread();
            return;
        }

        // Enumerate all modules
        ScopedHandle hModuleSnap(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId()));
        if (hModuleSnap.get() == INVALID_HANDLE_VALUE) {
            env->DeleteLocalRef(printStreamClass);
            env->DeleteLocalRef(printStream);
            env->DeleteLocalRef(systemClass);
            if (needsDetach) jvm->DetachCurrentThread();
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
                    
                    jstring logMessage = env->NewStringUTF(formattedMessage.c_str());
                    
                    // Log to System.out
                    env->CallVoidMethod(printStream, printlnMethod, logMessage);
                    env->DeleteLocalRef(logMessage);
                } catch (const std::exception& e) {
                    LogErrorToMinecraft("ModuleAnalysisError", std::string("Initial Scan Module Analysis Error: ") + e.what());
                } catch (...) {
                    LogErrorToMinecraft("ModuleAnalysisError", "Initial Scan Module Analysis Error: Unknown exception");
                }
                
            } while (Module32NextW(hModuleSnap.get(), &me32));
        } else {
            LogErrorToMinecraft("InitialScanError", "Could not enumerate first module");
        }

        env->DeleteLocalRef(printStreamClass);
        env->DeleteLocalRef(printStream);
        env->DeleteLocalRef(systemClass);
        jvm->DetachCurrentThread();
    } catch (const std::exception& e) {
        LogErrorToMinecraft("InitialScanFatalError", std::string("Initial Scan Fatal Error: ") + e.what());
    } catch (...) {
        LogErrorToMinecraft("InitialScanFatalError", "Initial Scan Fatal Error: Unknown exception");
    }
}

void HandleLoadedModule(HMODULE hModule) {
    if (!hModule) return;
    
    // Check if Fairplay was already detected
    if (g_FairplayDetected) {
        return;
    }
    
    WCHAR loadedPath[MAX_PATH];
    if (GetModuleFileNameW(hModule, loadedPath, MAX_PATH) == 0) {
        std::thread([]() {
            AttachThreadWithName("LibLogger");
            LogErrorToMinecraft("ModuleAnalysisError", "Could not get module file name");
        }).detach();
        return;
    }
    
    // Defer expensive module analysis to background thread
    std::thread([path = std::wstring(loadedPath)]() {
        AttachThreadWithName("LibLogger");
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
    HMODULE hModule = pOriginalLoadLibraryW(lpLibFileName);
    HandleLoadedModule(hModule);
    return hModule;
}

HMODULE WINAPI DetourLoadLibraryA(LPCSTR lpLibFileName) {
    HMODULE hModule = pOriginalLoadLibraryA(lpLibFileName);
    HandleLoadedModule(hModule);
    return hModule;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);

        // Initialize MinHook
        if (MH_Initialize() != MH_OK) return FALSE;

        // Create hooks for LoadLibraryW and LoadLibraryA
        if (MH_CreateHook(&LoadLibraryW, &DetourLoadLibraryW, reinterpret_cast<LPVOID*>(&pOriginalLoadLibraryW)) != MH_OK ||
            MH_CreateHook(&LoadLibraryA, &DetourLoadLibraryA, reinterpret_cast<LPVOID*>(&pOriginalLoadLibraryA)) != MH_OK) {
            MH_Uninitialize();
            return FALSE;
        }

        // Enable all hooks
        if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
            MH_Uninitialize();
            return FALSE;
        }

        // Run initial scan in a separate thread (don't check in DllMain - can cause deadlock)
        std::thread([]() {
            AttachThreadWithName("LibLogger");
            RunInitialScanOptimized();
        }).detach();

    } else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        // Cleanup
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
    }
    return TRUE;
}
