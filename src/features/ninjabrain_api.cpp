#include "ninjabrain_api.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "common/utils.h"
#include "config/config_defaults.h"

#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>

namespace {

using json = nlohmann::json;
using namespace std::chrono_literals;
using SteadyClock = std::chrono::steady_clock;

constexpr auto kNinjabrainReconnectIntervalMs = 200;
constexpr auto kNinjabrainConnectionTimeout = 200ms;
constexpr char kStrongholdEventsPath[] = "/api/v1/stronghold/events";
constexpr char kInformationMessagesEventsPath[] = "/api/v1/information-messages/events";
constexpr char kBoatEventsPath[] = "/api/v1/boat/events";
constexpr char kBlindEventsPath[] = "/api/v1/blind/events";

std::string TrimString(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) { return {}; }

    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

double NormalizeAngleDegrees(double angle) {
    if (!std::isfinite(angle)) { return 0.0; }
    angle = std::fmod(angle, 360.0);
    if (angle > 180.0) { angle -= 360.0; }
    if (angle < -180.0) { angle += 360.0; }
    return angle;
}

int ChunkCoordinateToBlock(int chunkCoordinate) {
    const long long blockCoordinate = static_cast<long long>(chunkCoordinate) * 16ll + 4ll;
    return static_cast<int>((std::clamp)(blockCoordinate,
                                         static_cast<long long>((std::numeric_limits<int>::min)()),
                                         static_cast<long long>((std::numeric_limits<int>::max)())));
}

void LogIfPresent(const NinjabrainLogCallback& callback, const std::string& message) {
    if (callback) { callback(message); }
}

void LogNinjabrainApiMessage(const std::string& message) {
    LogCategory("ninjabrain", message);
}

long long DurationToMilliseconds(SteadyClock::duration duration) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

std::pair<time_t, time_t> SplitDurationForHttplibTimeout(std::chrono::milliseconds duration) {
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration);
    const auto remainder = std::chrono::duration_cast<std::chrono::microseconds>(duration - seconds);
    return { static_cast<time_t>(seconds.count()), static_cast<time_t>(remainder.count()) };
}

template <typename Callback, typename... Args>
void InvokeIfPresent(const Callback& callback, Args&&... args) {
    if (callback) { callback(std::forward<Args>(args)...); }
}

} // namespace

void NinjabrainApiConnectionTracker::Start(std::string apiBaseUrl) {
    sessionRunning_ = true;
    apiBaseUrl_ = NormalizeNinjabrainApiBaseUrl(std::move(apiBaseUrl));
    lastError_.clear();
    strongholdState_ = StreamState::Connecting;
    informationMessagesState_ = StreamState::Connecting;
    boatState_ = StreamState::Connecting;
    blindState_ = StreamState::Connecting;
}

void NinjabrainApiConnectionTracker::Stop() {
    sessionRunning_ = false;
    lastError_.clear();
    strongholdState_ = StreamState::Disconnected;
    informationMessagesState_ = StreamState::Disconnected;
    boatState_ = StreamState::Disconnected;
    blindState_ = StreamState::Disconnected;
}

void NinjabrainApiConnectionTracker::MarkStrongholdConnected() {
    MarkStreamConnected(strongholdState_);
}

void NinjabrainApiConnectionTracker::MarkStrongholdDisconnected(std::string error) {
    MarkStreamDisconnected(strongholdState_, std::move(error));
}

void NinjabrainApiConnectionTracker::MarkInformationMessagesConnected() {
    MarkStreamConnected(informationMessagesState_);
}

void NinjabrainApiConnectionTracker::MarkInformationMessagesDisconnected(std::string error) {
    MarkStreamDisconnected(informationMessagesState_, std::move(error));
}

void NinjabrainApiConnectionTracker::MarkBoatConnected() {
    MarkStreamConnected(boatState_);
}

void NinjabrainApiConnectionTracker::MarkBoatDisconnected(std::string error) {
    MarkStreamDisconnected(boatState_, std::move(error));
}

