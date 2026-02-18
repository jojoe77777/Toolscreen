#include "version.h"
#include "utils.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <windows.h>

std::string GetToolscreenVersionString() { return TOOLSCREEN_VERSION_STRING; }

int GetConfigVersion() { return CONFIG_VERSION; }

std::string GetFullVersionInfo() {
    std::ostringstream oss;
    oss << "Toolscreen v" << TOOLSCREEN_VERSION_STRING;
    return oss.str();
}

void LogVersionInfo() {
    std::string versionInfo = GetFullVersionInfo();
    Log("=== " + versionInfo + " ===");
    Log("Toolscreen Version: " + GetToolscreenVersionString());
    Log("Config Version: " + std::to_string(GetConfigVersion()));
}

void PrintVersionToStdout() {
    std::string versionInfo = GetFullVersionInfo();
    std::cout << versionInfo << std::endl;
    std::cout.flush();
}

GameVersion ParseMinecraftVersionFromMMCPack(const std::wstring& mmcPackPath) {
    GameVersion result;

    try {
        // Convert wide string to string for ifstream
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, mmcPackPath.c_str(), (int)mmcPackPath.length(), NULL, 0, NULL, NULL);
        std::string mmcPackPathStr(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, mmcPackPath.c_str(), (int)mmcPackPath.length(), &mmcPackPathStr[0], size_needed, NULL, NULL);

        std::ifstream file(mmcPackPathStr);
        if (!file.is_open()) {
            Log("Failed to open mmc-pack.json at: " + mmcPackPathStr);
            return result;
        }

        nlohmann::json jsonData = nlohmann::json::parse(file);
        file.close();

        // Look for the component with "cachedName": "Minecraft"
        if (jsonData.contains("components") && jsonData["components"].is_array()) {
            for (const auto& component : jsonData["components"]) {
                if (component.contains("cachedName") && component["cachedName"].is_string()) {
                    if (component["cachedName"].get<std::string>() == "Minecraft") {
                        if (component.contains("version") && component["version"].is_string()) {
                            std::string versionStr = component["version"].get<std::string>();

                            // Parse the version string (e.g., "1.16.1")
                            std::regex versionRegex(R"((\d+)\.(\d+)(?:\.(\d+))?)");
                            std::smatch match;

                            if (std::regex_search(versionStr, match, versionRegex)) {
                                try {
                                    result.major = std::stoi(match[1].str());
                                    result.minor = std::stoi(match[2].str());
                                    result.patch = (match.size() > 3 && match[3].matched) ? std::stoi(match[3].str()) : 0;
                                    result.valid = true;

                                    std::ostringstream oss;
                                    oss << "Detected Minecraft version from mmc-pack.json: " << result.major << "." << result.minor << "."
                                        << result.patch;
                                    Log(oss.str());
                                    return result;
                                } catch (const std::exception& e) {
                                    Log(std::string("Failed to parse version from mmc-pack.json: ") + e.what());
                                }
                            }
                        }
                    }
                }
            }
        }

        Log("Could not find Minecraft component in mmc-pack.json");
    } catch (const std::exception& e) { Log(std::string("Error parsing mmc-pack.json: ") + e.what()); }

    return result;
}

GameVersion GetGameVersionFromCommandLine() {
    GameVersion result;

    // First, check if INST_MC_DIR environment variable is set
    char instMcDir[MAX_PATH] = { 0 };
    size_t len = 0;
    if (getenv_s(&len, instMcDir, sizeof(instMcDir), "INST_MC_DIR") == 0 && len > 0) {
        Log(std::string("INST_MC_DIR environment variable found: ") + instMcDir);

        // Convert to wide string and get parent directory
        int size_needed = MultiByteToWideChar(CP_UTF8, 0, instMcDir, -1, NULL, 0);
        std::wstring instMcDirW(size_needed - 1, 0);
        MultiByteToWideChar(CP_UTF8, 0, instMcDir, -1, &instMcDirW[0], size_needed);

        // Get parent directory
        std::filesystem::path instPath(instMcDirW);
        std::filesystem::path parentPath = instPath.parent_path();
        std::filesystem::path mmcPackPath = parentPath / L"mmc-pack.json";

        if (std::filesystem::exists(mmcPackPath)) {
            Log(L"Found mmc-pack.json at: " + mmcPackPath.wstring());
            result = ParseMinecraftVersionFromMMCPack(mmcPackPath.wstring());
            if (result.valid) { return result; }
            Log("Failed to parse version from mmc-pack.json, falling back to command line");
        } else {
            Log(L"mmc-pack.json not found at: " + mmcPackPath.wstring());
        }
    }

    // Get the command line for the current process
    LPWSTR cmdLine = GetCommandLineW();
    if (!cmdLine) {
        Log("Failed to get command line");
        return result;
    }

    std::wstring cmdLineStr(cmdLine);
    Log(L"Command line: " + cmdLineStr);

    // Look for --version flag followed by version number
    // Example: --version 1.16.1 or --version=1.16.1
    std::wregex versionRegex(L"--version[=\\s]+(\\d+)\\.(\\d+)(?:\\.(\\d+))?");
    std::wsmatch match;

    if (std::regex_search(cmdLineStr, match, versionRegex)) {
        try {
            result.major = std::stoi(match[1].str());
            result.minor = std::stoi(match[2].str());
            // Patch version is optional, default to 0 if not present
            result.patch = (match.size() > 3 && match[3].matched) ? std::stoi(match[3].str()) : 0;
            result.valid = true;

            std::ostringstream oss;
            oss << "Detected game version: " << result.major << "." << result.minor << "." << result.patch;
            Log(oss.str());
        } catch (const std::exception& e) { Log(std::string("Failed to parse version numbers: ") + e.what()); }
    } else {
        Log("No --version flag found in command line");
    }

    return result;
}

bool IsVersionInRange(const GameVersion& version, const GameVersion& minVer, const GameVersion& maxVer) {
    if (!version.valid) return false;
    return version >= minVer && version <= maxVer;
}

bool IsResolutionChangeSupported(const GameVersion& version) {
    // Resolution changing (modes and hotkeys) is only supported for Minecraft 1.13+
    // For versions below 1.13, we disable these features but keep overlays and cursors working
    if (!version.valid) {
        // If no version is detected, assume it's supported for backward compatibility
        return true;
    }

    // Check if version >= 1.13.0
    GameVersion minSupportedVersion(1, 13, 0);
    return version >= minSupportedVersion;
}