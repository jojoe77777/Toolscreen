#include "config_toml.h"
#include "config_defaults.h"
#include "common/expression_parser.h"
#include "gui/gui.h"
#include "runtime/logic_thread.h"
#include "common/utils.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <unordered_set>

template <typename T> T GetOr(const toml::table& tbl, const std::string& key, T defaultValue) {
    if (auto node = tbl.get(key)) {
        if (auto val = node->value<T>()) { return *val; }
    }
    return defaultValue;
}

std::string GetStringOr(const toml::table& tbl, const std::string& key, const std::string& defaultValue) {
    if (auto node = tbl.get(key)) {
        if (auto val = node->value<std::string>()) { return *val; }
    }
    return defaultValue;
}

const toml::table* GetTable(const toml::table& tbl, const std::string& key) {
    if (auto node = tbl.get(key)) { return node->as_table(); }
    return nullptr;
}

const toml::array* GetArray(const toml::table& tbl, const std::string& key) {
    if (auto node = tbl.get(key)) { return node->as_array(); }
    return nullptr;
}

std::optional<double> GetNumericValue(const toml::node* node) {
    if (!node) {
        return std::nullopt;
    }

    if (node->is_integer()) {
        return static_cast<double>(node->as_integer()->get());
    }

    if (node->is_floating_point()) {
        return node->as_floating_point()->get();
    }

    return std::nullopt;
}

std::optional<double> GetNumericValue(const toml::table& tbl, const std::string& key) {
    return GetNumericValue(tbl.get(key));
}

bool IsZeroOrOneValue(double value) {
    return value == 0.0 || value == 1.0;
}

const char* EyeZoomFontSizeModeToTomlString(EyeZoomFontSizeMode mode) {
    switch (mode) {
        case EyeZoomFontSizeMode::PerSquareAuto:
            return "per_square_auto";
        case EyeZoomFontSizeMode::Manual:
            return "manual";
        case EyeZoomFontSizeMode::Auto:
        default:
            return "auto";
    }
}

EyeZoomFontSizeMode EyeZoomFontSizeModeFromLegacyBool(bool autoFontSize) {
    return autoFontSize ? EyeZoomFontSizeMode::PerSquareAuto : EyeZoomFontSizeMode::Manual;
}

EyeZoomFontSizeMode ParseEyeZoomFontSizeMode(const toml::table& tbl) {
    if (auto modeNode = tbl.get("fontSizeMode")) {
        if (auto modeValue = modeNode->value<std::string>()) {
            std::string lowered = *modeValue;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            if (lowered == "auto") { return EyeZoomFontSizeMode::Auto; }
            if (lowered == "per_square_auto" || lowered == "per-square-auto" || lowered == "persquareauto") {
                return EyeZoomFontSizeMode::PerSquareAuto;
            }
            if (lowered == "manual") { return EyeZoomFontSizeMode::Manual; }
        }
    }

    return EyeZoomFontSizeModeFromLegacyBool(GetOr(tbl, "autoFontSize", false));
}

bool IsLiteralZeroOrOneString(const std::string& value) {
    const size_t start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return false;
    }

    const size_t end = value.find_last_not_of(" \t\r\n");
    const std::string trimmed = value.substr(start, end - start + 1);

    char* parseEnd = nullptr;
    const double parsed = std::strtod(trimmed.c_str(), &parseEnd);
    if (!parseEnd || *parseEnd != '\0') {
        return false;
    }

    return IsZeroOrOneValue(parsed);
}

bool ShouldRestoreModeDimensionFromDefault(const toml::table& tbl, const char* key) {
    const toml::node* node = tbl.get(key);
    if (!node) {
        return false;
    }

    if (auto numericValue = GetNumericValue(node)) {
        return IsZeroOrOneValue(*numericValue);
    }

    if (auto stringValue = node->value<std::string>()) {
        return IsLiteralZeroOrOneString(*stringValue);
    }

    return false;
}

const ModeConfig* FindDefaultModeById(const std::vector<ModeConfig>& defaultModes, const std::string& modeId) {
    for (const auto& defaultMode : defaultModes) {
        if (EqualsIgnoreCase(defaultMode.id, modeId)) {
            return &defaultMode;
        }
    }

    return nullptr;
}

void ApplyDefaultModeWidth(const ModeConfig& defaultMode, ModeConfig& mode) {
    mode.width = defaultMode.width;
    mode.manualWidth = defaultMode.manualWidth;
    mode.relativeWidth = defaultMode.relativeWidth;
    mode.widthExpr = defaultMode.widthExpr;
}

void ApplyDefaultModeHeight(const ModeConfig& defaultMode, ModeConfig& mode) {
    mode.height = defaultMode.height;
    mode.manualHeight = defaultMode.manualHeight;
    mode.relativeHeight = defaultMode.relativeHeight;
    mode.heightExpr = defaultMode.heightExpr;
}

void RestoreModeDimensionsFromDefaults(const toml::table& tbl, const std::vector<ModeConfig>* defaultModes, ModeConfig& mode) {
    if (!defaultModes || defaultModes->empty() || mode.id.empty()) {
        return;
    }

    const ModeConfig* defaultMode = FindDefaultModeById(*defaultModes, mode.id);
    if (!defaultMode) {
        return;
    }

    bool restoredWidth = false;
    bool restoredHeight = false;

    if (ShouldRestoreModeDimensionFromDefault(tbl, "width")) {
        ApplyDefaultModeWidth(*defaultMode, mode);
        restoredWidth = true;
    }

    if (ShouldRestoreModeDimensionFromDefault(tbl, "height")) {
        ApplyDefaultModeHeight(*defaultMode, mode);
        restoredHeight = true;
    }

    if (restoredWidth || restoredHeight) {
        std::string repairedFields;
        if (restoredWidth) {
            repairedFields = "width";
        }
        if (restoredHeight) {
            if (!repairedFields.empty()) {
                repairedFields += ", ";
            }
            repairedFields += "height";
        }

        Log("[CONFIG] Restored mode '" + mode.id + "' " + repairedFields + " from embedded defaults because config.toml contained " +
            (restoredWidth && restoredHeight ? "0/1 values for both dimensions." : "a 0/1 value."));
    }
}

void ModeConfigFromTomlInternal(const toml::table& tbl, ModeConfig& cfg, const std::vector<ModeConfig>* defaultModes) {
    cfg.id = GetStringOr(tbl, "id", "");

    cfg.width = ConfigDefaults::MODE_WIDTH;
    cfg.height = ConfigDefaults::MODE_HEIGHT;

    cfg.useRelativeSize = false;
    cfg.relativeWidth = -1.0f;
    cfg.relativeHeight = -1.0f;
    cfg.widthExpr.clear();
    cfg.heightExpr.clear();

    bool widthIsPercentage = false;
    bool heightIsPercentage = false;

    int cachedScreenWidth = GetCachedWindowWidth();
    int cachedScreenHeight = GetCachedWindowHeight();

    if (auto widthNode = tbl.get("width")) {
        if (auto widthStr = widthNode->value<std::string>()) {
            cfg.widthExpr = *widthStr;
        } else if (auto widthVal = GetNumericValue(widthNode)) {
            if (*widthVal >= 0.0 && *widthVal <= 1.0) {
                cfg.relativeWidth = static_cast<float>(*widthVal);
                widthIsPercentage = true;

                if (cachedScreenWidth > 0) {
                    cfg.width = (std::max)(1, static_cast<int>(std::lround(cfg.relativeWidth * static_cast<float>(cachedScreenWidth))));
                }
            } else {
                cfg.width = static_cast<int>(*widthVal);
            }
        }
    }

    if (auto heightNode = tbl.get("height")) {
        if (auto heightStr = heightNode->value<std::string>()) {
            cfg.heightExpr = *heightStr;
        } else if (auto heightVal = GetNumericValue(heightNode)) {
            if (*heightVal >= 0.0 && *heightVal <= 1.0) {
                cfg.relativeHeight = static_cast<float>(*heightVal);
                heightIsPercentage = true;

                if (cachedScreenHeight > 0) {
                    cfg.height = (std::max)(1, static_cast<int>(std::lround(cfg.relativeHeight * static_cast<float>(cachedScreenHeight))));
                }
            } else {
                cfg.height = static_cast<int>(*heightVal);
            }
        }
    }

    if (cfg.widthExpr.empty()) { cfg.widthExpr = GetStringOr(tbl, "widthExpr", ""); }
    if (cfg.heightExpr.empty()) { cfg.heightExpr = GetStringOr(tbl, "heightExpr", ""); }

    if (tbl.contains("useRelativeSize") || tbl.contains("relativeWidth") || tbl.contains("relativeHeight")) {
        cfg.useRelativeSize = GetOr(tbl, "useRelativeSize", false);
        if (auto relativeWidth = GetNumericValue(tbl, "relativeWidth")) {
            cfg.relativeWidth = static_cast<float>(*relativeWidth);
        }
        if (auto relativeHeight = GetNumericValue(tbl, "relativeHeight")) {
            cfg.relativeHeight = static_cast<float>(*relativeHeight);
        }
    } else if (widthIsPercentage || heightIsPercentage) {
        cfg.useRelativeSize = true;
    }

    if (!cfg.widthExpr.empty()) { cfg.relativeWidth = -1.0f; }
    if (!cfg.heightExpr.empty()) { cfg.relativeHeight = -1.0f; }

    RestoreModeDimensionsFromDefaults(tbl, defaultModes, cfg);

    const bool hasRelativeWidth = cfg.relativeWidth >= 0.0f && cfg.relativeWidth <= 1.0f;
    const bool hasRelativeHeight = cfg.relativeHeight >= 0.0f && cfg.relativeHeight <= 1.0f;
    if (hasRelativeWidth || hasRelativeHeight) {
        cfg.useRelativeSize = true;
    }

    if (hasRelativeWidth && cfg.width < 1 && cachedScreenWidth > 0) {
        cfg.width = (std::max)(1, static_cast<int>(std::lround(cfg.relativeWidth * static_cast<float>(cachedScreenWidth))));
    }
    if (hasRelativeHeight && cfg.height < 1 && cachedScreenHeight > 0) {
        cfg.height = (std::max)(1, static_cast<int>(std::lround(cfg.relativeHeight * static_cast<float>(cachedScreenHeight))));
    }

    if (!cfg.widthExpr.empty() && cachedScreenWidth > 0 && cachedScreenHeight > 0) {
        cfg.width = (std::max)(1, EvaluateExpression(cfg.widthExpr, cachedScreenWidth, cachedScreenHeight, cfg.width));
    }
    if (!cfg.heightExpr.empty() && cachedScreenWidth > 0 && cachedScreenHeight > 0) {
        cfg.height = (std::max)(1, EvaluateExpression(cfg.heightExpr, cachedScreenWidth, cachedScreenHeight, cfg.height));
    }

    cfg.manualWidth = (cfg.width > 0) ? cfg.width : ConfigDefaults::MODE_WIDTH;
    cfg.manualHeight = (cfg.height > 0) ? cfg.height : ConfigDefaults::MODE_HEIGHT;

    // Note: Actual pixel conversion from percentages is done elsewhere (GUI/logic thread)

    if (auto t = GetTable(tbl, "background")) { BackgroundConfigFromToml(*t, cfg.background); }

    cfg.mirrorIds.clear();
    if (auto arr = GetArray(tbl, "mirrorIds")) {
        for (const auto& elem : *arr) {
            if (auto val = elem.value<std::string>()) { cfg.mirrorIds.push_back(*val); }
        }
    }

    cfg.mirrorGroupIds.clear();
    if (auto arr = GetArray(tbl, "mirrorGroupIds")) {
        for (const auto& elem : *arr) {
            if (auto val = elem.value<std::string>()) { cfg.mirrorGroupIds.push_back(*val); }
        }
    }

    cfg.imageIds.clear();
    if (auto arr = GetArray(tbl, "imageIds")) {
        for (const auto& elem : *arr) {
            if (auto val = elem.value<std::string>()) { cfg.imageIds.push_back(*val); }
        }
    }

    cfg.windowOverlayIds.clear();
    if (auto arr = GetArray(tbl, "windowOverlayIds")) {
        for (const auto& elem : *arr) {
            if (auto val = elem.value<std::string>()) { cfg.windowOverlayIds.push_back(*val); }
        }
    }

    cfg.browserOverlayIds.clear();
    if (auto arr = GetArray(tbl, "browserOverlayIds")) {
        for (const auto& elem : *arr) {
            if (auto val = elem.value<std::string>()) { cfg.browserOverlayIds.push_back(*val); }
        }
    }

    if (auto t = GetTable(tbl, "stretch")) { StretchConfigFromToml(*t, cfg.stretch); }

    const toml::table* transitionTbl = GetTable(tbl, "transition");
    const toml::table& transitionSrc = transitionTbl ? *transitionTbl : tbl;

    cfg.gameTransition = StringToGameTransitionType(GetStringOr(transitionSrc, "gameTransition", ConfigDefaults::GAME_TRANSITION_CUT));
    cfg.overlayTransition =
        StringToOverlayTransitionType(GetStringOr(transitionSrc, "overlayTransition", ConfigDefaults::OVERLAY_TRANSITION_CUT));
    cfg.backgroundTransition =
        StringToBackgroundTransitionType(GetStringOr(transitionSrc, "backgroundTransition", ConfigDefaults::BACKGROUND_TRANSITION_CUT));
    cfg.transitionDurationMs = GetOr(transitionSrc, "transitionDurationMs", ConfigDefaults::MODE_TRANSITION_DURATION_MS);

    cfg.easeInPower = GetOr(transitionSrc, "easeInPower", ConfigDefaults::MODE_EASE_IN_POWER);
    cfg.easeOutPower = GetOr(transitionSrc, "easeOutPower", ConfigDefaults::MODE_EASE_OUT_POWER);
    cfg.bounceCount = GetOr(transitionSrc, "bounceCount", ConfigDefaults::MODE_BOUNCE_COUNT);
    cfg.bounceIntensity = GetOr(transitionSrc, "bounceIntensity", ConfigDefaults::MODE_BOUNCE_INTENSITY);
    cfg.bounceDurationMs = GetOr(transitionSrc, "bounceDurationMs", ConfigDefaults::MODE_BOUNCE_DURATION_MS);
    cfg.relativeStretching = GetOr(transitionSrc, "relativeStretching", ConfigDefaults::MODE_RELATIVE_STRETCHING);
    cfg.skipAnimateX = GetOr(transitionSrc, "skipAnimateX", false);
    cfg.skipAnimateY = GetOr(transitionSrc, "skipAnimateY", false);

    if (auto t = GetTable(tbl, "border")) { BorderConfigFromToml(*t, cfg.border); }

    cfg.sensitivityOverrideEnabled = GetOr(tbl, "sensitivityOverrideEnabled", ConfigDefaults::MODE_SENSITIVITY_OVERRIDE_ENABLED);
    cfg.modeSensitivity = GetOr(tbl, "modeSensitivity", ConfigDefaults::MODE_SENSITIVITY);
    cfg.separateXYSensitivity = GetOr(tbl, "separateXYSensitivity", ConfigDefaults::MODE_SEPARATE_XY_SENSITIVITY);
    cfg.modeSensitivityX = GetOr(tbl, "modeSensitivityX", ConfigDefaults::MODE_SENSITIVITY_X);
    cfg.modeSensitivityY = GetOr(tbl, "modeSensitivityY", ConfigDefaults::MODE_SENSITIVITY_Y);

    cfg.slideMirrorsIn = GetOr(transitionSrc, "slideMirrorsIn", false);
}

static int ClampMirrorCaptureDimension(int value) {
    return std::clamp(value, ConfigDefaults::MIRROR_CAPTURE_MIN_DIMENSION, ConfigDefaults::MIRROR_CAPTURE_MAX_DIMENSION);
}

static void WriteInlineTable(std::ostream& out, const toml::table& tbl, int depth = 0) {
    if (depth > 8) { out << "{ }"; return; }
    out << "{ ";
    bool first = true;
    for (const auto& [k, v] : tbl) {
        if (!first) out << ", ";
        first = false;
        out << k.str() << " = ";
        if (v.is_table()) {
            WriteInlineTable(out, *v.as_table(), depth + 1);
        } else if (v.is_array()) {
            out << *v.as_array();
        } else {
            v.visit([&out](auto&& val) { out << val; });
        }
    }
    out << " }";
}

static void WriteNode(std::ostream& out, const std::string& key, const toml::node& node, bool insideArrayElement) {
    if (node.is_table()) {
        const toml::table* subtbl = node.as_table();
        if (!subtbl) return;
        if (insideArrayElement) {
            out << key << " = ";
            WriteInlineTable(out, *subtbl);
            out << "\n";
        } else {
            out << "[" << key << "]\n";
            for (const auto& [subKey, subNode] : *subtbl) {
                if (!subNode.is_table()) {
                    std::string subKeyStr(subKey.str());
                    out << subKeyStr << " = ";
                    if (subNode.is_array()) {
                        out << *subNode.as_array();
                    } else {
                        subNode.visit([&out](auto&& val) { out << val; });
                    }
                    out << "\n";
                }
            }

            for (const auto& [subKey, subNode] : *subtbl) {
                if (subNode.is_table()) {
                    WriteNode(out, key + "." + std::string(subKey.str()), subNode, false);
                }
            }
        }
    } else if (node.is_array()) {
        const toml::array* arr = node.as_array();
        if (arr) out << key << " = " << *arr << "\n";
    } else {
        out << key << " = ";
        node.visit([&out](auto&& val) { out << val; });
        out << "\n";
    }
}

void WriteTableOrdered(std::ostream& out, const toml::table& tbl, const std::vector<std::string>& orderedKeys,
                       bool insideArrayElement = false) {
    for (const auto& key : orderedKeys) {
        if (tbl.contains(key)) {
            const toml::node* nodePtr = tbl.get(key);
            if (!nodePtr) continue;
            WriteNode(out, key, *nodePtr, insideArrayElement);
        }
    }

    for (const auto& [key, node] : tbl) {
        std::string keyStr(key.str());
        if (std::find(orderedKeys.begin(), orderedKeys.end(), keyStr) == orderedKeys.end()) {
            WriteNode(out, keyStr, node, insideArrayElement);
        }
    }
}

toml::array ColorToTomlArray(const Color& color) {
    toml::array arr;
    arr.push_back(static_cast<int64_t>(std::round(color.r * 255.0f)));
    arr.push_back(static_cast<int64_t>(std::round(color.g * 255.0f)));
    arr.push_back(static_cast<int64_t>(std::round(color.b * 255.0f)));
    if (color.a < 1.0f - 0.001f) {
        arr.push_back(static_cast<int64_t>(std::round(color.a * 255.0f)));
    }
    return arr;
}

Color ColorFromTomlArray(const toml::array* arr, Color defaultColor = { 0.0f, 0.0f, 0.0f, 1.0f }) {
    Color color = defaultColor;
    if (!arr || arr->size() < 3) {
        return color;
    }

    auto readComponent01 = [&](size_t idx, float fallback01) -> float {
        if (idx >= arr->size()) {
            return fallback01;
        }

        if (auto vInt = (*arr)[idx].value<int64_t>()) {
            return static_cast<float>(*vInt) / 255.0f;
        }

        if (auto vDbl = (*arr)[idx].value<double>()) {
            const double v = *vDbl;
            if (v <= 1.0) {
                return static_cast<float>(v);
            }
            return static_cast<float>(v / 255.0);
        }

        return fallback01;
    };

    color.r = readComponent01(0, defaultColor.r);
    color.g = readComponent01(1, defaultColor.g);
    color.b = readComponent01(2, defaultColor.b);
    color.a = (arr->size() >= 4) ? readComponent01(3, defaultColor.a) : 1.0f;

    color.r = (std::max)(0.0f, (std::min)(1.0f, color.r));
    color.g = (std::max)(0.0f, (std::min)(1.0f, color.g));
    color.b = (std::max)(0.0f, (std::min)(1.0f, color.b));
    color.a = (std::max)(0.0f, (std::min)(1.0f, color.a));
    return color;
}

bool IsValidAppearanceCustomColorKey(const std::string& key) {
    static const std::unordered_set<std::string> validKeys = {
        "WindowBg",
        "ChildBg",
        "PopupBg",
        "Border",
        "Text",
        "TextDisabled",
        "FrameBg",
        "FrameBgHovered",
        "FrameBgActive",
        "TitleBg",
        "TitleBgActive",
        "TitleBgCollapsed",
        "Button",
        "ButtonHovered",
        "ButtonActive",
        "Header",
        "HeaderHovered",
        "HeaderActive",
        "Tab",
        "TabHovered",
        "TabSelected",
        "SliderGrab",
        "SliderGrabActive",
        "ScrollbarBg",
        "ScrollbarGrab",
        "ScrollbarGrabHovered",
        "ScrollbarGrabActive",
        "CheckMark",
        "TextSelectedBg",
        "Separator",
        "SeparatorHovered",
        "SeparatorActive",
        "ResizeGrip",
        "ResizeGripHovered",
        "ResizeGripActive",
    };

    return validKeys.find(key) != validKeys.end();
}

bool IsValidAppearanceCustomColorArray(const toml::array* arr) {
    return arr != nullptr && (arr->size() == 3 || arr->size() == 4);
}

bool SanitizeAppearanceCustomColorsTable(toml::table& tbl, const char* customColorsKey) {
    toml::node* colorsNode = tbl.get(customColorsKey);
    if (colorsNode == nullptr) {
        return false;
    }

    toml::table* colorsTbl = colorsNode->as_table();
    if (colorsTbl == nullptr) {
        tbl.erase(customColorsKey);
        return true;
    }

    bool changed = false;
    for (auto it = colorsTbl->begin(); it != colorsTbl->end();) {
        const std::string keyName(it->first.str());
        const toml::array* arr = it->second.as_array();
        if (!IsValidAppearanceCustomColorKey(keyName) || !IsValidAppearanceCustomColorArray(arr)) {
            it = colorsTbl->erase(it);
            changed = true;
            continue;
        }
        ++it;
    }

    if (colorsTbl->empty()) {
        tbl.erase(customColorsKey);
        changed = true;
    }

    return changed;
}

static bool SanitizeConfigAppearanceCustomColors(toml::table& tbl) {
    toml::node* appearanceNode = tbl.get("appearance");
    if (appearanceNode == nullptr) {
        return false;
    }

    toml::table* appearanceTbl = appearanceNode->as_table();
    if (appearanceTbl == nullptr) {
        return false;
    }

    return SanitizeAppearanceCustomColorsTable(*appearanceTbl, "customColors");
}


std::string GradientAnimationTypeToString(GradientAnimationType type) {
    switch (type) {
    case GradientAnimationType::Rotate:
        return "Rotate";
    case GradientAnimationType::Slide:
        return "Slide";
    case GradientAnimationType::Wave:
        return "Wave";
    case GradientAnimationType::Spiral:
        return "Spiral";
    case GradientAnimationType::Fade:
        return "Fade";
    default:
        return "None";
    }
}

GradientAnimationType StringToGradientAnimationType(const std::string& str) {
    if (str == "Rotate") return GradientAnimationType::Rotate;
    if (str == "Slide") return GradientAnimationType::Slide;
    if (str == "Wave") return GradientAnimationType::Wave;
    if (str == "Spiral") return GradientAnimationType::Spiral;
    if (str == "Fade") return GradientAnimationType::Fade;
    return GradientAnimationType::None;
}

void GradientConfigToToml(const GradientConfig& cfg, toml::table& out) {
    out.is_inline(false);

    toml::array stopsArr;
    for (const auto& stop : cfg.gradientStops) {
        toml::table stopTbl;
        stopTbl.is_inline(true);
        stopTbl.insert("color", ColorToTomlArray(stop.color));
        stopTbl.insert("position", stop.position);
        stopsArr.push_back(stopTbl);
    }
    out.insert("gradientStops", stopsArr);
    out.insert("gradientAngle", cfg.gradientAngle);
    out.insert("gradientAnimation", GradientAnimationTypeToString(cfg.gradientAnimation));
    out.insert("gradientAnimationSpeed", cfg.gradientAnimationSpeed);
    out.insert("gradientColorFade", cfg.gradientColorFade);
}

