#include "utils.h"
#include "common/video_media.h"
#include "features/game_state_source.h"
#include "gui/gui.h"
#include "hooks/input_hook.h"
#include "runtime/logic_thread.h"
#include "render/mirror_thread.h"
#include "profiler.h"

#include <nlohmann/json.hpp>
#include <chrono>
#include <optional>

extern std::atomic<GLuint> g_cachedGameTextureId;

#include "third_party/stb_image.h"
#include <DbgHelp.h>
#include <ShlObj.h>
#include <Shlwapi.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <eh.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <shared_mutex>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "DbgHelp.lib")

static std::atomic<bool> g_symbolsInitialized{ false };
static std::atomic<bool> g_symbolInitAttempted{ false };
static std::mutex g_symbolMutex;

namespace {
constexpr size_t kMaxGifLoadBytes = 256ull * 1024ull * 1024ull;
constexpr size_t kMaxDecodedImageBytes = 512ull * 1024ull * 1024ull;
constexpr size_t kMaxScreenshotBytes = 512ull * 1024ull * 1024ull;

bool TryMultiplySize(size_t left, size_t right, size_t& out) {
    if (left == 0 || right == 0) {
        out = 0;
        return true;
    }
    if (left > (std::numeric_limits<size_t>::max)() / right) { return false; }
    out = left * right;
    return true;
}

bool TryComputeImageByteCount(int width, int height, int channels, size_t& outBytes) {
    if (width <= 0 || height <= 0 || channels <= 0) { return false; }

    size_t pixelCount = 0;
    if (!TryMultiplySize(static_cast<size_t>(width), static_cast<size_t>(height), pixelCount)) { return false; }
    return TryMultiplySize(pixelCount, static_cast<size_t>(channels), outBytes);
}

std::string FormatByteCount(size_t bytes) {
    const size_t mib = 1024ull * 1024ull;
    return std::to_string(bytes) + " bytes (" + std::to_string(bytes / mib) + " MiB)";
}

size_t GetConfiguredVideoCacheBudgetBytes(int budgetMiB) {
    if (budgetMiB <= 0) {
        return 0;
    }

    return static_cast<size_t>(budgetMiB) * 1024ull * 1024ull;
}
} // namespace

void EnsureSymbolsInitialized();

std::string ResolveStackFrame(void* address) {
    EnsureSymbolsInitialized();

    if (!g_symbolsInitialized.load()) {
        std::stringstream ss;
        ss << "0x" << std::hex << reinterpret_cast<uintptr_t>(address);
        return ss.str();
    }

    std::lock_guard<std::mutex> lock(g_symbolMutex);

    HANDLE process = GetCurrentProcess();
    DWORD64 addr64 = reinterpret_cast<DWORD64>(address);

    char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)];
    PSYMBOL_INFO symbol = reinterpret_cast<PSYMBOL_INFO>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;

    std::stringstream result;
    result << "0x" << std::hex << addr64;

    DWORD64 displacement = 0;
    if (SymFromAddr(process, addr64, &displacement, symbol)) {
        result << " " << symbol->Name;
        if (displacement != 0) { result << "+0x" << std::hex << displacement; }

        IMAGEHLP_LINE64 line;
        line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
        DWORD lineDisplacement = 0;
        if (SymGetLineFromAddr64(process, addr64, &lineDisplacement, &line)) {
            const char* filename = strrchr(line.FileName, '\\');
            if (!filename)
                filename = line.FileName;
            else
                filename++;

            result << " [" << filename << ":" << std::dec << line.LineNumber << "]";
        }
    }

    return result.str();
}

// Helper to format full stack trace with symbols
std::string FormatStackTraceWithSymbols(void** stack, USHORT frames, int skipFrames = 0) {
    std::stringstream ss;
    ss << "Stack trace (" << std::dec << (frames - skipFrames) << " frames):";
    for (USHORT i = skipFrames; i < frames; i++) { ss << "\n  [" << std::dec << (i - skipFrames) << "] " << ResolveStackFrame(stack[i]); }
    return ss.str();
}

void SignalHandler(int sig) {
    if (sig == SIGABRT) {
        Log("!!! SIGABRT SIGNAL RECEIVED - ABNORMAL TERMINATION !!!");
        FlushLogs();

        // Capture stack trace
        void* stack[64];
        USHORT frames = CaptureStackBackTrace(1, 64, stack, NULL);
        Log(FormatStackTraceWithSymbols(stack, frames));
        FlushLogs(); // Force flush after stack trace
    }

    signal(sig, SIG_DFL);
    raise(sig);
}

extern "C" void abort() {
    static std::atomic<bool> abortInProgress{ false };
    if (abortInProgress.exchange(true)) {
        signal(SIGABRT, SIG_DFL);
        raise(SIGABRT);
        TerminateProcess(GetCurrentProcess(), 3);
    }

    DWORD lastError = GetLastError();
    DWORD threadId = GetCurrentThreadId();
    DWORD processId = GetCurrentProcessId();

    std::stringstream contextSs;
    contextSs << "=== ABORT() CALLED ===" << std::endl;
    contextSs << "Thread ID: " << threadId << std::endl;
    contextSs << "Process ID: " << processId << std::endl;
    if (lastError != 0) {
        contextSs << "GetLastError: " << lastError << " (0x" << std::hex << lastError << std::dec << ")";

        LPSTR messageBuffer = nullptr;
        size_t size = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
                                     lastError, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&messageBuffer, 0, NULL);
        if (size > 0 && messageBuffer) {
            std::string errorMsg(messageBuffer, size);
            while (!errorMsg.empty() && (errorMsg.back() == '\n' || errorMsg.back() == '\r')) { errorMsg.pop_back(); }
            contextSs << " - " << errorMsg;
            LocalFree(messageBuffer);
        }
        contextSs << std::endl;
    }

    std::exception_ptr eptr = std::current_exception();
    if (eptr) {
        try {
            std::rethrow_exception(eptr);
        } catch (const std::exception& e) { contextSs << "Active C++ Exception: " << e.what() << std::endl; } catch (...) {
            contextSs << "Active C++ Exception: (unknown type)" << std::endl;
        }
    }

    Log(contextSs.str());
    FlushLogs();

    // Capture stack trace at abort point
    void* stack[64];
    USHORT frames = CaptureStackBackTrace(1, 64, stack, NULL);
    Log(FormatStackTraceWithSymbols(stack, frames));
    FlushLogs(); // Force flush after stack trace

    signal(SIGABRT, SIG_DFL);
    raise(SIGABRT);

    TerminateProcess(GetCurrentProcess(), 3);
}

std::string GetTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    struct tm timeinfo;
    localtime_s(&timeinfo, &time_t);

    std::stringstream ss;
    ss << std::put_time(&timeinfo, "%H:%M:%S");
    ss << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return ss.str();
}

bool IsWindowInForegroundTree(HWND hwnd) {
    if (hwnd == NULL || !IsWindow(hwnd)) { return false; }

    HWND foreground = GetForegroundWindow();
    if (foreground == NULL || !IsWindow(foreground)) { return false; }

    HWND hwndRootOwner = GetAncestor(hwnd, GA_ROOTOWNER);
    if (hwndRootOwner == NULL) { hwndRootOwner = hwnd; }

    HWND foregroundRootOwner = GetAncestor(foreground, GA_ROOTOWNER);
    if (foregroundRootOwner == NULL) { foregroundRootOwner = foreground; }

    return hwndRootOwner == foregroundRootOwner;
}


static uint32_t g_crc32Table[256];
static std::once_flag g_crc32InitFlag;

static void InitCrc32Table() {
    std::call_once(g_crc32InitFlag, []() {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int j = 0; j < 8; j++) { c = (c & 1u) ? ((c >> 1) ^ 0xEDB88320u) : (c >> 1); }
            g_crc32Table[i] = c;
        }
    });
}

static uint32_t Crc32(const uint8_t* data, size_t size) {
    InitCrc32Table();
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; i++) { crc = g_crc32Table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8); }
    return crc ^ 0xFFFFFFFFu;
}

static bool FileExistsW(const std::wstring& path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    return (attrs != INVALID_FILE_ATTRIBUTES) && ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0);
}

static bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& out) {
    // Open via std::filesystem::path so wide Win32 APIs are used.
    std::ifstream in(std::filesystem::path(path), std::ios::binary | std::ios::ate);
    if (!in.is_open()) return false;

    std::streamoff size = in.tellg();
    if (size < 0) return false;
    in.seekg(0, std::ios::beg);

    out.resize(static_cast<size_t>(size));
    if (!out.empty()) {
        in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
        if (in.bad() || static_cast<size_t>(in.gcount()) != out.size()) return false;
    }
    return true;
}

static void WriteLE32(std::ofstream& out, uint32_t v) {
    uint8_t b[4] = { (uint8_t)(v & 0xFFu), (uint8_t)((v >> 8) & 0xFFu), (uint8_t)((v >> 16) & 0xFFu), (uint8_t)((v >> 24) & 0xFFu) };
    out.write(reinterpret_cast<const char*>(b), 4);
}

static uint16_t ReverseBits(uint16_t v, uint8_t bitCount) {
    uint16_t r = 0;
    for (uint8_t i = 0; i < bitCount; i++) {
        r = static_cast<uint16_t>((r << 1) | (v & 1u));
        v >>= 1;
    }
    return r;
}

struct HuffCode {
    uint16_t code = 0;
    uint8_t bits = 0;
};

struct BitWriter {
    std::vector<uint8_t> bytes;
    uint32_t bitBuffer = 0;
    int bitCount = 0;

    void WriteBits(uint32_t value, int count) {
        bitBuffer |= ((value & ((1u << count) - 1u)) << bitCount);
        bitCount += count;
        while (bitCount >= 8) {
            bytes.push_back(static_cast<uint8_t>(bitBuffer & 0xFFu));
            bitBuffer >>= 8;
            bitCount -= 8;
        }
    }

    void FlushToByteBoundary() {
        if (bitCount > 0) {
            bytes.push_back(static_cast<uint8_t>(bitBuffer & 0xFFu));
            bitBuffer = 0;
            bitCount = 0;
        }
    }
};

static void BuildCanonicalCodes(const uint8_t* lengths, size_t count, std::vector<HuffCode>& out) {
    out.assign(count, {});

    int blCount[16] = { 0 };
    for (size_t i = 0; i < count; i++) {
        if (lengths[i] > 0 && lengths[i] <= 15) blCount[lengths[i]]++;
    }

    int nextCode[16] = { 0 };
    int code = 0;
    for (int bits = 1; bits <= 15; bits++) {
        code = (code + blCount[bits - 1]) << 1;
        nextCode[bits] = code;
    }

    for (size_t symbol = 0; symbol < count; symbol++) {
        uint8_t len = lengths[symbol];
        if (len == 0) continue;
        uint16_t c = static_cast<uint16_t>(nextCode[len]++);
        out[symbol].bits = len;
        out[symbol].code = ReverseBits(c, len);
    }
}

static std::once_flag g_fixedCodesInitFlag;
static std::vector<HuffCode> g_fixedLitLenCodes;
static std::vector<HuffCode> g_fixedDistCodes;

static void InitFixedCodes() {
    std::call_once(g_fixedCodesInitFlag, []() {
        std::array<uint8_t, 288> ll{};
        for (int i = 0; i <= 143; i++) ll[i] = 8;
        for (int i = 144; i <= 255; i++) ll[i] = 9;
        for (int i = 256; i <= 279; i++) ll[i] = 7;
        for (int i = 280; i <= 287; i++) ll[i] = 8;
        BuildCanonicalCodes(ll.data(), ll.size(), g_fixedLitLenCodes);

        std::array<uint8_t, 32> dd{};
        dd.fill(5);
        BuildCanonicalCodes(dd.data(), dd.size(), g_fixedDistCodes);
    });
}

struct DeflateToken {
    bool isMatch = false;
    uint16_t literal = 0;
    uint16_t length = 0;
    uint16_t distance = 0;
};

static std::vector<DeflateToken> BuildLz77Tokens(const std::vector<uint8_t>& data) {
    std::vector<DeflateToken> tokens;
    tokens.reserve(data.size());

    const size_t n = data.size();
    size_t i = 0;
    while (i < n) {
        size_t bestLen = 0;
        size_t bestDist = 0;

        if (i + 3 <= n) {
            const size_t windowStart = (i > 32768) ? (i - 32768) : 0;
            size_t attempts = 0;

            for (size_t j = i; j > windowStart && attempts < 2048;) {
                --j;
                if (data[j] != data[i]) continue;

                const size_t maxLen = std::min<size_t>(258, n - i);
                size_t len = 1;
                while (len < maxLen && data[j + len] == data[i + len]) len++;

                if (len >= 3 && len > bestLen) {
                    bestLen = len;
                    bestDist = i - j;
                    if (len == 258) break;
                }
                attempts++;
            }
        }

        if (bestLen >= 3) {
            DeflateToken t;
            t.isMatch = true;
            t.length = static_cast<uint16_t>(bestLen);
            t.distance = static_cast<uint16_t>(bestDist);
            tokens.push_back(t);
            i += bestLen;
        } else {
            DeflateToken t;
            t.isMatch = false;
            t.literal = data[i];
            tokens.push_back(t);
            i++;
        }
    }

    return tokens;
}

static bool EncodeLength(uint16_t length, int& outCode, int& outExtraBits, int& outExtraValue) {
    static const int kLenBase[29] = { 3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
    static const int kLenExtra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };

    if (length < 3 || length > 258) return false;

    for (int i = 0; i < 29; i++) {
        int base = kLenBase[i];
        int extra = kLenExtra[i];
        int maxLen = base + ((1 << extra) - 1);
        if (length >= base && length <= maxLen) {
            outCode = 257 + i;
            outExtraBits = extra;
            outExtraValue = length - base;
            return true;
        }
    }

    return false;
}

static bool EncodeDistance(uint16_t distance, int& outCode, int& outExtraBits, int& outExtraValue) {
    static const int kDistBase[30] = { 1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                       193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
    static const int kDistExtra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

    if (distance < 1 || distance > 32768) return false;

    for (int i = 0; i < 30; i++) {
        int base = kDistBase[i];
        int extra = kDistExtra[i];
        int maxDist = base + ((1 << extra) - 1);
        if (distance >= base && distance <= maxDist) {
            outCode = i;
            outExtraBits = extra;
            outExtraValue = distance - base;
            return true;
        }
    }

    return false;
}

static bool WriteFixedDeflateStream(const std::vector<uint8_t>& input, std::vector<uint8_t>& outDeflate) {
    InitFixedCodes();
    if (g_fixedLitLenCodes.size() < 288 || g_fixedDistCodes.size() < 30) return false;

    std::vector<DeflateToken> tokens = BuildLz77Tokens(input);

    BitWriter w;
    // Final block + fixed Huffman block type
    w.WriteBits(1, 1);
    w.WriteBits(0b01, 2);

    for (const DeflateToken& t : tokens) {
        if (!t.isMatch) {
            const HuffCode& hc = g_fixedLitLenCodes[t.literal];
            w.WriteBits(hc.code, hc.bits);
            continue;
        }

        int lenCode = 0, lenExtraBits = 0, lenExtraValue = 0;
        int distCode = 0, distExtraBits = 0, distExtraValue = 0;

        if (!EncodeLength(t.length, lenCode, lenExtraBits, lenExtraValue)) return false;
        if (!EncodeDistance(t.distance, distCode, distExtraBits, distExtraValue)) return false;

        const HuffCode& lenH = g_fixedLitLenCodes[lenCode];
        w.WriteBits(lenH.code, lenH.bits);
        if (lenExtraBits > 0) w.WriteBits(static_cast<uint32_t>(lenExtraValue), lenExtraBits);

        const HuffCode& distH = g_fixedDistCodes[distCode];
        w.WriteBits(distH.code, distH.bits);
        if (distExtraBits > 0) w.WriteBits(static_cast<uint32_t>(distExtraValue), distExtraBits);
    }

    // End of block symbol
    const HuffCode& eob = g_fixedLitLenCodes[256];
    w.WriteBits(eob.code, eob.bits);
    w.FlushToByteBoundary();

    outDeflate = std::move(w.bytes);
    return true;
}

bool CompressFileToGzip(const std::wstring& srcPath, const std::wstring& dstPath) {
    if (!FileExistsW(srcPath)) return false;

    std::vector<uint8_t> input;
    if (!ReadFileBytes(srcPath, input)) return false;

    std::vector<uint8_t> deflate;
    if (!WriteFixedDeflateStream(input, deflate)) return false;

    std::wstring tempPath = dstPath + L".tmp";
    DeleteFileW(tempPath.c_str());

    // Open via std::filesystem::path so wide Win32 APIs are used.
    std::ofstream out(std::filesystem::path(tempPath), std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;

    const uint8_t hdr[10] = {
        0x1F, 0x8B,
        0x08,
        0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00,
        0x0B                    // OS=NTFS/Windows
    };
    out.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
    if (!deflate.empty()) out.write(reinterpret_cast<const char*>(deflate.data()), static_cast<std::streamsize>(deflate.size()));

    const uint32_t crc = Crc32(input.data(), input.size());
    WriteLE32(out, crc);
    WriteLE32(out, static_cast<uint32_t>(input.size() & 0xFFFFFFFFu));

    out.flush();
    bool good = out.good();
    out.close();
    if (!good) {
        DeleteFileW(tempPath.c_str());
        return false;
    }

    if (!MoveFileExW(tempPath.c_str(), dstPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tempPath.c_str());
        return false;
    }

    return true;
}

namespace {
constexpr wchar_t kLogOwnerSuffix[] = L".owner";
constexpr size_t kLogOwnerSuffixLength = (sizeof(kLogOwnerSuffix) / sizeof(kLogOwnerSuffix[0])) - 1;
constexpr uint64_t kTransientOwnerGraceMs = 5000;

uint64_t FileTimeToUint64(const FILETIME& fileTime) {
    ULARGE_INTEGER value{};
    value.LowPart = fileTime.dwLowDateTime;
    value.HighPart = fileTime.dwHighDateTime;
    return value.QuadPart;
}

bool GetFileLastWriteTime(const std::wstring& path, FILETIME& outLastWriteTime) {
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes)) {
        return false;
    }

    outLastWriteTime = attributes.ftLastWriteTime;
    return true;
}

bool PathExists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring BuildLatestLogFilename(size_t slotIndex) {
    if (slotIndex == 0) {
        return L"latest.log";
    }

    return L"latest-" + std::to_wstring(slotIndex) + L".log";
}

bool ParseLatestLogFilename(const std::wstring& fileName, size_t& outSlotIndex) {
    if (fileName == L"latest.log") {
        outSlotIndex = 0;
        return true;
    }

    if (!fileName.starts_with(L"latest-") || !fileName.ends_with(L".log")) {
        return false;
    }

    const std::wstring slotText = fileName.substr(7, fileName.size() - 11);
    if (slotText.empty()) {
        return false;
    }

    size_t slotIndex = 0;
    for (wchar_t ch : slotText) {
        if (ch < L'0' || ch > L'9') {
            return false;
        }

        slotIndex = (slotIndex * 10) + static_cast<size_t>(ch - L'0');
    }

    if (slotIndex == 0) {
        return false;
    }

    outSlotIndex = slotIndex;
    return true;
}

bool ParseOwnerFileName(const std::wstring& fileName, size_t& outSlotIndex) {
    if (!fileName.ends_with(kLogOwnerSuffix)) {
        return false;
    }

    return ParseLatestLogFilename(fileName.substr(0, fileName.size() - kLogOwnerSuffixLength), outSlotIndex);
}

std::wstring BuildOwnerFilePath(const std::wstring& logFilePath) {
    return logFilePath + kLogOwnerSuffix;
}

bool GetProcessStartFileTime(DWORD processId, uint64_t& outProcessStartFileTime) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) {
        return false;
    }

    FILETIME creationTime{};
    FILETIME exitTime{};
    FILETIME kernelTime{};
    FILETIME userTime{};
    const BOOL success = GetProcessTimes(process, &creationTime, &exitTime, &kernelTime, &userTime);
    CloseHandle(process);

    if (!success) {
        return false;
    }

    outProcessStartFileTime = FileTimeToUint64(creationTime);
    return true;
}

