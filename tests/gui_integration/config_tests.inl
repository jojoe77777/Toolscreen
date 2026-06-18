void RunConfigDefaultLoadTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    const std::filesystem::path root = PrepareCaseDirectory("config_default_load");
    ResetGlobalTestState(root);

    LoadConfig();

    const std::filesystem::path configPath = root / "config.toml";
    Expect(std::filesystem::exists(configPath), "Expected LoadConfig to create config.toml when it is missing.");
    Expect(!g_configLoadFailed.load(std::memory_order_acquire), "Expected default config load to succeed.");
    Expect(g_configLoaded.load(std::memory_order_acquire), "Expected default config load to mark configuration as ready.");
    Expect(!g_config.modes.empty(), "Expected loaded config to contain at least one mode.");
    Expect(!g_config.hotkeys.empty(), "Expected loaded config to contain at least one hotkey.");
    Expect(!g_config.defaultMode.empty(), "Expected loaded config to have a default mode.");
    Expect(g_config.keyRebinds.indicatorMode == 0,
           "Expected default config load to leave the key rebind indicator disabled.");
        Expect(g_config.ninjabrainOverlay.relativeTo == "topLeftScreen",
            "Expected default config load to take the Ninjabrain overlay anchor from embedded default.toml.");
        Expect(g_config.ninjabrainOverlay.x == 0 && g_config.ninjabrainOverlay.y == 0,
            "Expected default config load to reset the Ninjabrain overlay position to the preset defaults.");
        Expect(g_config.ninjabrainOverlay.customFontPath == "fonts/OpenSans-Regular.ttf",
            "Expected default config load to use the bundled OpenSans Ninjabrain overlay font.");
        ExpectFloatNear(g_config.ninjabrainOverlay.overlayScale, 0.30000001192092896f,
                  "Expected default config load to apply the Ninjabrain Bot preset overlay scale.");
        Expect(!g_config.ninjabrainOverlay.onlyOnMyScreen && !g_config.ninjabrainOverlay.onlyOnObs,
            "Expected default config load to leave both Ninjabrain visibility filters disabled.");
        Expect(!g_config.ninjabrainOverlay.hideIfStale && g_config.ninjabrainOverlay.hideIfStaleDelaySeconds == 30,
            "Expected default config load to use the default stale-hide Ninjabrain settings.");

    if (runMode == TestRunMode::Visual) {
        RunVisualLoop(window, "config-default-load", &RenderInteractiveSettingsFrame);
    }
}

void RunConfigRoundtripTest(TestRunMode runMode = TestRunMode::Automated) {
    RunRichConfigRoundtripCase("config_roundtrip", []() {
        VerifyRichGlobalSettings();
        VerifyRichModes();
        VerifyRichMirrors();
        VerifyRichMirrorGroups();
        VerifyRichImages();
        VerifyRichWindowOverlays();
        VerifyRichBrowserOverlays();
        VerifyRichHotkeys();
        VerifyRichSensitivityHotkeys();
        VerifyRichCursorsAndEyeZoom();
        VerifyRichKeyRebindsAndAppearance();
        VerifyRichDebugSettings();
    }, runMode);
}

void RunConfigRoundtripGlobalSettingsTest(TestRunMode runMode = TestRunMode::Automated) {
    RunRichConfigRoundtripCase("config_roundtrip_global_settings", &VerifyRichGlobalSettings, runMode);
}

void RunConfigRoundtripModesTest(TestRunMode runMode = TestRunMode::Automated) {
    RunRichConfigRoundtripCase("config_roundtrip_modes", &VerifyRichModes, runMode);
}

void RunConfigRoundtripMirrorsTest(TestRunMode runMode = TestRunMode::Automated) {
    RunRichConfigRoundtripCase("config_roundtrip_mirrors", &VerifyRichMirrors, runMode);
}

void RunConfigRoundtripMirrorGroupsTest(TestRunMode runMode = TestRunMode::Automated) {
    RunRichConfigRoundtripCase("config_roundtrip_mirror_groups", &VerifyRichMirrorGroups, runMode);
}

void RunConfigRoundtripImagesTest(TestRunMode runMode = TestRunMode::Automated) {
    RunRichConfigRoundtripCase("config_roundtrip_images", &VerifyRichImages, runMode);
}

void RunConfigRoundtripWindowOverlaysTest(TestRunMode runMode = TestRunMode::Automated) {
    RunRichConfigRoundtripCase("config_roundtrip_window_overlays", &VerifyRichWindowOverlays, runMode);
}

void RunConfigRoundtripBrowserOverlaysTest(TestRunMode runMode = TestRunMode::Automated) {
    RunRichConfigRoundtripCase("config_roundtrip_browser_overlays", &VerifyRichBrowserOverlays, runMode);
}

void RunConfigRoundtripHotkeysTest(TestRunMode runMode = TestRunMode::Automated) {
    RunRichConfigRoundtripCase("config_roundtrip_hotkeys", &VerifyRichHotkeys, runMode);
}

void RunConfigRoundtripSensitivityHotkeysTest(TestRunMode runMode = TestRunMode::Automated) {
    RunRichConfigRoundtripCase("config_roundtrip_sensitivity_hotkeys", &VerifyRichSensitivityHotkeys, runMode);
}

void RunConfigRoundtripCursorsAndEyeZoomTest(TestRunMode runMode = TestRunMode::Automated) {
    RunRichConfigRoundtripCase("config_roundtrip_cursors_eyezoom", &VerifyRichCursorsAndEyeZoom, runMode);
}

void RunConfigRoundtripKeyRebindsAndAppearanceTest(TestRunMode runMode = TestRunMode::Automated) {
    RunRichConfigRoundtripCase("config_roundtrip_key_rebinds_appearance", &VerifyRichKeyRebindsAndAppearance, runMode);
}

void RunConfigRoundtripDebugSettingsTest(TestRunMode runMode = TestRunMode::Automated) {
    RunRichConfigRoundtripCase("config_roundtrip_debug_settings", &VerifyRichDebugSettings, runMode);
}

void RunConfigLoadEmbeddedNinjabrainPresetsTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    const std::filesystem::path root = PrepareCaseDirectory("config_load_embedded_ninjabrain_presets");
    ResetGlobalTestState(root);

    const std::vector<NinjabrainPresetDefinition> presets = GetEmbeddedNinjabrainPresets();
    Expect(presets.size() == 2, "Expected two embedded Ninjabrain presets.");

    const auto findPreset = [&](const std::string& presetId) -> const NinjabrainPresetDefinition* {
     for (const NinjabrainPresetDefinition& preset : presets) {
         if (preset.id == presetId) {
          return &preset;
         }
     }
     return nullptr;
    };

    const NinjabrainPresetDefinition* compactPreset = findPreset("compact");
    const NinjabrainPresetDefinition* ninjabrainBotPreset = findPreset("ninjabrainbot");
    Expect(compactPreset != nullptr, "Expected the embedded compact preset to be available.");
    Expect(ninjabrainBotPreset != nullptr, "Expected the embedded Ninjabrain Bot preset to be available.");

    Expect(compactPreset->translationKey == "ninjabrain.preset_compact",
        "Expected the compact preset to carry its translation key.");
    Expect(compactPreset->preserveCurrentPlacement,
        "Expected the compact preset to preserve placement-specific runtime fields.");
    Expect(compactPreset->overlay.layoutStyle == "compact",
        "Expected the compact preset overlay to stay on the compact layout.");
    Expect(compactPreset->overlay.shownPredictions == 1,
        "Expected the compact preset to show a single prediction row.");
    Expect(compactPreset->overlay.columns.size() == 5,
        "Expected the compact preset to define the current five-column layout.");
    Expect(!compactPreset->overlay.showBoatStateInTopBar,
        "Expected the compact preset to keep the top-bar boat state disabled by default.");
    Expect(!compactPreset->overlay.hideIfStale && compactPreset->overlay.hideIfStaleDelaySeconds == 10,
        "Expected the compact preset to define the stale-hide defaults.");
    Expect(compactPreset->overlay.columns.front().header == "Location",
        "Expected the compact preset to rename the coords column header to Location.");

    Expect(ninjabrainBotPreset->translationKey == "ninjabrain.preset_ninjabrainbot",
        "Expected the Ninjabrain Bot preset to carry its translation key.");
    Expect(!ninjabrainBotPreset->preserveCurrentPlacement,
        "Expected the Ninjabrain Bot preset to replace placement-specific runtime fields.");
        Expect(ninjabrainBotPreset->overlay.titleText == "Ninjabrain Bot",
            "Expected embedded presets to load directly from config-style ninjabrainOverlay TOML.");
    Expect(ninjabrainBotPreset->overlay.relativeTo == "topLeftScreen",
        "Expected the Ninjabrain Bot preset to anchor to the top-left screen corner.");
    Expect(ninjabrainBotPreset->overlay.x == 0 && ninjabrainBotPreset->overlay.y == 0,
        "Expected the Ninjabrain Bot preset to reset its position.");
    Expect(!ninjabrainBotPreset->overlay.fontAntialiasing,
        "Expected the Ninjabrain Bot preset to disable font antialiasing.");
    ExpectFloatNear(ninjabrainBotPreset->overlay.informationMessagesMinWidth, 285.0f,
              "Expected the Ninjabrain Bot preset information-message width to come from TOML.");
    ExpectFloatNear(ninjabrainBotPreset->overlay.failureMarginLeft, 24.0f,
              "Expected the Ninjabrain Bot preset failed-result left margin to come from TOML.");
    Expect(!ninjabrainBotPreset->overlay.onlyOnMyScreen,
        "Expected the Ninjabrain Bot preset to render outside the local-only path.");
    Expect(!ninjabrainBotPreset->overlay.onlyOnObs,
        "Expected the Ninjabrain Bot preset to keep the OBS-only filter disabled by default.");
    Expect(!ninjabrainBotPreset->overlay.hideIfStale && ninjabrainBotPreset->overlay.hideIfStaleDelaySeconds == 10,
        "Expected the Ninjabrain Bot preset to define the stale-hide defaults.");

    if (runMode == TestRunMode::Visual) {
     RunVisualLoop(window, "config-load-embedded-ninjabrain-presets", &RenderInteractiveSettingsFrame);
    }
}

void RunConfigLoadBundledFontPathsNormalizedTest(TestRunMode runMode = TestRunMode::Automated) {
    RunConfigLoadCase("config_load_bundled_font_paths_normalized",
                      []() {
                          Config config;
                          const std::filesystem::path root(g_toolscreenPath);

                          config.fontPath = Narrow((root / "fonts" / "OpenSans-Regular.ttf").wstring());
                          config.eyezoom.textFontPath = Narrow((root / "fonts" / "Monocraft.ttf").wstring());
                          config.ninjabrainOverlay.customFontPath = Narrow((root / "Minecraft.ttf").wstring());

                          WriteConfigFixtureToDisk(config);
                      },
                      []() {
                          ExpectConfigLoadSucceeded("config-load-bundled-font-paths-normalized");
                          Expect(g_config.fontPath == "fonts/OpenSans-Regular.ttf",
                                 "Expected absolute bundled GUI font path to normalize to the preset-relative path.");
                          Expect(g_config.eyezoom.textFontPath == "fonts/Monocraft.ttf",
                                 "Expected absolute bundled EyeZoom font path to normalize to the preset-relative path.");
                          Expect(g_config.ninjabrainOverlay.customFontPath == "fonts/Minecraft.ttf",
                                 "Expected legacy bundled Ninjabrain font path to normalize to the preset-relative path.");
                      },
                      runMode);
}

void RunConfigLoadMissingRequiredModesTest(TestRunMode runMode = TestRunMode::Automated) {
    RunConfigLoadCase("config_load_missing_required_modes",
                      []() {
                          Config config;
                          config.defaultMode = kPrimaryModeId;
                          config.modes.clear();

                          ModeConfig primaryMode;
                          primaryMode.id = kPrimaryModeId;
                          primaryMode.width = 1111;
                          primaryMode.height = 666;
                          primaryMode.manualWidth = 1111;
                          primaryMode.manualHeight = 666;
                          config.modes.push_back(primaryMode);

                          WriteConfigFixtureToDisk(config);
                      },
                      []() {
                          ExpectConfigLoadSucceeded("config-load-missing-required-modes");
                          const ModeConfig& fullscreen = FindModeOrThrow("Fullscreen");
                          const ModeConfig& eyezoom = FindModeOrThrow("EyeZoom");
                          const ModeConfig& preemptive = FindModeOrThrow("Preemptive");
                          const ModeConfig& thin = FindModeOrThrow("Thin");
                          const ModeConfig& wide = FindModeOrThrow("Wide");
                          (void)thin;
                          (void)wide;
                          Expect(fullscreen.stretch.enabled, "Expected missing Fullscreen mode to be recreated with stretch enabled.");
                          Expect(fullscreen.width > 0 && fullscreen.height > 0,
                                 "Expected recreated Fullscreen mode to receive valid dimensions.");
                          Expect(eyezoom.width > 0 && eyezoom.height > 0, "Expected missing EyeZoom mode to be recreated.");
                          Expect(preemptive.width == eyezoom.width && preemptive.height == eyezoom.height,
                                 "Expected missing Preemptive mode to inherit EyeZoom dimensions.");
                          Expect(!preemptive.useRelativeSize, "Expected recreated Preemptive mode to use absolute sizing.");
                      },
                      runMode);
}

