#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// Lock-free hierarchical profiler using a single-producer queue per thread
// Hot path (PROFILE_SCOPE) is completely lock-free - just writes to a ring buffer
// Background thread aggregates and processes timing data
class Profiler {
  public:
    class ScopedTimer;

    struct ProfileEntry {
        std::string displayName;
        double totalTime = 0.0;
        double selfTime = 0.0;
        int callCount = 0;

        double accumulatedTime = 0.0;
        double accumulatedSelfTime = 0.0;
        int accumulatedCalls = 0;
        int frameCount = 0;
        double rollingAverageTime = 0.0;
        double rollingSelfTime = 0.0;
        double rollingAverageCalls = 0.0;

        double maxTimeInLastSecond = 0.0;

        std::chrono::steady_clock::time_point lastUpdateTime{};

        std::string parentPath;
        std::vector<std::string> childPaths;
        int depth = 0;

        double parentPercentage = 0.0;
        double totalPercentage = 0.0;
    };

    // Minimal timing event for lock-free submission
    struct TimingEvent {
        const char* sectionName;
        std::array<const char*, 24> scopeNames{};
        double durationMs;
        uint32_t threadId;       // Thread that generated this event
        uint8_t depth;
        uint8_t scopeDepth;
        bool isRenderThread;     // Whether from render thread
    };

    // Lock-free ring buffer for timing events (per-thread)
    static constexpr size_t RING_BUFFER_SIZE = 4096;

    struct ThreadRingBuffer {
        TimingEvent events[RING_BUFFER_SIZE];
        std::atomic<size_t> writeIndex{ 0 }; // Only written by owning thread
        std::atomic<size_t> readIndex{ 0 };  // Only written by processing thread
        std::atomic<bool> isValid{ true };   // Set to false when thread exits
        bool isRenderThread = false;
        uint32_t threadId = 0;

        // Scope stack for hierarchy tracking (thread-local, no sync needed)
        std::vector<const char*> scopeStack;
        std::vector<ScopedTimer*> activeTimers;
        int pauseDepth = 0;
        std::chrono::high_resolution_clock::time_point pauseStartTime{};
    };

    // RAII timing helper class - completely lock-free
    class ScopedTimer {
      public:
        ScopedTimer(Profiler& profiler, const char* sectionName);
        ~ScopedTimer();

        ScopedTimer(const ScopedTimer&) = delete;
        ScopedTimer& operator=(const ScopedTimer&) = delete;

      private:
        friend class Profiler;

        const char* m_sectionName;
        std::chrono::high_resolution_clock::time_point m_startTime;
        std::chrono::nanoseconds m_pausedTime{ 0 };
        uint8_t m_depth;
        bool m_active;
    };

    class ScopedPause {
      public:
        explicit ScopedPause(Profiler& profiler);
        ~ScopedPause();

        ScopedPause(const ScopedPause&) = delete;
        ScopedPause& operator=(const ScopedPause&) = delete;

      private:
        Profiler* m_profiler;
    };

    static Profiler& GetInstance();
    static ThreadRingBuffer& GetThreadBuffer();

    // Mark the current thread as the render thread
    void MarkAsRenderThread();

    // Lock-free event submission (called from ScopedTimer destructor)
    void SubmitEvent(const char* sectionName, double durationMs, uint8_t depth, ThreadRingBuffer& buffer);
    void SubmitExternalTiming(const char* sectionName, double durationMs);

    void EndFrame();

    // Start/stop background processing thread
    void StartProcessingThread();
    void StopProcessingThread();

    struct DisplayData {
        std::vector<std::pair<std::string, ProfileEntry>> renderThread;
        std::vector<std::pair<std::string, ProfileEntry>> otherThreads;
    };
    DisplayData GetProfileData() const;

    std::vector<std::pair<std::string, ProfileEntry>> GetProfileDataFlat() const;

    void Clear();
    void SetEnabled(bool enabled) { m_enabled = enabled; }
    bool IsEnabled() const { return m_enabled; }

    void RegisterThreadBuffer(ThreadRingBuffer* buffer);

  private:
    Profiler() = default;
    ~Profiler();

    std::atomic<bool> m_enabled{ false };
    std::atomic<bool> m_processingThreadRunning{ false };
    std::thread m_processingThread;

    // Processed data (only accessed by processing thread and display)
    std::unordered_map<std::string, ProfileEntry> m_renderThreadEntries;
    std::unordered_map<std::string, ProfileEntry> m_otherThreadEntries;

    double m_totalRenderTime = 0.0;
    double m_totalOtherTime = 0.0;
    double m_accumulatedRenderTime = 0.0;
    double m_accumulatedOtherTime = 0.0;
    int m_frameCountForAveraging = 0;
    static constexpr int MAX_FRAMES_FOR_AVERAGING = 360;

    // Display cache - protected by mutex for thread-safe access
    mutable std::mutex m_displayDataMutex;
    DisplayData m_cachedDisplayData;
    std::chrono::steady_clock::time_point m_lastUpdateTime;
    static constexpr int UPDATE_INTERVAL_MS = 1000;

    // Thread registry (lock-free via atomic flag)
    std::atomic_flag m_registryLock = ATOMIC_FLAG_INIT;
    std::vector<ThreadRingBuffer*> m_threadRegistry;

    void ProcessingThreadMain();
    void ProcessEvents();
    void CalculateHierarchy(std::unordered_map<std::string, ProfileEntry>& entries, double totalTime);
    void BuildDisplayTree(const std::unordered_map<std::string, ProfileEntry>& entries,
                          std::vector<std::pair<std::string, ProfileEntry>>& output);
    void PauseCurrentThread();
    void ResumeCurrentThread();
};

// Convenience macros - completely lock-free on hot path
#define PROFILE_SCOPE(name) Profiler::ScopedTimer _profiler_timer_##__LINE__(Profiler::GetInstance(), name)

#define PROFILE_SCOPE_CAT(name, category) PROFILE_SCOPE(name)

#define PROFILE_START(name) /* deprecated - use PROFILE_SCOPE */


