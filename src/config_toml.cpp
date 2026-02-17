#include "config_toml.h"
#include "config_defaults.h"
#include "gui.h"
#include "logic_thread.h"
#include "utils.h"

#include <cmath>
#include <filesystem>
#include <fstream>

// Get optional value from TOML table with default
template <typename T> T GetOr(const toml::table& tbl, const std::string& key, T defaultValue) {
    if (auto node = tbl.get(key)) {
        if (auto val = node->value<T>()) { return *val; }
    }
    return defaultValue;
}

// Get optional string with default
std::string GetStringOr(const toml::table& tbl, const std::string& key, const std::string& defaultValue) {
    if (auto node = tbl.get(key)) {
        if (auto val = node->value<std::string>()) { return *val; }
    }
    return defaultValue;
}

// Get optional nested table
const toml::table* GetTable(const toml::table& tbl, const std::string& key) {
    if (auto node = tbl.get(key)) { return node->as_table(); }
    return nullptr;
}

// Get optional array
const toml::array* GetArray(const toml::table& tbl, const std::string& key) {
    if (auto node = tbl.get(key)) { return node->as_array(); }
    return nullptr;
}

// Write a table with keys in the specified order
void WriteTableOrdered(std::ostream& out, const toml::table& tbl, const std::vector<std::string>& orderedKeys) {
    // Write keys in the specified order first
    for (const auto& key : orderedKeys) {
        if (tbl.contains(key)) {
            const toml::node* nodePtr = tbl.get(key);
            if (!nodePtr) continue;

            if (nodePtr->is_table()) {
                // Inline table
                const toml::table* subtbl = nodePtr->as_table();
                if (subtbl) out << key << " = " << *subtbl << "\n";
            } else if (nodePtr->is_array()) {
                const toml::array* arr = nodePtr->as_array();
                if (arr) out << key << " = " << *arr << "\n";
            } else {
                // Scalar value
                out << key << " = ";
                nodePtr->visit([&out](auto&& val) { out << val; });
                out << "\n";
            }
        }
    }

    // Write any remaining keys not in orderedKeys
    for (const auto& [key, node] : tbl) {
        std::string keyStr(key.str());
        if (std::find(orderedKeys.begin(), orderedKeys.end(), keyStr) == orderedKeys.end()) {
            if (node.is_table()) {
                const toml::table* subtbl = node.as_table();
                if (subtbl) out << keyStr << " = " << *subtbl << "\n";
            } else if (node.is_array()) {
                const toml::array* arr = node.as_array();
                if (arr) out << keyStr << " = " << *arr << "\n";
            } else {
                out << keyStr << " = ";
                node.visit([&out](auto&& val) { out << val; });
                out << "\n";
            }
        }
    }
}

toml::array ColorToTomlArray(const Color& color) {
    // Convert from internal float [0-1] to int [0-255] RGB(A) array
    // Only include alpha if it's not fully opaque (1.0) for backward compatibility
    toml::array arr;
    arr.push_back(static_cast<int64_t>(std::round(color.r * 255.0f)));
    arr.push_back(static_cast<int64_t>(std::round(color.g * 255.0f)));
    arr.push_back(static_cast<int64_t>(std::round(color.b * 255.0f)));
    if (color.a < 1.0f - 0.001f) { // Only add alpha if not fully opaque
        arr.push_back(static_cast<int64_t>(std::round(color.a * 255.0f)));
    }
    return arr;
}

Color ColorFromTomlArray(const toml::array* arr, Color defaultColor = { 0.0f, 0.0f, 0.0f, 1.0f }) {
    Color color = defaultColor;
    if (arr && arr->size() >= 3) {
        if (auto r = (*arr)[0].value<int64_t>()) color.r = static_cast<float>(*r) / 255.0f;
        if (auto g = (*arr)[1].value<int64_t>()) color.g = static_cast<float>(*g) / 255.0f;
        if (auto b = (*arr)[2].value<int64_t>()) color.b = static_cast<float>(*b) / 255.0f;
        // Read alpha if present (4th element)
        if (arr->size() >= 4) {
            if (auto a = (*arr)[3].value<int64_t>()) color.a = static_cast<float>(*a) / 255.0f;
        } else {
            color.a = 1.0f; // Default to fully opaque if not specified
        }
    }
    return color;
}

// ============================================================================
// GradientAnimationType Serialization
// ============================================================================

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

void BackgroundConfigToToml(const BackgroundConfig& cfg, toml::table& out) {
    out.is_inline(false); // Need non-inline for gradient arrays

    out.insert("selectedMode", cfg.selectedMode);
    out.insert("image", cfg.image);
    out.insert("color", ColorToTomlArray(cfg.color));

    // Gradient settings
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

    // Gradient animation settings
    out.insert("gradientAnimation", GradientAnimationTypeToString(cfg.gradientAnimation));
    out.insert("gradientAnimationSpeed", cfg.gradientAnimationSpeed);
    out.insert("gradientColorFade", cfg.gradientColorFade);
}

void BackgroundConfigFromToml(const toml::table& tbl, BackgroundConfig& cfg) {
    cfg.selectedMode = GetStringOr(tbl, "selectedMode", ConfigDefaults::BACKGROUND_SELECTED_MODE);
    cfg.image = GetStringOr(tbl, "image", "");
    cfg.color = ColorFromTomlArray(GetArray(tbl, "color"), { 0.0f, 0.0f, 0.0f });

    // Load gradient stops
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
    // Ensure minimum 2 stops with defaults if empty
    if (cfg.gradientStops.size() < 2) {
        cfg.gradientStops.clear();
        cfg.gradientStops.push_back({ { 0.0f, 0.0f, 0.0f }, 0.0f });
        cfg.gradientStops.push_back({ { 1.0f, 1.0f, 1.0f }, 1.0f });
    }
    cfg.gradientAngle = GetOr(tbl, "gradientAngle", 0.0f);

    // Load gradient animation settings
    cfg.gradientAnimation = StringToGradientAnimationType(GetStringOr(tbl, "gradientAnimation", "None"));
    cfg.gradientAnimationSpeed = GetOr(tbl, "gradientAnimationSpeed", 1.0f);
    cfg.gradientColorFade = GetOr(tbl, "gradientColorFade", false);
}

// ============================================================================
// MirrorCaptureConfig Serialization
// ============================================================================

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

    // When using relative position, save as float (0-1) for percentage
    // When using absolute position, save as int for pixel values
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
    // Load explicit relative position fields if present
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

    // Auto-detect relative mode if both x and y were percentages
    if (!tbl.contains("useRelativePosition") && xIsPercentage && yIsPercentage) { cfg.useRelativePosition = true; }

    if (cfg.useRelativePosition) {
        int screenW = GetCachedScreenWidth();
        int screenH = GetCachedScreenHeight();

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

    // Serialize array of target colors
    toml::array targetColorsArr;
    for (const auto& color : cfg.targetColors) { targetColorsArr.push_back(ColorToTomlArray(color)); }
    out.insert("targetColors", targetColorsArr);

    out.insert("output", ColorToTomlArray(cfg.output));
    out.insert("border", ColorToTomlArray(cfg.border));
}