void GradientConfigFromToml(const toml::table& tbl, GradientConfig& cfg) {
    cfg.gradientStops.clear();
    if (auto arr = GetArray(tbl, "gradientStops")) {
        for (const auto& elem : *arr) {
            if (auto t = elem.as_table()) {
                GradientColorStop stop;
                stop.color = ColorFromTomlArray(GetArray(*t, "color"), { 0.0f, 0.0f, 0.0f });
                stop.position = GetOr(*t, "position", 0.0f);
                cfg.gradientStops.push_back(stop);
            }
        }
    }
    if (cfg.gradientStops.size() < 2) {
        cfg.gradientStops.clear();
        cfg.gradientStops.push_back({ { 0.0f, 0.0f, 0.0f }, 0.0f });
        cfg.gradientStops.push_back({ { 1.0f, 1.0f, 1.0f }, 1.0f });
    }
    cfg.gradientAngle = GetOr(tbl, "gradientAngle", 0.0f);
    cfg.gradientAnimation = StringToGradientAnimationType(GetStringOr(tbl, "gradientAnimation", "None"));
    cfg.gradientAnimationSpeed = GetOr(tbl, "gradientAnimationSpeed", 1.0f);
    cfg.gradientColorFade = GetOr(tbl, "gradientColorFade", false);
}

void BackgroundConfigToToml(const BackgroundConfig& cfg, toml::table& out) {
    out.is_inline(false);

    out.insert("selectedMode", cfg.selectedMode);
    out.insert("image", cfg.image);
    out.insert("color", ColorToTomlArray(cfg.color));

    GradientConfig gradientCfg;
    gradientCfg.gradientStops = cfg.gradientStops;
    gradientCfg.gradientAngle = cfg.gradientAngle;
    gradientCfg.gradientAnimation = cfg.gradientAnimation;
    gradientCfg.gradientAnimationSpeed = cfg.gradientAnimationSpeed;
    gradientCfg.gradientColorFade = cfg.gradientColorFade;
    GradientConfigToToml(gradientCfg, out);
}

void BackgroundConfigFromToml(const toml::table& tbl, BackgroundConfig& cfg) {
    cfg.selectedMode = GetStringOr(tbl, "selectedMode", ConfigDefaults::BACKGROUND_SELECTED_MODE);
    cfg.image = GetStringOr(tbl, "image", "");
    cfg.color = ColorFromTomlArray(GetArray(tbl, "color"), { 0.0f, 0.0f, 0.0f });

    GradientConfig gradientCfg;
    GradientConfigFromToml(tbl, gradientCfg);
    cfg.gradientStops = gradientCfg.gradientStops;
    cfg.gradientAngle = gradientCfg.gradientAngle;
    cfg.gradientAnimation = gradientCfg.gradientAnimation;
    cfg.gradientAnimationSpeed = gradientCfg.gradientAnimationSpeed;
    cfg.gradientColorFade = gradientCfg.gradientColorFade;
}


void MirrorCaptureConfigToToml(const MirrorCaptureConfig& cfg, toml::table& out) {
    out.is_inline(true);
    out.insert("x", cfg.x);
    out.insert("y", cfg.y);
    out.insert("relativeTo", cfg.relativeTo);
}

void MirrorCaptureConfigFromToml(const toml::table& tbl, MirrorCaptureConfig& cfg) {
    cfg.x = GetOr(tbl, "x", ConfigDefaults::MIRROR_CAPTURE_X);
    cfg.y = GetOr(tbl, "y", ConfigDefaults::MIRROR_CAPTURE_Y);
    cfg.relativeTo = GetStringOr(tbl, "relativeTo", ConfigDefaults::MIRROR_CAPTURE_RELATIVE_TO);
}

void MirrorRenderConfigToToml(const MirrorRenderConfig& cfg, toml::table& out) {
    out.is_inline(true);

    if (cfg.useRelativePosition) {
        out.insert("x", cfg.relativeX);
        out.insert("y", cfg.relativeY);
    } else {
        out.insert("x", cfg.x);
        out.insert("y", cfg.y);
    }
    out.insert("useRelativePosition", cfg.useRelativePosition);
    out.insert("relativeX", cfg.relativeX);
    out.insert("relativeY", cfg.relativeY);

    out.insert("scale", cfg.scale);
    out.insert("separateScale", cfg.separateScale);
    out.insert("scaleX", cfg.scaleX);
    out.insert("scaleY", cfg.scaleY);
    out.insert("relativeTo", cfg.relativeTo);
}

void MirrorRenderConfigFromToml(const toml::table& tbl, MirrorRenderConfig& cfg) {
    cfg.useRelativePosition = GetOr(tbl, "useRelativePosition", false);
    cfg.relativeX = GetOr(tbl, "relativeX", 0.5f);
    cfg.relativeY = GetOr(tbl, "relativeY", 0.5f);

    auto xNode = tbl["x"];
    auto yNode = tbl["y"];

    bool xIsPercentage = false;
    bool yIsPercentage = false;

    if (xNode.is_floating_point()) {
        double xVal = xNode.as_floating_point()->get();
        if (cfg.useRelativePosition) {
            cfg.relativeX = static_cast<float>(xVal);
            xIsPercentage = true;
        } else if (xVal >= 0.0 && xVal <= 1.0) {
            cfg.relativeX = static_cast<float>(xVal);
            xIsPercentage = true;
        } else {
            cfg.x = static_cast<int>(xVal);
        }
    } else if (xNode.is_integer()) {
        cfg.x = static_cast<int>(xNode.as_integer()->get());
    } else {
        cfg.x = ConfigDefaults::MIRROR_RENDER_X;
    }

    if (yNode.is_floating_point()) {
        double yVal = yNode.as_floating_point()->get();
        if (cfg.useRelativePosition) {
            cfg.relativeY = static_cast<float>(yVal);
            yIsPercentage = true;
        } else if (yVal >= 0.0 && yVal <= 1.0) {
            cfg.relativeY = static_cast<float>(yVal);
            yIsPercentage = true;
        } else {
            cfg.y = static_cast<int>(yVal);
        }
    } else if (yNode.is_integer()) {
        cfg.y = static_cast<int>(yNode.as_integer()->get());
    } else {
        cfg.y = ConfigDefaults::MIRROR_RENDER_Y;
    }

    if (!tbl.contains("useRelativePosition") && xIsPercentage && yIsPercentage) { cfg.useRelativePosition = true; }

    if (cfg.useRelativePosition) {
        int screenW = GetCachedWindowWidth();
        int screenH = GetCachedWindowHeight();

        if (screenW > 0 && (tbl.contains("relativeX") || xIsPercentage)) {
            cfg.x = static_cast<int>(cfg.relativeX * static_cast<float>(screenW));
        }
        if (screenH > 0 && (tbl.contains("relativeY") || yIsPercentage)) {
            cfg.y = static_cast<int>(cfg.relativeY * static_cast<float>(screenH));
        }
    }

    cfg.scale = GetOr(tbl, "scale", ConfigDefaults::MIRROR_RENDER_SCALE);
    cfg.separateScale = GetOr(tbl, "separateScale", ConfigDefaults::MIRROR_RENDER_SEPARATE_SCALE);
    cfg.scaleX = GetOr(tbl, "scaleX", ConfigDefaults::MIRROR_RENDER_SCALE_X);
    cfg.scaleY = GetOr(tbl, "scaleY", ConfigDefaults::MIRROR_RENDER_SCALE_Y);
    cfg.relativeTo = GetStringOr(tbl, "relativeTo", ConfigDefaults::MIRROR_RENDER_RELATIVE_TO);
}

void MirrorColorsToToml(const MirrorColors& cfg, toml::table& out) {
    out.is_inline(true);

    toml::array targetColorsArr;
    for (const auto& color : cfg.targetColors) { targetColorsArr.push_back(ColorToTomlArray(color)); }
    out.insert("targetColors", targetColorsArr);

    out.insert("output", ColorToTomlArray(cfg.output));
    out.insert("border", ColorToTomlArray(cfg.border));
}

void MirrorColorsFromToml(const toml::table& tbl, MirrorColors& cfg) {
    cfg.targetColors.clear();
    if (auto arr = GetArray(tbl, "targetColors")) {
        for (const auto& elem : *arr) {
            if (auto colorArr = elem.as_array()) { cfg.targetColors.push_back(ColorFromTomlArray(colorArr, { 0.0f, 1.0f, 0.0f })); }
        }
    }

    if (cfg.targetColors.empty()) { cfg.targetColors.push_back(ColorFromTomlArray(GetArray(tbl, "target"), { 0.0f, 1.0f, 0.0f })); }

    cfg.output = ColorFromTomlArray(GetArray(tbl, "output"), { 1.0f, 0.0f, 0.0f });
    cfg.border = ColorFromTomlArray(GetArray(tbl, "border"), { 1.0f, 1.0f, 1.0f });
}


static std::string MirrorGammaModeToString(MirrorGammaMode mode) {
    switch (mode) {
    case MirrorGammaMode::AssumeSRGB:
        return "SRGB";
    case MirrorGammaMode::AssumeLinear:
        return "Linear";
    default:
        return "Auto";
    }
}

static MirrorGammaMode StringToMirrorGammaMode(const std::string& str) {
    if (str == "SRGB" || str == "sRGB" || str == "srgb") return MirrorGammaMode::AssumeSRGB;
    if (str == "Linear" || str == "linear") return MirrorGammaMode::AssumeLinear;
    return MirrorGammaMode::Auto;
}

std::string MirrorBorderTypeToString(MirrorBorderType type) {
    switch (type) {
    case MirrorBorderType::Static:
        return "Static";
    default:
        return "Dynamic";
    }
}

MirrorBorderType StringToMirrorBorderType(const std::string& str) {
    if (str == "Static") return MirrorBorderType::Static;
    return MirrorBorderType::Dynamic;
}

std::string MirrorBorderShapeToString(MirrorBorderShape shape) {
    switch (shape) {
    case MirrorBorderShape::Circle:
        return "Circle";
    default:
        return "Rectangle";
    }
}

MirrorBorderShape StringToMirrorBorderShape(const std::string& str) {
    if (str == "Circle") return MirrorBorderShape::Circle;
    return MirrorBorderShape::Rectangle;
}

void MirrorBorderConfigToToml(const MirrorBorderConfig& cfg, toml::table& out) {
    out.insert("type", MirrorBorderTypeToString(cfg.type));
    out.insert("dynamicThickness", cfg.dynamicThickness);
    out.insert("staticShape", MirrorBorderShapeToString(cfg.staticShape));
    out.insert("staticColor", ColorToTomlArray(cfg.staticColor));
    out.insert("staticThickness", cfg.staticThickness);
    out.insert("staticRadius", cfg.staticRadius);
    out.insert("staticOffsetX", cfg.staticOffsetX);
    out.insert("staticOffsetY", cfg.staticOffsetY);
    out.insert("staticWidth", cfg.staticWidth);
    out.insert("staticHeight", cfg.staticHeight);
}

void MirrorBorderConfigFromToml(const toml::table& tbl, MirrorBorderConfig& cfg) {
    cfg.type = StringToMirrorBorderType(GetStringOr(tbl, "type", ConfigDefaults::MIRROR_BORDER_TYPE));
    cfg.dynamicThickness = GetOr(tbl, "dynamicThickness", ConfigDefaults::MIRROR_BORDER_DYNAMIC_THICKNESS);
    cfg.staticShape = StringToMirrorBorderShape(GetStringOr(tbl, "staticShape", ConfigDefaults::MIRROR_BORDER_STATIC_SHAPE));
    cfg.staticColor = ColorFromTomlArray(GetArray(tbl, "staticColor"), { 1.0f, 1.0f, 1.0f });
    cfg.staticThickness = GetOr(tbl, "staticThickness", ConfigDefaults::MIRROR_BORDER_STATIC_THICKNESS);
    cfg.staticRadius = GetOr(tbl, "staticRadius", ConfigDefaults::MIRROR_BORDER_STATIC_RADIUS);
    cfg.staticOffsetX = GetOr(tbl, "staticOffsetX", ConfigDefaults::MIRROR_BORDER_STATIC_OFFSET_X);
    cfg.staticOffsetY = GetOr(tbl, "staticOffsetY", ConfigDefaults::MIRROR_BORDER_STATIC_OFFSET_Y);
    cfg.staticWidth = GetOr(tbl, "staticWidth", ConfigDefaults::MIRROR_BORDER_STATIC_WIDTH);
    cfg.staticHeight = GetOr(tbl, "staticHeight", ConfigDefaults::MIRROR_BORDER_STATIC_HEIGHT);
}

void MirrorConfigToToml(const MirrorConfig& cfg, toml::table& out) {
    out.insert("name", cfg.name);
    out.insert("captureWidth", ClampMirrorCaptureDimension(cfg.captureWidth));
    out.insert("captureHeight", ClampMirrorCaptureDimension(cfg.captureHeight));

    toml::array inputArr;
    for (const auto& input : cfg.input) {
        toml::table inputTbl;
        MirrorCaptureConfigToToml(input, inputTbl);
        inputArr.push_back(inputTbl);
    }
    out.insert("input", inputArr);

    toml::table outputTbl;
    MirrorRenderConfigToToml(cfg.output, outputTbl);
    out.insert("output", outputTbl);

    toml::table colorsTbl;
    MirrorColorsToToml(cfg.colors, colorsTbl);
    out.insert("colors", colorsTbl);

    out.insert("colorSensitivity", std::round(cfg.colorSensitivity * 1000.0f) / 1000.0f);

    toml::table borderTbl;
    MirrorBorderConfigToToml(cfg.border, borderTbl);
    out.insert("border", borderTbl);

    out.insert("fps", cfg.fps);
    out.insert("opacity", std::round(cfg.opacity * 1000.0f) / 1000.0f);
    out.insert("rawOutput", cfg.rawOutput);
    out.insert("colorPassthrough", cfg.colorPassthrough);
    out.insert("gradientOutput", cfg.gradientOutput);

    toml::table gradientTbl;
    GradientConfigToToml(cfg.gradient, gradientTbl);
    out.insert("gradient", gradientTbl);

    out.insert("onlyOnMyScreen", cfg.onlyOnMyScreen);
}

void MirrorConfigFromToml(const toml::table& tbl, MirrorConfig& cfg) {
    cfg.name = GetStringOr(tbl, "name", "");
    cfg.captureWidth = ClampMirrorCaptureDimension(GetOr(tbl, "captureWidth", ConfigDefaults::MIRROR_CAPTURE_WIDTH));
    cfg.captureHeight = ClampMirrorCaptureDimension(GetOr(tbl, "captureHeight", ConfigDefaults::MIRROR_CAPTURE_HEIGHT));

    cfg.input.clear();
    if (auto arr = GetArray(tbl, "input")) {
        for (const auto& elem : *arr) {
            if (auto t = elem.as_table()) {
                MirrorCaptureConfig capture;
                MirrorCaptureConfigFromToml(*t, capture);
                cfg.input.push_back(capture);
            }
        }
    }

    if (auto t = GetTable(tbl, "output")) { MirrorRenderConfigFromToml(*t, cfg.output); }

    if (auto t = GetTable(tbl, "colors")) { MirrorColorsFromToml(*t, cfg.colors); }

    cfg.colorSensitivity = GetOr(tbl, "colorSensitivity", ConfigDefaults::MIRROR_COLOR_SENSITIVITY);

    if (auto t = GetTable(tbl, "border")) {
        MirrorBorderConfigFromToml(*t, cfg.border);
    } else {
        cfg.border.type = MirrorBorderType::Dynamic;
        cfg.border.dynamicThickness = GetOr(tbl, "borderThickness", ConfigDefaults::MIRROR_BORDER_DYNAMIC_THICKNESS);
    }

    cfg.fps = GetOr(tbl, "fps", ConfigDefaults::MIRROR_FPS);
    cfg.opacity = GetOr(tbl, "opacity", 1.0f);
    cfg.rawOutput = GetOr(tbl, "rawOutput", ConfigDefaults::MIRROR_RAW_OUTPUT);
    cfg.colorPassthrough = GetOr(tbl, "colorPassthrough", ConfigDefaults::MIRROR_COLOR_PASSTHROUGH);
    cfg.gradientOutput = GetOr(tbl, "gradientOutput", ConfigDefaults::MIRROR_GRADIENT_OUTPUT);
    if (auto t = GetTable(tbl, "gradient")) {
        GradientConfigFromToml(*t, cfg.gradient);
    } else {
        cfg.gradient = GradientConfig{};
    }
    cfg.onlyOnMyScreen = GetOr(tbl, "onlyOnMyScreen", ConfigDefaults::MIRROR_ONLY_ON_MY_SCREEN);
}

void MirrorGroupItemToToml(const MirrorGroupItem& item, toml::table& out) {
    out.is_inline(true);
    out.insert("mirrorId", item.mirrorId);
    out.insert("enabled", item.enabled);
    out.insert("widthPercent", item.widthPercent);
    out.insert("heightPercent", item.heightPercent);
    out.insert("offsetX", item.offsetX);
    out.insert("offsetY", item.offsetY);
}

void MirrorGroupItemFromToml(const toml::table& tbl, MirrorGroupItem& item) {
    item.mirrorId = GetStringOr(tbl, "mirrorId", "");
    item.enabled = GetOr(tbl, "enabled", true);
    item.widthPercent = GetOr(tbl, "widthPercent", 1.0f);
    item.heightPercent = GetOr(tbl, "heightPercent", 1.0f);
    item.offsetX = GetOr(tbl, "offsetX", 0);
    item.offsetY = GetOr(tbl, "offsetY", 0);
}

void MirrorGroupConfigToToml(const MirrorGroupConfig& cfg, toml::table& out) {
    out.insert("name", cfg.name);

    toml::table outputTbl;
    MirrorRenderConfigToToml(cfg.output, outputTbl);
    out.insert("output", outputTbl);

    toml::array mirrorsArr;
    for (const auto& item : cfg.mirrors) {
        toml::table itemTbl;
        MirrorGroupItemToToml(item, itemTbl);
        mirrorsArr.push_back(itemTbl);
    }
    out.insert("mirrors", mirrorsArr);
}

void MirrorGroupConfigFromToml(const toml::table& tbl, MirrorGroupConfig& cfg) {
    cfg.name = GetStringOr(tbl, "name", "");

    if (auto t = GetTable(tbl, "output")) { MirrorRenderConfigFromToml(*t, cfg.output); }

    cfg.mirrors.clear();

    if (auto arr = GetArray(tbl, "mirrors")) {
        for (const auto& elem : *arr) {
            if (auto t = elem.as_table()) {
                MirrorGroupItem item;
                MirrorGroupItemFromToml(*t, item);
                cfg.mirrors.push_back(item);
            }
        }
    }

    if (cfg.mirrors.empty()) {
        if (auto arr = GetArray(tbl, "mirrorIds")) {
            for (const auto& elem : *arr) {
                if (auto val = elem.value<std::string>()) {
                    MirrorGroupItem item;
                    item.mirrorId = *val;
                    item.widthPercent = 1.0f;
                    item.heightPercent = 1.0f;
                    cfg.mirrors.push_back(item);
                }
            }
        }
    }
}

void ImageBackgroundConfigToToml(const ImageBackgroundConfig& cfg, toml::table& out) {
    out.is_inline(true);
    out.insert("enabled", cfg.enabled);
    out.insert("color", ColorToTomlArray(cfg.color));
    out.insert("opacity", cfg.opacity);
}

void ImageBackgroundConfigFromToml(const toml::table& tbl, ImageBackgroundConfig& cfg) {
    cfg.enabled = GetOr(tbl, "enabled", ConfigDefaults::IMAGE_BG_ENABLED);
    cfg.color = ColorFromTomlArray(GetArray(tbl, "color"), { 0.0f, 0.0f, 0.0f });
    cfg.opacity = GetOr(tbl, "opacity", ConfigDefaults::IMAGE_BG_OPACITY);
}

void StretchConfigToToml(const StretchConfig& cfg, toml::table& out) {
    out.is_inline(true);
    out.insert("enabled", cfg.enabled);
    out.insert("width", cfg.width);
    out.insert("height", cfg.height);
    out.insert("x", cfg.x);
    out.insert("y", cfg.y);
}

void StretchConfigFromToml(const toml::table& tbl, StretchConfig& cfg) {
    cfg.enabled = GetOr(tbl, "enabled", ConfigDefaults::STRETCH_ENABLED);
    cfg.width = GetOr(tbl, "width", ConfigDefaults::STRETCH_WIDTH);
    cfg.height = GetOr(tbl, "height", ConfigDefaults::STRETCH_HEIGHT);
    cfg.x = GetOr(tbl, "x", ConfigDefaults::STRETCH_X);
    cfg.y = GetOr(tbl, "y", ConfigDefaults::STRETCH_Y);
}

void BorderConfigToToml(const BorderConfig& cfg, toml::table& out) {
    out.is_inline(true);
    out.insert("enabled", cfg.enabled);
    out.insert("color", ColorToTomlArray(cfg.color));
    out.insert("width", cfg.width);
    out.insert("radius", cfg.radius);
}

void BorderConfigFromToml(const toml::table& tbl, BorderConfig& cfg) {
    cfg.enabled = GetOr(tbl, "enabled", ConfigDefaults::BORDER_ENABLED);
    cfg.color = ColorFromTomlArray(GetArray(tbl, "color"), { 1.0f, 1.0f, 1.0f });
    cfg.width = GetOr(tbl, "width", ConfigDefaults::BORDER_WIDTH);
    cfg.radius = GetOr(tbl, "radius", ConfigDefaults::BORDER_RADIUS);
}

void ColorKeyConfigToToml(const ColorKeyConfig& cfg, toml::table& out) {
    out.is_inline(true);
    out.insert("color", ColorToTomlArray(cfg.color));
    out.insert("sensitivity", cfg.sensitivity);
}

void ColorKeyConfigFromToml(const toml::table& tbl, ColorKeyConfig& cfg) {
    cfg.color = ColorFromTomlArray(GetArray(tbl, "color"), { 0.0f, 0.0f, 0.0f });
    cfg.sensitivity = GetOr(tbl, "sensitivity", ConfigDefaults::COLOR_KEY_SENSITIVITY);
}

void ImageConfigToToml(const ImageConfig& cfg, toml::table& out) {
    out.insert("name", cfg.name);
    out.insert("path", cfg.path);
    out.insert("x", cfg.x);
    out.insert("y", cfg.y);
    out.insert("scale", cfg.scale);
    out.insert("relativeSizing", cfg.relativeSizing);
    out.insert("width", cfg.width);
    out.insert("height", cfg.height);
    out.insert("relativeTo", cfg.relativeTo);
    out.insert("crop_top", cfg.crop_top);
    out.insert("crop_bottom", cfg.crop_bottom);
    out.insert("crop_left", cfg.crop_left);
    out.insert("crop_right", cfg.crop_right);
    out.insert("cropToWidth", cfg.cropToWidth);
    out.insert("cropToHeight", cfg.cropToHeight);
    out.insert("enableColorKey", cfg.enableColorKey);

    toml::array colorKeysArr;
    for (const auto& ck : cfg.colorKeys) {
        toml::table ckTbl;
        ColorKeyConfigToToml(ck, ckTbl);
        colorKeysArr.push_back(ckTbl);
    }
    out.insert("colorKeys", colorKeysArr);

    out.insert("opacity", cfg.opacity);

    toml::table bgTbl;
    ImageBackgroundConfigToToml(cfg.background, bgTbl);
    out.insert("background", bgTbl);

    out.insert("pixelatedScaling", cfg.pixelatedScaling);
    out.insert("onlyOnMyScreen", cfg.onlyOnMyScreen);

    toml::table borderTbl;
    BorderConfigToToml(cfg.border, borderTbl);
    out.insert("border", borderTbl);
}

