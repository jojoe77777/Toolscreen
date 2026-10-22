#include "expression_parser.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

enum class ExprTokenKind {
    Number,
    Identifier,
    Plus,
    Minus,
    Star,
    Slash,
    LParen,
    RParen,
    Comma,
    End,
    Invalid
};

struct ExprToken {
    ExprTokenKind kind = ExprTokenKind::End;
    std::string text;
    double numValue = 0.0;

    ExprToken() = default;
    ExprToken(ExprTokenKind tokenKind, std::string tokenText, double tokenValue)
        : kind(tokenKind), text(std::move(tokenText)), numValue(tokenValue) {}
};

class Tokenizer {
public:
    explicit Tokenizer(const std::string& input) : m_input(input) {}

    ExprToken next() {
        skipWhitespace();
        if (m_pos >= m_input.size()) {
            return ExprToken(ExprTokenKind::End, "", 0.0);
        }

        const char c = m_input[m_pos];
        if (c == '+') {
            ++m_pos;
            return ExprToken(ExprTokenKind::Plus, "+", 0.0);
        }
        if (c == '-') {
            ++m_pos;
            return ExprToken(ExprTokenKind::Minus, "-", 0.0);
        }
        if (c == '*') {
            ++m_pos;
            return ExprToken(ExprTokenKind::Star, "*", 0.0);
        }
        if (c == '/') {
            ++m_pos;
            return ExprToken(ExprTokenKind::Slash, "/", 0.0);
        }
        if (c == '(') {
            ++m_pos;
            return ExprToken(ExprTokenKind::LParen, "(", 0.0);
        }
        if (c == ')') {
            ++m_pos;
            return ExprToken(ExprTokenKind::RParen, ")", 0.0);
        }
        if (c == ',') {
            ++m_pos;
            return ExprToken(ExprTokenKind::Comma, ",", 0.0);
        }

        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
            const size_t start = m_pos;
            bool hasDecimal = false;
            while (m_pos < m_input.size()) {
                const char current = m_input[m_pos];
                if (!std::isdigit(static_cast<unsigned char>(current)) && current != '.') {
                    break;
                }
                if (current == '.') {
                    if (hasDecimal) {
                        break;
                    }
                    hasDecimal = true;
                }
                ++m_pos;
            }
            const std::string numStr = m_input.substr(start, m_pos - start);
            return ExprToken(ExprTokenKind::Number, numStr, std::stod(numStr));
        }

        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            const size_t start = m_pos;
            while (m_pos < m_input.size()) {
                const char current = m_input[m_pos];
                if (!std::isalnum(static_cast<unsigned char>(current)) && current != '_') {
                    break;
                }
                ++m_pos;
            }
            return ExprToken(ExprTokenKind::Identifier, m_input.substr(start, m_pos - start), 0.0);
        }

        ++m_pos;
        return ExprToken(ExprTokenKind::Invalid, std::string(1, c), 0.0);
    }

private:
    void skipWhitespace() {
        while (m_pos < m_input.size() && std::isspace(static_cast<unsigned char>(m_input[m_pos]))) {
            ++m_pos;
        }
    }

    std::string m_input;
    size_t m_pos = 0;
};

class ExpressionParser {
public:
    ExpressionParser(const std::string& expr, int screenWidth, int screenHeight)
        : m_tokenizer(expr), m_screenWidth(screenWidth), m_screenHeight(screenHeight) {
        m_currentToken = m_tokenizer.next();
    }

    double parse() {
        const double result = parseExpression();
        if (m_currentToken.kind != ExprTokenKind::End) {
            throw std::runtime_error("Unexpected token at end: " + m_currentToken.text);
        }
        if (!std::isfinite(result)) {
            throw std::runtime_error("Expression result must be finite");
        }
        return result;
    }

    std::string validate() {
        try {
            (void)parse();
            return "";
        } catch (const std::exception& ex) {
            return ex.what();
        }
    }

private:
    double parseExpression() {
        double left = parseTerm();
        while (m_currentToken.kind == ExprTokenKind::Plus || m_currentToken.kind == ExprTokenKind::Minus) {
            const ExprTokenKind op = m_currentToken.kind;
            advance();
            const double right = parseTerm();
            left = (op == ExprTokenKind::Plus) ? (left + right) : (left - right);
        }
        return left;
    }

    double parseTerm() {
        double left = parseUnary();
        while (m_currentToken.kind == ExprTokenKind::Star || m_currentToken.kind == ExprTokenKind::Slash) {
            const ExprTokenKind op = m_currentToken.kind;
            advance();
            const double right = parseUnary();
            if (op == ExprTokenKind::Slash) {
                if (right == 0.0) {
                    throw std::runtime_error("Division by zero");
                }
                left /= right;
            } else {
                left *= right;
            }
        }
        return left;
    }

    double parseUnary() {
        if (m_currentToken.kind == ExprTokenKind::Minus) {
            advance();
            return -parseUnary();
        }
        if (m_currentToken.kind == ExprTokenKind::Plus) {
            advance();
            return parseUnary();
        }
        return parsePrimary();
    }