void MirrorColorsFromToml(const toml::table& tbl, MirrorColors& cfg) {
    // Load array of target colors
    cfg.targetColors.clear();
    if (auto arr = GetArray(tbl, "targetColors")) {
        for (const auto& elem : *arr) {
            if (auto colorArr = elem.as_array()) { cfg.targetColors.push_back(ColorFromTomlArray(colorArr, { 0.0f, 1.0f, 0.0f })); }
        }
    }

    // Backward compatibility: if no targetColors array, try old single "target" field
    if (cfg.targetColors.empty()) { cfg.targetColors.push_back(ColorFromTomlArray(GetArray(tbl, "target"), { 0.0f, 1.0f, 0.0f })); }

    cfg.output = ColorFromTomlArray(GetArray(tbl, "output"), { 1.0f, 0.0f, 0.0f });
    cfg.border = ColorFromTomlArray(GetArray(tbl, "border"), { 1.0f, 1.0f, 1.0f });
}

// ============================================================================
// MirrorBorderConfig Serialization
// ============================================================================

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
    out.insert("captureWidth", cfg.captureWidth);
    out.insert("captureHeight", cfg.captureHeight);

    // Input array
    toml::array inputArr;
    for (const auto& input : cfg.input) {
        toml::table inputTbl;
        MirrorCaptureConfigToToml(input, inputTbl);
        inputArr.push_back(inputTbl);
    }
    out.insert("input", inputArr);

    // Output
    toml::table outputTbl;
    MirrorRenderConfigToToml(cfg.output, outputTbl);
    out.insert("output", outputTbl);

    // Colors
    toml::table colorsTbl;
    MirrorColorsToToml(cfg.colors, colorsTbl);
    out.insert("colors", colorsTbl);

    out.insert("colorSensitivity", std::round(cfg.colorSensitivity * 1000.0f) / 1000.0f);

    // Border config
    toml::table borderTbl;
    MirrorBorderConfigToToml(cfg.border, borderTbl);
    out.insert("border", borderTbl);

    out.insert("fps", cfg.fps);
    out.insert("opacity", std::round(cfg.opacity * 1000.0f) / 1000.0f);
    out.insert("rawOutput", cfg.rawOutput);
    out.insert("colorPassthrough", cfg.colorPassthrough);
    out.insert("onlyOnMyScreen", cfg.onlyOnMyScreen);
}

