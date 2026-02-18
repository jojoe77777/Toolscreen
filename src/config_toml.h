#pragma once

// ============================================================================
// CONFIG_TOML.H - TOML Serialization/Deserialization Declarations
// ============================================================================
// Functions to convert between Config structs and TOML format.
// Uses tomlplusplus library for TOML parsing and generation.
// ============================================================================

#include <toml++/toml.h>
#include <string>

// Need full Color definition for ColorFromTomlArray return type and default parameter
#include "gui.h"

// Forward declarations of config structs (Color is fully declared via gui.h above)
struct BackgroundConfig;
struct MirrorCaptureConfig;
struct MirrorRenderConfig;
struct MirrorColors;
struct MirrorConfig;
struct MirrorGroupConfig;
struct ImageBackgroundConfig;
struct StretchConfig;
struct BorderConfig;
struct ColorKeyConfig;
struct ImageConfig;
struct WindowOverlayConfig;
struct ModeConfig;
struct HotkeyConditions;
struct AltSecondaryMode;
struct HotkeyConfig;
struct SensitivityHotkeyConfig;
struct DebugGlobalConfig;
struct CursorConfig;
struct CursorsConfig;
struct EyeZoomConfig;
struct KeyRebind;
struct KeyRebindsConfig;
struct AppearanceConfig;
struct Config;

// ============================================================================
// TOML Serialization Functions (Config -> TOML)
// ============================================================================

toml::array ColorToTomlArray(const Color& color);
Color ColorFromTomlArray(const toml::array* arr, Color defaultColor);
void BackgroundConfigToToml(const BackgroundConfig& cfg, toml::table& out);
void MirrorCaptureConfigToToml(const MirrorCaptureConfig& cfg, toml::table& out);
void MirrorRenderConfigToToml(const MirrorRenderConfig& cfg, toml::table& out);
void MirrorColorsToToml(const MirrorColors& cfg, toml::table& out);
void MirrorConfigToToml(const MirrorConfig& cfg, toml::table& out);
void MirrorGroupConfigToToml(const MirrorGroupConfig& cfg, toml::table& out);
void ImageBackgroundConfigToToml(const ImageBackgroundConfig& cfg, toml::table& out);
void StretchConfigToToml(const StretchConfig& cfg, toml::table& out);
void BorderConfigToToml(const BorderConfig& cfg, toml::table& out);
void ColorKeyConfigToToml(const ColorKeyConfig& cfg, toml::table& out);
void ImageConfigToToml(const ImageConfig& cfg, toml::table& out);
void WindowOverlayConfigToToml(const WindowOverlayConfig& cfg, toml::table& out);
void ModeConfigToToml(const ModeConfig& cfg, toml::table& out);
void HotkeyConditionsToToml(const HotkeyConditions& cfg, toml::table& out);
void AltSecondaryModeToToml(const AltSecondaryMode& cfg, toml::table& out);
void HotkeyConfigToToml(const HotkeyConfig& cfg, toml::table& out);
void SensitivityHotkeyConfigToToml(const SensitivityHotkeyConfig& cfg, toml::table& out);
void DebugGlobalConfigToToml(const DebugGlobalConfig& cfg, toml::table& out);
void CursorConfigToToml(const CursorConfig& cfg, toml::table& out);
void CursorsConfigToToml(const CursorsConfig& cfg, toml::table& out);
void EyeZoomConfigToToml(const EyeZoomConfig& cfg, toml::table& out);
void KeyRebindToToml(const KeyRebind& cfg, toml::table& out);
void KeyRebindsConfigToToml(const KeyRebindsConfig& cfg, toml::table& out);
void AppearanceConfigToToml(const AppearanceConfig& cfg, toml::table& out);
void ConfigToToml(const Config& config, toml::table& out);

// ============================================================================
// TOML Deserialization Functions (TOML -> Config)
// ============================================================================

// Note: Color deserialization uses ColorFromTomlArray() directly
void BackgroundConfigFromToml(const toml::table& tbl, BackgroundConfig& cfg);
void MirrorCaptureConfigFromToml(const toml::table& tbl, MirrorCaptureConfig& cfg);
void MirrorRenderConfigFromToml(const toml::table& tbl, MirrorRenderConfig& cfg);
void MirrorColorsFromToml(const toml::table& tbl, MirrorColors& cfg);
void MirrorConfigFromToml(const toml::table& tbl, MirrorConfig& cfg);
void MirrorGroupConfigFromToml(const toml::table& tbl, MirrorGroupConfig& cfg);
void ImageBackgroundConfigFromToml(const toml::table& tbl, ImageBackgroundConfig& cfg);
void StretchConfigFromToml(const toml::table& tbl, StretchConfig& cfg);
void BorderConfigFromToml(const toml::table& tbl, BorderConfig& cfg);
void ColorKeyConfigFromToml(const toml::table& tbl, ColorKeyConfig& cfg);
void ImageConfigFromToml(const toml::table& tbl, ImageConfig& cfg);
void WindowOverlayConfigFromToml(const toml::table& tbl, WindowOverlayConfig& cfg);
void ModeConfigFromToml(const toml::table& tbl, ModeConfig& cfg);
void HotkeyConditionsFromToml(const toml::table& tbl, HotkeyConditions& cfg);
void AltSecondaryModeFromToml(const toml::table& tbl, AltSecondaryMode& cfg);
void HotkeyConfigFromToml(const toml::table& tbl, HotkeyConfig& cfg);
void SensitivityHotkeyConfigFromToml(const toml::table& tbl, SensitivityHotkeyConfig& cfg);
void DebugGlobalConfigFromToml(const toml::table& tbl, DebugGlobalConfig& cfg);
void CursorConfigFromToml(const toml::table& tbl, CursorConfig& cfg);
void CursorsConfigFromToml(const toml::table& tbl, CursorsConfig& cfg);
void EyeZoomConfigFromToml(const toml::table& tbl, EyeZoomConfig& cfg);
void KeyRebindFromToml(const toml::table& tbl, KeyRebind& cfg);
void KeyRebindsConfigFromToml(const toml::table& tbl, KeyRebindsConfig& cfg);
void AppearanceConfigFromToml(const toml::table& tbl, AppearanceConfig& cfg);
void ConfigFromToml(const toml::table& tbl, Config& config);

// ============================================================================
// File I/O Helpers
// ============================================================================

// Save config to TOML file
bool SaveConfigToTomlFile(const Config& config, const std::wstring& path);

// Load config from TOML file
bool LoadConfigFromTomlFile(const std::wstring& path, Config& config);

// ============================================================================
// Embedded Default Config Functions
// ============================================================================

// Get the raw embedded default.toml string from DLL resources
std::string GetEmbeddedDefaultConfigString();

// Load the full default config from embedded resource
bool LoadEmbeddedDefaultConfig(Config& config);

// Get default modes from embedded config (with screen-relative adjustments)
std::vector<ModeConfig> GetDefaultModesFromEmbedded();

// Get default mirrors from embedded config
std::vector<MirrorConfig> GetDefaultMirrorsFromEmbedded();

// Get default mirror groups from embedded config
std::vector<MirrorGroupConfig> GetDefaultMirrorGroupsFromEmbedded();

// Get default hotkeys from embedded config
std::vector<HotkeyConfig> GetDefaultHotkeysFromEmbedded();

// Get default images from embedded config
std::vector<ImageConfig> GetDefaultImagesFromEmbedded();

// Get default cursors from embedded config
CursorsConfig GetDefaultCursorsFromEmbedded();

// Get default EyeZoom config from embedded config
EyeZoomConfig GetDefaultEyeZoomConfigFromEmbedded();