void NinjabrainApiConnectionTracker::MarkBlindConnected() {
    MarkStreamConnected(blindState_);
}

void NinjabrainApiConnectionTracker::MarkBlindDisconnected(std::string error) {
    MarkStreamDisconnected(blindState_, std::move(error));
}

NinjabrainApiStatus NinjabrainApiConnectionTracker::Snapshot() const {
    NinjabrainApiStatus status;
    status.apiBaseUrl = apiBaseUrl_;

    if (!sessionRunning_) {
        status.connectionState = NinjabrainApiConnectionState::Stopped;
        return status;
    }

    if (strongholdState_ == StreamState::Connected ||
        informationMessagesState_ == StreamState::Connected ||
        boatState_ == StreamState::Connected ||
        blindState_ == StreamState::Connected) {
        status.connectionState = NinjabrainApiConnectionState::Connected;
        return status;
    }

    if (!lastError_.empty()) {
        status.connectionState = NinjabrainApiConnectionState::Offline;
        status.error = lastError_;
        return status;
    }

    status.connectionState = NinjabrainApiConnectionState::Connecting;
    return status;
}

void NinjabrainApiConnectionTracker::MarkStreamConnected(StreamState& streamState) {
    streamState = StreamState::Connected;
    if (strongholdState_ == StreamState::Connected &&
        informationMessagesState_ == StreamState::Connected &&
        boatState_ == StreamState::Connected &&
        blindState_ == StreamState::Connected) {
        lastError_.clear();
    }
}

void NinjabrainApiConnectionTracker::MarkStreamDisconnected(StreamState& streamState, std::string error) {
    streamState = StreamState::Disconnected;
    lastError_ = std::move(error);
}

void ClearNinjabrainStrongholdData(NinjabrainData& data) {
    const std::string boatState = data.boatState;
    const double boatAngle = data.boatAngle;
    const bool hasBoatAngle = data.hasBoatAngle;
    const auto informationMessages = data.informationMessages;
    const int informationMessageCount = data.informationMessageCount;
    const NinjabrainBlindData blind = data.blind;

    data = NinjabrainData{};
    data.informationMessages = informationMessages;
    data.informationMessageCount = informationMessageCount;
    data.boatState = boatState;
    data.boatAngle = boatAngle;
    data.hasBoatAngle = hasBoatAngle;
    data.blind = blind;
}

void ClearNinjabrainInformationMessagesData(NinjabrainData& data) {
    data.informationMessages = {};
    data.informationMessageCount = 0;
}

void ClearNinjabrainBoatData(NinjabrainData& data) {
    data.boatState = "NONE";
    data.boatAngle = 0.0;
    data.hasBoatAngle = false;
}

void ClearNinjabrainBlindData(NinjabrainData& data) {
    data.blind = {};
}

void ApplyNinjabrainBoatEvent(
    const std::string& payload,
    NinjabrainData& data,
    const NinjabrainLogCallback& logError) {
    if (payload.empty()) { return; }

    try {
        const json parsed = json::parse(payload);
        data.boatState = parsed.value("boatState", "NONE");

        const auto boatAngle = parsed.find("boatAngle");
        if (boatAngle != parsed.end() && !boatAngle->is_null()) {
            data.boatAngle = boatAngle->get<double>();
            data.hasBoatAngle = true;
        } else {
            data.boatAngle = 0.0;
            data.hasBoatAngle = false;
        }
    } catch (const std::exception& exception) {
        LogIfPresent(logError, "Failed to parse boat event: " + std::string(exception.what()));
    }
}

