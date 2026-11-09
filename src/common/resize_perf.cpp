#include "resize_perf.h"

#include "common/utils.h"
#include "features/virtual_camera.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>

extern std::atomic<bool> g_graphicsHookDetected;

namespace ResizePerf {
namespace {

constexpr int64_t kWatchUs = 1000000;
constexpr int64_t kStallReportUs = 250000;
constexpr int64_t kMaxPlausibleStallUs = 5000000;
constexpr int kMaxReportsPerSession = 20;
constexpr double kMicrosecondsPerMillisecond = 1000.0;

std::atomic<int64_t> g_watchStartUs{ 0 };
std::atomic<int64_t> g_watchUntilUs{ 0 };
std::atomic<const char*> g_watchSource{ "" };
std::atomic<int> g_watchTriggerCount{ 0 };
constexpr const char* kFollowUpResizeSource = "resize";
std::atomic<int64_t> g_lastFrameEndUs{ 0 };
std::atomic<int> g_modalLoopDepth{ 0 };
int64_t g_maxFrameGapUs = 0;
int64_t g_gapWatchStartUs = 0;
int g_reportsWritten = 0;

std::mutex g_statsMutex;
int g_loadCount = 0;
int g_alreadyLoadedCount = 0;
int64_t g_loadTotalUs = 0;
int64_t g_slowestLoadUs = 0;
std::string g_slowestLoadName;
unsigned g_slowestMessage = 0;
int64_t g_slowestMessageUs = 0;

const char* MessageName(unsigned message) {
    switch (message) {
    case WM_SIZE: return "WM_SIZE";
    case WM_WINDOWPOSCHANGING: return "WM_WINDOWPOSCHANGING";
    case WM_WINDOWPOSCHANGED: return "WM_WINDOWPOSCHANGED";
    case WM_STYLECHANGED: return "WM_STYLECHANGED";
    case WM_DISPLAYCHANGE: return "WM_DISPLAYCHANGE";
    case WM_NCCALCSIZE: return "WM_NCCALCSIZE";
    case WM_KEYDOWN: return "WM_KEYDOWN";
    case WM_SYSKEYDOWN: return "WM_SYSKEYDOWN";
    }
    return nullptr;
}

double ToMs(int64_t microseconds) { return static_cast<double>(microseconds) / kMicrosecondsPerMillisecond; }

void ResetStats() {
    std::lock_guard<std::mutex> lock(g_statsMutex);
    g_loadCount = 0;
    g_alreadyLoadedCount = 0;
    g_loadTotalUs = 0;
    g_slowestLoadUs = 0;
    g_slowestLoadName.clear();
    g_slowestMessage = 0;
    g_slowestMessageUs = 0;
}

void EndWatch() {
    g_watchStartUs.store(0, std::memory_order_release);
    g_watchUntilUs.store(0, std::memory_order_release);
}

void WriteReport(const char* source) {
    std::string loadName;
    int loads = 0;
    int alreadyLoaded = 0;
    int64_t loadTotalUs = 0;
    int64_t slowestLoadUs = 0;
    unsigned message = 0;
    int64_t messageUs = 0;
    {
        std::lock_guard<std::mutex> lock(g_statsMutex);
        loadName = g_slowestLoadName;
        loads = g_loadCount;
        alreadyLoaded = g_alreadyLoadedCount;
        loadTotalUs = g_loadTotalUs;
        slowestLoadUs = g_slowestLoadUs;
        message = g_slowestMessage;
        messageUs = g_slowestMessageUs;
    }

    char messageText[16];
    const char* messageName = MessageName(message);
    if (!messageName) {
        std::snprintf(messageText, sizeof(messageText), "0x%04X", message);
        messageName = messageText;
    }

    char countText[16] = "";
    const int triggerCount = g_watchTriggerCount.load(std::memory_order_acquire);
    if (triggerCount > 1) { std::snprintf(countText, sizeof(countText), " x%d", triggerCount); }

    char line[256];
    std::snprintf(line, sizeof(line),
                  "[PERF] resize stall %.0fms after %s%s (obs=%d vcam=%d): LoadLibrary %d calls %.1fms (%d already loaded), slowest ",
                  ToMs(g_maxFrameGapUs), source, countText, g_graphicsHookDetected.load(std::memory_order_relaxed) ? 1 : 0,
                  IsVirtualCameraActive() ? 1 : 0, loads, ToMs(loadTotalUs), alreadyLoaded);
    char tail[96];
    std::snprintf(tail, sizeof(tail), " %.1fms; slowest window message %s %.1fms", ToMs(slowestLoadUs), messageName, ToMs(messageUs));
    Log(std::string(line) + (loadName.empty() ? std::string("-") : loadName) + tail);
}

}

int64_t NowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool Watching() {
    const int64_t until = g_watchUntilUs.load(std::memory_order_acquire);
    return until != 0 && NowUs() <= until;
}

void Trigger(const char* source) {
    if (g_lastFrameEndUs.load(std::memory_order_acquire) == 0 || g_modalLoopDepth.load(std::memory_order_acquire) > 0) { return; }
    const int64_t now = NowUs();
    if (g_watchStartUs.load(std::memory_order_acquire) == 0) {
        ResetStats();
        g_watchSource.store(source, std::memory_order_release);
        g_watchTriggerCount.store(1, std::memory_order_release);
        g_watchStartUs.store(now, std::memory_order_release);
    } else if (std::strcmp(source, kFollowUpResizeSource) != 0) {
        g_watchTriggerCount.fetch_add(1, std::memory_order_acq_rel);
    }
    g_watchUntilUs.store(now + kWatchUs, std::memory_order_release);
}

int64_t CurrentWatchStart() { return g_watchStartUs.load(std::memory_order_acquire); }

void EnterModalLoop() {
    g_modalLoopDepth.fetch_add(1, std::memory_order_acq_rel);
    EndWatch();
}

void ExitModalLoop() {
    int depth = g_modalLoopDepth.load(std::memory_order_acquire);
    while (depth > 0 && !g_modalLoopDepth.compare_exchange_weak(depth, depth - 1, std::memory_order_acq_rel)) {}
    EndWatch();
}

void RecordLoadLibrary(int64_t microseconds, bool alreadyLoaded, const std::string& name) {
    std::lock_guard<std::mutex> lock(g_statsMutex);
    ++g_loadCount;
    if (alreadyLoaded) { ++g_alreadyLoadedCount; }
    g_loadTotalUs += microseconds;
    if (microseconds > g_slowestLoadUs) {
        g_slowestLoadUs = microseconds;
        g_slowestLoadName = name;
    }
}

void RecordWindowMessage(unsigned message, int64_t microseconds, int64_t watchStart) {
    if (watchStart == 0 || watchStart != g_watchStartUs.load(std::memory_order_acquire)) { return; }
    std::lock_guard<std::mutex> lock(g_statsMutex);
    if (microseconds > g_slowestMessageUs) {
        g_slowestMessageUs = microseconds;
        g_slowestMessage = message;
    }
}

void OnFrameEnd() noexcept {
    try {
        const int64_t now = NowUs();
        const int64_t start = g_watchStartUs.load(std::memory_order_acquire);
        const int64_t lastFrameEnd = g_lastFrameEndUs.exchange(now, std::memory_order_acq_rel);
        if (start == 0) { return; }
        if (start != g_gapWatchStartUs) {
            g_gapWatchStartUs = start;
            g_maxFrameGapUs = 0;
        }

        const int64_t gap = now - (std::max)(lastFrameEnd, start);
        if (gap > g_maxFrameGapUs) { g_maxFrameGapUs = gap; }
        if (now <= g_watchUntilUs.load(std::memory_order_acquire)) { return; }

        if (g_maxFrameGapUs >= kStallReportUs && g_maxFrameGapUs <= kMaxPlausibleStallUs) {
            if (g_reportsWritten < kMaxReportsPerSession) {
                WriteReport(g_watchSource.load(std::memory_order_acquire));
            } else if (g_reportsWritten == kMaxReportsPerSession) {
                Log("[PERF] further resize stall reports suppressed for this session");
            }
            ++g_reportsWritten;
        }
        EndWatch();
    } catch (...) {
        EndWatch();
    }
}

}
