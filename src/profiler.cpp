#include "profiler.h"
#include "utils.h" // For Log()
#include <algorithm>
#include <functional>
#include <sstream>

Profiler& Profiler::GetInstance() {
    static Profiler instance;
    return instance;
}

Profiler::~Profiler() { StopProcessingThread(); }

// RAII guard to invalidate buffer when thread exits
struct ThreadBufferGuard {
    Profiler::ThreadRingBuffer* buffer;
    ~ThreadBufferGuard() {
        if (buffer) { buffer->isValid.store(false, std::memory_order_release); }
    }
};

Profiler::ThreadRingBuffer& Profiler::GetThreadBuffer() {
    thread_local ThreadRingBuffer tls_buffer;
    thread_local ThreadBufferGuard guard{ &tls_buffer }; // Will invalidate on thread exit
    thread_local bool registered = false;

    if (!registered) {
        tls_buffer.threadId = static_cast<uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
        GetInstance().RegisterThreadBuffer(&tls_buffer);
        registered = true;
    }

    return tls_buffer;
}

void Profiler::RegisterThreadBuffer(ThreadRingBuffer* buffer) {
    // Brief spin-lock for registration only (rare operation)
    while (m_registryLock.test_and_set(std::memory_order_acquire)) {}
    m_threadRegistry.push_back(buffer);
    m_registryLock.clear(std::memory_order_release);
}

void Profiler::MarkAsRenderThread() { GetThreadBuffer().isRenderThread = true; }

// ScopedTimer - completely lock-free
Profiler::ScopedTimer::ScopedTimer(Profiler& profiler, const char* sectionName) : m_sectionName(sectionName), m_depth(0), m_active(false) {
    if (profiler.IsEnabled()) {
        m_startTime = std::chrono::high_resolution_clock::now();

        // Track stack depth for hierarchy (thread-local, no sync)
        ThreadRingBuffer& buffer = GetThreadBuffer();
        m_depth = static_cast<uint8_t>(buffer.scopeStack.size());
        buffer.scopeStack.push_back(sectionName);

        m_active = true;
    }
}

Profiler::ScopedTimer::~ScopedTimer() {
    if (m_active) {
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration<double, std::milli>(endTime - m_startTime);
        double durationMs = duration.count();

        // Get parent name BEFORE popping (thread-local, no sync)
        ThreadRingBuffer& buffer = GetThreadBuffer();
        const char* parentName = nullptr;
        if (buffer.scopeStack.size() > 1) {
            // Parent is second-to-last in the stack (current scope is last)
            parentName = buffer.scopeStack[buffer.scopeStack.size() - 2];
        }

        // Pop scope stack
        if (!buffer.scopeStack.empty()) { buffer.scopeStack.pop_back(); }

        // Submit event with parent info - completely lock-free
        Profiler::GetInstance().SubmitEvent(m_sectionName, parentName, durationMs, m_depth);
    }
}

// Lock-free event submission - O(1), no locks, no allocations
void Profiler::SubmitEvent(const char* sectionName, const char* parentName, double durationMs, uint8_t depth) {
    if (!m_enabled) return;

    ThreadRingBuffer& buffer = GetThreadBuffer();

    // SLOW SCOPE DETECTION: Log any scope that takes more than 100ms
    constexpr double SLOW_THRESHOLD_MS = 100.0;
    if (durationMs > SLOW_THRESHOLD_MS) {
        // Build path from current scope stack for better context
        std::string pathStr = sectionName;
        Log("[SLOW PROFILER] " + pathStr + " took " + std::to_string(durationMs) + "ms (>" +
            std::to_string(static_cast<int>(SLOW_THRESHOLD_MS)) + "ms threshold)");
    }

    // Get write position (only this thread writes to writeIndex)
    size_t writePos = buffer.writeIndex.load(std::memory_order_relaxed);
    size_t nextWritePos = (writePos + 1) % RING_BUFFER_SIZE;

    // Check if buffer is full (would overwrite unread data)
    if (nextWritePos == buffer.readIndex.load(std::memory_order_acquire)) {
        // Buffer full - drop this event (better than blocking)
        return;
    }

    // Write event data
    TimingEvent& event = buffer.events[writePos];
    event.sectionName = sectionName;
    event.parentName = parentName;
    event.durationMs = durationMs;
    event.threadId = buffer.threadId;
    event.depth = depth;
    event.isRenderThread = buffer.isRenderThread;

    // Publish the write (release semantics ensure event data is visible)
    buffer.writeIndex.store(nextWritePos, std::memory_order_release);
}