void ApplyNinjabrainStrongholdEvent(
    const std::string& payload,
    NinjabrainData& data,
    const NinjabrainLogCallback& logError) {
    if (payload.empty()) { return; }

    try {
        const json parsedJson = json::parse(payload);

        const std::string resultType = parsedJson.value("resultType", "NONE");
        if (resultType == "NONE") {
            ClearNinjabrainStrongholdData(data);
            data.resultType = resultType;
            return;
        }

        NinjabrainData next;
        next.resultType = resultType;

        if (const auto playerPosition = parsedJson.find("playerPosition");
            playerPosition != parsedJson.end() && playerPosition->is_object() && !playerPosition->empty()) {
            next.playerX = playerPosition->value("xInOverworld", 0.0);
            next.playerZ = playerPosition->value("zInOverworld", 0.0);
            next.playerInNether = playerPosition->value("isInNether", false);
            next.playerHorizontalAngle = playerPosition->value("horizontalAngle", 0.0);
            next.hasPlayerPos = true;
        }

        if (const auto eyeThrows = parsedJson.find("eyeThrows"); eyeThrows != parsedJson.end() && eyeThrows->is_array()) {
            next.eyeCount = (std::min)(static_cast<int>(eyeThrows->size()), static_cast<int>(next.throws.size()));
            for (int index = 0; index < next.eyeCount; ++index) {
                const auto& throwJson = (*eyeThrows)[index];
                auto& currentThrow = next.throws[index];
                currentThrow.xInOverworld = throwJson.value("xInOverworld", next.playerX);
                currentThrow.zInOverworld = throwJson.value("zInOverworld", next.playerZ);
                currentThrow.hasPosition = throwJson.contains("xInOverworld") || throwJson.contains("zInOverworld") || next.hasPlayerPos;
                currentThrow.angle = throwJson.value("angle", 0.0);
                currentThrow.angleWithoutCorrection = throwJson.value("angleWithoutCorrection", currentThrow.angle);
                currentThrow.correction = throwJson.value("correction", 0.0);
                currentThrow.error = throwJson.value("error", currentThrow.correction);
                currentThrow.type = throwJson.value("type", "NORMAL");

                const auto correctionIncrements = throwJson.find("correctionIncrements");
                if (correctionIncrements != throwJson.end() && !correctionIncrements->is_null()) {
                    currentThrow.correctionIncrements = correctionIncrements->get<int>();
                    currentThrow.hasCorrectionIncrements = true;
                }
            }
        }

        if (next.eyeCount > 0 && !next.throws[next.eyeCount - 1].hasCorrectionIncrements) {
            next.correctionIncrements151 = data.correctionIncrements151;

            if (next.eyeCount != data.eyeCount) {
                next.correctionIncrements151 = 0;
            } else {
                const double previousCorrection = data.throws[data.eyeCount - 1].correction;
                const double delta = next.throws[next.eyeCount - 1].correction - previousCorrection;
                if (delta > 1e-9) {
                    ++next.correctionIncrements151;
                } else if (delta < -1e-9) {
                    --next.correctionIncrements151;
                }
            }
        }

        if (next.eyeCount >= 1) {
            const auto& lastThrow = next.throws[next.eyeCount - 1];
            next.lastAngle = lastThrow.angle;
            next.lastAngleWithoutCorrection = lastThrow.angleWithoutCorrection;
            next.lastCorrection = lastThrow.correction;
            next.lastThrowError = lastThrow.error;
            next.hasCorrection = std::abs(next.lastCorrection) > 1e-9;
            next.hasThrowError = std::abs(next.lastThrowError) > 1e-9;

            if (next.eyeCount >= 2) {
                next.prevAngle = next.throws[next.eyeCount - 2].angle;
                next.hasAngleChange = true;
                next.hasNetherAngle = true;
                next.netherAngle = next.lastAngle;
                next.netherAngleDiff = next.lastAngle - next.throws[0].angle;
            }
        }

        if (const auto predictions = parsedJson.find("predictions"); predictions != parsedJson.end() && predictions->is_array()) {
            next.predictionCount =
                (std::min)(static_cast<int>(predictions->size()), static_cast<int>(next.predictions.size()));
            for (int index = 0; index < next.predictionCount; ++index) {
                const auto& predictionJson = (*predictions)[index];
                auto& prediction = next.predictions[index];
                prediction.chunkX = predictionJson.value("chunkX", 0);
                prediction.chunkZ = predictionJson.value("chunkZ", 0);
                prediction.certainty = predictionJson.value("certainty", 0.0);
                prediction.overworldDistance = predictionJson.value("overworldDistance", 0.0);

                if (next.hasPlayerPos) {
                    constexpr double kPi = 3.14159265358979323846;
                    const double blockX = prediction.chunkX * 16.0 + 4.0;
                    const double blockZ = prediction.chunkZ * 16.0 + 4.0;
                    const double xDiff = blockX - next.playerX;
                    const double zDiff = blockZ - next.playerZ;
                    const double structureAngle = -std::atan2(xDiff, zDiff) * 180.0 / kPi;

                    next.predictionAngles[index].actualAngle = structureAngle;
                    next.predictionAngles[index].neededCorrection =
                        NormalizeAngleDegrees(structureAngle - next.playerHorizontalAngle);
                    next.predictionAngles[index].valid = true;
                }
            }
        }

        if (next.predictionCount > 0) {
            next.strongholdX = ChunkCoordinateToBlock(next.predictions[0].chunkX);
            next.strongholdZ = ChunkCoordinateToBlock(next.predictions[0].chunkZ);
            next.distance = next.predictions[0].overworldDistance;
            next.certainty = next.predictions[0].certainty;
            next.validPrediction = true;
        }

        next.informationMessages = data.informationMessages;
        next.informationMessageCount = data.informationMessageCount;
        next.boatState = data.boatState;
        next.boatAngle = data.boatAngle;
        next.hasBoatAngle = data.hasBoatAngle;
        next.blind = data.blind;
        data = std::move(next);
    } catch (const std::exception& exception) {
        LogIfPresent(logError, "Failed to parse stronghold event: " + std::string(exception.what()));
    }
}