void RunConfigLoadInvalidHotkeyModeReferencesTest(TestRunMode runMode = TestRunMode::Automated) {
    RunConfigLoadCase("config_load_invalid_hotkey_mode_references",
                      []() {
                          Config config;
                          config.defaultMode = kPrecisionModeId;
                          config.modes.clear();

                          ModeConfig precisionMode;
                          precisionMode.id = kPrecisionModeId;
                          precisionMode.width = 900;
                          precisionMode.height = 500;
                          precisionMode.manualWidth = 900;
                          precisionMode.manualHeight = 500;
                          config.modes.push_back(precisionMode);

                          HotkeyConfig invalidHotkey;
                          invalidHotkey.keys = { VK_F2 };
                          invalidHotkey.mainMode = "Missing Main";
                          invalidHotkey.secondaryMode = "Missing Secondary";
                          invalidHotkey.altSecondaryModes = {
                              { { 'A' }, "Missing Alt" },
                              { { 'B' }, kPrecisionModeId },
                          };
                          config.hotkeys = { invalidHotkey };

                          WriteConfigFixtureToDisk(config);
                      },
                      []() {
                          ExpectConfigLoadSucceeded("config-load-invalid-hotkey-mode-references");
                          Expect(!g_config.hotkeys.empty(), "Expected hotkey fixture to load.");
                          const HotkeyConfig& hotkey = g_config.hotkeys.front();
                          Expect(hotkey.mainMode == kPrecisionModeId,
                                 "Expected invalid hotkey main mode to reset to the existing default mode.");
                          Expect(hotkey.secondaryMode.empty(), "Expected invalid hotkey secondary mode to be cleared.");
                          Expect(hotkey.altSecondaryModes.size() == 1, "Expected invalid alt secondary modes to be removed.");
                          Expect(hotkey.altSecondaryModes.front().mode == kPrecisionModeId,
                                 "Expected valid alt secondary mode to remain after sanitization.");
                      },
                      runMode);
}

void RunConfigLoadRelativeModeDimensionsTest(TestRunMode runMode = TestRunMode::Automated) {
    int expectedWidth = 1000;
    int expectedHeight = 600;

    RunConfigLoadCase("config_load_relative_mode_dimensions",
                      [&]() {
                          Config config;
                          config.defaultMode = kRelativeModeId;
                          config.modes.clear();

                          ModeConfig relativeMode;
                          relativeMode.id = kRelativeModeId;
                          relativeMode.useRelativeSize = true;
                          relativeMode.relativeWidth = 0.625f;
                          relativeMode.relativeHeight = 0.4f;
                          relativeMode.width = 1000;
                          relativeMode.height = 600;
                          relativeMode.manualWidth = 1000;
                          relativeMode.manualHeight = 600;
                          config.modes.push_back(relativeMode);

                          const int loadScreenWidth = (std::max)(1, GetCachedWindowWidth());
                          const int loadScreenHeight = (std::max)(1, GetCachedWindowHeight());
                          expectedWidth = (std::max)(1, static_cast<int>(std::lround(0.625f * static_cast<float>(loadScreenWidth))));
                          expectedHeight = (std::max)(1, static_cast<int>(std::lround(0.4f * static_cast<float>(loadScreenHeight))));

                          WriteConfigFixtureToDisk(config);
                      },
                      [&]() {
                          ExpectConfigLoadSucceeded("config-load-relative-mode-dimensions");
                          const ModeConfig& relativeMode = FindModeOrThrow(kRelativeModeId);
                          Expect(relativeMode.useRelativeSize, "Expected relative mode to stay marked as relative.");
                          ExpectFloatNear(relativeMode.relativeWidth, 0.625f, "Expected relative mode relativeWidth to roundtrip.");
                          ExpectFloatNear(relativeMode.relativeHeight, 0.4f, "Expected relative mode relativeHeight to roundtrip.");
                          Expect(relativeMode.width == expectedWidth, "Expected relative mode width to be recomputed from cached client width.");
                          Expect(relativeMode.height == expectedHeight,
                                 "Expected relative mode height to be recomputed from cached client height.");
                          Expect(relativeMode.manualWidth == 1000 && relativeMode.manualHeight == 600,
                                 "Expected relative mode manual dimensions to preserve their persisted values.");
                      },
                      runMode);
}

void RunConfigLoadExpressionModeDimensionsTest(TestRunMode runMode = TestRunMode::Automated) {
    int expectedWidth = 123;
    int expectedHeight = 456;

    RunConfigLoadCase("config_load_expression_mode_dimensions",
                      [&]() {
                          Config config;
                          config.defaultMode = kExpressionModeId;
                          config.modes.clear();

                          ModeConfig expressionMode;
                          expressionMode.id = kExpressionModeId;
                          expressionMode.widthExpr = "screenWidth / 3";
                          expressionMode.heightExpr = "screenHeight - 111";
                          expressionMode.width = 123;
                          expressionMode.height = 456;
                          expressionMode.manualWidth = 123;
                          expressionMode.manualHeight = 456;
                          config.modes.push_back(expressionMode);

                          const int loadScreenWidth = (std::max)(1, GetCachedWindowWidth());
                          const int loadScreenHeight = (std::max)(1, GetCachedWindowHeight());
                          expectedWidth = (std::max)(1, loadScreenWidth / 3);
                          expectedHeight = (std::max)(1, loadScreenHeight - 111);

                          WriteConfigFixtureToDisk(config);
                      },
                      [&]() {
                          ExpectConfigLoadSucceeded("config-load-expression-mode-dimensions");
                          const ModeConfig& expressionMode = FindModeOrThrow(kExpressionModeId);
                          Expect(expressionMode.widthExpr == "screenWidth / 3", "Expected width expression to roundtrip.");
                          Expect(expressionMode.heightExpr == "screenHeight - 111", "Expected height expression to roundtrip.");
                          Expect(expressionMode.width == expectedWidth, "Expected width expression to be evaluated during load.");
                          Expect(expressionMode.height == expectedHeight, "Expected height expression to be evaluated during load.");
                      },
                      runMode);
}