void Profiler::StartProcessingThread() {
    if (m_processingThreadRunning.load()) return;

    m_processingThreadRunning.store(true);
    m_processingThread = std::thread(&Profiler::ProcessingThreadMain, this);
}

void Profiler::StopProcessingThread() {
    if (!m_processingThreadRunning.load()) return;

    m_processingThreadRunning.store(false);
    if (m_processingThread.joinable()) { m_processingThread.join(); }
}

void Profiler::ProcessingThreadMain() {
    while (m_processingThreadRunning.load()) {
        ProcessEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60Hz processing
    }
}

void Profiler::ProcessEvents() {
    // Process events from all registered thread buffers
    while (m_registryLock.test_and_set(std::memory_order_acquire)) {}
    std::vector<ThreadRingBuffer*> buffers = m_threadRegistry; // Copy to release lock quickly
    m_registryLock.clear(std::memory_order_release);

    for (ThreadRingBuffer* buffer : buffers) {
        // Skip invalidated buffers (thread has exited)
        if (!buffer->isValid.load(std::memory_order_acquire)) { continue; }

        // Read all available events from this buffer
        size_t readPos = buffer->readIndex.load(std::memory_order_relaxed);
        size_t writePos = buffer->writeIndex.load(std::memory_order_acquire);

        while (readPos != writePos) {
            const TimingEvent& event = buffer->events[readPos];

            // Process this event into our aggregated data
            auto& targetEntries = event.isRenderThread ? m_renderThreadEntries : m_otherThreadEntries;

            // Use section name as key
            std::string pathKey = event.sectionName;

            auto& entry = targetEntries[pathKey];
            entry.displayName = event.sectionName;
            entry.totalTime += event.durationMs;
            entry.callCount++;
            entry.depth = event.depth;
            entry.lastUpdateTime = std::chrono::steady_clock::now();

            // Build parent-child relationships
            if (event.parentName != nullptr) {
                std::string parentKey = event.parentName;
                entry.parentPath = parentKey;

                // Add this as a child of the parent
                auto& parentEntry = targetEntries[parentKey];
                parentEntry.displayName = event.parentName;
                // Add to child paths if not already present
                bool found = false;
                for (const auto& child : parentEntry.childPaths) {
                    if (child == pathKey) {
                        found = true;
                        break;
                    }
                }
                if (!found) { parentEntry.childPaths.push_back(pathKey); }
            }

            // Track max time
            if (event.durationMs > entry.maxTimeInLastSecond) { entry.maxTimeInLastSecond = event.durationMs; }

            // Advance read position
            readPos = (readPos + 1) % RING_BUFFER_SIZE;
        }

        // Publish read progress
        buffer->readIndex.store(readPos, std::memory_order_release);
    }
}

void Profiler::CalculateHierarchy(std::unordered_map<std::string, ProfileEntry>& entries, double totalTime) {
    // Calculate self time (total time minus children's time)
    for (auto& [path, entry] : entries) {
        double childrenTime = 0.0;
        for (const auto& childPath : entry.childPaths) {
            auto it = entries.find(childPath);
            if (it != entries.end()) { childrenTime += it->second.totalTime; }
        }
        entry.selfTime = entry.totalTime - childrenTime;
        if (entry.selfTime < 0.0) entry.selfTime = 0.0; // Clamp to 0
    }

    // Calculate percentages
    for (auto& [path, entry] : entries) {
        entry.totalPercentage = totalTime > 0.0 ? (entry.totalTime / totalTime) * 100.0 : 0.0;

        // Parent percentage
        if (!entry.parentPath.empty()) {
            auto it = entries.find(entry.parentPath);
            if (it != entries.end() && it->second.totalTime > 0.0) {
                entry.parentPercentage = (entry.totalTime / it->second.totalTime) * 100.0;
            }
        } else {
            entry.parentPercentage = entry.totalPercentage;
        }
    }
}

