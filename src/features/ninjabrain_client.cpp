#include "ninjabrain_client.h"

#include "features/ninjabrain_api.h"

#include "common/utils.h"
#include "config/config_defaults.h"
#include "gui/gui.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

struct BackgroundStopThread {
    std::thread thread;
    std::shared_ptr<std::atomic_bool> completed;
};

std::mutex g_ninjabrainClientMutex;
NinjabrainApiConnectionTracker g_ninjabrainClientStatus;
std::vector<BackgroundStopThread> g_ninjabrainClientStopThreads;
uint64_t g_ninjabrainClientGeneration = 0;
uint64_t g_ninjabrainClientActiveGeneration = 0;

bool IsNinjabrainOverlayEnabled() {
    if (const auto configSnapshot = GetConfigSnapshot()) {
        return configSnapshot->ninjabrainOverlay.enabled;
    }

    return g_config.ninjabrainOverlay.enabled;
}

void LogNinjabrainMessage(const std::string& message) {
    LogCategory(Log_Ninjabrain, message);
}

std::string ResolveApiBaseUrl() {
    std::string apiBaseUrl = ConfigDefaults::CONFIG_NINJABRAIN_API_BASE_URL;

    if (const auto configSnapshot = GetConfigSnapshot()) {
        apiBaseUrl = configSnapshot->ninjabrainOverlay.apiBaseUrl;
    } else {
        apiBaseUrl = g_config.ninjabrainOverlay.apiBaseUrl;
    }

    return NormalizeNinjabrainApiBaseUrl(std::move(apiBaseUrl));
}

std::unique_ptr<NinjabrainApiSession> g_ninjabrainClientSession;

bool IsActiveNinjabrainClientGeneration(uint64_t generation) {
    std::lock_guard<std::mutex> lock(g_ninjabrainClientMutex);
    return g_ninjabrainClientActiveGeneration == generation;
}

void ReapCompletedNinjabrainClientStopThreads(std::vector<std::thread>& threadsToJoin) {
    std::lock_guard<std::mutex> lock(g_ninjabrainClientMutex);
    auto it = g_ninjabrainClientStopThreads.begin();
    while (it != g_ninjabrainClientStopThreads.end()) {
        if (it->completed && it->completed->load(std::memory_order_acquire) && it->thread.joinable() &&
            it->thread.get_id() != std::this_thread::get_id()) {
            threadsToJoin.push_back(std::move(it->thread));
            it = g_ninjabrainClientStopThreads.erase(it);
            continue;
        }

        ++it;
    }
}

} // namespace

void StartNinjabrainClient() {
    std::vector<std::thread> threadsToJoin;
    ReapCompletedNinjabrainClientStopThreads(threadsToJoin);
    for (auto& threadToJoin : threadsToJoin) {
        if (threadToJoin.joinable()) { threadToJoin.join(); }
    }

    uint64_t generation = 0;
    std::lock_guard<std::mutex> lock(g_ninjabrainClientMutex);
    if (!IsNinjabrainOverlayEnabled()) {
        return;
    }
    if (g_ninjabrainClientSession) {
        return;
    }

    const std::string apiBaseUrl = ResolveApiBaseUrl();
    generation = ++g_ninjabrainClientGeneration;
    g_ninjabrainClientActiveGeneration = generation;

    PublishNinjabrainData(NinjabrainData{});

    g_ninjabrainClientStatus.Start(apiBaseUrl);

    NinjabrainApiSessionCallbacks callbacks;
    callbacks.onStrongholdMessage = [generation](const std::string& payload) {
        if (!IsActiveNinjabrainClientGeneration(generation)) { return; }
        ModifyNinjabrainData([&](NinjabrainData& data) {
            ApplyNinjabrainStrongholdEvent(payload, data, LogNinjabrainMessage);
        });
    };
    callbacks.onStrongholdConnect = [generation]() {
        if (!IsActiveNinjabrainClientGeneration(generation)) { return; }
        std::lock_guard<std::mutex> lock(g_ninjabrainClientMutex);
        g_ninjabrainClientStatus.MarkStrongholdConnected();
    };
    callbacks.onStrongholdDisconnect = [generation](const std::string& error) {
        if (!IsActiveNinjabrainClientGeneration(generation)) { return; }
        {
            std::lock_guard<std::mutex> lock(g_ninjabrainClientMutex);
            g_ninjabrainClientStatus.MarkStrongholdDisconnected(error);
        }
        ModifyNinjabrainData([](NinjabrainData& data) {
            ClearNinjabrainStrongholdData(data);
        });
    };
    callbacks.onInformationMessagesMessage = [generation](const std::string& payload) {
        if (!IsActiveNinjabrainClientGeneration(generation)) { return; }
        ModifyNinjabrainData([&](NinjabrainData& data) {
            ApplyNinjabrainInformationMessagesEvent(payload, data, LogNinjabrainMessage);
        });
    };
    callbacks.onInformationMessagesConnect = [generation]() {
        if (!IsActiveNinjabrainClientGeneration(generation)) { return; }
        std::lock_guard<std::mutex> lock(g_ninjabrainClientMutex);
        g_ninjabrainClientStatus.MarkInformationMessagesConnected();
    };
    callbacks.onInformationMessagesDisconnect = [generation](const std::string& error) {
        if (!IsActiveNinjabrainClientGeneration(generation)) { return; }
        {
            std::lock_guard<std::mutex> lock(g_ninjabrainClientMutex);
            g_ninjabrainClientStatus.MarkInformationMessagesDisconnected(error);
        }
        ModifyNinjabrainData([](NinjabrainData& data) {
            ClearNinjabrainInformationMessagesData(data);
        });
    };
    callbacks.onBoatMessage = [generation](const std::string& payload) {
        if (!IsActiveNinjabrainClientGeneration(generation)) { return; }
        ModifyNinjabrainData([&](NinjabrainData& data) {
            ApplyNinjabrainBoatEvent(payload, data, LogNinjabrainMessage);
        });
    };
    callbacks.onBoatConnect = [generation]() {
        if (!IsActiveNinjabrainClientGeneration(generation)) { return; }
        std::lock_guard<std::mutex> lock(g_ninjabrainClientMutex);
        g_ninjabrainClientStatus.MarkBoatConnected();
    };
    callbacks.onBoatDisconnect = [generation](const std::string& error) {
        if (!IsActiveNinjabrainClientGeneration(generation)) { return; }
        {
            std::lock_guard<std::mutex> lock(g_ninjabrainClientMutex);
            g_ninjabrainClientStatus.MarkBoatDisconnected(error);
        }
        ModifyNinjabrainData([](NinjabrainData& data) {
            ClearNinjabrainBoatData(data);
        });
    };
    callbacks.onBlindMessage = [generation](const std::string& payload) {
        if (!IsActiveNinjabrainClientGeneration(generation)) { return; }
        ModifyNinjabrainData([&](NinjabrainData& data) {
            ApplyNinjabrainBlindEvent(payload, data, LogNinjabrainMessage);
        });
    };
    callbacks.onBlindConnect = [generation]() {
        if (!IsActiveNinjabrainClientGeneration(generation)) { return; }
        std::lock_guard<std::mutex> lock(g_ninjabrainClientMutex);
        g_ninjabrainClientStatus.MarkBlindConnected();
    };
    callbacks.onBlindDisconnect = [generation](const std::string& error) {
        if (!IsActiveNinjabrainClientGeneration(generation)) { return; }
        {
            std::lock_guard<std::mutex> lock(g_ninjabrainClientMutex);
            g_ninjabrainClientStatus.MarkBlindDisconnected(error);
        }
        ModifyNinjabrainData([](NinjabrainData& data) {
            ClearNinjabrainBlindData(data);
        });
    };
    callbacks.onLog = LogNinjabrainMessage;

    try {
        g_ninjabrainClientSession = std::make_unique<NinjabrainApiSession>(apiBaseUrl, std::move(callbacks));
    } catch (...) {
        g_ninjabrainClientActiveGeneration = 0;
        g_ninjabrainClientStatus.Stop();
        throw;
    }
}