void RunConfigLoadLegacyVersionUpgradeTest(TestRunMode runMode = TestRunMode::Automated) {
    RunConfigLoadCase("config_load_legacy_version_upgrade",
                      []() {
                          Config config;
                          config.configVersion = 1;
                          config.disableHookChaining = true;
                          config.defaultMode = kPrecisionModeId;
                          config.keyRepeatStartDelay = 10;
                          config.keyRepeatDelay = 0;
                          config.modes.clear();

                          ModeConfig precisionMode;
                          precisionMode.id = kPrecisionModeId;
                          precisionMode.width = 800;
                          precisionMode.height = 450;
                          precisionMode.manualWidth = 800;
                          precisionMode.manualHeight = 450;
                          config.modes.push_back(precisionMode);

                          WriteConfigFixtureToDisk(config);

                             const std::filesystem::path configPath = GetCurrentConfigPath();
                             std::ifstream in(configPath, std::ios::binary);
                             Expect(in.is_open(), "Failed to reopen legacy config fixture for keyRepeatDelay migration rewrite.");
                             std::string rawToml((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                             in.close();

                                 const std::string currentDelayLinePrefix = "keyRepeatDelay = ";
                                 const size_t delayLineStart = rawToml.find(currentDelayLinePrefix);
                                 Expect(delayLineStart != std::string::npos,
                                     "Expected serialized legacy config fixture to contain the keyRepeatDelay line.");
                                 size_t delayLineEnd = rawToml.find('\n', delayLineStart);
                                 if (delayLineEnd == std::string::npos) {
                                  delayLineEnd = rawToml.size();
                                 }
                                 rawToml.replace(delayLineStart, delayLineEnd - delayLineStart, "keyRepeatDelay = 0");
                             WriteRawConfigTomlToDisk(rawToml);
                      },
                      []() {
                          ExpectConfigLoadSucceeded("config-load-legacy-version-upgrade");
                          Expect(g_config.configVersion == GetConfigVersion(), "Expected legacy config version to upgrade to the current version.");
                          Expect(g_config.disableHookChaining,
                                 "Expected legacy disableHookChaining to remain preserved on the 1.2.1 branch.");
                             Expect(g_config.useSystemKeyRepeat,
                                 "Expected legacy configs without the useSystemKeyRepeat flag to load with system key repeat forced on.");
                             Expect(g_config.keyRepeatStartDelay == 100,
                                 "Expected legacy non-auto keyRepeatStartDelay to normalize to the minimum supported value.");
                             Expect(g_config.keyRepeatDelay == ConfigDefaults::CONFIG_KEY_REPEAT_DELAY,
                                 "Expected legacy zero keyRepeatDelay to normalize to the default value.");
                      },
                      runMode);
}

void RunConfigLoadClampGlobalValuesTest(TestRunMode runMode = TestRunMode::Automated) {
    RunConfigLoadCase("config_load_clamp_global_values",
                      []() {
                          Config config;
                          config.defaultMode = "Fullscreen";
                          config.obsFramerate = 999;
                          config.keyRepeatStartDelay = 97;
                          config.keyRepeatDelay = 999;
                          config.cursors.enabled = true;
                          config.cursors.title.cursorSize = 9999;
                          config.cursors.wall.cursorSize = 2;
                          config.cursors.ingame.cursorSize = 321;
                          WriteConfigFixtureToDisk(config);
                      },
                      []() {
                          ExpectConfigLoadSucceeded("config-load-clamp-global-values");
                          Expect(g_config.obsFramerate == 120, "Expected obsFramerate to clamp to the configured maximum.");
                         Expect(g_config.keyRepeatStartDelay == 100,
                             "Expected keyRepeatStartDelay to clamp to the minimum supported non-auto value.");
                         Expect(g_config.keyRepeatDelay == 300,
                             "Expected keyRepeatDelay to clamp to the system-repeat maximum.");
                          Expect(g_config.cursors.title.cursorSize == ConfigDefaults::CURSOR_MAX_SIZE,
                                 "Expected title cursor size to clamp to CURSOR_MAX_SIZE.");
                          Expect(g_config.cursors.wall.cursorSize == ConfigDefaults::CURSOR_MIN_SIZE,
                                 "Expected wall cursor size to clamp to CURSOR_MIN_SIZE.");
                          Expect(g_config.cursors.ingame.cursorSize == ConfigDefaults::CURSOR_MAX_SIZE,
                                 "Expected ingame cursor size to clamp to CURSOR_MAX_SIZE.");
                      },
                      runMode);
}

void RunConfigLoadClampGlobalValuesSystemKeyRepeatTest(TestRunMode runMode = TestRunMode::Automated) {
    RunConfigLoadCase("config_load_clamp_global_values_system_key_repeat",
                      []() {
                          Config config;
                          config.useSystemKeyRepeat = true;
                          config.keyRepeatStartDelay = 97;
                          config.keyRepeatDelay = 999;
                          WriteConfigFixtureToDisk(config);
                      },
                      []() {
                          ExpectConfigLoadSucceeded("config-load-clamp-global-values-system-key-repeat");
                          Expect(g_config.useSystemKeyRepeat,
                                 "Expected useSystemKeyRepeat to remain enabled while loading the system repeat clamp case.");
                          Expect(g_config.keyRepeatStartDelay == 100,
                                 "Expected system-repeat keyRepeatStartDelay to clamp to the minimum supported non-auto value.");
                          Expect(g_config.keyRepeatDelay == 300,
                                 "Expected system-repeat keyRepeatDelay to clamp to the legacy 300ms maximum.");
                      },
                      runMode);
}

void RunConfigLoadModeDefaultDimensionsRestoredTest(TestRunMode runMode = TestRunMode::Automated) {
    RunConfigLoadCase("config_load_mode_default_dimensions_restored",
                      []() {
                          WriteRawConfigTomlToDisk(R"(configVersion = 4
defaultMode = "Wide"

[[mode]]
id = "Wide"
width = 1
height = 0
)");
                      },
                      []() {
                          ExpectConfigLoadSucceeded("config-load-mode-default-dimensions-restored");
                          const ModeConfig& wideMode = FindModeOrThrow("Wide");
                          Expect(wideMode.useRelativeSize, "Expected repaired Wide mode to remain relative-sized.");
                          Expect(!wideMode.widthExpr.empty(), "Expected repaired Wide mode width expression to come from embedded defaults.");
                          ExpectFloatNear(wideMode.relativeHeight, 0.25f,
                                          "Expected repaired Wide mode relativeHeight to come from embedded defaults.");
                          Expect(wideMode.width > 1, "Expected repaired Wide mode width to be recomputed from the restored default expression.");
                          Expect(wideMode.height > 1, "Expected repaired Wide mode height to be recomputed from the restored default percentage.");
                      },
                      runMode);
}

void RunConfigLoadModeSourceListsLoadedTest(TestRunMode runMode = TestRunMode::Automated) {
    RunConfigLoadCase("config_load_mode_source_lists_loaded",
                      []() {
                          WriteRawConfigTomlToDisk(R"(configVersion = 4
defaultMode = "Source Lists"

[[mode]]
id = "Source Lists"
width = 800
height = 600
mirrorIds = ["Mirror One"]
mirrorGroupIds = ["Group One"]
imageIds = ["Image One"]
windowOverlayIds = ["Window One"]
browserOverlayIds = ["Browser One"]
)");
                      },
                      []() {
                          ExpectConfigLoadSucceeded("config-load-mode-source-lists-loaded");
                          const ModeConfig& mode = FindModeOrThrow("Source Lists");
                          ExpectVectorEquals(SourceIdsOfType(mode, ModeSourceType::Mirror), std::vector<std::string>{ "Mirror One" },
                                             "Expected legacy mirrorIds to migrate into mirror sources.");
                          ExpectVectorEquals(SourceIdsOfType(mode, ModeSourceType::MirrorGroup), std::vector<std::string>{ "Group One" },
                                             "Expected legacy mirrorGroupIds to migrate into mirror-group sources.");
                          ExpectVectorEquals(SourceIdsOfType(mode, ModeSourceType::Image), std::vector<std::string>{ "Image One" },
                                             "Expected legacy imageIds to migrate into image sources.");
                          ExpectVectorEquals(SourceIdsOfType(mode, ModeSourceType::WindowOverlay), std::vector<std::string>{ "Window One" },
                                             "Expected legacy windowOverlayIds to migrate into window-overlay sources.");
                          ExpectVectorEquals(SourceIdsOfType(mode, ModeSourceType::BrowserOverlay), std::vector<std::string>{ "Browser One" },
                                             "Expected legacy browserOverlayIds to migrate into browser-overlay sources.");
                      },
                      runMode);
}

void RunConfigMigrateVersionAppliesTest(TestRunMode runMode = TestRunMode::Automated) {
    (void)runMode;

    Config config;
    config.configVersion = 2;
    config.fpsLimit = 144;
    config.limitCaptureFramerate = true;

    Expect(MigrateConfigToCurrentVersion(config), "Expected an old-version config to report a migration.");
    Expect(config.configVersion == GetConfigVersion(), "Expected the config version to be stamped to current.");
    Expect(config.fpsLimit == 0, "Expected the v3 migration to reset fpsLimit.");
    Expect(!config.limitCaptureFramerate, "Expected the v3 migration to clear limitCaptureFramerate.");

    config.fpsLimit = 144;
    Expect(!MigrateConfigToCurrentVersion(config), "Expected an already-current config to need no migration.");
    Expect(config.fpsLimit == 144, "Expected a no-op migration to leave fields untouched.");
    Expect(config.configVersion == GetConfigVersion(), "Expected the version to remain current after a no-op migration.");
}

void RunConfigDowngradeSourcesCompatTest(TestRunMode runMode = TestRunMode::Automated) {
    (void)runMode;

    Config config;
    ModeConfig mode;
    mode.id = "Downgrade Mode";
    mode.width = 800;
    mode.height = 600;
    AddModeSource(mode, ModeSourceType::Mirror, "Mirror One");
    AddModeSource(mode, ModeSourceType::Image, "Image One");
    AddModeSource(mode, ModeSourceType::Mirror, "Mirror Two");
    AddModeSource(mode, ModeSourceType::MirrorGroup, "Group One");
    AddModeSource(mode, ModeSourceType::WindowOverlay, "Window One");
    AddModeSource(mode, ModeSourceType::BrowserOverlay, "Browser One");
    config.modes.push_back(mode);

    toml::table serialized;
    ConfigToToml(config, serialized);

    auto modeArr = serialized.get_as<toml::array>("mode");
    Expect(modeArr != nullptr && modeArr->size() == 1, "Expected exactly one serialized mode.");
    toml::table* modeTbl = modeArr->get(0)->as_table();
    Expect(modeTbl != nullptr, "Expected the serialized mode to be a table.");

    const auto legacyList = [&](const char* key) {
        std::vector<std::string> ids;
        if (auto arr = modeTbl->get_as<toml::array>(key)) {
            for (auto& elem : *arr) {
                if (auto val = elem.value<std::string>()) { ids.push_back(*val); }
            }
        }
        return ids;
    };
    ExpectVectorEquals(legacyList("mirrorIds"), std::vector<std::string>{ "Mirror One", "Mirror Two" },
                       "Expected sources to also serialize legacy mirrorIds in order for downgrade.");
    ExpectVectorEquals(legacyList("mirrorGroupIds"), std::vector<std::string>{ "Group One" },
                       "Expected sources to also serialize legacy mirrorGroupIds for downgrade.");
    ExpectVectorEquals(legacyList("imageIds"), std::vector<std::string>{ "Image One" },
                       "Expected sources to also serialize legacy imageIds for downgrade.");
    ExpectVectorEquals(legacyList("windowOverlayIds"), std::vector<std::string>{ "Window One" },
                       "Expected sources to also serialize legacy windowOverlayIds for downgrade.");
    ExpectVectorEquals(legacyList("browserOverlayIds"), std::vector<std::string>{ "Browser One" },
                       "Expected sources to also serialize legacy browserOverlayIds for downgrade.");
}

void RunConfigLoadModePercentageDimensionsDetectedTest(TestRunMode runMode = TestRunMode::Automated) {
    int expectedWidth = 1;
    int expectedHeight = 1;

    RunConfigLoadCase("config_load_mode_percentage_dimensions_detected",
                      [&]() {
                          const int screenWidth = (std::max)(1, GetCachedWindowWidth());
                          const int screenHeight = (std::max)(1, GetCachedWindowHeight());
                          expectedWidth = (std::max)(1, static_cast<int>(std::lround(0.5f * static_cast<float>(screenWidth))));
                          expectedHeight = (std::max)(1, static_cast<int>(std::lround(0.25f * static_cast<float>(screenHeight))));

                          WriteRawConfigTomlToDisk(R"(configVersion = 4
defaultMode = "Percentage Mode"

[[mode]]
id = "Percentage Mode"
width = 0.5
height = 0.25
)");
                      },
                      [&]() {
                          ExpectConfigLoadSucceeded("config-load-mode-percentage-dimensions-detected");
                          const ModeConfig& mode = FindModeOrThrow("Percentage Mode");
                          Expect(mode.useRelativeSize, "Expected percentage width/height values to mark the mode as relative-sized.");
                          ExpectFloatNear(mode.relativeWidth, 0.5f, "Expected percentage width to map to relativeWidth.");
                          ExpectFloatNear(mode.relativeHeight, 0.25f, "Expected percentage height to map to relativeHeight.");
                          Expect(mode.width == expectedWidth, "Expected percentage width to resolve against the cached window width.");
                          Expect(mode.height == expectedHeight, "Expected percentage height to resolve against the cached window height.");
                      },
                      runMode);
}

void RunConfigLoadModeTypedSourcesLoadedTest(TestRunMode runMode = TestRunMode::Automated) {
    RunConfigLoadCase("config_load_mode_typed_sources_loaded",
                      []() {
                          WriteRawConfigTomlToDisk(R"(configVersion = 4
defaultMode = "Typed Sources"

[[mode]]
id = "Typed Sources"
width = 800
height = 600
sources = [
    { type = "Mirror", id = "" },
    { type = "Mirror", id = "Valid Mirror" },
    { type = "Image", id = "" }
]
)");
                      },
                      []() {
                          ExpectConfigLoadSucceeded("config-load-mode-typed-sources-loaded");
                          const ModeConfig& mode = FindModeOrThrow("Typed Sources");
                          ExpectVectorEquals(SourceIdsOfType(mode, ModeSourceType::Mirror), std::vector<std::string>{ "Valid Mirror" },
                                             "Expected typed mirror sources to load, skipping empty ids.");
                          Expect(SourceIdsOfType(mode, ModeSourceType::Image).empty(),
                                 "Expected the empty-id image source to be skipped.");
                          Expect(mode.sources.size() == 1, "Expected only the one valid typed source to load.");
                      },
                      runMode);
}

void RunConfigLoadEmptyMainHotkeyFallbackTest(TestRunMode runMode = TestRunMode::Automated) {
    RunConfigLoadCase("config_load_empty_main_hotkey_fallback",
                      []() {
                          Config config;
                          config.defaultMode = kPrecisionModeId;

                          ModeConfig precisionMode;
                          precisionMode.id = kPrecisionModeId;
                          precisionMode.width = 900;
                          precisionMode.height = 500;
                          precisionMode.manualWidth = 900;
                          precisionMode.manualHeight = 500;
                          config.modes.push_back(precisionMode);

                          HotkeyConfig hotkey;
                          hotkey.keys = { VK_F2 };
                          hotkey.mainMode.clear();
                          hotkey.secondaryMode = kPrecisionModeId;
                          config.hotkeys.push_back(hotkey);

                          WriteConfigFixtureToDisk(config);
                      },
                      []() {
                          ExpectConfigLoadSucceeded("config-load-empty-main-hotkey-fallback");
                          const HotkeyConfig& hotkey = FindHotkeyByKeysOrThrow({ VK_F2 });
                          Expect(hotkey.mainMode == kPrecisionModeId,
                                 "Expected a hotkey with an empty main mode to fall back to the default mode.");
                      },
                      runMode);
}

void RunConfigLoadMissingGuiHotkeyDefaultedTest(TestRunMode runMode = TestRunMode::Automated) {
    RunConfigLoadCase("config_load_missing_gui_hotkey_defaulted",
                      []() {
                          WriteRawConfigTomlToDisk(R"(configVersion = 4
defaultMode = "Fullscreen"
)");
                      },
                      []() {
                          ExpectConfigLoadSucceeded("config-load-missing-gui-hotkey-defaulted");
                          ExpectVectorEquals(g_config.guiHotkey, ConfigDefaults::GetDefaultGuiHotkey(),
                                             "Expected a missing guiHotkey to default to the configured hotkey.");
                      },
                      runMode);
}

void RunConfigLoadEmptyGuiHotkeyDefaultedTest(TestRunMode runMode = TestRunMode::Automated) {
    RunConfigLoadCase("config_load_empty_gui_hotkey_defaulted",
                      []() {
                          WriteRawConfigTomlToDisk(R"(configVersion = 4
defaultMode = "Fullscreen"
guiHotkey = []
)");
                      },
                      []() {
                          ExpectConfigLoadSucceeded("config-load-empty-gui-hotkey-defaulted");
                          ExpectVectorEquals(g_config.guiHotkey, ConfigDefaults::GetDefaultGuiHotkey(),
                                             "Expected an empty guiHotkey array to fall back to the configured default hotkey.");
                      },
                      runMode);
}

void RunConfigLoadLegacyMirrorGammaMigratedTest(TestRunMode runMode = TestRunMode::Automated) {
    RunConfigLoadCase("config_load_legacy_mirror_gamma_migrated",
                      []() {
                          WriteRawConfigTomlToDisk(R"(configVersion = 4
defaultMode = "Fullscreen"

[[mirror]]
name = "Legacy Gamma"
gammaMode = "Linear"
)");
                      },
                      []() {
                          ExpectConfigLoadSucceeded("config-load-legacy-mirror-gamma-migrated");
                          Expect(g_config.mirrorGammaMode == MirrorGammaMode::AssumeLinear,
                                 "Expected legacy per-mirror gammaMode to migrate into the global mirror gamma mode.");
                      },
                      runMode);
}

void RunConfigLoadMirrorCaptureDimensionsClampedTest(TestRunMode runMode = TestRunMode::Automated) {
    RunConfigLoadCase("config_load_mirror_capture_dimensions_clamped",
                      []() {
                          Config config;
                          config.defaultMode = "Fullscreen";

                          MirrorConfig mirror;
                          mirror.name = "Clamp Mirror";
                          mirror.captureWidth = ConfigDefaults::MIRROR_CAPTURE_MAX_DIMENSION + 200;
                          mirror.captureHeight = 0;
                          config.mirrors.push_back(mirror);

                          WriteConfigFixtureToDisk(config);
                      },
                      []() {
                          ExpectConfigLoadSucceeded("config-load-mirror-capture-dimensions-clamped");
                          const MirrorConfig& mirror = FindMirrorOrThrow("Clamp Mirror");
                          Expect(mirror.captureWidth == ConfigDefaults::MIRROR_CAPTURE_MAX_DIMENSION,
                                 "Expected mirror captureWidth to clamp to MIRROR_CAPTURE_MAX_DIMENSION.");
                          Expect(mirror.captureHeight == ConfigDefaults::MIRROR_CAPTURE_MIN_DIMENSION,
                                 "Expected mirror captureHeight to clamp to MIRROR_CAPTURE_MIN_DIMENSION.");
                      },
                      runMode);
}

void RunConfigLoadEyeZoomCloneWidthNormalizedTest(TestRunMode runMode = TestRunMode::Automated) {
    RunConfigLoadCase("config_load_eyezoom_clone_width_normalized",
                      []() {
                          WriteRawConfigTomlToDisk(R"(configVersion = 4
defaultMode = "Fullscreen"

[eyezoom]
cloneWidth = 15
)");
                      },
                      []() {
                          ExpectConfigLoadSucceeded("config-load-eyezoom-clone-width-normalized");
                          Expect(g_config.eyezoom.cloneWidth == 14,
                                 "Expected odd eyezoom cloneWidth values to normalize to the nearest even width.");
                      },
                      runMode);
}

void RunConfigLoadEyeZoomOverlayWidthDefaultedTest(TestRunMode runMode = TestRunMode::Automated) {
    RunConfigLoadCase("config_load_eyezoom_overlay_width_defaulted",
                      []() {
                          WriteRawConfigTomlToDisk(R"(configVersion = 4
defaultMode = "Fullscreen"

[eyezoom]
cloneWidth = 14
)");
                      },
                      []() {
                          ExpectConfigLoadSucceeded("config-load-eyezoom-overlay-width-defaulted");
                          Expect(g_config.eyezoom.overlayWidth == 7,
                                 "Expected a missing eyezoom overlayWidth to default to half of cloneWidth.");
                      },
                      runMode);
}

void RunConfigLoadEyeZoomOverlayWidthClampedTest(TestRunMode runMode = TestRunMode::Automated) {
    RunConfigLoadCase("config_load_eyezoom_overlay_width_clamped",
                      []() {
                          WriteRawConfigTomlToDisk(R"(configVersion = 4
defaultMode = "Fullscreen"

[eyezoom]
cloneWidth = 10
overlayWidth = 99
)");
                      },
                      []() {
                          ExpectConfigLoadSucceeded("config-load-eyezoom-overlay-width-clamped");
                          Expect(g_config.eyezoom.overlayWidth == 5,
                                 "Expected eyezoom overlayWidth to clamp to half of cloneWidth.");
                      },
                      runMode);
}

void RunConfigLoadEyeZoomLegacyMarginsMigratedTest(TestRunMode runMode = TestRunMode::Automated) {
    int expectedWidth = 1;
    int expectedHeight = 1;

    RunConfigLoadCase("config_load_eyezoom_legacy_margins_migrated",
                      [&]() {
                          const int screenWidth = (std::max)(1, GetCachedWindowWidth());
                          const int screenHeight = (std::max)(1, GetCachedWindowHeight());
                          const int viewportX = (screenWidth - 400) / 2;
                          expectedWidth = (viewportX > 0) ? (viewportX - (2 * 20)) : screenWidth;
                          expectedHeight = screenHeight - (2 * 30);

                          WriteRawConfigTomlToDisk(R"(configVersion = 4
defaultMode = "Fullscreen"

[eyezoom]
windowWidth = 400
horizontalMargin = 20
verticalMargin = 30
)");
                      },
                      [&]() {
                          ExpectConfigLoadSucceeded("config-load-eyezoom-legacy-margins-migrated");
                          Expect(g_config.eyezoom.zoomAreaWidth == expectedWidth,
                                 "Expected legacy eyezoom horizontalMargin to migrate into zoomAreaWidth.");
                          Expect(g_config.eyezoom.zoomAreaHeight == expectedHeight,
                                 "Expected legacy eyezoom verticalMargin to migrate into zoomAreaHeight.");
                      },
                      runMode);
}

void RunConfigLoadEyeZoomLegacyCustomPositionMigratedTest(TestRunMode runMode = TestRunMode::Automated) {
    RunConfigLoadCase("config_load_eyezoom_legacy_custom_position_migrated",
                      []() {
                          WriteRawConfigTomlToDisk(R"(configVersion = 4
defaultMode = "Fullscreen"

[eyezoom]
useCustomPosition = true
)");
                      },
                      []() {
                          ExpectConfigLoadSucceeded("config-load-eyezoom-legacy-custom-position-migrated");
                          Expect(g_config.eyezoom.useCustomSizePosition,
                                 "Expected legacy eyezoom useCustomPosition to map to useCustomSizePosition.");
                      },
                      runMode);
}

void RunConfigLoadEyeZoomInvalidActiveOverlayResetTest(TestRunMode runMode = TestRunMode::Automated) {
    RunConfigLoadCase("config_load_eyezoom_invalid_active_overlay_reset",
                      []() {
                          Config config;
                          config.defaultMode = "Fullscreen";
                          config.eyezoom.activeOverlayIndex = 4;
                          config.eyezoom.overlays = {
                              { "Only Overlay", "C:\\temp\\overlay.png", EyeZoomOverlayDisplayMode::Fit, 120, 80, false, 0.75f },
                          };
                          WriteConfigFixtureToDisk(config);
                      },
                      []() {
                          ExpectConfigLoadSucceeded("config-load-eyezoom-invalid-active-overlay-reset");
                          Expect(g_config.eyezoom.activeOverlayIndex == -1,
                                 "Expected out-of-range eyezoom activeOverlayIndex values to reset to -1.");
                      },
                      runMode);
}

void RunConfigLoadWindowOverlayCaptureMethodMigratedTest(TestRunMode runMode = TestRunMode::Automated) {
    RunConfigLoadCase("config_load_window_overlay_capture_method_migrated",
                      []() {
                          WriteRawConfigTomlToDisk(R"(configVersion = 4
defaultMode = "Fullscreen"

[[windowOverlay]]
name = "Legacy Capture"
captureMethod = "Auto"
)");
                      },
                      []() {
                          ExpectConfigLoadSucceeded("config-load-window-overlay-capture-method-migrated");
                          const WindowOverlayConfig& overlay = FindWindowOverlayOrThrow("Legacy Capture");
                          Expect(overlay.captureMethod == "Windows 10+",
                                 "Expected legacy window overlay captureMethod values to migrate to Windows 10+.");
                      },
                      runMode);
}

void RunConfigLoadWindowOverlayCropMigratedTest(TestRunMode runMode = TestRunMode::Automated) {
    RunConfigLoadCase("config_load_window_overlay_crop_migrated",
                      []() {
                          WriteRawConfigTomlToDisk(R"(configVersion = 4
defaultMode = "Fullscreen"

[[windowOverlay]]
name = "Legacy Crop"
crop_top = 4
crop_bottom = 5
crop_left = 6
crop_right = 7
)");
                      },
                      []() {
                          ExpectConfigLoadSucceeded("config-load-window-overlay-crop-migrated");
                          const WindowOverlayConfig& overlay = FindWindowOverlayOrThrow("Legacy Crop");
                          Expect(overlay.crop_top == 8 && overlay.crop_bottom == 10 && overlay.crop_left == 12 &&
                                     overlay.crop_right == 14,
                                 "Expected legacy window overlay crop values to migrate to the v5 scale.");
                          Expect(!overlay.cropToWidth && !overlay.cropToHeight,
                                 "Expected crop-to toggles to stay disabled for migrated legacy window overlays.");
                          Expect(g_config.configVersion == GetConfigVersion(),
                                 "Expected legacy crop migration to upgrade the config to the current version.");
                      },
                      runMode);
}

void RunConfigLoadKeyRebindUnicodeStringParsedTest(TestRunMode runMode = TestRunMode::Automated) {
    RunConfigLoadCase("config_load_key_rebind_unicode_string_parsed",
                      []() {
                          WriteRawConfigTomlToDisk(R"(configVersion = 4
defaultMode = "Fullscreen"

[keyRebinds]
enabled = true
resolveRebindTargetsForHotkeys = false
toggleHotkey = []

[[keyRebinds.rebinds]]
fromKey = 74
toKey = 75
enabled = true
useCustomOutput = true
customOutputUnicode = "U+00F8"
shiftLayerEnabled = true
shiftLayerOutputUnicode = "{00D8}"
)");
                      },
                      []() {
                          ExpectConfigLoadSucceeded("config-load-key-rebind-unicode-string-parsed");
                          Expect(g_config.keyRebinds.rebinds.size() == 1, "Expected exactly one key rebind fixture to load.");
                          const KeyRebind& rebind = g_config.keyRebinds.rebinds.front();
                          Expect(rebind.customOutputUnicode == 0x00F8,
                                 "Expected customOutputUnicode strings to parse into Unicode code points.");
                          Expect(rebind.shiftLayerOutputUnicode == 0x00D8,
                                 "Expected shiftLayerOutputUnicode strings to parse into Unicode code points.");
                      },
                      runMode);
}

void RunConfigLoadKeyRebindEscapedUnicodeStringParsedTest(TestRunMode runMode = TestRunMode::Automated) {
    RunConfigLoadCase("config_load_key_rebind_escaped_unicode_string_parsed",
                      []() {
                          WriteRawConfigTomlToDisk(R"(configVersion = 4
defaultMode = "Fullscreen"

[keyRebinds]
enabled = true
resolveRebindTargetsForHotkeys = false
toggleHotkey = []

[[keyRebinds.rebinds]]
fromKey = 74
toKey = 75
enabled = true
useCustomOutput = true
customOutputUnicode = "\\u00f8"
shiftLayerEnabled = true
shiftLayerOutputUnicode = "\\U00D8"
)");
                      },
                      []() {
                          ExpectConfigLoadSucceeded("config-load-key-rebind-escaped-unicode-string-parsed");
                          Expect(g_config.keyRebinds.rebinds.size() == 1, "Expected exactly one key rebind fixture to load.");
                          const KeyRebind& rebind = g_config.keyRebinds.rebinds.front();
                          Expect(rebind.customOutputUnicode == 0x00F8,
                                 "Expected escaped customOutputUnicode strings to parse into Unicode code points.");
                          Expect(rebind.shiftLayerOutputUnicode == 0x00D8,
                                 "Expected escaped shiftLayerOutputUnicode strings to parse into Unicode code points.");
                      },
                      runMode);
}

void RunConfigLoadKeyRebindHexUnicodeStringParsedTest(TestRunMode runMode = TestRunMode::Automated) {
    RunConfigLoadCase("config_load_key_rebind_hex_unicode_string_parsed",
                      []() {
                          WriteRawConfigTomlToDisk(R"(configVersion = 4
defaultMode = "Fullscreen"

[keyRebinds]
enabled = true
resolveRebindTargetsForHotkeys = false
toggleHotkey = []

[[keyRebinds.rebinds]]
fromKey = 74
toKey = 75
enabled = true
useCustomOutput = true
customOutputUnicode = " 0x00f8 "
shiftLayerEnabled = true
shiftLayerOutputUnicode = " 0X00D8 "
)");
                      },
                      []() {
                          ExpectConfigLoadSucceeded("config-load-key-rebind-hex-unicode-string-parsed");
                          Expect(g_config.keyRebinds.rebinds.size() == 1, "Expected exactly one key rebind fixture to load.");
                          const KeyRebind& rebind = g_config.keyRebinds.rebinds.front();
                          Expect(rebind.customOutputUnicode == 0x00F8,
                                 "Expected hexadecimal customOutputUnicode strings to parse into Unicode code points.");
                          Expect(rebind.shiftLayerOutputUnicode == 0x00D8,
                                 "Expected hexadecimal shiftLayerOutputUnicode strings to parse into Unicode code points.");
                      },
                      runMode);
}

void RunConfigLoadKeyRebindInvalidUnicodeDefaultedTest(TestRunMode runMode = TestRunMode::Automated) {
    RunConfigLoadCase("config_load_key_rebind_invalid_unicode_defaulted",
                      []() {
                          WriteRawConfigTomlToDisk(R"(configVersion = 4
defaultMode = "Fullscreen"

[keyRebinds]
enabled = true
resolveRebindTargetsForHotkeys = false
toggleHotkey = []

[[keyRebinds.rebinds]]
fromKey = 74
toKey = 75
enabled = true
useCustomOutput = true
customOutputUnicode = "bogus"
shiftLayerEnabled = true
shiftLayerOutputUnicode = "U+D800"
)");
                      },
                      []() {
                          ExpectConfigLoadSucceeded("config-load-key-rebind-invalid-unicode-defaulted");
                          Expect(g_config.keyRebinds.rebinds.size() == 1, "Expected exactly one key rebind fixture to load.");
                          const KeyRebind& rebind = g_config.keyRebinds.rebinds.front();
                          Expect(rebind.customOutputUnicode == ConfigDefaults::KEY_REBIND_CUSTOM_OUTPUT_UNICODE,
                                 "Expected invalid customOutputUnicode strings to fall back to the configured default.");
                          Expect(rebind.shiftLayerOutputUnicode == ConfigDefaults::KEY_REBIND_SHIFT_LAYER_OUTPUT_UNICODE,
                                 "Expected invalid shiftLayerOutputUnicode strings to fall back to the configured default.");
                      },
                      runMode);
}

void RunConfigLoadKeyRebindLegacyShowIndicatorMigratedTest(TestRunMode runMode = TestRunMode::Automated) {
    RunConfigLoadCase("config_load_key_rebind_legacy_show_indicator_migrated",
                      []() {
                          WriteRawConfigTomlToDisk(R"(configVersion = 4
defaultMode = "Fullscreen"

[keyRebinds]
showIndicator = true
)");
                      },
                      []() {
                          ExpectConfigLoadSucceeded("config-load-key-rebind-legacy-show-indicator-migrated");
                          Expect(g_config.keyRebinds.indicatorMode == 1,
                                 "Expected legacy showIndicator=true to migrate to the active-only indicator mode.");
                      },
                      runMode);
}

void RunConfigLoadKeyRebindCannotTypeClearsTypedOutputTest(TestRunMode runMode = TestRunMode::Automated) {
    RunConfigLoadCase("config_load_key_rebind_cannot_type_clears_typed_output",
                      []() {
                          WriteRawConfigTomlToDisk(R"(configVersion = 4
defaultMode = "Fullscreen"

[keyRebinds]
enabled = true
resolveRebindTargetsForHotkeys = false
toggleHotkey = []

[[keyRebinds.rebinds]]
fromKey = 74
toKey = 36
enabled = true
useCustomOutput = true
baseOutputDisabled = true
customOutputVK = 75
customOutputUnicode = "U+00F8"
customOutputScanCode = 30
baseOutputShifted = true
shiftLayerEnabled = true
shiftLayerUsesCapsLock = true
shiftLayerOutputDisabled = true
shiftLayerOutputVK = 76
shiftLayerOutputUnicode = "U+00D8"
shiftLayerOutputShifted = true
)");
                      },
                      []() {
                          ExpectConfigLoadSucceeded("config-load-key-rebind-cannot-type-clears-typed-output");
                          Expect(g_config.keyRebinds.rebinds.size() == 1, "Expected exactly one key rebind fixture to load.");

                          const KeyRebind& rebind = g_config.keyRebinds.rebinds.front();
                          Expect(rebind.useCustomOutput,
                                 "Expected cannot-type sanitization to preserve trigger-side scan overrides.");
                          Expect(rebind.customOutputScanCode == 30,
                                 "Expected cannot-type sanitization to keep the trigger-side scan override.");
                          Expect(!rebind.baseOutputDisabled,
                                 "Expected cannot-type sanitization to clear the base typed-output disabled flag.");
                          Expect(rebind.customOutputVK == 0,
                                 "Expected cannot-type sanitization to clear invalid base typed-output VK overrides.");
                          Expect(rebind.customOutputUnicode == 0,
                                 "Expected cannot-type sanitization to clear invalid base typed-output Unicode overrides.");
                          Expect(!rebind.baseOutputShifted,
                                 "Expected cannot-type sanitization to clear invalid base shifted text state.");
                          Expect(!rebind.shiftLayerEnabled,
                                 "Expected cannot-type sanitization to disable invalid shift-layer typed output.");
                          Expect(!rebind.shiftLayerUsesCapsLock,
                                 "Expected cannot-type sanitization to clear invalid shift-layer Caps Lock state.");
                          Expect(!rebind.shiftLayerOutputDisabled,
                                 "Expected cannot-type sanitization to clear invalid shift-layer disabled state.");
                          Expect(rebind.shiftLayerOutputVK == 0,
                                 "Expected cannot-type sanitization to clear invalid shift-layer VK overrides.");
                          Expect(rebind.shiftLayerOutputUnicode == 0,
                                 "Expected cannot-type sanitization to clear invalid shift-layer Unicode overrides.");
                          Expect(!rebind.shiftLayerOutputShifted,
                                 "Expected cannot-type sanitization to clear invalid shift-layer shifted text state.");

                          SaveAndReloadCurrentConfig();
                          ExpectConfigLoadSucceeded("config-load-key-rebind-cannot-type-clears-typed-output reload");
                          Expect(g_config.keyRebinds.rebinds.size() == 1,
                                 "Expected the sanitized key rebind to survive the save-and-reload roundtrip.");

                          const KeyRebind& reloadedRebind = g_config.keyRebinds.rebinds.front();
                          Expect(reloadedRebind.useCustomOutput,
                                 "Expected save sanitization to preserve trigger-side scan overrides after reload.");
                          Expect(reloadedRebind.customOutputScanCode == 30,
                                 "Expected save sanitization to preserve the trigger-side scan override after reload.");
                          Expect(reloadedRebind.customOutputVK == 0 && reloadedRebind.customOutputUnicode == 0,
                                 "Expected save sanitization to keep typed-output overrides cleared after reload.");
                          Expect(!reloadedRebind.baseOutputDisabled && !reloadedRebind.baseOutputShifted,
                                 "Expected save sanitization to keep base typed-output flags cleared after reload.");
                          Expect(!reloadedRebind.shiftLayerEnabled && !reloadedRebind.shiftLayerUsesCapsLock &&
                                     !reloadedRebind.shiftLayerOutputDisabled && !reloadedRebind.shiftLayerOutputShifted &&
                                     reloadedRebind.shiftLayerOutputVK == 0 && reloadedRebind.shiftLayerOutputUnicode == 0,
                                 "Expected save sanitization to keep shift-layer typed-output state cleared after reload.");

                          const std::string savedToml = ReadTextFileUtf8(GetCurrentConfigPath());
                          Expect(savedToml.find("customOutputVK = 0") != std::string::npos,
                                 "Expected the saved config TOML to persist a cleared base typed-output VK.");
                          Expect(savedToml.find("customOutputUnicode = 0") != std::string::npos,
                                 "Expected the saved config TOML to persist a cleared base typed-output Unicode value.");
                          Expect(savedToml.find("baseOutputDisabled = false") != std::string::npos,
                                 "Expected the saved config TOML to persist a cleared base typed-output disabled flag.");
                          Expect(savedToml.find("shiftLayerEnabled = false") != std::string::npos,
                                 "Expected the saved config TOML to persist a cleared shift-layer typed-output flag.");
                          Expect(savedToml.find("shiftLayerOutputUnicode = 0") != std::string::npos,
                                 "Expected the saved config TOML to persist a cleared shift-layer Unicode value.");
                      },
                      runMode);
}

void RunConfigPublishKeyRebindCannotTypeClearsTypedOutputTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    const std::filesystem::path root = PrepareCaseDirectory("config_publish_key_rebind_cannot_type_clears_typed_output");
    ResetGlobalTestState(root);

    Config config;
    config.keyRebinds.enabled = true;
    config.keyRebinds.resolveRebindTargetsForHotkeys = false;

    KeyRebind rebind;
    rebind.fromKey = 'J';
    rebind.toKey = VK_HOME;
    rebind.enabled = true;
    rebind.useCustomOutput = true;
    rebind.baseOutputDisabled = true;
    rebind.customOutputVK = 'K';
    rebind.customOutputUnicode = 0x00F8;
    rebind.customOutputScanCode = 30;
    rebind.baseOutputShifted = true;
    rebind.shiftLayerEnabled = true;
    rebind.shiftLayerUsesCapsLock = true;
    rebind.shiftLayerOutputDisabled = true;
    rebind.shiftLayerOutputVK = 'L';
    rebind.shiftLayerOutputUnicode = 0x00D8;
    rebind.shiftLayerOutputShifted = true;
    config.keyRebinds.rebinds.push_back(rebind);

    PublishConfigSnapshot(config);

    auto publishedConfig = GetConfigSnapshot();
    Expect(publishedConfig != nullptr,
           "Expected config publish sanitization test to publish a config snapshot.");
    Expect(publishedConfig->keyRebinds.rebinds.size() == 1,
           "Expected config publish sanitization test to publish exactly one key rebind.");

    const KeyRebind& publishedRebind = publishedConfig->keyRebinds.rebinds.front();
    Expect(publishedRebind.useCustomOutput,
           "Expected config publish sanitization to preserve trigger-side scan overrides.");
    Expect(publishedRebind.customOutputScanCode == 30,
           "Expected config publish sanitization to keep the trigger-side scan override.");
    Expect(!publishedRebind.baseOutputDisabled && !publishedRebind.baseOutputShifted,
           "Expected config publish sanitization to clear base typed-output flags.");
    Expect(publishedRebind.customOutputVK == 0 && publishedRebind.customOutputUnicode == 0,
           "Expected config publish sanitization to clear base typed-output overrides.");
    Expect(!publishedRebind.shiftLayerEnabled && !publishedRebind.shiftLayerUsesCapsLock &&
               !publishedRebind.shiftLayerOutputDisabled && !publishedRebind.shiftLayerOutputShifted &&
               publishedRebind.shiftLayerOutputVK == 0 && publishedRebind.shiftLayerOutputUnicode == 0,
           "Expected config publish sanitization to clear shift-layer typed-output state.");

    if (runMode == TestRunMode::Visual) {
        RunVisualLoop(window, "config-publish-key-rebind-cannot-type-clears-typed-output", &RenderInteractiveSettingsFrame);
    }
}

void RunConfigLoadFullscreenStretchRepairedTest(TestRunMode runMode = TestRunMode::Automated) {
    RunConfigLoadCase("config_load_fullscreen_stretch_repaired",
                      []() {
                          Config config;
                          config.defaultMode = "Fullscreen";

                          ModeConfig fullscreenMode;
                          fullscreenMode.id = "Fullscreen";
                          fullscreenMode.width = 0;
                          fullscreenMode.height = 0;
                          fullscreenMode.manualWidth = 0;
                          fullscreenMode.manualHeight = 0;
                          fullscreenMode.stretch.enabled = false;
                          fullscreenMode.stretch.x = 33;
                          fullscreenMode.stretch.y = 44;
                          fullscreenMode.stretch.width = 55;
                          fullscreenMode.stretch.height = 66;
                          config.modes.push_back(fullscreenMode);

                          WriteConfigFixtureToDisk(config);
                      },
                      []() {
                          ExpectConfigLoadSucceeded("config-load-fullscreen-stretch-repaired");
                          const ModeConfig& fullscreenMode = FindModeOrThrow("Fullscreen");
                          Expect(fullscreenMode.width > 0 && fullscreenMode.height > 0,
                                 "Expected Fullscreen mode dimensions to repair to valid values.");
                          Expect(fullscreenMode.manualWidth == fullscreenMode.width && fullscreenMode.manualHeight == fullscreenMode.height,
                                 "Expected Fullscreen manual dimensions to repair alongside the live dimensions.");
                          Expect(fullscreenMode.stretch.enabled, "Expected Fullscreen stretch to be forced on during load.");
                          Expect(fullscreenMode.stretch.x == 0 && fullscreenMode.stretch.y == 0,
                                 "Expected Fullscreen stretch origin to reset to the top-left corner.");
                          Expect(fullscreenMode.stretch.width == fullscreenMode.width &&
                                     fullscreenMode.stretch.height == fullscreenMode.height,
                                 "Expected Fullscreen stretch bounds to match the repaired mode dimensions.");
                      },
                      runMode);
}

        void RunConfigLoadFullscreenManualDimensionsPreservedTest(TestRunMode runMode = TestRunMode::Automated) {
            RunConfigLoadCase("config_load_fullscreen_manual_dimensions_preserved",
                     []() {
                         Config config;
                         config.defaultMode = "Fullscreen";

                         ModeConfig fullscreenMode;
                         fullscreenMode.id = "Fullscreen";
                         fullscreenMode.useRelativeSize = false;
                         fullscreenMode.relativeWidth = -1.0f;
                         fullscreenMode.relativeHeight = -1.0f;
                         fullscreenMode.width = 1280;
                         fullscreenMode.height = 720;
                         fullscreenMode.manualWidth = 1280;
                         fullscreenMode.manualHeight = 720;
                         fullscreenMode.stretch.enabled = false;
                         fullscreenMode.stretch.x = 33;
                         fullscreenMode.stretch.y = 44;
                         fullscreenMode.stretch.width = 55;
                         fullscreenMode.stretch.height = 66;
                         config.modes.push_back(fullscreenMode);

                         WriteConfigFixtureToDisk(config);
                     },
                     []() {
                         ExpectConfigLoadSucceeded("config-load-fullscreen-manual-dimensions-preserved");
                         const ModeConfig& fullscreenMode = FindModeOrThrow("Fullscreen");

                         Expect(!fullscreenMode.useRelativeSize,
                             "Expected Fullscreen manual sizing to stay in manual-pixel mode after recalculation.");
                         Expect(fullscreenMode.relativeWidth < 0.0f && fullscreenMode.relativeHeight < 0.0f,
                             "Expected Fullscreen manual sizing to keep relative dimensions disabled.");
                         Expect(fullscreenMode.width == 1280 && fullscreenMode.height == 720,
                             "Expected Fullscreen manual render dimensions to preserve the configured internal size.");
                         Expect(fullscreenMode.manualWidth == 1280 && fullscreenMode.manualHeight == 720,
                             "Expected Fullscreen manual dimensions to preserve their persisted values.");
                         Expect(fullscreenMode.stretch.enabled, "Expected Fullscreen stretch to remain forced on.");
                         Expect(fullscreenMode.stretch.x == 0 && fullscreenMode.stretch.y == 0,
                             "Expected Fullscreen stretch origin to stay pinned to the top-left corner.");
                     },
                     runMode);
        }

        void RunConfigLoadFullscreenRelativeDimensionsPreservedTest(TestRunMode runMode = TestRunMode::Automated) {
            constexpr float kRelativeWidth = 0.625f;
            constexpr float kRelativeHeight = 0.4f;
            int expectedWidth = 1;
            int expectedHeight = 1;

            RunConfigLoadCase("config_load_fullscreen_relative_dimensions_preserved",
                     [&]() {
                         Config config;
                         config.defaultMode = "Fullscreen";

                         ModeConfig fullscreenMode;
                         fullscreenMode.id = "Fullscreen";
                         fullscreenMode.useRelativeSize = true;
                         fullscreenMode.relativeWidth = kRelativeWidth;
                         fullscreenMode.relativeHeight = kRelativeHeight;
                         fullscreenMode.width = 999;
                         fullscreenMode.height = 555;
                         fullscreenMode.manualWidth = 1234;
                         fullscreenMode.manualHeight = 678;
                         fullscreenMode.stretch.enabled = false;
                         fullscreenMode.stretch.x = 33;
                         fullscreenMode.stretch.y = 44;
                         fullscreenMode.stretch.width = 55;
                         fullscreenMode.stretch.height = 66;
                         config.modes.push_back(fullscreenMode);

                         const int liveScreenWidth = (std::max)(1, GetCachedWindowWidth());
                         const int liveScreenHeight = (std::max)(1, GetCachedWindowHeight());
                         expectedWidth = (std::max)(1, static_cast<int>(std::lround(kRelativeWidth * static_cast<float>(liveScreenWidth))));
                         expectedHeight = (std::max)(1, static_cast<int>(std::lround(kRelativeHeight * static_cast<float>(liveScreenHeight))));

                         WriteConfigFixtureToDisk(config);
                     },
                     [&]() {
                         ExpectConfigLoadSucceeded("config-load-fullscreen-relative-dimensions-preserved");
                         const ModeConfig& fullscreenMode = FindModeOrThrow("Fullscreen");

                         Expect(fullscreenMode.useRelativeSize,
                             "Expected Fullscreen percentage sizing to stay marked as relative after recalculation.");
                         ExpectFloatNear(fullscreenMode.relativeWidth, kRelativeWidth,
                                "Expected Fullscreen relativeWidth to preserve the configured percentage.");
                         ExpectFloatNear(fullscreenMode.relativeHeight, kRelativeHeight,
                                "Expected Fullscreen relativeHeight to preserve the configured percentage.");
                         Expect(fullscreenMode.width == expectedWidth,
                             "Expected Fullscreen relative width to be recomputed from the live client width.");
                         Expect(fullscreenMode.height == expectedHeight,
                             "Expected Fullscreen relative height to be recomputed from the live client height.");
                         Expect(fullscreenMode.manualWidth == 1234 && fullscreenMode.manualHeight == 678,
                             "Expected Fullscreen relative sizing to preserve the persisted manual fallback dimensions.");
                         Expect(fullscreenMode.stretch.enabled, "Expected Fullscreen stretch to remain forced on.");
                         Expect(fullscreenMode.stretch.x == 0 && fullscreenMode.stretch.y == 0,
                             "Expected Fullscreen stretch origin to stay pinned to the top-left corner.");
                     },
                     runMode);
        }

        struct CapturedConfigOriginalWindowMessage {
            UINT message = 0;
            WPARAM wParam = 0;
            LPARAM lParam = 0;
        };

        static std::vector<CapturedConfigOriginalWindowMessage> g_capturedConfigOriginalWindowMessages;

        static LRESULT CALLBACK CaptureConfigOriginalWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
            (void)hwnd;
            g_capturedConfigOriginalWindowMessages.push_back({ message, wParam, lParam });
            return 0;
        }

        void RunFullscreenRelativeExternalResizeSkipsStaleResendTest(TestRunMode runMode = TestRunMode::Automated) {
            DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
            const std::filesystem::path root = PrepareCaseDirectory("fullscreen_relative_external_resize_skips_stale_resend");
            ResetGlobalTestState(root);

            RECT initialClientRect{};
            Expect(GetClientRect(window.hwnd(), &initialClientRect) != FALSE,
                "Expected to read the initial client rect for the Fullscreen external-resize regression test.");
            const int initialClientW = initialClientRect.right - initialClientRect.left;
            const int initialClientH = initialClientRect.bottom - initialClientRect.top;
            Expect(initialClientW > 0 && initialClientH > 0,
                "Expected the Fullscreen external-resize regression test window to have a valid initial client size.");
            UpdateCachedWindowMetricsFromSize(initialClientW, initialClientH);

            g_config = Config();
            g_config.defaultMode = "Fullscreen";
            g_config.restoreWindowedModeOnFullscreenExit = false;

            ModeConfig fullscreenMode;
            fullscreenMode.id = "Fullscreen";
            fullscreenMode.useRelativeSize = true;
            fullscreenMode.relativeWidth = 0.8f;
            fullscreenMode.relativeHeight = 0.7f;
            fullscreenMode.width = (std::max)(1, static_cast<int>(std::lround(fullscreenMode.relativeWidth * static_cast<float>(initialClientW))));
            fullscreenMode.height = (std::max)(1, static_cast<int>(std::lround(fullscreenMode.relativeHeight * static_cast<float>(initialClientH))));
            fullscreenMode.manualWidth = fullscreenMode.width;
            fullscreenMode.manualHeight = fullscreenMode.height;
            fullscreenMode.stretch.enabled = true;
            fullscreenMode.stretch.x = 0;
            fullscreenMode.stretch.y = 0;
            fullscreenMode.stretch.width = initialClientW;
            fullscreenMode.stretch.height = initialClientH;
            g_config.modes.push_back(fullscreenMode);
            PublishConfigSnapshot();
            g_configLoaded.store(true, std::memory_order_release);

            {
             std::lock_guard<std::mutex> lock(g_modeIdMutex);
             g_currentModeId = "Fullscreen";
             const int nextIndex = 1 - g_currentModeIdIndex.load(std::memory_order_relaxed);
             g_modeIdBuffers[nextIndex] = "Fullscreen";
             g_currentModeIdIndex.store(nextIndex, std::memory_order_release);
            }

            constexpr int kSentinelRequestedW = 321;
            constexpr int kSentinelRequestedH = 654;
            Expect(RequestWindowClientResize(window.hwnd(), kSentinelRequestedW, kSentinelRequestedH, "test:prime_resize_state"),
                "Expected to prime requested resize state for the Fullscreen external-resize regression test.");
            Expect(window.PumpMessages(), "Expected the Fullscreen external-resize regression test window to stay open after priming.");

            const int staleCachedW = (std::max)(1, initialClientW - 37);
            const int staleCachedH = (std::max)(1, initialClientH - 23);
            Expect(staleCachedW != initialClientW || staleCachedH != initialClientH,
                "Expected the Fullscreen external-resize regression test to stage a stale cached client size.");
            UpdateCachedWindowMetricsFromSize(staleCachedW, staleCachedH);

            WINDOWPOS pos{};
            pos.hwnd = window.hwnd();
            pos.cx = initialClientW;
            pos.cy = initialClientH;
            pos.flags = SWP_NOACTIVATE | SWP_NOMOVE;

            const HWND previousSubclassedHwnd = g_subclassedHwnd.load(std::memory_order_acquire);
            const WNDPROC previousOriginalWndProc = g_originalWndProc;
            g_subclassedHwnd.store(window.hwnd(), std::memory_order_release);
            g_originalWndProc = &DefWindowProcW;

            try {
             (void)SubclassedWndProc(window.hwnd(), WM_WINDOWPOSCHANGED, 0, reinterpret_cast<LPARAM>(&pos));
            } catch (...) {
             g_originalWndProc = previousOriginalWndProc;
             g_subclassedHwnd.store(previousSubclassedHwnd, std::memory_order_release);
             throw;
            }

            g_originalWndProc = previousOriginalWndProc;
            g_subclassedHwnd.store(previousSubclassedHwnd, std::memory_order_release);

            int requestedW = 0;
            int requestedH = 0;
            int previousRequestedW = 0;
            int previousRequestedH = 0;
            Expect(GetRecentRequestedWindowClientResizes(requestedW, requestedH, previousRequestedW, previousRequestedH),
                "Expected requested resize history to be available for the Fullscreen external-resize regression test.");
            Expect(requestedW == kSentinelRequestedW && requestedH == kSentinelRequestedH,
                "Expected external resize handling to avoid reposting stale Fullscreen relative WM_SIZE before logic-thread recalculation.");

            if (runMode == TestRunMode::Visual) {
             RunVisualLoop(window, "fullscreen-relative-external-resize-skips-stale-resend", &RenderInteractiveSettingsFrame);
            }
        }

        void RunFullscreenRelativeOsWmSizeOverridesComputedDimensionsTest(TestRunMode runMode = TestRunMode::Automated) {
            (void)runMode;

            DummyWindow window(kWindowWidth, kWindowHeight, false);
            const std::filesystem::path root = PrepareCaseDirectory("fullscreen_relative_os_wmsize_overrides_computed_dimensions");
            ResetGlobalTestState(root);

            RECT clientRect{};
            Expect(GetClientRect(window.hwnd(), &clientRect) != FALSE,
                "Expected the Fullscreen WM_SIZE override regression test window to expose a client rect.");
            const int clientW = clientRect.right - clientRect.left;
            const int clientH = clientRect.bottom - clientRect.top;
            Expect(clientW > 0 && clientH > 0,
                "Expected the Fullscreen WM_SIZE override regression test window to expose a positive client size.");
            UpdateCachedWindowMetricsFromSize(clientW, clientH);

            constexpr float kRelativeWidth = 0.8f;
            constexpr float kRelativeHeight = 0.7f;
            constexpr int kSentinelRequestedW = 321;
            constexpr int kSentinelRequestedH = 654;

            g_config = Config();
            g_config.defaultMode = "Fullscreen";
            g_config.restoreWindowedModeOnFullscreenExit = false;

            ModeConfig fullscreenMode;
            fullscreenMode.id = "Fullscreen";
            fullscreenMode.useRelativeSize = true;
            fullscreenMode.relativeWidth = kRelativeWidth;
            fullscreenMode.relativeHeight = kRelativeHeight;
            fullscreenMode.width = 111;
            fullscreenMode.height = 222;
            fullscreenMode.manualWidth = 111;
            fullscreenMode.manualHeight = 222;
            fullscreenMode.stretch.enabled = true;
            fullscreenMode.stretch.x = 0;
            fullscreenMode.stretch.y = 0;
            fullscreenMode.stretch.width = clientW;
            fullscreenMode.stretch.height = clientH;
            g_config.modes.push_back(fullscreenMode);
            PublishConfigSnapshot();
            g_configLoaded.store(true, std::memory_order_release);

            {
             std::lock_guard<std::mutex> lock(g_modeIdMutex);
             g_currentModeId = "Fullscreen";
             const int nextIndex = 1 - g_currentModeIdIndex.load(std::memory_order_relaxed);
             g_modeIdBuffers[nextIndex] = "Fullscreen";
             g_currentModeIdIndex.store(nextIndex, std::memory_order_release);
            }

            const int expectedWidth = (std::max)(1, static_cast<int>(std::lround(kRelativeWidth * static_cast<float>(clientW))));
            const int expectedHeight = (std::max)(1, static_cast<int>(std::lround(kRelativeHeight * static_cast<float>(clientH))));
            Expect(expectedWidth != clientW || expectedHeight != clientH,
                "Expected the Fullscreen WM_SIZE override regression test to use an internal render size different from the live client size.");

            RememberRequestedWindowClientResize(kSentinelRequestedW, kSentinelRequestedH);

            const HWND previousSubclassedHwnd = g_subclassedHwnd.load(std::memory_order_acquire);
            const WNDPROC previousOriginalWndProc = g_originalWndProc;
            g_subclassedHwnd.store(window.hwnd(), std::memory_order_release);
            g_originalWndProc = &CaptureConfigOriginalWndProc;
            g_capturedConfigOriginalWindowMessages.clear();

            try {
             (void)SubclassedWndProc(window.hwnd(), WM_SIZE, SIZE_RESTORED, MAKELPARAM(clientW, clientH));

             Expect(g_capturedConfigOriginalWindowMessages.size() == 1,
                 "Expected the Fullscreen WM_SIZE override regression test to forward exactly one WM_SIZE to the original window proc.");
             Expect(g_capturedConfigOriginalWindowMessages.front().message == WM_SIZE,
                 "Expected the Fullscreen WM_SIZE override regression test to forward a WM_SIZE message.");
             Expect(LOWORD(g_capturedConfigOriginalWindowMessages.front().lParam) == expectedWidth &&
                        HIWORD(g_capturedConfigOriginalWindowMessages.front().lParam) == expectedHeight,
                 "Expected OS-originated Fullscreen WM_SIZE to be overridden with the computed internal render size.");

             int requestedW = 0;
             int requestedH = 0;
             int previousRequestedW = 0;
             int previousRequestedH = 0;
             Expect(GetRecentRequestedWindowClientResizes(requestedW, requestedH, previousRequestedW, previousRequestedH),
                 "Expected the Fullscreen WM_SIZE override regression test to populate requested resize history.");
             Expect(requestedW == expectedWidth && requestedH == expectedHeight,
                 "Expected OS-originated Fullscreen WM_SIZE overrides to update the requested resize history to the computed size.");
             Expect(previousRequestedW == kSentinelRequestedW && previousRequestedH == kSentinelRequestedH,
                 "Expected OS-originated Fullscreen WM_SIZE overrides to replace the previous requested resize history entry.");

             g_capturedConfigOriginalWindowMessages.clear();
             (void)SubclassedWndProc(window.hwnd(), WM_SIZE, SIZE_RESTORED, MAKELPARAM(expectedWidth, expectedHeight));

             Expect(g_capturedConfigOriginalWindowMessages.size() == 1,
                 "Expected the Fullscreen WM_SIZE override regression test to forward the synthetic WM_SIZE once.");
             Expect(LOWORD(g_capturedConfigOriginalWindowMessages.front().lParam) == expectedWidth &&
                        HIWORD(g_capturedConfigOriginalWindowMessages.front().lParam) == expectedHeight,
                 "Expected Toolscreen-posted Fullscreen WM_SIZE to pass through without being recomputed again.");
            } catch (...) {
             g_originalWndProc = previousOriginalWndProc;
             g_subclassedHwnd.store(previousSubclassedHwnd, std::memory_order_release);
             g_capturedConfigOriginalWindowMessages.clear();
             throw;
            }

            g_originalWndProc = previousOriginalWndProc;
            g_subclassedHwnd.store(previousSubclassedHwnd, std::memory_order_release);
            g_capturedConfigOriginalWindowMessages.clear();
        }

        void RunFullscreenRelativeDisplayDimensionsFollowWindowResizeTest(TestRunMode runMode = TestRunMode::Automated) {
            (void)runMode;

            ModeConfig fullscreenMode;
            fullscreenMode.id = "Fullscreen";
            fullscreenMode.useRelativeSize = true;
            fullscreenMode.relativeWidth = 0.75f;
            fullscreenMode.relativeHeight = 0.6f;
            fullscreenMode.width = 1200;
            fullscreenMode.height = 700;

            const int initialScreenW = 1600;
            const int initialScreenH = 900;
            const int resizedScreenW = 1937;
            const int resizedScreenH = 1113;

            const int initialDisplayedWidth = ResolveModeDisplayWidth(fullscreenMode, initialScreenW, initialScreenH);
            const int initialDisplayedHeight = ResolveModeDisplayHeight(fullscreenMode, initialScreenW, initialScreenH);
            const int resizedDisplayedWidth = ResolveModeDisplayWidth(fullscreenMode, resizedScreenW, resizedScreenH);
            const int resizedDisplayedHeight = ResolveModeDisplayHeight(fullscreenMode, resizedScreenW, resizedScreenH);

            Expect(initialDisplayedWidth == static_cast<int>(std::lround(fullscreenMode.relativeWidth * static_cast<float>(initialScreenW))),
                "Expected Fullscreen relative display width to resolve from the initial client width.");
            Expect(initialDisplayedHeight == static_cast<int>(std::lround(fullscreenMode.relativeHeight * static_cast<float>(initialScreenH))),
                "Expected Fullscreen relative display height to resolve from the initial client height.");
            Expect(resizedDisplayedWidth == static_cast<int>(std::lround(fullscreenMode.relativeWidth * static_cast<float>(resizedScreenW))),
                "Expected Fullscreen relative display width to follow the resized client width immediately.");
            Expect(resizedDisplayedHeight == static_cast<int>(std::lround(fullscreenMode.relativeHeight * static_cast<float>(resizedScreenH))),
                "Expected Fullscreen relative display height to follow the resized client height immediately.");
            Expect(fullscreenMode.width == 1200 && fullscreenMode.height == 700,
                "Expected display-only relative dimension resolution to avoid mutating the stored Fullscreen GUI dimensions.");
        }

        void RunFullscreenRelativeGuiPublishPreservesRecalculatedSizeTest(TestRunMode runMode = TestRunMode::Automated) {
            DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
            const std::filesystem::path root = PrepareCaseDirectory("fullscreen_relative_gui_publish_preserves_recalculated_size");
            ResetGlobalTestState(root);

            RECT initialClientRect{};
            Expect(GetClientRect(window.hwnd(), &initialClientRect) != FALSE,
                "Expected to read the initial client rect for the Fullscreen GUI publish regression test.");
            const int initialClientW = initialClientRect.right - initialClientRect.left;
            const int initialClientH = initialClientRect.bottom - initialClientRect.top;
            Expect(initialClientW > 0 && initialClientH > 0,
                "Expected the Fullscreen GUI publish regression test window to have a valid initial client size.");
            UpdateCachedWindowMetricsFromSize(initialClientW, initialClientH);

            constexpr float kRelativeWidth = 0.8f;
            constexpr float kRelativeHeight = 0.7f;

            g_config = Config();
            g_sharedConfig = Config();
            g_config.defaultMode = "Fullscreen";

            ModeConfig fullscreenMode;
            fullscreenMode.id = "Fullscreen";
            fullscreenMode.useRelativeSize = true;
            fullscreenMode.relativeWidth = kRelativeWidth;
            fullscreenMode.relativeHeight = kRelativeHeight;
            fullscreenMode.width = (std::max)(1, static_cast<int>(std::lround(kRelativeWidth * static_cast<float>(initialClientW))));
            fullscreenMode.height = (std::max)(1, static_cast<int>(std::lround(kRelativeHeight * static_cast<float>(initialClientH))));
            fullscreenMode.manualWidth = fullscreenMode.width;
            fullscreenMode.manualHeight = fullscreenMode.height;
            fullscreenMode.stretch.enabled = true;
            fullscreenMode.stretch.x = 0;
            fullscreenMode.stretch.y = 0;
            fullscreenMode.stretch.width = initialClientW;
            fullscreenMode.stretch.height = initialClientH;
            g_config.modes.push_back(fullscreenMode);

            PublishGuiConfigSnapshot();

            const int resizedClientW = initialClientW + 137;
            const int resizedClientH = initialClientH + 91;
            UpdateCachedWindowMetricsFromSize(resizedClientW, resizedClientH);

            auto publishedBeforeGuiEdit = GetConfigSnapshot();
            Expect(publishedBeforeGuiEdit != nullptr,
                "Expected a published config snapshot before staging the Fullscreen GUI publish regression.");

            Config runtimeResolvedConfig = *publishedBeforeGuiEdit;
            RecalculateModeDimensions(runtimeResolvedConfig, resizedClientW, resizedClientH);
            PublishConfigSnapshot(runtimeResolvedConfig);

            const int staleWidth = g_config.modes.front().width;
            const int staleHeight = g_config.modes.front().height;
            const int expectedWidth = (std::max)(1, static_cast<int>(std::lround(kRelativeWidth * static_cast<float>(resizedClientW))));
            const int expectedHeight = (std::max)(1, static_cast<int>(std::lround(kRelativeHeight * static_cast<float>(resizedClientH))));
            Expect(staleWidth != expectedWidth || staleHeight != expectedHeight,
                "Expected the editable GUI config to keep stale Fullscreen dimensions before the unrelated GUI publish.");

            WindowOverlayConfig overlay;
            overlay.name = "GUI Publish Overlay";
            overlay.relativeTo = "centerViewport";
            g_config.windowOverlays.push_back(overlay);
            g_configIsDirty.store(true, std::memory_order_release);

            PublishGuiConfigSnapshot();

            auto publishedAfterGuiEdit = GetConfigSnapshot();
            Expect(publishedAfterGuiEdit != nullptr,
                "Expected a published config snapshot after the unrelated GUI publish.");

            const ModeConfig* publishedFullscreen = GetModeFromSnapshotOrFallback(*publishedAfterGuiEdit, "Fullscreen");
            Expect(publishedFullscreen != nullptr,
                "Expected the published config snapshot to still contain the Fullscreen mode after the unrelated GUI publish.");
            Expect(publishedFullscreen->useRelativeSize,
                "Expected the unrelated GUI publish to preserve Fullscreen relative sizing.");
            ExpectFloatNear(publishedFullscreen->relativeWidth, kRelativeWidth,
                "Expected the unrelated GUI publish to preserve the Fullscreen relative width percentage.");
            ExpectFloatNear(publishedFullscreen->relativeHeight, kRelativeHeight,
                "Expected the unrelated GUI publish to preserve the Fullscreen relative height percentage.");
            Expect(publishedFullscreen->width == expectedWidth,
                "Expected the unrelated GUI publish to keep the logic-thread-recalculated Fullscreen width.");
            Expect(publishedFullscreen->height == expectedHeight,
                "Expected the unrelated GUI publish to keep the logic-thread-recalculated Fullscreen height.");
            Expect(publishedFullscreen->stretch.enabled,
                "Expected the unrelated GUI publish to preserve Fullscreen stretch output.");
            Expect(publishedFullscreen->stretch.x == 0 && publishedFullscreen->stretch.y == 0,
                "Expected the unrelated GUI publish to keep the Fullscreen stretch origin pinned to the top-left corner.");
            Expect(publishedFullscreen->stretch.width == resizedClientW && publishedFullscreen->stretch.height == resizedClientH,
                "Expected the unrelated GUI publish to keep the Fullscreen stretch size aligned with the resized client area.");
            Expect(publishedAfterGuiEdit->windowOverlays.size() == 1 && publishedAfterGuiEdit->windowOverlays.front().name == overlay.name,
                "Expected the unrelated GUI publish to still apply the overlay edit while preserving recalculated Fullscreen dimensions.");

            if (runMode == TestRunMode::Visual) {
             RunVisualLoop(window, "fullscreen-relative-gui-publish-preserves-recalculated-size", &RenderInteractiveSettingsFrame);
            }
        }

