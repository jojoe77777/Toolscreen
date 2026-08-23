#include "common/expression_parser.h"

#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& label) {
    if (!condition) {
        std::cerr << "  ASSERT FAILED: " << label << '\n';
        ++g_failures;
    }
}

void ArithmeticAndVariables() {
    Check(EvaluateExpression("screenWidth / 2 + 10", 1920, 1080, -1) == 970, "variables and precedence");
    Check(EvaluateExpression("min(screenWidth, screenHeight)", 1920, 1080, -1) == 1080, "function call");
    Check(EvaluateExpression("-1.2", 1, 1, 0) == -2, "result is floored");
}

void MalformedNumbersDoNotThrow() {
    std::string error;
    bool valid = true;
    try {
        valid = ValidateExpression(".", error);
    } catch (...) {
        Check(false, "dot-only validation must not throw");
        return;
    }
    Check(!valid, "dot-only number is invalid");
    Check(!error.empty(), "dot-only number reports an error");

    const std::string hugeLiteral(10000, '9');
    try {
        valid = ValidateExpression(hugeLiteral, error);
    } catch (...) {
        Check(false, "out-of-range literal validation must not throw");
        return;
    }
    Check(!valid, "out-of-range literal is invalid");
    Check(EvaluateExpression(hugeLiteral, 1, 1, 73) == 73, "out-of-range literal uses fallback");
}

void NonFiniteAndIntegerOverflowUseFallback() {
    Check(EvaluateExpression("99999999999999999999 * 99999999999999999999", 1, 1, 41) == 41,
          "positive integer overflow uses fallback");
    Check(EvaluateExpression("-99999999999999999999 * 99999999999999999999", 1, 1, 42) == 42,
          "negative integer overflow uses fallback");

    std::string error;
    const std::string largeFiniteLiteral(200, '9');
    Check(!ValidateExpression(largeFiniteLiteral + " * " + largeFiniteLiteral, error),
          "non-finite result is invalid");
}

struct TestCase { const char* name; std::function<void()> run; };

const std::vector<TestCase>& Registry() {
    static const std::vector<TestCase> cases = {
        {"arithmetic_and_variables", &ArithmeticAndVariables},
        {"malformed_numbers_do_not_throw", &MalformedNumbersDoNotThrow},
        {"non_finite_and_integer_overflow_use_fallback", &NonFiniteAndIntegerOverflowUseFallback},
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
    std::cerr << "Unknown test case: " << name << '\n';
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--list") == 0) {
        for (const auto& test : Registry()) std::cout << test.name << '\n';
        return 0;
    }
    if (argc == 3 && std::strcmp(argv[1], "--run") == 0) return RunNamed(argv[2]);
    std::cerr << "Usage: " << argv[0] << " --run <case> | --list\n";
    return 2;
}