void ApplyNinjabrainInformationMessagesEvent(
    const std::string& payload,
    NinjabrainData& data,
    const NinjabrainLogCallback& logError) {
    if (payload.empty()) { return; }

    try {
        const json parsedJson = json::parse(payload);
        data.informationMessages = {};
        data.informationMessageCount = 0;

        const auto informationMessages = parsedJson.find("informationMessages");
        if (informationMessages == parsedJson.end() || !informationMessages->is_array()) {
            return;
        }

        data.informationMessageCount =
            (std::min)(static_cast<int>(informationMessages->size()), static_cast<int>(data.informationMessages.size()));
        for (int index = 0; index < data.informationMessageCount; ++index) {
            const auto& messageJson = (*informationMessages)[index];
            auto& message = data.informationMessages[index];
            message.severity = messageJson.value("severity", "INFO");
            message.type = messageJson.value("type", "");
            message.message = messageJson.value("message", "");
        }
    } catch (const std::exception& exception) {
        LogIfPresent(logError, "Failed to parse information-messages event: " + std::string(exception.what()));
    }
}

void ApplyNinjabrainBlindEvent(
    const std::string& payload,
    NinjabrainData& data,
    const NinjabrainLogCallback& logError) {
    if (payload.empty()) { return; }

    try {
        const json parsedJson = json::parse(payload);

        NinjabrainBlindData blind;
        blind.enabled = parsedJson.value("isBlindModeEnabled", false);
        blind.hasDivine = parsedJson.value("hasDivine", false);

        const auto blindResult = parsedJson.find("blindResult");
        if (blindResult != parsedJson.end() && blindResult->is_object() && !blindResult->empty()) {
            blind.hasResult = true;
            blind.evaluation = blindResult->value("evaluation", "");
            blind.xInNether = blindResult->value("xInNether", 0.0);
            blind.zInNether = blindResult->value("zInNether", 0.0);
            blind.improveDistance = blindResult->value("improveDistance", 0.0);
            blind.averageDistance = blindResult->value("averageDistance", 0.0);
            blind.improveDirection = blindResult->value("improveDirection", 0.0);
            blind.highrollProbability = blindResult->value("highrollProbability", 0.0);
            blind.highrollThreshold = blindResult->value("highrollThreshold", 0.0);
        }

        if (!blind.enabled) {
            blind = NinjabrainBlindData{};
        }

        data.blind = std::move(blind);
    } catch (const std::exception& exception) {
        LogIfPresent(logError, "Failed to parse blind event: " + std::string(exception.what()));
    }
}