void RunConfigLoadPreemptiveSyncExistingModeTest(TestRunMode runMode = TestRunMode::Automated) {
    RunConfigLoadCase("config_load_preemptive_sync_existing_mode",
                      []() {
                          Config config;
                          config.defaultMode = "EyeZoom";

                          ModeConfig eyezoomMode;
                          eyezoomMode.id = "EyeZoom";
                          eyezoomMode.width = 640;
                          eyezoomMode.height = 1200;
                          eyezoomMode.manualWidth = 640;
                          eyezoomMode.manualHeight = 1200;
                          config.modes.push_back(eyezoomMode);

                          ModeConfig preemptiveMode;
                          preemptiveMode.id = "Preemptive";
                          preemptiveMode.width = 123;
                          preemptiveMode.height = 456;
                          preemptiveMode.manualWidth = 123;
                          preemptiveMode.manualHeight = 456;
                          preemptiveMode.useRelativeSize = true;
                          preemptiveMode.relativeWidth = 0.5f;
                          preemptiveMode.relativeHeight = 0.25f;
                          config.modes.push_back(preemptiveMode);

                          WriteConfigFixtureToDisk(config);
                      },
                      []() {
                          ExpectConfigLoadSucceeded("config-load-preemptive-sync-existing-mode");
                          const ModeConfig& eyezoomMode = FindModeOrThrow("EyeZoom");
                          const ModeConfig& preemptiveMode = FindModeOrThrow("Preemptive");
                          Expect(preemptiveMode.width == eyezoomMode.width && preemptiveMode.height == eyezoomMode.height,
                                 "Expected Preemptive mode dimensions to sync to EyeZoom during load.");
                          Expect(preemptiveMode.manualWidth == eyezoomMode.manualWidth &&
                                     preemptiveMode.manualHeight == eyezoomMode.manualHeight,
                                 "Expected Preemptive manual dimensions to sync to EyeZoom during load.");
                          Expect(!preemptiveMode.useRelativeSize && preemptiveMode.relativeWidth < 0.0f &&
                                     preemptiveMode.relativeHeight < 0.0f,
                                 "Expected Preemptive relative sizing to clear when it is resynced from EyeZoom.");
                      },
                      runMode);
}