bool TryParseUint64(const std::string& text, uint64_t& outValue) {
    if (text.empty()) {
        return false;
    }

    uint64_t value = 0;
    for (char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }

        value = (value * 10u) + static_cast<uint64_t>(ch - '0');
    }

    outValue = value;
    return true;
}

bool TryParseOwnerFile(const std::wstring& ownerFilePath, LogInstanceInfo& outInfo) {
    std::ifstream in(std::filesystem::path(ownerFilePath), std::ios::binary);
    if (!in.is_open()) {
        return false;
    }

    std::string line;
    std::string logPathUtf8;
    bool foundPid = false;
    bool foundStartTime = false;
    while (std::getline(in, line)) {
        const size_t equalsPos = line.find('=');
        if (equalsPos == std::string::npos) {
            continue;
        }

        const std::string key = line.substr(0, equalsPos);
        const std::string value = line.substr(equalsPos + 1);
        if (key == "pid") {
            uint64_t parsedPid = 0;
            if (!TryParseUint64(value, parsedPid) || parsedPid > static_cast<uint64_t>(std::numeric_limits<DWORD>::max())) {
                return false;
            }

            outInfo.processId = static_cast<DWORD>(parsedPid);
            foundPid = true;
        } else if (key == "processStartFileTime") {
            if (!TryParseUint64(value, outInfo.processStartFileTime)) {
                return false;
            }

            foundStartTime = true;
        } else if (key == "logFile") {
            logPathUtf8 = value;
        }
    }

    if (!foundPid || !foundStartTime || logPathUtf8.empty()) {
        return false;
    }

    outInfo.logFilePath = Utf8ToWide(logPathUtf8);
    return !outInfo.logFilePath.empty();
}

bool IsLiveLogInstance(const LogInstanceInfo& info) {
    if (info.processId == 0 || info.processStartFileTime == 0 || info.logFilePath.empty()) {
        return false;
    }

    uint64_t currentProcessStartFileTime = 0;
    return GetProcessStartFileTime(info.processId, currentProcessStartFileTime) &&
           currentProcessStartFileTime == info.processStartFileTime;
}

struct OwnerFileState {
    bool exists = false;
    bool live = false;
    bool stale = false;
    LogInstanceInfo instance;
};

OwnerFileState InspectOwnerFile(const std::wstring& ownerFilePath) {
    OwnerFileState state;
    state.exists = PathExists(ownerFilePath);
    if (!state.exists) {
        return state;
    }

    if (TryParseOwnerFile(ownerFilePath, state.instance)) {
        if (IsLiveLogInstance(state.instance)) {
            state.live = true;
            return state;
        }

        state.stale = true;
        return state;
    }

    FILETIME lastWriteTime{};
    if (!GetFileLastWriteTime(ownerFilePath, lastWriteTime)) {
        state.live = true;
        return state;
    }

    FILETIME nowFileTime{};
    GetSystemTimeAsFileTime(&nowFileTime);
    const uint64_t now = FileTimeToUint64(nowFileTime);
    const uint64_t lastWrite = FileTimeToUint64(lastWriteTime);
    const uint64_t ageMs = now > lastWrite ? (now - lastWrite) / 10000u : 0;
    if (ageMs > kTransientOwnerGraceMs) {
        state.stale = true;
    } else {
        state.live = true;
    }

    return state;
}

std::vector<size_t> GatherKnownLatestLogSlots(const std::wstring& logsDirectory) {
    std::vector<size_t> slots;
    std::error_code error;
    if (!std::filesystem::exists(std::filesystem::path(logsDirectory), error)) {
        return slots;
    }

    for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(logsDirectory), error)) {
        if (error) {
            break;
        }

        const std::wstring fileName = entry.path().filename().wstring();
        size_t slotIndex = 0;
        if (ParseLatestLogFilename(fileName, slotIndex) || ParseOwnerFileName(fileName, slotIndex)) {
            slots.push_back(slotIndex);
        }
    }

    std::sort(slots.begin(), slots.end());
    slots.erase(std::unique(slots.begin(), slots.end()), slots.end());
    return slots;
}

std::wstring GetArchiveTimestampString(const std::wstring& logFilePath) {
    FILETIME lastWriteTime{};
    if (!GetFileLastWriteTime(logFilePath, lastWriteTime)) {
        GetSystemTimeAsFileTime(&lastWriteTime);
    }

    FILETIME localFileTime{};
    if (!FileTimeToLocalFileTime(&lastWriteTime, &localFileTime)) {
        localFileTime = lastWriteTime;
    }

    SYSTEMTIME localSystemTime{};
    if (!FileTimeToSystemTime(&localFileTime, &localSystemTime)) {
        GetLocalTime(&localSystemTime);
    }

    wchar_t timestamp[32]{};
    swprintf_s(timestamp,
               L"%04d%02d%02d_%02d%02d%02d",
               localSystemTime.wYear,
               localSystemTime.wMonth,
               localSystemTime.wDay,
               localSystemTime.wHour,
               localSystemTime.wMinute,
               localSystemTime.wSecond);
    return timestamp;
}

std::wstring MakeUniqueArchivedLogPath(const std::wstring& logsDirectory, const std::wstring& timestamp) {
    for (int counter = 0; counter < 1000; ++counter) {
        const std::wstring suffix = counter == 0 ? L"" : (L"_" + std::to_wstring(counter));
        const std::wstring archivedLogPath = logsDirectory + L"\\" + timestamp + suffix + L".log";
        if (!PathExists(archivedLogPath) && !PathExists(archivedLogPath + L".gz")) {
            return archivedLogPath;
        }
    }

    return logsDirectory + L"\\" + timestamp + L"_" + std::to_wstring(GetTickCount64()) + L".log";
}

std::wstring ArchiveLatestLogFile(const std::wstring& logsDirectory, const std::wstring& logFilePath) {
    if (!PathExists(logFilePath)) {
        return std::wstring();
    }

    const std::wstring archivedLogPath = MakeUniqueArchivedLogPath(logsDirectory, GetArchiveTimestampString(logFilePath));
    if (!MoveFileW(logFilePath.c_str(), archivedLogPath.c_str())) {
        return std::wstring();
    }

    return archivedLogPath;
}

void CleanupStaleLatestLogs(const std::wstring& logsDirectory) {
    for (const size_t slotIndex : GatherKnownLatestLogSlots(logsDirectory)) {
        const std::wstring logFilePath = logsDirectory + L"\\" + BuildLatestLogFilename(slotIndex);
        const std::wstring ownerFilePath = BuildOwnerFilePath(logFilePath);
        const OwnerFileState ownerState = InspectOwnerFile(ownerFilePath);
        if (ownerState.live) {
            continue;
        }

        if (ownerState.exists) {
            DeleteFileW(ownerFilePath.c_str());
        }

        const std::wstring archivedLogPath = ArchiveLatestLogFile(logsDirectory, logFilePath);
        if (!archivedLogPath.empty()) {
            QueueArchivedLogCompression(archivedLogPath);
        }
    }
}

std::vector<LogInstanceInfo> EnumerateLiveLogInstances(const std::wstring& logsDirectory) {
    std::vector<LogInstanceInfo> instances;
    for (const size_t slotIndex : GatherKnownLatestLogSlots(logsDirectory)) {
        const std::wstring logFilePath = logsDirectory + L"\\" + BuildLatestLogFilename(slotIndex);
        const OwnerFileState ownerState = InspectOwnerFile(BuildOwnerFilePath(logFilePath));
        if (!ownerState.live) {
            continue;
        }

        LogInstanceInfo info = ownerState.instance;
        if (info.logFilePath.empty()) {
            info.logFilePath = logFilePath;
        }
        instances.push_back(std::move(info));
    }

    std::sort(instances.begin(), instances.end(), [](const LogInstanceInfo& left, const LogInstanceInfo& right) {
        return left.logFilePath < right.logFilePath;
    });
    return instances;
}

bool WriteOwnerFile(HANDLE ownerHandle, const LogSession& session) {
    const std::string contents = "pid=" + std::to_string(session.processId) + "\n" +
                                 "processStartFileTime=" + std::to_string(session.processStartFileTime) + "\n" +
                                 "logFile=" + WideToUtf8(session.logFilePath) + "\n";

    DWORD bytesWritten = 0;
    if (!WriteFile(ownerHandle, contents.data(), static_cast<DWORD>(contents.size()), &bytesWritten, nullptr)) {
        return false;
    }

    if (bytesWritten != contents.size()) {
        return false;
    }

    return FlushFileBuffers(ownerHandle) == TRUE;
}

std::string FormatLocalDateTimeForHeader() {
    const auto now = std::chrono::system_clock::now();
    const auto timeValue = std::chrono::system_clock::to_time_t(now);

    std::tm localTime{};
    localtime_s(&localTime, &timeValue);

    std::ostringstream stream;
    stream << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S");
    return stream.str();
}
} // namespace

bool AcquireLatestLogSession(const std::wstring& logsDirectory, LogSession& outSession) {
    outSession = LogSession{};
    if (logsDirectory.empty()) {
        return false;
    }

    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(logsDirectory), error);
    if (error) {
        return false;
    }

    LogSession session;
    session.logsDirectory = logsDirectory;
    session.processId = GetCurrentProcessId();
    GetProcessStartFileTime(session.processId, session.processStartFileTime);

    CleanupStaleLatestLogs(logsDirectory);

    for (size_t slotIndex = 0; slotIndex < 1024; ++slotIndex) {
        session.slotIndex = slotIndex;
        session.logFilePath = logsDirectory + L"\\" + BuildLatestLogFilename(slotIndex);
        session.ownerFilePath = BuildOwnerFilePath(session.logFilePath);

        const OwnerFileState ownerState = InspectOwnerFile(session.ownerFilePath);
        if (ownerState.live) {
            continue;
        }

        if (ownerState.exists) {
            DeleteFileW(session.ownerFilePath.c_str());

            const std::wstring archivedLogPath = ArchiveLatestLogFile(logsDirectory, session.logFilePath);
            if (!archivedLogPath.empty()) {
                QueueArchivedLogCompression(archivedLogPath);
            }
        } else if (PathExists(session.logFilePath)) {
            const std::wstring archivedLogPath = ArchiveLatestLogFile(logsDirectory, session.logFilePath);
            if (!archivedLogPath.empty()) {
                QueueArchivedLogCompression(archivedLogPath);
            }
        }

        HANDLE ownerHandle =
            CreateFileW(session.ownerFilePath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (ownerHandle == INVALID_HANDLE_VALUE) {
            const DWORD lastError = GetLastError();
            if (lastError == ERROR_FILE_EXISTS || lastError == ERROR_ALREADY_EXISTS) {
                continue;
            }

            continue;
        }

        const bool wroteOwnerFile = WriteOwnerFile(ownerHandle, session);
        CloseHandle(ownerHandle);
        if (!wroteOwnerFile) {
            DeleteFileW(session.ownerFilePath.c_str());
            continue;
        }

        session.otherOpenInstances.clear();
        for (const LogInstanceInfo& instance : EnumerateLiveLogInstances(logsDirectory)) {
            if (instance.processId == session.processId && instance.processStartFileTime == session.processStartFileTime &&
                instance.logFilePath == session.logFilePath) {
                continue;
            }

            session.otherOpenInstances.push_back(instance);
        }

        outSession = std::move(session);
        return true;
    }

    return false;
}

void ReleaseLatestLogSession(LogSession& session) {
    if (!session.ownerFilePath.empty()) {
        DeleteFileW(session.ownerFilePath.c_str());
    }

    session = LogSession{};
}

bool GetCurrentProcessLogFilePath(const std::wstring& logsDirectory, std::wstring& outLogFilePath) {
    outLogFilePath.clear();
    if (logsDirectory.empty()) {
        return false;
    }

    const DWORD currentProcessId = GetCurrentProcessId();
    uint64_t currentProcessStartFileTime = 0;
    if (!GetProcessStartFileTime(currentProcessId, currentProcessStartFileTime)) {
        return false;
    }

    for (const LogInstanceInfo& instance : EnumerateLiveLogInstances(logsDirectory)) {
        if (instance.processId != currentProcessId || instance.processStartFileTime != currentProcessStartFileTime ||
            instance.logFilePath.empty()) {
            continue;
        }

        outLogFilePath = instance.logFilePath;
        return true;
    }

    return false;
}

std::string BuildLogSessionHeader(const LogSession& session) {
    std::ostringstream stream;
    stream << "========================================\n";
    stream << "Toolscreen log session\n";
    stream << "Started local time: " << FormatLocalDateTimeForHeader() << "\n";
    stream << "Process ID: " << session.processId << "\n";
    stream << "Process start FILETIME: " << session.processStartFileTime << "\n";
    stream << "Log file: " << WideToUtf8(session.logFilePath) << "\n";
    stream << "Other open instances at startup: " << session.otherOpenInstances.size() << "\n";
    if (session.otherOpenInstances.empty()) {
        stream << "  (none)\n";
    } else {
        for (const LogInstanceInfo& instance : session.otherOpenInstances) {
            stream << "  - pid=" << instance.processId << ", start=" << instance.processStartFileTime
                   << ", log=" << WideToUtf8(instance.logFilePath) << "\n";
        }
    }
    stream << "========================================\n\n";
    return stream.str();
}

// Uses a lock-free ring buffer for zero-contention log submission.
// A background thread writes to disk every 500ms.

struct LogEntry {
    std::atomic<bool> ready{ false };
    std::string formattedMessage;
};

// Lock-free ring buffer for log entries
static constexpr size_t LOG_BUFFER_SIZE = 8192;
static LogEntry g_logBuffer[LOG_BUFFER_SIZE];
static std::atomic<size_t> g_logClaimIndex{ 0 };
static std::atomic<size_t> g_logReadIndex{ 0 };
static std::mutex g_logArchiveQueueMutex;
static std::vector<std::wstring> g_pendingLogArchives;

// Background writer thread
static std::thread g_logThread;
static std::atomic<bool> g_logThreadRunning{ false };

static void LogThreadMain();
static void WriteLogsToFile();

static void ProcessPendingLogArchives() {
    std::vector<std::wstring> pendingArchives;
    {
        std::lock_guard<std::mutex> lock(g_logArchiveQueueMutex);
        if (g_pendingLogArchives.empty()) return;
        pendingArchives.swap(g_pendingLogArchives);
    }

    for (const std::wstring& archiveSrc : pendingArchives) {
        const std::wstring gzPath = archiveSrc + L".gz";
        if (CompressFileToGzip(archiveSrc, gzPath)) {
            DeleteFileW(archiveSrc.c_str());
        }
    }
}

void QueueArchivedLogCompression(const std::wstring& archivedLogPath) {
    if (archivedLogPath.empty()) return;

    std::lock_guard<std::mutex> lock(g_logArchiveQueueMutex);
    g_pendingLogArchives.push_back(archivedLogPath);
}

void ProcessQueuedArchivedLogCompressions() { ProcessPendingLogArchives(); }

void StartLogThread() {
    if (g_logThreadRunning.load()) return;
    g_logThreadRunning.store(true);
    g_logThread = std::thread(LogThreadMain);
}

