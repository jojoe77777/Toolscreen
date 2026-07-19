#pragma once

#include <string>
#include <format>
#include <utility>

void Log(const std::string& message);
void Log(const std::wstring& message);

template <typename... Args> requires (sizeof...(Args) > 0)
void Log(const std::format_string<Args...>& fmt, Args&&... args) {
    Log(std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args> requires (sizeof...(Args) > 0)
void Log(const std::wformat_string<Args...>& fmt, Args&&... args) {
    Log(std::format(fmt, std::forward<Args>(args)...));
}