void RunConfigLoadThinMinWidthEnforcedTest(TestRunMode runMode = TestRunMode::Automated) {
    RunConfigLoadCase("config_load_thin_min_width_enforced",
                      []() {
                          Config config;
                          config.defaultMode = "Thin";

                          ModeConfig thinMode;
                          thinMode.id = "Thin";
                          thinMode.width = 100;
                          thinMode.height = 700;
                          thinMode.manualWidth = 100;
                          thinMode.manualHeight = 700;
                          config.modes.push_back(thinMode);

                          WriteConfigFixtureToDisk(config);
                      },
                      []() {
                          ExpectConfigLoadSucceeded("config-load-thin-min-width-enforced");
                          const ModeConfig& thinMode = FindModeOrThrow("Thin");
                          Expect(thinMode.width == 330, "Expected Thin mode width to clamp to the hard minimum during load.");
                      },
                      runMode);
}

void RunConfigLoadBrowserOverlayDefaultsTest(TestRunMode runMode = TestRunMode::Automated) {
    RunConfigLoadCase("config_load_browser_overlay_defaults",
                      []() {
                          WriteRawConfigTomlToDisk(R"(configVersion = 4
defaultMode = "Fullscreen"

[[browserOverlay]]
name = "Default Browser"
)");
                      },
                      []() {
                          ExpectConfigLoadSucceeded("config-load-browser-overlay-defaults");
                          const BrowserOverlayConfig& overlay = FindBrowserOverlayOrThrow("Default Browser");
                          Expect(overlay.url == "https://example.com", "Expected browser overlay URL to default when omitted.");
                          Expect(overlay.browserWidth == ConfigDefaults::BROWSER_OVERLAY_WIDTH,
                                 "Expected browser overlay width to default when omitted.");
                          Expect(overlay.browserHeight == ConfigDefaults::BROWSER_OVERLAY_HEIGHT,
                                 "Expected browser overlay height to default when omitted.");
                          Expect(overlay.transparentBackground == ConfigDefaults::BROWSER_OVERLAY_TRANSPARENT_BACKGROUND,
                                 "Expected browser overlay transparentBackground to default when omitted.");
                          Expect(overlay.reloadInterval == ConfigDefaults::BROWSER_OVERLAY_RELOAD_INTERVAL,
                                 "Expected browser overlay reloadInterval to default when omitted.");
                      },
                      runMode);
}