void StopLogThread() {
    if (g_logThreadRunning.load()) {
        g_logThreadRunning.store(false);
        if (g_logThread.joinable()) { g_logThread.join(); }
    }

    FlushLogs();
    ProcessQueuedArchivedLogCompressions();
}

static void LogThreadMain() {
    while (g_logThreadRunning.load()) {
        WriteLogsToFile();
        ProcessPendingLogArchives();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    ProcessPendingLogArchives();
}

// Internal: Write all pending log entries to file (called by background thread or FlushLogs)
static void WriteLogsToFile() {
    size_t readPos = g_logReadIndex.load(std::memory_order_relaxed);
    size_t claimPos = g_logClaimIndex.load(std::memory_order_acquire);

    if (readPos == claimPos) return;

    // Lock only during actual file I/O (not during Log() calls)
    std::lock_guard<std::mutex> lock(g_logFileMutex);
    if (!logFile.is_open()) return;

    while (readPos != claimPos) {
        LogEntry& entry = g_logBuffer[readPos % LOG_BUFFER_SIZE];

        // Use relaxed load in tight loop, acquire synchronizes with writer's release
        if (!entry.ready.load(std::memory_order_acquire)) {
            break;
        }

        logFile << entry.formattedMessage << std::endl;

        entry.ready.store(false, std::memory_order_relaxed);

        readPos = (readPos + 1) % LOG_BUFFER_SIZE;
    }

    logFile.flush();
    g_logReadIndex.store(readPos, std::memory_order_release);
}

void FlushLogs() { WriteLogsToFile(); }

void LogCategory(const char* category, const std::string& message) {
    bool enabled = false;
    if (strcmp(category, "mode_switch") == 0)
        enabled = g_config.debug.logModeSwitch;
    else if (strcmp(category, "animation") == 0)
        enabled = g_config.debug.logAnimation;
    else if (strcmp(category, "hotkey") == 0)
        enabled = g_config.debug.logHotkey;
    else if (strcmp(category, "obs") == 0)
        enabled = g_config.debug.logObs;
    else if (strcmp(category, "window_overlay") == 0)
        enabled = g_config.debug.logWindowOverlay;
    else if (strcmp(category, "browser_overlay") == 0)
        enabled = g_config.debug.logBrowserOverlay;
    else if (strcmp(category, "ninjabrain") == 0)
        enabled = g_config.debug.logNinjabrain;
    else if (strcmp(category, "file_monitor") == 0)
        enabled = g_config.debug.logFileMonitor;
    else if (strcmp(category, "image_monitor") == 0)
        enabled = g_config.debug.logImageMonitor;
    else if (strcmp(category, "performance") == 0)
        enabled = g_config.debug.logPerformance;
    else if (strcmp(category, "texture_ops") == 0)
        enabled = g_config.debug.logTextureOps;
    else if (strcmp(category, "gui") == 0)
        enabled = g_config.debug.logGui;
    else if (strcmp(category, "init") == 0)
        enabled = g_config.debug.logInit;
    else if (strcmp(category, "cursor_textures") == 0)
        enabled = g_config.debug.logCursorTextures;
    else if (strcmp(category, "hookchain") == 0)
        enabled = true;

    if (!enabled) return;
    Log(message);
}

// True lock-free log submission using two-phase commit:
// 1. Atomically claim a slot with CAS on g_logClaimIndex
void Log(const std::string& message) {
    std::string formatted = "[" + GetTimestamp() + "] " + message;

    // Atomically claim a slot using CAS loop
    size_t claimPos, nextClaimPos;
    do {
        claimPos = g_logClaimIndex.load(std::memory_order_relaxed);
        nextClaimPos = (claimPos + 1) % LOG_BUFFER_SIZE;

        if (nextClaimPos == g_logReadIndex.load(std::memory_order_acquire)) {
            // Buffer full - drop this message (better than blocking)
            return;
        }
        // Try to atomically claim this slot by advancing claimIndex
    } while (!g_logClaimIndex.compare_exchange_weak(claimPos, nextClaimPos, std::memory_order_acq_rel, std::memory_order_relaxed));

    LogEntry& entry = g_logBuffer[claimPos % LOG_BUFFER_SIZE];
    entry.formattedMessage = std::move(formatted);

    // Mark slot as ready (release ensures data write is visible before ready flag)
    entry.ready.store(true, std::memory_order_release);
}

void Log(const std::wstring& message) { Log(WideToUtf8(message)); }

std::wstring Utf8ToWide(const std::string& utf8_string) {
    if (utf8_string.empty()) return std::wstring();
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &utf8_string[0], (int)utf8_string.size(), NULL, 0);
    std::wstring wstr_to(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &utf8_string[0], (int)utf8_string.size(), &wstr_to[0], size_needed);
    return wstr_to;
}

std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

#include "common/path_sanitize.inl"

void LogException(const std::string& context, const std::exception& e) {
    std::stringstream ss;
    ss << "EXCEPTION in " << context << ": " << e.what();
    Log(ss.str());
}

void LogException(const std::string& context, DWORD exceptionCode, EXCEPTION_POINTERS* exceptionInfo) {
    // If an exception occurs repeatedly (e.g. every frame inside SwapBuffers), logging + stack traces + FlushLogs()

    const uint64_t nowMs = GetTickCount64();
    const uintptr_t addr = (exceptionInfo && exceptionInfo->ExceptionRecord)
                               ? reinterpret_cast<uintptr_t>(exceptionInfo->ExceptionRecord->ExceptionAddress)
                               : 0;

    // Per-process spam guard for repeated identical SEH events.
    static std::atomic<uint64_t> s_lastSehLogMs{ 0 };
    static std::atomic<DWORD> s_lastSehCode{ 0 };
    static std::atomic<uintptr_t> s_lastSehAddr{ 0 };
    static std::atomic<uint32_t> s_suppressedSehCount{ 0 };

    const DWORD lastCode = s_lastSehCode.load(std::memory_order_relaxed);
    const uintptr_t lastAddr = s_lastSehAddr.load(std::memory_order_relaxed);
    const uint64_t lastMs = s_lastSehLogMs.load(std::memory_order_relaxed);

    constexpr uint64_t kRepeatSuppressWindowMs = 250;
    const bool isRepeatBurst = (exceptionCode == lastCode) && (addr == lastAddr) && (lastMs != 0) && ((nowMs - lastMs) < kRepeatSuppressWindowMs);
    if (isRepeatBurst) {
        s_suppressedSehCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const uint32_t suppressed = s_suppressedSehCount.exchange(0, std::memory_order_relaxed);
    if (suppressed > 0) {
        Log("(Suppressed " + std::to_string(suppressed) + " repeat structured exceptions in last " +
            std::to_string(kRepeatSuppressWindowMs) + "ms)");
    }

    s_lastSehCode.store(exceptionCode, std::memory_order_relaxed);
    s_lastSehAddr.store(addr, std::memory_order_relaxed);
    s_lastSehLogMs.store(nowMs, std::memory_order_relaxed);

    std::stringstream ss;
    ss << "STRUCTURED EXCEPTION in " << context << ": Code=0x" << std::hex << exceptionCode;
    if (addr != 0) { ss << " Address=0x" << std::hex << addr; }
    Log(ss.str());

    // Try to get a simple stack trace, but do it at most once per second.
    static std::atomic<uint64_t> s_lastStackMs{ 0 };
    const uint64_t lastStack = s_lastStackMs.load(std::memory_order_relaxed);
    constexpr uint64_t kStackTraceMinIntervalMs = 1000;
    if (exceptionInfo && (lastStack == 0 || (nowMs - lastStack) >= kStackTraceMinIntervalMs)) {
        s_lastStackMs.store(nowMs, std::memory_order_relaxed);
        void* stack[32];
        USHORT frames = CaptureStackBackTrace(0, 32, stack, NULL);
        Log(FormatStackTraceWithSymbols(stack, frames));
    }

    static std::atomic<uint64_t> s_lastFlushMs{ 0 };
    constexpr uint64_t kFlushMinIntervalMs = 1000;
    const uint64_t lastFlush = s_lastFlushMs.load(std::memory_order_relaxed);
    if (lastFlush == 0 || (nowMs - lastFlush) >= kFlushMinIntervalMs) {
        s_lastFlushMs.store(nowMs, std::memory_order_relaxed);
        FlushLogs();
    }
}

LONG WINAPI CustomUnhandledExceptionFilter(EXCEPTION_POINTERS* exceptionInfo) {
    FlushLogs();

    std::cerr << "[Toolscreen] EXCEPTION FILTER TRIGGERED" << std::endl;
    std::cerr.flush();

    if (exceptionInfo && exceptionInfo->ExceptionRecord) {
        DWORD code = exceptionInfo->ExceptionRecord->ExceptionCode;
        std::stringstream ss;
        ss << "=== UNHANDLED EXCEPTION ===";
        ss << "\nException Code: 0x" << std::hex << code;
        ss << "\nException Address: 0x" << std::hex << reinterpret_cast<uintptr_t>(exceptionInfo->ExceptionRecord->ExceptionAddress);
        ss << "\nFlags: 0x" << std::hex << exceptionInfo->ExceptionRecord->ExceptionFlags;

        switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
            ss << "\nType: ACCESS_VIOLATION";
            if (exceptionInfo->ExceptionRecord->NumberParameters >= 2) {
                ss << "\nAccess Type: "
                   << (exceptionInfo->ExceptionRecord->ExceptionInformation[0] == 0   ? "Read"
                       : exceptionInfo->ExceptionRecord->ExceptionInformation[0] == 1 ? "Write"
                                                                                      : "Execute");
                ss << "\nAddress: 0x" << std::hex << exceptionInfo->ExceptionRecord->ExceptionInformation[1];
            }
            break;
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
            ss << "\nType: ARRAY_BOUNDS_EXCEEDED";
            break;
        case EXCEPTION_BREAKPOINT:
            ss << "\nType: BREAKPOINT";
            break;
        case EXCEPTION_DATATYPE_MISALIGNMENT:
            ss << "\nType: DATATYPE_MISALIGNMENT";
            break;
        case EXCEPTION_FLT_DENORMAL_OPERAND:
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        case EXCEPTION_FLT_INEXACT_RESULT:
        case EXCEPTION_FLT_INVALID_OPERATION:
        case EXCEPTION_FLT_OVERFLOW:
        case EXCEPTION_FLT_STACK_CHECK:
        case EXCEPTION_FLT_UNDERFLOW:
            ss << "\nType: FLOATING_POINT_EXCEPTION";
            break;
        case EXCEPTION_ILLEGAL_INSTRUCTION:
            ss << "\nType: ILLEGAL_INSTRUCTION";
            break;
        case EXCEPTION_IN_PAGE_ERROR:
            ss << "\nType: IN_PAGE_ERROR";
            break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
            ss << "\nType: INTEGER_DIVIDE_BY_ZERO";
            break;
        case EXCEPTION_INT_OVERFLOW:
            ss << "\nType: INTEGER_OVERFLOW";
            break;
        case EXCEPTION_INVALID_DISPOSITION:
            ss << "\nType: INVALID_DISPOSITION";
            break;
        case EXCEPTION_NONCONTINUABLE_EXCEPTION:
            ss << "\nType: NONCONTINUABLE_EXCEPTION";
            break;
        case EXCEPTION_PRIV_INSTRUCTION:
            ss << "\nType: PRIVILEGED_INSTRUCTION";
            break;
        case EXCEPTION_STACK_OVERFLOW:
            ss << "\nType: STACK_OVERFLOW";
            break;
        default:
            ss << "\nType: UNKNOWN";
            break;
        }

        Log(ss.str());

        // Capture stack trace
        void* stack[64];
        USHORT frames = CaptureStackBackTrace(0, 64, stack, NULL);
        Log(FormatStackTraceWithSymbols(stack, frames));
    }

    Log("=== END EXCEPTION DETAILS ===");

    FlushLogs();

    std::cerr << "[Toolscreen] EXCEPTION LOGGED - Check log file" << std::endl;
    std::cerr.flush();

    return EXCEPTION_CONTINUE_SEARCH;
}

void SEHTranslator(unsigned int code, EXCEPTION_POINTERS* info) {
    LogException("SEH_Translator", code, info);
    throw SE_Exception(code, info);
}

void EnsureSymbolsInitialized() {
    std::lock_guard<std::mutex> lock(g_symbolMutex);

    if (g_symbolsInitialized.load()) { return; }

    /*// Check if PDB file exists before doing any symbol initialization work
    const char* pdbPath = "C:\\Toolscreen.pdb";
    if (GetFileAttributesA(pdbPath) == INVALID_FILE_ATTRIBUTES) {
        Log("WARNING: PDB file not found at C:\\Toolscreen.pdb - symbols will not be available");
        return;
    }*/

    HANDLE process = GetCurrentProcess();

    HMODULE hCurrentModule = NULL;
    if (!GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCTSTR)&EnsureSymbolsInitialized, &hCurrentModule)) {
        Log("FAILED: Could not determine current module handle.");
        return;
    }

    DWORD64 dllBaseAddr = (DWORD64)hCurrentModule;

    std::stringstream ssAddr;
    ssAddr << "Detected DLL loaded at address: 0x" << std::hex << dllBaseAddr;
    Log(ssAddr.str());
    g_symbolsInitialized.store(true);
    return;
    /*

    DWORD options = SymGetOptions();
    options |= SYMOPT_LOAD_LINES;
    options |= SYMOPT_UNDNAME;
    options |= SYMOPT_DEFERRED_LOADS;
    options |= SYMOPT_FAIL_CRITICAL_ERRORS;
    SymSetOptions(options);

    if (!SymInitialize(process, NULL, FALSE)) {
        DWORD error = GetLastError();
        std::stringstream ss;
        ss << "FAILED: SymInitialize error " << error;
        Log(ss.str());
        return;
    }

    Log(std::string("Force loading PDB: ") + pdbPath);

    DWORD pdbFileSize = 0;
    WIN32_FILE_ATTRIBUTE_DATA fileInfo;
    if (GetFileAttributesExA(pdbPath, GetFileExInfoStandard, &fileInfo)) { pdbFileSize = fileInfo.nFileSizeLow; }

    DWORD64 baseAddr = SymLoadModuleEx(process, NULL, pdbPath, NULL, dllBaseAddr, pdbFileSize, NULL, 0);

    if (baseAddr == 0) {
        DWORD error = GetLastError();
        std::stringstream ss;
        ss << "FAILED: SymLoadModuleEx error " << error << " (0x" << std::hex << error << ")";
        Log(ss.str());
    } else {
        g_symbolsInitialized.store(true);
        Log("SUCCESS: Symbols loaded manually from Toolscreen.pdb mapped to Toolscreen memory.");
    }*/
}

void InstallGlobalExceptionHandlers() {
    EnsureSymbolsInitialized();

    signal(SIGABRT, SignalHandler);

    SetUnhandledExceptionFilter(CustomUnhandledExceptionFilter);

    // Install SEH to C++ exception translator
    _set_se_translator(SEHTranslator);

    Log("Global exception handlers installed (SEH + SIGABRT + Symbols)");
}

std::wstring GetToolscreenPath() {
    WCHAR currentDir[MAX_PATH];
    if (GetCurrentDirectoryW(MAX_PATH, currentDir)) {
        std::wstring localPath = std::wstring(currentDir) + L"\\toolscreen";
        DWORD attrs = GetFileAttributesW(localPath.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) { return localPath; }
    }

    WCHAR userProfile[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PROFILE, NULL, 0, userProfile))) {
        std::wstring path = std::wstring(userProfile) + L"\\.config\\toolscreen";
        if (SHCreateDirectoryExW(NULL, path.c_str(), NULL) == ERROR_SUCCESS || GetLastError() == ERROR_ALREADY_EXISTS) { return path; }
    }
    return L"";
}

// Async version - fire and forget, no blocking
void WriteCurrentModeToFile(const std::string& modeId) {
    if (g_modeFilePath.empty()) return;

    std::string modeIdCopy = modeId;
    std::wstring filePathCopy = g_modeFilePath;

    // Fire-and-forget async write - never blocks the calling thread
    // NOTE: Do NOT use PROFILE_SCOPE inside short-lived detached threads!
    // The thread-local profiler buffer gets destroyed when the thread exits,
    std::thread([modeIdCopy, filePathCopy]() {
        // Open via std::filesystem::path so wide Win32 APIs are used.
        std::ofstream modeFile(std::filesystem::path(filePathCopy), std::ios_base::out | std::ios_base::trunc);
        if (modeFile.is_open()) {
            modeFile << modeIdCopy;
            modeFile.close();
        }
    }).detach();
}