void ImageConfigFromToml(const toml::table& tbl, ImageConfig& cfg) {
    cfg.name = GetStringOr(tbl, "name", "");
    cfg.path = GetStringOr(tbl, "path", "");
    cfg.x = GetOr(tbl, "x", ConfigDefaults::IMAGE_X);
    cfg.y = GetOr(tbl, "y", ConfigDefaults::IMAGE_Y);
    cfg.scale = GetOr(tbl, "scale", ConfigDefaults::IMAGE_SCALE);
    cfg.relativeSizing = GetOr(tbl, "relativeSizing", ConfigDefaults::IMAGE_RELATIVE_SIZING);
    cfg.width = GetOr(tbl, "width", ConfigDefaults::IMAGE_WIDTH);
    cfg.height = GetOr(tbl, "height", ConfigDefaults::IMAGE_HEIGHT);
    cfg.relativeTo = GetStringOr(tbl, "relativeTo", ConfigDefaults::IMAGE_RELATIVE_TO);
    cfg.crop_top = GetOr(tbl, "crop_top", ConfigDefaults::IMAGE_CROP_TOP);
    cfg.crop_bottom = GetOr(tbl, "crop_bottom", ConfigDefaults::IMAGE_CROP_BOTTOM);
    cfg.crop_left = GetOr(tbl, "crop_left", ConfigDefaults::IMAGE_CROP_LEFT);
    cfg.crop_right = GetOr(tbl, "crop_right", ConfigDefaults::IMAGE_CROP_RIGHT);
    cfg.cropToWidth = GetOr(tbl, "cropToWidth", false);
    cfg.cropToHeight = GetOr(tbl, "cropToHeight", false);
    cfg.enableColorKey = GetOr(tbl, "enableColorKey", ConfigDefaults::IMAGE_ENABLE_COLOR_KEY);

    cfg.colorKeys.clear();
    if (auto arr = GetArray(tbl, "colorKeys")) {
        for (const auto& elem : *arr) {
            if (auto t = elem.as_table()) {
                ColorKeyConfig ck;
                ColorKeyConfigFromToml(*t, ck);
                cfg.colorKeys.push_back(ck);
            }
        }
    }

    cfg.opacity = GetOr(tbl, "opacity", ConfigDefaults::IMAGE_OPACITY);

    if (auto t = GetTable(tbl, "background")) { ImageBackgroundConfigFromToml(*t, cfg.background); }

    cfg.pixelatedScaling = GetOr(tbl, "pixelatedScaling", ConfigDefaults::IMAGE_PIXELATED_SCALING);
    cfg.onlyOnMyScreen = GetOr(tbl, "onlyOnMyScreen", ConfigDefaults::IMAGE_ONLY_ON_MY_SCREEN);

    if (auto t = GetTable(tbl, "border")) { BorderConfigFromToml(*t, cfg.border); }
}

void WindowOverlayConfigToToml(const WindowOverlayConfig& cfg, toml::table& out) {
    out.insert("name", cfg.name);
    out.insert("windowTitle", cfg.windowTitle);
    out.insert("windowClass", cfg.windowClass);
    out.insert("executableName", cfg.executableName);
    out.insert("windowMatchPriority", cfg.windowMatchPriority);
    out.insert("x", cfg.x);
    out.insert("y", cfg.y);
    out.insert("scale", cfg.scale);
    out.insert("relativeTo", cfg.relativeTo);
    out.insert("crop_top", cfg.crop_top);
    out.insert("crop_bottom", cfg.crop_bottom);
    out.insert("crop_left", cfg.crop_left);
    out.insert("crop_right", cfg.crop_right);
    out.insert("cropToWidth", cfg.cropToWidth);
    out.insert("cropToHeight", cfg.cropToHeight);
    out.insert("enableColorKey", cfg.enableColorKey);

    toml::array colorKeysArr;
    for (const auto& ck : cfg.colorKeys) {
        toml::table ckTbl;
        ColorKeyConfigToToml(ck, ckTbl);
        colorKeysArr.push_back(ckTbl);
    }
    out.insert("colorKeys", colorKeysArr);

    out.insert("opacity", cfg.opacity);

    toml::table bgTbl;
    ImageBackgroundConfigToToml(cfg.background, bgTbl);
    out.insert("background", bgTbl);

    out.insert("pixelatedScaling", cfg.pixelatedScaling);
    out.insert("onlyOnMyScreen", cfg.onlyOnMyScreen);
    out.insert("fps", cfg.fps);
    out.insert("searchInterval", cfg.searchInterval);
    out.insert("captureMethod", cfg.captureMethod);
    out.insert("forceUpdate", cfg.forceUpdate);
    out.insert("enableInteraction", cfg.enableInteraction);

    toml::table borderTbl;
    BorderConfigToToml(cfg.border, borderTbl);
    out.insert("border", borderTbl);
}

void WindowOverlayConfigFromToml(const toml::table& tbl, WindowOverlayConfig& cfg) {
    cfg.name = GetStringOr(tbl, "name", "");
    cfg.windowTitle = GetStringOr(tbl, "windowTitle", "");
    cfg.windowClass = GetStringOr(tbl, "windowClass", "");
    cfg.executableName = GetStringOr(tbl, "executableName", "");
    cfg.windowMatchPriority = GetStringOr(tbl, "windowMatchPriority", ConfigDefaults::WINDOW_OVERLAY_MATCH_PRIORITY);
    cfg.x = GetOr(tbl, "x", ConfigDefaults::IMAGE_X);
    cfg.y = GetOr(tbl, "y", ConfigDefaults::IMAGE_Y);
    cfg.scale = GetOr(tbl, "scale", ConfigDefaults::IMAGE_SCALE);
    cfg.relativeTo = GetStringOr(tbl, "relativeTo", ConfigDefaults::IMAGE_RELATIVE_TO);
    cfg.crop_top = GetOr(tbl, "crop_top", ConfigDefaults::IMAGE_CROP_TOP);
    cfg.crop_bottom = GetOr(tbl, "crop_bottom", ConfigDefaults::IMAGE_CROP_BOTTOM);
    cfg.crop_left = GetOr(tbl, "crop_left", ConfigDefaults::IMAGE_CROP_LEFT);
    cfg.crop_right = GetOr(tbl, "crop_right", ConfigDefaults::IMAGE_CROP_RIGHT);
    cfg.cropToWidth = GetOr(tbl, "cropToWidth", false);
    cfg.cropToHeight = GetOr(tbl, "cropToHeight", false);
    cfg.enableColorKey = GetOr(tbl, "enableColorKey", ConfigDefaults::IMAGE_ENABLE_COLOR_KEY);

    cfg.colorKeys.clear();
    if (auto arr = GetArray(tbl, "colorKeys")) {
        for (const auto& elem : *arr) {
            if (auto t = elem.as_table()) {
                ColorKeyConfig ck;
                ColorKeyConfigFromToml(*t, ck);
                cfg.colorKeys.push_back(ck);
            }
        }
    }

    cfg.opacity = GetOr(tbl, "opacity", ConfigDefaults::IMAGE_OPACITY);

    if (auto t = GetTable(tbl, "background")) { ImageBackgroundConfigFromToml(*t, cfg.background); }

    cfg.pixelatedScaling = GetOr(tbl, "pixelatedScaling", ConfigDefaults::IMAGE_PIXELATED_SCALING);
    cfg.onlyOnMyScreen = GetOr(tbl, "onlyOnMyScreen", ConfigDefaults::IMAGE_ONLY_ON_MY_SCREEN);
    cfg.fps = GetOr(tbl, "fps", ConfigDefaults::WINDOW_OVERLAY_FPS);
    cfg.searchInterval = GetOr(tbl, "searchInterval", ConfigDefaults::WINDOW_OVERLAY_SEARCH_INTERVAL);
    cfg.captureMethod = GetStringOr(tbl, "captureMethod", ConfigDefaults::WINDOW_OVERLAY_CAPTURE_METHOD);
    cfg.forceUpdate = GetOr(tbl, "forceUpdate", ConfigDefaults::WINDOW_OVERLAY_FORCE_UPDATE);
    cfg.enableInteraction = GetOr(tbl, "enableInteraction", ConfigDefaults::WINDOW_OVERLAY_ENABLE_INTERACTION);

    if (cfg.captureMethod == "Auto" || cfg.captureMethod == "PrintWindow_FullContent" || cfg.captureMethod == "PrintWindow_ClientOnly" ||
        cfg.captureMethod == "PrintWindow_Default") {
        cfg.captureMethod = "Windows 10+";
    }

    if (auto t = GetTable(tbl, "border")) { BorderConfigFromToml(*t, cfg.border); }
}

void BrowserOverlayConfigToToml(const BrowserOverlayConfig& cfg, toml::table& out) {
    out.insert("name", cfg.name);
    out.insert("url", cfg.url);
    out.insert("customCss", cfg.customCss);
    out.insert("browserWidth", cfg.browserWidth);
    out.insert("browserHeight", cfg.browserHeight);
    out.insert("x", cfg.x);
    out.insert("y", cfg.y);
    out.insert("scale", cfg.scale);
    out.insert("relativeTo", cfg.relativeTo);
    out.insert("crop_top", cfg.crop_top);
    out.insert("crop_bottom", cfg.crop_bottom);
    out.insert("crop_left", cfg.crop_left);
    out.insert("crop_right", cfg.crop_right);
    out.insert("cropToWidth", cfg.cropToWidth);
    out.insert("cropToHeight", cfg.cropToHeight);
    out.insert("enableColorKey", cfg.enableColorKey);

    toml::array colorKeysArr;
    for (const auto& ck : cfg.colorKeys) {
        toml::table ckTbl;
        ColorKeyConfigToToml(ck, ckTbl);
        colorKeysArr.push_back(ckTbl);
    }
    out.insert("colorKeys", colorKeysArr);

    out.insert("opacity", cfg.opacity);

    toml::table bgTbl;
    ImageBackgroundConfigToToml(cfg.background, bgTbl);
    out.insert("background", bgTbl);

    out.insert("pixelatedScaling", cfg.pixelatedScaling);
    out.insert("onlyOnMyScreen", cfg.onlyOnMyScreen);
    out.insert("fps", cfg.fps);
    out.insert("transparentBackground", cfg.transparentBackground);
    out.insert("muteAudio", cfg.muteAudio);
    out.insert("hardwareAcceleration", cfg.hardwareAcceleration);
    out.insert("allowSystemMediaKeys", cfg.allowSystemMediaKeys);
    out.insert("reloadOnUpdate", cfg.reloadOnUpdate);
    out.insert("reloadInterval", cfg.reloadInterval);

    toml::table borderTbl;
    BorderConfigToToml(cfg.border, borderTbl);
    out.insert("border", borderTbl);
}

void BrowserOverlayConfigFromToml(const toml::table& tbl, BrowserOverlayConfig& cfg) {
    cfg.name = GetStringOr(tbl, "name", "");
    cfg.url = GetStringOr(tbl, "url", "https://example.com");
    cfg.customCss = GetStringOr(tbl, "customCss", ConfigDefaults::BROWSER_OVERLAY_CUSTOM_CSS);
    cfg.browserWidth = GetOr(tbl, "browserWidth", ConfigDefaults::BROWSER_OVERLAY_WIDTH);
    cfg.browserHeight = GetOr(tbl, "browserHeight", ConfigDefaults::BROWSER_OVERLAY_HEIGHT);
    cfg.x = GetOr(tbl, "x", ConfigDefaults::IMAGE_X);
    cfg.y = GetOr(tbl, "y", ConfigDefaults::IMAGE_Y);
    cfg.scale = GetOr(tbl, "scale", ConfigDefaults::IMAGE_SCALE);
    cfg.relativeTo = GetStringOr(tbl, "relativeTo", ConfigDefaults::IMAGE_RELATIVE_TO);
    cfg.crop_top = GetOr(tbl, "crop_top", ConfigDefaults::IMAGE_CROP_TOP);
    cfg.crop_bottom = GetOr(tbl, "crop_bottom", ConfigDefaults::IMAGE_CROP_BOTTOM);
    cfg.crop_left = GetOr(tbl, "crop_left", ConfigDefaults::IMAGE_CROP_LEFT);
    cfg.crop_right = GetOr(tbl, "crop_right", ConfigDefaults::IMAGE_CROP_RIGHT);
    cfg.cropToWidth = GetOr(tbl, "cropToWidth", false);
    cfg.cropToHeight = GetOr(tbl, "cropToHeight", false);
    cfg.enableColorKey = GetOr(tbl, "enableColorKey", ConfigDefaults::BROWSER_OVERLAY_ENABLE_COLOR_KEY);

    cfg.colorKeys.clear();
    if (auto arr = GetArray(tbl, "colorKeys")) {
        for (const auto& elem : *arr) {
            if (auto t = elem.as_table()) {
                ColorKeyConfig ck;
                ColorKeyConfigFromToml(*t, ck);
                cfg.colorKeys.push_back(ck);
            }
        }
    }

    cfg.opacity = GetOr(tbl, "opacity", ConfigDefaults::IMAGE_OPACITY);

    if (auto t = GetTable(tbl, "background")) { ImageBackgroundConfigFromToml(*t, cfg.background); }

    cfg.pixelatedScaling = GetOr(tbl, "pixelatedScaling", ConfigDefaults::IMAGE_PIXELATED_SCALING);
    cfg.onlyOnMyScreen = GetOr(tbl, "onlyOnMyScreen", ConfigDefaults::IMAGE_ONLY_ON_MY_SCREEN);
    cfg.fps = GetOr(tbl, "fps", ConfigDefaults::BROWSER_OVERLAY_FPS);
    cfg.transparentBackground = GetOr(tbl, "transparentBackground", ConfigDefaults::BROWSER_OVERLAY_TRANSPARENT_BACKGROUND);
    cfg.muteAudio = GetOr(tbl, "muteAudio", ConfigDefaults::BROWSER_OVERLAY_MUTE_AUDIO);
    cfg.hardwareAcceleration = GetOr(tbl, "hardwareAcceleration", ConfigDefaults::BROWSER_OVERLAY_HARDWARE_ACCELERATION);
    cfg.allowSystemMediaKeys = GetOr(tbl, "allowSystemMediaKeys", ConfigDefaults::BROWSER_OVERLAY_ALLOW_SYSTEM_MEDIA_KEYS);
    cfg.reloadOnUpdate = GetOr(tbl, "reloadOnUpdate", ConfigDefaults::BROWSER_OVERLAY_RELOAD_ON_UPDATE);
    cfg.reloadInterval = GetOr(tbl, "reloadInterval", ConfigDefaults::BROWSER_OVERLAY_RELOAD_INTERVAL);

    if (auto t = GetTable(tbl, "border")) { BorderConfigFromToml(*t, cfg.border); }
}

void ModeConfigToToml(const ModeConfig& cfg, toml::table& out) {
    out.insert("id", cfg.id);

    const bool widthHasRelative = cfg.relativeWidth >= 0.0f && cfg.relativeWidth <= 1.0f;
    const bool heightHasRelative = cfg.relativeHeight >= 0.0f && cfg.relativeHeight <= 1.0f;
    const bool widthUsesRelative = cfg.useRelativeSize && widthHasRelative;
    const bool heightUsesRelative = cfg.useRelativeSize && heightHasRelative;
    const bool widthUsesExpression = !widthUsesRelative && !cfg.widthExpr.empty();
    const bool heightUsesExpression = !heightUsesRelative && !cfg.heightExpr.empty();
    const bool widthIsDynamic = widthUsesExpression || widthUsesRelative;
    const bool heightIsDynamic = heightUsesExpression || heightUsesRelative;

    int persistedWidth = cfg.width;
    int persistedHeight = cfg.height;

    if (widthIsDynamic) {
        persistedWidth = (cfg.manualWidth > 0) ? cfg.manualWidth : cfg.width;
    }
    if (heightIsDynamic) {
        persistedHeight = (cfg.manualHeight > 0) ? cfg.manualHeight : cfg.height;
    }

    if (persistedWidth < 1) persistedWidth = ConfigDefaults::MODE_WIDTH;
    if (persistedHeight < 1) persistedHeight = ConfigDefaults::MODE_HEIGHT;

    if (widthUsesExpression) {
        out.insert("width", cfg.widthExpr);
    } else {
        out.insert("width", persistedWidth);
    }
    if (heightUsesExpression) {
        out.insert("height", cfg.heightExpr);
    } else {
        out.insert("height", persistedHeight);
    }

    out.insert("useRelativeSize", cfg.useRelativeSize);
    if (widthUsesRelative) { out.insert("relativeWidth", cfg.relativeWidth); }
    if (heightUsesRelative) { out.insert("relativeHeight", cfg.relativeHeight); }

    toml::table bgTbl;
    BackgroundConfigToToml(cfg.background, bgTbl);
    out.insert("background", bgTbl);

    toml::array mirrorIds;
    for (const auto& id : cfg.mirrorIds) { mirrorIds.push_back(id); }
    out.insert("mirrorIds", mirrorIds);

    toml::array mirrorGroupIds;
    for (const auto& id : cfg.mirrorGroupIds) { mirrorGroupIds.push_back(id); }
    out.insert("mirrorGroupIds", mirrorGroupIds);

    toml::array imageIds;
    for (const auto& id : cfg.imageIds) { imageIds.push_back(id); }
    out.insert("imageIds", imageIds);

    toml::array windowOverlayIds;
    for (const auto& id : cfg.windowOverlayIds) { windowOverlayIds.push_back(id); }
    out.insert("windowOverlayIds", windowOverlayIds);

    toml::array browserOverlayIds;
    for (const auto& id : cfg.browserOverlayIds) { browserOverlayIds.push_back(id); }
    out.insert("browserOverlayIds", browserOverlayIds);

    toml::table stretchTbl;
    StretchConfigToToml(cfg.stretch, stretchTbl);
    out.insert("stretch", stretchTbl);

    toml::table transitionTbl;
    transitionTbl.insert("gameTransition", GameTransitionTypeToString(cfg.gameTransition));
    transitionTbl.insert("overlayTransition", OverlayTransitionTypeToString(cfg.overlayTransition));
    transitionTbl.insert("backgroundTransition", BackgroundTransitionTypeToString(cfg.backgroundTransition));
    transitionTbl.insert("transitionDurationMs", cfg.transitionDurationMs);

    transitionTbl.insert("easeInPower", cfg.easeInPower);
    transitionTbl.insert("easeOutPower", cfg.easeOutPower);
    transitionTbl.insert("bounceCount", cfg.bounceCount);
    transitionTbl.insert("bounceIntensity", cfg.bounceIntensity);
    transitionTbl.insert("bounceDurationMs", cfg.bounceDurationMs);
    transitionTbl.insert("relativeStretching", cfg.relativeStretching);
    transitionTbl.insert("skipAnimateX", cfg.skipAnimateX);
    transitionTbl.insert("skipAnimateY", cfg.skipAnimateY);
    transitionTbl.insert("slideMirrorsIn", cfg.slideMirrorsIn);
    out.insert("transition", transitionTbl);

    toml::table borderTbl;
    BorderConfigToToml(cfg.border, borderTbl);
    out.insert("border", borderTbl);

    out.insert("sensitivityOverrideEnabled", cfg.sensitivityOverrideEnabled);
    out.insert("modeSensitivity", cfg.modeSensitivity);
    out.insert("separateXYSensitivity", cfg.separateXYSensitivity);
    out.insert("modeSensitivityX", cfg.modeSensitivityX);
    out.insert("modeSensitivityY", cfg.modeSensitivityY);

}

void ModeConfigFromToml(const toml::table& tbl, ModeConfig& cfg) {
    ModeConfigFromTomlInternal(tbl, cfg, nullptr);
}

void HotkeyConditionsToToml(const HotkeyConditions& cfg, toml::table& out) {
    toml::array gameStateArr;
    for (const auto& state : cfg.gameState) { gameStateArr.push_back(state); }
    out.insert("gameState", gameStateArr);

    toml::array exclusionsArr;
    for (const auto& excl : cfg.exclusions) { exclusionsArr.push_back(static_cast<int64_t>(excl)); }
    out.insert("exclusions", exclusionsArr);
}

void HotkeyConditionsFromToml(const toml::table& tbl, HotkeyConditions& cfg) {
    cfg.gameState.clear();
    if (auto arr = GetArray(tbl, "gameState")) {
        for (const auto& elem : *arr) {
            if (auto val = elem.value<std::string>()) { cfg.gameState.push_back(*val); }
        }
    }

    cfg.exclusions.clear();
    if (auto arr = GetArray(tbl, "exclusions")) {
        for (const auto& elem : *arr) {
            if (auto val = elem.value<int64_t>()) { cfg.exclusions.push_back(static_cast<DWORD>(*val)); }
        }
    }
}

void AltSecondaryModeToToml(const AltSecondaryMode& cfg, toml::table& out) {
    toml::array keysArr;
    for (const auto& key : cfg.keys) { keysArr.push_back(static_cast<int64_t>(key)); }
    out.insert("keys", keysArr);
    out.insert("mode", cfg.mode);
}

void AltSecondaryModeFromToml(const toml::table& tbl, AltSecondaryMode& cfg) {
    cfg.keys.clear();
    if (auto arr = GetArray(tbl, "keys")) {
        for (const auto& elem : *arr) {
            if (auto val = elem.value<int64_t>()) { cfg.keys.push_back(static_cast<DWORD>(*val)); }
        }
    }
    cfg.mode = GetStringOr(tbl, "mode", "");
}

void HotkeyConfigToToml(const HotkeyConfig& cfg, toml::table& out) {
    toml::array keysArr;
    for (const auto& key : cfg.keys) { keysArr.push_back(static_cast<int64_t>(key)); }
    out.insert("keys", keysArr);

    out.insert("mainMode", cfg.mainMode);
    out.insert("secondaryMode", cfg.secondaryMode);

    toml::array altSecondaryArr;
    for (const auto& alt : cfg.altSecondaryModes) {
        toml::table altTbl;
        AltSecondaryModeToToml(alt, altTbl);
        altSecondaryArr.push_back(altTbl);
    }
    out.insert("altSecondaryModes", altSecondaryArr);

    toml::table conditionsTbl;
    HotkeyConditionsToToml(cfg.conditions, conditionsTbl);
    out.insert("conditions", conditionsTbl);

    out.insert("debounce", cfg.debounce);
    out.insert("triggerOnRelease", cfg.triggerOnRelease);
    out.insert("triggerOnHold", cfg.triggerOnHold);

    out.insert("blockKeyFromGame", cfg.blockKeyFromGame);
    out.insert("allowExitToFullscreenRegardlessOfGameState", cfg.allowExitToFullscreenRegardlessOfGameState);
}

void HotkeyConfigFromToml(const toml::table& tbl, HotkeyConfig& cfg) {
    cfg.keys.clear();
    if (auto arr = GetArray(tbl, "keys")) {
        for (const auto& elem : *arr) {
            if (auto val = elem.value<int64_t>()) { cfg.keys.push_back(static_cast<DWORD>(*val)); }
        }
    }

    cfg.mainMode = GetStringOr(tbl, "mainMode", "");
    cfg.secondaryMode = GetStringOr(tbl, "secondaryMode", "");

    cfg.altSecondaryModes.clear();
    if (auto arr = GetArray(tbl, "altSecondaryModes")) {
        for (const auto& elem : *arr) {
            if (auto t = elem.as_table()) {
                AltSecondaryMode alt;
                AltSecondaryModeFromToml(*t, alt);
                cfg.altSecondaryModes.push_back(alt);
            }
        }
    }

    if (auto t = GetTable(tbl, "conditions")) { HotkeyConditionsFromToml(*t, cfg.conditions); }

    cfg.debounce = GetOr(tbl, "debounce", ConfigDefaults::HOTKEY_DEBOUNCE);
    cfg.triggerOnRelease = GetOr(tbl, "triggerOnRelease", false);
    cfg.triggerOnHold = GetOr(tbl, "triggerOnHold", false);

    cfg.blockKeyFromGame = GetOr(tbl, "blockKeyFromGame", false);
    cfg.allowExitToFullscreenRegardlessOfGameState = GetOr(tbl, "allowExitToFullscreenRegardlessOfGameState", false);
    // Note: currentSecondaryMode is now tracked separately via thread-safe
}