void StopNinjabrainClient() {
    std::vector<std::thread> threadsToJoin;
    std::unique_ptr<NinjabrainApiSession> session;
    {
        std::lock_guard<std::mutex> lock(g_ninjabrainClientMutex);
        g_ninjabrainClientActiveGeneration = 0;
        session = std::move(g_ninjabrainClientSession);
        for (auto& stopThread : g_ninjabrainClientStopThreads) {
            if (stopThread.thread.joinable() && stopThread.thread.get_id() != std::this_thread::get_id()) {
                threadsToJoin.push_back(std::move(stopThread.thread));
            }
        }
        g_ninjabrainClientStopThreads.clear();
        g_ninjabrainClientStatus.Stop();
    }

    if (session) { session->Stop(); }
    for (auto& threadToJoin : threadsToJoin) {
        if (threadToJoin.joinable()) { threadToJoin.join(); }
    }
}

void StopNinjabrainClientAsync() {
    std::vector<std::thread> threadsToJoin;
    ReapCompletedNinjabrainClientStopThreads(threadsToJoin);
    for (auto& threadToJoin : threadsToJoin) {
        if (threadToJoin.joinable()) { threadToJoin.join(); }
    }

    std::unique_ptr<NinjabrainApiSession> session;
    std::shared_ptr<std::atomic_bool> completed;
    {
        std::lock_guard<std::mutex> lock(g_ninjabrainClientMutex);
        g_ninjabrainClientActiveGeneration = 0;
        if (!g_ninjabrainClientSession) {
            g_ninjabrainClientStatus.Stop();
            return;
        }

        session = std::move(g_ninjabrainClientSession);
        g_ninjabrainClientStatus.Stop();
        completed = std::make_shared<std::atomic_bool>(false);
        g_ninjabrainClientStopThreads.push_back(BackgroundStopThread{
            std::thread([session = std::move(session), completed]() mutable {
                const auto stopStart = std::chrono::steady_clock::now();
                session->Stop();
                session.reset();

                const auto stopDurationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - stopStart).count();
                LogNinjabrainMessage("Client shutdown finished in " + std::to_string(stopDurationMs) + " ms.");
                completed->store(true, std::memory_order_release);
            }),
            completed,
        });
    }
}

void RestartNinjabrainClient() {
    StopNinjabrainClient();
    StartNinjabrainClient();
}

void RestartNinjabrainClientAsync() {
    if (!IsNinjabrainOverlayEnabled()) { return; }

    StopNinjabrainClientAsync();
    StartNinjabrainClient();
}

NinjabrainApiStatus GetNinjabrainClientStatus() {
    std::lock_guard<std::mutex> lock(g_ninjabrainClientMutex);
    return g_ninjabrainClientStatus.Snapshot();
}