bool SwitchToMode(const std::string& newModeId, const std::string& source, bool forceCut) {
    PROFILE_SCOPE_CAT("Mode Switch", "Mode Management");

    LogCategory("mode_switch", "[MODE_SWITCH] Entry: Attempting to switch to '" + newModeId + "' from source: " + source);

    if (newModeId.empty()) {
        Log("ERROR: Attempted to switch to empty mode ID");
        return false;
    }

    ClearTempSensitivityOverride();

    if (!IsResolutionChangeSupported(g_gameVersion)) {
        std::ostringstream oss;
        oss << "Mode switching disabled: Minecraft version ";
        if (g_gameVersion.valid) {
            oss << g_gameVersion.major << "." << g_gameVersion.minor << "." << g_gameVersion.patch;
        } else {
            oss << "unknown";
        }
        oss << " does not support resolution changes (requires 1.13+)";
        Log(oss.str());
        return false;
    }

    std::string currentMode;

    LogCategory("mode_switch", "[MODE_SWITCH] Acquiring g_modeIdMutex...");
    // Keep the mode ID publication serialized with StartModeTransition so render readers
    // never observe the new mode before the transition snapshot is ready.
    std::unique_lock<std::mutex> modeLock(g_modeIdMutex);
    LogCategory("mode_switch", "[MODE_SWITCH] g_modeIdMutex acquired");
    currentMode = g_currentModeId;

    if (EqualsIgnoreCase(currentMode, newModeId)) {
        Log("Mode switch to '" + newModeId + "' requested, but already in that mode.");
        return false;
    }

    std::string logMessage = "[MODE] Switching from '" + currentMode + "' to '" + newModeId + "'";
    if (!source.empty()) { logMessage += " (source: " + source + ")"; }
    LogCategory("mode_switch", logMessage);

    int fromWidth = 0, fromHeight = 0, fromX = 0, fromY = 0;
    int toWidth = 0, toHeight = 0, toX = 0, toY = 0;
    const int fullW = GetCachedWindowWidth();
    const int fullH = GetCachedWindowHeight();

    // Variable to store target mode config (copied to avoid holding lock)
    ModeConfig toModeCopy;

    bool useAnimatedPosition = false;
    float distanceRatio = 1.0f;
    {
        std::lock_guard<std::mutex> transitionLock(g_modeTransitionMutex);
        if (g_modeTransition.active && g_modeTransition.gameTransition == GameTransitionType::Bounce) {
            fromWidth = g_modeTransition.currentWidth;
            fromHeight = g_modeTransition.currentHeight;
            fromX = g_modeTransition.currentX;
            fromY = g_modeTransition.currentY;
            useAnimatedPosition = true;

            int origDeltaW = std::abs(g_modeTransition.toWidth - g_modeTransition.fromWidth);
            int origDeltaH = std::abs(g_modeTransition.toHeight - g_modeTransition.fromHeight);
            int origDeltaX = std::abs(g_modeTransition.toX - g_modeTransition.fromX);
            int origDeltaY = std::abs(g_modeTransition.toY - g_modeTransition.fromY);
            float origDistance =
                std::sqrt((float)(origDeltaW * origDeltaW + origDeltaH * origDeltaH + origDeltaX * origDeltaX + origDeltaY * origDeltaY));

            if (origDistance > 0) {
            }

            LogCategory("mode_switch",
                        "[MODE_SWITCH] Active transition detected - using current animated position: " + std::to_string(fromWidth) + "x" +
                            std::to_string(fromHeight) + " at " + std::to_string(fromX) + "," + std::to_string(fromY));
        }
    }

    {
        std::vector<std::string> mirrorsToInvalidate;

        // Use config snapshot for thread-safe mode lookup (SwitchToMode is called from multiple threads)
        auto modeSnap = GetConfigSnapshot();
        const ModeConfig* fromMode = modeSnap ? GetModeFromSnapshot(*modeSnap, currentMode) : nullptr;
        const ModeConfig* toMode = modeSnap ? GetModeFromSnapshot(*modeSnap, newModeId) : nullptr;

        const bool preserveEyeZoomSourceMirrorsForSlideOut =
            fromMode && EqualsIgnoreCase(fromMode->id, "EyeZoom") && modeSnap && modeSnap->eyezoom.slideMirrorsIn && !forceCut;
        const bool preserveModeSourceMirrorsForSlideOut = fromMode && fromMode->slideMirrorsIn && !forceCut;
        const bool preserveSourceOnlyMirrorsForSlideOut =
            preserveEyeZoomSourceMirrorsForSlideOut || preserveModeSourceMirrorsForSlideOut;

        if (!preserveSourceOnlyMirrorsForSlideOut && modeSnap && fromMode && toMode && !EqualsIgnoreCase(fromMode->id, toMode->id)) {
            std::unordered_set<std::string> fromMirrorIds;
            std::unordered_set<std::string> toMirrorIds;
            if (fromMode) { CollectModeMirrorIds(*modeSnap, *fromMode, fromMirrorIds); }
            if (toMode) { CollectModeMirrorIds(*modeSnap, *toMode, toMirrorIds); }

            mirrorsToInvalidate.reserve(fromMirrorIds.size());
            for (const auto& mirrorId : fromMirrorIds) {
                if (toMirrorIds.find(mirrorId) == toMirrorIds.end()) { mirrorsToInvalidate.push_back(mirrorId); }
            }
        }

        if (!mirrorsToInvalidate.empty()) { InvalidateMirrorTextureCaches(mirrorsToInvalidate); }

        if (!useAnimatedPosition) {
            if (fromMode) {
                if (fromMode->stretch.enabled) {
                    if (EqualsIgnoreCase(fromMode->id, "Fullscreen")) {
                        fromWidth = fullW;
                        fromHeight = fullH;
                        fromX = 0;
                        fromY = 0;
                    } else {
                        fromWidth = fromMode->stretch.width;
                        fromHeight = fromMode->stretch.height;
                        fromX = fromMode->stretch.x;
                        fromY = fromMode->stretch.y;
                    }
                } else {
                    fromWidth = fromMode->width;
                    fromHeight = fromMode->height;
                    fromX = GetCenteredAxisOffset(fullW, fromWidth);
                    fromY = GetCenteredAxisOffset(fullH, fromHeight);
                }
            } else {
                fromWidth = fullW;
                fromHeight = fullH;
                fromX = 0;
                fromY = 0;
            }
        }

        if (toMode) {
            if (toMode->stretch.enabled) {
                if (EqualsIgnoreCase(toMode->id, "Fullscreen")) {
                    toWidth = fullW;
                    toHeight = fullH;
                    toX = 0;
                    toY = 0;
                } else {
                    toWidth = toMode->stretch.width;
                    toHeight = toMode->stretch.height;
                    toX = toMode->stretch.x;
                    toY = toMode->stretch.y;
                }
            } else {
                toWidth = toMode->width;
                toHeight = toMode->height;
                toX = GetCenteredAxisOffset(fullW, toWidth);
                toY = GetCenteredAxisOffset(fullH, toHeight);
            }

            toModeCopy = *toMode;
        } else {
            toWidth = fullW;
            toHeight = fullH;
            toX = 0;
            toY = 0;
            toModeCopy.id = newModeId;
            toModeCopy.width = fullW;
            toModeCopy.height = fullH;
            toModeCopy.gameTransition = GameTransitionType::Cut;
            toModeCopy.overlayTransition = OverlayTransitionType::Cut;
            toModeCopy.backgroundTransition = BackgroundTransitionType::Cut;
        }
        LogCategory("mode_switch", "[MODE_SWITCH] Mode dimensions calculated - from: " + std::to_string(fromWidth) + "x" +
                                       std::to_string(fromHeight) + ", to: " + std::to_string(toWidth) + "x" + std::to_string(toHeight));
    }

    if (useAnimatedPosition && toModeCopy.gameTransition == GameTransitionType::Bounce) {
        int newDeltaW = std::abs(toWidth - fromWidth);
        int newDeltaH = std::abs(toHeight - fromHeight);
        int newDeltaX = std::abs(toX - fromX);
        int newDeltaY = std::abs(toY - fromY);
        float newDistance =
            std::sqrt((float)(newDeltaW * newDeltaW + newDeltaH * newDeltaH + newDeltaX * newDeltaX + newDeltaY * newDeltaY));

        // Use snapshot for thread-safe lookup (reuse modeSnap if still valid, else re-acquire)
        auto distSnap = GetConfigSnapshot();
        const ModeConfig* targetMode = distSnap ? GetModeFromSnapshot(*distSnap, newModeId) : nullptr;
        if (targetMode) {
            int refFromW = fullW, refFromH = fullH, refFromX = 0, refFromY = 0;
            int refToW = toWidth, refToH = toHeight, refToX = toX, refToY = toY;

            int fullDeltaW = std::abs(refToW - refFromW);
            int fullDeltaH = std::abs(refToH - refFromH);
            int fullDeltaX = std::abs(refToX - refFromX);
            int fullDeltaY = std::abs(refToY - refFromY);
            float fullDistance =
                std::sqrt((float)(fullDeltaW * fullDeltaW + fullDeltaH * fullDeltaH + fullDeltaX * fullDeltaX + fullDeltaY * fullDeltaY));

            if (fullDistance > 0) {
                distanceRatio = newDistance / fullDistance;
                distanceRatio = (std::max)(0.1f, (std::min)(1.0f, distanceRatio));

                int originalDuration = toModeCopy.transitionDurationMs;
                toModeCopy.transitionDurationMs = static_cast<int>(originalDuration * distanceRatio);

                LogCategory("mode_switch",
                            "[MODE_SWITCH] Mid-animation reversal: scaling duration from " + std::to_string(originalDuration) + "ms to " +
                                std::to_string(toModeCopy.transitionDurationMs) + "ms (ratio: " + std::to_string(distanceRatio) + ")");
            }
        }
    }

    if (forceCut) {
        toModeCopy.gameTransition = GameTransitionType::Cut;
        toModeCopy.overlayTransition = OverlayTransitionType::Cut;
        toModeCopy.backgroundTransition = BackgroundTransitionType::Cut;
    }

    LogCategory("mode_switch",
                "[MODE_SWITCH] Calling StartModeTransition with Game:" + GameTransitionTypeToString(toModeCopy.gameTransition) +
                    ", Overlay:" + OverlayTransitionTypeToString(toModeCopy.overlayTransition) +
                    ", Bg:" + BackgroundTransitionTypeToString(toModeCopy.backgroundTransition));
    // The next real game glViewport after a mode switch must republish the live source size.
    // Until then, input translation should resolve from the new mode snapshot instead of the prior mode's viewport.
    InvalidateLatestGameViewportSize();
    StartModeTransition(currentMode, newModeId, fromWidth, fromHeight, fromX, fromY, toWidth, toHeight, toX, toY, toModeCopy);
    LogCategory("mode_switch", "[MODE_SWITCH] StartModeTransition completed");

    g_currentModeId = newModeId;
    int nextIndex = 1 - g_currentModeIdIndex.load(std::memory_order_relaxed);
    g_modeIdBuffers[nextIndex] = newModeId;
    g_currentModeIdIndex.store(nextIndex, std::memory_order_release);
    LogCategory("mode_switch", "[MODE_SWITCH] Published new active mode after transition setup: " + newModeId);

    modeLock.unlock();
    LogCategory("mode_switch", "[MODE_SWITCH] g_modeIdMutex released");

    // Async file write OUTSIDE the mutex - never blocks
    WriteCurrentModeToFile(newModeId);

    return true;
}

bool GetMonitorRectForWindow(HWND hwnd, RECT& outRect) {
    if (!hwnd) { return false; }

    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!mon) {
        RECT wr{};
        if (GetWindowRect(hwnd, &wr)) {
            mon = MonitorFromRect(&wr, MONITOR_DEFAULTTONEAREST);
        }
    }
    if (!mon) { return false; }

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfo(mon, &mi)) { return false; }

    outRect = mi.rcMonitor;
    return true;
}

bool GetMonitorSizeForWindow(HWND hwnd, int& outW, int& outH) {
    RECT r{};
    if (!GetMonitorRectForWindow(hwnd, r)) { return false; }
    outW = (r.right - r.left);
    outH = (r.bottom - r.top);
    return (outW > 0 && outH > 0);
}

bool GetWindowClientRectInScreen(HWND hwnd, RECT& outRect) {
    if (!hwnd) { return false; }

    RECT clientRect{};
    if (!GetClientRect(hwnd, &clientRect)) { return false; }

    POINT topLeft{ clientRect.left, clientRect.top };
    POINT bottomRight{ clientRect.right, clientRect.bottom };
    if (!ClientToScreen(hwnd, &topLeft) || !ClientToScreen(hwnd, &bottomRight)) { return false; }

    outRect.left = topLeft.x;
    outRect.top = topLeft.y;
    outRect.right = bottomRight.x;
    outRect.bottom = bottomRight.y;

    return (outRect.right > outRect.left) && (outRect.bottom > outRect.top);
}

bool IsCursorVisible() {
    if (g_gameVersion >= GameVersion(1, 13, 0)) {
        CURSORINFO ci = { sizeof(CURSORINFO) };
        if (!GetCursorInfo(&ci)) {
            Log("Failed to get cursor info");
            return false;
        }
        return (ci.flags & CURSOR_SHOWING) != 0;
    }

    if (g_specialCursorHandle.load() == NULL) { return true; }

    HCURSOR cur = GetCursor();
    return cur != NULL && cur != g_specialCursorHandle.load();
}

bool IsHardcodedMode(const std::string& id) {
    return EqualsIgnoreCase(id, "Fullscreen") || EqualsIgnoreCase(id, "EyeZoom") || EqualsIgnoreCase(id, "Preemptive") ||
           EqualsIgnoreCase(id, "Thin") || EqualsIgnoreCase(id, "Wide");
}

bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) { return false; }

    return std::equal(a.begin(), a.end(), b.begin(), [](char a, char b) { return std::tolower(a) == std::tolower(b); });
}

std::string ModeSourceTypeToString(ModeSourceType type) {
    switch (type) {
    case ModeSourceType::Mirror:
        return "mirror";
    case ModeSourceType::MirrorGroup:
        return "mirrorGroup";
    case ModeSourceType::Image:
        return "image";
    case ModeSourceType::WindowOverlay:
        return "windowOverlay";
    case ModeSourceType::BrowserOverlay:
        return "browserOverlay";
    default:
        return "mirror";
    }
}

ModeSourceType StringToModeSourceType(const std::string& value) {
    if (EqualsIgnoreCase(value, "mirror")) return ModeSourceType::Mirror;
    if (EqualsIgnoreCase(value, "mirrorGroup") || EqualsIgnoreCase(value, "group")) return ModeSourceType::MirrorGroup;
    if (EqualsIgnoreCase(value, "image")) return ModeSourceType::Image;
    if (EqualsIgnoreCase(value, "windowOverlay") || EqualsIgnoreCase(value, "window")) return ModeSourceType::WindowOverlay;
    if (EqualsIgnoreCase(value, "browserOverlay") || EqualsIgnoreCase(value, "browser")) return ModeSourceType::BrowserOverlay;
    return ModeSourceType::Mirror;
}

static void AppendModeSources(std::vector<ModeSourceRef>& outSources, ModeSourceType type, const std::vector<std::string>& ids) {
    outSources.reserve(outSources.size() + ids.size());
    for (const auto& id : ids) {
        if (id.empty()) continue;
        outSources.push_back({ type, id });
    }
}

std::vector<ModeSourceRef> BuildModeSourcesFromLegacyLists(const std::vector<std::string>& mirrorIds,
                                                           const std::vector<std::string>& mirrorGroupIds,
                                                           const std::vector<std::string>& imageIds,
                                                           const std::vector<std::string>& windowOverlayIds,
                                                           const std::vector<std::string>& browserOverlayIds) {
    std::vector<ModeSourceRef> rebuiltSources;
    rebuiltSources.reserve(mirrorIds.size() + mirrorGroupIds.size() + imageIds.size() + windowOverlayIds.size() +
                           browserOverlayIds.size());
    AppendModeSources(rebuiltSources, ModeSourceType::Mirror, mirrorIds);
    AppendModeSources(rebuiltSources, ModeSourceType::MirrorGroup, mirrorGroupIds);
    AppendModeSources(rebuiltSources, ModeSourceType::Image, imageIds);
    AppendModeSources(rebuiltSources, ModeSourceType::WindowOverlay, windowOverlayIds);
    AppendModeSources(rebuiltSources, ModeSourceType::BrowserOverlay, browserOverlayIds);
    return rebuiltSources;
}

bool ModeHasSource(const ModeConfig& mode, ModeSourceType type, const std::string& id) {
    return std::any_of(mode.sources.begin(), mode.sources.end(), [&](const ModeSourceRef& source) {
        return source.type == type && source.id == id;
    });
}

bool AddModeSource(ModeConfig& mode, ModeSourceType type, const std::string& id) {
    if (id.empty() || ModeHasSource(mode, type, id)) { return false; }
    mode.sources.push_back({ type, id });
    return true;
}

bool RemoveModeSource(ModeConfig& mode, ModeSourceType type, const std::string& id) {
    auto it = std::find_if(mode.sources.begin(), mode.sources.end(), [&](const ModeSourceRef& source) {
        return source.type == type && source.id == id;
    });
    if (it == mode.sources.end()) { return false; }
    mode.sources.erase(it);
    return true;
}

size_t RemoveAllModeSources(ModeConfig& mode, ModeSourceType type, const std::string& id) {
    const size_t originalSize = mode.sources.size();
    mode.sources.erase(std::remove_if(mode.sources.begin(), mode.sources.end(), [&](const ModeSourceRef& source) {
                           return source.type == type && source.id == id;
                       }),
                       mode.sources.end());
    return originalSize - mode.sources.size();
}

bool RenameModeSource(ModeConfig& mode, ModeSourceType type, const std::string& oldId, const std::string& newId) {
    bool changed = false;
    for (auto& source : mode.sources) {
        if (source.type == type && source.id == oldId) {
            source.id = newId;
            changed = true;
        }
    }
    return changed;
}

bool MoveModeSource(ModeConfig& mode, size_t fromIndex, size_t toIndex) {
    if (fromIndex >= mode.sources.size() || toIndex >= mode.sources.size() || fromIndex == toIndex) { return false; }

    ModeSourceRef movedSource = mode.sources[fromIndex];
    mode.sources.erase(mode.sources.begin() + static_cast<std::ptrdiff_t>(fromIndex));
    mode.sources.insert(mode.sources.begin() + static_cast<std::ptrdiff_t>(toIndex), std::move(movedSource));
    return true;
}