void RunConfigLoadWindowOverlayDefaultsTest(TestRunMode runMode = TestRunMode::Automated) {
    RunConfigLoadCase("config_load_window_overlay_defaults",
                      []() {
                          WriteRawConfigTomlToDisk(R"(configVersion = 4
defaultMode = "Fullscreen"

[[windowOverlay]]
name = "Default Window"
)");
                      },
                      []() {
                          ExpectConfigLoadSucceeded("config-load-window-overlay-defaults");
                          const WindowOverlayConfig& overlay = FindWindowOverlayOrThrow("Default Window");
                          Expect(overlay.windowMatchPriority == ConfigDefaults::WINDOW_OVERLAY_MATCH_PRIORITY,
                                 "Expected window overlay match priority to default when omitted.");
                          Expect(overlay.captureMethod == ConfigDefaults::WINDOW_OVERLAY_CAPTURE_METHOD,
                                 "Expected window overlay captureMethod to default when omitted.");
                          Expect(overlay.fps == ConfigDefaults::WINDOW_OVERLAY_FPS,
                                 "Expected window overlay FPS to default when omitted.");
                          Expect(overlay.searchInterval == ConfigDefaults::WINDOW_OVERLAY_SEARCH_INTERVAL,
                                 "Expected window overlay searchInterval to default when omitted.");
                          Expect(overlay.enableInteraction == ConfigDefaults::WINDOW_OVERLAY_ENABLE_INTERACTION,
                                 "Expected window overlay enableInteraction to default when omitted.");
                      },
                      runMode);
}

