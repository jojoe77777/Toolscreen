#include "profiler.h"
#include "utils.h"
#include <algorithm>
#include <functional>
#include <sstream>

namespace {
constexpr char kProfilerPathSeparator = '\x1f';
constexpr double kProfilerUnspecifiedDisplayThresholdMs = 0.01;
constexpr char kProfilerUnspecifiedName[] = "Unspecified";

std::string BuildScopeKey(const Profiler::TimingEvent& event, uint8_t scopeDepth) {
    std::string key;
    key.reserve(scopeDepth * 24);

    for (uint8_t index = 0; index < scopeDepth; ++index) {
        const char* scopeName = event.scopeNames[index];
        if (scopeName == nullptr || scopeName[0] == '\0') { continue; }

        if (!key.empty()) { key.push_back(kProfilerPathSeparator); }
        key += scopeName;
    }

    if (key.empty() && event.sectionName != nullptr) { key = event.sectionName; }
    return key;
}

double SumRootScopeTimes(const std::unordered_map<std::string, Profiler::ProfileEntry>& entries) {
    double totalTime = 0.0;
    for (const auto& [path, entry] : entries) {
        if (entry.parentPath.empty()) { totalTime += entry.totalTime; }
    }
    return totalTime;
}

std::string BuildUnspecifiedDisplayKey(const std::string& parentPath) {
    std::string key = parentPath;
    if (!key.empty()) { key.push_back(kProfilerPathSeparator); }
    key += "<unspecified-self>";
    return key;
}

Profiler::ProfileEntry BuildUnspecifiedDisplayEntry(const std::string& parentPath, const Profiler::ProfileEntry& parentEntry) {
    Profiler::ProfileEntry entry;
    entry.displayName = kProfilerUnspecifiedName;
    entry.totalTime = parentEntry.rollingSelfTime;
    entry.selfTime = parentEntry.rollingSelfTime;
    entry.depth = parentEntry.depth + 1;
    entry.parentPath = parentPath;
    entry.rollingAverageTime = parentEntry.rollingSelfTime;
    entry.rollingSelfTime = parentEntry.rollingSelfTime;
    entry.rollingAverageCalls = parentEntry.rollingAverageCalls;

    if (parentEntry.rollingAverageTime > 0.0) {
        const double parentRatio = parentEntry.rollingSelfTime / parentEntry.rollingAverageTime;
        entry.parentPercentage = parentRatio * 100.0;
        entry.totalPercentage = parentEntry.totalPercentage * parentRatio;
    }

    return entry;
}
} // namespace

Profiler& Profiler::GetInstance() {
    static Profiler instance;
    return instance;
}

Profiler::~Profiler() { StopProcessingThread(); }

// RAII guard to invalidate buffer when thread exits
struct ThreadBufferGuard {
    std::shared_ptr<Profiler::ThreadRingBuffer> buffer;
    ~ThreadBufferGuard() {
        if (buffer) { buffer->isValid.store(false, std::memory_order_release); }
    }
};

Profiler::ThreadRingBuffer& Profiler::GetThreadBuffer() {
    thread_local auto tls_buffer = std::make_shared<ThreadRingBuffer>();
    thread_local ThreadBufferGuard guard{ tls_buffer }; // Will invalidate on thread exit
    thread_local bool registered = false;

    if (!registered) {
        tls_buffer->threadId = static_cast<uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
        GetInstance().RegisterThreadBuffer(tls_buffer);
        registered = true;
    }

    return *tls_buffer;
}

void Profiler::RegisterThreadBuffer(const std::shared_ptr<ThreadRingBuffer>& buffer) {
    // Brief spin-lock for registration only (rare operation)
    while (m_registryLock.test_and_set(std::memory_order_acquire)) {}
    m_threadRegistry.push_back(buffer);
    m_registryLock.clear(std::memory_order_release);
}

void Profiler::MarkAsRenderThread() { GetThreadBuffer().isRenderThread = true; }

Profiler::ScopedPause::ScopedPause(Profiler& profiler) : m_profiler(profiler.IsEnabled() ? &profiler : nullptr) {
    if (m_profiler != nullptr) { m_profiler->PauseCurrentThread(); }
}

Profiler::ScopedPause::~ScopedPause() {
    if (m_profiler != nullptr) { m_profiler->ResumeCurrentThread(); }
}

// ScopedTimer - completely lock-free
Profiler::ScopedTimer::ScopedTimer(Profiler& profiler, const char* sectionName) : m_sectionName(sectionName), m_depth(0), m_active(false) {
    if (profiler.IsEnabled()) {
        ThreadRingBuffer& buffer = GetThreadBuffer();
        if (buffer.pauseDepth > 0) { return; }

        m_startTime = std::chrono::high_resolution_clock::now();

        // Track stack depth for hierarchy (thread-local, no sync)
        m_depth = static_cast<uint8_t>(buffer.scopeStack.size());
        buffer.scopeStack.push_back(sectionName);
        buffer.activeTimers.push_back(this);

        m_active = true;
    }
}