void CollectModeMirrorIds(const Config& config, const ModeConfig& mode, std::unordered_set<std::string>& outMirrorIds) {
    for (const auto& source : mode.sources) {
        if (source.id.empty()) continue;

        if (source.type == ModeSourceType::Mirror) {
            outMirrorIds.insert(source.id);
            continue;
        }

        if (source.type != ModeSourceType::MirrorGroup) { continue; }

        for (const auto& group : config.mirrorGroups) {
            if (group.name != source.id) continue;
            for (const auto& item : group.mirrors) {
                if (!item.enabled || item.mirrorId.empty()) continue;
                outMirrorIds.insert(item.mirrorId);
            }
            break;
        }
    }
}

void CollectModeOrderedMirrorIds(const Config& config, const ModeConfig& mode, std::vector<std::string>& outMirrorIds) {
    std::unordered_set<std::string> seenMirrorIds;
    const size_t estimatedSourceCount = mode.sources.size();
    seenMirrorIds.reserve(estimatedSourceCount * 2 + 4);

    for (const auto& source : mode.sources) {
        if (source.id.empty()) continue;

        if (source.type == ModeSourceType::Mirror) {
            if (seenMirrorIds.insert(source.id).second) { outMirrorIds.push_back(source.id); }
            continue;
        }

        if (source.type != ModeSourceType::MirrorGroup) { continue; }

        for (const auto& group : config.mirrorGroups) {
            if (group.name != source.id) continue;
            for (const auto& item : group.mirrors) {
                if (!item.enabled || item.mirrorId.empty()) continue;
                if (seenMirrorIds.insert(item.mirrorId).second) { outMirrorIds.push_back(item.mirrorId); }
            }
            break;
        }
    }
}

// Internal version - requires g_configMutex to already be held
const ModeConfig* GetMode_Internal(const std::string& id) {
    for (const auto& mode : g_config.modes) {
        if (EqualsIgnoreCase(mode.id, id)) return &mode;
    }
    return nullptr;
}

const ModeConfig* GetMode(const std::string& id) { return GetMode_Internal(id); }

ModeConfig* GetModeMutable(const std::string& id) {
    for (auto& mode : g_config.modes) {
        if (EqualsIgnoreCase(mode.id, id)) return &mode;
    }
    return nullptr;
}

MirrorConfig* GetMutableMirror(const std::string& name) {
    for (auto& mirror : g_config.mirrors) {
        if (mirror.name == name) return &mirror;
    }
    return nullptr;
}

const ModeConfig* GetModeFromSnapshot(const Config& config, const std::string& id) {
    for (const auto& mode : config.modes) {
        if (EqualsIgnoreCase(mode.id, id)) return &mode;
    }
    return nullptr;
}

const ModeConfig* GetModeFromSnapshotOrFallback(const Config& config, const std::string& id, std::string* resolvedId) {
    if (const ModeConfig* mode = GetModeFromSnapshot(config, id)) {
        if (resolvedId) {
            *resolvedId = mode->id;
        }
        return mode;
    }

    if (!config.defaultMode.empty()) {
        if (const ModeConfig* defaultMode = GetModeFromSnapshot(config, config.defaultMode)) {
            if (resolvedId) {
                *resolvedId = defaultMode->id;
            }
            return defaultMode;
        }
    }

    if (!config.modes.empty()) {
        if (resolvedId) {
            *resolvedId = config.modes.front().id;
        }
        return &config.modes.front();
    }

    if (resolvedId) {
        resolvedId->clear();
    }
    return nullptr;
}

const MirrorConfig* GetMirrorFromSnapshot(const Config& config, const std::string& name) {
    for (const auto& mirror : config.mirrors) {
        if (mirror.name == name) return &mirror;
    }
    return nullptr;
}

bool isWallTitleOrWaiting(const std::string& state) {
    return state == "wall" || state == "title" || state == "waiting" || state.rfind("generating", 0) == 0;
}

// Uses config snapshot for thread-safe mode lookup + lock-free mode ID
ModeViewportInfo GetCurrentModeViewport_Internal() {
    ModeViewportInfo info;
    // Lock-free read of current mode ID from double-buffer
    std::string modeId = g_modeIdBuffers[g_currentModeIdIndex.load(std::memory_order_acquire)];

    // Use snapshot for thread-safe mode config lookup (called from multiple threads)
    auto vpSnap = GetConfigSnapshot();
    const ModeConfig* mode = vpSnap ? GetModeFromSnapshotOrFallback(*vpSnap, modeId) : nullptr;
    if (!mode) {
        return info;
    }

    info.valid = true;
    info.x = 0;
    info.y = 0;
    info.width = mode->width;
    info.height = mode->height;

    int screenW = GetCachedWindowWidth();
    int screenH = GetCachedWindowHeight();

    info.stretchEnabled = mode->stretch.enabled;
    if (mode->stretch.enabled) {
        if (EqualsIgnoreCase(mode->id, "Fullscreen")) {
            info.stretchX = 0;
            info.stretchY = 0;
            info.stretchWidth = screenW;
            info.stretchHeight = screenH;
        } else {
            info.stretchX = mode->stretch.x;
            info.stretchY = mode->stretch.y;
            info.stretchWidth = mode->stretch.width;
            info.stretchHeight = mode->stretch.height;
        }
    } else {
        info.stretchX = GetCenteredAxisOffset(screenW, mode->width);
        info.stretchY = GetCenteredAxisOffset(screenH, mode->height);
        info.stretchWidth = mode->width;
        info.stretchHeight = mode->height;
    }

    return info;
}

ModeViewportInfo GetCurrentModeViewport() {
    // Lock-free - just calls internal version which uses double-buffered mode ID
    return GetCurrentModeViewport_Internal();
}

GLuint CompileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint ok;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, 512, NULL, log);
        Log("ERROR: Shader compile failed: " + std::string(log));
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint CreateShaderProgram(const char* vert, const char* frag) {
    PROFILE_SCOPE_CAT("Shader Program Creation", "GPU Operations");
    GLuint v = CompileShader(GL_VERTEX_SHADER, vert);
    GLuint f = CompileShader(GL_FRAGMENT_SHADER, frag);
    if (v == 0 || f == 0) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(p, 512, NULL, log);
        Log("ERROR: Shader link failed: " + std::string(log));
        glDeleteProgram(p);
        p = 0;
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}

void LoadImageAsync(DecodedImageData::Type type, std::string id, std::string path, const std::wstring& toolscreenPath) {
    PROFILE_SCOPE_CAT("Async Image Load", "IO Operations");
    if (path.empty()) {
        Log("Skipping image load for '" + id + "' due to empty path.");
        return;
    }

    const int videoCacheBudgetMiB = g_config.debug.videoCacheBudgetMiB;
    const size_t videoCacheBudgetBytes = GetConfiguredVideoCacheBudgetBytes(videoCacheBudgetMiB);

    std::thread([type, id, path, toolscreenPath, videoCacheBudgetMiB, videoCacheBudgetBytes]() {
        _set_se_translator(SEHTranslator);

        try {
            LogCategory("image_monitor", "Started thread for loading image '" + id + "' from path '" + path + "'");
            try {
                if (g_isShuttingDown.load()) { return; }

                std::wstring final_path;
                std::wstring image_wpath = Utf8ToWide(path);
                if (PathIsRelativeW(image_wpath.c_str()) && !toolscreenPath.empty()) {
                    final_path = toolscreenPath + L"\\" + image_wpath;
                } else {
                    final_path = image_wpath;
                }
                std::string path_utf8 = WideToUtf8(final_path);

                const VisualMediaKind mediaKind = DetectVisualMediaKindFromPath(path);
                const bool isGif = mediaKind == VisualMediaKind::AnimatedGif;
                const bool isVideo = mediaKind == VisualMediaKind::VideoMpeg1;
                if (mediaKind == VisualMediaKind::Unsupported) {
                    LogCategory("image_monitor",
                                "Skipping unsupported visual media for '" + id + "' from '" + path + "'. " +
                                    DescribeSupportedVisualMediaFormats());
                    return;
                }

                int w, h, c;
                unsigned char* data = nullptr;
                int frameCount = 0;
                int* delays = nullptr;
                std::vector<int> decodedFrameDelays;

                if (isVideo) {
                    CachedMpegVideoResult cachedVideo;
                    if (videoCacheBudgetBytes == 0) {
                        LogCategory("image_monitor",
                                    "Skipping MPEG-1 video '" + id + "' from '" + path +
                                        "' because debug.videoCacheBudgetMiB is set to 0 and live streaming playback is disabled.");
                        return;
                    }

                    if (DecodeCachedMpegVideoFile(path_utf8, videoCacheBudgetBytes, cachedVideo)) {
                        w = cachedVideo.width;
                        h = cachedVideo.height;
                        c = 4;
                        frameCount = cachedVideo.frameCount;
                        decodedFrameDelays = std::move(cachedVideo.frameDelaysMs);

                        if (!cachedVideo.rgbaFrames.empty()) {
                            const size_t byteCount = cachedVideo.rgbaFrames.size();
                            data = static_cast<unsigned char*>(malloc(byteCount));
                            if (data) {
                                memcpy(data, cachedVideo.rgbaFrames.data(), byteCount);
                            }
                        }
                    } else {
                        LogCategory("image_monitor",
                                    "Skipping MPEG-1 video '" + id + "' from '" + path + "' because it could not be cached within the configured budget of " +
                                        std::to_string(videoCacheBudgetMiB) + " MiB: " + cachedVideo.error);
                        return;
                    }
                }

                if (!isVideo && isGif) {
                    FILE* f = nullptr;
                    errno_t err = fopen_s(&f, path_utf8.c_str(), "rb");
                    if (err == 0 && f) {
                        fseek(f, 0, SEEK_END);
                        long fileSize = ftell(f);
                        fseek(f, 0, SEEK_SET);

                        if (fileSize <= 0) {
                            LogCategory("image_monitor", "Skipping GIF load for '" + id + "' from '" + path + "' due to invalid file size " +
                                                           std::to_string(fileSize) + ".");
                        } else if (static_cast<unsigned long long>(fileSize) > kMaxGifLoadBytes) {
                            LogCategory("image_monitor",
                                        "Skipping GIF load for '" + id + "' from '" + path + "' because file size " +
                                            FormatByteCount(static_cast<size_t>(fileSize)) + " exceeds guard limit of " +
                                            FormatByteCount(kMaxGifLoadBytes) + ".");
                        } else if (fileSize > (std::numeric_limits<int>::max)()) {
                            LogCategory("image_monitor",
                                        "Skipping GIF load for '" + id + "' from '" + path + "' because file size " +
                                            FormatByteCount(static_cast<size_t>(fileSize)) + " exceeds stb_image int input range.");
                        } else {
                            std::vector<unsigned char> fileData(static_cast<size_t>(fileSize));
                            size_t bytesRead = fread(fileData.data(), 1, static_cast<size_t>(fileSize), f);
                            fclose(f);
                            f = nullptr;

                            if (bytesRead == static_cast<size_t>(fileSize)) {
                                data = stbi_load_gif_from_memory(fileData.data(), static_cast<int>(fileSize), &delays, &w, &h, &frameCount, &c, 4);

                                if (data && frameCount <= 1) {
                                    frameCount = 1;
                                    stbi_image_free(delays);
                                    delays = nullptr;
                                }
                            } else {
                                LogCategory("image_monitor",
                                            "Failed to read full GIF file for '" + id + "' from '" + path + "'. Expected " +
                                                std::to_string(fileSize) + " bytes, read " + std::to_string(bytesRead) + ".");
                            }
                        }

                        if (f) { fclose(f); }
                    }

                    if (!data) {
                        frameCount = 0;
                        data = stbi_load(path_utf8.c_str(), &w, &h, &c, 4);
                    }
                } else if (!isVideo) {
                    data = stbi_load(path_utf8.c_str(), &w, &h, &c, 4);
                }

                if (g_isShuttingDown.load()) {
                    if (data) stbi_image_free(data);
                    if (delays) stbi_image_free(delays);
                    return;
                }

                if (data && w > 0 && h > 0) {
                    int decodedHeight = h;
                    if (frameCount > 1) {
                        long long totalHeight = static_cast<long long>(h) * static_cast<long long>(frameCount);
                        if (frameCount <= 0 || totalHeight <= 0 || totalHeight > (std::numeric_limits<int>::max)()) {
                            LogCategory("image_monitor",
                                        "Skipping decoded animated image '" + id + "' from '" + path +
                                            "' because frame dimensions are invalid: frameCount=" + std::to_string(frameCount) +
                                            ", frameHeight=" + std::to_string(h) + ".");
                            stbi_image_free(data);
                            if (delays) stbi_image_free(delays);
                            return;
                        }
                        decodedHeight = static_cast<int>(totalHeight);
                    }

                    size_t decodedBytes = 0;
                    if (!TryComputeImageByteCount(w, decodedHeight, 4, decodedBytes)) {
                        LogCategory("image_monitor",
                                    "Skipping decoded image '" + id + "' from '" + path +
                                        "' because dimensions overflow byte-count calculation: " + std::to_string(w) + "x" +
                                        std::to_string(decodedHeight) + "x4.");
                        stbi_image_free(data);
                        if (delays) stbi_image_free(delays);
                        return;
                    }
                    if (!isVideo && decodedBytes > kMaxDecodedImageBytes) {
                        LogCategory("image_monitor",
                                    "Skipping decoded image '" + id + "' from '" + path + "' because estimated pixel storage " +
                                        FormatByteCount(decodedBytes) + " exceeds guard limit of " +
                                        FormatByteCount(kMaxDecodedImageBytes) + ".");
                        stbi_image_free(data);
                        if (delays) stbi_image_free(delays);
                        return;
                    }

                    DecodedImageData decoded;
                    decoded.type = type;
                    decoded.id = id;
                    decoded.width = w;
                    decoded.channels = 4;
                    decoded.data = data;
                    decoded.isVideo = isVideo;

                    if (frameCount > 1) {
                        decoded.isAnimated = true;
                        decoded.frameCount = frameCount;
                        decoded.height = decodedHeight;
                        decoded.frameHeight = h;
                        if (!decodedFrameDelays.empty()) {
                            decoded.frameDelays = decodedFrameDelays;
                        } else {
                            for (int i = 0; i < frameCount; i++) {
                                int delayMs = (delays && delays[i] > 0) ? delays[i] : 100;
                                decoded.frameDelays.push_back(delayMs);
                            }
                        }
                        if (isVideo) {
                            Log("Loaded cached MPEG-1 video '" + id + "' with " + std::to_string(frameCount) +
                                " frames, frame size: " + std::to_string(w) + "x" + std::to_string(h));
                        } else {
                            Log("Loaded animated GIF '" + id + "' with " + std::to_string(frameCount) +
                                " frames, frame size: " + std::to_string(w) + "x" + std::to_string(h));
                        }
                    } else {
                        decoded.isAnimated = false;
                        decoded.frameCount = 1;
                        decoded.height = h;
                        decoded.frameHeight = h;
                    }

                    if (delays) stbi_image_free(delays);

                    std::lock_guard<std::mutex> lock(g_decodedImagesMutex);
                    g_decodedImagesQueue.push_back(decoded);
                    LogCategory("image_monitor", "Successfully decoded visual media for '" + id + "' from '" + path +
                                                   "' on background thread: " + std::to_string(decoded.width) + "x" +
                                                   std::to_string(decoded.height) + ", frameCount=" +
                                                   std::to_string(decoded.frameCount) + ", queueSize=" +
                                                   std::to_string(g_decodedImagesQueue.size()) + ", bytes=" +
                                                   FormatByteCount(decodedBytes) + ".");
                } else {
                    std::string reason = "unknown error";
                    if (isVideo) {
                        reason = "MPEG-1 video could not be decoded into the configured cache budget";
                    } else if (stbi_failure_reason()) {
                        reason = stbi_failure_reason();
                    }
                    Log("ERROR: Failed to decode visual media '" + path + "' for ID '" + id + "'. Reason: " + reason);
                    if (data) stbi_image_free(data);
                    if (delays) stbi_image_free(delays);
                }
            } catch (const std::exception& ex) {
                Log("ERROR: Exception during image load for '" + id + "' from '" + path + "': " + ex.what());
            }
        } catch (const SE_Exception& e) {
            LogException("ImageLoadThread (SEH) for '" + id + "'", e.getCode(), e.getInfo());
        } catch (const std::exception& e) { LogException("ImageLoadThread for '" + id + "'", e); } catch (...) {
            Log("EXCEPTION in ImageLoadThread for '" + id + "': Unknown exception");
        }
        LogCategory("image_monitor", "Image load thread for '" + id + "' has completed.");
    }).detach();
}

void LoadAllImages() {
    PROFILE_SCOPE_CAT("Load All Images", "IO Operations");
    if (g_allImagesLoaded) {
        Log("All images have already been loaded, skipping LoadAllImages call.");
        return;
    };
    Log("Spawning background threads to load all configured images...");
    stbi_set_flip_vertically_on_load(true);

    std::vector<ModeConfig> modesToLoad;
    std::vector<ImageConfig> imagesToLoad;

    {
        modesToLoad = g_config.modes;
        imagesToLoad = g_config.images;
    }

    for (const auto& mode : modesToLoad) {
        if (mode.background.selectedMode == "image" && !mode.background.image.empty()) {
            Log("Queueing background image load for mode '" + mode.id + "': " + mode.background.image);
            LoadImageAsync(DecodedImageData::Type::Background, mode.id, mode.background.image, g_toolscreenPath);
        }
    }

    for (const auto& imageConf : imagesToLoad) {
        LoadImageAsync(DecodedImageData::Type::UserImage, imageConf.name, imageConf.path, g_toolscreenPath);
    }

    for (const auto& overlay : g_config.eyezoom.overlays) {
        if (!overlay.path.empty()) {
            LoadImageAsync(DecodedImageData::Type::UserImage, "ezoverlay_" + overlay.name, overlay.path, g_toolscreenPath);
        }
    }
}

