#include "features/game_state_source.h"

#include <nlohmann/json.hpp>

namespace game_state_source {
namespace {

constexpr const char* kHermesLoadingScreenClass    = "net.minecraft.class_3928";  // leveloadingscreen
constexpr const char* kHermesSeedQueueWallClass    = "me.contaria.seedqueue.gui.wall.SeedQueueWallScreen";
constexpr const char* kHermesSeedQueuePreviewClass = "me.contaria.seedqueue.gui.wall.SeedQueuePreview";

uint64_t ReadBigEndianU64(const unsigned char* bytes) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) { value = (value << 8) | bytes[i]; }
    return value;
}

std::string TrimWhitespace(const std::string& text) {
    const size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) { return std::string(); }
    const size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

}  // namespace

std::optional<std::string> DeriveStateFromHermesJson(const std::string& text) {
    if (text.empty()) { return std::nullopt; }
    nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) { return std::nullopt; }

    const auto world = j.find("world");
    const bool inWorld = world != j.end() && !world->is_null();

    std::string screenClass;
    if (auto s = j.find("screen"); s != j.end() && s->is_object()) {
        if (auto c = s->find("class"); c != s->end() && c->is_string()) { screenClass = c->get<std::string>(); }
    }

    // loading + seedqueue wall show while world is set
    if (screenClass == kHermesLoadingScreenClass) { return std::string("generating"); }
    if (screenClass == kHermesSeedQueueWallClass || screenClass == kHermesSeedQueuePreviewClass) { return std::string("wall"); }

    if (inWorld) {
        return screenClass.empty() ? std::string("inworld,cursor_grabbed") : std::string("inworld,cursor_free");
    }
    return std::string("title");
}

std::optional<std::string> DeriveStateFromStateOutputText(const std::string& text) {
    const std::string content = TrimWhitespace(text);
    if (content.empty()) { return std::nullopt; }

    if (content.rfind("generating", 0) == 0) { return std::string("generating"); }
    if (content == "inworld,unpaused") { return std::string("inworld,cursor_grabbed"); }
    if (content == "inworld,paused" || content == "inworld,gamescreenopen") { return std::string("inworld,cursor_free"); }

    if (content == "wall" || content == "title" || content == "waiting" || content == "inworld,cursor_free" ||
        content == "inworld,cursor_grabbed") {
        return content;
    }
    return std::nullopt;
}

GameStateSourceKind Select(bool hermesAlive, bool stateOutputAvailable) {
    if (hermesAlive) { return GameStateSourceKind::Hermes; }
    if (stateOutputAvailable) { return GameStateSourceKind::StateOutput; }
    return GameStateSourceKind::None;
}

bool EvaluateHermesAlive(const unsigned char bytes[16], uint64_t currentPid, long long nowEpochMs) {
    const uint64_t pid = ReadBigEndianU64(bytes);
    if (pid != currentPid) { return false; }

    // The heartbeat lives in shared memory owned by another mod, so treat it
    // as untrusted bytes. Avoid signed conversion/subtraction overflow for a
    // corrupt timestamp while preserving the allowed small future skew.
    if (nowEpochMs < 0) { return false; }
    const uint64_t heartbeatMs = ReadBigEndianU64(bytes + 8);
    const uint64_t nowMs = static_cast<uint64_t>(nowEpochMs);
    const uint64_t difference = nowMs >= heartbeatMs ? nowMs - heartbeatMs : heartbeatMs - nowMs;
    return difference < static_cast<uint64_t>(kHermesAliveStaleThresholdMs);
}

}  // namespace game_state_source