void Profiler::BuildDisplayTree(const std::unordered_map<std::string, ProfileEntry>& entries,
                                std::vector<std::pair<std::string, ProfileEntry>>& output) {
    output.clear();

    // Build a map of parent -> children for quick lookup
    std::unordered_map<std::string, std::vector<std::string>> childrenMap;
    std::vector<std::string> rootEntries;

    for (const auto& [path, entry] : entries) {
        if (entry.parentPath.empty()) {
            rootEntries.push_back(path);
        } else {
            childrenMap[entry.parentPath].push_back(path);
        }
    }

    // Sort children by rolling average time (descending) within each parent
    auto sortByTime = [&entries](std::vector<std::string>& names) {
        std::sort(names.begin(), names.end(), [&entries](const std::string& a, const std::string& b) {
            auto itA = entries.find(a);
            auto itB = entries.find(b);
            double timeA = (itA != entries.end()) ? itA->second.rollingAverageTime : 0.0;
            double timeB = (itB != entries.end()) ? itB->second.rollingAverageTime : 0.0;
            return timeA > timeB;
        });
    };

    // Sort root entries by time
    sortByTime(rootEntries);

    // Sort all children groups by time
    for (auto& [parent, children] : childrenMap) { sortByTime(children); }

    // Recursive function to add entry and its children in order
    std::function<void(const std::string&)> addEntryWithChildren = [&](const std::string& path) {
        auto it = entries.find(path);
        if (it != entries.end()) {
            output.emplace_back(path, it->second);

            // Add all children recursively
            auto childIt = childrenMap.find(path);
            if (childIt != childrenMap.end()) {
                for (const auto& childPath : childIt->second) { addEntryWithChildren(childPath); }
            }
        }
    };

    // Start with root entries and recursively add children
    for (const auto& rootPath : rootEntries) { addEntryWithChildren(rootPath); }
}