namespace {
long long NowEpochMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

bool ReadAliveFileBytes(const std::wstring& alivePath, unsigned char (&bytes)[16]) {
    HANDLE file = CreateFileW(alivePath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) { return false; }
    DWORD bytesRead = 0;
    const bool readWholeFile = ReadFile(file, bytes, sizeof(bytes), &bytesRead, NULL) && bytesRead == sizeof(bytes);
    CloseHandle(file);
    return readWholeFile;
}

bool IsHermesAlive(const std::wstring& alivePath) {
    if (alivePath.empty()) { return false; }
    unsigned char bytes[16] = {};
    if (!ReadAliveFileBytes(alivePath, bytes)) { return false; }
    return game_state_source::EvaluateHermesAlive(bytes, static_cast<uint64_t>(GetCurrentProcessId()), NowEpochMillis());
}

bool StateOutputFileAvailable(const std::wstring& path) {
    if (path.empty()) { return false; }
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}
}  // namespace

DWORD WINAPI FileMonitorThread(LPVOID lpParam) {
    _set_se_translator(SEHTranslator);

    try {
        Log("[FMON] FileMonitorThread started.");
        g_isStateOutputAvailable.store(false, std::memory_order_release);
        g_activeGameStateSource.store(GameStateSourceKind::None, std::memory_order_release);

        auto steadyNowMs = []() {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        };

        std::vector<char> buffer;
        buffer.reserve(512);

        HANDLE hFile = INVALID_HANDLE_VALUE;
        GameStateSourceKind openSource = GameStateSourceKind::None;
        FILETIME lastWriteTime{};
        bool haveLastWriteTime = false;

        constexpr DWORD kFileMonitorSleepMs = 8;
        constexpr DWORD kMaxStateFileSize = 65536;
        constexpr long long kSourceCheckIntervalMs = 1000;
        long long lastSourceCheckMs = steadyNowMs() - kSourceCheckIntervalMs;
        GameStateSourceKind activeSource = GameStateSourceKind::None;

        auto closeActiveFile = [&]() {
            if (hFile != INVALID_HANDLE_VALUE) {
                CloseHandle(hFile);
                hFile = INVALID_HANDLE_VALUE;
            }
            openSource = GameStateSourceKind::None;
            haveLastWriteTime = false;
        };

        while (!g_stopMonitoring) {
            Sleep(kFileMonitorSleepMs);

            const long long nowMs = steadyNowMs();
            if (nowMs - lastSourceCheckMs >= kSourceCheckIntervalMs) {
                lastSourceCheckMs = nowMs;
                const bool hermesAlive = IsHermesAlive(g_hermesAliveFilePath);
                const bool stateOutputAvailable = StateOutputFileAvailable(g_stateOutputFilePath);
                activeSource = game_state_source::Select(hermesAlive, stateOutputAvailable);
                g_activeGameStateSource.store(activeSource, std::memory_order_release);
                g_isStateOutputAvailable.store(activeSource != GameStateSourceKind::None, std::memory_order_release);
            }

            if (activeSource == GameStateSourceKind::None) {
                closeActiveFile();
                continue;
            }

            if (activeSource != openSource) {
                closeActiveFile();
                const std::wstring& path = (activeSource == GameStateSourceKind::Hermes) ? g_stateFilePath : g_stateOutputFilePath;
                hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, NULL);
                if (hFile == INVALID_HANDLE_VALUE) { continue; }
                openSource = activeSource;
            }

            FILETIME curWriteTime{};
            if (GetFileTime(hFile, NULL, NULL, &curWriteTime)) {
                if (haveLastWriteTime && CompareFileTime(&lastWriteTime, &curWriteTime) == 0) { continue; }
            }

            if (SetFilePointer(hFile, 0, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER) { continue; }

            DWORD fileSize = GetFileSize(hFile, NULL);
            if (fileSize == 0 || fileSize >= kMaxStateFileSize) { continue; }

            buffer.resize(fileSize);
            DWORD bytesRead = 0;
            if (!ReadFile(hFile, buffer.data(), fileSize, &bytesRead, NULL) || bytesRead != fileSize) { continue; }

            std::string content(buffer.data(), bytesRead);
            std::optional<std::string> state = (openSource == GameStateSourceKind::Hermes)
                                                   ? game_state_source::DeriveStateFromHermesJson(content)
                                                   : game_state_source::DeriveStateFromStateOutputText(content);
            if (!state) { continue; }

            lastWriteTime = curWriteTime;
            haveLastWriteTime = true;

            int currentIdx = g_currentGameStateIndex.load(std::memory_order_acquire);
            if (g_gameStateBuffers[currentIdx] != *state) {
                int nextIdx = 1 - currentIdx;
                g_gameStateBuffers[nextIdx] = *state;
                g_currentGameStateIndex.store(nextIdx, std::memory_order_release);
            }
        }

        closeActiveFile();
        Log("[FMON] FileMonitorThread stopped.");
        return 0;
    } catch (const SE_Exception& e) {
        LogException("FileMonitorThread (SEH)", e.getCode(), e.getInfo());
        return 1;
    } catch (const std::exception& e) {
        LogException("FileMonitorThread", e);
        return 1;
    } catch (...) {
        Log("EXCEPTION in FileMonitorThread: Unknown exception");
        return 1;
    }
}