void SensitivityHotkeyConfigToToml(const SensitivityHotkeyConfig& cfg, toml::table& out) {
    toml::array keysArr;
    for (const auto& key : cfg.keys) { keysArr.push_back(static_cast<int64_t>(key)); }
    out.insert("keys", keysArr);

    out.insert("sensitivity", cfg.sensitivity);
    out.insert("separateXY", cfg.separateXY);
    out.insert("sensitivityX", cfg.sensitivityX);
    out.insert("sensitivityY", cfg.sensitivityY);

    toml::table conditionsTbl;
    HotkeyConditionsToToml(cfg.conditions, conditionsTbl);
    out.insert("conditions", conditionsTbl);

    out.insert("debounce", cfg.debounce);
    out.insert("toggle", cfg.toggle);
}

void SensitivityHotkeyConfigFromToml(const toml::table& tbl, SensitivityHotkeyConfig& cfg) {
    cfg.keys.clear();
    if (auto arr = GetArray(tbl, "keys")) {
        for (const auto& elem : *arr) {
            if (auto val = elem.value<int64_t>()) { cfg.keys.push_back(static_cast<DWORD>(*val)); }
        }
    }

    cfg.sensitivity = GetOr(tbl, "sensitivity", 1.0f);
    cfg.separateXY = GetOr(tbl, "separateXY", false);
    cfg.sensitivityX = GetOr(tbl, "sensitivityX", 1.0f);
    cfg.sensitivityY = GetOr(tbl, "sensitivityY", 1.0f);

    if (auto t = GetTable(tbl, "conditions")) { HotkeyConditionsFromToml(*t, cfg.conditions); }

    cfg.debounce = GetOr(tbl, "debounce", ConfigDefaults::HOTKEY_DEBOUNCE);
    cfg.toggle = GetOr(tbl, "toggle", false);
}

void DebugGlobalConfigToToml(const DebugGlobalConfig& cfg, toml::table& out) {
    out.insert("showPerformanceOverlay", cfg.showPerformanceOverlay);
    out.insert("showProfiler", cfg.showProfiler);
    out.insert("profilerScale", cfg.profilerScale);
    out.insert("showHotkeyDebug", cfg.showHotkeyDebug);
    out.insert("fakeCursor", cfg.fakeCursor);
    out.insert("showTextureGrid", cfg.showTextureGrid);
    out.insert("delayRenderingUntilFinished", cfg.delayRenderingUntilFinished);
    out.insert("virtualCameraEnabled", cfg.virtualCameraEnabled);
    out.insert("videoCacheBudgetMiB", cfg.videoCacheBudgetMiB);

    out.insert("logModeSwitch", cfg.logModeSwitch);
    out.insert("logAnimation", cfg.logAnimation);
    out.insert("logHotkey", cfg.logHotkey);
    out.insert("logObs", cfg.logObs);
    out.insert("logWindowOverlay", cfg.logWindowOverlay);
    out.insert("logBrowserOverlay", cfg.logBrowserOverlay);
    out.insert("logNinjabrain", cfg.logNinjabrain);
    out.insert("logFileMonitor", cfg.logFileMonitor);
    out.insert("logImageMonitor", cfg.logImageMonitor);
    out.insert("logPerformance", cfg.logPerformance);
    out.insert("logTextureOps", cfg.logTextureOps);
    out.insert("logGui", cfg.logGui);
    out.insert("logInit", cfg.logInit);
    out.insert("logCursorTextures", cfg.logCursorTextures);
}

void DebugGlobalConfigFromToml(const toml::table& tbl, DebugGlobalConfig& cfg) {
    cfg.showPerformanceOverlay = GetOr(tbl, "showPerformanceOverlay", ConfigDefaults::DEBUG_GLOBAL_SHOW_PERFORMANCE_OVERLAY);
    cfg.showProfiler = GetOr(tbl, "showProfiler", ConfigDefaults::DEBUG_GLOBAL_SHOW_PROFILER);
    cfg.profilerScale = GetOr(tbl, "profilerScale", ConfigDefaults::DEBUG_GLOBAL_PROFILER_SCALE);
    cfg.showHotkeyDebug = GetOr(tbl, "showHotkeyDebug", ConfigDefaults::DEBUG_GLOBAL_SHOW_HOTKEY_DEBUG);
    cfg.fakeCursor = GetOr(tbl, "fakeCursor", ConfigDefaults::DEBUG_GLOBAL_FAKE_CURSOR);
    cfg.showTextureGrid = GetOr(tbl, "showTextureGrid", ConfigDefaults::DEBUG_GLOBAL_SHOW_TEXTURE_GRID);
    cfg.delayRenderingUntilFinished =
        GetOr(tbl, "delayRenderingUntilFinished", ConfigDefaults::DEBUG_GLOBAL_DELAY_RENDERING_UNTIL_FINISHED);
    cfg.virtualCameraEnabled = GetOr(tbl, "virtualCameraEnabled", false);
    cfg.videoCacheBudgetMiB = GetOr(tbl, "videoCacheBudgetMiB", ConfigDefaults::DEBUG_GLOBAL_VIDEO_CACHE_BUDGET_MIB);

    cfg.logModeSwitch = GetOr(tbl, "logModeSwitch", ConfigDefaults::DEBUG_GLOBAL_LOG_MODE_SWITCH);
    cfg.logAnimation = GetOr(tbl, "logAnimation", ConfigDefaults::DEBUG_GLOBAL_LOG_ANIMATION);
    cfg.logHotkey = GetOr(tbl, "logHotkey", ConfigDefaults::DEBUG_GLOBAL_LOG_HOTKEY);
    cfg.logObs = GetOr(tbl, "logObs", ConfigDefaults::DEBUG_GLOBAL_LOG_OBS);
    cfg.logWindowOverlay = GetOr(tbl, "logWindowOverlay", ConfigDefaults::DEBUG_GLOBAL_LOG_WINDOW_OVERLAY);
    cfg.logBrowserOverlay = GetOr(tbl, "logBrowserOverlay", ConfigDefaults::DEBUG_GLOBAL_LOG_BROWSER_OVERLAY);
    cfg.logNinjabrain = GetOr(tbl, "logNinjabrain", ConfigDefaults::DEBUG_GLOBAL_LOG_NINJABRAIN);
    cfg.logFileMonitor = GetOr(tbl, "logFileMonitor", ConfigDefaults::DEBUG_GLOBAL_LOG_FILE_MONITOR);
    cfg.logImageMonitor = GetOr(tbl, "logImageMonitor", ConfigDefaults::DEBUG_GLOBAL_LOG_IMAGE_MONITOR);
    cfg.logPerformance = GetOr(tbl, "logPerformance", ConfigDefaults::DEBUG_GLOBAL_LOG_PERFORMANCE);
    cfg.logTextureOps = GetOr(tbl, "logTextureOps", ConfigDefaults::DEBUG_GLOBAL_LOG_TEXTURE_OPS);
    cfg.logGui = GetOr(tbl, "logGui", ConfigDefaults::DEBUG_GLOBAL_LOG_GUI);
    cfg.logInit = GetOr(tbl, "logInit", ConfigDefaults::DEBUG_GLOBAL_LOG_INIT);
    cfg.logCursorTextures = GetOr(tbl, "logCursorTextures", ConfigDefaults::DEBUG_GLOBAL_LOG_CURSOR_TEXTURES);
}

void CursorConfigToToml(const CursorConfig& cfg, toml::table& out) {
    out.insert("cursorName", cfg.cursorName);
    out.insert("cursorSize", cfg.cursorSize);
}

void CursorConfigFromToml(const toml::table& tbl, CursorConfig& cfg) {
    cfg.cursorName = GetStringOr(tbl, "cursorName", "");
    cfg.cursorSize = std::clamp(GetOr(tbl, "cursorSize", ConfigDefaults::CURSOR_SIZE), ConfigDefaults::CURSOR_MIN_SIZE,
                                ConfigDefaults::CURSOR_MAX_SIZE);
}

void CursorsConfigToToml(const CursorsConfig& cfg, toml::table& out) {
    out.insert("enabled", cfg.enabled);

    toml::table titleTbl;
    CursorConfigToToml(cfg.title, titleTbl);
    out.insert("title", titleTbl);

    toml::table wallTbl;
    CursorConfigToToml(cfg.wall, wallTbl);
    out.insert("wall", wallTbl);

    toml::table ingameTbl;
    CursorConfigToToml(cfg.ingame, ingameTbl);
    out.insert("ingame", ingameTbl);
}

void CursorsConfigFromToml(const toml::table& tbl, CursorsConfig& cfg) {
    cfg.enabled = GetOr(tbl, "enabled", ConfigDefaults::CURSORS_ENABLED);

    if (auto t = GetTable(tbl, "title")) { CursorConfigFromToml(*t, cfg.title); }
    if (auto t = GetTable(tbl, "wall")) { CursorConfigFromToml(*t, cfg.wall); }
    if (auto t = GetTable(tbl, "ingame")) { CursorConfigFromToml(*t, cfg.ingame); }
}

void CursorTrailConfigToToml(const CursorTrailConfig& cfg, toml::table& out) {
    out.insert("enabled", cfg.enabled);
    out.insert("onlyOnMyScreen", cfg.onlyOnMyScreen);
    out.insert("onlyOnObs", cfg.onlyOnObs);
    out.insert("lifetimeMs", cfg.lifetimeMs);
    out.insert("stampSpacingPx", cfg.stampSpacingPx);
    out.insert("spriteSizePx", cfg.spriteSizePx);
    out.insert("tailSizeScale", cfg.tailSizeScale);
    out.insert("useVelocitySize", cfg.useVelocitySize);
    out.insert("velocitySizeIntensity", cfg.velocitySizeIntensity);
    out.insert("color", ColorToTomlArray(cfg.color));
    out.insert("useGradient", cfg.useGradient);
    out.insert("tailColor", ColorToTomlArray(cfg.tailColor));
    out.insert("opacity", cfg.opacity);
    out.insert("blendMode", cfg.blendMode);
    out.insert("spritePath", cfg.spritePath);
}

void CursorTrailConfigFromToml(const toml::table& tbl, CursorTrailConfig& cfg) {
    cfg.enabled = GetOr(tbl, "enabled", ConfigDefaults::CURSOR_TRAIL_ENABLED);
    cfg.onlyOnMyScreen = GetOr(tbl, "onlyOnMyScreen", ConfigDefaults::CURSOR_TRAIL_ONLY_ON_MY_SCREEN);
    cfg.onlyOnObs = GetOr(tbl, "onlyOnObs", ConfigDefaults::CURSOR_TRAIL_ONLY_ON_OBS);
    cfg.lifetimeMs = std::clamp(GetOr(tbl, "lifetimeMs", ConfigDefaults::CURSOR_TRAIL_LIFETIME_MS), 50, 500);
    cfg.stampSpacingPx = std::clamp(GetOr(tbl, "stampSpacingPx", ConfigDefaults::CURSOR_TRAIL_STAMP_SPACING_PX), 1, 64);
    cfg.spriteSizePx = std::clamp(GetOr(tbl, "spriteSizePx", ConfigDefaults::CURSOR_TRAIL_SPRITE_SIZE_PX), 4, 256);
    cfg.tailSizeScale = std::clamp(GetOr(tbl, "tailSizeScale", ConfigDefaults::CURSOR_TRAIL_TAIL_SIZE_SCALE), 0.0f, 2.0f);
    cfg.useVelocitySize = GetOr(tbl, "useVelocitySize", ConfigDefaults::CURSOR_TRAIL_USE_VELOCITY_SIZE);
    cfg.velocitySizeIntensity = std::clamp(GetOr(tbl, "velocitySizeIntensity", ConfigDefaults::CURSOR_TRAIL_VELOCITY_SIZE_INTENSITY), 0.0f, 1.0f);
    cfg.color = ColorFromTomlArray(GetArray(tbl, "color"),
                                   Color{ ConfigDefaults::CURSOR_TRAIL_COLOR_R,
                                          ConfigDefaults::CURSOR_TRAIL_COLOR_G,
                                          ConfigDefaults::CURSOR_TRAIL_COLOR_B });
    cfg.useGradient = GetOr(tbl, "useGradient", ConfigDefaults::CURSOR_TRAIL_USE_GRADIENT);
    cfg.tailColor = ColorFromTomlArray(GetArray(tbl, "tailColor"),
                                       Color{ ConfigDefaults::CURSOR_TRAIL_TAIL_COLOR_R,
                                              ConfigDefaults::CURSOR_TRAIL_TAIL_COLOR_G,
                                              ConfigDefaults::CURSOR_TRAIL_TAIL_COLOR_B });
    cfg.opacity = std::clamp(GetOr(tbl, "opacity", ConfigDefaults::CURSOR_TRAIL_OPACITY), 0.0f, 1.0f);
    cfg.blendMode = GetStringOr(tbl, "blendMode", ConfigDefaults::CURSOR_TRAIL_BLEND_MODE);
    cfg.spritePath = GetStringOr(tbl, "spritePath", ConfigDefaults::CURSOR_TRAIL_SPRITE_PATH);
}

void EyeZoomConfigToToml(const EyeZoomConfig& cfg, toml::table& out) {
    out.insert("cloneWidth", cfg.cloneWidth);
    out.insert("overlayWidth", cfg.overlayWidth);
    out.insert("cloneHeight", cfg.cloneHeight);
    out.insert("stretchWidth", cfg.stretchWidth);
    out.insert("windowWidth", cfg.windowWidth);
    out.insert("windowHeight", cfg.windowHeight);
    out.insert("zoomAreaWidth", cfg.zoomAreaWidth);
    out.insert("zoomAreaHeight", cfg.zoomAreaHeight);
    out.insert("useCustomSizePosition", cfg.useCustomSizePosition);
    out.insert("positionX", cfg.positionX);
    out.insert("positionY", cfg.positionY);
    out.insert("fontSizeMode", EyeZoomFontSizeModeToTomlString(cfg.fontSizeMode));
    out.insert("textFontSize", cfg.textFontSize);
    out.insert("textFontPath", cfg.textFontPath);
    out.insert("rectHeight", cfg.rectHeight);
    out.insert("linkRectToFont", cfg.linkRectToFont);
    out.insert("gridColor1", ColorToTomlArray(cfg.gridColor1));
    out.insert("gridColor1Opacity", cfg.gridColor1Opacity);
    out.insert("gridColor2", ColorToTomlArray(cfg.gridColor2));
    out.insert("gridColor2Opacity", cfg.gridColor2Opacity);
    out.insert("centerLineColor", ColorToTomlArray(cfg.centerLineColor));
    out.insert("centerLineColorOpacity", cfg.centerLineColorOpacity);
    out.insert("textColor", ColorToTomlArray(cfg.textColor));
    out.insert("textColorOpacity", cfg.textColorOpacity);
    out.insert("slideZoomIn", cfg.slideZoomIn);
    out.insert("slideMirrorsIn", cfg.slideMirrorsIn);
    out.insert("activeOverlayIndex", cfg.activeOverlayIndex);

    if (!cfg.overlays.empty()) {
        toml::array overlayArr;
        for (const auto& ov : cfg.overlays) {
            toml::table ovTbl;
            ovTbl.insert("name", ov.name);
            ovTbl.insert("path", ov.path);
            switch (ov.displayMode) {
                case EyeZoomOverlayDisplayMode::Manual:  ovTbl.insert("displayMode", "manual"); break;
                case EyeZoomOverlayDisplayMode::Stretch: ovTbl.insert("displayMode", "stretch"); break;
                default:                                 ovTbl.insert("displayMode", "fit"); break;
            }
            ovTbl.insert("manualWidth", ov.manualWidth);
            ovTbl.insert("manualHeight", ov.manualHeight);
            ovTbl.insert("clipToZoomArea", ov.clipToZoomArea);
            ovTbl.insert("opacity", ov.opacity);
            overlayArr.push_back(std::move(ovTbl));
        }
        out.insert("overlay", std::move(overlayArr));
    }
}

void EyeZoomConfigFromToml(const toml::table& tbl, EyeZoomConfig& cfg) {
    cfg.cloneWidth = GetOr(tbl, "cloneWidth", ConfigDefaults::EYEZOOM_CLONE_WIDTH);
    // cloneWidth must be even and >= 2 for center-split math used by the overlay.
    if (cfg.cloneWidth < 2) cfg.cloneWidth = 2;
    if (cfg.cloneWidth % 2 != 0) cfg.cloneWidth = (cfg.cloneWidth / 2) * 2;

    int overlayDefaultSentinel = -1;
    int overlayWidth = GetOr(tbl, "overlayWidth", overlayDefaultSentinel);
    if (overlayWidth == overlayDefaultSentinel) {
        cfg.overlayWidth = cfg.cloneWidth / 2;
    } else {
        cfg.overlayWidth = overlayWidth;
    }
    if (cfg.overlayWidth < 0) cfg.overlayWidth = 0;
    int maxOverlay = cfg.cloneWidth / 2;
    if (cfg.overlayWidth > maxOverlay) cfg.overlayWidth = maxOverlay;

    cfg.cloneHeight = GetOr(tbl, "cloneHeight", ConfigDefaults::EYEZOOM_CLONE_HEIGHT);
    cfg.stretchWidth = GetOr(tbl, "stretchWidth", ConfigDefaults::EYEZOOM_STRETCH_WIDTH);
    cfg.windowWidth = GetOr(tbl, "windowWidth", ConfigDefaults::EYEZOOM_WINDOW_WIDTH);
    cfg.windowHeight = GetOr(tbl, "windowHeight", ConfigDefaults::EYEZOOM_WINDOW_HEIGHT);

    int screenWidth = GetCachedWindowWidth();
    int screenHeight = GetCachedWindowHeight();
    int viewportX = (screenWidth > 0) ? ((screenWidth - cfg.windowWidth) / 2) : 0;
    int autoHorizontalMargin = (viewportX > 0) ? (viewportX / 10) : 0;
    int autoVerticalMargin = (screenHeight > 0) ? (screenHeight / 8) : 0;
    int computedAutoZoomAreaWidth = viewportX - (2 * autoHorizontalMargin);
    int computedAutoZoomAreaHeight = screenHeight - (2 * autoVerticalMargin);
    if (computedAutoZoomAreaWidth < 1) {
        computedAutoZoomAreaWidth = (screenWidth > 0) ? screenWidth : (std::max)(1, cfg.windowWidth);
    }
    if (computedAutoZoomAreaHeight < 1) {
        computedAutoZoomAreaHeight = (screenHeight > 0) ? screenHeight : (std::max)(1, cfg.windowHeight);
    }

    bool hasZoomAreaWidth = tbl.contains("zoomAreaWidth");
    bool hasZoomAreaHeight = tbl.contains("zoomAreaHeight");

    int legacyHorizontalMargin = GetOr(tbl, "horizontalMargin", -1);
    int legacyVerticalMargin = GetOr(tbl, "verticalMargin", -1);

    if (hasZoomAreaWidth) {
        int parsedZoomAreaWidth = GetOr(tbl, "zoomAreaWidth", computedAutoZoomAreaWidth);
        cfg.zoomAreaWidth = (parsedZoomAreaWidth > 0) ? parsedZoomAreaWidth : computedAutoZoomAreaWidth;
    } else if (legacyHorizontalMargin >= 0 && viewportX > 0) {
        cfg.zoomAreaWidth = viewportX - (2 * legacyHorizontalMargin);
    } else {
        cfg.zoomAreaWidth = computedAutoZoomAreaWidth;
    }

    if (hasZoomAreaHeight) {
        int parsedZoomAreaHeight = GetOr(tbl, "zoomAreaHeight", computedAutoZoomAreaHeight);
        cfg.zoomAreaHeight = (parsedZoomAreaHeight > 0) ? parsedZoomAreaHeight : computedAutoZoomAreaHeight;
    } else if (legacyVerticalMargin >= 0 && screenHeight > 0) {
        cfg.zoomAreaHeight = screenHeight - (2 * legacyVerticalMargin);
    } else {
        cfg.zoomAreaHeight = computedAutoZoomAreaHeight;
    }

    if (cfg.zoomAreaWidth < 1) cfg.zoomAreaWidth = 1;
    if (cfg.zoomAreaHeight < 1) cfg.zoomAreaHeight = 1;

    cfg.useCustomSizePosition =
        GetOr(tbl, "useCustomSizePosition", GetOr(tbl, "useCustomPosition", ConfigDefaults::EYEZOOM_USE_CUSTOM_SIZE_POSITION));

    cfg.positionX = GetOr(tbl, "positionX", ConfigDefaults::EYEZOOM_POSITION_X);
    cfg.positionY = GetOr(tbl, "positionY", ConfigDefaults::EYEZOOM_POSITION_Y);
    cfg.fontSizeMode = ParseEyeZoomFontSizeMode(tbl);
    cfg.textFontSize = GetOr(tbl, "textFontSize", ConfigDefaults::EYEZOOM_TEXT_FONT_SIZE);
    cfg.textFontPath = GetStringOr(tbl, "textFontPath", ConfigDefaults::EYEZOOM_TEXT_FONT_PATH);
    cfg.rectHeight = GetOr(tbl, "rectHeight", ConfigDefaults::EYEZOOM_RECT_HEIGHT);
    cfg.linkRectToFont = GetOr(tbl, "linkRectToFont", ConfigDefaults::EYEZOOM_LINK_RECT_TO_FONT);
    cfg.gridColor1 = ColorFromTomlArray(GetArray(tbl, "gridColor1"), { 0.2f, 0.2f, 0.2f });
    cfg.gridColor1Opacity = GetOr(tbl, "gridColor1Opacity", 1.0f);
    cfg.gridColor2 = ColorFromTomlArray(GetArray(tbl, "gridColor2"), { 0.3f, 0.3f, 0.3f });
    cfg.gridColor2Opacity = GetOr(tbl, "gridColor2Opacity", 1.0f);
    cfg.centerLineColor = ColorFromTomlArray(GetArray(tbl, "centerLineColor"), { 1.0f, 0.0f, 0.0f });
    cfg.centerLineColorOpacity = GetOr(tbl, "centerLineColorOpacity", 1.0f);
    cfg.textColor = ColorFromTomlArray(GetArray(tbl, "textColor"), { 1.0f, 1.0f, 1.0f });
    cfg.textColorOpacity = GetOr(tbl, "textColorOpacity", 1.0f);
    cfg.slideZoomIn = GetOr(tbl, "slideZoomIn", false);
    cfg.slideMirrorsIn = GetOr(tbl, "slideMirrorsIn", false);
    cfg.activeOverlayIndex = GetOr(tbl, "activeOverlayIndex", -1);

    cfg.overlays.clear();
    if (auto arr = GetArray(tbl, "overlay")) {
        for (const auto& elem : *arr) {
            if (auto t = elem.as_table()) {
                EyeZoomOverlayConfig ov;
                ov.name = GetStringOr(*t, "name", "");
                ov.path = GetStringOr(*t, "path", "");
                std::string modeStr = GetStringOr(*t, "displayMode", "fit");
                if (modeStr == "manual")       ov.displayMode = EyeZoomOverlayDisplayMode::Manual;
                else if (modeStr == "stretch") ov.displayMode = EyeZoomOverlayDisplayMode::Stretch;
                else                           ov.displayMode = EyeZoomOverlayDisplayMode::Fit;
                ov.manualWidth = GetOr(*t, "manualWidth", 100);
                ov.manualHeight = GetOr(*t, "manualHeight", 100);
                ov.clipToZoomArea = GetOr(*t, "clipToZoomArea", false);
                ov.opacity = GetOr(*t, "opacity", 1.0f);
                cfg.overlays.push_back(std::move(ov));
            }
        }
    }

    if (cfg.activeOverlayIndex < -1 || cfg.activeOverlayIndex >= (int)cfg.overlays.size()) {
        cfg.activeOverlayIndex = -1;
    }
}