void MirrorConfigFromToml(const toml::table& tbl, MirrorConfig& cfg) {
    cfg.name = GetStringOr(tbl, "name", "");
    cfg.captureWidth = GetOr(tbl, "captureWidth", ConfigDefaults::MIRROR_CAPTURE_WIDTH);
    cfg.captureHeight = GetOr(tbl, "captureHeight", ConfigDefaults::MIRROR_CAPTURE_HEIGHT);

    // Input array
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

    // Output
    if (auto t = GetTable(tbl, "output")) { MirrorRenderConfigFromToml(*t, cfg.output); }

    // Colors
    if (auto t = GetTable(tbl, "colors")) { MirrorColorsFromToml(*t, cfg.colors); }

    cfg.colorSensitivity = GetOr(tbl, "colorSensitivity", ConfigDefaults::MIRROR_COLOR_SENSITIVITY);

    // Border config - check for new format first, then fallback to old borderThickness
    if (auto t = GetTable(tbl, "border")) {
        MirrorBorderConfigFromToml(*t, cfg.border);
    } else {
        // Backward compatibility: migrate old borderThickness to new format
        cfg.border.type = MirrorBorderType::Dynamic;
        cfg.border.dynamicThickness = GetOr(tbl, "borderThickness", ConfigDefaults::MIRROR_BORDER_DYNAMIC_THICKNESS);
    }

    cfg.fps = GetOr(tbl, "fps", ConfigDefaults::MIRROR_FPS);
    cfg.opacity = GetOr(tbl, "opacity", 1.0f);
    cfg.rawOutput = GetOr(tbl, "rawOutput", ConfigDefaults::MIRROR_RAW_OUTPUT);
    cfg.colorPassthrough = GetOr(tbl, "colorPassthrough", ConfigDefaults::MIRROR_COLOR_PASSTHROUGH);
    cfg.onlyOnMyScreen = GetOr(tbl, "onlyOnMyScreen", ConfigDefaults::MIRROR_ONLY_ON_MY_SCREEN);
    // Note: mirror.debug section is ignored for backward compatibility
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

    // Serialize mirrors array with per-item sizing
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

    // New format: mirrors array with per-item sizing
    if (auto arr = GetArray(tbl, "mirrors")) {
        for (const auto& elem : *arr) {
            if (auto t = elem.as_table()) {
                MirrorGroupItem item;
                MirrorGroupItemFromToml(*t, item);
                cfg.mirrors.push_back(item);
            }
        }
    }

    // Backward compatibility: old mirrorIds format (string array)
    if (cfg.mirrors.empty()) {
        if (auto arr = GetArray(tbl, "mirrorIds")) {
            for (const auto& elem : *arr) {
                if (auto val = elem.value<std::string>()) {
                    MirrorGroupItem item;
                    item.mirrorId = *val;
                    item.widthPercent = 1.0f;  // Default 100%
                    item.heightPercent = 1.0f; // Default 100%
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

    // Expression fields (only save if non-empty)
    if (!cfg.widthExpr.empty()) { out.insert("widthExpr", cfg.widthExpr); }
    if (!cfg.heightExpr.empty()) { out.insert("heightExpr", cfg.heightExpr); }
    if (!cfg.xExpr.empty()) { out.insert("xExpr", cfg.xExpr); }
    if (!cfg.yExpr.empty()) { out.insert("yExpr", cfg.yExpr); }
}

void StretchConfigFromToml(const toml::table& tbl, StretchConfig& cfg) {
    cfg.enabled = GetOr(tbl, "enabled", ConfigDefaults::STRETCH_ENABLED);
    cfg.width = GetOr(tbl, "width", ConfigDefaults::STRETCH_WIDTH);
    cfg.height = GetOr(tbl, "height", ConfigDefaults::STRETCH_HEIGHT);
    cfg.x = GetOr(tbl, "x", ConfigDefaults::STRETCH_X);
    cfg.y = GetOr(tbl, "y", ConfigDefaults::STRETCH_Y);

    // Expression fields
    cfg.widthExpr = GetStringOr(tbl, "widthExpr", "");
    cfg.heightExpr = GetStringOr(tbl, "heightExpr", "");
    cfg.xExpr = GetStringOr(tbl, "xExpr", "");
    cfg.yExpr = GetStringOr(tbl, "yExpr", "");
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
    cfg.color = ColorFromTomlArray(GetArray(tbl, "color"), { 1.0f, 1.0f, 1.0f }); // Default white
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
    out.insert("relativeTo", cfg.relativeTo);
    out.insert("crop_top", cfg.crop_top);
    out.insert("crop_bottom", cfg.crop_bottom);
    out.insert("crop_left", cfg.crop_left);
    out.insert("crop_right", cfg.crop_right);
    out.insert("enableColorKey", cfg.enableColorKey);

    // Color keys array
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
    cfg.relativeTo = GetStringOr(tbl, "relativeTo", ConfigDefaults::IMAGE_RELATIVE_TO);
    cfg.crop_top = GetOr(tbl, "crop_top", ConfigDefaults::IMAGE_CROP_TOP);
    cfg.crop_bottom = GetOr(tbl, "crop_bottom", ConfigDefaults::IMAGE_CROP_BOTTOM);
    cfg.crop_left = GetOr(tbl, "crop_left", ConfigDefaults::IMAGE_CROP_LEFT);
    cfg.crop_right = GetOr(tbl, "crop_right", ConfigDefaults::IMAGE_CROP_RIGHT);
    cfg.enableColorKey = GetOr(tbl, "enableColorKey", ConfigDefaults::IMAGE_ENABLE_COLOR_KEY);

    // Color keys array
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
    out.insert("enableColorKey", cfg.enableColorKey);

    // Color keys array
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
    out.insert("captureMethod", cfg.captureMethod);
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
    cfg.enableColorKey = GetOr(tbl, "enableColorKey", ConfigDefaults::IMAGE_ENABLE_COLOR_KEY);

    // Color keys array
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
    cfg.captureMethod = GetStringOr(tbl, "captureMethod", ConfigDefaults::WINDOW_OVERLAY_CAPTURE_METHOD);
    cfg.enableInteraction = GetOr(tbl, "enableInteraction", ConfigDefaults::WINDOW_OVERLAY_ENABLE_INTERACTION);

    // Migrate old capture method names
    if (cfg.captureMethod == "Auto" || cfg.captureMethod == "PrintWindow_FullContent" || cfg.captureMethod == "PrintWindow_ClientOnly" ||
        cfg.captureMethod == "PrintWindow_Default") {
        cfg.captureMethod = "Windows 10+";
    }

    if (auto t = GetTable(tbl, "border")) { BorderConfigFromToml(*t, cfg.border); }
}

void ModeConfigToToml(const ModeConfig& cfg, toml::table& out) {
    out.insert("id", cfg.id);

    // Width/Height can be absolute pixels, relative (0-1), or expressions (string)
    if (!cfg.widthExpr.empty()) {
        out.insert("width", cfg.widthExpr);
    } else if (cfg.useRelativeSize && cfg.relativeWidth >= 0.0f && cfg.relativeWidth <= 1.0f) {
        out.insert("width", cfg.relativeWidth);
    } else {
        out.insert("width", cfg.width);
    }

    if (!cfg.heightExpr.empty()) {
        out.insert("height", cfg.heightExpr);
    } else if (cfg.useRelativeSize && cfg.relativeHeight >= 0.0f && cfg.relativeHeight <= 1.0f) {
        out.insert("height", cfg.relativeHeight);
    } else {
        out.insert("height", cfg.height);
    }

    toml::table bgTbl;
    BackgroundConfigToToml(cfg.background, bgTbl);
    out.insert("background", bgTbl);

    // Mirror IDs array
    toml::array mirrorIds;
    for (const auto& id : cfg.mirrorIds) { mirrorIds.push_back(id); }
    out.insert("mirrorIds", mirrorIds);

    // Mirror Group IDs array
    toml::array mirrorGroupIds;
    for (const auto& id : cfg.mirrorGroupIds) { mirrorGroupIds.push_back(id); }
    out.insert("mirrorGroupIds", mirrorGroupIds);

    // Image IDs array
    toml::array imageIds;
    for (const auto& id : cfg.imageIds) { imageIds.push_back(id); }
    out.insert("imageIds", imageIds);

    // Window overlay IDs array
    toml::array windowOverlayIds;
    for (const auto& id : cfg.windowOverlayIds) { windowOverlayIds.push_back(id); }
    out.insert("windowOverlayIds", windowOverlayIds);

    toml::table stretchTbl;
    StretchConfigToToml(cfg.stretch, stretchTbl);
    out.insert("stretch", stretchTbl);

    // Transitions
    toml::table transitionTbl;
    transitionTbl.insert("gameTransition", GameTransitionTypeToString(cfg.gameTransition));
    transitionTbl.insert("overlayTransition", OverlayTransitionTypeToString(cfg.overlayTransition));
    transitionTbl.insert("backgroundTransition", BackgroundTransitionTypeToString(cfg.backgroundTransition));
    transitionTbl.insert("transitionDurationMs", cfg.transitionDurationMs);

    // Easing settings
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

    // Sensitivity
    out.insert("sensitivityOverrideEnabled", cfg.sensitivityOverrideEnabled);
    out.insert("modeSensitivity", cfg.modeSensitivity);
    out.insert("separateXYSensitivity", cfg.separateXYSensitivity);
    out.insert("modeSensitivityX", cfg.modeSensitivityX);
    out.insert("modeSensitivityY", cfg.modeSensitivityY);

    // Transition animation now lives under transition table
}

void ModeConfigFromToml(const toml::table& tbl, ModeConfig& cfg) {
    cfg.id = GetStringOr(tbl, "id", "");

    cfg.useRelativeSize = false;
    cfg.relativeWidth = -1.0f;
    cfg.relativeHeight = -1.0f;
    cfg.widthExpr.clear();
    cfg.heightExpr.clear();

    // Read width/height - can be expression string, float (0-1) relative, or absolute number
    bool widthIsPercentage = false;
    bool heightIsPercentage = false;

    if (auto widthNode = tbl.get("width")) {
        if (auto widthStr = widthNode->value<std::string>()) {
            cfg.widthExpr = *widthStr;
        } else if (widthNode->is_floating_point()) {
            double widthVal = widthNode->as_floating_point()->get();
            if (widthVal >= 0.0 && widthVal <= 1.0) {
                cfg.relativeWidth = static_cast<float>(widthVal);
                widthIsPercentage = true;
            } else {
                cfg.width = static_cast<int>(widthVal);
            }
        } else if (widthNode->is_integer()) {
            cfg.width = static_cast<int>(widthNode->as_integer()->get());
        } else {
            cfg.width = ConfigDefaults::MODE_WIDTH;
        }
    } else {
        cfg.width = ConfigDefaults::MODE_WIDTH;
    }

    if (auto heightNode = tbl.get("height")) {
        if (auto heightStr = heightNode->value<std::string>()) {
            cfg.heightExpr = *heightStr;
        } else if (heightNode->is_floating_point()) {
            double heightVal = heightNode->as_floating_point()->get();
            if (heightVal >= 0.0 && heightVal <= 1.0) {
                cfg.relativeHeight = static_cast<float>(heightVal);
                heightIsPercentage = true;
            } else {
                cfg.height = static_cast<int>(heightVal);
            }
        } else if (heightNode->is_integer()) {
            cfg.height = static_cast<int>(heightNode->as_integer()->get());
        } else {
            cfg.height = ConfigDefaults::MODE_HEIGHT;
        }
    } else {
        cfg.height = ConfigDefaults::MODE_HEIGHT;
    }

    // Legacy expression fields (widthExpr/heightExpr) for backward compat
    if (cfg.widthExpr.empty()) { cfg.widthExpr = GetStringOr(tbl, "widthExpr", ""); }
    if (cfg.heightExpr.empty()) { cfg.heightExpr = GetStringOr(tbl, "heightExpr", ""); }

    // Legacy relative size fields for backward compat
    if (tbl.contains("useRelativeSize") || tbl.contains("relativeWidth") || tbl.contains("relativeHeight")) {
        cfg.useRelativeSize = GetOr(tbl, "useRelativeSize", false);
        cfg.relativeWidth = GetOr(tbl, "relativeWidth", cfg.relativeWidth);
        cfg.relativeHeight = GetOr(tbl, "relativeHeight", cfg.relativeHeight);
    } else if (widthIsPercentage || heightIsPercentage) {
        cfg.useRelativeSize = true;
    }

    if (!cfg.widthExpr.empty()) { cfg.relativeWidth = -1.0f; }
    if (!cfg.heightExpr.empty()) { cfg.relativeHeight = -1.0f; }

    // Note: Actual pixel conversion from percentages is done elsewhere (GUI/logic thread)
    // because we need screen dimensions which may not be available during config load

    if (auto t = GetTable(tbl, "background")) { BackgroundConfigFromToml(*t, cfg.background); }

    // Mirror IDs
    cfg.mirrorIds.clear();
    if (auto arr = GetArray(tbl, "mirrorIds")) {
        for (const auto& elem : *arr) {
            if (auto val = elem.value<std::string>()) { cfg.mirrorIds.push_back(*val); }
        }
    }

    // Mirror Group IDs
    cfg.mirrorGroupIds.clear();
    if (auto arr = GetArray(tbl, "mirrorGroupIds")) {
        for (const auto& elem : *arr) {
            if (auto val = elem.value<std::string>()) { cfg.mirrorGroupIds.push_back(*val); }
        }
    }

    // Image IDs
    cfg.imageIds.clear();
    if (auto arr = GetArray(tbl, "imageIds")) {
        for (const auto& elem : *arr) {
            if (auto val = elem.value<std::string>()) { cfg.imageIds.push_back(*val); }
        }
    }

    // Window overlay IDs
    cfg.windowOverlayIds.clear();
    if (auto arr = GetArray(tbl, "windowOverlayIds")) {
        for (const auto& elem : *arr) {
            if (auto val = elem.value<std::string>()) { cfg.windowOverlayIds.push_back(*val); }
        }
    }

    if (auto t = GetTable(tbl, "stretch")) { StretchConfigFromToml(*t, cfg.stretch); }

    // Transitions (new: transition table, old: top-level keys)
    const toml::table* transitionTbl = GetTable(tbl, "transition");
    const toml::table& transitionSrc = transitionTbl ? *transitionTbl : tbl;

    cfg.gameTransition = StringToGameTransitionType(GetStringOr(transitionSrc, "gameTransition", ConfigDefaults::GAME_TRANSITION_BOUNCE));
    cfg.overlayTransition =
        StringToOverlayTransitionType(GetStringOr(transitionSrc, "overlayTransition", ConfigDefaults::OVERLAY_TRANSITION_CUT));
    cfg.backgroundTransition =
        StringToBackgroundTransitionType(GetStringOr(transitionSrc, "backgroundTransition", ConfigDefaults::BACKGROUND_TRANSITION_CUT));
    cfg.transitionDurationMs = GetOr(transitionSrc, "transitionDurationMs", ConfigDefaults::MODE_TRANSITION_DURATION_MS);

    // Easing
    cfg.easeInPower = GetOr(transitionSrc, "easeInPower", ConfigDefaults::MODE_EASE_IN_POWER);
    cfg.easeOutPower = GetOr(transitionSrc, "easeOutPower", ConfigDefaults::MODE_EASE_OUT_POWER);
    cfg.bounceCount = GetOr(transitionSrc, "bounceCount", ConfigDefaults::MODE_BOUNCE_COUNT);
    cfg.bounceIntensity = GetOr(transitionSrc, "bounceIntensity", ConfigDefaults::MODE_BOUNCE_INTENSITY);
    cfg.bounceDurationMs = GetOr(transitionSrc, "bounceDurationMs", ConfigDefaults::MODE_BOUNCE_DURATION_MS);
    cfg.relativeStretching = GetOr(transitionSrc, "relativeStretching", ConfigDefaults::MODE_RELATIVE_STRETCHING);
    cfg.skipAnimateX = GetOr(transitionSrc, "skipAnimateX", false);
    cfg.skipAnimateY = GetOr(transitionSrc, "skipAnimateY", false);

    if (auto t = GetTable(tbl, "border")) { BorderConfigFromToml(*t, cfg.border); }

    // Sensitivity
    cfg.sensitivityOverrideEnabled = GetOr(tbl, "sensitivityOverrideEnabled", ConfigDefaults::MODE_SENSITIVITY_OVERRIDE_ENABLED);
    cfg.modeSensitivity = GetOr(tbl, "modeSensitivity", ConfigDefaults::MODE_SENSITIVITY);
    cfg.separateXYSensitivity = GetOr(tbl, "separateXYSensitivity", ConfigDefaults::MODE_SEPARATE_XY_SENSITIVITY);
    cfg.modeSensitivityX = GetOr(tbl, "modeSensitivityX", ConfigDefaults::MODE_SENSITIVITY_X);
    cfg.modeSensitivityY = GetOr(tbl, "modeSensitivityY", ConfigDefaults::MODE_SENSITIVITY_Y);

    // Transition animation
    cfg.slideMirrorsIn = GetOr(transitionSrc, "slideMirrorsIn", false);
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

    cfg.blockKeyFromGame = GetOr(tbl, "blockKeyFromGame", false);
    cfg.allowExitToFullscreenRegardlessOfGameState = GetOr(tbl, "allowExitToFullscreenRegardlessOfGameState", false);
    // Note: currentSecondaryMode is now tracked separately via thread-safe
    // Get/SetHotkeySecondaryMode() API - initialized by ResetAllHotkeySecondaryModes() after load
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
    out.insert("fakeCursor", cfg.fakeCursor);
    out.insert("showTextureGrid", cfg.showTextureGrid);
    out.insert("delayRenderingUntilFinished", cfg.delayRenderingUntilFinished);
    out.insert("delayRenderingUntilBlitted", cfg.delayRenderingUntilBlitted);
    out.insert("virtualCameraEnabled", cfg.virtualCameraEnabled);
    out.insert("virtualCameraFps", cfg.virtualCameraFps);

    out.insert("logModeSwitch", cfg.logModeSwitch);
    out.insert("logAnimation", cfg.logAnimation);
    out.insert("logHotkey", cfg.logHotkey);
    out.insert("logObs", cfg.logObs);
    out.insert("logWindowOverlay", cfg.logWindowOverlay);
    out.insert("logFileMonitor", cfg.logFileMonitor);
    out.insert("logImageMonitor", cfg.logImageMonitor);
    out.insert("logPerformance", cfg.logPerformance);
    out.insert("logTextureOps", cfg.logTextureOps);
    out.insert("logGui", cfg.logGui);
    out.insert("logInit", cfg.logInit);
}

void DebugGlobalConfigFromToml(const toml::table& tbl, DebugGlobalConfig& cfg) {
    cfg.showPerformanceOverlay = GetOr(tbl, "showPerformanceOverlay", ConfigDefaults::DEBUG_GLOBAL_SHOW_PERFORMANCE_OVERLAY);
    cfg.showProfiler = GetOr(tbl, "showProfiler", ConfigDefaults::DEBUG_GLOBAL_SHOW_PROFILER);
    cfg.profilerScale = GetOr(tbl, "profilerScale", ConfigDefaults::DEBUG_GLOBAL_PROFILER_SCALE);
    cfg.fakeCursor = GetOr(tbl, "fakeCursor", ConfigDefaults::DEBUG_GLOBAL_FAKE_CURSOR);
    cfg.showTextureGrid = GetOr(tbl, "showTextureGrid", ConfigDefaults::DEBUG_GLOBAL_SHOW_TEXTURE_GRID);
    cfg.delayRenderingUntilFinished =
        GetOr(tbl, "delayRenderingUntilFinished", ConfigDefaults::DEBUG_GLOBAL_DELAY_RENDERING_UNTIL_FINISHED);
    cfg.delayRenderingUntilBlitted = GetOr(tbl, "delayRenderingUntilBlitted", ConfigDefaults::DEBUG_GLOBAL_DELAY_RENDERING_UNTIL_BLITTED);
    cfg.virtualCameraEnabled = GetOr(tbl, "virtualCameraEnabled", false);
    cfg.virtualCameraFps = GetOr(tbl, "virtualCameraFps", 30);

    cfg.logModeSwitch = GetOr(tbl, "logModeSwitch", ConfigDefaults::DEBUG_GLOBAL_LOG_MODE_SWITCH);
    cfg.logAnimation = GetOr(tbl, "logAnimation", ConfigDefaults::DEBUG_GLOBAL_LOG_ANIMATION);
    cfg.logHotkey = GetOr(tbl, "logHotkey", ConfigDefaults::DEBUG_GLOBAL_LOG_HOTKEY);
    cfg.logObs = GetOr(tbl, "logObs", ConfigDefaults::DEBUG_GLOBAL_LOG_OBS);
    cfg.logWindowOverlay = GetOr(tbl, "logWindowOverlay", ConfigDefaults::DEBUG_GLOBAL_LOG_WINDOW_OVERLAY);
    cfg.logFileMonitor = GetOr(tbl, "logFileMonitor", ConfigDefaults::DEBUG_GLOBAL_LOG_FILE_MONITOR);
    cfg.logImageMonitor = GetOr(tbl, "logImageMonitor", ConfigDefaults::DEBUG_GLOBAL_LOG_IMAGE_MONITOR);
    cfg.logPerformance = GetOr(tbl, "logPerformance", ConfigDefaults::DEBUG_GLOBAL_LOG_PERFORMANCE);
    cfg.logTextureOps = GetOr(tbl, "logTextureOps", ConfigDefaults::DEBUG_GLOBAL_LOG_TEXTURE_OPS);
    cfg.logGui = GetOr(tbl, "logGui", ConfigDefaults::DEBUG_GLOBAL_LOG_GUI);
    cfg.logInit = GetOr(tbl, "logInit", ConfigDefaults::DEBUG_GLOBAL_LOG_INIT);
}

void CursorConfigToToml(const CursorConfig& cfg, toml::table& out) {
    out.insert("cursorName", cfg.cursorName);
    out.insert("cursorSize", cfg.cursorSize);
}

void CursorConfigFromToml(const toml::table& tbl, CursorConfig& cfg) {
    cfg.cursorName = GetStringOr(tbl, "cursorName", "");
    cfg.cursorSize = GetOr(tbl, "cursorSize", ConfigDefaults::CURSOR_SIZE);

    // Validate size is in valid range
    static const std::vector<int> validSizes = {
        16, 20, 24, 28, 32, 40, 48, 56, 64, 72, 80, 96, 112, 128, 144, 160, 192, 224, 256, 288, 320
    };
    if (std::find(validSizes.begin(), validSizes.end(), cfg.cursorSize) == validSizes.end()) {
        cfg.cursorSize = ConfigDefaults::CURSOR_SIZE;
    }
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

void EyeZoomConfigToToml(const EyeZoomConfig& cfg, toml::table& out) {
    out.insert("cloneWidth", cfg.cloneWidth);
    out.insert("overlayWidth", cfg.overlayWidth);
    out.insert("cloneHeight", cfg.cloneHeight);
    out.insert("stretchWidth", cfg.stretchWidth);
    out.insert("windowWidth", cfg.windowWidth);
    out.insert("windowHeight", cfg.windowHeight);
    out.insert("horizontalMargin", cfg.horizontalMargin);
    out.insert("verticalMargin", cfg.verticalMargin);
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
}

void EyeZoomConfigFromToml(const toml::table& tbl, EyeZoomConfig& cfg) {
    cfg.cloneWidth = GetOr(tbl, "cloneWidth", ConfigDefaults::EYEZOOM_CLONE_WIDTH);
    // cloneWidth must be even and >= 2 for center-split math used by the overlay.
    if (cfg.cloneWidth < 2) cfg.cloneWidth = 2;
    if (cfg.cloneWidth % 2 != 0) cfg.cloneWidth = (cfg.cloneWidth / 2) * 2;

    // overlayWidth is boxes/labels PER SIDE. Backward-compatible default is cloneWidth/2.
    // Allow older configs (without overlayWidth) to behave like before.
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
    cfg.horizontalMargin = GetOr(tbl, "horizontalMargin", ConfigDefaults::EYEZOOM_HORIZONTAL_MARGIN);
    cfg.verticalMargin = GetOr(tbl, "verticalMargin", ConfigDefaults::EYEZOOM_VERTICAL_MARGIN);
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
}

void KeyRebindToToml(const KeyRebind& cfg, toml::table& out) {
    out.insert("fromKey", static_cast<int64_t>(cfg.fromKey));
    out.insert("toKey", static_cast<int64_t>(cfg.toKey));
    out.insert("enabled", cfg.enabled);
    out.insert("useCustomOutput", cfg.useCustomOutput);
    out.insert("customOutputVK", static_cast<int64_t>(cfg.customOutputVK));
    out.insert("customOutputScanCode", static_cast<int64_t>(cfg.customOutputScanCode));
}

void KeyRebindFromToml(const toml::table& tbl, KeyRebind& cfg) {
    cfg.fromKey = static_cast<DWORD>(GetOr<int64_t>(tbl, "fromKey", 0));
    cfg.toKey = static_cast<DWORD>(GetOr<int64_t>(tbl, "toKey", 0));
    cfg.enabled = GetOr(tbl, "enabled", ConfigDefaults::KEY_REBIND_ENABLED);
    cfg.useCustomOutput = GetOr(tbl, "useCustomOutput", ConfigDefaults::KEY_REBIND_USE_CUSTOM_OUTPUT);
    cfg.customOutputVK = static_cast<DWORD>(GetOr<int64_t>(tbl, "customOutputVK", ConfigDefaults::KEY_REBIND_CUSTOM_OUTPUT_VK));
    cfg.customOutputScanCode =
        static_cast<DWORD>(GetOr<int64_t>(tbl, "customOutputScanCode", ConfigDefaults::KEY_REBIND_CUSTOM_OUTPUT_SCANCODE));
}

void KeyRebindsConfigToToml(const KeyRebindsConfig& cfg, toml::table& out) {
    out.insert("enabled", cfg.enabled);

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

// ============================================================================
// AppearanceConfig Serialization
// ============================================================================

void AppearanceConfigToToml(const AppearanceConfig& cfg, toml::table& out) {
    out.insert("theme", cfg.theme);

    // Save custom colors whenever present.
    // Rationale: users may customize colors, then switch to a preset theme temporarily.
    // Keeping the custom palette in the config allows switching back to "Custom" without losing edits.
    if (!cfg.customColors.empty()) {
        toml::table colorsTbl;
        for (const auto& [name, color] : cfg.customColors) { colorsTbl.insert(name, ColorToTomlArray(color)); }
        out.insert("customColors", colorsTbl);
    }
}

void AppearanceConfigFromToml(const toml::table& tbl, AppearanceConfig& cfg) {
    cfg.theme = GetStringOr(tbl, "theme", "Dark");

    cfg.customColors.clear();
    if (auto colorsTbl = GetTable(tbl, "customColors")) {
        for (const auto& [key, value] : *colorsTbl) {
            if (auto arr = value.as_array()) { cfg.customColors[std::string(key.str())] = ColorFromTomlArray(arr); }
        }
    }
}

void ConfigToToml(const Config& config, toml::table& out) {
    out.insert("configVersion", config.configVersion);
    out.insert("defaultMode", config.defaultMode);
    out.insert("fontPath", config.fontPath);
    out.insert("fpsLimit", config.fpsLimit);
    out.insert("fpsLimitSleepThreshold", config.fpsLimitSleepThreshold);
    out.insert("mirrorMatchColorspace", MirrorGammaModeToString(config.mirrorGammaMode));
    out.insert("allowCursorEscape", config.allowCursorEscape);
    out.insert("mouseSensitivity", config.mouseSensitivity);
    out.insert("windowsMouseSpeed", config.windowsMouseSpeed);
    out.insert("hideAnimationsInGame", config.hideAnimationsInGame);
    out.insert("keyRepeatStartDelay", config.keyRepeatStartDelay);
    out.insert("keyRepeatDelay", config.keyRepeatDelay);
    out.insert("basicModeEnabled", config.basicModeEnabled);
    out.insert("disableFullscreenPrompt", config.disableFullscreenPrompt);
    out.insert("disableConfigurePrompt", config.disableConfigurePrompt);

    // GUI Hotkey
    toml::array guiHotkeyArr;
    for (const auto& key : config.guiHotkey) { guiHotkeyArr.push_back(static_cast<int64_t>(key)); }
    out.insert("guiHotkey", guiHotkeyArr);

    // Debug
    toml::table debugTbl;
    DebugGlobalConfigToToml(config.debug, debugTbl);
    out.insert("debug", debugTbl);

    // EyeZoom
    toml::table eyezoomTbl;
    EyeZoomConfigToToml(config.eyezoom, eyezoomTbl);
    out.insert("eyezoom", eyezoomTbl);

    // Cursors
    toml::table cursorsTbl;
    CursorsConfigToToml(config.cursors, cursorsTbl);
    out.insert("cursors", cursorsTbl);

    // Key Rebinds
    toml::table keyRebindsTbl;
    KeyRebindsConfigToToml(config.keyRebinds, keyRebindsTbl);
    out.insert("keyRebinds", keyRebindsTbl);

    // Appearance
    toml::table appearanceTbl;
    AppearanceConfigToToml(config.appearance, appearanceTbl);
    out.insert("appearance", appearanceTbl);

    // Modes array
    toml::array modesArr;
    for (const auto& mode : config.modes) {
        toml::table modeTbl;
        ModeConfigToToml(mode, modeTbl);
        modesArr.push_back(modeTbl);
    }
    out.insert("mode", modesArr);

    // Mirrors array
    toml::array mirrorsArr;
    for (const auto& mirror : config.mirrors) {
        toml::table mirrorTbl;
        MirrorConfigToToml(mirror, mirrorTbl);
        mirrorsArr.push_back(mirrorTbl);
    }
    out.insert("mirror", mirrorsArr);

    // Mirror groups array
    toml::array mirrorGroupsArr;
    for (const auto& group : config.mirrorGroups) {
        toml::table groupTbl;
        MirrorGroupConfigToToml(group, groupTbl);
        mirrorGroupsArr.push_back(groupTbl);
    }
    out.insert("mirrorGroup", mirrorGroupsArr);

    // Images array
    toml::array imagesArr;
    for (const auto& image : config.images) {
        toml::table imageTbl;
        ImageConfigToToml(image, imageTbl);
        imagesArr.push_back(imageTbl);
    }
    out.insert("image", imagesArr);

    // Window overlays array
    toml::array windowOverlaysArr;
    for (const auto& overlay : config.windowOverlays) {
        toml::table overlayTbl;
        WindowOverlayConfigToToml(overlay, overlayTbl);
        windowOverlaysArr.push_back(overlayTbl);
    }
    out.insert("windowOverlay", windowOverlaysArr);

    // Hotkeys array
    toml::array hotkeysArr;
    for (const auto& hotkey : config.hotkeys) {
        toml::table hotkeyTbl;
        HotkeyConfigToToml(hotkey, hotkeyTbl);
        hotkeysArr.push_back(hotkeyTbl);
    }
    out.insert("hotkey", hotkeysArr);

    // Sensitivity Hotkeys array
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
    config.defaultMode = GetStringOr(tbl, "defaultMode", ConfigDefaults::CONFIG_DEFAULT_MODE);
    config.fontPath = GetStringOr(tbl, "fontPath", ConfigDefaults::CONFIG_FONT_PATH);
    config.fpsLimit = GetOr(tbl, "fpsLimit", ConfigDefaults::CONFIG_FPS_LIMIT);
    config.fpsLimitSleepThreshold = GetOr(tbl, "fpsLimitSleepThreshold", ConfigDefaults::CONFIG_FPS_LIMIT_SLEEP_THRESHOLD);
    bool hasGlobalMirrorMatchColorspace = tbl.contains("mirrorMatchColorspace");
    config.mirrorGammaMode = StringToMirrorGammaMode(
        GetStringOr(tbl, "mirrorMatchColorspace", ConfigDefaults::CONFIG_MIRROR_MATCH_COLORSPACE));
    config.allowCursorEscape = GetOr(tbl, "allowCursorEscape", ConfigDefaults::CONFIG_ALLOW_CURSOR_ESCAPE);
    config.mouseSensitivity = GetOr(tbl, "mouseSensitivity", ConfigDefaults::CONFIG_MOUSE_SENSITIVITY);
    config.windowsMouseSpeed = GetOr(tbl, "windowsMouseSpeed", ConfigDefaults::CONFIG_WINDOWS_MOUSE_SPEED);
    config.hideAnimationsInGame = GetOr(tbl, "hideAnimationsInGame", ConfigDefaults::CONFIG_HIDE_ANIMATIONS_IN_GAME);
    config.keyRepeatStartDelay = GetOr(tbl, "keyRepeatStartDelay", ConfigDefaults::CONFIG_KEY_REPEAT_START_DELAY);
    config.keyRepeatDelay = GetOr(tbl, "keyRepeatDelay", ConfigDefaults::CONFIG_KEY_REPEAT_DELAY);
    config.basicModeEnabled = GetOr(tbl, "basicModeEnabled", ConfigDefaults::CONFIG_BASIC_MODE_ENABLED);
    config.disableFullscreenPrompt = GetOr(tbl, "disableFullscreenPrompt", ConfigDefaults::CONFIG_DISABLE_FULLSCREEN_PROMPT);
    config.disableConfigurePrompt = GetOr(tbl, "disableConfigurePrompt", ConfigDefaults::CONFIG_DISABLE_CONFIGURE_PROMPT);

    // GUI Hotkey
    config.guiHotkey.clear();
    if (auto arr = GetArray(tbl, "guiHotkey")) {
        for (const auto& elem : *arr) {
            if (auto val = elem.value<int64_t>()) { config.guiHotkey.push_back(static_cast<DWORD>(*val)); }
        }
    }
    if (config.guiHotkey.empty()) { config.guiHotkey = ConfigDefaults::GetDefaultGuiHotkey(); }

    // Debug
    if (auto t = GetTable(tbl, "debug")) { DebugGlobalConfigFromToml(*t, config.debug); }

    // EyeZoom
    if (auto t = GetTable(tbl, "eyezoom")) { EyeZoomConfigFromToml(*t, config.eyezoom); }

    // Cursors
    if (auto t = GetTable(tbl, "cursors")) { CursorsConfigFromToml(*t, config.cursors); }

    // Key Rebinds
    if (auto t = GetTable(tbl, "keyRebinds")) { KeyRebindsConfigFromToml(*t, config.keyRebinds); }

    // Appearance
    if (auto t = GetTable(tbl, "appearance")) { AppearanceConfigFromToml(*t, config.appearance); }

    // Modes
    config.modes.clear();
    if (auto arr = GetArray(tbl, "mode")) {
        for (const auto& elem : *arr) {
            if (auto t = elem.as_table()) {
                ModeConfig mode;
                ModeConfigFromToml(*t, mode);
                config.modes.push_back(mode);
            }
        }
    }

    // Mirrors
    config.mirrors.clear();
    if (auto arr = GetArray(tbl, "mirror")) {
        for (const auto& elem : *arr) {
            if (auto t = elem.as_table()) {
                // Backward compatibility: old per-mirror gammaMode setting.
                // If the new global key isn't present, adopt the first mirror's gammaMode as the global setting.
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

    // Mirror Groups
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

    // Images
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

    // Window overlays
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

    // Hotkeys
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

    // Sensitivity Hotkeys
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
}

bool SaveConfigToTomlFile(const Config& config, const std::wstring& path) {
    try {
        toml::table tbl;
        ConfigToToml(config, tbl);

        // Convert wide path to narrow for ofstream
        std::string narrowPath = WideToUtf8(path);
        std::ofstream file(narrowPath);
        if (!file.is_open()) { return false; }

        // Define the order of top-level keys
        std::vector<std::string> orderedKeys = { "configVersion",
                                                 "defaultMode",
                                                 "fontPath",
                                                 "fpsLimit",
                                                 "fpsLimitSleepThreshold",
                                                 "allowCursorEscape",
                                                 "mouseSensitivity",
                                                 "windowsMouseSpeed",
                                                 "hideAnimationsInGame",
                                                 "keyRepeatStartDelay",
                                                 "keyRepeatDelay",
                                                 "basicModeEnabled",
                                                 "disableFullscreenPrompt",
                                                 "disableConfigurePrompt",
                                                 "guiHotkey",
                                                 "debug",
                                                 "eyezoom",
                                                 "cursors",
                                                 "keyRebinds",
                                                 "appearance",
                                                 "mode",
                                                 "mirror",
                                                 "mirrorGroup",
                                                 "image",
                                                 "windowOverlay",
                                                 "hotkey",
                                                 "sensitivityHotkey" };

        // Key orders for each item type (matches the order in *ToToml functions)
        std::vector<std::string> modeKeys = { "id",
                                              "width",
                                              "height",
                                              "background",
                                              "mirrorIds",
                                              "mirrorGroupIds",
                                              "imageIds",
                                              "windowOverlayIds",
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
                                              "modeSensitivityY" };
        std::vector<std::string> mirrorKeys = { "name",   "captureWidth", "captureHeight",    "input",
                                                "output", "colors",       "colorSensitivity", "border",
                                                "fps",    "rawOutput",    "colorPassthrough", "onlyOnMyScreen",
                                                "debug" };
        std::vector<std::string> mirrorGroupKeys = { "name", "output", "mirrorIds" };
        std::vector<std::string> imageKeys = { "name",           "path",      "x",           "y",          "scale",
                                               "relativeTo",     "crop_top",  "crop_bottom", "crop_left",  "crop_right",
                                               "enableColorKey", "colorKeys", "opacity",     "background", "pixelatedScaling",
                                               "onlyOnMyScreen", "border" };
        std::vector<std::string> windowOverlayKeys = { "name",
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
                                                       "captureMethod",
                                                       "enableInteraction",
                                                       "border" };
        std::vector<std::string> hotkeyKeys = { "keys", "mainMode", "secondaryMode", "altSecondaryModes", "conditions", "debounce" };

        // Helper to get key order for a given array type
        auto getKeyOrder = [&](const std::string& arrayKey) -> const std::vector<std::string>* {
            if (arrayKey == "mode") return &modeKeys;
            if (arrayKey == "mirror") return &mirrorKeys;
            if (arrayKey == "mirrorGroup") return &mirrorGroupKeys;
            if (arrayKey == "image") return &imageKeys;
            if (arrayKey == "windowOverlay") return &windowOverlayKeys;
            if (arrayKey == "hotkey") return &hotkeyKeys;
            return nullptr;
        };

        // Write keys in the specified order
        for (const auto& key : orderedKeys) {
            if (tbl.contains(key)) {
                const toml::node* nodePtr = tbl.get(key);
                if (!nodePtr) continue;

                // Arrays of tables need special formatting with [[array_name]]
                if (nodePtr->is_array()) {
                    const toml::array* arr = nodePtr->as_array();
                    // Check if it's an array of tables (like [[mode]])
                    if (arr && !arr->empty() && (*arr)[0].is_table()) {
                        const std::vector<std::string>* itemKeyOrder = getKeyOrder(key);
                        for (const auto& elem : *arr) {
                            file << "\n[[" << key << "]]\n";
                            const toml::table* elemTbl = elem.as_table();
                            if (elemTbl) {
                                if (itemKeyOrder) {
                                    WriteTableOrdered(file, *elemTbl, *itemKeyOrder);
                                } else {
                                    file << *elemTbl;
                                }
                            }
                        }
                    } else if (arr) {
                        // Simple array - use default formatting
                        file << key << " = " << *arr << "\n";
                    }
                } else if (nodePtr->is_table()) {
                    // Named table like [debug]
                    file << "\n[" << key << "]\n";
                    const toml::table* subtbl = nodePtr->as_table();
                    if (subtbl) file << *subtbl;
                } else {
                    // Scalar values
                    file << key << " = ";
                    nodePtr->visit([&file](auto&& val) { file << val; });
                    file << "\n";
                }
            }
        }

        // Write any remaining keys that weren't in orderedKeys (for forward compatibility)
        for (const auto& [key, node] : tbl) {
            std::string keyStr(key.str());
            if (std::find(orderedKeys.begin(), orderedKeys.end(), keyStr) == orderedKeys.end()) {
                file << keyStr << " = ";
                node.visit([&file](auto&& val) { file << val; });
                file << "\n";
            }
        }

        file.close();
        return true;
    } catch (const std::exception& e) {
        Log("ERROR: Failed to save config to TOML: " + std::string(e.what()));
        return false;
    }
}

bool LoadConfigFromTomlFile(const std::wstring& path, Config& config) {
    try {
        std::string narrowPath = WideToUtf8(path);
        toml::table tbl = toml::parse_file(narrowPath);
        ConfigFromToml(tbl, config);
        return true;
    } catch (const toml::parse_error& e) {
        Log("ERROR: TOML parse error: " + std::string(e.what()));
        return false;
    } catch (const std::exception& e) {
        Log("ERROR: Failed to load config from TOML: " + std::string(e.what()));
        return false;
    }
}

// ============================================================================
// Embedded Default Config Implementation
// ============================================================================

#include "resource.h"

// Cache for the embedded config string (loaded once)
static std::string s_embeddedConfigCache;
static bool s_embeddedConfigLoaded = false;

std::string GetEmbeddedDefaultConfigString() {
    // Return cached value if available
    if (s_embeddedConfigLoaded) { return s_embeddedConfigCache; }

    // Get module handle for this DLL
    HMODULE hModule = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&GetEmbeddedDefaultConfigString), &hModule)) {
        Log("ERROR: Failed to get module handle for embedded config");
        return "";
    }

    // Find the resource
    HRSRC hResource = FindResourceW(hModule, MAKEINTRESOURCEW(IDR_DEFAULT_CONFIG), RT_RCDATA);
    if (!hResource) {
        Log("ERROR: Failed to find embedded default.toml resource. Error: " + std::to_string(GetLastError()));
        return "";
    }

    // Load the resource
    HGLOBAL hData = LoadResource(hModule, hResource);
    if (!hData) {
        Log("ERROR: Failed to load embedded default.toml resource. Error: " + std::to_string(GetLastError()));
        return "";
    }

    // Get the resource data
    DWORD size = SizeofResource(hModule, hResource);
    const char* data = static_cast<const char*>(LockResource(hData));

    if (!data || size == 0) {
        Log("ERROR: Failed to lock embedded default.toml resource or resource is empty");
        return "";
    }

    // Copy to string and cache
    s_embeddedConfigCache = std::string(data, size);
    s_embeddedConfigLoaded = true;

    Log("Loaded embedded default.toml (" + std::to_string(size) + " bytes)");
    return s_embeddedConfigCache;
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

// Forward declarations for screen size functions (defined in gui.cpp)
int GetCachedScreenWidth();
int GetCachedScreenHeight();

std::vector<ModeConfig> GetDefaultModesFromEmbedded() {
    std::string configStr = GetEmbeddedDefaultConfigString();
    std::vector<ModeConfig> modes;

    if (configStr.empty()) {
        Log("WARNING: Could not load embedded config for modes, falling back to empty");
        return modes;
    }

    try {
        toml::table tbl = toml::parse(configStr);

        // Parse modes array
        if (auto arr = GetArray(tbl, "mode")) {
            for (const auto& elem : *arr) {
                if (auto t = elem.as_table()) {
                    ModeConfig mode;
                    ModeConfigFromToml(*t, mode);
                    modes.push_back(mode);
                }
            }
        }

        // Apply dynamic screen-relative values
        int screenWidth = GetCachedScreenWidth();
        int screenHeight = GetCachedScreenHeight();

        for (auto& mode : modes) {
            // Update Fullscreen mode to match current screen size
            if (mode.id == "Fullscreen") {
                mode.width = screenWidth;
                mode.height = screenHeight;
                if (mode.stretch.enabled) {
                    mode.stretch.width = screenWidth;
                    mode.stretch.height = screenHeight;
                }
            }
            // Update height-relative modes (Thin uses full screen height)
            else if (mode.id == "Thin") {
                mode.height = screenHeight;
            }
            // Wide mode uses full screen width
            else if (mode.id == "Wide") {
                mode.width = screenWidth;
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

        // Parse mirrors array
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

std::vector<HotkeyConfig> GetDefaultHotkeysFromEmbedded() {
    std::string configStr = GetEmbeddedDefaultConfigString();
    std::vector<HotkeyConfig> hotkeys;

    if (configStr.empty()) {
        Log("WARNING: Could not load embedded config for hotkeys, falling back to empty");
        return hotkeys;
    }

    try {
        toml::table tbl = toml::parse(configStr);

        // Parse hotkeys array
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

        // Parse images array
        if (auto arr = GetArray(tbl, "image")) {
            for (const auto& elem : *arr) {
                if (auto t = elem.as_table()) {
                    ImageConfig image;
                    ImageConfigFromToml(*t, image);
                    images.push_back(image);
                }
            }
        }

        // Apply dynamic paths for Ninjabrain Bot image
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

        // Parse cursors section
        if (auto t = GetTable(tbl, "cursors")) { CursorsConfigFromToml(*t, cursors); }

        // Apply dynamic cursor size based on system DPI
        HDC hdc = GetDC(NULL);
        int dpi = GetDeviceCaps(hdc, LOGPIXELSY);
        ReleaseDC(NULL, hdc);

        int systemCursorSize = GetSystemMetricsForDpi(SM_CYCURSOR, dpi);
        if (systemCursorSize < 16) systemCursorSize = 16;
        if (systemCursorSize > 320) systemCursorSize = 320;

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

        // Parse eyezoom section
        if (auto t = GetTable(tbl, "eyezoom")) { EyeZoomConfigFromToml(*t, eyezoom); }

        // Apply dynamic margins based on screen size
        int screenWidth = GetCachedScreenWidth();
        int screenHeight = GetCachedScreenHeight();

        int horizontalMargin = ((screenWidth / 2) - (384 / 2)) / 10;
        int verticalMargin = (screenHeight / 2) / 4;

        eyezoom.horizontalMargin = horizontalMargin;
        eyezoom.verticalMargin = verticalMargin;

    } catch (const std::exception& e) { Log("ERROR: Failed to parse embedded eyezoom: " + std::string(e.what())); }

    return eyezoom;
}