DWORD WINAPI ImageMonitorThread(LPVOID lpParam) {
    _set_se_translator(SEHTranslator);

    try {
        Log("[IMON] ImageMonitorThread started.");
        static std::map<std::string, FILETIME> s_lastWriteTimes;

        while (!g_stopImageMonitoring) {
            Sleep(250);

            // Use snapshot to avoid racing GUI edits and to allow future lock-free snapshot impls.
            auto cfgSnap = GetConfigSnapshot();
            if (!cfgSnap) { continue; }

            const auto& imagesToCheck = cfgSnap->images;
            if (imagesToCheck.empty()) { continue; }

            for (const auto& img : imagesToCheck) {
                if (img.path.empty()) continue;

                std::wstring final_path = Utf8ToWide(img.path);
                if (PathIsRelativeW(final_path.c_str())) { final_path = g_toolscreenPath + L"\\" + final_path; }

                HANDLE hFile = CreateFileW(final_path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
                if (hFile == INVALID_HANDLE_VALUE) continue;

                FILETIME currentWriteTime;
                if (GetFileTime(hFile, NULL, NULL, &currentWriteTime)) {
                    auto it = s_lastWriteTimes.find(img.name);
                    if (it == s_lastWriteTimes.end() || CompareFileTime(&it->second, &currentWriteTime) != 0) {
                        if (it != s_lastWriteTimes.end()) {
                            Log("[IMON] Detected change in image file, queueing for reload: " + img.path);
                            LoadImageAsync(DecodedImageData::Type::UserImage, img.name, img.path, g_toolscreenPath);
                        }
                        s_lastWriteTimes[img.name] = currentWriteTime;
                    }
                }
                CloseHandle(hFile);
            }
        }
        Log("[IMON] ImageMonitorThread stopped.");
        return 0;
    } catch (const SE_Exception& e) {
        LogException("ImageMonitorThread (SEH)", e.getCode(), e.getInfo());
        return 1;
    } catch (const std::exception& e) {
        LogException("ImageMonitorThread", e);
        return 1;
    } catch (...) {
        Log("EXCEPTION in ImageMonitorThread: Unknown exception");
        return 1;
    }
}

bool MatchesConfiguredInputKeyEvent(DWORD incomingVk, DWORD incomingRawVk, DWORD configuredKey) {
    if (configuredKey == 0) return false;
    if (incomingVk == configuredKey) return true;

    if (configuredKey == VK_CONTROL) {
        return incomingVk == VK_LCONTROL || incomingVk == VK_RCONTROL || incomingRawVk == VK_CONTROL;
    }
    if (configuredKey == VK_SHIFT) {
        return incomingVk == VK_LSHIFT || incomingVk == VK_RSHIFT || incomingRawVk == VK_SHIFT;
    }
    if (configuredKey == VK_MENU) {
        return incomingVk == VK_LMENU || incomingVk == VK_RMENU || incomingRawVk == VK_MENU;
    }

    if (incomingRawVk == VK_CONTROL && incomingVk == VK_CONTROL && (configuredKey == VK_LCONTROL || configuredKey == VK_RCONTROL)) {
        return true;
    }
    if (incomingRawVk == VK_SHIFT && incomingVk == VK_SHIFT && (configuredKey == VK_LSHIFT || configuredKey == VK_RSHIFT)) {
        return true;
    }
    if (incomingRawVk == VK_MENU && incomingVk == VK_MENU && (configuredKey == VK_LMENU || configuredKey == VK_RMENU)) {
        return true;
    }

    return false;
}

bool IsConfiguredInputKeyDown(DWORD key) {
    auto isKeyboardKeyDown = [](int vk) {
        return (GetKeyState(vk) & 0x8000) != 0;
    };
    auto isMouseKeyDown = [](int vk) {
        return (GetAsyncKeyState(vk) & 0x8000) != 0;
    };

    switch (key) {
    case VK_CONTROL:
        return isKeyboardKeyDown(VK_LCONTROL) || isKeyboardKeyDown(VK_RCONTROL);
    case VK_LCONTROL:
        return isKeyboardKeyDown(VK_LCONTROL);
    case VK_RCONTROL:
        return isKeyboardKeyDown(VK_RCONTROL);
    case VK_SHIFT:
        return isKeyboardKeyDown(VK_LSHIFT) || isKeyboardKeyDown(VK_RSHIFT);
    case VK_LSHIFT:
        return isKeyboardKeyDown(VK_LSHIFT);
    case VK_RSHIFT:
        return isKeyboardKeyDown(VK_RSHIFT);
    case VK_MENU:
        return isKeyboardKeyDown(VK_LMENU) || isKeyboardKeyDown(VK_RMENU);
    case VK_LMENU:
        return isKeyboardKeyDown(VK_LMENU);
    case VK_RMENU:
        return isKeyboardKeyDown(VK_RMENU);
    case VK_LBUTTON:
    case VK_RBUTTON:
    case VK_MBUTTON:
    case VK_XBUTTON1:
    case VK_XBUTTON2:
        return isMouseKeyDown(static_cast<int>(key));
    default:
        return isKeyboardKeyDown(static_cast<int>(key));
    }
}

bool CheckHotkeyMatch(const std::vector<DWORD>& keys, WPARAM wParam, const std::vector<DWORD>& exclusionKeys, bool skipLiveKeyStateChecks,
                      size_t minKeyCount, WPARAM rawWParam, bool hasIncomingKeyState, bool incomingIsKeyDown, bool skipExclusionChecks) {
    PROFILE_SCOPE_CAT("Hotkey Match Check", "Game Logic");
    if (keys.empty()) return false;
    if (keys.size() < minKeyCount) return false;

    auto isModifierKey = [](DWORD key) {
        return key == VK_CONTROL || key == VK_LCONTROL || key == VK_RCONTROL || key == VK_SHIFT || key == VK_LSHIFT || key == VK_RSHIFT ||
               key == VK_MENU || key == VK_LMENU || key == VK_RMENU;
    };

    const bool lctrl_down = IsConfiguredInputKeyDown(VK_LCONTROL);
    const bool rctrl_down = IsConfiguredInputKeyDown(VK_RCONTROL);
    const bool lshift_down = IsConfiguredInputKeyDown(VK_LSHIFT);
    const bool rshift_down = IsConfiguredInputKeyDown(VK_RSHIFT);
    const bool lalt_down = IsConfiguredInputKeyDown(VK_LMENU);
    const bool ralt_down = IsConfiguredInputKeyDown(VK_RMENU);
    const bool ctrl_down_now = lctrl_down || rctrl_down;
    const bool shift_down_now = lshift_down || rshift_down;
    const bool alt_down_now = lalt_down || ralt_down;

    auto matchesModifierFamilyEvent = [&](DWORD key) {
        switch (key) {
        case VK_CONTROL:
        case VK_LCONTROL:
        case VK_RCONTROL:
            return wParam == VK_CONTROL || wParam == VK_LCONTROL || wParam == VK_RCONTROL || rawWParam == VK_CONTROL;
        case VK_SHIFT:
        case VK_LSHIFT:
        case VK_RSHIFT:
            return wParam == VK_SHIFT || wParam == VK_LSHIFT || wParam == VK_RSHIFT || rawWParam == VK_SHIFT;
        case VK_MENU:
        case VK_LMENU:
        case VK_RMENU:
            return wParam == VK_MENU || wParam == VK_LMENU || wParam == VK_RMENU || rawWParam == VK_MENU;
        default:
            return false;
        }
    };

    auto getGenericModifierKey = [](DWORD key) {
        switch (key) {
        case VK_CONTROL:
        case VK_LCONTROL:
        case VK_RCONTROL:
            return static_cast<DWORD>(VK_CONTROL);
        case VK_SHIFT:
        case VK_LSHIFT:
        case VK_RSHIFT:
            return static_cast<DWORD>(VK_SHIFT);
        case VK_MENU:
        case VK_LMENU:
        case VK_RMENU:
            return static_cast<DWORD>(VK_MENU);
        default:
            return key;
        }
    };

    auto isModifierDownNow = [&](DWORD key) {
        switch (key) {
        case VK_CONTROL:
            return ctrl_down_now;
        case VK_LCONTROL:
            return lctrl_down;
        case VK_RCONTROL:
            return rctrl_down;
        case VK_SHIFT:
            return shift_down_now;
        case VK_LSHIFT:
            return lshift_down;
        case VK_RSHIFT:
            return rshift_down;
        case VK_MENU:
            return alt_down_now;
        case VK_LMENU:
            return lalt_down;
        case VK_RMENU:
            return ralt_down;
        default:
            return IsConfiguredInputKeyDown(key);
        }
    };

    if (!skipExclusionChecks) {
        for (DWORD excluded_key : exclusionKeys) {
            bool excludedPressed = false;
            if (excluded_key == VK_CONTROL) {
                excludedPressed = ctrl_down_now || IsKeyCurrentlyLowLevelSuppressed(VK_LCONTROL) || IsKeyCurrentlyLowLevelSuppressed(VK_RCONTROL);
            } else if (excluded_key == VK_LCONTROL) {
                excludedPressed = lctrl_down || IsKeyCurrentlyLowLevelSuppressed(VK_LCONTROL);
            } else if (excluded_key == VK_RCONTROL) {
                excludedPressed = rctrl_down || IsKeyCurrentlyLowLevelSuppressed(VK_RCONTROL);
            } else if (excluded_key == VK_SHIFT) {
                excludedPressed = shift_down_now || IsKeyCurrentlyLowLevelSuppressed(VK_LSHIFT) || IsKeyCurrentlyLowLevelSuppressed(VK_RSHIFT);
            } else if (excluded_key == VK_LSHIFT) {
                excludedPressed = lshift_down || IsKeyCurrentlyLowLevelSuppressed(VK_LSHIFT);
            } else if (excluded_key == VK_RSHIFT) {
                excludedPressed = rshift_down || IsKeyCurrentlyLowLevelSuppressed(VK_RSHIFT);
            } else if (excluded_key == VK_MENU) {
                excludedPressed = alt_down_now || IsKeyCurrentlyLowLevelSuppressed(VK_LMENU) || IsKeyCurrentlyLowLevelSuppressed(VK_RMENU);
            } else if (excluded_key == VK_LMENU) {
                excludedPressed = lalt_down || IsKeyCurrentlyLowLevelSuppressed(VK_LMENU);
            } else if (excluded_key == VK_RMENU) {
                excludedPressed = ralt_down || IsKeyCurrentlyLowLevelSuppressed(VK_RMENU);
            } else {
                excludedPressed = IsConfiguredInputKeyDown(excluded_key) || IsKeyCurrentlyLowLevelSuppressed(excluded_key);
            }

            if (excludedPressed) {
                if (g_config.debug.showHotkeyDebug) { Log("[Hotkey] FAIL: Exclusion key " + std::to_string(excluded_key) + " is pressed"); }
                return false;
            }
        }
    }

    DWORD main_key = keys.back();

    bool requires_lctrl = false, requires_rctrl = false, requires_ctrl = false;
    bool requires_lshift = false, requires_rshift = false, requires_shift = false;
    bool requires_lalt = false, requires_ralt = false, requires_alt = false;

    if (!skipLiveKeyStateChecks) {
        for (size_t i = 0; i < keys.size() - 1; ++i) {
            DWORD key = keys[i];
            if (key == VK_LCONTROL)
                requires_lctrl = true;
            else if (key == VK_RCONTROL)
                requires_rctrl = true;
            else if (key == VK_CONTROL)
                requires_ctrl = true;
            else if (key == VK_LSHIFT)
                requires_lshift = true;
            else if (key == VK_RSHIFT)
                requires_rshift = true;
            else if (key == VK_SHIFT)
                requires_shift = true;
            else if (key == VK_LMENU)
                requires_lalt = true;
            else if (key == VK_RMENU)
                requires_ralt = true;
            else if (key == VK_MENU)
                requires_alt = true;
        }
    }

    bool s_enableHotkeyDebug = g_config.debug.showHotkeyDebug;
    std::string keyCombo;

    if (s_enableHotkeyDebug) {
        keyCombo = GetKeyComboString(keys);
        Log("[Hotkey] Check: " + keyCombo + " vs keypress " + std::to_string(wParam));
    }

    // Handle modifier keys specially - Windows may report generic modifiers in wParam,
    // and release-style paths need to reason about post-event modifier state.
    const bool isModifierReleaseEvent = skipLiveKeyStateChecks && hasIncomingKeyState && !incomingIsKeyDown && isModifierKey(main_key) &&
                                        matchesModifierFamilyEvent(main_key);

    bool main_key_pressed = false;

    if (isModifierReleaseEvent) {
        const DWORD genericModifierKey = getGenericModifierKey(main_key);
        const bool isSpecificModifierKey = main_key != genericModifierKey;

        if (isSpecificModifierKey && wParam == main_key) {
            main_key_pressed = true;
        } else if (isSpecificModifierKey && wParam != genericModifierKey) {
            main_key_pressed = false;
        } else {
            main_key_pressed = !isModifierDownNow(main_key);
        }
    } else {
        main_key_pressed = (main_key == wParam);
    }

    if (!main_key_pressed && !isModifierReleaseEvent) {
        switch (main_key) {
        case VK_CONTROL:
            main_key_pressed = (wParam == VK_CONTROL || wParam == VK_LCONTROL || wParam == VK_RCONTROL);
            break;
        case VK_LCONTROL:
            main_key_pressed = (wParam == VK_LCONTROL) || (wParam == VK_CONTROL && (skipLiveKeyStateChecks ? true : lctrl_down));
            break;
        case VK_RCONTROL:
            main_key_pressed = (wParam == VK_RCONTROL) || (wParam == VK_CONTROL && (skipLiveKeyStateChecks ? true : rctrl_down));
            break;
        case VK_SHIFT:
            main_key_pressed = (wParam == VK_SHIFT || wParam == VK_LSHIFT || wParam == VK_RSHIFT);
            break;
        case VK_LSHIFT:
            main_key_pressed = (wParam == VK_LSHIFT) || (wParam == VK_SHIFT && (skipLiveKeyStateChecks ? true : lshift_down));
            break;
        case VK_RSHIFT:
            main_key_pressed = (wParam == VK_RSHIFT) || (wParam == VK_SHIFT && (skipLiveKeyStateChecks ? true : rshift_down));
            break;
        case VK_MENU:
            main_key_pressed = (wParam == VK_MENU || wParam == VK_LMENU || wParam == VK_RMENU);
            break;
        case VK_LMENU:
            main_key_pressed = (wParam == VK_LMENU) || (wParam == VK_MENU && (skipLiveKeyStateChecks ? true : lalt_down));
            break;
        case VK_RMENU:
            main_key_pressed = (wParam == VK_RMENU) || (wParam == VK_MENU && (skipLiveKeyStateChecks ? true : ralt_down));
            break;
        default:
            break;
        }
    }

    if (!main_key_pressed) {
        if (s_enableHotkeyDebug) { Log("[Hotkey] SKIP: main key " + std::to_string(main_key) + " != " + std::to_string(wParam)); }
        return false;
    }

    if (!skipLiveKeyStateChecks) {
        if (s_enableHotkeyDebug) {
            Log("[Hotkey] Modifiers - Need: LCtrl=" + std::to_string(requires_lctrl) + " RCtrl=" + std::to_string(requires_rctrl) +
                " Ctrl=" + std::to_string(requires_ctrl) + " LShift=" + std::to_string(requires_lshift) + " RShift=" +
                std::to_string(requires_rshift) + " Shift=" + std::to_string(requires_shift) + " LAlt=" + std::to_string(requires_lalt) +
                " RAlt=" + std::to_string(requires_ralt) + " Alt=" + std::to_string(requires_alt));
            Log("[Hotkey] Modifiers - Have: LCtrl=" + std::to_string(lctrl_down) + " RCtrl=" + std::to_string(rctrl_down) +
                " LShift=" + std::to_string(lshift_down) + " RShift=" + std::to_string(rshift_down) + " LAlt=" + std::to_string(lalt_down) +
                " RAlt=" + std::to_string(ralt_down));
        }

        if (requires_lctrl && !lctrl_down) {
            if (s_enableHotkeyDebug) Log("[Hotkey] FAIL: Left Ctrl required but not pressed");
            return false;
        }
        if (requires_rctrl && !rctrl_down) {
            if (s_enableHotkeyDebug) Log("[Hotkey] FAIL: Right Ctrl required but not pressed");
            return false;
        }
        if (requires_ctrl && !ctrl_down_now) {
            if (s_enableHotkeyDebug) Log("[Hotkey] FAIL: Ctrl required but not pressed");
            return false;
        }
        if (requires_lshift && !lshift_down) {
            if (s_enableHotkeyDebug) Log("[Hotkey] FAIL: Left Shift required but not pressed");
            return false;
        }
        if (requires_rshift && !rshift_down) {
            if (s_enableHotkeyDebug) Log("[Hotkey] FAIL: Right Shift required but not pressed");
            return false;
        }
        if (requires_shift && !shift_down_now) {
            if (s_enableHotkeyDebug) Log("[Hotkey] FAIL: Shift required but not pressed");
            return false;
        }
        if (requires_lalt && !lalt_down) {
            if (s_enableHotkeyDebug) Log("[Hotkey] FAIL: Left Alt required but not pressed");
            return false;
        }
        if (requires_ralt && !ralt_down) {
            if (s_enableHotkeyDebug) Log("[Hotkey] FAIL: Right Alt required but not pressed");
            return false;
        }
        if (requires_alt && !alt_down_now) {
            if (s_enableHotkeyDebug) Log("[Hotkey] FAIL: Alt required but not pressed");
            return false;
        }

        for (size_t i = 0; i + 1 < keys.size(); ++i) {
            DWORD requiredKey = keys[i];
            if (isModifierKey(requiredKey)) continue;
            if (requiredKey == 0) continue;

            if (!IsConfiguredInputKeyDown(requiredKey)) {
                if (s_enableHotkeyDebug) {
                    Log("[Hotkey] FAIL: Required key " + std::to_string(requiredKey) + " is not pressed");
                }
                return false;
            }
        }
    } else {
        if (s_enableHotkeyDebug) { Log("[Hotkey] Skipping live key-state checks for release-style hotkey evaluation"); }
    }

    if (s_enableHotkeyDebug) {
        if (keyCombo.empty()) keyCombo = GetKeyComboString(keys);
        Log("[Hotkey] ✓ MATCH: " + keyCombo);
    }

    return true;
}

static std::vector<DWORD> NormalizeKeysForComparison(const std::vector<DWORD>& keys, bool collapseCtrlSides, bool collapseShiftSides,
                                                     bool collapseAltSides) {
    if (keys.empty()) return {};

    auto normalizeModifier = [collapseCtrlSides, collapseShiftSides, collapseAltSides](DWORD k) -> DWORD {
        if (collapseCtrlSides && (k == VK_LCONTROL || k == VK_RCONTROL)) return VK_CONTROL;
        if (collapseShiftSides && (k == VK_LSHIFT || k == VK_RSHIFT)) return VK_SHIFT;
        if (collapseAltSides && (k == VK_LMENU || k == VK_RMENU)) return VK_MENU;
        return k;
    };

    std::vector<DWORD> result;
    result.reserve(keys.size());
    for (size_t i = 0; i + 1 < keys.size(); ++i)
        result.push_back(normalizeModifier(keys[i]));
    std::sort(result.begin(), result.end());
    result.push_back(normalizeModifier(keys.back()));
    return result;
}

static bool ContainsGenericAltKey(const std::vector<DWORD>& keys) {
    return std::find(keys.begin(), keys.end(), VK_MENU) != keys.end();
}

static bool ContainsGenericCtrlKey(const std::vector<DWORD>& keys) {
    return std::find(keys.begin(), keys.end(), VK_CONTROL) != keys.end();
}

static bool ContainsGenericShiftKey(const std::vector<DWORD>& keys) {
    return std::find(keys.begin(), keys.end(), VK_SHIFT) != keys.end();
}

std::string FindHotkeyConflict(const std::vector<DWORD>& newKeys, const std::string& excludeLabel) {
    if (newKeys.empty()) return "";

    struct Entry {
        const std::vector<DWORD>* keys;
        std::string label;
    };

    std::vector<Entry> all;

    auto add = [&](const std::vector<DWORD>& k, std::string lbl) {
        if (!k.empty()) all.push_back({ &k, std::move(lbl) });
    };

    add(g_config.guiHotkey, "GUI Toggle");
    add(g_config.borderlessHotkey, "Borderless Toggle");
    add(g_config.imageOverlaysHotkey, "Image Overlays Toggle");
    add(g_config.windowOverlaysHotkey, "Window Overlays Toggle");
    add(g_config.ninjabrainOverlayHotkey, "Ninjabrain Overlay Toggle");
    add(g_config.keyRebinds.toggleHotkey, "Key Rebinds Toggle");

    for (size_t i = 0; i < g_config.hotkeys.size(); ++i) {
        const auto& hk = g_config.hotkeys[i];
        add(hk.keys, "Mode Hotkey #" + std::to_string(i + 1));
        for (size_t j = 0; j < hk.altSecondaryModes.size(); ++j)
            add(hk.altSecondaryModes[j].keys, "Mode Hotkey #" + std::to_string(i + 1) + " Alt #" + std::to_string(j + 1));
    }

    for (size_t i = 0; i < g_config.sensitivityHotkeys.size(); ++i)
        add(g_config.sensitivityHotkeys[i].keys, "Sensitivity Hotkey #" + std::to_string(i + 1));

    auto normalizedStrict = NormalizeKeysForComparison(newKeys, false, false, false);
    const bool newHasGenericAlt = ContainsGenericAltKey(newKeys);
    const bool newHasGenericCtrl = ContainsGenericCtrlKey(newKeys);
    const bool newHasGenericShift = ContainsGenericShiftKey(newKeys);

    for (const auto& entry : all) {
        if (entry.label == excludeLabel) continue;
        auto existingStrict = NormalizeKeysForComparison(*entry.keys, false, false, false);
        if (existingStrict == normalizedStrict)
            return entry.label;

        const bool existingHasGenericCtrl = ContainsGenericCtrlKey(*entry.keys);
        const bool existingHasGenericShift = ContainsGenericShiftKey(*entry.keys);
        const bool existingHasGenericAlt = ContainsGenericAltKey(*entry.keys);
        const bool collapseCtrlSides = newHasGenericCtrl || existingHasGenericCtrl;
        const bool collapseShiftSides = newHasGenericShift || existingHasGenericShift;
        const bool collapseAltSides = newHasGenericAlt || existingHasGenericAlt;

        if ((collapseCtrlSides || collapseShiftSides || collapseAltSides) &&
            NormalizeKeysForComparison(*entry.keys, collapseCtrlSides, collapseShiftSides, collapseAltSides) ==
                NormalizeKeysForComparison(newKeys, collapseCtrlSides, collapseShiftSides, collapseAltSides)) {
            return entry.label;
        }
    }

    return "";
}

void GetRelativeCoords(const std::string& type, int relX, int relY, int w, int h, int containerW, int containerH, int& outX, int& outY) {
    std::string anchor = type;
    if (anchor.length() > 8 && anchor.substr(anchor.length() - 8) == "Viewport") {
        anchor = anchor.substr(0, anchor.length() - 8);
    } else if (anchor.length() > 6 && anchor.substr(anchor.length() - 6) == "Screen") {
        anchor = anchor.substr(0, anchor.length() - 6);
    }

    char firstChar = anchor.empty() ? '\0' : anchor[0];

    if (firstChar == 't') {
        outY = relY;
        outX = (anchor == "topLeft") ? relX : containerW - w - relX;
    } else if (firstChar == 'c') {
        outX = (containerW - w) / 2 + relX;
        outY = (containerH - h) / 2 + relY;
    } else if (firstChar == 'p') {
        const int PIE_Y_TOP = 220, PIE_X_LEFT = 92, PIE_X_RIGHT = 36;
        int base_x = (anchor == "pieLeft") ? containerW - PIE_X_LEFT : containerW - PIE_X_RIGHT;
        outX = base_x + relX;
        outY = containerH - PIE_Y_TOP + relY;
    } else {
        outY = containerH - h - relY;
        outX = (anchor == "bottomRight") ? containerW - w - relX : relX;
    }
}

void GetRelativeCoordsForImage(const std::string& type, int relX, int relY, int w, int h, int containerW, int containerH, int& outX,
                               int& outY) {
    int anchor_x = 0, anchor_y = 0;
    char firstChar = type.empty() ? '\0' : type[0];

    if (firstChar == 't') {
        anchor_x = (type == "topLeft") ? 0 : containerW - w;
        anchor_y = 0;
    } else if (firstChar == 'c') {
        anchor_x = (containerW - w) / 2;
        anchor_y = (containerH - h) / 2;
    } else if (firstChar == 'b') {
        anchor_x = (type == "bottomLeft") ? 0 : containerW - w;
        anchor_y = containerH - h;
    }

    outX = anchor_x + relX;
    outY = anchor_y + relY;
}

void GetRelativeCoordsForImageWithViewport(const std::string& type, int relX, int relY, int w, int h, int gameX, int gameY, int gameW,
                                           int gameH, int fullW, int fullH, int& outX, int& outY) {
    if (type.length() > 8 && type.substr(type.length() - 8) == "Viewport") {
        std::string baseAnchor = type.substr(0, type.length() - 8);

        int anchor_x = 0, anchor_y = 0;
        char firstChar = baseAnchor.empty() ? '\0' : baseAnchor[0];

        if (firstChar == 't') {
            anchor_x = (baseAnchor == "topLeft") ? 0 : gameW - w;
            anchor_y = 0;
        } else if (firstChar == 'c') {
            anchor_x = (gameW - w) / 2;
            anchor_y = (gameH - h) / 2;
        } else if (firstChar == 'b') {
            anchor_x = (baseAnchor == "bottomLeft") ? 0 : gameW - w;
            anchor_y = gameH - h;
        }

        outX = gameX + anchor_x + relX;
        outY = gameY + anchor_y + relY;
    } else {
        std::string baseAnchor = type;
        if (type.length() > 6 && type.substr(type.length() - 6) == "Screen") { baseAnchor = type.substr(0, type.length() - 6); }
        GetRelativeCoordsForImage(baseAnchor, relX, relY, w, h, fullW, fullH, outX, outY);
    }
}

void CalculateFinalScreenPos(const MirrorConfig* conf, const MirrorInstance& inst, int gameW, int gameH, int finalX, int finalY, int finalW,
                             int finalH, int fullW, int fullH, int& outScreenX, int& outScreenY) {
    (void)gameW;
    (void)gameH;

    float scaleX = conf->output.separateScale ? conf->output.scaleX : conf->output.scale;
    float scaleY = conf->output.separateScale ? conf->output.scaleY : conf->output.scale;
    int outW = static_cast<int>(inst.fbo_w * scaleX);
    int outH = static_cast<int>(inst.fbo_h * scaleY);

    std::string anchor = conf->output.relativeTo;
    int offsetX = conf->output.x;
    int offsetY = conf->output.y;

    if (anchor.length() > 6 && anchor.substr(anchor.length() - 6) == "Screen") {
        anchor = anchor.substr(0, anchor.length() - 6);
        int relative_x, relative_y;
        GetRelativeCoords(anchor, offsetX, offsetY, outW, outH, fullW, fullH, relative_x, relative_y);
        outScreenX = relative_x;
        outScreenY = relative_y;
        return;
    }

    if (anchor.length() > 8 && anchor.substr(anchor.length() - 8) == "Viewport") { anchor = anchor.substr(0, anchor.length() - 8); }

    int relative_x = 0;
    int relative_y = 0;
    GetRelativeCoords(anchor, offsetX, offsetY, outW, outH, finalW, finalH, relative_x, relative_y);
    outScreenX = finalX + relative_x;
    outScreenY = finalY + relative_y;
}

void ScreenDeltaToMirrorConfigDelta(const std::string& relativeTo,
                                    int screenDx, int screenDy,
                                    int gameW, int gameH,
                                    int finalW, int finalH,
                                    int& outConfigDx, int& outConfigDy) {
    const std::string& anchor = relativeTo;

    if (anchor.length() > 6 && anchor.substr(anchor.length() - 6) == "Screen") {
        outConfigDx = screenDx;
        outConfigDy = screenDy;

        if (anchor == "topRightScreen" || anchor == "bottomRightScreen") { outConfigDx = -screenDx; }
        if (anchor == "bottomLeftScreen" || anchor == "bottomRightScreen") { outConfigDy = -screenDy; }
        return;
    }

    float xScale = (gameW > 0 && finalW > 0) ? static_cast<float>(finalW) / gameW : 1.0f;
    float yScale = (gameH > 0 && finalH > 0) ? static_cast<float>(finalH) / gameH : 1.0f;

    int gameDx = (xScale > 0.0f) ? static_cast<int>(screenDx / xScale) : screenDx;
    int gameDy = (yScale > 0.0f) ? static_cast<int>(screenDy / yScale) : screenDy;

    outConfigDx = gameDx;
    outConfigDy = gameDy;

    if (anchor == "topRightViewport" || anchor == "bottomRightViewport") { outConfigDx = -gameDx; }
    if (anchor == "bottomLeftViewport" || anchor == "bottomRightViewport") { outConfigDy = -gameDy; }
}

void ScreenshotToClipboard(int width, int height) {
    PROFILE_SCOPE_CAT("Screenshot to Clipboard", "System");
    if (width <= 0 || height <= 0) {
        Log("ERROR: Screenshot request rejected due to invalid dimensions " + std::to_string(width) + "x" + std::to_string(height) + ".");
        return;
    }

    size_t bufferSize = 0;
    if (!TryComputeImageByteCount(width, height, 4, bufferSize)) {
        Log("ERROR: Screenshot request rejected because byte-count overflowed for dimensions " + std::to_string(width) + "x" +
            std::to_string(height) + ".");
        return;
    }
    if (bufferSize > kMaxScreenshotBytes) {
        Log("ERROR: Screenshot request rejected because buffer size " + FormatByteCount(bufferSize) + " exceeds guard limit of " +
            FormatByteCount(kMaxScreenshotBytes) + " for dimensions " + std::to_string(width) + "x" + std::to_string(height) + ".");
        return;
    }

    Log("Taking screenshot at " + std::to_string(width) + "x" + std::to_string(height) + " (" + FormatByteCount(bufferSize) + ").");
    std::vector<BYTE> pixels(bufferSize);

    GLint previousPackRowLength = 0;
    glGetIntegerv(GL_PACK_ROW_LENGTH, &previousPackRowLength);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glPixelStorei(GL_PACK_ROW_LENGTH, previousPackRowLength);

    for (size_t i = 0; i < bufferSize; i += 4) {
        std::swap(pixels[i + 0], pixels[i + 2]);
    }

    if (!OpenClipboard(g_minecraftHwnd.load())) {
        Log("ERROR: Could not open clipboard.");
        return;
    }
    if (!EmptyClipboard()) {
        Log("ERROR: Could not empty clipboard.");
        CloseClipboard();
        return;
    }

    if (pixels.size() > (std::numeric_limits<SIZE_T>::max)() - sizeof(BITMAPINFOHEADER)) {
        Log("ERROR: Screenshot clipboard payload size overflowed for dimensions " + std::to_string(width) + "x" +
            std::to_string(height) + ".");
        CloseClipboard();
        return;
    }

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + pixels.size());
    if (!hMem) {
        Log("ERROR: GlobalAlloc failed.");
        CloseClipboard();
        return;
    }

    LPVOID pMem = GlobalLock(hMem);
    if (!pMem) {
        Log("ERROR: GlobalLock failed.");
        GlobalFree(hMem);
        CloseClipboard();
        return;
    }

    BITMAPINFOHEADER bih = { 0 };
    bih.biSize = sizeof(BITMAPINFOHEADER);
    bih.biWidth = width;
    bih.biHeight = height;
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;

    memcpy(pMem, &bih, sizeof(bih));
    memcpy(static_cast<BYTE*>(pMem) + sizeof(bih), pixels.data(), pixels.size());
    GlobalUnlock(hMem);

    if (!SetClipboardData(CF_DIB, hMem)) {
        Log("ERROR: SetClipboardData failed with error code: " + std::to_string(GetLastError()));
        GlobalFree(hMem);
    } else {
        Log("Screenshot copied to clipboard.");
    }
    CloseClipboard();
}