void KeyRebindToToml(const KeyRebind& cfg, toml::table& out) {
    out.insert("fromKey", static_cast<int64_t>(cfg.fromKey));
    out.insert("toKey", static_cast<int64_t>(cfg.toKey));
    out.insert("enabled", cfg.enabled);
    const std::string cursorState = NormalizeKeyRebindCursorStateId(cfg.cursorState);
    if (cursorState != kKeyRebindCursorStateAny) {
        out.insert("cursorState", cursorState);
    }
    out.insert("triggerOutputDisabled", cfg.triggerOutputDisabled);
    out.insert("useCustomOutput", cfg.useCustomOutput);
    out.insert("baseOutputDisabled", cfg.baseOutputDisabled);
    out.insert("customOutputVK", static_cast<int64_t>(cfg.customOutputVK));
    out.insert("customOutputUnicode", static_cast<int64_t>(cfg.customOutputUnicode));
    out.insert("customOutputScanCode", static_cast<int64_t>(cfg.customOutputScanCode));
    out.insert("baseOutputShifted", cfg.baseOutputShifted);
    out.insert("shiftLayerEnabled", cfg.shiftLayerEnabled);
    out.insert("shiftLayerUsesCapsLock", cfg.shiftLayerUsesCapsLock);
    out.insert("shiftLayerOutputDisabled", cfg.shiftLayerOutputDisabled);
    out.insert("shiftLayerOutputVK", static_cast<int64_t>(cfg.shiftLayerOutputVK));
    out.insert("shiftLayerOutputUnicode", static_cast<int64_t>(cfg.shiftLayerOutputUnicode));
    out.insert("shiftLayerOutputShifted", cfg.shiftLayerOutputShifted);
}