Profiler::ScopedTimer::~ScopedTimer() {
    if (m_active) {
        auto endTime = std::chrono::high_resolution_clock::now();

        ThreadRingBuffer& buffer = GetThreadBuffer();

        if (!buffer.activeTimers.empty()) { buffer.activeTimers.pop_back(); }

        auto activeDuration = endTime - m_startTime - m_pausedTime;
        if (activeDuration < std::chrono::high_resolution_clock::duration::zero()) {
            activeDuration = std::chrono::high_resolution_clock::duration::zero();
        }
        double durationMs = std::chrono::duration<double, std::milli>(activeDuration).count();

        // Capture the full scope ancestry before popping the current scope.
        Profiler::GetInstance().SubmitEvent(m_sectionName, durationMs, m_depth, buffer);

        if (!buffer.scopeStack.empty()) { buffer.scopeStack.pop_back(); }
    }
}

void Profiler::PauseCurrentThread() {
    ThreadRingBuffer& buffer = GetThreadBuffer();
    if (buffer.pauseDepth++ == 0) { buffer.pauseStartTime = std::chrono::high_resolution_clock::now(); }
}

void Profiler::ResumeCurrentThread() {
    ThreadRingBuffer& buffer = GetThreadBuffer();
    if (buffer.pauseDepth == 0) { return; }

    buffer.pauseDepth--;
    if (buffer.pauseDepth > 0) { return; }

    const auto pausedDuration = std::chrono::high_resolution_clock::now() - buffer.pauseStartTime;
    for (ScopedTimer* timer : buffer.activeTimers) {
        if (timer != nullptr) { timer->m_pausedTime += pausedDuration; }
    }
}

// Lock-free event submission - O(1), no locks, no allocations
void Profiler::SubmitEvent(const char* sectionName, double durationMs, uint8_t depth, ThreadRingBuffer& buffer) {
    if (!m_enabled) return;

    constexpr double SLOW_THRESHOLD_MS = 100.0;
    if (durationMs > SLOW_THRESHOLD_MS) {
        std::string pathStr = sectionName;
        Log("[SLOW PROFILER] " + pathStr + " took " + std::to_string(durationMs) + "ms (>" +
            std::to_string(static_cast<int>(SLOW_THRESHOLD_MS)) + "ms threshold)");
    }

    // Get write position (only this thread writes to writeIndex)
    size_t writePos = buffer.writeIndex.load(std::memory_order_relaxed);
    size_t nextWritePos = (writePos + 1) % RING_BUFFER_SIZE;

    if (nextWritePos == buffer.readIndex.load(std::memory_order_acquire)) {
        // Buffer full - drop this event (better than blocking)
        return;
    }

    TimingEvent& event = buffer.events[writePos];
    event.sectionName = sectionName;
    event.durationMs = durationMs;
    event.threadId = buffer.threadId;
    event.depth = depth;
    event.scopeDepth = static_cast<uint8_t>((std::min)(buffer.scopeStack.size(), event.scopeNames.size()));
    for (size_t index = 0; index < event.scopeDepth; ++index) {
        event.scopeNames[index] = buffer.scopeStack[index];
    }
    for (size_t index = event.scopeDepth; index < event.scopeNames.size(); ++index) {
        event.scopeNames[index] = nullptr;
    }
    event.isRenderThread = buffer.isRenderThread;

    // Publish the write (release semantics ensure event data is visible)
    buffer.writeIndex.store(nextWritePos, std::memory_order_release);
}

void Profiler::SubmitExternalTiming(const char* sectionName, double durationMs) {
    if (!m_enabled.load(std::memory_order_relaxed) || !sectionName) { return; }
    ThreadRingBuffer& buffer = GetThreadBuffer();
    SubmitEvent(sectionName, durationMs, static_cast<uint8_t>(buffer.scopeStack.size()), buffer);
}

void Profiler::StartProcessingThread() {
    bool expected = false;
    if (!m_processingThreadRunning.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) { return; }

    try {
        m_processingThread = std::thread(&Profiler::ProcessingThreadMain, this);
    } catch (...) {
        m_processingThreadRunning.store(false, std::memory_order_release);
        throw;
    }
}

void Profiler::StopProcessingThread() {
    if (!m_processingThreadRunning.exchange(false, std::memory_order_acq_rel)) return;
    if (m_processingThread.joinable()) { m_processingThread.join(); }
}