void BackupConfigFile() {
    PROFILE_SCOPE_CAT("Config Backup", "IO Operations");

    if (g_toolscreenPath.empty()) {
        Log("Cannot backup config, toolscreen path is not available.");
        return;
    }

    std::wstring configPath = g_toolscreenPath + L"\\config.toml";
    std::wstring backupDir = g_toolscreenPath + L"\\backups";

    if (GetFileAttributesW(configPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Log("Config file does not exist, skipping backup.");
        return;
    }

    CreateDirectoryW(backupDir.c_str(), NULL);

    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

    std::wstring backupFileName = backupDir + L"\\config_" + std::to_wstring(timestamp) + L".toml";

    if (CopyFileW(configPath.c_str(), backupFileName.c_str(), FALSE)) {
        Log("Config backed up to: " + WideToUtf8(backupFileName));

        WIN32_FIND_DATAW findData;
        std::vector<std::pair<FILETIME, std::wstring>> backupFiles;

        std::wstring searchPattern = backupDir + L"\\config_*.toml";
        HANDLE hFind = FindFirstFileW(searchPattern.c_str(), &findData);

        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    std::wstring fileName = findData.cFileName;
                    if (fileName.find(L"config_") == 0 && fileName.find(L".toml") != std::wstring::npos) {
                        std::wstring fullPath = backupDir + L"\\" + fileName;
                        backupFiles.push_back(std::make_pair(findData.ftLastWriteTime, fullPath));
                    }
                }
            } while (FindNextFileW(hFind, &findData));
            FindClose(hFind);
        }

        std::sort(backupFiles.begin(), backupFiles.end(),
                  [](const std::pair<FILETIME, std::wstring>& a, const std::pair<FILETIME, std::wstring>& b) {
                      return CompareFileTime(&a.first, &b.first) > 0;
                  });

        if (backupFiles.size() > 50) {
            for (size_t i = 50; i < backupFiles.size(); i++) {
                if (DeleteFileW(backupFiles[i].second.c_str())) {
                    Log("Deleted old backup: " + WideToUtf8(backupFiles[i].second));
                } else {
                    Log("Failed to delete old backup: " + WideToUtf8(backupFiles[i].second));
                }
            }
        }
    } else {
        DWORD error = GetLastError();
        Log("Failed to backup config file. Error code: " + std::to_string(error));
    }
}

UINT GetToolscreenBorderlessToggleMessageId() {
    static const UINT s_msg = RegisterWindowMessageA("Toolscreen_BorderlessToggle");
    return s_msg;
}

static std::atomic<int> s_lastRequestedClientW{ 0 };
static std::atomic<int> s_lastRequestedClientH{ 0 };
static std::atomic<int> s_prevRequestedClientW{ 0 };
static std::atomic<int> s_prevRequestedClientH{ 0 };

static void RememberRequestedWindowClientResize_Internal(int width, int height) {
    if (width <= 0 || height <= 0) { return; }

    const int lastRequestedW = s_lastRequestedClientW.load(std::memory_order_relaxed);
    const int lastRequestedH = s_lastRequestedClientH.load(std::memory_order_relaxed);
    if (lastRequestedW != width || lastRequestedH != height) {
        s_prevRequestedClientW.store(lastRequestedW, std::memory_order_relaxed);
        s_prevRequestedClientH.store(lastRequestedH, std::memory_order_relaxed);
        s_lastRequestedClientW.store(width, std::memory_order_relaxed);
        s_lastRequestedClientH.store(height, std::memory_order_relaxed);
    }
}

void RememberRequestedWindowClientResize(int width, int height) {
    RememberRequestedWindowClientResize_Internal(width, height);
}

bool GetRecentRequestedWindowClientResizes(int& outCurrentW, int& outCurrentH, int& outPreviousW, int& outPreviousH) {
    outCurrentW = s_lastRequestedClientW.load(std::memory_order_relaxed);
    outCurrentH = s_lastRequestedClientH.load(std::memory_order_relaxed);
    outPreviousW = s_prevRequestedClientW.load(std::memory_order_relaxed);
    outPreviousH = s_prevRequestedClientH.load(std::memory_order_relaxed);
    return outCurrentW > 0 && outCurrentH > 0;
}

static bool GetCenteredWindowedRestoreRect(HWND hwnd, RECT& outRect) {
    RECT monitorRect{};
    if (!GetMonitorRectForWindow(hwnd, monitorRect)) {
        if (!GetWindowRect(hwnd, &monitorRect)) { return false; }
    }

    const int monitorW = monitorRect.right - monitorRect.left;
    const int monitorH = monitorRect.bottom - monitorRect.top;
    if (monitorW <= 0 || monitorH <= 0) { return false; }

    const int windowedW = (std::max)(1, monitorW / 2);
    const int windowedH = (std::max)(1, monitorH / 2);

    outRect.left = monitorRect.left + (monitorW - windowedW) / 2;
    outRect.top = monitorRect.top + (monitorH - windowedH) / 2;
    outRect.right = outRect.left + windowedW;
    outRect.bottom = outRect.top + windowedH;
    return true;
}

static bool ApplyCenteredWindowedRestore(HWND hwnd, UINT extraFlags, const char* source, RECT* appliedRect = nullptr) {
    if (!hwnd || !IsWindow(hwnd)) { return false; }

    if (IsIconic(hwnd) || IsZoomed(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    }

    RECT targetRect{};
    if (!GetCenteredWindowedRestoreRect(hwnd, targetRect)) { return false; }

    const int targetW = targetRect.right - targetRect.left;
    const int targetH = targetRect.bottom - targetRect.top;
    if (!SetWindowPos(hwnd, HWND_NOTOPMOST, targetRect.left, targetRect.top, targetW, targetH, SWP_NOOWNERZORDER | extraFlags)) {
        std::string src = source ? source : "unknown";
        Log("[WINDOW] SetWindowPos failed while centering windowed restore (" + src + "). Error=" + std::to_string(GetLastError()));
        return false;
    }

    if (appliedRect) { *appliedRect = targetRect; }
    return true;
}

bool CenterWindowedRestoreOnCurrentMonitor(HWND hwnd, const char* source) {
    RECT targetRect{};
    if (!ApplyCenteredWindowedRestore(hwnd, 0, source, &targetRect)) { return false; }

    const int targetW = targetRect.right - targetRect.left;
    const int targetH = targetRect.bottom - targetRect.top;
    std::string src = source ? source : "unknown";
    Log("[WINDOW] Centered windowed restore (" + src + ") -> " + std::to_string(targetW) + "x" + std::to_string(targetH) +
        " at " + std::to_string(targetRect.left) + "," + std::to_string(targetRect.top));
    return true;
}

bool RequestWindowClientResize(HWND hwnd, int width, int height, const char* source) {
    if (!hwnd || !IsWindow(hwnd) || width <= 0 || height <= 0) { return false; }

    static std::mutex s_resizeRequestMutex;
    static HWND s_lastHwnd = nullptr;
    static int s_lastWidth = 0;
    static int s_lastHeight = 0;
    static ULONGLONG s_lastPostedMs = 0;

    const ULONGLONG nowMs = GetTickCount64();

    std::lock_guard<std::mutex> lock(s_resizeRequestMutex);

    RememberRequestedWindowClientResize_Internal(width, height);

    if (s_lastHwnd == hwnd && s_lastWidth == width && s_lastHeight == height && (nowMs - s_lastPostedMs) <= 50) { return true; }

    if (!PostMessage(hwnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(width, height))) {
        DWORD err = GetLastError();
        std::string src = source ? source : "unknown";
        Log("[WINDOW] Failed to post WM_SIZE resize request (" + src + "): " + std::to_string(width) + "x" + std::to_string(height) +
            ", error=" + std::to_string(err));
        return false;
    }

    s_lastHwnd = hwnd;
    s_lastWidth = width;
    s_lastHeight = height;
    s_lastPostedMs = nowMs;

    InvalidateTrackedGameTextureId(false, false);
    return true;
}

static void RequestCurrentModeClientResizeSync(HWND hwnd, const char* source) {
    if (!hwnd || !IsWindow(hwnd)) { return; }

    auto cfgSnap = GetConfigSnapshot();
    if (!cfgSnap) { return; }

    const std::string currentModeId = g_modeIdBuffers[g_currentModeIdIndex.load(std::memory_order_acquire)];
    const ModeConfig* mode = GetModeFromSnapshotOrFallback(*cfgSnap, currentModeId);
    if (!mode || mode->width <= 0 || mode->height <= 0) { return; }

    if (EqualsIgnoreCase(mode->id, "Fullscreen") && mode->useRelativeSize) {
        // Real window-size changes will trigger a logic-thread recalculation that reposts
        // WM_SIZE with the freshly recomputed internal size. Avoid sending the stale
        // pre-recalc fullscreen-relative dimensions here.
        return;
    }

    RequestWindowClientResize(hwnd, mode->width, mode->height, source);
}

void ToggleBorderlessWindowedFullscreen(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) { return; }

    const DWORD windowThreadId = GetWindowThreadProcessId(hwnd, nullptr);
    const DWORD currentThreadId = GetCurrentThreadId();
    if (windowThreadId != 0 && windowThreadId != currentThreadId) {
        const UINT msg = GetToolscreenBorderlessToggleMessageId();
        if (!PostMessage(hwnd, msg, 0, 0)) {
            Log("[WINDOW] Failed to post borderless toggle request to window thread. Error=" + std::to_string(GetLastError()));
        }
        return;
    }

    struct BorderlessWindowState {
        bool active = false;
        bool saved = false;
        DWORD savedStyle = 0;
        DWORD savedExStyle = 0;
    };

    static std::mutex s_borderlessMutex;
    static std::unordered_map<HWND, BorderlessWindowState> s_borderlessStateByHwnd;

    std::lock_guard<std::mutex> lock(s_borderlessMutex);
    BorderlessWindowState& state = s_borderlessStateByHwnd[hwnd];

    RECT targetRect{};
    if (!GetMonitorRectForWindow(hwnd, targetRect)) {
        if (!GetWindowRect(hwnd, &targetRect)) { return; }
    }

    const int targetW = (targetRect.right - targetRect.left);
    const int targetH = (targetRect.bottom - targetRect.top);

    RECT beforeRect{};
    if (!GetWindowRect(hwnd, &beforeRect)) { return; }

    const int beforeW = beforeRect.right - beforeRect.left;
    const int beforeH = beforeRect.bottom - beforeRect.top;

    DWORD beforeStyle = static_cast<DWORD>(GetWindowLongPtr(hwnd, GWL_STYLE));
    DWORD beforeExStyle = static_cast<DWORD>(GetWindowLongPtr(hwnd, GWL_EXSTYLE));

    auto setWindowLongChecked = [&](int index, DWORD value) {
        SetLastError(0);
        LONG_PTR prev = SetWindowLongPtr(hwnd, index, static_cast<LONG_PTR>(value));
        if (prev == 0 && GetLastError() != 0) {
            Log("[WINDOW] SetWindowLongPtr failed (index=" + std::to_string(index) + ", error=" + std::to_string(GetLastError()) + ")");
            return false;
        }
        return true;
    };

    auto rollbackWindowState = [&]() {
        SetWindowLongPtr(hwnd, GWL_STYLE, static_cast<LONG_PTR>(beforeStyle));
        SetWindowLongPtr(hwnd, GWL_EXSTYLE, static_cast<LONG_PTR>(beforeExStyle));
        if (beforeW > 0 && beforeH > 0) {
            SetWindowPos(hwnd, HWND_NOTOPMOST, beforeRect.left, beforeRect.top, beforeW, beforeH, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        } else {
            SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE);
        }
    };

    if (!state.active) {
        state.savedStyle = beforeStyle;
        state.savedExStyle = beforeExStyle;
        state.saved = true;

        if (IsIconic(hwnd) || IsZoomed(hwnd)) {
            ShowWindow(hwnd, SW_RESTORE);
        }

        bool ok = true;

        DWORD style = beforeStyle;
        style &= ~(WS_POPUP | WS_CAPTION | WS_BORDER | WS_DLGFRAME | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
        style |= WS_OVERLAPPED;
        if (style != beforeStyle) {
            ok = setWindowLongChecked(GWL_STYLE, style);
        }

        DWORD exStyle = beforeExStyle;
        exStyle &= ~(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME | WS_EX_STATICEDGE);
        exStyle |= WS_EX_APPWINDOW;
        if (ok && exStyle != beforeExStyle) {
            ok = setWindowLongChecked(GWL_EXSTYLE, exStyle);
        }

        if (ok) {
            ok = SetWindowPos(hwnd, HWND_NOTOPMOST, targetRect.left, targetRect.top, targetW, targetH, SWP_NOOWNERZORDER | SWP_FRAMECHANGED) !=
                 FALSE;
            if (!ok) { Log("[WINDOW] SetWindowPos failed while enabling borderless. Error=" + std::to_string(GetLastError())); }
        }

        if (!ok) {
            rollbackWindowState();
            return;
        }

        InvalidateTrackedGameTextureId(false, false);
        state.active = true;
        RequestScreenMetricsRecalculation();
        RequestCurrentModeClientResizeSync(hwnd, "window:borderless_on");
        Log("[WINDOW] Toggled borderless ON (" + std::to_string(targetW) + "x" + std::to_string(targetH) + ")");
    } else {
        if (IsIconic(hwnd) || IsZoomed(hwnd)) {
            ShowWindow(hwnd, SW_RESTORE);
        }

        bool ok = true;

        DWORD targetStyle = state.saved ? state.savedStyle : beforeStyle;
        targetStyle &= ~(WS_POPUP);
        targetStyle |= WS_OVERLAPPEDWINDOW;
        if (targetStyle != beforeStyle) {
            ok = setWindowLongChecked(GWL_STYLE, targetStyle);
        }

        DWORD targetExStyle = state.saved ? state.savedExStyle : beforeExStyle;
        targetExStyle &= ~(WS_EX_TOPMOST | WS_EX_TOOLWINDOW);
        targetExStyle |= WS_EX_APPWINDOW;
        if (ok && targetExStyle != beforeExStyle) {
            ok = setWindowLongChecked(GWL_EXSTYLE, targetExStyle);
        }

        RECT centeredRect{};
        if (ok) {
            ok = ApplyCenteredWindowedRestore(hwnd, SWP_FRAMECHANGED, "window:borderless_off", &centeredRect);
        }

        if (!ok) {
            rollbackWindowState();
            return;
        }

        InvalidateTrackedGameTextureId(false, false);
        state.active = false;
        RequestScreenMetricsRecalculation();
        RequestCurrentModeClientResizeSync(hwnd, "window:borderless_off");
        const int centeredW = centeredRect.right - centeredRect.left;
        const int centeredH = centeredRect.bottom - centeredRect.top;
        Log("[WINDOW] Toggled borderless OFF -> windowed centered (" + std::to_string(centeredW) + "x" + std::to_string(centeredH) + ")");
    }
}