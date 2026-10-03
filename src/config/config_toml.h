#pragma once


#include "third_party/toml.hpp"
#include <functional>
#include <ostream>
#include <string>
#include <vector>

#include "gui/gui.h"

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
struct BrowserOverlayConfig;
struct ModeConfig;
struct HotkeyConditions;
struct AltSecondaryMode;
struct HotkeyConfig;
struct SensitivityHotkeyConfig;
struct DebugGlobalConfig;
struct CursorConfig;
struct CursorsConfig;
struct CursorTrailConfig;
struct EyeZoomConfig;
struct KeyRebind;
struct KeyRebindsConfig;
struct AppearanceConfig;
struct Config;

struct NinjabrainPresetDefinition {
	std::string id;
	std::string translationKey;
	bool preserveCurrentPlacement = true;
	NinjabrainOverlayConfig overlay;
};


toml::array ColorToTomlArray(const Color& color);
Color ColorFromTomlArray(const toml::array* arr, Color defaultColor);
bool IsValidAppearanceCustomColorKey(const std::string& key);
bool IsValidAppearanceCustomColorArray(const toml::array* arr);
bool SanitizeAppearanceCustomColorsTable(toml::table& tbl, const char* customColorsKey);
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
void BrowserOverlayConfigToToml(const BrowserOverlayConfig& cfg, toml::table& out);
void ModeConfigToToml(const ModeConfig& cfg, toml::table& out);
void HotkeyConditionsToToml(const HotkeyConditions& cfg, toml::table& out);
void AltSecondaryModeToToml(const AltSecondaryMode& cfg, toml::table& out);
void HotkeyConfigToToml(const HotkeyConfig& cfg, toml::table& out);
void SensitivityHotkeyConfigToToml(const SensitivityHotkeyConfig& cfg, toml::table& out);
void DebugGlobalConfigToToml(const DebugGlobalConfig& cfg, toml::table& out);
void CursorConfigToToml(const CursorConfig& cfg, toml::table& out);
void CursorsConfigToToml(const CursorsConfig& cfg, toml::table& out);
void CursorTrailConfigToToml(const CursorTrailConfig& cfg, toml::table& out);
void EyeZoomConfigToToml(const EyeZoomConfig& cfg, toml::table& out);
void KeyRebindToToml(const KeyRebind& cfg, toml::table& out);
void KeyRebindsConfigToToml(const KeyRebindsConfig& cfg, toml::table& out);
void AppearanceConfigToToml(const AppearanceConfig& cfg, toml::table& out);
void ConfigToToml(const Config& config, toml::table& out);
bool SerializeConfigToTomlString(const Config& config, std::string& outToml);


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
void BrowserOverlayConfigFromToml(const toml::table& tbl, BrowserOverlayConfig& cfg);
void ModeConfigFromToml(const toml::table& tbl, ModeConfig& cfg);
void HotkeyConditionsFromToml(const toml::table& tbl, HotkeyConditions& cfg);
void AltSecondaryModeFromToml(const toml::table& tbl, AltSecondaryMode& cfg);
void HotkeyConfigFromToml(const toml::table& tbl, HotkeyConfig& cfg);
void SensitivityHotkeyConfigFromToml(const toml::table& tbl, SensitivityHotkeyConfig& cfg);
void DebugGlobalConfigFromToml(const toml::table& tbl, DebugGlobalConfig& cfg);
void CursorConfigFromToml(const toml::table& tbl, CursorConfig& cfg);
void CursorsConfigFromToml(const toml::table& tbl, CursorsConfig& cfg);
void CursorTrailConfigFromToml(const toml::table& tbl, CursorTrailConfig& cfg);
void EyeZoomConfigFromToml(const toml::table& tbl, EyeZoomConfig& cfg);
void KeyRebindFromToml(const toml::table& tbl, KeyRebind& cfg);
void KeyRebindsConfigFromToml(const toml::table& tbl, KeyRebindsConfig& cfg);
void AppearanceConfigFromToml(const toml::table& tbl, AppearanceConfig& cfg);
void ConfigFromToml(const toml::table& tbl, Config& config);


// Writes to a temp file next to `path`, then replaces `path` with it. On failure the temp file is
// removed and `path` is left untouched.
bool WriteFileAtomically(const std::wstring& path, const std::function<bool(std::ostream&)>& writeContents);

bool SaveConfigToTomlFile(const Config& config, const std::wstring& path);

bool LoadConfigFromTomlFile(const std::wstring& path, Config& config);


std::string GetEmbeddedDefaultConfigString();

std::vector<NinjabrainPresetDefinition> GetEmbeddedNinjabrainPresets();

bool LoadEmbeddedDefaultConfig(Config& config);

std::vector<ModeConfig> GetDefaultModesFromEmbedded();

std::vector<MirrorConfig> GetDefaultMirrorsFromEmbedded();

std::vector<MirrorGroupConfig> GetDefaultMirrorGroupsFromEmbedded();

std::vector<HotkeyConfig> GetDefaultHotkeysFromEmbedded();

std::vector<ImageConfig> GetDefaultImagesFromEmbedded();

CursorsConfig GetDefaultCursorsFromEmbedded();

EyeZoomConfig GetDefaultEyeZoomConfigFromEmbedded();