    double parsePrimary() {
        if (m_currentToken.kind == ExprTokenKind::Number) {
            const double value = m_currentToken.numValue;
            advance();
            return value;
        }

        if (m_currentToken.kind == ExprTokenKind::Identifier) {
            const std::string identifier = m_currentToken.text;
            advance();
            if (m_currentToken.kind == ExprTokenKind::LParen) {
                return parseFunctionCall(identifier);
            }
            return lookupVariable(identifier);
        }

        if (m_currentToken.kind == ExprTokenKind::LParen) {
            advance();
            const double value = parseExpression();
            expect(ExprTokenKind::RParen, "Expected ')'");
            return value;
        }

        throw std::runtime_error("Unexpected token: " + m_currentToken.text);
    }

    double parseFunctionCall(const std::string& functionName) {
        expect(ExprTokenKind::LParen, "Expected '(' after function name");

        std::vector<double> args;
        if (m_currentToken.kind != ExprTokenKind::RParen) {
            args.push_back(parseExpression());
            while (m_currentToken.kind == ExprTokenKind::Comma) {
                advance();
                args.push_back(parseExpression());
            }
        }

        expect(ExprTokenKind::RParen, "Expected ')' after function arguments");
        return callFunction(functionName, args);
    }

    double lookupVariable(const std::string& name) const {
        if (name == "screenWidth") {
            return static_cast<double>(m_screenWidth);
        }
        if (name == "screenHeight") {
            return static_cast<double>(m_screenHeight);
        }
        throw std::runtime_error("Unknown variable: " + name);
    }

    static double callFunction(const std::string& name, const std::vector<double>& args) {
        if (name == "min") {
            if (args.size() != 2) {
                throw std::runtime_error("min() requires 2 arguments");
            }
            return (std::min)(args[0], args[1]);
        }
        if (name == "max") {
            if (args.size() != 2) {
                throw std::runtime_error("max() requires 2 arguments");
            }
            return (std::max)(args[0], args[1]);
        }
        if (name == "floor") {
            if (args.size() != 1) {
                throw std::runtime_error("floor() requires 1 argument");
            }
            return std::floor(args[0]);
        }
        if (name == "ceil") {
            if (args.size() != 1) {
                throw std::runtime_error("ceil() requires 1 argument");
            }
            return std::ceil(args[0]);
        }
        if (name == "round") {
            if (args.size() != 1) {
                throw std::runtime_error("round() requires 1 argument");
            }
            return std::round(args[0]);
        }
        if (name == "abs") {
            if (args.size() != 1) {
                throw std::runtime_error("abs() requires 1 argument");
            }
            return std::abs(args[0]);
        }
        if (name == "roundEven") {
            if (args.size() != 1) {
                throw std::runtime_error("roundEven() requires 1 argument");
            }
            return std::ceil(args[0] / 2.0) * 2.0;
        }

        throw std::runtime_error("Unknown function: " + name);
    }

    void advance() {
        m_currentToken = m_tokenizer.next();
    }

    void expect(ExprTokenKind expected, const std::string& error) {
        if (m_currentToken.kind != expected) {
            throw std::runtime_error(error);
        }
        advance();
    }

    Tokenizer m_tokenizer;
    ExprToken m_currentToken;
    int m_screenWidth;
    int m_screenHeight;
};

std::string TrimExpressionString(const std::string& value) {
    const size_t start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "";
    }
    const size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

} // namespace

int EvaluateExpression(const std::string& expr, int screenWidth, int screenHeight, int defaultValue) {
    const std::string trimmed = TrimExpressionString(expr);
    if (trimmed.empty()) {
        return defaultValue;
    }

    try {
        ExpressionParser parser(trimmed, screenWidth, screenHeight);
        const double result = parser.parse();
        const double floored = std::floor(result);
        if (floored < static_cast<double>((std::numeric_limits<int>::min)()) ||
            floored > static_cast<double>((std::numeric_limits<int>::max)())) {
            return defaultValue;
        }
        return static_cast<int>(floored);
    } catch (const std::exception&) {
        return defaultValue;
    }
}

bool IsExpression(const std::string& str) {
    const std::string trimmed = TrimExpressionString(str);
    if (trimmed.empty()) {
        return false;
    }

    size_t checkStart = 0;
    if (trimmed[0] == '-') {
        checkStart = 1;
    }
    if (checkStart >= trimmed.size()) {
        return true;
    }

    for (size_t i = checkStart; i < trimmed.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(trimmed[i]))) {
            return true;
        }
    }
    return false;
}

bool ValidateExpression(const std::string& expr, std::string& errorOut) {
    const std::string trimmed = TrimExpressionString(expr);
    if (trimmed.empty()) {
        errorOut = "Expression cannot be empty";
        return false;
    }

    try {
        ExpressionParser parser(trimmed, 1920, 1080);
        const std::string error = parser.validate();
        if (!error.empty()) {
            errorOut = error;
            return false;
        }
    } catch (const std::exception& ex) {
        // Tokenization can fail before ExpressionParser::validate() starts
        // (for example, std::stod(".") or an out-of-range numeric literal).
        errorOut = ex.what();
        return false;
    }

    errorOut.clear();
    return true;
}