static bool TryParseUnicodeCodepointString(const std::string& in, uint32_t& outCp) {
    auto isSpace = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    std::string s = in;
    while (!s.empty() && isSpace((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && isSpace((unsigned char)s.back())) s.pop_back();
    if (s.empty()) return false;

    auto startsWithI = [&](const char* pfx) {
        size_t n = std::char_traits<char>::length(pfx);
        if (s.size() < n) return false;
        for (size_t i = 0; i < n; ++i) {
            char a = s[i];
            char b = pfx[i];
            if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
            if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');
            if (a != b) return false;
        }
        return true;
    };

    std::string hex = s;
    bool explicitCodepointNotation = false;
    if (startsWithI("U+")) {
        hex = s.substr(2);
        explicitCodepointNotation = true;
    } else if (startsWithI("\\U")) {
        hex = s.substr(2);
        explicitCodepointNotation = true;
    } else if (startsWithI("\\u")) {
        hex = s.substr(2);
        explicitCodepointNotation = true;
    } else if (startsWithI("0X")) {
        hex = s.substr(2);
        explicitCodepointNotation = true;
    }

    // Strip optional surrounding braces like "{00F8}".
    if (!hex.empty() && hex.front() == '{' && hex.back() == '}') {
        hex = hex.substr(1, hex.size() - 2);
        explicitCodepointNotation = true;
    }

    if (explicitCodepointNotation) {
        try {
            size_t idx = 0;
            unsigned long v = std::stoul(hex, &idx, 16);
            if (idx == 0 || idx != hex.size()) return false;
            if (v == 0 || v > 0x10FFFFul) return false;
            if (v >= 0xD800ul && v <= 0xDFFFul) return false;
            outCp = (uint32_t)v;
            return true;
        } catch (...) {
            return false;
        }
    }

    {
        std::wstring w = Utf8ToWide(s);
        if (!w.empty()) {
            uint32_t cp = 0;
            if (w.size() == 1) {
                cp = (uint32_t)w[0];
            } else if (w.size() == 2 && w[0] >= 0xD800 && w[0] <= 0xDBFF && w[1] >= 0xDC00 && w[1] <= 0xDFFF) {
                cp = 0x10000u + (((uint32_t)w[0] - 0xD800u) << 10) + ((uint32_t)w[1] - 0xDC00u);
            } else {
                return false;
            }

            if (cp != 0 && cp <= 0x10FFFFu && !(cp >= 0xD800u && cp <= 0xDFFFu)) {
                outCp = cp;
                return true;
            }
        }
    }

    return false;
}

void KeyRebindFromToml(const toml::table& tbl, KeyRebind& cfg) {
    cfg.fromKey = static_cast<DWORD>(GetOr<int64_t>(tbl, "fromKey", 0));
    cfg.toKey = static_cast<DWORD>(GetOr<int64_t>(tbl, "toKey", 0));
    cfg.enabled = GetOr(tbl, "enabled", ConfigDefaults::KEY_REBIND_ENABLED);
    cfg.cursorState = NormalizeKeyRebindCursorStateId(GetStringOr(tbl, "cursorState", kKeyRebindCursorStateAny));
    cfg.triggerOutputDisabled = GetOr(tbl, "triggerOutputDisabled", ConfigDefaults::KEY_REBIND_TRIGGER_OUTPUT_DISABLED);
    cfg.useCustomOutput = GetOr(tbl, "useCustomOutput", ConfigDefaults::KEY_REBIND_USE_CUSTOM_OUTPUT);
    cfg.baseOutputDisabled = GetOr(tbl, "baseOutputDisabled", ConfigDefaults::KEY_REBIND_BASE_OUTPUT_DISABLED);
    cfg.customOutputVK = static_cast<DWORD>(GetOr<int64_t>(tbl, "customOutputVK", ConfigDefaults::KEY_REBIND_CUSTOM_OUTPUT_VK));
    cfg.customOutputUnicode = ConfigDefaults::KEY_REBIND_CUSTOM_OUTPUT_UNICODE;
    if (auto u = tbl["customOutputUnicode"]) {
        if (auto v = u.value<int64_t>()) {
            uint64_t vv = (uint64_t)*v;
            if (vv <= 0x10FFFFull && vv != 0 && !(vv >= 0xD800ull && vv <= 0xDFFFull)) {
                cfg.customOutputUnicode = (DWORD)vv;
            }
        } else if (auto s = u.value<std::string>()) {
            uint32_t cp = 0;
            if (TryParseUnicodeCodepointString(*s, cp)) {
                cfg.customOutputUnicode = (DWORD)cp;
            }
        }
    }
    cfg.customOutputScanCode =
        static_cast<DWORD>(GetOr<int64_t>(tbl, "customOutputScanCode", ConfigDefaults::KEY_REBIND_CUSTOM_OUTPUT_SCANCODE));
    cfg.baseOutputShifted = GetOr(tbl, "baseOutputShifted", ConfigDefaults::KEY_REBIND_BASE_OUTPUT_SHIFTED);
    cfg.shiftLayerEnabled = GetOr(tbl, "shiftLayerEnabled", ConfigDefaults::KEY_REBIND_SHIFT_LAYER_ENABLED);
    cfg.shiftLayerUsesCapsLock =
        GetOr(tbl, "shiftLayerUsesCapsLock", ConfigDefaults::KEY_REBIND_SHIFT_LAYER_USES_CAPS_LOCK);
    cfg.shiftLayerOutputDisabled =
        GetOr(tbl, "shiftLayerOutputDisabled", ConfigDefaults::KEY_REBIND_SHIFT_LAYER_OUTPUT_DISABLED);
    cfg.shiftLayerOutputVK =
        static_cast<DWORD>(GetOr<int64_t>(tbl, "shiftLayerOutputVK", ConfigDefaults::KEY_REBIND_SHIFT_LAYER_OUTPUT_VK));
    cfg.shiftLayerOutputUnicode = ConfigDefaults::KEY_REBIND_SHIFT_LAYER_OUTPUT_UNICODE;
    if (auto su = tbl["shiftLayerOutputUnicode"]) {
        if (auto v = su.value<int64_t>()) {
            uint64_t vv = (uint64_t)*v;
            if (vv <= 0x10FFFFull && vv != 0 && !(vv >= 0xD800ull && vv <= 0xDFFFull)) {
                cfg.shiftLayerOutputUnicode = (DWORD)vv;
            }
        } else if (auto s = su.value<std::string>()) {
            uint32_t cp = 0;
            if (TryParseUnicodeCodepointString(*s, cp)) {
                cfg.shiftLayerOutputUnicode = (DWORD)cp;
            }
        }
    }
    cfg.shiftLayerOutputShifted =
        GetOr(tbl, "shiftLayerOutputShifted", ConfigDefaults::KEY_REBIND_SHIFT_LAYER_OUTPUT_SHIFTED);
}

void KeyRebindsConfigToToml(const KeyRebindsConfig& cfg, toml::table& out) {
    out.insert("enabled", cfg.enabled);
    out.insert("resolveRebindTargetsForHotkeys", cfg.resolveRebindTargetsForHotkeys);
    out.insert("allowSystemAltTab", cfg.allowSystemAltTab);
    out.insert("indicatorMode", cfg.indicatorMode);
    out.insert("indicatorPosition", cfg.indicatorPosition);
    if (!cfg.indicatorImageEnabled.empty()) out.insert("indicatorImageEnabled", cfg.indicatorImageEnabled);
    if (!cfg.indicatorImageDisabled.empty()) out.insert("indicatorImageDisabled", cfg.indicatorImageDisabled);
    out.insert("allowSystemAltF4", cfg.allowSystemAltF4);
    out.insert("suppressCapsLockToggle", cfg.suppressCapsLockToggle);

    toml::array toggleHotkeyArr;
    for (const auto& key : cfg.toggleHotkey) { toggleHotkeyArr.push_back(static_cast<int64_t>(key)); }
    out.insert("toggleHotkey", toggleHotkeyArr);

    toml::array layoutExtraKeysArr;
    for (const auto& key : cfg.layoutExtraKeys) { layoutExtraKeysArr.push_back(static_cast<int64_t>(key)); }
    out.insert("layoutExtraKeys", layoutExtraKeysArr);

    toml::array rebindsArr;
    for (const auto& rebind : cfg.rebinds) {
        toml::table rebindTbl;
        KeyRebindToToml(rebind, rebindTbl);
        rebindsArr.push_back(rebindTbl);
    }
    out.insert("rebinds", rebindsArr);
}

void KeyRebindsConfigFromToml(const toml::table& tbl, KeyRebindsConfig& cfg) {
    cfg.enabled = GetOr(tbl, "enabled", ConfigDefaults::KEY_REBINDS_ENABLED);
    cfg.resolveRebindTargetsForHotkeys =
        GetOr(tbl, "resolveRebindTargetsForHotkeys", ConfigDefaults::KEY_REBINDS_RESOLVE_REBIND_TARGETS_FOR_HOTKEYS);
    cfg.allowSystemAltTab = GetOr(tbl, "allowSystemAltTab", ConfigDefaults::KEY_REBINDS_ALLOW_SYSTEM_ALT_TAB);
    if (tbl.contains("indicatorMode")) {
        cfg.indicatorMode = GetOr(tbl, "indicatorMode", ConfigDefaults::KEY_REBINDS_INDICATOR_MODE);
    } else if (tbl.contains("showIndicator")) {
        cfg.indicatorMode = GetOr(tbl, "showIndicator", false) ? 1 : 0;
    } else {
        cfg.indicatorMode = ConfigDefaults::KEY_REBINDS_INDICATOR_MODE;
    }
    cfg.indicatorPosition = GetOr(tbl, "indicatorPosition", ConfigDefaults::KEY_REBINDS_INDICATOR_POSITION);
    cfg.indicatorImageEnabled = GetStringOr(tbl, "indicatorImageEnabled", "");
    cfg.indicatorImageDisabled = GetStringOr(tbl, "indicatorImageDisabled", "");
    cfg.allowSystemAltF4 = GetOr(tbl, "allowSystemAltF4", ConfigDefaults::KEY_REBINDS_ALLOW_SYSTEM_ALT_F4);
    cfg.suppressCapsLockToggle =
        GetOr(tbl, "suppressCapsLockToggle", ConfigDefaults::KEY_REBINDS_SUPPRESS_CAPS_LOCK_TOGGLE);

    cfg.toggleHotkey.clear();
    const bool hasToggleHotkey = tbl.contains("toggleHotkey");
    if (auto arr = GetArray(tbl, "toggleHotkey")) {
        for (const auto& elem : *arr) {
            if (auto val = elem.value<int64_t>()) { cfg.toggleHotkey.push_back(static_cast<DWORD>(*val)); }
        }
    }
    if (!hasToggleHotkey) { cfg.toggleHotkey = ConfigDefaults::GetDefaultKeyRebindsToggleHotkey(); }

    cfg.layoutExtraKeys.clear();
    if (auto arr = GetArray(tbl, "layoutExtraKeys")) {
        for (const auto& elem : *arr) {
            if (auto val = elem.value<int64_t>()) {
                const DWORD vk = static_cast<DWORD>(*val);
                if (vk == 0) continue;
                if (std::find(cfg.layoutExtraKeys.begin(), cfg.layoutExtraKeys.end(), vk) != cfg.layoutExtraKeys.end()) continue;
                cfg.layoutExtraKeys.push_back(vk);
            }
        }
    }

    cfg.rebinds.clear();
    if (auto arr = GetArray(tbl, "rebinds")) {
        for (const auto& elem : *arr) {
            if (auto t = elem.as_table()) {
                KeyRebind rebind;
                KeyRebindFromToml(*t, rebind);
                cfg.rebinds.push_back(rebind);
            }
        }
    }
}


void AppearanceConfigToToml(const AppearanceConfig& cfg, toml::table& out) {
    out.insert("theme", cfg.theme);
    out.insert("guiFontScale", cfg.guiFontScale);

    if (!cfg.customColors.empty()) {
        toml::table colorsTbl;
        for (const auto& [name, color] : cfg.customColors) {
            if (!IsValidAppearanceCustomColorKey(name)) {
                continue;
            }
            colorsTbl.insert(name, ColorToTomlArray(color));
        }
        if (!colorsTbl.empty()) {
            out.insert("customColors", colorsTbl);
        }
    }
}

void AppearanceConfigFromToml(const toml::table& tbl, AppearanceConfig& cfg) {
    cfg.theme = GetStringOr(tbl, "theme", "Dark");
    cfg.guiFontScale = std::clamp(GetOr(tbl, "guiFontScale", ConfigDefaults::CONFIG_GUI_FONT_SCALE), 0.75f, 2.0f);

    cfg.customColors.clear();
    if (auto colorsTbl = GetTable(tbl, "customColors")) {
        for (const auto& [key, value] : *colorsTbl) {
            const std::string keyName(key.str());
            if (!IsValidAppearanceCustomColorKey(keyName)) {
                continue;
            }
            if (auto arr = value.as_array(); IsValidAppearanceCustomColorArray(arr)) {
                cfg.customColors[keyName] = ColorFromTomlArray(arr);
            }
        }
    }
}

static int ClampObsFramerateConfigValue(int value) {
    if (value < 15) { return 15; }
    if (value > 120) { return 120; }
    return value;
}

static int ClampKeyRepeatStartDelayConfigValue(int value) {
    if (value < 0) {
        return -1;
    }
    if (value < 100) {
        return 100;
    }

    value = (std::min)(value, 300);
    return 100 + (((value - 100) + 2) / 5) * 5;
}

static int ClampKeyRepeatDelayConfigValue(int value, bool useSystemKeyRepeat) {
    if (value < 0) {
        return -1;
    }

    const int maxValue = useSystemKeyRepeat ? 300 : 50;
    if (value > maxValue) {
        value = maxValue;
    }

    return (std::max)(value, 1);
}

void ConfigToToml(const Config& config, toml::table& out) {
    out.insert("configVersion", config.configVersion);
    out.insert("disableHookChaining", config.disableHookChaining);
    out.insert("defaultMode", config.defaultMode);
    out.insert("fontPath", config.fontPath);
    out.insert("lang", config.lang);
    out.insert("fpsLimit", config.fpsLimit);
    out.insert("fpsLimitSleepThreshold", config.fpsLimitSleepThreshold);
    out.insert("mirrorMatchColorspace", MirrorGammaModeToString(config.mirrorGammaMode));
    out.insert("allowCursorEscape", config.allowCursorEscape);
    out.insert("confineCursor", config.confineCursor);
    out.insert("mouseSensitivity", config.mouseSensitivity);
    out.insert("windowsMouseSpeed", config.windowsMouseSpeed);
    out.insert("hideAnimationsInGame", config.hideAnimationsInGame);
    out.insert("captureFakeCursor", config.captureFakeCursor);
    out.insert("limitCaptureFramerate", config.limitCaptureFramerate);
    out.insert("obsFramerate", config.obsFramerate);
    out.insert("keyRepeatStartDelay", ClampKeyRepeatStartDelayConfigValue(config.keyRepeatStartDelay));
    out.insert("keyRepeatDelay", ClampKeyRepeatDelayConfigValue(config.keyRepeatDelay, true));
    out.insert("basicModeEnabled", config.basicModeEnabled);
    out.insert("restoreWindowedModeOnFullscreenExit", config.restoreWindowedModeOnFullscreenExit);
    out.insert("disableFullscreenPrompt", config.disableFullscreenPrompt);
    out.insert("disableConfigurePrompt", config.disableConfigurePrompt);

    toml::array guiHotkeyArr;
    for (const auto& key : config.guiHotkey) { guiHotkeyArr.push_back(static_cast<int64_t>(key)); }
    out.insert("guiHotkey", guiHotkeyArr);

    toml::array borderlessHotkeyArr;
    for (const auto& key : config.borderlessHotkey) { borderlessHotkeyArr.push_back(static_cast<int64_t>(key)); }
    out.insert("borderlessHotkey", borderlessHotkeyArr);
    out.insert("autoBorderless", config.autoBorderless);

    toml::array imageOverlaysHotkeyArr;
    for (const auto& key : config.imageOverlaysHotkey) { imageOverlaysHotkeyArr.push_back(static_cast<int64_t>(key)); }
    out.insert("imageOverlaysHotkey", imageOverlaysHotkeyArr);

    toml::array windowOverlaysHotkeyArr;
    for (const auto& key : config.windowOverlaysHotkey) { windowOverlaysHotkeyArr.push_back(static_cast<int64_t>(key)); }
    out.insert("windowOverlaysHotkey", windowOverlaysHotkeyArr);

    toml::array ninjabrainOverlayHotkeyArr;
    for (const auto& key : config.ninjabrainOverlayHotkey) { ninjabrainOverlayHotkeyArr.push_back(static_cast<int64_t>(key)); }
    out.insert("ninjabrainOverlayHotkey", ninjabrainOverlayHotkeyArr);

    toml::table debugTbl;
    DebugGlobalConfigToToml(config.debug, debugTbl);
    out.insert("debug", debugTbl);

    toml::table eyezoomTbl;
    EyeZoomConfigToToml(config.eyezoom, eyezoomTbl);
    out.insert("eyezoom", eyezoomTbl);

    // NinjabrainBot overlay config
    {
        toml::table nb;
        auto& o = config.ninjabrainOverlay;
        nb.insert("enabled",              o.enabled);
        nb.insert("x",                    o.x);
        nb.insert("y",                    o.y);
        nb.insert("relativeTo",           o.relativeTo);
        nb.insert("apiBaseUrl",           o.apiBaseUrl);
        nb.insert("bgEnabled",            o.bgEnabled);
        nb.insert("bgOpacity",            o.bgOpacity);
        nb.insert("bgColor",              ColorToTomlArray(o.bgColor));
        nb.insert("layoutStyle",          o.layoutStyle);
        nb.insert("titleText",            o.titleText);
        nb.insert("showTitleBar",         o.showTitleBar);
        nb.insert("showWindowControls",   o.showWindowControls);
        nb.insert("showThrowDetails",     o.showThrowDetails);
        nb.insert("showDirectionToStronghold", o.showDirectionToStronghold);
        nb.insert("staticColumnWidths",   o.staticColumnWidths);
        nb.insert("showSeparators",       o.showSeparators);
        nb.insert("showRowStripes",       o.showRowStripes);
        nb.insert("borderWidth",          o.borderWidth);
        nb.insert("borderRadius",         o.borderRadius);
        nb.insert("cornerRadius",         o.cornerRadius);
        nb.insert("headerFillColor",      ColorToTomlArray(o.headerFillColor));
        nb.insert("coordsDisplay",        o.coordsDisplay);
        nb.insert("chromeColor",          ColorToTomlArray(o.chromeColor));
        nb.insert("borderColor",          ColorToTomlArray(o.borderColor));
        nb.insert("dividerColor",         ColorToTomlArray(o.dividerColor));
        nb.insert("headerDividerColor",   ColorToTomlArray(o.headerDividerColor));
        nb.insert("accentColor",          ColorToTomlArray(o.accentColor));
        nb.insert("buttonColor",          ColorToTomlArray(o.buttonColor));
        nb.insert("outlineWidth",         o.outlineWidth);
        nb.insert("outlineColor",         ColorToTomlArray(o.outlineColor));
        nb.insert("textColor",            ColorToTomlArray(o.textColor));
        nb.insert("dataColor",            ColorToTomlArray(o.dataColor));
        nb.insert("titleTextColor",       ColorToTomlArray(o.titleTextColor));
        nb.insert("throwsTextColor",      ColorToTomlArray(o.throwsTextColor));
        nb.insert("divineTextColor",      ColorToTomlArray(o.divineTextColor));
        nb.insert("versionTextColor",     ColorToTomlArray(o.versionTextColor));
        nb.insert("throwsBackgroundColor", ColorToTomlArray(o.throwsBackgroundColor));
        nb.insert("coordPositiveColor",   ColorToTomlArray(o.coordPositiveColor));
        nb.insert("coordNegativeColor",   ColorToTomlArray(o.coordNegativeColor));
        nb.insert("certaintyColor",       ColorToTomlArray(o.certaintyColor));
        nb.insert("certaintyMidColor",    ColorToTomlArray(o.certaintyMidColor));
        nb.insert("certaintyLowColor",    ColorToTomlArray(o.certaintyLowColor));
        nb.insert("subpixelPositiveColor", ColorToTomlArray(o.subpixelPositiveColor));
        nb.insert("subpixelNegativeColor", ColorToTomlArray(o.subpixelNegativeColor));
        nb.insert("showEyeOverlay",       o.showEyeOverlay);
        nb.insert("shownPredictions",     o.shownPredictions);
        nb.insert("showAllPreds",         o.showAllPreds);
        nb.insert("alwaysShow",           o.alwaysShow);
        nb.insert("alwaysShowBoat",       o.alwaysShowBoat);
        nb.insert("showBoatStateInTopBar", o.showBoatStateInTopBar);
        nb.insert("boatStateSize",        o.boatStateSize);
        nb.insert("boatStateMarginRight", o.boatStateMarginRight);
        nb.insert("angleDisplay",         o.angleDisplay);
        nb.insert("fontAntialiasing",     o.fontAntialiasing);
        nb.insert("rowSpacing",           o.rowSpacing);
        nb.insert("colSpacing",           o.colSpacing);
        nb.insert("sidePadding",          o.sidePadding);
        nb.insert("sectionLayoutMode",    o.sectionLayoutMode);
        nb.insert("contentPaddingTop",    o.contentPaddingTop);
        nb.insert("contentPaddingBottom", o.contentPaddingBottom);
        nb.insert("resultsMarginLeft",    o.resultsMarginLeft);
        nb.insert("resultsMarginRight",   o.resultsMarginRight);
        nb.insert("resultsMarginTop",     o.resultsMarginTop);
        nb.insert("resultsMarginBottom",  o.resultsMarginBottom);
        nb.insert("resultsHeaderPaddingY", o.resultsHeaderPaddingY);
        nb.insert("resultsColumnGap",     o.resultsColumnGap);
        nb.insert("resultsAnchor",        o.resultsAnchor);
        nb.insert("resultsOffsetX",       o.resultsOffsetX);
        nb.insert("resultsOffsetY",       o.resultsOffsetY);
        nb.insert("resultsDrawOrder",     o.resultsDrawOrder);
        nb.insert("informationMessagesPlacement",   o.informationMessagesPlacement);
        nb.insert("informationMessagesFontScale",   o.informationMessagesFontScale);
        nb.insert("informationMessagesMinWidth",    o.informationMessagesMinWidth);
        nb.insert("informationMessagesMarginLeft",  o.informationMessagesMarginLeft);
        nb.insert("informationMessagesMarginRight", o.informationMessagesMarginRight);
        nb.insert("informationMessagesMarginTop",   o.informationMessagesMarginTop);
        nb.insert("informationMessagesMarginBottom", o.informationMessagesMarginBottom);
        nb.insert("informationMessagesIconTextMargin", o.informationMessagesIconTextMargin);
        nb.insert("informationMessagesIconScale", o.informationMessagesIconScale);
        nb.insert("informationMessagesAnchor",      o.informationMessagesAnchor);
        nb.insert("informationMessagesOffsetX",     o.informationMessagesOffsetX);
        nb.insert("informationMessagesOffsetY",     o.informationMessagesOffsetY);
        nb.insert("informationMessagesDrawOrder",   o.informationMessagesDrawOrder);
        nb.insert("throwsMarginLeft",     o.throwsMarginLeft);
        nb.insert("throwsMarginRight",    o.throwsMarginRight);
        nb.insert("throwsMarginTop",      o.throwsMarginTop);
        nb.insert("throwsMarginBottom",   o.throwsMarginBottom);
        nb.insert("throwsHeaderPaddingY", o.throwsHeaderPaddingY);
        nb.insert("throwsRowPaddingY",    o.throwsRowPaddingY);
        nb.insert("eyeThrowRows",         o.eyeThrowRows);
        nb.insert("throwsAnchor",         o.throwsAnchor);
        nb.insert("throwsOffsetX",        o.throwsOffsetX);
        nb.insert("throwsOffsetY",        o.throwsOffsetY);
        nb.insert("throwsDrawOrder",      o.throwsDrawOrder);
        nb.insert("failureMarginLeft",    o.failureMarginLeft);
        nb.insert("failureMarginRight",   o.failureMarginRight);
        nb.insert("failureMarginTop",     o.failureMarginTop);
        nb.insert("failureMarginBottom",  o.failureMarginBottom);
        nb.insert("failureLineGap",       o.failureLineGap);
        nb.insert("failureAnchor",        o.failureAnchor);
        nb.insert("failureOffsetX",       o.failureOffsetX);
        nb.insert("failureOffsetY",       o.failureOffsetY);
        nb.insert("failureDrawOrder",     o.failureDrawOrder);
        nb.insert("blindMarginLeft",      o.blindMarginLeft);
        nb.insert("blindMarginRight",     o.blindMarginRight);
        nb.insert("blindMarginTop",       o.blindMarginTop);
        nb.insert("blindMarginBottom",    o.blindMarginBottom);
        nb.insert("blindLineGap",         o.blindLineGap);
        nb.insert("blindAnchor",          o.blindAnchor);
        nb.insert("blindOffsetX",         o.blindOffsetX);
        nb.insert("blindOffsetY",         o.blindOffsetY);
        nb.insert("blindDrawOrder",       o.blindDrawOrder);
        nb.insert("customFontPath",       o.customFontPath);
        nb.insert("overlayOpacity",       o.overlayOpacity);
        nb.insert("overlayScale",         o.overlayScale);
        nb.insert("onlyOnMyScreen",       o.onlyOnMyScreen);
        nb.insert("onlyOnObs",            o.onlyOnObs);
        nb.insert("hideIfStale",          o.hideIfStale);
        nb.insert("hideIfStaleDelaySeconds", o.hideIfStaleDelaySeconds);
        { toml::array arr; for (auto& m : o.allowedModes) arr.push_back(m); nb.insert("allowedModes", arr); }
        toml::array colArr;
        for (auto& col : o.columns) {
            toml::table ct;
            ct.insert("id",     col.id);
            ct.insert("header", col.header);
            ct.insert("show",   col.show);
            ct.insert("staticWidth", col.staticWidth);
            colArr.push_back(ct);
        }
        nb.insert("columns", colArr);
        out.insert("ninjabrainOverlay", nb);
    }

    toml::table cursorsTbl;
    CursorsConfigToToml(config.cursors, cursorsTbl);
    out.insert("cursors", cursorsTbl);

    toml::table cursorTrailTbl;
    CursorTrailConfigToToml(config.cursorTrail, cursorTrailTbl);
    out.insert("cursorTrail", cursorTrailTbl);

    toml::table keyRebindsTbl;
    KeyRebindsConfigToToml(config.keyRebinds, keyRebindsTbl);
    out.insert("keyRebinds", keyRebindsTbl);

    toml::table appearanceTbl;
    AppearanceConfigToToml(config.appearance, appearanceTbl);
    out.insert("appearance", appearanceTbl);

    toml::array modesArr;
    for (const auto& mode : config.modes) {
        toml::table modeTbl;
        ModeConfigToToml(mode, modeTbl);
        modesArr.push_back(modeTbl);
    }
    out.insert("mode", modesArr);

    toml::array mirrorsArr;
    for (const auto& mirror : config.mirrors) {
        toml::table mirrorTbl;
        MirrorConfigToToml(mirror, mirrorTbl);
        mirrorsArr.push_back(mirrorTbl);
    }
    out.insert("mirror", mirrorsArr);

    toml::array mirrorGroupsArr;
    for (const auto& group : config.mirrorGroups) {
        toml::table groupTbl;
        MirrorGroupConfigToToml(group, groupTbl);
        mirrorGroupsArr.push_back(groupTbl);
    }
    out.insert("mirrorGroup", mirrorGroupsArr);

    toml::array imagesArr;
    for (const auto& image : config.images) {
        toml::table imageTbl;
        ImageConfigToToml(image, imageTbl);
        imagesArr.push_back(imageTbl);
    }
    out.insert("image", imagesArr);

    toml::array windowOverlaysArr;
    for (const auto& overlay : config.windowOverlays) {
        toml::table overlayTbl;
        WindowOverlayConfigToToml(overlay, overlayTbl);
        windowOverlaysArr.push_back(overlayTbl);
    }
    out.insert("windowOverlay", windowOverlaysArr);

    toml::array browserOverlaysArr;
    for (const auto& overlay : config.browserOverlays) {
        toml::table overlayTbl;
        BrowserOverlayConfigToToml(overlay, overlayTbl);
        browserOverlaysArr.push_back(overlayTbl);
    }
    out.insert("browserOverlay", browserOverlaysArr);

    toml::array hotkeysArr;
    for (const auto& hotkey : config.hotkeys) {
        toml::table hotkeyTbl;
        HotkeyConfigToToml(hotkey, hotkeyTbl);
        hotkeysArr.push_back(hotkeyTbl);
    }
    out.insert("hotkey", hotkeysArr);

    toml::array sensitivityHotkeysArr;
    for (const auto& sensHotkey : config.sensitivityHotkeys) {
        toml::table sensHotkeyTbl;
        SensitivityHotkeyConfigToToml(sensHotkey, sensHotkeyTbl);
        sensitivityHotkeysArr.push_back(sensHotkeyTbl);
    }
    out.insert("sensitivityHotkey", sensitivityHotkeysArr);
}

void ConfigFromToml(const toml::table& tbl, Config& config) {
    config.configVersion = GetOr(tbl, "configVersion", ConfigDefaults::DEFAULT_CONFIG_VERSION);
    const int originalConfigVersion = config.configVersion;
    config.disableHookChaining = GetOr(tbl, "disableHookChaining", ConfigDefaults::CONFIG_DISABLE_HOOK_CHAINING);
    config.defaultMode = GetStringOr(tbl, "defaultMode", ConfigDefaults::CONFIG_DEFAULT_MODE);
    config.fontPath = GetStringOr(tbl, "fontPath", ConfigDefaults::CONFIG_DEFAULT_GUI_FONT_PATH);
    config.lang = GetStringOr(tbl, "lang", ConfigDefaults::CONFIG_LANG);
    config.fpsLimit = GetOr(tbl, "fpsLimit", ConfigDefaults::CONFIG_FPS_LIMIT);
    config.fpsLimitSleepThreshold = GetOr(tbl, "fpsLimitSleepThreshold", ConfigDefaults::CONFIG_FPS_LIMIT_SLEEP_THRESHOLD);
    bool hasGlobalMirrorMatchColorspace = tbl.contains("mirrorMatchColorspace");
    config.mirrorGammaMode = StringToMirrorGammaMode(
        GetStringOr(tbl, "mirrorMatchColorspace", ConfigDefaults::CONFIG_MIRROR_MATCH_COLORSPACE));
    config.allowCursorEscape = GetOr(tbl, "allowCursorEscape", ConfigDefaults::CONFIG_ALLOW_CURSOR_ESCAPE);
    config.confineCursor = GetOr(tbl, "confineCursor", false);
    config.mouseSensitivity = GetOr(tbl, "mouseSensitivity", ConfigDefaults::CONFIG_MOUSE_SENSITIVITY);
    config.windowsMouseSpeed = GetOr(tbl, "windowsMouseSpeed", ConfigDefaults::CONFIG_WINDOWS_MOUSE_SPEED);
    config.hideAnimationsInGame = GetOr(tbl, "hideAnimationsInGame", ConfigDefaults::CONFIG_HIDE_ANIMATIONS_IN_GAME);
    config.captureFakeCursor = GetOr(tbl, "captureFakeCursor", ConfigDefaults::CONFIG_CAPTURE_FAKE_CURSOR);
    config.limitCaptureFramerate = GetOr(tbl, "limitCaptureFramerate", ConfigDefaults::CONFIG_LIMIT_CAPTURE_FRAMERATE);
    config.obsFramerate = ClampObsFramerateConfigValue(GetOr(tbl, "obsFramerate", ConfigDefaults::CONFIG_OBS_FRAMERATE));
    config.useSystemKeyRepeat = true;
    int keyRepeatStartDelay = GetOr(tbl, "keyRepeatStartDelay", ConfigDefaults::CONFIG_KEY_REPEAT_START_DELAY);
    int keyRepeatDelay = GetOr(tbl, "keyRepeatDelay", ConfigDefaults::CONFIG_KEY_REPEAT_DELAY);
    if (originalConfigVersion < ConfigDefaults::DEFAULT_CONFIG_VERSION) {
        if (keyRepeatStartDelay == 0) { keyRepeatStartDelay = ConfigDefaults::CONFIG_KEY_REPEAT_START_DELAY; }
        if (keyRepeatDelay == 0) { keyRepeatDelay = ConfigDefaults::CONFIG_KEY_REPEAT_DELAY; }
    }
    config.keyRepeatStartDelay = ClampKeyRepeatStartDelayConfigValue(keyRepeatStartDelay);
    config.keyRepeatDelay = ClampKeyRepeatDelayConfigValue(keyRepeatDelay, true);
    config.basicModeEnabled = GetOr(tbl, "basicModeEnabled", ConfigDefaults::CONFIG_BASIC_MODE_ENABLED);
    config.restoreWindowedModeOnFullscreenExit =
        GetOr(tbl, "restoreWindowedModeOnFullscreenExit", ConfigDefaults::CONFIG_RESTORE_WINDOWED_MODE_ON_FULLSCREEN_EXIT);
    config.disableFullscreenPrompt = GetOr(tbl, "disableFullscreenPrompt", ConfigDefaults::CONFIG_DISABLE_FULLSCREEN_PROMPT);
    config.disableConfigurePrompt = GetOr(tbl, "disableConfigurePrompt", ConfigDefaults::CONFIG_DISABLE_CONFIGURE_PROMPT);

    config.guiHotkey.clear();
    if (auto arr = GetArray(tbl, "guiHotkey")) {
        for (const auto& elem : *arr) {
            if (auto val = elem.value<int64_t>()) { config.guiHotkey.push_back(static_cast<DWORD>(*val)); }
        }
    }
    if (config.guiHotkey.empty()) { config.guiHotkey = ConfigDefaults::GetDefaultGuiHotkey(); }

    config.borderlessHotkey.clear();
    const bool hasBorderlessHotkey = tbl.contains("borderlessHotkey");
    if (auto arr = GetArray(tbl, "borderlessHotkey")) {
        for (const auto& elem : *arr) {
            if (auto val = elem.value<int64_t>()) { config.borderlessHotkey.push_back(static_cast<DWORD>(*val)); }
        }
    }
    if (!hasBorderlessHotkey) { config.borderlessHotkey = ConfigDefaults::GetDefaultBorderlessHotkey(); }
    config.autoBorderless = GetOr(tbl, "autoBorderless", ConfigDefaults::CONFIG_AUTO_BORDERLESS);

    config.imageOverlaysHotkey.clear();
    const bool hasImageOverlaysHotkey = tbl.contains("imageOverlaysHotkey");
    if (auto arr = GetArray(tbl, "imageOverlaysHotkey")) {
        for (const auto& elem : *arr) {
            if (auto val = elem.value<int64_t>()) { config.imageOverlaysHotkey.push_back(static_cast<DWORD>(*val)); }
        }
    }
    if (!hasImageOverlaysHotkey) { config.imageOverlaysHotkey = ConfigDefaults::GetDefaultImageOverlaysHotkey(); }

    config.windowOverlaysHotkey.clear();
    const bool hasWindowOverlaysHotkey = tbl.contains("windowOverlaysHotkey");
    if (auto arr = GetArray(tbl, "windowOverlaysHotkey")) {
        for (const auto& elem : *arr) {
            if (auto val = elem.value<int64_t>()) { config.windowOverlaysHotkey.push_back(static_cast<DWORD>(*val)); }
        }
    }
    if (!hasWindowOverlaysHotkey) { config.windowOverlaysHotkey = ConfigDefaults::GetDefaultWindowOverlaysHotkey(); }

    config.ninjabrainOverlayHotkey.clear();
    const bool hasNinjabrainOverlayHotkey = tbl.contains("ninjabrainOverlayHotkey");
    if (auto arr = GetArray(tbl, "ninjabrainOverlayHotkey")) {
        for (const auto& elem : *arr) {
            if (auto val = elem.value<int64_t>()) { config.ninjabrainOverlayHotkey.push_back(static_cast<DWORD>(*val)); }
        }
    }
    if (!hasNinjabrainOverlayHotkey) { config.ninjabrainOverlayHotkey = ConfigDefaults::GetDefaultNinjabrainOverlayHotkey(); }

    if (auto t = GetTable(tbl, "debug")) { DebugGlobalConfigFromToml(*t, config.debug); }

    if (auto t = GetTable(tbl, "eyezoom")) { EyeZoomConfigFromToml(*t, config.eyezoom); }

    // NinjabrainBot overlay config
    if (auto nb = tbl.get_as<toml::table>("ninjabrainOverlay")) {
        auto& c = config.ninjabrainOverlay;
        c.enabled              = GetOr(*nb, "enabled",              false);
        c.x                    = GetOr(*nb, "x",                    5);
        c.y                    = GetOr(*nb, "y",                    -5);
        c.relativeTo           = GetStringOr(*nb, "relativeTo",     "bottomLeftScreen");
        c.apiBaseUrl           = GetStringOr(*nb, "apiBaseUrl",     ConfigDefaults::CONFIG_NINJABRAIN_API_BASE_URL);
        c.bgEnabled            = GetOr(*nb, "bgEnabled",            true);
        c.bgOpacity            = GetOr(*nb, "bgOpacity",            0.92f);
        c.bgColor              = ColorFromTomlArray(nb->get_as<toml::array>("bgColor"),        Color{0.15f,0.16f,0.17f,1.0f});
        c.layoutStyle          = GetStringOr(*nb, "layoutStyle",          "compact");
        c.layoutStyle          = "compact";
        c.titleText            = GetStringOr(*nb, "titleText",            "Ninjabrain Bot");
        c.showTitleBar         = GetOr(*nb, "showTitleBar",         false);
        c.showWindowControls   = GetOr(*nb, "showWindowControls",   false);
        c.showThrowDetails     = GetOr(*nb, "showThrowDetails",     true);
        c.showDirectionToStronghold = GetOr(*nb, "showDirectionToStronghold", true);
        c.staticColumnWidths   = GetOr(*nb, "staticColumnWidths",   true);
        c.showSeparators       = GetOr(*nb, "showSeparators",       true);
        c.showRowStripes       = GetOr(*nb, "showRowStripes",       true);
        c.borderWidth          = GetOr(*nb, "borderWidth",          1);
        c.cornerRadius         = GetOr(*nb, "cornerRadius",         0.0f);
        c.borderRadius         = GetOr(*nb, "borderRadius",         c.cornerRadius);
        c.chromeColor          = ColorFromTomlArray(nb->get_as<toml::array>("chromeColor"),    Color{0.18f,0.20f,0.24f,1.0f});
        c.borderColor          = ColorFromTomlArray(nb->get_as<toml::array>("borderColor"),    Color{0.28f,0.29f,0.31f,1.0f});
        c.dividerColor         = ColorFromTomlArray(nb->get_as<toml::array>("dividerColor"),   Color{0.24f,0.25f,0.27f,1.0f});
        c.headerDividerColor   = ColorFromTomlArray(nb->get_as<toml::array>("headerDividerColor"), c.borderColor);
        const float legacyHeaderFillMix = std::clamp(GetOr(*nb, "headerFillOpacity", 0.55f), 0.0f, 1.0f);
        const Color legacyHeaderFillColor = {
            c.bgColor.r + (c.dividerColor.r - c.bgColor.r) * legacyHeaderFillMix,
            c.bgColor.g + (c.dividerColor.g - c.bgColor.g) * legacyHeaderFillMix,
            c.bgColor.b + (c.dividerColor.b - c.bgColor.b) * legacyHeaderFillMix,
            1.0f,
        };
        c.headerFillColor      = ColorFromTomlArray(nb->get_as<toml::array>("headerFillColor"), legacyHeaderFillColor);
        c.headerFillColor.a    = 1.0f;
        c.coordsDisplay        = GetStringOr(*nb, "coordsDisplay",  "chunk");
        c.accentColor          = ColorFromTomlArray(nb->get_as<toml::array>("accentColor"),    Color{0.31f,0.86f,0.31f,1.0f});
        c.buttonColor          = ColorFromTomlArray(nb->get_as<toml::array>("buttonColor"),    Color{0.23f,0.26f,0.30f,1.0f});
        c.outlineWidth         = GetOr(*nb, "outlineWidth",         0);
        c.outlineColor         = ColorFromTomlArray(nb->get_as<toml::array>("outlineColor"),   Color{0.0f,0.0f,0.0f,0.8627f});
        c.textColor            = ColorFromTomlArray(nb->get_as<toml::array>("textColor"),      Color{0.76f,0.76f,0.76f,1.0f});
        c.dataColor            = ColorFromTomlArray(nb->get_as<toml::array>("dataColor"),      Color{1.0f,1.0f,1.0f,1.0f});
        c.titleTextColor       = ColorFromTomlArray(nb->get_as<toml::array>("titleTextColor"), c.dataColor);
        c.throwsTextColor      = ColorFromTomlArray(nb->get_as<toml::array>("throwsTextColor"), c.dataColor);
        c.divineTextColor      = ColorFromTomlArray(nb->get_as<toml::array>("divineTextColor"), c.dataColor);
        c.versionTextColor     = ColorFromTomlArray(nb->get_as<toml::array>("versionTextColor"), c.textColor);
        c.throwsBackgroundColor = ColorFromTomlArray(nb->get_as<toml::array>("throwsBackgroundColor"), c.bgColor);
        const Color legacyNegCoordColor = ColorFromTomlArray(nb->get_as<toml::array>("negCoordColor"), Color{1.0f,0.45f,0.45f,1.0f});
        const bool legacyNegCoordEnabled = GetOr(*nb, "negCoordColorEnabled", true);
        c.coordPositiveColor   = ColorFromTomlArray(nb->get_as<toml::array>("coordPositiveColor"), c.dataColor);
        c.coordNegativeColor   = ColorFromTomlArray(nb->get_as<toml::array>("coordNegativeColor"),
                                legacyNegCoordEnabled ? legacyNegCoordColor : c.dataColor);
        c.certaintyColor       = ColorFromTomlArray(nb->get_as<toml::array>("certaintyColor"), Color{0.31f,0.86f,0.31f,1.0f});
        c.certaintyMidColor    = ColorFromTomlArray(nb->get_as<toml::array>("certaintyMidColor"), Color{1.0f,0.74f,0.17f,1.0f});
        c.certaintyLowColor    = ColorFromTomlArray(nb->get_as<toml::array>("certaintyLowColor"), Color{0.97f,0.20f,0.20f,1.0f});
        c.subpixelPositiveColor = ColorFromTomlArray(nb->get_as<toml::array>("subpixelPositiveColor"), Color{0.459f,0.800f,0.424f,1.0f});
        c.subpixelNegativeColor = ColorFromTomlArray(nb->get_as<toml::array>("subpixelNegativeColor"), Color{0.800f,0.431f,0.447f,1.0f});
        c.showEyeOverlay       = GetOr(*nb, "showEyeOverlay",       true);
        c.shownPredictions     = GetOr(*nb, "shownPredictions",     5);
        c.showAllPreds         = GetOr(*nb, "showAllPreds",         false);
        c.alwaysShow           = GetOr(*nb, "alwaysShow",           false);
        c.alwaysShowBoat       = GetOr(*nb, "alwaysShowBoat",       false);
        c.showBoatStateInTopBar = GetOr(*nb, "showBoatStateInTopBar", false);
        c.boatStateSize        = GetOr(*nb, "boatStateSize",        64.0f);
        c.boatStateMarginRight = GetOr(*nb, "boatStateMarginRight", 8.0f);
        c.angleDisplay         = GetOr(*nb, "angleDisplay",         1);
        c.fontAntialiasing     = GetOr(*nb, "fontAntialiasing",     true);
        c.rowSpacing           = GetOr(*nb, "rowSpacing",           4.0f);
        c.colSpacing           = GetOr(*nb, "colSpacing",           12.0f);
        c.sidePadding          = GetOr(*nb, "sidePadding",          0.0f);
        c.sectionLayoutMode    = GetStringOr(*nb, "sectionLayoutMode", "flow");
        c.contentPaddingTop    = GetOr(*nb, "contentPaddingTop",    0.0f);
        c.contentPaddingBottom = GetOr(*nb, "contentPaddingBottom", 0.0f);
        const float legacyResultsIndentX = GetOr(*nb, "resultsIndentX", 0.0f);
        c.resultsMarginLeft    = GetOr(*nb, "resultsMarginLeft",    0.0f) + (nb->contains("resultsIndentX") ? legacyResultsIndentX : 0.0f);
        c.resultsMarginRight   = GetOr(*nb, "resultsMarginRight",   0.0f);
        c.resultsMarginTop     = GetOr(*nb, "resultsMarginTop",     0.0f);
        c.resultsMarginBottom  = GetOr(*nb, "resultsMarginBottom",  0.0f);
        c.resultsHeaderPaddingY = GetOr(*nb, "resultsHeaderPaddingY", 2.0f);
        c.resultsColumnGap     = GetOr(*nb, "resultsColumnGap",     0.0f);
        c.resultsAnchor        = GetStringOr(*nb, "resultsAnchor", "topLeft");
        c.resultsOffsetX       = GetOr(*nb, "resultsOffsetX", 0.0f);
        c.resultsOffsetY       = GetOr(*nb, "resultsOffsetY", 0.0f);
        c.resultsDrawOrder     = GetOr(*nb, "resultsDrawOrder", 0);
        c.informationMessagesPlacement = GetStringOr(*nb, "informationMessagesPlacement", "middle");
        const float legacyInformationMessagesIndentX = GetOr(*nb, "informationMessagesIndentX", 0.0f);
        const float legacyInformationMessagesPaddingX = GetOr(*nb, "informationMessagesPaddingX", 0.0f);
        const float legacyInformationMessagesPaddingY = GetOr(*nb, "informationMessagesPaddingY", 0.0f);
        c.informationMessagesFontScale = GetOr(*nb, "informationMessagesFontScale", 1.0f);
        c.informationMessagesMinWidth = GetOr(*nb, "informationMessagesMinWidth", 420.0f);
        c.informationMessagesMarginLeft = GetOr(*nb, "informationMessagesMarginLeft", 0.0f)
            + (nb->contains("informationMessagesIndentX") ? legacyInformationMessagesIndentX : 0.0f)
            + (nb->contains("informationMessagesPaddingX") ? legacyInformationMessagesPaddingX : 0.0f);
        c.informationMessagesMarginRight = GetOr(*nb, "informationMessagesMarginRight", 0.0f)
            + (nb->contains("informationMessagesPaddingX") ? legacyInformationMessagesPaddingX : 0.0f);
        c.informationMessagesMarginTop = GetOr(*nb, "informationMessagesMarginTop", 0.0f)
            + (nb->contains("informationMessagesPaddingY") ? legacyInformationMessagesPaddingY : 0.0f);
        c.informationMessagesMarginBottom = GetOr(*nb, "informationMessagesMarginBottom", 0.0f)
            + (nb->contains("informationMessagesPaddingY") ? legacyInformationMessagesPaddingY : 0.0f);
        c.informationMessagesIconTextMargin = GetOr(*nb, "informationMessagesIconTextMargin", 8.0f);
        c.informationMessagesIconScale = GetOr(*nb, "informationMessagesIconScale", 1.0f);
        c.informationMessagesAnchor = GetStringOr(*nb, "informationMessagesAnchor", "topLeft");
        c.informationMessagesOffsetX = GetOr(*nb, "informationMessagesOffsetX", 0.0f);
        c.informationMessagesOffsetY = GetOr(*nb, "informationMessagesOffsetY", 0.0f);
        c.informationMessagesDrawOrder = GetOr(*nb, "informationMessagesDrawOrder", 1);
        const float legacyThrowsIndentX = GetOr(*nb, "throwsIndentX", 0.0f);
        const float legacyThrowsSectionPaddingY = GetOr(*nb, "throwsSectionPaddingY", 0.0f);
        c.throwsMarginLeft     = GetOr(*nb, "throwsMarginLeft",     0.0f) + (nb->contains("throwsIndentX") ? legacyThrowsIndentX : 0.0f);
        c.throwsMarginRight    = GetOr(*nb, "throwsMarginRight",    0.0f);
        c.throwsMarginTop      = GetOr(*nb, "throwsMarginTop",      4.0f)
            + (nb->contains("throwsSectionPaddingY") ? legacyThrowsSectionPaddingY : 0.0f);
        c.throwsMarginBottom   = GetOr(*nb, "throwsMarginBottom",   0.0f)
            + (nb->contains("throwsSectionPaddingY") ? legacyThrowsSectionPaddingY : 0.0f);
        c.throwsHeaderPaddingY = GetOr(*nb, "throwsHeaderPaddingY", 3.0f);
        c.throwsRowPaddingY    = GetOr(*nb, "throwsRowPaddingY",    3.0f);
        c.eyeThrowRows         = GetOr(*nb, "eyeThrowRows",         3);
        c.throwsAnchor         = GetStringOr(*nb, "throwsAnchor", "topLeft");
        c.throwsOffsetX        = GetOr(*nb, "throwsOffsetX", 0.0f);
        c.throwsOffsetY        = GetOr(*nb, "throwsOffsetY", 0.0f);
        c.throwsDrawOrder      = GetOr(*nb, "throwsDrawOrder", 2);
        const float legacyFailureIndentX = GetOr(*nb, "failureIndentX", 0.0f);
        const float legacyFailurePaddingY = GetOr(*nb, "failurePaddingY", 0.0f);
        c.failureMarginLeft    = GetOr(*nb, "failureMarginLeft",    0.0f) + (nb->contains("failureIndentX") ? legacyFailureIndentX : 0.0f);
        c.failureMarginRight   = GetOr(*nb, "failureMarginRight",   0.0f);
        c.failureMarginTop     = GetOr(*nb, "failureMarginTop",     0.0f)
            + (nb->contains("failurePaddingY") ? legacyFailurePaddingY : 0.0f);
        c.failureMarginBottom  = GetOr(*nb, "failureMarginBottom",  0.0f)
            + (nb->contains("failurePaddingY") ? legacyFailurePaddingY : 0.0f);
        c.failureLineGap       = GetOr(*nb, "failureLineGap",       8.0f);
        c.failureAnchor        = GetStringOr(*nb, "failureAnchor", "topLeft");
        c.failureOffsetX       = GetOr(*nb, "failureOffsetX", 0.0f);
        c.failureOffsetY       = GetOr(*nb, "failureOffsetY", 0.0f);
        c.failureDrawOrder     = GetOr(*nb, "failureDrawOrder", 0);
        c.blindMarginLeft      = GetOr(*nb, "blindMarginLeft",     0.0f);
        c.blindMarginRight     = GetOr(*nb, "blindMarginRight",    0.0f);
        c.blindMarginTop       = GetOr(*nb, "blindMarginTop",      0.0f);
        c.blindMarginBottom    = GetOr(*nb, "blindMarginBottom",   0.0f);
        c.blindLineGap         = GetOr(*nb, "blindLineGap",        8.0f);
        c.blindAnchor          = GetStringOr(*nb, "blindAnchor", "topLeft");
        c.blindOffsetX         = GetOr(*nb, "blindOffsetX", 0.0f);
        c.blindOffsetY         = GetOr(*nb, "blindOffsetY", 0.0f);
        c.blindDrawOrder       = GetOr(*nb, "blindDrawOrder", 0);
        c.customFontPath       = GetStringOr(*nb, "customFontPath", "");
        c.overlayOpacity       = GetOr(*nb, "overlayOpacity",       1.0f);
        c.overlayScale         = GetOr(*nb, "overlayScale",         c.overlayScale);
        c.onlyOnMyScreen       = GetOr(*nb, "onlyOnMyScreen",       false);
        c.onlyOnObs            = GetOr(*nb, "onlyOnObs",            false);
        c.hideIfStale          = GetOr(*nb, "hideIfStale",          false);
        c.hideIfStaleDelaySeconds = GetOr(*nb, "hideIfStaleDelaySeconds", 10);
        if (c.layoutStyle != "compact" && c.layoutStyle != "classicWindow") { c.layoutStyle = "compact"; }
        if (c.apiBaseUrl.empty()) { c.apiBaseUrl = ConfigDefaults::CONFIG_NINJABRAIN_API_BASE_URL; }
        if (c.titleText.empty()) { c.titleText = "Ninjabrain Bot"; }
        if (c.borderWidth < 0) { c.borderWidth = 0; }
        if (c.borderRadius < 0.0f) { c.borderRadius = 0.0f; }
        if (c.cornerRadius < 0.0f) { c.cornerRadius = 0.0f; }
        if (c.coordsDisplay != "block" && c.coordsDisplay != "chunk") { c.coordsDisplay = "chunk"; }
        if (c.sidePadding < 0.0f) { c.sidePadding = 0.0f; }
        if (c.sidePadding > 200.0f) { c.sidePadding = 200.0f; }
        if (c.sectionLayoutMode != "flow" && c.sectionLayoutMode != "manual") { c.sectionLayoutMode = "flow"; }
        c.contentPaddingTop = std::clamp(c.contentPaddingTop, 0.0f, 160.0f);
        c.contentPaddingBottom = std::clamp(c.contentPaddingBottom, 0.0f, 160.0f);
        c.resultsMarginLeft = std::clamp(c.resultsMarginLeft, 0.0f, 400.0f);
        c.resultsMarginRight = std::clamp(c.resultsMarginRight, 0.0f, 400.0f);
        c.resultsMarginTop = std::clamp(c.resultsMarginTop, 0.0f, 160.0f);
        c.resultsMarginBottom = std::clamp(c.resultsMarginBottom, 0.0f, 160.0f);
        c.resultsHeaderPaddingY = std::clamp(c.resultsHeaderPaddingY, 0.0f, 48.0f);
        c.resultsColumnGap = std::clamp(c.resultsColumnGap, 0.0f, 200.0f);
        if (c.resultsAnchor != "topLeft" && c.resultsAnchor != "topRight" && c.resultsAnchor != "bottomLeft" && c.resultsAnchor != "bottomRight") {
            c.resultsAnchor = "topLeft";
        }
        c.resultsOffsetX = std::clamp(c.resultsOffsetX, 0.0f, 1000.0f);
        c.resultsOffsetY = std::clamp(c.resultsOffsetY, 0.0f, 1000.0f);
        c.resultsDrawOrder = std::clamp(c.resultsDrawOrder, 0, 32);
        c.boatStateSize = std::clamp(c.boatStateSize, 8.0f, 128.0f);
        c.boatStateMarginRight = std::clamp(c.boatStateMarginRight, 0.0f, 160.0f);
        if (c.informationMessagesPlacement != "top" && c.informationMessagesPlacement != "middle" &&
            c.informationMessagesPlacement != "bottom") {
            c.informationMessagesPlacement = "middle";
        }
        c.informationMessagesFontScale = std::clamp(c.informationMessagesFontScale, 0.4f, 3.0f);
        c.informationMessagesMinWidth = std::clamp(c.informationMessagesMinWidth, 120.0f, 1200.0f);
        c.informationMessagesMarginLeft = std::clamp(c.informationMessagesMarginLeft, 0.0f, 400.0f);
        c.informationMessagesMarginRight = std::clamp(c.informationMessagesMarginRight, 0.0f, 400.0f);
        c.informationMessagesMarginTop = std::clamp(c.informationMessagesMarginTop, 0.0f, 160.0f);
        c.informationMessagesMarginBottom = std::clamp(c.informationMessagesMarginBottom, 0.0f, 160.0f);
        c.informationMessagesIconTextMargin = std::clamp(c.informationMessagesIconTextMargin, 0.0f, 96.0f);
        c.informationMessagesIconScale = std::clamp(c.informationMessagesIconScale, 0.25f, 4.0f);
        if (c.informationMessagesAnchor != "topLeft" && c.informationMessagesAnchor != "topRight" &&
            c.informationMessagesAnchor != "bottomLeft" && c.informationMessagesAnchor != "bottomRight") {
            c.informationMessagesAnchor = "topLeft";
        }
        c.informationMessagesOffsetX = std::clamp(c.informationMessagesOffsetX, 0.0f, 1000.0f);
        c.informationMessagesOffsetY = std::clamp(c.informationMessagesOffsetY, 0.0f, 1000.0f);
        c.informationMessagesDrawOrder = std::clamp(c.informationMessagesDrawOrder, 0, 32);
        c.throwsMarginLeft = std::clamp(c.throwsMarginLeft, 0.0f, 400.0f);
        c.throwsMarginRight = std::clamp(c.throwsMarginRight, 0.0f, 400.0f);
        c.throwsMarginTop = std::clamp(c.throwsMarginTop, 0.0f, 160.0f);
        c.throwsMarginBottom = std::clamp(c.throwsMarginBottom, 0.0f, 160.0f);
        c.throwsHeaderPaddingY = std::clamp(c.throwsHeaderPaddingY, 0.0f, 48.0f);
        c.throwsRowPaddingY = std::clamp(c.throwsRowPaddingY, 0.0f, 48.0f);
        c.eyeThrowRows = std::clamp(c.eyeThrowRows, 1, static_cast<int>(kNinjabrainThrowLimit));
        if (c.throwsAnchor != "topLeft" && c.throwsAnchor != "topRight" && c.throwsAnchor != "bottomLeft" && c.throwsAnchor != "bottomRight") {
            c.throwsAnchor = "topLeft";
        }
        c.throwsOffsetX = std::clamp(c.throwsOffsetX, 0.0f, 1000.0f);
        c.throwsOffsetY = std::clamp(c.throwsOffsetY, 0.0f, 1000.0f);
        c.throwsDrawOrder = std::clamp(c.throwsDrawOrder, 0, 32);
        c.failureMarginLeft = std::clamp(c.failureMarginLeft, 0.0f, 400.0f);
        c.failureMarginRight = std::clamp(c.failureMarginRight, 0.0f, 400.0f);
        c.failureMarginTop = std::clamp(c.failureMarginTop, 0.0f, 160.0f);
        c.failureMarginBottom = std::clamp(c.failureMarginBottom, 0.0f, 160.0f);
        c.failureLineGap = std::clamp(c.failureLineGap, 0.0f, 96.0f);
        c.borderRadius = std::clamp(c.borderRadius, 0.0f, 160.0f);
        if (c.failureAnchor != "topLeft" && c.failureAnchor != "topRight" && c.failureAnchor != "bottomLeft" && c.failureAnchor != "bottomRight") {
            c.failureAnchor = "topLeft";
        }
        c.failureOffsetX = std::clamp(c.failureOffsetX, 0.0f, 1000.0f);
        c.failureOffsetY = std::clamp(c.failureOffsetY, 0.0f, 1000.0f);
        c.failureDrawOrder = std::clamp(c.failureDrawOrder, 0, 32);
        c.blindMarginLeft = std::clamp(c.blindMarginLeft, 0.0f, 400.0f);
        c.blindMarginRight = std::clamp(c.blindMarginRight, 0.0f, 400.0f);
        c.blindMarginTop = std::clamp(c.blindMarginTop, 0.0f, 160.0f);
        c.blindMarginBottom = std::clamp(c.blindMarginBottom, 0.0f, 160.0f);
        c.blindLineGap = std::clamp(c.blindLineGap, 0.0f, 96.0f);
        if (c.blindAnchor != "topLeft" && c.blindAnchor != "topRight" && c.blindAnchor != "bottomLeft" && c.blindAnchor != "bottomRight") {
            c.blindAnchor = "topLeft";
        }
        c.blindOffsetX = std::clamp(c.blindOffsetX, 0.0f, 1000.0f);
        c.blindOffsetY = std::clamp(c.blindOffsetY, 0.0f, 1000.0f);
        c.blindDrawOrder = std::clamp(c.blindDrawOrder, 0, 32);
        c.hideIfStaleDelaySeconds = std::clamp(c.hideIfStaleDelaySeconds, 1, 3600);
        if (auto* arr = nb->get_as<toml::array>("allowedModes")) {
            c.allowedModes.clear();
            for (auto& el : *arr) { if (auto* s = el.as_string()) c.allowedModes.push_back(s->get()); }
        }
        if (auto cols = nb->get_as<toml::array>("columns")) {
            c.columns.clear();
            bool legacyBoatColumnEnabled = false;
            for (auto& elem : *cols) {
                if (auto ct = elem.as_table()) {
                    NinjabrainColumn col;
                    if (auto v = ct->get_as<std::string>("id"))     col.id     = v->get();
                    if (auto v = ct->get_as<std::string>("header")) col.header = v->get();
                    col.show = GetOr(*ct, "show", true);
                    col.staticWidth = (std::max)(0, GetOr(*ct, "staticWidth", 0));
                    if (col.id == "boat") {
                        legacyBoatColumnEnabled = legacyBoatColumnEnabled || col.show;
                        continue;
                    }
                    c.columns.push_back(col);
                }
            }
            if (!nb->contains("showBoatStateInTopBar") && legacyBoatColumnEnabled) {
                c.showBoatStateInTopBar = true;
            }
        }
        if (c.columns.empty()) {
            c.columns = {
                {"coords",    "Chunk",    true},
                {"certainty", "%",        true},
                {"distance",  "Dist.",    true},
                {"nether",    "Nether",   true},
                {"angle",     "Angle",    true},
            };
        }
    }

    if (auto t = GetTable(tbl, "cursors")) { CursorsConfigFromToml(*t, config.cursors); }

    if (auto t = GetTable(tbl, "cursorTrail")) { CursorTrailConfigFromToml(*t, config.cursorTrail); }

    if (auto t = GetTable(tbl, "keyRebinds")) { KeyRebindsConfigFromToml(*t, config.keyRebinds); }

    if (auto t = GetTable(tbl, "appearance")) { AppearanceConfigFromToml(*t, config.appearance); }

    config.modes.clear();
    const std::vector<ModeConfig> defaultModes = GetDefaultModesFromEmbedded();
    if (auto arr = GetArray(tbl, "mode")) {
        for (const auto& elem : *arr) {
            if (auto t = elem.as_table()) {
                ModeConfig mode;
                ModeConfigFromTomlInternal(*t, mode, &defaultModes);
                config.modes.push_back(mode);
            }
        }
    }

    config.mirrors.clear();
    if (auto arr = GetArray(tbl, "mirror")) {
        for (const auto& elem : *arr) {
            if (auto t = elem.as_table()) {
                if (!hasGlobalMirrorMatchColorspace && t->contains("gammaMode")) {
                    config.mirrorGammaMode = StringToMirrorGammaMode(GetStringOr(*t, "gammaMode", ConfigDefaults::CONFIG_MIRROR_MATCH_COLORSPACE));
                    hasGlobalMirrorMatchColorspace = true;
                }
                MirrorConfig mirror;
                MirrorConfigFromToml(*t, mirror);
                config.mirrors.push_back(mirror);
            }
        }
    }

    config.mirrorGroups.clear();
    if (auto arr = GetArray(tbl, "mirrorGroup")) {
        for (const auto& elem : *arr) {
            if (auto t = elem.as_table()) {
                MirrorGroupConfig group;
                MirrorGroupConfigFromToml(*t, group);
                config.mirrorGroups.push_back(group);
            }
        }
    }

    config.images.clear();
    if (auto arr = GetArray(tbl, "image")) {
        for (const auto& elem : *arr) {
            if (auto t = elem.as_table()) {
                ImageConfig image;
                ImageConfigFromToml(*t, image);
                config.images.push_back(image);
            }
        }
    }

    config.windowOverlays.clear();
    if (auto arr = GetArray(tbl, "windowOverlay")) {
        for (const auto& elem : *arr) {
            if (auto t = elem.as_table()) {
                WindowOverlayConfig overlay;
                WindowOverlayConfigFromToml(*t, overlay);
                config.windowOverlays.push_back(overlay);
            }
        }
    }

    config.browserOverlays.clear();
    if (auto arr = GetArray(tbl, "browserOverlay")) {
        for (const auto& elem : *arr) {
            if (auto t = elem.as_table()) {
                BrowserOverlayConfig overlay;
                BrowserOverlayConfigFromToml(*t, overlay);
                config.browserOverlays.push_back(overlay);
            }
        }
    }

    config.hotkeys.clear();
    if (auto arr = GetArray(tbl, "hotkey")) {
        for (const auto& elem : *arr) {
            if (auto t = elem.as_table()) {
                HotkeyConfig hotkey;
                HotkeyConfigFromToml(*t, hotkey);
                config.hotkeys.push_back(hotkey);
            }
        }
    }

    config.sensitivityHotkeys.clear();
    if (auto arr = GetArray(tbl, "sensitivityHotkey")) {
        for (const auto& elem : *arr) {
            if (auto t = elem.as_table()) {
                SensitivityHotkeyConfig sensHotkey;
                SensitivityHotkeyConfigFromToml(*t, sensHotkey);
                config.sensitivityHotkeys.push_back(sensHotkey);
            }
        }
    }

    if (originalConfigVersion < 5) {
        for (auto& wo : config.windowOverlays) {
            wo.crop_top *= 2;
            wo.crop_bottom *= 2;
            wo.crop_left *= 2;
            wo.crop_right *= 2;
        }
    }

    SanitizeConfigKeyRebindsForCannotTypeTriggers(config);
}

namespace {

const std::vector<std::string>& GetConfigTomlOrderedKeys() {
    static const std::vector<std::string> orderedKeys = {
        "configVersion",
        "disableHookChaining",
        "defaultMode",
        "fontPath",
        "lang",
        "fpsLimit",
        "fpsLimitSleepThreshold",
        "mirrorMatchColorspace",
        "allowCursorEscape",
        "confineCursor",
        "mouseSensitivity",
        "windowsMouseSpeed",
        "hideAnimationsInGame",
        "captureFakeCursor",
        "limitCaptureFramerate",
        "obsFramerate",
        "useSystemKeyRepeat",
        "modifiersInterruptKeyRepeat",
        "keyRepeatStartDelay",
        "keyRepeatDelay",
        "basicModeEnabled",
        "restoreWindowedModeOnFullscreenExit",
        "disableFullscreenPrompt",
        "disableConfigurePrompt",
        "guiHotkey",
        "borderlessHotkey",
        "autoBorderless",
        "imageOverlaysHotkey",
        "windowOverlaysHotkey",
        "ninjabrainOverlayHotkey",
        "debug",
        "eyezoom",
        "ninjabrainOverlay",
        "cursors",
        "cursorTrail",
        "keyRebinds",
        "appearance",
        "mode",
        "mirror",
        "mirrorGroup",
        "image",
        "windowOverlay",
        "browserOverlay",
        "hotkey",
        "sensitivityHotkey",
    };
    return orderedKeys;
}

const std::vector<std::string>& GetModeTomlKeys() {
    static const std::vector<std::string> keys = {
        "id",
        "width",
        "height",
        "useRelativeSize",
        "relativeWidth",
        "relativeHeight",
        "background",
        "mirrorIds",
        "mirrorGroupIds",
        "imageIds",
        "windowOverlayIds",
        "browserOverlayIds",
        "stretch",
        "gameTransition",
        "overlayTransition",
        "backgroundTransition",
        "transitionDurationMs",
        "easeInPower",
        "easeOutPower",
        "bounceCount",
        "bounceIntensity",
        "bounceDurationMs",
        "relativeStretching",
        "skipAnimateX",
        "skipAnimateY",
        "border",
        "sensitivityOverrideEnabled",
        "modeSensitivity",
        "separateXYSensitivity",
        "modeSensitivityX",
        "modeSensitivityY",
    };
    return keys;
}

const std::vector<std::string>& GetMirrorTomlKeys() {
    static const std::vector<std::string> keys = {
        "name",
        "captureWidth",
        "captureHeight",
        "input",
        "output",
        "colors",
        "colorSensitivity",
        "border",
        "fps",
        "rawOutput",
        "colorPassthrough",
        "gradientOutput",
        "gradient",
        "onlyOnMyScreen",
        "debug",
    };
    return keys;
}

const std::vector<std::string>& GetMirrorGroupTomlKeys() {
    static const std::vector<std::string> keys = { "name", "output", "mirrorIds" };
    return keys;
}

const std::vector<std::string>& GetImageTomlKeys() {
    static const std::vector<std::string> keys = {
        "name",
        "path",
        "x",
        "y",
        "scale",
        "relativeTo",
        "crop_top",
        "crop_bottom",
        "crop_left",
        "crop_right",
        "enableColorKey",
        "colorKeys",
        "opacity",
        "background",
        "pixelatedScaling",
        "onlyOnMyScreen",
        "border",
    };
    return keys;
}

const std::vector<std::string>& GetWindowOverlayTomlKeys() {
    static const std::vector<std::string> keys = {
        "name",
        "windowTitle",
        "windowClass",
        "executableName",
        "windowMatchPriority",
        "x",
        "y",
        "scale",
        "relativeTo",
        "crop_top",
        "crop_bottom",
        "crop_left",
        "crop_right",
        "enableColorKey",
        "colorKeys",
        "opacity",
        "background",
        "pixelatedScaling",
        "onlyOnMyScreen",
        "fps",
        "searchInterval",
        "captureMethod",
        "forceUpdate",
        "enableInteraction",
        "border",
    };
    return keys;
}

const std::vector<std::string>& GetBrowserOverlayTomlKeys() {
    static const std::vector<std::string> keys = {
        "name",
        "url",
        "customCss",
        "browserWidth",
        "browserHeight",
        "x",
        "y",
        "scale",
        "relativeTo",
        "crop_top",
        "crop_bottom",
        "crop_left",
        "crop_right",
        "enableColorKey",
        "colorKeys",
        "opacity",
        "background",
        "pixelatedScaling",
        "onlyOnMyScreen",
        "fps",
        "transparentBackground",
        "muteAudio",
        "hardwareAcceleration",
        "allowSystemMediaKeys",
        "reloadOnUpdate",
        "reloadInterval",
        "border",
    };
    return keys;
}

const std::vector<std::string>& GetHotkeyTomlKeys() {
    static const std::vector<std::string> keys = {
        "keys",
        "mainMode",
        "secondaryMode",
        "altSecondaryModes",
        "conditions",
        "debounce",
        "allowExitToFullscreenRegardlessOfGameState",
        "blockKeyFromGame",
        "triggerOnHold",
        "triggerOnRelease",
    };
    return keys;
}

const std::vector<std::string>* GetConfigTomlArrayItemKeyOrder(const std::string& arrayKey) {
    if (arrayKey == "mode") return &GetModeTomlKeys();
    if (arrayKey == "mirror") return &GetMirrorTomlKeys();
    if (arrayKey == "mirrorGroup") return &GetMirrorGroupTomlKeys();
    if (arrayKey == "image") return &GetImageTomlKeys();
    if (arrayKey == "windowOverlay") return &GetWindowOverlayTomlKeys();
    if (arrayKey == "browserOverlay") return &GetBrowserOverlayTomlKeys();
    if (arrayKey == "hotkey") return &GetHotkeyTomlKeys();
    return nullptr;
}

struct ConfigTomlDocumentEntry {
    std::string key;
    const toml::node* node = nullptr;
};

const std::vector<std::string>& GetConfigTomlArrayOfTablesKeys() {
    static const std::vector<std::string> keys = {
        "mode",
        "mirror",
        "mirrorGroup",
        "image",
        "windowOverlay",
        "browserOverlay",
        "hotkey",
        "sensitivityHotkey",
    };
    return keys;
}

bool IsConfigTomlArrayOfTablesKey(const std::string& key) {
    const auto& keys = GetConfigTomlArrayOfTablesKeys();
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}

bool IsArrayOfTablesNodeForConfigKey(const std::string& key, const toml::node& node) {
    if (!node.is_array()) {
        return false;
    }

    if (IsConfigTomlArrayOfTablesKey(key)) {
        return true;
    }

    const toml::array* arr = node.as_array();
    return arr != nullptr && !arr->empty() && (*arr)[0].is_table();
}

bool IsNonEmptyArrayOfTables(const toml::node& node) {
    if (!node.is_array()) {
        return false;
    }

    const toml::array* arr = node.as_array();
    return arr != nullptr && !arr->empty() && (*arr)[0].is_table();
}

void AppendConfigTomlDocumentEntry(const std::string& key, const toml::node* node,
                                   std::vector<ConfigTomlDocumentEntry>& simpleEntries,
                                   std::vector<ConfigTomlDocumentEntry>& tableEntries,
                                   std::vector<ConfigTomlDocumentEntry>& arrayOfTablesEntries) {
    if (node == nullptr) {
        return;
    }

    if (node->is_table()) {
        tableEntries.push_back({ key, node });
        return;
    }

    if (IsArrayOfTablesNodeForConfigKey(key, *node)) {
        arrayOfTablesEntries.push_back({ key, node });
        return;
    }

    simpleEntries.push_back({ key, node });
}

void CollectConfigTomlDocumentEntries(const toml::table& tbl, const std::vector<std::string>& orderedKeys,
                                      std::vector<ConfigTomlDocumentEntry>& simpleEntries,
                                      std::vector<ConfigTomlDocumentEntry>& tableEntries,
                                      std::vector<ConfigTomlDocumentEntry>& arrayOfTablesEntries) {
    std::unordered_set<std::string> seenKeys;

    for (const auto& key : orderedKeys) {
        const toml::node* node = tbl.get(key);
        if (node == nullptr) {
            continue;
        }

        seenKeys.insert(key);
        AppendConfigTomlDocumentEntry(key, node, simpleEntries, tableEntries, arrayOfTablesEntries);
    }

    for (const auto& [key, node] : tbl) {
        const std::string keyStr(key.str());
        if (seenKeys.find(keyStr) != seenKeys.end()) {
            continue;
        }

        AppendConfigTomlDocumentEntry(keyStr, &node, simpleEntries, tableEntries, arrayOfTablesEntries);
    }
}

std::string TrimTomlWhitespace(const std::string& value) {
    const size_t start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return std::string();
    }

    const size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

std::string TrimTomlLeadingWhitespace(const std::string& value) {
    const size_t start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return std::string();
    }

    return value.substr(start);
}

std::string GetArrayOfTablesHeaderKey(const std::string& trimmedLine) {
    if (trimmedLine.rfind("[[", 0) != 0) {
        return std::string();
    }

    const size_t closePos = trimmedLine.find("]]", 2);
    if (closePos == std::string::npos) {
        return std::string();
    }

    const std::string header = TrimTomlWhitespace(trimmedLine.substr(2, closePos - 2));
    if (header.empty() || header.find('.') != std::string::npos || !IsConfigTomlArrayOfTablesKey(header)) {
        return std::string();
    }

    return header;
}

bool IsLegacyArrayOfTablesAssignmentLine(const std::string& trimmedLine, const std::string& key) {
    if (trimmedLine.compare(0, key.size(), key) != 0) {
        return false;
    }

    size_t pos = key.size();
    while (pos < trimmedLine.size() && (trimmedLine[pos] == ' ' || trimmedLine[pos] == '\t')) {
        ++pos;
    }
    if (pos >= trimmedLine.size() || trimmedLine[pos] != '=') {
        return false;
    }

    ++pos;
    while (pos < trimmedLine.size() && (trimmedLine[pos] == ' ' || trimmedLine[pos] == '\t')) {
        ++pos;
    }

    return pos < trimmedLine.size() && trimmedLine[pos] == '[';
}

bool RepairLegacyArrayOfTablesConflicts(const std::string& source, std::string& repairedSource) {
    std::istringstream input(source);
    std::vector<std::string> lines;
    std::unordered_set<std::string> arrayHeaders;
    std::string line;

    while (std::getline(input, line)) {
        lines.push_back(line);
        const std::string headerKey = GetArrayOfTablesHeaderKey(TrimTomlLeadingWhitespace(line));
        if (!headerKey.empty()) {
            arrayHeaders.insert(headerKey);
        }
    }

    if (arrayHeaders.empty()) {
        return false;
    }

    std::ostringstream rebuilt;
    bool changed = false;
    for (const std::string& originalLine : lines) {
        const std::string trimmedLine = TrimTomlLeadingWhitespace(originalLine);
        bool skipLine = false;

        for (const auto& key : arrayHeaders) {
            if (IsLegacyArrayOfTablesAssignmentLine(trimmedLine, key)) {
                skipLine = true;
                changed = true;
                break;
            }
        }

        if (!skipLine) {
            rebuilt << originalLine << "\n";
        }
    }

    if (!changed) {
        return false;
    }

    repairedSource = rebuilt.str();
    return true;
}

bool ParseTomlTableFromString(const std::string& source, toml::table& tbl, std::string& errorDescription) {
#if TOML_EXCEPTIONS
    try {
        tbl = toml::parse(source);
        return true;
    } catch (const toml::parse_error& e) {
        errorDescription = e.what();
        return false;
    } catch (const std::exception& e) {
        errorDescription = e.what();
        return false;
    }
#else
    toml::parse_result result = toml::parse(source);
    if (!result) {
        errorDescription = std::string(result.error().description());
        return false;
    }

    tbl = std::move(result).table();
    return true;
#endif
}

bool WriteConfigTomlDocument(std::ostream& out, const Config& config) {
    toml::table tbl;
    Config sanitizedConfig = config;
    SanitizeConfigKeyRebindsForCannotTypeTriggers(sanitizedConfig);
    ConfigToToml(sanitizedConfig, tbl);

    const auto& orderedKeys = GetConfigTomlOrderedKeys();
    static const std::vector<std::string> emptyOrder;

    std::vector<ConfigTomlDocumentEntry> simpleEntries;
    std::vector<ConfigTomlDocumentEntry> tableEntries;
    std::vector<ConfigTomlDocumentEntry> arrayOfTablesEntries;
    CollectConfigTomlDocumentEntries(tbl, orderedKeys, simpleEntries, tableEntries, arrayOfTablesEntries);

    bool wroteAny = false;
    for (const auto& entry : simpleEntries) {
        if (entry.node == nullptr) {
            continue;
        }

        WriteNode(out, entry.key, *entry.node, false);
        wroteAny = true;
    }

    for (const auto& entry : tableEntries) {
        if (entry.node == nullptr) {
            continue;
        }

        if (wroteAny) {
            out << "\n";
        }
        WriteNode(out, entry.key, *entry.node, false);
        wroteAny = true;
    }

    for (const auto& entry : arrayOfTablesEntries) {
        const toml::array* arr = entry.node != nullptr ? entry.node->as_array() : nullptr;
        if (arr == nullptr) {
            continue;
        }

        const std::vector<std::string>* itemKeyOrder = GetConfigTomlArrayItemKeyOrder(entry.key);
        for (const auto& elem : *arr) {
            if (wroteAny) {
                out << "\n";
            }
            out << "[[" << entry.key << "]]\n";

            if (const toml::table* elemTbl = elem.as_table()) {
                WriteTableOrdered(out, *elemTbl, itemKeyOrder ? *itemKeyOrder : emptyOrder, true);
            }
            wroteAny = true;
        }
    }

    return out.good();
}

} // namespace

bool SerializeConfigToTomlString(const Config& config, std::string& outToml) {
    try {
        std::ostringstream out;
        if (!WriteConfigTomlDocument(out, config)) {
            return false;
        }
        outToml = out.str();
        return true;
    } catch (const std::exception& e) {
        Log("ERROR: Failed to serialize config to TOML: " + std::string(e.what()));
        return false;
    }
}

bool SaveConfigToTomlFile(const Config& config, const std::wstring& path) {
    try {
        // Do not pass UTF-8 narrow strings to std::ofstream.
        // Use std::filesystem::path so the wide Win32 APIs are used under the hood.
        std::ofstream file(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
        if (!file.is_open()) { return false; }
        if (!WriteConfigTomlDocument(file, config)) {
            return false;
        }

        file.close();
        return file.good();
    } catch (const std::exception& e) {
        Log("ERROR: Failed to save config to TOML: " + std::string(e.what()));
        return false;
    }
}

bool LoadConfigFromTomlFile(const std::wstring& path, Config& config) {
    try {
        std::ifstream in(std::filesystem::path(path), std::ios::binary);
        if (!in.is_open()) {
            Log("ERROR: Failed to open config for reading: " + WideToUtf8(path));
            return false;
        }

        std::ostringstream buffer;
        buffer << in.rdbuf();

        toml::table tbl;
        std::string parseError;
        const std::string source = buffer.str();
        bool repairedLegacyArrayConflict = false;
        if (!ParseTomlTableFromString(source, tbl, parseError)) {
            std::string repairedSource;
            if (parseError.find("cannot redefine existing array") != std::string::npos &&
                RepairLegacyArrayOfTablesConflicts(source, repairedSource) &&
                ParseTomlTableFromString(repairedSource, tbl, parseError)) {
                repairedLegacyArrayConflict = true;
                Log("WARNING: Repaired legacy TOML array-of-tables conflict while loading: " + WideToUtf8(path));
            } else {
                Log("ERROR: TOML parse error: " + parseError);
                return false;
            }
        }

        const bool cleanedAppearanceCustomColors = SanitizeConfigAppearanceCustomColors(tbl);

        ConfigFromToml(tbl, config);

        if (cleanedAppearanceCustomColors || repairedLegacyArrayConflict) {
            if (!SaveConfigToTomlFile(config, path)) {
                Log("WARNING: Failed to persist sanitized config/profile TOML after load: " + WideToUtf8(path));
            } else if (cleanedAppearanceCustomColors) {
                Log("WARNING: Removed invalid appearance.customColors entries while loading: " + WideToUtf8(path));
            }
        }
        return true;
    } catch (const std::exception& e) {
        Log("ERROR: Failed to load config from TOML: " + std::string(e.what()));
        return false;
    }
}


#include "platform/resource.h"

static std::string s_embeddedConfigCache;
static bool s_embeddedConfigLoaded = false;
static std::vector<NinjabrainPresetDefinition> s_embeddedNinjabrainPresetsCache;
static bool s_embeddedNinjabrainPresetsLoaded = false;

static std::string LoadEmbeddedRcDataString(int resourceId, const char* debugName) {
    HMODULE hModule = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&LoadEmbeddedRcDataString), &hModule)) {
        Log(std::string("ERROR: Failed to get module handle for ") + debugName);
        return "";
    }

    HRSRC hResource = FindResourceW(hModule, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (!hResource) {
        Log(std::string("ERROR: Failed to find ") + debugName + " resource. Error: " + std::to_string(GetLastError()));
        return "";
    }

    HGLOBAL hData = LoadResource(hModule, hResource);
    if (!hData) {
        Log(std::string("ERROR: Failed to load ") + debugName + " resource. Error: " + std::to_string(GetLastError()));
        return "";
    }

    const DWORD size = SizeofResource(hModule, hResource);
    const char* data = static_cast<const char*>(LockResource(hData));
    if (!data || size == 0) {
        Log(std::string("ERROR: Failed to lock ") + debugName + " resource or resource is empty");
        return "";
    }

    return std::string(data, size);
}

