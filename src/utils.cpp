#include "utils.h"
#include "gui.h"
#include "logic_thread.h"
#include "profiler.h"

#include "stb_image.h"
#include <DbgHelp.h>
#include <ShlObj.h>
#include <Shlwapi.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <eh.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <shared_mutex>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "DbgHelp.lib")

// Symbol resolution state
static std::atomic<bool> g_symbolsInitialized{ false };
static std::atomic<bool> g_symbolInitAttempted{ false };
static std::mutex g_symbolMutex;

// Forward declaration
void EnsureSymbolsInitialized();

// Helper to resolve a single stack frame to function name + line number
std::string ResolveStackFrame(void* address) {
    // Try to initialize symbols if not done yet
    EnsureSymbolsInitialized();

    if (!g_symbolsInitialized.load()) {
        // Still not initialized, return address only
        std::stringstream ss;
        ss << "0x" << std::hex << reinterpret_cast<uintptr_t>(address) << " (symbols failed)";
        return ss.str();
    }

    std::lock_guard<std::mutex> lock(g_symbolMutex);

    HANDLE process = GetCurrentProcess();
    DWORD64 addr64 = reinterpret_cast<DWORD64>(address);

    // Allocate symbol info structure
    char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)];
    PSYMBOL_INFO symbol = reinterpret_cast<PSYMBOL_INFO>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;

    std::stringstream result;
    result << "0x" << std::hex << addr64;

    // Try to get function name
    DWORD64 displacement = 0;
    if (SymFromAddr(process, addr64, &displacement, symbol)) {
        result << " " << symbol->Name;
        if (displacement != 0) { result << "+0x" << std::hex << displacement; }

        // Try to get file and line number
        IMAGEHLP_LINE64 line;
        line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
        DWORD lineDisplacement = 0;
        if (SymGetLineFromAddr64(process, addr64, &lineDisplacement, &line)) {
            // Extract just the filename from full path
            const char* filename = strrchr(line.FileName, '\\');
            if (!filename)
                filename = line.FileName;
            else
                filename++; // Skip the backslash

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

// Signal handler for SIGABRT
void SignalHandler(int sig) {
    if (sig == SIGABRT) {
        Log("!!! SIGABRT SIGNAL RECEIVED - ABNORMAL TERMINATION !!!");
        FlushLogs(); // Force flush async log queue

        // Capture stack trace
        void* stack[64];
        USHORT frames = CaptureStackBackTrace(1, 64, stack, NULL);
        Log(FormatStackTraceWithSymbols(stack, frames));
        FlushLogs(); // Force flush after stack trace
    }

    // Re-raise signal to let default handler terminate
    signal(sig, SIG_DFL);
    raise(sig);
}

// Override abort() to log before terminating
// Commented out - conflicts with C runtime when using /MD
/*
extern "C" void abort() {
    // Prevent re-entry if abort is called recursively
    static std::atomic<bool> abortInProgress{ false };
    if (abortInProgress.exchange(true)) {
        // Already handling abort - just raise signal to avoid infinite loop
        signal(SIGABRT, SIG_DFL);
        raise(SIGABRT);
        // If raise returns (shouldn't), force terminate
        TerminateProcess(GetCurrentProcess(), 3);
    }

    // Get additional context before logging
    DWORD lastError = GetLastError();
    DWORD threadId = GetCurrentThreadId();
    DWORD processId = GetCurrentProcessId();

    std::stringstream contextSs;
    contextSs << "=== ABORT() CALLED ===" << std::endl;
    contextSs << "Thread ID: " << threadId << std::endl;
    contextSs << "Process ID: " << processId << std::endl;
    if (lastError != 0) {
        contextSs << "GetLastError: " << lastError << " (0x" << std::hex << lastError << std::dec << ")";

        // Try to get error message
        LPSTR messageBuffer = nullptr;
        size_t size = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
                                     lastError, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&messageBuffer, 0, NULL);
        if (size > 0 && messageBuffer) {
            std::string errorMsg(messageBuffer, size);
            // Remove trailing newlines
            while (!errorMsg.empty() && (errorMsg.back() == '\n' || errorMsg.back() == '\r')) { errorMsg.pop_back(); }
            contextSs << " - " << errorMsg;
            LocalFree(messageBuffer);
        }
        contextSs << std::endl;
    }

    // Try to get C++ exception info
    std::exception_ptr eptr = std::current_exception();
    if (eptr) {
        try {
            std::rethrow_exception(eptr);
        } catch (const std::exception& e) { contextSs << "Active C++ Exception: " << e.what() << std::endl; } catch (...) {
            contextSs << "Active C++ Exception: (unknown type)" << std::endl;
        }
    }

    Log(contextSs.str());
    FlushLogs(); // Force flush async log queue

    // Capture stack trace at abort point
    void* stack[64];
    USHORT frames = CaptureStackBackTrace(1, 64, stack, NULL);
    Log(FormatStackTraceWithSymbols(stack, frames));
    FlushLogs(); // Force flush after stack trace

    // Reset signal handler to default and raise SIGABRT to preserve original crash behavior
    // This ensures the exit code and crash dump generation remain unchanged
    signal(SIGABRT, SIG_DFL);
    raise(SIGABRT);

    // If raise returns (shouldn't happen), force terminate as fallback
    TerminateProcess(GetCurrentProcess(), 3);
}
*/

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

// ============================================================================
// GZIP LOG COMPRESSION
// In-process gzip writer with real DEFLATE compression.
// Uses a compact fixed-Huffman encoder + LZ77 matcher, no external tools.
// ============================================================================

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
    std::ifstream in(path, std::ios::binary | std::ios::ate);
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
    uint16_t code = 0; // bit-reversed for LSB-first bitstream writer
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
    w.WriteBits(1, 1);    // BFINAL
    w.WriteBits(0b01, 2); // BTYPE=01 (fixed)

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

    std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;

    // Gzip header (RFC 1952)
    const uint8_t hdr[10] = {
        0x1F, 0x8B,             // ID1, ID2
        0x08,                   // CM=deflate
        0x00,                   // FLG
        0x00, 0x00, 0x00, 0x00, // MTIME
        0x00,                   // XFL
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

// ASYNC LOGGING SYSTEM
// Uses a lock-free ring buffer for zero-contention log submission.
// A background thread writes to disk every 500ms.
// FlushLogs() force-writes all pending messages (for crash/shutdown).

// Pre-formatted log entry with timestamp already applied
struct LogEntry {
    std::atomic<bool> ready{ false }; // True when data is fully written and can be read
    std::string formattedMessage;     // "[HH:MM:SS.mmm] message"
};

// Lock-free ring buffer for log entries
// Uses two-phase commit: claim slot with CAS, mark ready after write
static constexpr size_t LOG_BUFFER_SIZE = 8192; // Power of 2 for fast modulo
static LogEntry g_logBuffer[LOG_BUFFER_SIZE];
static std::atomic<size_t> g_logClaimIndex{ 0 }; // Next position to claim (writers)
static std::atomic<size_t> g_logReadIndex{ 0 };  // Next position to read (reader)

// Background writer thread
static std::thread g_logThread;
static std::atomic<bool> g_logThreadRunning{ false };

// Forward declaration
static void LogThreadMain();
static void WriteLogsToFile();

void StartLogThread() {
    if (g_logThreadRunning.load()) return;
    g_logThreadRunning.store(true);
    g_logThread = std::thread(LogThreadMain);
}

void StopLogThread() {
    if (!g_logThreadRunning.load()) return;
    g_logThreadRunning.store(false);
    if (g_logThread.joinable()) { g_logThread.join(); }
    // Final flush after thread stops
    FlushLogs();
}

static void LogThreadMain() {
    while (g_logThreadRunning.load()) {
        WriteLogsToFile();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

// Internal: Write all pending log entries to file (called by background thread or FlushLogs)
static void WriteLogsToFile() {
    size_t readPos = g_logReadIndex.load(std::memory_order_relaxed);
    size_t claimPos = g_logClaimIndex.load(std::memory_order_acquire);

    if (readPos == claimPos) return; // Nothing claimed yet

    // Lock only during actual file I/O (not during Log() calls)
    std::lock_guard<std::mutex> lock(g_logFileMutex);
    if (!logFile.is_open()) return;

    // Process all ready entries in order
    // Note: entries might be claimed but not yet ready if writer is mid-write
    while (readPos != claimPos) {
        LogEntry& entry = g_logBuffer[readPos % LOG_BUFFER_SIZE];

        // Wait for this slot to be ready (writer finished)
        // Use relaxed load in tight loop, acquire synchronizes with writer's release
        if (!entry.ready.load(std::memory_order_acquire)) {
            // Entry not ready yet - stop here, will continue next flush
            // This handles out-of-order completion
            break;
        }

        logFile << entry.formattedMessage << std::endl;

        // Clear ready flag for next use of this slot
        entry.ready.store(false, std::memory_order_relaxed);

        readPos = (readPos + 1) % LOG_BUFFER_SIZE;
    }

    logFile.flush();
    g_logReadIndex.store(readPos, std::memory_order_release);
}

// Force flush all pending logs - call during crash/shutdown
void FlushLogs() { WriteLogsToFile(); }

// Category-based logging - only logs if category is enabled in debug config
void LogCategory(const char* category, const std::string& message) {
    // Check if category is enabled
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

    if (!enabled) return;
    Log(message); // Use standard Log for actual output
}

// True lock-free log submission using two-phase commit:
// 1. Atomically claim a slot with CAS on g_logClaimIndex
// 2. Write data to the claimed slot
// 3. Mark slot as ready (signals reader that data is complete)
void Log(const std::string& message) {
    // Format with timestamp before entering critical path
    std::string formatted = "[" + GetTimestamp() + "] " + message;

    // Atomically claim a slot using CAS loop
    size_t claimPos, nextClaimPos;
    do {
        claimPos = g_logClaimIndex.load(std::memory_order_relaxed);
        nextClaimPos = (claimPos + 1) % LOG_BUFFER_SIZE;

        // Check if buffer is full (would overwrite unread data)
        if (nextClaimPos == g_logReadIndex.load(std::memory_order_acquire)) {
            // Buffer full - drop this message (better than blocking)
            return;
        }
        // Try to atomically claim this slot by advancing claimIndex
    } while (!g_logClaimIndex.compare_exchange_weak(claimPos, nextClaimPos, std::memory_order_acq_rel, std::memory_order_relaxed));

    // We successfully claimed slot 'claimPos' - write data
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

void LogException(const std::string& context, const std::exception& e) {
    std::stringstream ss;
    ss << "EXCEPTION in " << context << ": " << e.what();
    Log(ss.str());
}

void LogException(const std::string& context, DWORD exceptionCode, EXCEPTION_POINTERS* exceptionInfo) {
    std::stringstream ss;
    ss << "STRUCTURED EXCEPTION in " << context << ": Code=0x" << std::hex << exceptionCode;

    if (exceptionInfo && exceptionInfo->ExceptionRecord) {
        ss << " Address=0x" << std::hex << reinterpret_cast<uintptr_t>(exceptionInfo->ExceptionRecord->ExceptionAddress);
    }

    Log(ss.str());

    // Try to get a simple stack trace
    if (exceptionInfo) {
        void* stack[32];
        USHORT frames = CaptureStackBackTrace(0, 32, stack, NULL);
        Log(FormatStackTraceWithSymbols(stack, frames));
    }

    // Force flush after exception logging
    FlushLogs();
}

LONG WINAPI CustomUnhandledExceptionFilter(EXCEPTION_POINTERS* exceptionInfo) {
    // Force flush any pending logs first
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

        // Decode common exception codes
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

    // Force immediate flush to ensure log is written before crash
    FlushLogs();

    // Also write to stderr for immediate visibility
    std::cerr << "[Toolscreen] EXCEPTION LOGGED - Check log file" << std::endl;
    std::cerr.flush();

    // Return EXCEPTION_CONTINUE_SEARCH to let Java's exception handler process it
    return EXCEPTION_CONTINUE_SEARCH;
}

void SEHTranslator(unsigned int code, EXCEPTION_POINTERS* info) {
    LogException("SEH_Translator", code, info);
    throw SE_Exception(code, info);
}

void EnsureSymbolsInitialized() {
    std::lock_guard<std::mutex> lock(g_symbolMutex);

    if (g_symbolsInitialized.load()) { return; }

    // Check if PDB file exists before doing any symbol initialization work
    const char* pdbPath = "C:\\Toolscreen.pdb";
    if (GetFileAttributesA(pdbPath) == INVALID_FILE_ATTRIBUTES) {
        Log("WARNING: PDB file not found at C:\\Toolscreen.pdb - symbols will not be available");
        return;
    }

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
    }
}

void InstallGlobalExceptionHandlers() {
    // Initialize symbol resolution first
    EnsureSymbolsInitialized();

    // Install signal handler for SIGABRT
    signal(SIGABRT, SignalHandler);

    // Install unhandled exception filter
    SetUnhandledExceptionFilter(CustomUnhandledExceptionFilter);

    // Install SEH to C++ exception translator
    _set_se_translator(SEHTranslator);

    Log("Global exception handlers installed (SEH + SIGABRT + Symbols)");
}

std::wstring GetToolscreenPath() {
    // First, check if a "toolscreen" folder exists in the current working directory
    WCHAR currentDir[MAX_PATH];
    if (GetCurrentDirectoryW(MAX_PATH, currentDir)) {
        std::wstring localPath = std::wstring(currentDir) + L"\\toolscreen";
        DWORD attrs = GetFileAttributesW(localPath.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) { return localPath; }
    }

    // Fall back to %USERPROFILE%\.config\toolscreen
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

    // Copy the mode ID and file path for the async operation
    std::string modeIdCopy = modeId;
    std::wstring filePathCopy = g_modeFilePath;

    // Fire-and-forget async write - never blocks the calling thread
    // NOTE: Do NOT use PROFILE_SCOPE inside short-lived detached threads!
    // The thread-local profiler buffer gets destroyed when the thread exits,
    // while the pointer remains in the registry, causing access violations.
    std::thread([modeIdCopy, filePathCopy]() {
        std::ofstream modeFile(filePathCopy, std::ios_base::out | std::ios_base::trunc);
        if (modeFile.is_open()) {
            modeFile << modeIdCopy;
            modeFile.close();
        }
        // Don't log on failure - this is fire-and-forget
    }).detach();
}

bool SwitchToMode(const std::string& newModeId, const std::string& source, bool forceCut) {
    PROFILE_SCOPE_CAT("Mode Switch", "Mode Management");

    LogCategory("mode_switch", "[MODE_SWITCH] Entry: Attempting to switch to '" + newModeId + "' from source: " + source);

    if (newModeId.empty()) {
        Log("ERROR: Attempted to switch to empty mode ID");
        return false;
    }

    // Clear temporary sensitivity override on any mode change
    ClearTempSensitivityOverride();

    // Check if resolution changing is supported for this game version
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
    // Get current mode - keep lock minimal, no I/O inside
    {
        std::lock_guard<std::mutex> lock(g_modeIdMutex);
        LogCategory("mode_switch", "[MODE_SWITCH] g_modeIdMutex acquired");
        currentMode = g_currentModeId;

        // Don't switch if we're already in the target mode
        if (EqualsIgnoreCase(currentMode, newModeId)) {
            Log("Mode switch to '" + newModeId + "' requested, but already in that mode.");
            return false; // No change needed
        }

        g_currentModeId = newModeId;
        // Update lock-free double-buffer for input handlers
        int nextIndex = 1 - g_currentModeIdIndex.load(std::memory_order_relaxed);
        g_modeIdBuffers[nextIndex] = newModeId;
        g_currentModeIdIndex.store(nextIndex, std::memory_order_release);
        LogCategory("mode_switch", "[MODE_SWITCH] g_currentModeId updated to: " + newModeId);
    }
    LogCategory("mode_switch", "[MODE_SWITCH] g_modeIdMutex released");

    // Async file write OUTSIDE the mutex - never blocks
    WriteCurrentModeToFile(newModeId);

    std::string logMessage = "[MODE] Switching from '" + currentMode + "' to '" + newModeId + "'";
    if (!source.empty()) { logMessage += " (source: " + source + ")"; }
    LogCategory("mode_switch", logMessage);

    // Read mode configurations to get dimensions/positions
    int fromWidth = 0, fromHeight = 0, fromX = 0, fromY = 0;
    int toWidth = 0, toHeight = 0, toX = 0, toY = 0;
    const int fullW = GetCachedScreenWidth();
    const int fullH = GetCachedScreenHeight();

    // Variable to store target mode config (copied to avoid holding lock)
    ModeConfig toModeCopy;

    // Check if a transition is already in progress - if so, use the current animated position
    // as the "from" position so the animation smoothly reverses from where it currently is
    bool useAnimatedPosition = false;
    float distanceRatio = 1.0f; // Ratio of new distance to original full distance (for duration scaling)
    {
        std::lock_guard<std::mutex> transitionLock(g_modeTransitionMutex);
        if (g_modeTransition.active && g_modeTransition.gameTransition == GameTransitionType::Bounce) {
            // Capture current animated position
            fromWidth = g_modeTransition.currentWidth;
            fromHeight = g_modeTransition.currentHeight;
            fromX = g_modeTransition.currentX;
            fromY = g_modeTransition.currentY;
            useAnimatedPosition = true;

            // Calculate distance ratio for duration scaling
            // Compare current-to-new-target distance vs original full distance
            int origDeltaW = std::abs(g_modeTransition.toWidth - g_modeTransition.fromWidth);
            int origDeltaH = std::abs(g_modeTransition.toHeight - g_modeTransition.fromHeight);
            int origDeltaX = std::abs(g_modeTransition.toX - g_modeTransition.fromX);
            int origDeltaY = std::abs(g_modeTransition.toY - g_modeTransition.fromY);
            float origDistance =
                std::sqrt((float)(origDeltaW * origDeltaW + origDeltaH * origDeltaH + origDeltaX * origDeltaX + origDeltaY * origDeltaY));

            // We'll calculate new distance after we know the new target (toWidth/toHeight/toX/toY)
            // Store original distance for now
            if (origDistance > 0) {
                // Store for later calculation after toMode is read
                // We need to defer this calculation, so we'll just mark that we need to scale
            }

            LogCategory("mode_switch",
                        "[MODE_SWITCH] Active transition detected - using current animated position: " + std::to_string(fromWidth) + "x" +
                            std::to_string(fromHeight) + " at " + std::to_string(fromX) + "," + std::to_string(fromY));
        }
    }

    {
        // Use config snapshot for thread-safe mode lookup (SwitchToMode is called from multiple threads)
        auto modeSnap = GetConfigSnapshot();
        const ModeConfig* fromMode = modeSnap ? GetModeFromSnapshot(*modeSnap, currentMode) : nullptr;
        const ModeConfig* toMode = modeSnap ? GetModeFromSnapshot(*modeSnap, newModeId) : nullptr;

        // Only calculate from dimensions from mode config if we're not using animated position
        if (!useAnimatedPosition) {
            if (fromMode) {
                if (fromMode->stretch.enabled) {
                    fromWidth = fromMode->stretch.width;
                    fromHeight = fromMode->stretch.height;
                    fromX = fromMode->stretch.x;
                    fromY = fromMode->stretch.y;
                } else {
                    fromWidth = fromMode->width;
                    fromHeight = fromMode->height;
                    fromX = (fullW - fromWidth) / 2;
                    fromY = (fullH - fromHeight) / 2;
                }
            } else {
                // Fallback to fullscreen
                fromWidth = fullW;
                fromHeight = fullH;
                fromX = 0;
                fromY = 0;
            }
        }

        if (toMode) {
            if (toMode->stretch.enabled) {
                toWidth = toMode->stretch.width;
                toHeight = toMode->stretch.height;
                toX = toMode->stretch.x;
                toY = toMode->stretch.y;
            } else {
                toWidth = toMode->width;
                toHeight = toMode->height;
                toX = (fullW - toWidth) / 2;
                toY = (fullH - toHeight) / 2;
            }

            // Copy the target mode for use
            toModeCopy = *toMode;
        } else {
            // Fallback to fullscreen
            toWidth = fullW;
            toHeight = fullH;
            toX = 0;
            toY = 0;
            // Create a default fullscreen mode config
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

    // If we're reversing mid-animation, scale the duration based on distance ratio
    if (useAnimatedPosition && toModeCopy.gameTransition == GameTransitionType::Bounce) {
        // Calculate the new distance (from current animated position to new target)
        int newDeltaW = std::abs(toWidth - fromWidth);
        int newDeltaH = std::abs(toHeight - fromHeight);
        int newDeltaX = std::abs(toX - fromX);
        int newDeltaY = std::abs(toY - fromY);
        float newDistance =
            std::sqrt((float)(newDeltaW * newDeltaW + newDeltaH * newDeltaH + newDeltaX * newDeltaX + newDeltaY * newDeltaY));

        // Get the full distance for the target mode (what it would be from static mode positions)
        // Use the new target mode's static dimensions as reference for "full" distance
        // Use snapshot for thread-safe lookup (reuse modeSnap if still valid, else re-acquire)
        auto distSnap = GetConfigSnapshot();
        const ModeConfig* targetMode = distSnap ? GetModeFromSnapshot(*distSnap, newModeId) : nullptr;
        if (targetMode) {
            // Calculate what the full distance would be (fullscreen to target, or vice versa)
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
                // Clamp to reasonable range (minimum 10% of normal duration)
                distanceRatio = (std::max)(0.1f, (std::min)(1.0f, distanceRatio));

                int originalDuration = toModeCopy.transitionDurationMs;
                toModeCopy.transitionDurationMs = static_cast<int>(originalDuration * distanceRatio);

                LogCategory("mode_switch",
                            "[MODE_SWITCH] Mid-animation reversal: scaling duration from " + std::to_string(originalDuration) + "ms to " +
                                std::to_string(toModeCopy.transitionDurationMs) + "ms (ratio: " + std::to_string(distanceRatio) + ")");
            }
        }
    }

    // Apply forceCut override — forces all transitions to Cut (instant) without mutating g_config
    if (forceCut) {
        toModeCopy.gameTransition = GameTransitionType::Cut;
        toModeCopy.overlayTransition = OverlayTransitionType::Cut;
        toModeCopy.backgroundTransition = BackgroundTransitionType::Cut;
    }

    // Start animated transition (handles size interpolation and WM_SIZE messages)
    LogCategory("mode_switch",
                "[MODE_SWITCH] Calling StartModeTransition with Game:" + GameTransitionTypeToString(toModeCopy.gameTransition) +
                    ", Overlay:" + OverlayTransitionTypeToString(toModeCopy.overlayTransition) +
                    ", Bg:" + BackgroundTransitionTypeToString(toModeCopy.backgroundTransition));
    StartModeTransition(currentMode, newModeId, fromWidth, fromHeight, fromX, fromY, toWidth, toHeight, toX, toY, toModeCopy);
    LogCategory("mode_switch", "[MODE_SWITCH] StartModeTransition completed");

    return true; // Mode was changed
}

bool IsFullscreen() {
    HWND hwnd = g_minecraftHwnd.load();
    if (!hwnd) { return false; }

    RECT r{};
    if (!GetWindowRect(hwnd, &r)) { return false; }

    RECT monRect{};
    if (!GetMonitorRectForWindow(hwnd, monRect)) {
        // Fallback to legacy primary-monitor heuristic.
        return r.left == 0 && r.top == 0 && r.right == GetCachedScreenWidth() && r.bottom == GetCachedScreenHeight();
    }

    // Borderless windows can be off by a pixel due to DPI rounding or driver quirks.
    const int tol = 1;
    const bool leftOk = std::abs(r.left - monRect.left) <= tol;
    const bool topOk = std::abs(r.top - monRect.top) <= tol;
    const bool rightOk = std::abs(r.right - monRect.right) <= tol;
    const bool bottomOk = std::abs(r.bottom - monRect.bottom) <= tol;
    return leftOk && topOk && rightOk && bottomOk;
}

bool GetMonitorRectForWindow(HWND hwnd, RECT& outRect) {
    if (!hwnd) { return false; }
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
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
    return EqualsIgnoreCase(id, "Fullscreen") || EqualsIgnoreCase(id, "EyeZoom") || EqualsIgnoreCase(id, "Thin") ||
           EqualsIgnoreCase(id, "Wide");
}

bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) { return false; }

    return std::equal(a.begin(), a.end(), b.begin(), [](char a, char b) { return std::tolower(a) == std::tolower(b); });
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

// Snapshot-safe overloads: look up in a specific config snapshot instead of g_config
const ModeConfig* GetModeFromSnapshot(const Config& config, const std::string& id) {
    for (const auto& mode : config.modes) {
        if (EqualsIgnoreCase(mode.id, id)) return &mode;
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
    const ModeConfig* mode = vpSnap ? GetModeFromSnapshot(*vpSnap, modeId) : nullptr;
    if (!mode) {
        return info; // valid = false
    }

    info.valid = true;
    info.x = 0;
    info.y = 0;
    info.width = mode->width;
    info.height = mode->height;

    int screenW = GetCachedScreenWidth();
    int screenH = GetCachedScreenHeight();

    info.stretchEnabled = mode->stretch.enabled;
    if (mode->stretch.enabled) {
        info.stretchX = mode->stretch.x;
        info.stretchY = mode->stretch.y;
        info.stretchWidth = mode->stretch.width;
        info.stretchHeight = mode->stretch.height;
    } else {
        info.stretchX = screenW / 2 - mode->width / 2;
        info.stretchY = screenH / 2 - mode->height / 2;
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

    std::thread([type, id, path, toolscreenPath]() {
        _set_se_translator(SEHTranslator);

        try {
            Log("Started thread for loading image '" + id + "' from path '" + path + "'");
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

                // Check if file is a GIF by extension (case-insensitive)
                bool isGif = false;
                if (path.size() >= 4) {
                    std::string ext = path.substr(path.size() - 4);
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    isGif = (ext == ".gif");
                }

                int w, h, c;
                unsigned char* data = nullptr;
                int frameCount = 0;
                int* delays = nullptr;

                if (isGif) {
                    // Read file into memory for GIF loading
                    FILE* f = nullptr;
                    errno_t err = fopen_s(&f, path_utf8.c_str(), "rb");
                    if (err == 0 && f) {
                        fseek(f, 0, SEEK_END);
                        long fileSize = ftell(f);
                        fseek(f, 0, SEEK_SET);

                        std::vector<unsigned char> fileData(fileSize);
                        size_t bytesRead = fread(fileData.data(), 1, fileSize, f);
                        fclose(f);

                        if (bytesRead == static_cast<size_t>(fileSize)) {
                            // Try loading as animated GIF
                            data = stbi_load_gif_from_memory(fileData.data(), (int)fileSize, &delays, &w, &h, &frameCount, &c, 4);

                            if (data && frameCount <= 1) {
                                // Single-frame GIF, treat as static image
                                frameCount = 1;
                                stbi_image_free(delays);
                                delays = nullptr;
                            }
                        }
                    }

                    // Fall back to regular load if GIF loading failed
                    if (!data) {
                        frameCount = 0;
                        data = stbi_load(path_utf8.c_str(), &w, &h, &c, 4);
                    }
                } else {
                    // Non-GIF: use regular stbi_load
                    data = stbi_load(path_utf8.c_str(), &w, &h, &c, 4);
                }

                if (g_isShuttingDown.load()) {
                    if (data) stbi_image_free(data);
                    if (delays) stbi_image_free(delays);
                    return;
                }

                if (data && w > 0 && h > 0) {
                    DecodedImageData decoded;
                    decoded.type = type;
                    decoded.id = id;
                    decoded.width = w;
                    decoded.channels = 4;
                    decoded.data = data;

                    // Set animation data
                    if (frameCount > 1) {
                        decoded.isAnimated = true;
                        decoded.frameCount = frameCount;
                        decoded.height = h * frameCount; // stbi_load_gif returns stacked frames
                        decoded.frameHeight = h;
                        for (int i = 0; i < frameCount; i++) {
                            // stb_image already converts GIF delays to milliseconds internally
                            // (see stb_image.h line 6916: "delay - 1/100th of a second, saving as 1/1000ths")
                            int delayMs = (delays && delays[i] > 0) ? delays[i] : 100;
                            decoded.frameDelays.push_back(delayMs);
                        }
                        Log("Loaded animated GIF '" + id + "' with " + std::to_string(frameCount) +
                            " frames, frame size: " + std::to_string(w) + "x" + std::to_string(h));
                    } else {
                        decoded.isAnimated = false;
                        decoded.frameCount = 1;
                        decoded.height = h;
                        decoded.frameHeight = h;
                    }

                    if (delays) stbi_image_free(delays);

                    std::lock_guard<std::mutex> lock(g_decodedImagesMutex);
                    g_decodedImagesQueue.push_back(decoded);
                    Log("Successfully decoded image for '" + id + "' from '" + path + "' on background thread.");
                } else {
                    Log("ERROR: Failed to decode image '" + path + "' for ID '" + id +
                        "'. Reason: " + (stbi_failure_reason() ? stbi_failure_reason() : "unknown error"));
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
        Log("Image load thread for '" + id + "' has completed.");
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
}

DWORD WINAPI FileMonitorThread(LPVOID lpParam) {
    _set_se_translator(SEHTranslator);

    try {
        Log("[FMON] FileMonitorThread started.");
        g_isStateOutputAvailable.store(false, std::memory_order_release);
        const std::vector<std::string> VALID_STATES = { "wall",
                                                        "inworld,cursor_free",
                                                        "inworld,cursor_grabbed",
                                                        "inworld,unpaused",
                                                        "inworld,paused",
                                                        "inworld,gamescreenopen",
                                                        "title",
                                                        "waiting" };

        HANDLE hFile = CreateFileW(g_stateFilePath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, NULL);

        if (hFile == INVALID_HANDLE_VALUE) {
            g_isStateOutputAvailable.store(false, std::memory_order_release);
            Log("[FMON] ERROR: Could not open state file on thread start. The file might not exist yet. Thread will now exit.");
            return 1;
        }

        g_isStateOutputAvailable.store(true, std::memory_order_release);

        // Pre-allocate buffer to avoid repeated allocations
        std::vector<char> buffer;
        buffer.reserve(128);

        while (!g_stopMonitoring) {
            Sleep(5);

            if (SetFilePointer(hFile, 0, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER) { continue; }

            DWORD fileSize = GetFileSize(hFile, NULL);
            if (fileSize > 0 && fileSize < 128) {
                buffer.resize(fileSize);
                DWORD bytesRead;
                if (ReadFile(hFile, buffer.data(), fileSize, &bytesRead, NULL) && bytesRead == fileSize) {
                    std::string content(buffer.data(), bytesRead);

                    // Optimize: Check prefix first before full vector search
                    bool isValid = (content.rfind("generating", 0) == 0) ||
                                   (std::find(VALID_STATES.begin(), VALID_STATES.end(), content) != VALID_STATES.end());

                    if (isValid) {
                        // Map old inworld substates to cursor-based states
                        if (content == "inworld,unpaused" || content == "inworld,paused" || content == "inworld,gamescreenopen") {
                            content = IsCursorVisible() ? "inworld,cursor_free" : "inworld,cursor_grabbed";
                        }

                        int currentIdx = g_currentGameStateIndex.load(std::memory_order_acquire);
                        if (g_gameStateBuffers[currentIdx] != content) {
                            int nextIdx = 1 - currentIdx;
                            g_gameStateBuffers[nextIdx] = content;
                            g_currentGameStateIndex.store(nextIdx, std::memory_order_release);
                        }
                    }
                }
            }
        }

        CloseHandle(hFile);
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
            Sleep(50);

            std::string localGameState = g_gameStateBuffers[g_currentGameStateIndex.load(std::memory_order_acquire)];

            std::vector<ImageConfig> imagesToCheck;
            imagesToCheck = g_config.images;

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

bool CheckHotkeyMatch(const std::vector<DWORD>& keys, WPARAM wParam, const std::vector<DWORD>& exclusionKeys, bool triggerOnRelease) {
    PROFILE_SCOPE_CAT("Hotkey Match Check", "Game Logic");
    if (keys.empty()) return false;

    // For trigger on release, skip exclusion key checks since user may have released modifiers
    // before or at the same time as the main key
    if (!triggerOnRelease) {
        // First check if any exclusion keys are pressed
        for (DWORD excluded_key : exclusionKeys) {
            if (GetAsyncKeyState(excluded_key) & 0x8000) {
                if (g_config.debug.showHotkeyDebug) { Log("[Hotkey] FAIL: Exclusion key " + std::to_string(excluded_key) + " is pressed"); }
                return false;
            }
        }
    }

    // The last key is always the "main" key that triggers the hotkey
    // Keys before it are treated as required modifiers
    DWORD main_key = keys.back();

    // Track specific modifier requirements (left/right variants)
    // Only look at keys BEFORE the main key
    bool requires_lctrl = false, requires_rctrl = false, requires_ctrl = false;
    bool requires_lshift = false, requires_rshift = false, requires_shift = false;
    bool requires_lalt = false, requires_ralt = false, requires_alt = false;

    // For trigger on release, we don't need to track modifier requirements since we skip the check
    // But we still need to parse the main key and match wParam
    if (!triggerOnRelease) {
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

    // Debug logging (can be toggled via config)
    bool s_enableHotkeyDebug = g_config.debug.showHotkeyDebug;
    std::string keyCombo; // Declare outside conditional for later use

    if (s_enableHotkeyDebug) {
        keyCombo = GetKeyComboString(keys);
        Log("[Hotkey] Check: " + keyCombo + " vs keypress " + std::to_string(wParam));
    }

    // Handle modifier keys specially - Windows sends VK_CONTROL/VK_SHIFT/VK_MENU in wParam,
    // not the specific left/right variants. We need to check if the specific key is pressed.
    bool main_key_pressed = (main_key == wParam);

    if (!main_key_pressed) {
        // Check if wParam is a generic modifier and main_key is a specific variant
        if (wParam == VK_CONTROL && (main_key == VK_LCONTROL || main_key == VK_RCONTROL)) {
            if (triggerOnRelease) {
                // For release triggers, we can't use GetAsyncKeyState (key is already released)
                // Just accept the match - the hotkey was configured for this specific key
                // Note: This means both left and right will trigger if either is released
                // To distinguish, we'd need to pass lParam and check the extended key flag
                main_key_pressed = true;
            } else {
                // Check if the specific control key is pressed
                main_key_pressed = (GetAsyncKeyState(main_key) & 0x8000) != 0;
            }
        } else if (wParam == VK_SHIFT && (main_key == VK_LSHIFT || main_key == VK_RSHIFT)) {
            if (triggerOnRelease) {
                main_key_pressed = true;
            } else {
                // Check if the specific shift key is pressed
                main_key_pressed = (GetAsyncKeyState(main_key) & 0x8000) != 0;
            }
        } else if (wParam == VK_MENU && (main_key == VK_LMENU || main_key == VK_RMENU)) {
            if (triggerOnRelease) {
                main_key_pressed = true;
            } else {
                // Check if the specific alt key is pressed
                main_key_pressed = (GetAsyncKeyState(main_key) & 0x8000) != 0;
            }
        }
    }

    if (!main_key_pressed) {
        if (s_enableHotkeyDebug) { Log("[Hotkey] SKIP: main key " + std::to_string(main_key) + " != " + std::to_string(wParam)); }
        return false;
    }

    // For trigger on release, skip modifier state checks since modifiers may have been
    // released before or at the same time as the main key
    if (!triggerOnRelease) {
        // Check specific modifier states
        bool lctrl_down = (GetAsyncKeyState(VK_LCONTROL) & 0x8000) != 0;
        bool rctrl_down = (GetAsyncKeyState(VK_RCONTROL) & 0x8000) != 0;
        bool lshift_down = (GetAsyncKeyState(VK_LSHIFT) & 0x8000) != 0;
        bool rshift_down = (GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0;
        bool lalt_down = (GetAsyncKeyState(VK_LMENU) & 0x8000) != 0;
        bool ralt_down = (GetAsyncKeyState(VK_RMENU) & 0x8000) != 0;

        // Generic modifier state (either left or right)
        bool ctrl_down_now = lctrl_down || rctrl_down;
        bool shift_down_now = lshift_down || rshift_down;
        bool alt_down_now = lalt_down || ralt_down;

        if (s_enableHotkeyDebug) {
            Log("[Hotkey] Modifiers - Need: LCtrl=" + std::to_string(requires_lctrl) + " RCtrl=" + std::to_string(requires_rctrl) +
                " Ctrl=" + std::to_string(requires_ctrl) + " LShift=" + std::to_string(requires_lshift) + " RShift=" +
                std::to_string(requires_rshift) + " Shift=" + std::to_string(requires_shift) + " LAlt=" + std::to_string(requires_lalt) +
                " RAlt=" + std::to_string(requires_ralt) + " Alt=" + std::to_string(requires_alt));
            Log("[Hotkey] Modifiers - Have: LCtrl=" + std::to_string(lctrl_down) + " RCtrl=" + std::to_string(rctrl_down) +
                " LShift=" + std::to_string(lshift_down) + " RShift=" + std::to_string(rshift_down) + " LAlt=" + std::to_string(lalt_down) +
                " RAlt=" + std::to_string(ralt_down));
        }

        // Check specific modifier requirements
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

        // Only check for unwanted modifiers if they are NOT in the exclusion list
        // This allows modifiers to be pressed without affecting the match, unless specifically excluded
        bool ctrl_in_exclusions = std::find_if(exclusionKeys.begin(), exclusionKeys.end(), [](DWORD k) {
                                      return k == VK_CONTROL || k == VK_LCONTROL || k == VK_RCONTROL;
                                  }) != exclusionKeys.end();
        bool shift_in_exclusions = std::find_if(exclusionKeys.begin(), exclusionKeys.end(), [](DWORD k) {
                                       return k == VK_SHIFT || k == VK_LSHIFT || k == VK_RSHIFT;
                                   }) != exclusionKeys.end();
        bool alt_in_exclusions = std::find_if(exclusionKeys.begin(), exclusionKeys.end(), [](DWORD k) {
                                     return k == VK_MENU || k == VK_LMENU || k == VK_RMENU;
                                 }) != exclusionKeys.end();

        // Determine if any ctrl/shift/alt is required (including specific variants)
        bool any_ctrl_required = requires_ctrl || requires_lctrl || requires_rctrl;
        bool any_shift_required = requires_shift || requires_lshift || requires_rshift;
        bool any_alt_required = requires_alt || requires_lalt || requires_ralt;

        if (!any_ctrl_required && ctrl_down_now && ctrl_in_exclusions) {
            if (s_enableHotkeyDebug) Log("[Hotkey] FAIL: Ctrl pressed but excluded");
            return false;
        }
        if (!any_shift_required && shift_down_now && shift_in_exclusions) {
            if (s_enableHotkeyDebug) Log("[Hotkey] FAIL: Shift pressed but excluded");
            return false;
        }
        if (!any_alt_required && alt_down_now && alt_in_exclusions) {
            if (s_enableHotkeyDebug) Log("[Hotkey] FAIL: Alt pressed but excluded");
            return false;
        }
    } else {
        if (s_enableHotkeyDebug) { Log("[Hotkey] Skipping modifier checks for trigger-on-release hotkey"); }
    }

    if (s_enableHotkeyDebug) {
        if (keyCombo.empty()) keyCombo = GetKeyComboString(keys);
        Log("[Hotkey] ✓ MATCH: " + keyCombo);
    }

    return true;
}

void GetRelativeCoords(const std::string& type, int relX, int relY, int w, int h, int containerW, int containerH, int& outX, int& outY) {
    // Strip Viewport/Screen suffix if present to get base anchor name
    std::string anchor = type;
    if (anchor.length() > 8 && anchor.substr(anchor.length() - 8) == "Viewport") {
        anchor = anchor.substr(0, anchor.length() - 8);
    } else if (anchor.length() > 6 && anchor.substr(anchor.length() - 6) == "Screen") {
        anchor = anchor.substr(0, anchor.length() - 6);
    }

    // Optimize by checking first character to reduce string comparisons
    char firstChar = anchor.empty() ? '\0' : anchor[0];

    if (firstChar == 't') { // "topLeft" or "topRight"
        outY = relY;
        outX = (anchor == "topLeft") ? relX : containerW - w - relX;
    } else if (firstChar == 'c') { // "center"
        outX = (containerW - w) / 2 + relX;
        outY = (containerH - h) / 2 + relY;
    } else if (firstChar == 'p') { // "pieLeft" or "pieRight"
        const int PIE_Y_TOP = 220, PIE_X_LEFT = 92, PIE_X_RIGHT = 36;
        int base_x = (anchor == "pieLeft") ? containerW - PIE_X_LEFT : containerW - PIE_X_RIGHT;
        outX = base_x + relX;
        outY = containerH - PIE_Y_TOP + relY;
    } else { // "bottomLeft" or "bottomRight"
        outY = containerH - h - relY;
        outX = (anchor == "bottomRight") ? containerW - w - relX : relX;
    }
}

void GetRelativeCoordsForImage(const std::string& type, int relX, int relY, int w, int h, int containerW, int containerH, int& outX,
                               int& outY) {
    int anchor_x = 0, anchor_y = 0;
    // Optimize by checking first character to reduce string comparisons
    char firstChar = type.empty() ? '\0' : type[0];

    if (firstChar == 't') { // "topLeft" or "topRight"
        anchor_x = (type == "topLeft") ? 0 : containerW - w;
        anchor_y = 0;
    } else if (firstChar == 'c') { // "center"
        anchor_x = (containerW - w) / 2;
        anchor_y = (containerH - h) / 2;
    } else if (firstChar == 'b') { // "bottomLeft" or "bottomRight"
        anchor_x = (type == "bottomLeft") ? 0 : containerW - w;
        anchor_y = containerH - h;
    }

    outX = anchor_x + relX;
    outY = anchor_y + relY;
}

// Get relative coordinates with viewport-aware positioning
// For "Viewport" suffixed types: position relative to game viewport (animates with game)
// For "Screen" suffixed types: position relative to screen (does not animate)
void GetRelativeCoordsForImageWithViewport(const std::string& type, int relX, int relY, int w, int h, int gameX, int gameY, int gameW,
                                           int gameH, int fullW, int fullH, int& outX, int& outY) {
    // Check if this is a viewport-relative anchor (ends with "Viewport")
    if (type.length() > 8 && type.substr(type.length() - 8) == "Viewport") {
        // Strip the "Viewport" suffix to get the base anchor name
        std::string baseAnchor = type.substr(0, type.length() - 8);

        // Calculate position relative to game viewport
        int anchor_x = 0, anchor_y = 0;
        char firstChar = baseAnchor.empty() ? '\0' : baseAnchor[0];

        if (firstChar == 't') { // "topLeft" or "topRight"
            anchor_x = (baseAnchor == "topLeft") ? 0 : gameW - w;
            anchor_y = 0;
        } else if (firstChar == 'c') { // "center"
            anchor_x = (gameW - w) / 2;
            anchor_y = (gameH - h) / 2;
        } else if (firstChar == 'b') { // "bottomLeft" or "bottomRight"
            anchor_x = (baseAnchor == "bottomLeft") ? 0 : gameW - w;
            anchor_y = gameH - h;
        }

        // Position is relative to game viewport origin
        outX = gameX + anchor_x + relX;
        outY = gameY + anchor_y + relY;
    } else {
        // Screen-relative: strip "Screen" suffix if present and use screen dimensions
        std::string baseAnchor = type;
        if (type.length() > 6 && type.substr(type.length() - 6) == "Screen") { baseAnchor = type.substr(0, type.length() - 6); }
        GetRelativeCoordsForImage(baseAnchor, relX, relY, w, h, fullW, fullH, outX, outY);
    }
}

void CalculateFinalScreenPos(const MirrorConfig* conf, const MirrorInstance& inst, int gameW, int gameH, int finalX, int finalY, int finalW,
                             int finalH, int fullW, int fullH, int& outScreenX, int& outScreenY) {
    float scaleX = conf->output.separateScale ? conf->output.scaleX : conf->output.scale;
    float scaleY = conf->output.separateScale ? conf->output.scaleY : conf->output.scale;
    int outW = static_cast<int>(inst.fbo_w * scaleX);
    int outH = static_cast<int>(inst.fbo_h * scaleY);

    std::string anchor = conf->output.relativeTo;
    int offsetX = conf->output.x;
    int offsetY = conf->output.y;

    // Check if this is a Screen anchor (absolute screen positioning)
    if (anchor.length() > 6 && anchor.substr(anchor.length() - 6) == "Screen") {
        anchor = anchor.substr(0, anchor.length() - 6);
        int relative_x, relative_y;
        GetRelativeCoords(anchor, offsetX, offsetY, outW, outH, fullW, fullH, relative_x, relative_y);
        outScreenX = relative_x;
        outScreenY = relative_y;
        return;
    }

    // Check if this is a Viewport anchor - strip the suffix
    if (anchor.length() > 8 && anchor.substr(anchor.length() - 8) == "Viewport") { anchor = anchor.substr(0, anchor.length() - 8); }

    // Calculate position in GAME coordinates first, then scale to screen.
    // This mirrors how input regions work - they use GetRelativeCoords with gameW/gameH,
    // then scale the result: finalX + gameCoordX * xScale.
    float xScale = (gameW > 0) ? static_cast<float>(finalW) / gameW : 1.0f;
    float yScale = (gameH > 0) ? static_cast<float>(finalH) / gameH : 1.0f;

    // Output dimensions in game space (for calculating position relative to game edges)
    int outW_game = static_cast<int>(outW / xScale);
    int outH_game = static_cast<int>(outH / yScale);

    // Calculate position in game coordinates
    int gamePosX, gamePosY;
    char firstChar = anchor.empty() ? '\0' : anchor[0];

    if (firstChar == 't') { // topLeft or topRight
        gamePosY = offsetY;
        if (anchor == "topLeft") {
            gamePosX = offsetX;
        } else { // topRight
            gamePosX = gameW - offsetX - outW_game;
        }
    } else if (firstChar == 'c') { // center
        gamePosX = (gameW - outW_game) / 2 + offsetX;
        gamePosY = (gameH - outH_game) / 2 + offsetY;
    } else if (firstChar == 'p') { // pieLeft or pieRight
        const int PIE_Y_TOP = 220, PIE_X_LEFT = 92, PIE_X_RIGHT = 36;
        int pieXOffset = (anchor == "pieLeft") ? PIE_X_LEFT : PIE_X_RIGHT;
        gamePosX = gameW - pieXOffset + offsetX - outW_game;
        gamePosY = gameH - PIE_Y_TOP + offsetY - outH_game;
    } else { // bottomLeft or bottomRight
        gamePosY = gameH - offsetY - outH_game;
        if (anchor == "bottomRight") {
            gamePosX = gameW - offsetX - outW_game;
        } else { // bottomLeft
            gamePosX = offsetX;
        }
    }

    // Scale game coordinates to screen coordinates (same as input region approach)
    outScreenX = finalX + static_cast<int>(gamePosX * xScale);
    outScreenY = finalY + static_cast<int>(gamePosY * yScale);
}

void ScreenshotToClipboard(int width, int height) {
    PROFILE_SCOPE_CAT("Screenshot to Clipboard", "System");
    Log("Taking screenshot...");
    size_t bufferSize = static_cast<size_t>(width) * height * 4;
    std::vector<BYTE> pixels(bufferSize);

    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    for (size_t i = 0; i < bufferSize; i += 4) {
        std::swap(pixels[i + 0], pixels[i + 2]); // R <-> B
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

    // Check if config file exists
    if (GetFileAttributesW(configPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Log("Config file does not exist, skipping backup.");
        return;
    }

    // Create backup directory if it doesn't exist
    CreateDirectoryW(backupDir.c_str(), NULL);

    // Get current unix timestamp
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

    // Create backup filename with timestamp
    std::wstring backupFileName = backupDir + L"\\config_" + std::to_wstring(timestamp) + L".toml";

    // Copy the config file to backup location
    if (CopyFileW(configPath.c_str(), backupFileName.c_str(), FALSE)) {
        Log("Config backed up to: " + WideToUtf8(backupFileName));

        // Clean up old backups, keeping only the latest 50
        WIN32_FIND_DATAW findData;
        std::vector<std::pair<FILETIME, std::wstring>> backupFiles;

        std::wstring searchPattern = backupDir + L"\\config_*.toml";
        HANDLE hFind = FindFirstFileW(searchPattern.c_str(), &findData);

        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    std::wstring fileName = findData.cFileName;
                    // Verify it matches our backup filename pattern
                    if (fileName.find(L"config_") == 0 && fileName.find(L".toml") != std::wstring::npos) {
                        std::wstring fullPath = backupDir + L"\\" + fileName;
                        backupFiles.push_back(std::make_pair(findData.ftLastWriteTime, fullPath));
                    }
                }
            } while (FindNextFileW(hFind, &findData));
            FindClose(hFind);
        }

        // Sort by modification time (newest first)
        std::sort(backupFiles.begin(), backupFiles.end(),
                  [](const std::pair<FILETIME, std::wstring>& a, const std::pair<FILETIME, std::wstring>& b) {
                      return CompareFileTime(&a.first, &b.first) > 0;
                  });

        // Delete files beyond the 50 most recent
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