void RunConfigLoadImageDefaultsTest(TestRunMode runMode = TestRunMode::Automated) {
    RunConfigLoadCase("config_load_image_defaults",
                      []() {
                          WriteRawConfigTomlToDisk(R"(configVersion = 4
defaultMode = "Fullscreen"

[[image]]
name = "Default Image"
)");
                      },
                      []() {
                          ExpectConfigLoadSucceeded("config-load-image-defaults");
                          const ImageConfig& image = FindImageOrThrow("Default Image");
                          ExpectFloatNear(image.scale, ConfigDefaults::IMAGE_SCALE, "Expected image scale to default when omitted.");
                          Expect(image.relativeSizing == ConfigDefaults::IMAGE_RELATIVE_SIZING,
                                 "Expected image relativeSizing to default when omitted.");
                          Expect(image.relativeTo == ConfigDefaults::IMAGE_RELATIVE_TO,
                                 "Expected image relativeTo to default when omitted.");
                          ExpectFloatNear(image.opacity, ConfigDefaults::IMAGE_OPACITY,
                                          "Expected image opacity to default when omitted.");
                          Expect(image.onlyOnMyScreen == ConfigDefaults::IMAGE_ONLY_ON_MY_SCREEN,
                                 "Expected image onlyOnMyScreen to default when omitted.");
                      },
                      runMode);
}