std::string NormalizeNinjabrainApiBaseUrl(std::string apiBaseUrl) {
    apiBaseUrl = TrimString(std::move(apiBaseUrl));
    while (!apiBaseUrl.empty() && apiBaseUrl.back() == '/') { apiBaseUrl.pop_back(); }
    if (apiBaseUrl.empty()) { apiBaseUrl = ConfigDefaults::CONFIG_NINJABRAIN_API_BASE_URL; }
    return apiBaseUrl;
}

NinjabrainApiSession::NinjabrainApiSession(std::string apiBaseUrl, NinjabrainApiSessionCallbacks callbacks)
    : apiBaseUrl_(NormalizeNinjabrainApiBaseUrl(std::move(apiBaseUrl))),
      callbacks_(std::move(callbacks)) {
    strongholdThread_ = std::jthread([this](std::stop_token stopToken) {
        RunStream(
            stopToken,
            "stronghold",
            kStrongholdEventsPath,
            callbacks_.onStrongholdMessage,
            callbacks_.onStrongholdConnect,
            callbacks_.onStrongholdDisconnect);
    });
    informationMessagesThread_ = std::jthread([this](std::stop_token stopToken) {
        RunStream(
            stopToken,
            "information-messages",
            kInformationMessagesEventsPath,
            callbacks_.onInformationMessagesMessage,
            callbacks_.onInformationMessagesConnect,
            callbacks_.onInformationMessagesDisconnect);
    });
    boatThread_ = std::jthread([this](std::stop_token stopToken) {
        RunStream(
            stopToken,
            "boat",
            kBoatEventsPath,
            callbacks_.onBoatMessage,
            callbacks_.onBoatConnect,
            callbacks_.onBoatDisconnect);
    });
    blindThread_ = std::jthread([this](std::stop_token stopToken) {
        RunStream(
            stopToken,
            "blind",
            kBlindEventsPath,
            callbacks_.onBlindMessage,
            callbacks_.onBlindConnect,
            callbacks_.onBlindDisconnect);
    });
}

NinjabrainApiSession::~NinjabrainApiSession() {
    Stop();
}

void NinjabrainApiSession::Stop() {
    strongholdThread_.request_stop();
    informationMessagesThread_.request_stop();
    boatThread_.request_stop();
    blindThread_.request_stop();
}