void Profiler::EndFrame() {
    if (!m_enabled) return;

    auto currentTime = std::chrono::steady_clock::now();

    // Process any pending events
    ProcessEvents();

    // Calculate totals
    m_totalRenderTime = 0.0;
    m_totalOtherTime = 0.0;

    for (const auto& [path, entry] : m_renderThreadEntries) { m_totalRenderTime += entry.totalTime; }
    for (const auto& [path, entry] : m_otherThreadEntries) { m_totalOtherTime += entry.totalTime; }

    // Calculate hierarchy (self time, percentages)
    CalculateHierarchy(m_renderThreadEntries, m_totalRenderTime);
    CalculateHierarchy(m_otherThreadEntries, m_totalOtherTime);

    // Accumulate for rolling average
    m_accumulatedRenderTime += m_totalRenderTime;
    m_accumulatedOtherTime += m_totalOtherTime;
    m_frameCountForAveraging++;

    // Accumulate per-entry data
    auto accumulateEntries = [this](std::unordered_map<std::string, ProfileEntry>& entries) {
        for (auto& [path, entry] : entries) {
            entry.accumulatedTime += entry.totalTime;
            entry.accumulatedSelfTime += entry.selfTime;
            entry.accumulatedCalls += entry.callCount;
            entry.frameCount++;
        }
    };
    accumulateEntries(m_renderThreadEntries);
    accumulateEntries(m_otherThreadEntries);

    // Reset frame data for next frame
    for (auto& [path, entry] : m_renderThreadEntries) {
        entry.totalTime = 0.0;
        entry.selfTime = 0.0;
        entry.callCount = 0;
    }
    for (auto& [path, entry] : m_otherThreadEntries) {
        entry.totalTime = 0.0;
        entry.selfTime = 0.0;
        entry.callCount = 0;
    }

    // Remove stale entries that haven't been updated in 5 seconds
    constexpr auto STALE_THRESHOLD = std::chrono::seconds(5);
    auto removeStaleEntries = [&currentTime, STALE_THRESHOLD](std::unordered_map<std::string, ProfileEntry>& entries) {
        for (auto it = entries.begin(); it != entries.end();) {
            auto timeSinceUpdate = currentTime - it->second.lastUpdateTime;
            if (timeSinceUpdate > STALE_THRESHOLD) {
                it = entries.erase(it);
            } else {
                ++it;
            }
        }
    };
    removeStaleEntries(m_renderThreadEntries);
    removeStaleEntries(m_otherThreadEntries);

    // Update display cache
    auto timeSinceLastUpdate = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - m_lastUpdateTime);
    if (timeSinceLastUpdate.count() >= UPDATE_INTERVAL_MS) {
        double avgRenderTime = m_frameCountForAveraging > 0 ? m_accumulatedRenderTime / m_frameCountForAveraging : 0.0;
        double avgOtherTime = m_frameCountForAveraging > 0 ? m_accumulatedOtherTime / m_frameCountForAveraging : 0.0;

        auto updateRollingAverages = [this](std::unordered_map<std::string, ProfileEntry>& entries, double avgTotal) {
            for (auto& [path, entry] : entries) {
                if (entry.frameCount > 0) {
                    entry.rollingAverageTime = entry.accumulatedTime / entry.frameCount;
                    entry.rollingSelfTime = entry.accumulatedSelfTime / entry.frameCount;
                }
                entry.totalPercentage = avgTotal > 0.0 ? (entry.rollingAverageTime / avgTotal) * 100.0 : 0.0;
            }
        };
        updateRollingAverages(m_renderThreadEntries, avgRenderTime);
        updateRollingAverages(m_otherThreadEntries, avgOtherTime);

        // Lock mutex while updating display cache to prevent race with GetProfileData
        {
            std::lock_guard<std::mutex> lock(m_displayDataMutex);
            BuildDisplayTree(m_renderThreadEntries, m_cachedDisplayData.renderThread);
            BuildDisplayTree(m_otherThreadEntries, m_cachedDisplayData.otherThreads);
        }

        m_lastUpdateTime = currentTime;
    }
}

Profiler::DisplayData Profiler::GetProfileData() const {
    std::lock_guard<std::mutex> lock(m_displayDataMutex);
    return m_cachedDisplayData;
}

std::vector<std::pair<std::string, Profiler::ProfileEntry>> Profiler::GetProfileDataFlat() const {
    std::lock_guard<std::mutex> lock(m_displayDataMutex);
    std::vector<std::pair<std::string, ProfileEntry>> result;
    result.reserve(m_cachedDisplayData.renderThread.size() + m_cachedDisplayData.otherThreads.size());
    for (const auto& entry : m_cachedDisplayData.renderThread) result.push_back(entry);
    for (const auto& entry : m_cachedDisplayData.otherThreads) result.push_back(entry);
    return result;
}

void Profiler::Clear() {
    // Clear all thread buffers
    while (m_registryLock.test_and_set(std::memory_order_acquire)) {}

    for (ThreadRingBuffer* buffer : m_threadRegistry) {
        buffer->readIndex.store(0, std::memory_order_relaxed);
        buffer->writeIndex.store(0, std::memory_order_relaxed);
        buffer->scopeStack.clear();
    }

    m_registryLock.clear(std::memory_order_release);

    m_renderThreadEntries.clear();
    m_otherThreadEntries.clear();
    m_cachedDisplayData.renderThread.clear();
    m_cachedDisplayData.otherThreads.clear();
    m_totalRenderTime = 0.0;
    m_totalOtherTime = 0.0;
    m_accumulatedRenderTime = 0.0;
    m_accumulatedOtherTime = 0.0;
    m_frameCountForAveraging = 0;
}
