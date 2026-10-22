#include "base64.h"

#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool condition, const char* label) {
    if (!condition) {
        std::cerr << "  ASSERT FAILED: " << label << '\n';
        ++g_failures;
    }
}

void RoundTripsTextAndBinary() {
    for (const std::string input : { std::string(), std::string("f"), std::string("fo"), std::string("foo"),
                                     std::string("hello world"), std::string("\0\x80\xff", 3) }) {
        const std::string encoded = macaron::Base64::Encode(input);
        std::string decoded;
        Check(macaron::Base64::Decode(encoded, decoded).empty(), "encoded value decodes");
        Check(decoded == input, "decoded value matches input");
    }
}

void RejectsInvalidInputSafely() {
    std::string decoded = "unchanged";
    Check(!macaron::Base64::Decode("abc", decoded).empty(), "non-multiple-of-four length rejected");
    Check(!macaron::Base64::Decode("AA=A", decoded).empty(), "interior padding rejected");
    Check(!macaron::Base64::Decode(std::string("AAA\xff", 4), decoded).empty(), "non-ASCII byte rejected");
}

struct TestCase { const char* name; std::function<void()> run; };

const std::vector<TestCase>& Registry() {
    static const std::vector<TestCase> cases = {
        {"round_trips_text_and_binary", &RoundTripsTextAndBinary},
        {"rejects_invalid_input_safely", &RejectsInvalidInputSafely},
    };
    return cases;
}

int RunNamed(const std::string& name) {
    for (const auto& test : Registry()) {
        if (name != test.name) continue;
        g_failures = 0;
        test.run();
        return g_failures == 0 ? 0 : 1;
    }
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--list") == 0) {
        for (const auto& test : Registry()) std::cout << test.name << '\n';
        return 0;
    }
    if (argc == 3 && std::strcmp(argv[1], "--run") == 0) return RunNamed(argv[2]);
    return 2;
}