void NinjabrainApiSession::RunStream(
    std::stop_token stopToken,
    const char* streamName,
    const char* path,
    const std::function<void(const std::string&)>& onMessage,
    const std::function<void()>& onConnect,
    const std::function<void(const std::string&)>& onDisconnect) const {
    httplib::Client client(apiBaseUrl_);
    client.set_keep_alive(true);
    const auto [connectionTimeoutSeconds, connectionTimeoutMicros] =
        SplitDurationForHttplibTimeout(std::chrono::duration_cast<std::chrono::milliseconds>(kNinjabrainConnectionTimeout));
    client.set_connection_timeout(connectionTimeoutSeconds, connectionTimeoutMicros);

    const httplib::Headers headers = {
        { "Accept", "text/event-stream" },
        { "Cache-Control", "no-cache" },
    };

    httplib::sse::SSEClient sse(client, path, headers);
    sse.set_reconnect_interval(kNinjabrainReconnectIntervalMs);

    const std::string streamNameString = streamName;
    const long long connectionTimeoutMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(kNinjabrainConnectionTimeout).count();
    auto currentAttemptStartedAt = SteadyClock::now();
    auto outageStartedAt = currentAttemptStartedAt;
    auto connectedAt = SteadyClock::time_point{};
    bool outageInProgress = true;
    bool connected = false;
    bool disconnected = false;
    bool hasConnectedOnce = false;

    LogNinjabrainApiMessage(
        "NinjabrainBot API connecting " + streamNameString + " stream to " + apiBaseUrl_ + path +
        " (connection timeout " + std::to_string(connectionTimeoutMs) + " ms, reconnect delay " +
        std::to_string(kNinjabrainReconnectIntervalMs) + " ms).");

    sse.on_open([&, streamName = streamNameString]() {
        const auto now = SteadyClock::now();
        const long long outageDurationMs = DurationToMilliseconds(now - outageStartedAt);

        InvokeIfPresent(onConnect);
        connectedAt = now;
        connected = true;
        currentAttemptStartedAt = now;

        if (!hasConnectedOnce) {
            LogNinjabrainApiMessage(
                "NinjabrainBot API connected " + streamName + " stream after " +
                std::to_string(outageDurationMs) + " ms.");
            hasConnectedOnce = true;
        } else if (disconnected) {
            LogNinjabrainApiMessage(
                "NinjabrainBot API reconnected " + streamName + " stream after " +
                std::to_string(outageDurationMs) + " ms of downtime.");
            LogIfPresent(callbacks_.onLog, "Reconnected " + streamName + " stream.");
        }

        disconnected = false;
        outageInProgress = false;
    });
    sse.on_message([&](const httplib::sse::SSEMessage& message) {
        if (onMessage) { onMessage(message.data); }
    });
    sse.on_error([&, streamName = streamNameString](httplib::Error error) {
        if (stopToken.stop_requested() || error == httplib::Error::Canceled) { return; }

        const auto now = SteadyClock::now();
        const std::string errorString = httplib::to_string(error);
        const long long attemptDurationMs = DurationToMilliseconds(now - currentAttemptStartedAt);

        if (!outageInProgress) {
            outageStartedAt = now;
            outageInProgress = true;
        }

        const long long outageDurationMs = DurationToMilliseconds(now - outageStartedAt);

        InvokeIfPresent(onDisconnect, errorString);

        if (connected) {
            const long long connectedDurationMs = DurationToMilliseconds(now - connectedAt);
            LogNinjabrainApiMessage(
                "NinjabrainBot API lost " + streamName + " stream after " +
                std::to_string(connectedDurationMs) + " ms connected: " + errorString +
                ". Retrying in " + std::to_string(kNinjabrainReconnectIntervalMs) + " ms.");
            connected = false;
        } else if (error == httplib::Error::ConnectionTimeout) {
            LogNinjabrainApiMessage(
                "NinjabrainBot API " + streamName + " stream connection attempt timed out after " +
                std::to_string(attemptDurationMs) + " ms (total delay " +
                std::to_string(outageDurationMs) + " ms). Retrying in " +
                std::to_string(kNinjabrainReconnectIntervalMs) + " ms.");
        } else {
            const std::string attemptKind = hasConnectedOnce ? "reconnect" : "initial connection";
            LogNinjabrainApiMessage(
                "NinjabrainBot API " + streamName + " stream " + attemptKind + " failed after " +
                std::to_string(attemptDurationMs) + " ms (total delay " +
                std::to_string(outageDurationMs) + " ms): " + errorString + ". Retrying in " +
                std::to_string(kNinjabrainReconnectIntervalMs) + " ms.");
        }

        if (!disconnected) {
            LogIfPresent(
                callbacks_.onLog,
                "Lost " + streamName + " stream: " + errorString + ". Waiting for reconnect.");
            disconnected = true;
        }

        currentAttemptStartedAt = now;
    });

    std::stop_callback stopCallback(stopToken, [&]() {
        sse.stop();
        client.stop();
    });

    sse.start();
}