void Profiler::ProcessingThreadMain() {
    while (m_processingThreadRunning.load()) {
        ProcessEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}

void Profiler::ProcessEvents() {
    std::lock_guard<std::recursive_mutex> processLock(m_processMutex);

    // Process events from all registered thread buffers
    while (m_registryLock.test_and_set(std::memory_order_acquire)) {}
    std::vector<std::shared_ptr<ThreadRingBuffer>> buffers = m_threadRegistry; // Keep buffers alive while processing.
    std::erase_if(m_threadRegistry, [](const std::shared_ptr<ThreadRingBuffer>& buffer) {
        return !buffer->isValid.load(std::memory_order_acquire);
    });
    m_registryLock.clear(std::memory_order_release);

    for (const auto& buffer : buffers) {
        // Skip invalidated buffers (thread has exited)
        if (!buffer->isValid.load(std::memory_order_acquire)) { continue; }

        size_t readPos = buffer->readIndex.load(std::memory_order_relaxed);
        size_t writePos = buffer->writeIndex.load(std::memory_order_acquire);

        while (readPos != writePos) {
            const TimingEvent& event = buffer->events[readPos];

            auto& targetEntries = event.isRenderThread ? m_renderThreadEntries : m_otherThreadEntries;

            const std::string pathKey = BuildScopeKey(event, event.scopeDepth);

            auto& entry = targetEntries[pathKey];
            entry.displayName = event.sectionName;
            entry.totalTime += event.durationMs;
            entry.callCount++;
            entry.depth = event.depth;
            entry.lastUpdateTime = std::chrono::steady_clock::now();

            if (event.scopeDepth > 1) {
                std::string parentKey = BuildScopeKey(event, static_cast<uint8_t>(event.scopeDepth - 1));
                entry.parentPath = parentKey;

                auto& parentEntry = targetEntries[parentKey];
                parentEntry.displayName = event.scopeNames[event.scopeDepth - 2];
                parentEntry.depth = entry.depth > 0 ? entry.depth - 1 : 0;
                bool found = false;
                for (const auto& child : parentEntry.childPaths) {
                    if (child == pathKey) {
                        found = true;
                        break;
                    }
                }
                if (!found) { parentEntry.childPaths.push_back(pathKey); }
            } else {
                entry.parentPath.clear();
            }

            if (event.durationMs > entry.maxTimeInLastSecond) { entry.maxTimeInLastSecond = event.durationMs; }

            readPos = (readPos + 1) % RING_BUFFER_SIZE;
        }

        buffer->readIndex.store(readPos, std::memory_order_release);
    }
}

void Profiler::CalculateHierarchy(std::unordered_map<std::string, ProfileEntry>& entries, double totalTime) {
    for (auto& [path, entry] : entries) {
        double childrenTime = 0.0;
        for (const auto& childPath : entry.childPaths) {
            auto it = entries.find(childPath);
            if (it != entries.end()) { childrenTime += it->second.totalTime; }
        }
        entry.selfTime = entry.totalTime - childrenTime;
        if (entry.selfTime < 0.0) entry.selfTime = 0.0;
    }

    for (auto& [path, entry] : entries) {
        entry.totalPercentage = totalTime > 0.0 ? (entry.totalTime / totalTime) * 100.0 : 0.0;

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

    struct DisplayChild {
        std::string path;
        double rollingAverageTime = 0.0;
        bool isSyntheticUnspecified = false;
    };

    std::unordered_map<std::string, std::vector<std::string>> childrenMap;
    std::vector<std::string> rootEntries;

    for (const auto& [path, entry] : entries) {
        if (entry.parentPath.empty()) {
            rootEntries.push_back(path);
        } else {
            childrenMap[entry.parentPath].push_back(path);
        }
    }

    auto sortByTime = [&entries](std::vector<std::string>& names) {
        std::sort(names.begin(), names.end(), [&entries](const std::string& a, const std::string& b) {
            auto itA = entries.find(a);
            auto itB = entries.find(b);
            double timeA = (itA != entries.end()) ? itA->second.rollingAverageTime : 0.0;
            double timeB = (itB != entries.end()) ? itB->second.rollingAverageTime : 0.0;
            return timeA > timeB;
        });
    };

    sortByTime(rootEntries);

    for (auto& [parent, children] : childrenMap) { sortByTime(children); }

    std::function<void(const std::string&)> addEntryWithChildren = [&](const std::string& path) {
        auto it = entries.find(path);
        if (it != entries.end()) {
            const ProfileEntry& entry = it->second;
            output.emplace_back(path, entry);

            std::vector<DisplayChild> displayChildren;
            auto childIt = childrenMap.find(path);
            if (childIt != childrenMap.end()) {
                displayChildren.reserve(childIt->second.size() + 1);
                for (const auto& childPath : childIt->second) {
                    auto childEntryIt = entries.find(childPath);
                    if (childEntryIt == entries.end()) { continue; }

                    displayChildren.push_back({ childPath, childEntryIt->second.rollingAverageTime, false });
                }

                if (!displayChildren.empty() && entry.rollingSelfTime > kProfilerUnspecifiedDisplayThresholdMs) {
                    displayChildren.push_back({ BuildUnspecifiedDisplayKey(path), entry.rollingSelfTime, true });
                }
            }

            std::sort(displayChildren.begin(), displayChildren.end(), [](const DisplayChild& a, const DisplayChild& b) {
                if (a.rollingAverageTime != b.rollingAverageTime) {
                    return a.rollingAverageTime > b.rollingAverageTime;
                }

                if (a.isSyntheticUnspecified != b.isSyntheticUnspecified) {
                    return !a.isSyntheticUnspecified;
                }

                return a.path < b.path;
            });

            for (const auto& child : displayChildren) {
                if (child.isSyntheticUnspecified) {
                    output.emplace_back(child.path, BuildUnspecifiedDisplayEntry(path, entry));
                } else {
                    addEntryWithChildren(child.path);
                }
            }
        }
    };

    for (const auto& rootPath : rootEntries) { addEntryWithChildren(rootPath); }
}

void Profiler::EndFrame() {
    if (!m_enabled) return;

    std::lock_guard<std::recursive_mutex> processLock(m_processMutex);

    auto currentTime = std::chrono::steady_clock::now();

    ProcessEvents();

    m_totalRenderTime = SumRootScopeTimes(m_renderThreadEntries);
    m_totalOtherTime = SumRootScopeTimes(m_otherThreadEntries);

    CalculateHierarchy(m_renderThreadEntries, m_totalRenderTime);
    CalculateHierarchy(m_otherThreadEntries, m_totalOtherTime);

    m_accumulatedRenderTime += m_totalRenderTime;
    m_accumulatedOtherTime += m_totalOtherTime;
    m_frameCountForAveraging++;

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

    constexpr auto STALE_THRESHOLD = std::chrono::seconds(5);
    auto removeStaleEntries = [&currentTime, &STALE_THRESHOLD](std::unordered_map<std::string, ProfileEntry>& entries) {
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

    auto timeSinceLastUpdate = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - m_lastUpdateTime);
    if (timeSinceLastUpdate.count() >= UPDATE_INTERVAL_MS) {
        double avgRenderTime = m_frameCountForAveraging > 0 ? m_accumulatedRenderTime / m_frameCountForAveraging : 0.0;
        double avgOtherTime = m_frameCountForAveraging > 0 ? m_accumulatedOtherTime / m_frameCountForAveraging : 0.0;

        auto updateRollingAverages = [this](std::unordered_map<std::string, ProfileEntry>& entries, double avgTotal) {
            for (auto& [path, entry] : entries) {
                if (entry.frameCount > 0) {
                    entry.rollingAverageTime = entry.accumulatedTime / entry.frameCount;
                    entry.rollingSelfTime = entry.accumulatedSelfTime / entry.frameCount;
                    entry.rollingAverageCalls = static_cast<double>(entry.accumulatedCalls) / entry.frameCount;
                } else {
                    entry.rollingAverageCalls = 0.0;
                }
            }

            for (auto& [path, entry] : entries) {
                entry.totalPercentage = avgTotal > 0.0 ? (entry.rollingAverageTime / avgTotal) * 100.0 : 0.0;

                if (!entry.parentPath.empty()) {
                    auto parentIt = entries.find(entry.parentPath);
                    if (parentIt != entries.end() && parentIt->second.rollingAverageTime > 0.0) {
                        entry.parentPercentage = (entry.rollingAverageTime / parentIt->second.rollingAverageTime) * 100.0;
                    } else {
                        entry.parentPercentage = 0.0;
                    }
                } else {
                    entry.parentPercentage = entry.totalPercentage;
                }
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

        auto resetWindowMaximums = [](std::unordered_map<std::string, ProfileEntry>& entries) {
            for (auto& [path, entry] : entries) { entry.maxTimeInLastSecond = 0.0; }
        };
        resetWindowMaximums(m_renderThreadEntries);
        resetWindowMaximums(m_otherThreadEntries);

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
    std::lock_guard<std::recursive_mutex> processLock(m_processMutex);

    // Clear all thread buffers
    while (m_registryLock.test_and_set(std::memory_order_acquire)) {}

    for (const auto& buffer : m_threadRegistry) {
        // Only the producer thread may change writeIndex or its scope stack.
        // Advancing the consumer cursor safely discards currently queued
        // events without racing an active profiled worker.
        const size_t writeIndex = buffer->writeIndex.load(std::memory_order_acquire);
        buffer->readIndex.store(writeIndex, std::memory_order_release);
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