std::string GetEmbeddedDefaultConfigString() {
    if (s_embeddedConfigLoaded) { return s_embeddedConfigCache; }

    s_embeddedConfigCache = LoadEmbeddedRcDataString(IDR_DEFAULT_CONFIG, "embedded default.toml");
    if (s_embeddedConfigCache.empty()) {
        return "";
    }
    s_embeddedConfigLoaded = true;

    Log("Loaded embedded default.toml (" + std::to_string(s_embeddedConfigCache.size()) + " bytes)");
    return s_embeddedConfigCache;
}

std::vector<NinjabrainPresetDefinition> GetEmbeddedNinjabrainPresets() {
    if (s_embeddedNinjabrainPresetsLoaded) {
        return s_embeddedNinjabrainPresetsCache;
    }

    struct EmbeddedPresetResource {
        int resourceId;
        const char* presetId;
        const char* translationKey;
        bool preserveCurrentPlacement;
        const char* debugName;
    };

    static const EmbeddedPresetResource kPresetResources[] = {
        { IDR_NINJABRAIN_PRESET_COMPACT, "compact", "ninjabrain.preset_compact", true,
          "embedded Ninjabrain compact preset" },
        { IDR_NINJABRAIN_PRESET_NINJABRAINBOT, "ninjabrainbot", "ninjabrain.preset_ninjabrainbot", false,
          "embedded Ninjabrain Bot preset" },
    };

    s_embeddedNinjabrainPresetsCache.clear();

    for (const EmbeddedPresetResource& resource : kPresetResources) {
        const std::string presetToml = LoadEmbeddedRcDataString(resource.resourceId, resource.debugName);
        if (presetToml.empty()) {
            continue;
        }

        try {
            const toml::table tbl = toml::parse(presetToml);
            const toml::table* overlayTbl = GetTable(tbl, "ninjabrainOverlay");
            if (!overlayTbl) {
                Log(std::string("ERROR: Ninjabrain preset TOML is missing [ninjabrainOverlay]: ") + resource.debugName);
                continue;
            }

            NinjabrainPresetDefinition preset;
            preset.id = resource.presetId;
            preset.translationKey = resource.translationKey;
            preset.preserveCurrentPlacement = resource.preserveCurrentPlacement;

            Config presetConfig;
            ConfigFromToml(tbl, presetConfig);
            preset.overlay = presetConfig.ninjabrainOverlay;

            s_embeddedNinjabrainPresetsCache.push_back(std::move(preset));
        } catch (const std::exception& e) {
            Log(std::string("ERROR: Failed to parse ") + resource.debugName + ": " + e.what());
        }
    }

    s_embeddedNinjabrainPresetsLoaded = true;
    return s_embeddedNinjabrainPresetsCache;
}

