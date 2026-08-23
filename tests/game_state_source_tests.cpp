#include "features/game_state_source.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void CheckOptEq(const std::optional<std::string>& actual, const std::optional<std::string>& expected, const std::string& label) {
    if (actual != expected) {
        std::cerr << "  ASSERT FAILED: " << label << " expected "
                  << (expected ? *expected : std::string("<nullopt>")) << " got "
                  << (actual ? *actual : std::string("<nullopt>")) << '\n';
        ++g_failures;
    }
}

void CheckTrue(bool condition, const std::string& label) {
    if (!condition) {
        std::cerr << "  ASSERT FAILED: " << label << '\n';
        ++g_failures;
    }
}

std::array<unsigned char, 16> MakeAliveBytes(uint64_t pid, uint64_t heartbeatMs) {
    std::array<unsigned char, 16> bytes{};
    for (int i = 0; i < 8; ++i) {
        bytes[i] = static_cast<unsigned char>((pid >> (8 * (7 - i))) & 0xFF);
        bytes[8 + i] = static_cast<unsigned char>((heartbeatMs >> (8 * (7 - i))) & 0xFF);
    }
    return bytes;
}

using game_state_source::DeriveStateFromHermesJson;
using game_state_source::DeriveStateFromStateOutputText;
using game_state_source::EvaluateHermesAlive;
using game_state_source::Select;

void StateOutputNormalizesUnpaused() {
    CheckOptEq(DeriveStateFromStateOutputText("inworld,unpaused"), std::string("inworld,cursor_grabbed"), "unpaused->grabbed");
}

void StateOutputNormalizesPausedAndGamescreen() {
    CheckOptEq(DeriveStateFromStateOutputText("inworld,paused"), std::string("inworld,cursor_free"), "paused->free");
    CheckOptEq(DeriveStateFromStateOutputText("inworld,gamescreenopen"), std::string("inworld,cursor_free"), "gamescreen->free");
}

void StateOutputGeneratingStripsSuffix() {
    CheckOptEq(DeriveStateFromStateOutputText("generating"), std::string("generating"), "bare generating");
    CheckOptEq(DeriveStateFromStateOutputText("generating,42%"), std::string("generating"), "generating with pct");
    CheckOptEq(DeriveStateFromStateOutputText("generating,"), std::string("generating"), "generating trailing comma");
}

void StateOutputPassesThroughCanonical() {
    CheckOptEq(DeriveStateFromStateOutputText("wall"), std::string("wall"), "wall");
    CheckOptEq(DeriveStateFromStateOutputText("title"), std::string("title"), "title");
    CheckOptEq(DeriveStateFromStateOutputText("waiting"), std::string("waiting"), "waiting");
    CheckOptEq(DeriveStateFromStateOutputText("inworld,cursor_free"), std::string("inworld,cursor_free"), "cursor_free");
    CheckOptEq(DeriveStateFromStateOutputText("inworld,cursor_grabbed"), std::string("inworld,cursor_grabbed"), "cursor_grabbed");
}

void StateOutputTrimsWhitespace() {
    CheckOptEq(DeriveStateFromStateOutputText("  wall\r\n"), std::string("wall"), "trimmed wall");
}

void StateOutputUnknownReturnsNullopt() {
    CheckOptEq(DeriveStateFromStateOutputText("bogus"), std::nullopt, "unknown token");
    CheckOptEq(DeriveStateFromStateOutputText(""), std::nullopt, "empty");
    CheckOptEq(DeriveStateFromStateOutputText("   \n"), std::nullopt, "whitespace only");
}

void HermesInworldGrabbedWhenNoScreen() {
    CheckOptEq(DeriveStateFromHermesJson(R"({"world":{}})"), std::string("inworld,cursor_grabbed"), "world no screen");
}

void HermesInworldFreeWhenScreen() {
    CheckOptEq(DeriveStateFromHermesJson(R"({"world":{},"screen":{"class":"some.Menu"}})"),
               std::string("inworld,cursor_free"), "world with screen");
}

void HermesTitleWhenNoWorld() {
    CheckOptEq(DeriveStateFromHermesJson(R"({"world":null})"), std::string("title"), "no world");
}

void HermesLoadingScreenGenerating() {
    CheckOptEq(DeriveStateFromHermesJson(R"({"world":{},"screen":{"class":"net.minecraft.class_3928"}})"),
               std::string("generating"), "loading screen");
}

void HermesSeedQueueWall() {
    CheckOptEq(DeriveStateFromHermesJson(R"({"world":null,"screen":{"class":"me.contaria.seedqueue.gui.wall.SeedQueueWallScreen"}})"),
               std::string("wall"), "seedqueue wall");
}

void HermesInvalidJsonNullopt() {
    CheckOptEq(DeriveStateFromHermesJson("{not json"), std::nullopt, "invalid json");
    CheckOptEq(DeriveStateFromHermesJson(""), std::nullopt, "empty json");
    CheckOptEq(DeriveStateFromHermesJson("[]"), std::nullopt, "array not object");
}