void RunConfigLoadHotkeyDefaultFlagsTest(TestRunMode runMode = TestRunMode::Automated) {
    RunConfigLoadCase("config_load_hotkey_default_flags",
                      []() {
                          WriteRawConfigTomlToDisk(R"(configVersion = 4
defaultMode = "Fullscreen"

[[mode]]
id = "Target Mode"
width = 800
height = 600

[[hotkey]]
keys = [65]
mainMode = "Fullscreen"
secondaryMode = "Target Mode"
)");
                      },
                      []() {
                          ExpectConfigLoadSucceeded("config-load-hotkey-default-flags");
                          const HotkeyConfig& hotkey = FindHotkeyByKeysOrThrow({ 65 });
                          Expect(hotkey.debounce == ConfigDefaults::HOTKEY_DEBOUNCE,
                                 "Expected hotkey debounce to default when omitted.");
                          Expect(!hotkey.triggerOnRelease && !hotkey.triggerOnHold,
                                 "Expected hotkey trigger flags to default to false when omitted.");
                          Expect(!hotkey.blockKeyFromGame && !hotkey.allowExitToFullscreenRegardlessOfGameState,
                                 "Expected hotkey behavior flags to default to false when omitted.");
                          Expect(hotkey.conditions.gameState.empty() && hotkey.conditions.exclusions.empty(),
                                 "Expected omitted hotkey conditions to remain empty.");
                      },
                      runMode);
}

void RunConfigLoadSensitivityHotkeyDefaultFlagsTest(TestRunMode runMode = TestRunMode::Automated) {
    RunConfigLoadCase("config_load_sensitivity_hotkey_default_flags",
                      []() {
                          WriteRawConfigTomlToDisk(R"(configVersion = 4
defaultMode = "Fullscreen"

[[sensitivityHotkey]]
keys = [70]
)");
                      },
                      []() {
                          ExpectConfigLoadSucceeded("config-load-sensitivity-hotkey-default-flags");
                          const SensitivityHotkeyConfig& hotkey = FindSensitivityHotkeyOrThrow({ 70 });
                          ExpectFloatNear(hotkey.sensitivity, 1.0f, "Expected sensitivity hotkey sensitivity to default when omitted.");
                          Expect(!hotkey.separateXY, "Expected sensitivity hotkey separateXY to default to false when omitted.");
                          ExpectFloatNear(hotkey.sensitivityX, 1.0f,
                                          "Expected sensitivity hotkey sensitivityX to default when omitted.");
                          ExpectFloatNear(hotkey.sensitivityY, 1.0f,
                                          "Expected sensitivity hotkey sensitivityY to default when omitted.");
                          Expect(hotkey.debounce == ConfigDefaults::HOTKEY_DEBOUNCE,
                                 "Expected sensitivity hotkey debounce to default when omitted.");
                          Expect(!hotkey.toggle, "Expected sensitivity hotkey toggle to default to false when omitted.");
                      },
                      runMode);
}

void RunConfigLoadImageColorKeyDefaultSensitivityTest(TestRunMode runMode = TestRunMode::Automated) {
    RunConfigLoadCase("config_load_image_color_key_default_sensitivity",
                      []() {
                          WriteRawConfigTomlToDisk(R"(configVersion = 4
defaultMode = "Fullscreen"

[[image]]
name = "Color Key Image"
colorKeys = [{ color = [255, 0, 255] }]
)");
                      },
                      []() {
                          ExpectConfigLoadSucceeded("config-load-image-color-key-default-sensitivity");
                          const ImageConfig& image = FindImageOrThrow("Color Key Image");
                          Expect(image.colorKeys.size() == 1, "Expected image color key fixture to load.");
                          ExpectFloatNear(image.colorKeys.front().sensitivity, ConfigDefaults::COLOR_KEY_SENSITIVITY,
                                          "Expected image color key sensitivity to default when omitted.");
                      },
                      runMode);
}

void RunConfigLoadWindowOverlayColorKeyDefaultSensitivityTest(TestRunMode runMode = TestRunMode::Automated) {
    RunConfigLoadCase("config_load_window_overlay_color_key_default_sensitivity",
                      []() {
                          WriteRawConfigTomlToDisk(R"(configVersion = 4
defaultMode = "Fullscreen"

[[windowOverlay]]
name = "Color Key Window"
colorKeys = [{ color = [0, 255, 0] }]
)");
                      },
                      []() {
                          ExpectConfigLoadSucceeded("config-load-window-overlay-color-key-default-sensitivity");
                          const WindowOverlayConfig& overlay = FindWindowOverlayOrThrow("Color Key Window");
                          Expect(overlay.colorKeys.size() == 1, "Expected window overlay color key fixture to load.");
                          ExpectFloatNear(overlay.colorKeys.front().sensitivity, ConfigDefaults::COLOR_KEY_SENSITIVITY,
                                          "Expected window overlay color key sensitivity to default when omitted.");
                      },
                      runMode);
}

void RunConfigLoadBrowserOverlayColorKeyDefaultSensitivityTest(TestRunMode runMode = TestRunMode::Automated) {
    RunConfigLoadCase("config_load_browser_overlay_color_key_default_sensitivity",
                      []() {
                          WriteRawConfigTomlToDisk(R"(configVersion = 4
defaultMode = "Fullscreen"

[[browserOverlay]]
name = "Color Key Browser"
colorKeys = [{ color = [0, 0, 0] }]
)");
                      },
                      []() {
                          ExpectConfigLoadSucceeded("config-load-browser-overlay-color-key-default-sensitivity");
                          const BrowserOverlayConfig& overlay = FindBrowserOverlayOrThrow("Color Key Browser");
                          Expect(overlay.colorKeys.size() == 1, "Expected browser overlay color key fixture to load.");
                          ExpectFloatNear(overlay.colorKeys.front().sensitivity, ConfigDefaults::COLOR_KEY_SENSITIVITY,
                                          "Expected browser overlay color key sensitivity to default when omitted.");
                      },
                      runMode);
}

void RunConfigModeSourceHelpersTest(TestRunMode /*runMode*/ = TestRunMode::Automated) {
    ModeConfig mode;

    Expect(!ModeHasSource(mode, ModeSourceType::Mirror, "A"),
           "ModeHasSource on empty mode should return false.");

    Expect(AddModeSource(mode, ModeSourceType::Mirror, "A"),
           "AddModeSource of a fresh entry should return true.");
    Expect(mode.sources.size() == 1,
           "AddModeSource of a fresh entry should append exactly one source.");
    Expect(ModeHasSource(mode, ModeSourceType::Mirror, "A"),
           "ModeHasSource should report the added entry.");

    Expect(!AddModeSource(mode, ModeSourceType::Mirror, "A"),
           "AddModeSource of an existing (type, id) should return false (dedup).");
    Expect(mode.sources.size() == 1,
           "Duplicate AddModeSource should not append a second entry.");

    Expect(!AddModeSource(mode, ModeSourceType::Mirror, ""),
           "AddModeSource with empty id should return false.");
    Expect(mode.sources.size() == 1,
           "AddModeSource with empty id should not append.");

    Expect(AddModeSource(mode, ModeSourceType::Image, "A"),
           "Same id with different type is a distinct source and should be added.");
    Expect(mode.sources.size() == 2,
           "Different-type same-id should produce two entries.");
    Expect(ModeHasSource(mode, ModeSourceType::Mirror, "A") && ModeHasSource(mode, ModeSourceType::Image, "A"),
           "Both (Mirror, A) and (Image, A) should be present.");

    Expect(AddModeSource(mode, ModeSourceType::Mirror, "B"),
           "Second distinct mirror should add.");

    Expect(RemoveModeSource(mode, ModeSourceType::Mirror, "A"),
           "RemoveModeSource of an existing entry should return true.");
    Expect(!ModeHasSource(mode, ModeSourceType::Mirror, "A"),
           "After RemoveModeSource, (Mirror, A) should be gone.");
    Expect(ModeHasSource(mode, ModeSourceType::Image, "A"),
           "RemoveModeSource of (Mirror, A) must not remove (Image, A).");
    Expect(ModeHasSource(mode, ModeSourceType::Mirror, "B"),
           "Removing (Mirror, A) must not touch (Mirror, B).");

    Expect(!RemoveModeSource(mode, ModeSourceType::Mirror, "DoesNotExist"),
           "RemoveModeSource of a missing entry should return false.");

    mode.sources.clear();
    AddModeSource(mode, ModeSourceType::Mirror, "A");
    mode.sources.push_back({ ModeSourceType::Mirror, "A" });
    mode.sources.push_back({ ModeSourceType::Mirror, "A" });
    const size_t removed = RemoveAllModeSources(mode, ModeSourceType::Mirror, "A");
    Expect(removed == 3,
           "RemoveAllModeSources should remove every matching (type, id) pair.");
    Expect(mode.sources.empty(),
           "RemoveAllModeSources should leave the sources list empty when no other entries exist.");

    mode.sources.clear();
    AddModeSource(mode, ModeSourceType::Mirror, "OldName");
    AddModeSource(mode, ModeSourceType::Image, "OldName");
    Expect(RenameModeSource(mode, ModeSourceType::Mirror, "OldName", "NewName"),
           "RenameModeSource should report a successful rename.");
    Expect(ModeHasSource(mode, ModeSourceType::Mirror, "NewName"),
           "Renamed entry should be present under the new id.");
    Expect(!ModeHasSource(mode, ModeSourceType::Mirror, "OldName"),
           "Old id should no longer be present after rename.");
    Expect(ModeHasSource(mode, ModeSourceType::Image, "OldName"),
           "RenameModeSource must not touch other types with the same id.");

    Expect(!RenameModeSource(mode, ModeSourceType::Mirror, "NoSuchName", "Anything"),
           "RenameModeSource of a missing entry should return false.");
}