bool LoadEmbeddedDefaultConfig(Config& config) {
    std::string configStr = GetEmbeddedDefaultConfigString();
    if (configStr.empty()) { return false; }

    try {
        toml::table tbl = toml::parse(configStr);
        ConfigFromToml(tbl, config);
        return true;
    } catch (const toml::parse_error& e) {
        Log("ERROR: Failed to parse embedded default.toml: " + std::string(e.what()));
        return false;
    } catch (const std::exception& e) {
        Log("ERROR: Failed to load embedded default config: " + std::string(e.what()));
        return false;
    }
}

int GetCachedWindowWidth();
int GetCachedWindowHeight();

std::vector<ModeConfig> GetDefaultModesFromEmbedded() {
    std::string configStr = GetEmbeddedDefaultConfigString();
    std::vector<ModeConfig> modes;

    if (configStr.empty()) {
        Log("WARNING: Could not load embedded config for modes, falling back to empty");
        return modes;
    }

    try {
        toml::table tbl = toml::parse(configStr);

        if (auto arr = GetArray(tbl, "mode")) {
            for (const auto& elem : *arr) {
                if (auto t = elem.as_table()) {
                    ModeConfig mode;
                    ModeConfigFromToml(*t, mode);
                    modes.push_back(mode);
                }
            }
        }

        int screenWidth = GetCachedWindowWidth();
        int screenHeight = GetCachedWindowHeight();

        for (auto& mode : modes) {
            if (mode.id == "Fullscreen") {
                mode.width = screenWidth;
                mode.height = screenHeight;
                if (mode.stretch.enabled) {
                    mode.stretch.width = screenWidth;
                    mode.stretch.height = screenHeight;
                }
            }
        }

    } catch (const std::exception& e) { Log("ERROR: Failed to parse embedded modes: " + std::string(e.what())); }

    return modes;
}

std::vector<MirrorConfig> GetDefaultMirrorsFromEmbedded() {
    std::string configStr = GetEmbeddedDefaultConfigString();
    std::vector<MirrorConfig> mirrors;

    if (configStr.empty()) {
        Log("WARNING: Could not load embedded config for mirrors, falling back to empty");
        return mirrors;
    }

    try {
        toml::table tbl = toml::parse(configStr);

        if (auto arr = GetArray(tbl, "mirror")) {
            for (const auto& elem : *arr) {
                if (auto t = elem.as_table()) {
                    MirrorConfig mirror;
                    MirrorConfigFromToml(*t, mirror);
                    mirrors.push_back(mirror);
                }
            }
        }

    } catch (const std::exception& e) { Log("ERROR: Failed to parse embedded mirrors: " + std::string(e.what())); }

    return mirrors;
}

std::vector<MirrorGroupConfig> GetDefaultMirrorGroupsFromEmbedded() {
    std::string configStr = GetEmbeddedDefaultConfigString();
    std::vector<MirrorGroupConfig> groups;

    if (configStr.empty()) {
        Log("WARNING: Could not load embedded config for mirror groups, falling back to empty");
        return groups;
    }

    try {
        toml::table tbl = toml::parse(configStr);

        if (auto arr = GetArray(tbl, "mirrorGroup")) {
            for (const auto& elem : *arr) {
                if (auto t = elem.as_table()) {
                    MirrorGroupConfig group;
                    MirrorGroupConfigFromToml(*t, group);
                    groups.push_back(group);
                }
            }
        }

    } catch (const std::exception& e) {
        Log("ERROR: Failed to parse embedded mirror groups: " + std::string(e.what()));
    }

    return groups;
}

std::vector<HotkeyConfig> GetDefaultHotkeysFromEmbedded() {
    std::string configStr = GetEmbeddedDefaultConfigString();
    std::vector<HotkeyConfig> hotkeys;

    if (configStr.empty()) {
        Log("WARNING: Could not load embedded config for hotkeys, falling back to empty");
        return hotkeys;
    }

    try {
        toml::table tbl = toml::parse(configStr);

        if (auto arr = GetArray(tbl, "hotkey")) {
            for (const auto& elem : *arr) {
                if (auto t = elem.as_table()) {
                    HotkeyConfig hotkey;
                    HotkeyConfigFromToml(*t, hotkey);
                    hotkeys.push_back(hotkey);
                }
            }
        }

    } catch (const std::exception& e) { Log("ERROR: Failed to parse embedded hotkeys: " + std::string(e.what())); }

    return hotkeys;
}

std::vector<ImageConfig> GetDefaultImagesFromEmbedded() {
    std::string configStr = GetEmbeddedDefaultConfigString();
    std::vector<ImageConfig> images;

    if (configStr.empty()) {
        Log("WARNING: Could not load embedded config for images, falling back to empty");
        return images;
    }

    try {
        toml::table tbl = toml::parse(configStr);

        if (auto arr = GetArray(tbl, "image")) {
            for (const auto& elem : *arr) {
                if (auto t = elem.as_table()) {
                    ImageConfig image;
                    ImageConfigFromToml(*t, image);
                    images.push_back(image);
                }
            }
        }

        for (auto& image : images) {
            if (image.name == "Ninjabrain Bot" && image.path.empty()) {
                WCHAR tempPath[MAX_PATH];
                if (GetTempPathW(MAX_PATH, tempPath) > 0) {
                    std::wstring nbImagePath = std::wstring(tempPath) + L"nb-overlay.png";
                    image.path = WideToUtf8(nbImagePath);
                }
            }
        }

    } catch (const std::exception& e) { Log("ERROR: Failed to parse embedded images: " + std::string(e.what())); }

    return images;
}

CursorsConfig GetDefaultCursorsFromEmbedded() {
    std::string configStr = GetEmbeddedDefaultConfigString();
    CursorsConfig cursors;

    if (configStr.empty()) {
        Log("WARNING: Could not load embedded config for cursors, falling back to defaults");
        return cursors;
    }

    try {
        toml::table tbl = toml::parse(configStr);

        if (auto t = GetTable(tbl, "cursors")) { CursorsConfigFromToml(*t, cursors); }

        HDC hdc = GetDC(NULL);
        int dpi = GetDeviceCaps(hdc, LOGPIXELSY);
        ReleaseDC(NULL, hdc);

        int systemCursorSize = GetSystemMetricsForDpi(SM_CYCURSOR, dpi);
        systemCursorSize = std::clamp(systemCursorSize, ConfigDefaults::CURSOR_MIN_SIZE, ConfigDefaults::CURSOR_MAX_SIZE);

        cursors.title.cursorSize = systemCursorSize;
        cursors.wall.cursorSize = systemCursorSize;
        cursors.ingame.cursorSize = systemCursorSize;

    } catch (const std::exception& e) { Log("ERROR: Failed to parse embedded cursors: " + std::string(e.what())); }

    return cursors;
}

EyeZoomConfig GetDefaultEyeZoomConfigFromEmbedded() {
    std::string configStr = GetEmbeddedDefaultConfigString();
    EyeZoomConfig eyezoom;

    if (configStr.empty()) {
        Log("WARNING: Could not load embedded config for eyezoom, falling back to defaults");
        return eyezoom;
    }

    try {
        toml::table tbl = toml::parse(configStr);

        if (auto t = GetTable(tbl, "eyezoom")) { EyeZoomConfigFromToml(*t, eyezoom); }

        int screenWidth = GetCachedWindowWidth();
        int screenHeight = GetCachedWindowHeight();

        int eyezoomWindowWidth = eyezoom.windowWidth;
        if (eyezoomWindowWidth < 1) eyezoomWindowWidth = ConfigDefaults::EYEZOOM_WINDOW_WIDTH;
        int eyezoomTargetFinalX = (screenWidth - eyezoomWindowWidth) / 2;
        if (eyezoomTargetFinalX < 1) eyezoomTargetFinalX = 1;
        int horizontalMargin = ((screenWidth / 2) - (eyezoomWindowWidth / 2)) / 20;
        int verticalMargin = (screenHeight / 2) / 4;

        int defaultZoomAreaWidth = eyezoomTargetFinalX - (2 * horizontalMargin);
        int defaultZoomAreaHeight = screenHeight - (2 * verticalMargin);
        if (defaultZoomAreaWidth < 1) defaultZoomAreaWidth = 1;
        if (defaultZoomAreaHeight < 1) defaultZoomAreaHeight = 1;

        eyezoom.zoomAreaWidth = defaultZoomAreaWidth;
        eyezoom.zoomAreaHeight = defaultZoomAreaHeight;
        eyezoom.positionX = horizontalMargin;
        eyezoom.positionY = verticalMargin;

    } catch (const std::exception& e) { Log("ERROR: Failed to parse embedded eyezoom: " + std::string(e.what())); }

    return eyezoom;
}