void SelectPrefersHermes() {
    CheckTrue(Select(true, true) == GameStateSourceKind::Hermes, "both -> hermes");
    CheckTrue(Select(true, false) == GameStateSourceKind::Hermes, "hermes only");
}

void SelectFallsBackToStateOutput() {
    CheckTrue(Select(false, true) == GameStateSourceKind::StateOutput, "stateoutput only");
}

void SelectNone() {
    CheckTrue(Select(false, false) == GameStateSourceKind::None, "neither");
}

void AliveFreshSamePidTrue() {
    auto bytes = MakeAliveBytes(1234, 100000);
    CheckTrue(EvaluateHermesAlive(bytes.data(), 1234, 101000), "fresh 1s same pid");
}

void AliveWrongPidFalse() {
    auto bytes = MakeAliveBytes(1234, 100000);
    CheckTrue(!EvaluateHermesAlive(bytes.data(), 5678, 100000), "wrong pid");
}

void AliveStaleHeartbeatFalse() {
    auto bytes = MakeAliveBytes(1234, 100000);
    CheckTrue(!EvaluateHermesAlive(bytes.data(), 1234, 104000), "4s stale");
}

void AliveFutureSkewWithinThresholdTrue() {
    auto bytes = MakeAliveBytes(1234, 101000);
    CheckTrue(EvaluateHermesAlive(bytes.data(), 1234, 100000), "1s future skew alive");
}

void AliveCorruptHeartbeatFalse() {
    auto bytes = MakeAliveBytes(1234, UINT64_MAX);
    CheckTrue(!EvaluateHermesAlive(bytes.data(), 1234, 100000), "corrupt maximum heartbeat rejected");
}

void AliveNegativeCurrentTimeFalse() {
    auto bytes = MakeAliveBytes(1234, 100000);
    CheckTrue(!EvaluateHermesAlive(bytes.data(), 1234, -1), "negative current time rejected");
}

struct TestCase {
    const char* name;
    std::function<void()> run;
};

const std::vector<TestCase>& Registry() {
    static const std::vector<TestCase> cases = {
        {"stateoutput_normalizes_unpaused", &StateOutputNormalizesUnpaused},
        {"stateoutput_normalizes_paused_and_gamescreen", &StateOutputNormalizesPausedAndGamescreen},
        {"stateoutput_generating_strips_suffix", &StateOutputGeneratingStripsSuffix},
        {"stateoutput_passes_through_canonical", &StateOutputPassesThroughCanonical},
        {"stateoutput_trims_whitespace", &StateOutputTrimsWhitespace},
        {"stateoutput_unknown_returns_nullopt", &StateOutputUnknownReturnsNullopt},
        {"hermes_inworld_grabbed_when_no_screen", &HermesInworldGrabbedWhenNoScreen},
        {"hermes_inworld_free_when_screen", &HermesInworldFreeWhenScreen},
        {"hermes_title_when_no_world", &HermesTitleWhenNoWorld},
        {"hermes_loading_screen_generating", &HermesLoadingScreenGenerating},
        {"hermes_seedqueue_wall", &HermesSeedQueueWall},
        {"hermes_invalid_json_nullopt", &HermesInvalidJsonNullopt},
        {"select_prefers_hermes", &SelectPrefersHermes},
        {"select_falls_back_to_stateoutput", &SelectFallsBackToStateOutput},
        {"select_none", &SelectNone},
        {"alive_fresh_same_pid_true", &AliveFreshSamePidTrue},
        {"alive_wrong_pid_false", &AliveWrongPidFalse},
        {"alive_stale_heartbeat_false", &AliveStaleHeartbeatFalse},
        {"alive_future_skew_within_threshold_true", &AliveFutureSkewWithinThresholdTrue},
        {"alive_corrupt_heartbeat_false", &AliveCorruptHeartbeatFalse},
        {"alive_negative_current_time_false", &AliveNegativeCurrentTimeFalse},
    };
    return cases;
}

int RunNamed(const std::string& name) {
    for (const auto& testCase : Registry()) {
        if (name == testCase.name) {
            g_failures = 0;
            std::cout << "RUN " << name << '\n';
            testCase.run();
            if (g_failures == 0) {
                std::cout << "PASS " << name << '\n';
                return 0;
            }
            std::cerr << "FAIL " << name << " (" << g_failures << " assertion(s))\n";
            return 1;
        }
    }
    std::cerr << "Unknown test case: " << name << '\n';
    return 2;
}

int RunAll() {
    int failed = 0;
    for (const auto& testCase : Registry()) {
        if (RunNamed(testCase.name) != 0) ++failed;
    }
    return failed == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 1 || (argc == 2 && std::strcmp(argv[1], "--run-all") == 0)) {
        return RunAll();
    }
    if (argc == 2 && std::strcmp(argv[1], "--list") == 0) {
        for (const auto& testCase : Registry()) std::cout << testCase.name << '\n';
        return 0;
    }
    if (argc == 3 && std::strcmp(argv[1], "--run") == 0) {
        return RunNamed(argv[2]);
    }
    std::cerr << "Usage: " << argv[0] << " [--run <case> | --run-all | --list]\n";
    return 2;
}
