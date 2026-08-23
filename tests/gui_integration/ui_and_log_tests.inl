void RunConfigErrorGuiTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    const std::filesystem::path root = PrepareCaseDirectory("config_error_gui");
    ResetGlobalTestState(root);

    const std::filesystem::path configPath = root / "config.toml";
    std::ofstream out(configPath, std::ios::binary | std::ios::trunc);
    Expect(out.is_open(), "Failed to open config.toml for invalid-config test setup.");
    out << "configVersion = 4\n[[modes]]\nid = \"Fullscreen\"\nwidth =\n";
    out.close();

    LoadConfig();

    Expect(g_configLoadFailed.load(std::memory_order_acquire), "Expected invalid TOML to mark config loading as failed.");
    {
        std::lock_guard<std::mutex> lock(g_configErrorMutex);
        Expect(!g_configLoadError.empty(), "Expected invalid TOML to populate a config error message.");
    }

    if (runMode == TestRunMode::Visual) {
        RunVisualLoop(window, "config-error-gui", &RenderInteractiveConfigErrorFrame);
        return;
    }

    RenderConfigErrorFrame(window);
}

void RunSettingsGuiBasicTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    const std::filesystem::path root = PrepareCaseDirectory("settings_gui_basic");
    ResetGlobalTestState(root);

    LoadConfig();
    Expect(!g_configLoadFailed.load(std::memory_order_acquire), "Expected config load to succeed before basic GUI rendering.");

    g_config.basicModeEnabled = true;
    g_configIsDirty.store(false, std::memory_order_release);

    if (runMode == TestRunMode::Visual) {
        RunVisualLoopWithNinjabrainPreview(window, "settings-gui-basic", &RenderInteractiveSettingsFrame);
        return;
    }

    const std::vector<std::string> tabs = {
        tr("tabs.general"),
        tr("tabs.other"),
        tr("tabs.supporters"),
    };
    for (const std::string& tab : tabs) {
        RenderSettingsFrame(window, tab.c_str());
    }
}

void RunSettingsGuiAdvancedTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    const std::filesystem::path root = PrepareCaseDirectory("settings_gui_advanced");
    ResetGlobalTestState(root);

    LoadConfig();
    Expect(!g_configLoadFailed.load(std::memory_order_acquire), "Expected config load to succeed before advanced GUI rendering.");

    g_config.basicModeEnabled = false;
    g_configIsDirty.store(false, std::memory_order_release);

    if (runMode == TestRunMode::Visual) {
        RunVisualLoopWithNinjabrainPreview(window, "settings-gui-advanced", &RenderInteractiveSettingsFrame);
        return;
    }

    const std::string inputsTab = tr("tabs.inputs");
    const std::string mouseSubTab = tr("inputs.mouse");
    const std::string keyboardSubTab = tr("inputs.keyboard");

    const std::vector<std::string> tabs = {
        tr("tabs.modes"),
        tr("tabs.mirrors"),
        tr("tabs.images"),
        tr("tabs.window_overlays"),
        tr("tabs.browser_overlays"),
        tr("tabs.hotkeys"),
        inputsTab,
        tr("tabs.settings"),
        tr("tabs.appearance"),
        tr("tabs.misc"),
        tr("tabs.supporters"),
    };

    for (const std::string& tab : tabs) {
        if (tab == inputsTab) {
            RenderSettingsFrame(window, tab.c_str(), mouseSubTab.c_str());
            RenderSettingsFrame(window, tab.c_str(), keyboardSubTab.c_str());
            continue;
        }

        RenderSettingsFrame(window, tab.c_str());
    }
}

void RunSettingsSearchSubcategoryFilteringTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    if (!window.hasModernGL()) { std::cout << "SKIP (no GL 3.3+)" << std::endl; return; }
    PrepareDefaultConfigForGui("settings_search_subcategory_filtering", false);

    if (runMode == TestRunMode::Visual) {
        RunVisualLoopWithNinjabrainPreview(window, "settings-search-subcategory-filtering", [&](DummyWindow& visualWindow) {
            RenderSettingsSearchFrame(visualWindow, trc("inputs.cursor_configuration"), tr("tabs.inputs").c_str(), tr("inputs.mouse").c_str());
        });
        return;
    }

    RenderSettingsSearchFrame(window, trc("settings.capture_streaming"), tr("tabs.other").c_str());
    ExpectGuiInteractionRectPresence("config.search_bar", true, "Expected the config search bar to be rendered while filtering settings.");
    ExpectGuiInteractionRectPresence("config.section.settings.capture_streaming", true,
                                     "Expected capture and streaming settings to remain visible for the matching section query.");
    ExpectGuiInteractionRectPresence("config.section.settings.performance", false,
                                     "Expected unrelated settings sections to be hidden for a capture and streaming query.");

    RenderSettingsSearchFrame(window, trc("inputs.cursor_configuration"), tr("tabs.inputs").c_str(), tr("inputs.mouse").c_str());
    ExpectGuiInteractionRectPresence("config.section.inputs.mouse.cursor_configuration", true,
                                     "Expected the mouse cursor configuration section to remain visible for a matching query.");
    ExpectGuiInteractionRectPresence("config.section.inputs.keyboard.key_rebinding", false,
                                     "Expected keyboard sections to be hidden while filtering to mouse cursor configuration.");

    RenderSettingsSearchFrame(window, trc("appearance.title_bar"), tr("tabs.appearance").c_str());
    ExpectGuiInteractionRectPresence("config.section.appearance.title_bar", true,
                                     "Expected the appearance title bar color section to remain visible for a matching appearance subsection query.");
    ExpectGuiInteractionRectPresence("config.section.appearance.sliders_scrollbars", false,
                                     "Expected unrelated appearance sections to be hidden for a title bar subsection query.");
}

void RunSettingsSearchSpecificOptionsTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    if (!window.hasModernGL()) { std::cout << "SKIP (no GL 3.3+)" << std::endl; return; }
    PrepareDefaultConfigForGui("settings_search_specific_options", false);

    if (runMode == TestRunMode::Visual) {
        RunVisualLoopWithNinjabrainPreview(window, "settings-search-specific-options", [&](DummyWindow& visualWindow) {
            RenderSettingsSearchFrame(visualWindow, trc("appearance.slider_grab_active"), tr("tabs.appearance").c_str());
        });
        return;
    }

    RenderSettingsSearchFrame(window, trc("label.fps_limit"), tr("tabs.other").c_str());
    ExpectGuiInteractionRectPresence("config.section.settings.performance", true,
                                     "Expected the performance section to remain visible for the FPS limit option query.");
    ExpectGuiInteractionRectPresence("config.section.settings.capture_streaming", false,
                                     "Expected non-matching settings sections to be hidden for the FPS limit option query.");

    RenderSettingsSearchFrame(window, trc("inputs.enable_key_rebinding"), tr("tabs.inputs").c_str(), tr("inputs.keyboard").c_str());
    ExpectGuiInteractionRectPresence("config.section.inputs.keyboard.key_rebinding", true,
                                     "Expected the keyboard rebind section to remain visible for the enable key rebinding option query.");
    ExpectGuiInteractionRectPresence("config.section.inputs.keyboard.key_repeat_rate", false,
                                     "Expected key repeat controls to be hidden for the enable key rebinding option query.");

    RenderSettingsSearchFrame(window, trc("appearance.slider_grab_active"), tr("tabs.appearance").c_str());
    ExpectGuiInteractionRectPresence("config.section.appearance.sliders_scrollbars", true,
                                     "Expected the sliders and scrollbars section to remain visible for the slider grab active option query.");
    ExpectGuiInteractionRectPresence("config.section.appearance.window", false,
                                     "Expected unrelated appearance sections to be hidden for the slider grab active option query.");
}

void RunSettingsSearchResetOnCloseTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    if (!window.hasModernGL()) { std::cout << "SKIP (no GL 3.3+)" << std::endl; return; }
    PrepareDefaultConfigForGui("settings_search_reset_on_close", false);

    if (runMode == TestRunMode::Visual) {
        RunVisualLoopWithNinjabrainPreview(window, "settings-search-reset-on-close", [&](DummyWindow& visualWindow) {
            RenderSettingsSearchFrame(visualWindow, trc("appearance.title_bar"), tr("tabs.appearance").c_str());
        });
        return;
    }

    RenderSettingsSearchFrame(window, trc("appearance.title_bar"), tr("tabs.appearance").c_str());
    ExpectGuiInteractionRectPresence("config.section.appearance.title_bar", true,
                                     "Expected the title bar section to remain visible while the config search is active.");
    ExpectGuiInteractionRectPresence("config.section.appearance.sliders_scrollbars", false,
                                     "Expected the search filter to hide unrelated appearance sections before closing the GUI.");

    CloseSettingsGuiWindow();
    Expect(!g_showGui.load(std::memory_order_acquire), "Expected closing the settings GUI to hide the config window.");

    g_showGui.store(true, std::memory_order_release);
    g_guiNeedsRecenter.store(true, std::memory_order_release);
    RenderSettingsFrame(window, tr("tabs.appearance").c_str());
    ExpectGuiInteractionRectPresence("config.section.appearance.sliders_scrollbars", true,
                                     "Expected reopening the settings GUI after closing it to clear the config search filter.");
}

void RunProfilerUnspecifiedBreakdownTest(TestRunMode runMode = TestRunMode::Automated) {
    (void)runMode;

    const std::filesystem::path root = PrepareCaseDirectory("profiler_unspecified_breakdown");
    ResetGlobalTestState(root);

    Profiler& profiler = Profiler::GetInstance();
    Profiler::ThreadRingBuffer& threadBuffer = Profiler::GetThreadBuffer();
    const bool wasRenderThread = threadBuffer.isRenderThread;

    profiler.Clear();
    profiler.SetEnabled(true);
    profiler.MarkAsRenderThread();

    {
        PROFILE_SCOPE("Parent Scope");
        {
            PROFILE_SCOPE("Tracked Child Scope");
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    profiler.EndFrame();
    profiler.SetEnabled(false);

    const auto displayData = profiler.GetProfileData();
    const auto& renderEntries = displayData.renderThread;
    Expect(!renderEntries.empty(), "Expected profiler display data after ending a profiled frame.");

    const auto parentIt = std::find_if(renderEntries.begin(), renderEntries.end(), [](const auto& item) {
        return item.second.displayName == "Parent Scope";
    });
    Expect(parentIt != renderEntries.end(), "Expected the parent profiler scope to appear in the render-thread tree.");

    const int parentDepth = parentIt->second.depth;
    const auto subtreeEnd = std::find_if(parentIt + 1, renderEntries.end(), [parentDepth](const auto& item) {
        return item.second.depth <= parentDepth;
    });

    const auto childIt = std::find_if(parentIt + 1, subtreeEnd, [](const auto& item) {
        return item.second.displayName == "Tracked Child Scope";
    });
    Expect(childIt != subtreeEnd, "Expected the tracked child profiler scope to appear below the parent scope.");

    const auto unspecifiedIt = std::find_if(parentIt + 1, subtreeEnd, [](const auto& item) {
        return item.second.displayName == "Unspecified";
    });
    Expect(unspecifiedIt != subtreeEnd, "Expected an Unspecified profiler row for parent self time.");
    Expect(unspecifiedIt->second.depth == parentDepth + 1, "Expected the Unspecified row to be emitted as a child of the parent scope.");
    Expect(unspecifiedIt->second.rollingAverageTime > 0.01,
           "Expected the Unspecified profiler row to respect the 0.01ms visibility threshold.");
    Expect(unspecifiedIt->second.parentPercentage > 0.0,
           "Expected the Unspecified profiler row to report a non-zero share of the parent scope.");

    profiler.Clear();
    profiler.SetEnabled(false);
    threadBuffer.isRenderThread = wasRenderThread;
}

void RunSettingsTabGeneralPopulatedTest(TestRunMode runMode = TestRunMode::Automated) {
    RunPopulatedSettingsTabCase("settings_tab_general_populated", tr("tabs.general"), std::string(), runMode);
}

void RunSettingsTabOtherPopulatedTest(TestRunMode runMode = TestRunMode::Automated) {
    RunPopulatedSettingsTabCase("settings_tab_other_populated", tr("tabs.other"), std::string(), runMode);
}

void RunSettingsTabModesPopulatedTest(TestRunMode runMode = TestRunMode::Automated) {
    RunPopulatedSettingsTabCase("settings_tab_modes_populated", tr("tabs.modes"), std::string(), runMode);
}

void RunSettingsTabMirrorsPopulatedTest(TestRunMode runMode = TestRunMode::Automated) {
    RunPopulatedSettingsTabCase("settings_tab_mirrors_populated", tr("tabs.mirrors"), std::string(), runMode);
}

void RunSettingsTabImagesPopulatedTest(TestRunMode runMode = TestRunMode::Automated) {
    RunPopulatedSettingsTabCase("settings_tab_images_populated", tr("tabs.images"), std::string(), runMode);
}

void RunSettingsTabWindowOverlaysPopulatedTest(TestRunMode runMode = TestRunMode::Automated) {
    RunPopulatedSettingsTabCase("settings_tab_window_overlays_populated", tr("tabs.window_overlays"), std::string(), runMode);
}

void RunSettingsTabBrowserOverlaysPopulatedTest(TestRunMode runMode = TestRunMode::Automated) {
    RunPopulatedSettingsTabCase("settings_tab_browser_overlays_populated", tr("tabs.browser_overlays"), std::string(), runMode);
}

void RunSettingsTabHotkeysPopulatedTest(TestRunMode runMode = TestRunMode::Automated) {
    RunPopulatedSettingsTabCase("settings_tab_hotkeys_populated", tr("tabs.hotkeys"), std::string(), runMode);
}

void RunSettingsTabInputsMousePopulatedTest(TestRunMode runMode = TestRunMode::Automated) {
    RunPopulatedSettingsTabCase("settings_tab_inputs_mouse_populated", tr("tabs.inputs"), tr("inputs.mouse"), runMode);
}

void RunSettingsTabInputsKeyboardPopulatedTest(TestRunMode runMode = TestRunMode::Automated) {
    RunPopulatedSettingsTabCase("settings_tab_inputs_keyboard_populated", tr("tabs.inputs"), tr("inputs.keyboard"), runMode);
}

void RunSettingsTabSettingsPopulatedTest(TestRunMode runMode = TestRunMode::Automated) {
    RunPopulatedSettingsTabCase("settings_tab_settings_populated", tr("tabs.settings"), std::string(), runMode);
}

void RunSettingsTabAppearancePopulatedTest(TestRunMode runMode = TestRunMode::Automated) {
    RunPopulatedSettingsTabCase("settings_tab_appearance_populated", tr("tabs.appearance"), std::string(), runMode);
}

void RunSettingsTabMiscPopulatedTest(TestRunMode runMode = TestRunMode::Automated) {
    RunPopulatedSettingsTabCase("settings_tab_misc_populated", tr("tabs.misc"), std::string(), runMode);
}

void RunSettingsTabSupportersPopulatedTest(TestRunMode runMode = TestRunMode::Automated) {
    RunPopulatedSettingsTabCase("settings_tab_supporters_populated", tr("tabs.supporters"), std::string(), runMode);
}

void RunSettingsTabGeneralDefaultTest(TestRunMode runMode = TestRunMode::Automated) {
    RunDefaultSettingsTabCase("settings_tab_general_default", tr("tabs.general"), std::string(), true, runMode);
}

void RunSettingsTabOtherDefaultTest(TestRunMode runMode = TestRunMode::Automated) {
    RunDefaultSettingsTabCase("settings_tab_other_default", tr("tabs.other"), std::string(), true, runMode);
}

void RunSettingsTabSupportersDefaultTest(TestRunMode runMode = TestRunMode::Automated) {
    RunDefaultSettingsTabCase("settings_tab_supporters_default", tr("tabs.supporters"), std::string(), true, runMode);
}

void RunSettingsTabModesDefaultTest(TestRunMode runMode = TestRunMode::Automated) {
    RunDefaultSettingsTabCase("settings_tab_modes_default", tr("tabs.modes"), std::string(), false, runMode);
}

void RunSettingsTabMirrorsDefaultTest(TestRunMode runMode = TestRunMode::Automated) {
    RunDefaultSettingsTabCase("settings_tab_mirrors_default", tr("tabs.mirrors"), std::string(), false, runMode);
}

void RunSettingsTabImagesDefaultTest(TestRunMode runMode = TestRunMode::Automated) {
    RunDefaultSettingsTabCase("settings_tab_images_default", tr("tabs.images"), std::string(), false, runMode);
}

void RunSettingsTabWindowOverlaysDefaultTest(TestRunMode runMode = TestRunMode::Automated) {
    RunDefaultSettingsTabCase("settings_tab_window_overlays_default", tr("tabs.window_overlays"), std::string(), false, runMode);
}

void RunSettingsTabBrowserOverlaysDefaultTest(TestRunMode runMode = TestRunMode::Automated) {
    RunDefaultSettingsTabCase("settings_tab_browser_overlays_default", tr("tabs.browser_overlays"), std::string(), false,
                              runMode);
}

void RunSettingsTabHotkeysDefaultTest(TestRunMode runMode = TestRunMode::Automated) {
    RunDefaultSettingsTabCase("settings_tab_hotkeys_default", tr("tabs.hotkeys"), std::string(), false, runMode);
}

void RunSettingsTabInputsMouseDefaultTest(TestRunMode runMode = TestRunMode::Automated) {
    RunDefaultSettingsTabCase("settings_tab_inputs_mouse_default", tr("tabs.inputs"), tr("inputs.mouse"), false, runMode);
}

void RunSettingsTabInputsKeyboardDefaultTest(TestRunMode runMode = TestRunMode::Automated) {
    RunDefaultSettingsTabCase("settings_tab_inputs_keyboard_default", tr("tabs.inputs"), tr("inputs.keyboard"), false, runMode);
}

void RunSettingsKeyRepeatSystemRepeatHidesLocalControlsTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    if (!window.hasModernGL()) { std::cout << "SKIP (no GL 3.3+)" << std::endl; return; }
    PrepareDefaultConfigForGui("settings_key_repeat_system_repeat_hides_local_controls", false);

    if (runMode == TestRunMode::Visual) {
     RunVisualLoopWithNinjabrainPreview(window, "settings-key-repeat-system-repeat-hides-local-controls", [&](DummyWindow& visualWindow) {
         g_config.useSystemKeyRepeat = true;
         RenderSettingsFrame(visualWindow, tr("tabs.inputs").c_str(), tr("inputs.keyboard").c_str());
     });
     return;
    }

    int startDelayMs = 0;
    int repeatDelayMs = 0;
    const FILTERKEYS savedOriginalFilterKeys = g_originalFilterKeys;
    const bool savedOriginalFilterKeysCaptured = g_originalFilterKeysCaptured.load(std::memory_order_acquire);

    g_originalFilterKeys = FILTERKEYS{ sizeof(FILTERKEYS) };
    g_originalFilterKeys.iDelayMSec = 275;
    g_originalFilterKeys.iRepeatMSec = 33;
    g_originalFilterKeysCaptured.store(true, std::memory_order_release);

    g_config.useSystemKeyRepeat = true;
    g_config.keyRepeatStartDelay = ConfigDefaults::CONFIG_KEY_REPEAT_START_DELAY;
    g_config.keyRepeatDelay = ConfigDefaults::CONFIG_KEY_REPEAT_DELAY;
    RenderSettingsSearchFrame(window, trc("inputs.key_repeat_delay"), tr("tabs.inputs").c_str(), tr("inputs.keyboard").c_str());
    ExpectGuiInteractionRectPresence("config.section.inputs.keyboard.key_repeat_rate", true,
                      "Expected the key repeat settings section to remain visible for a matching query when system key repeat is enabled.");

    Expect(GetEffectiveKeyRepeatTimings(startDelayMs, repeatDelayMs), "Expected key repeat timings to resolve for Auto values.");
    Expect(startDelayMs == 275,
        "Expected system key repeat Auto start delay to resolve to the captured system default.");
    Expect(repeatDelayMs == 33,
        "Expected system key repeat Auto delay to resolve to the captured system default.");
    Expect(GetKeyRepeatSliderFormat(-1, ConfigDefaults::CONFIG_KEY_REPEAT_AUTO_START_DELAY_MS, true) == "System default",
        "Expected the system key repeat start delay slider to label Auto as System default.");
    Expect(GetKeyRepeatDelaySliderFormat(-1, ConfigDefaults::CONFIG_KEY_REPEAT_AUTO_DELAY_MS, true) == "System default",
        "Expected the system key repeat delay slider to label Auto as System default.");

    g_config.keyRepeatDelay = 240;
    Expect(GetEffectiveKeyRepeatTimings(startDelayMs, repeatDelayMs), "Expected key repeat timings to resolve for a 1ms delay value.");
    Expect(repeatDelayMs == 240, "Expected system key repeat delay to preserve the legacy 300ms slider range.");

    g_originalFilterKeys = savedOriginalFilterKeys;
    g_originalFilterKeysCaptured.store(savedOriginalFilterKeysCaptured, std::memory_order_release);
}

void RunSettingsTabSettingsDefaultTest(TestRunMode runMode = TestRunMode::Automated) {
    RunDefaultSettingsTabCase("settings_tab_settings_default", tr("tabs.settings"), std::string(), false, runMode);
}

void RunSettingsTabAppearanceDefaultTest(TestRunMode runMode = TestRunMode::Automated) {
    RunDefaultSettingsTabCase("settings_tab_appearance_default", tr("tabs.appearance"), std::string(), false, runMode);
}

void RunSettingsTabMiscDefaultTest(TestRunMode runMode = TestRunMode::Automated) {
    RunDefaultSettingsTabCase("settings_tab_misc_default", tr("tabs.misc"), std::string(), false, runMode);
}

void ResetProfileTestState(std::string_view caseName) {
    const std::filesystem::path root = PrepareCaseDirectory(caseName);
    ResetGlobalTestState(root);
    g_profilesConfig = ProfilesConfig();
    LoadConfig();
    ExpectConfigLoadSucceeded(std::string(caseName) + " initial load");
}

class ScopedLatestGameViewportSizeOverride {
  public:
    ScopedLatestGameViewportSizeOverride(int width, int height) {
        m_hadOriginal = GetLatestGameViewportSize(m_originalWidth, m_originalHeight);
        SetLatestGameViewportSizeForTests(width, height);
    }

    ~ScopedLatestGameViewportSizeOverride() {
        if (m_hadOriginal) {
            SetLatestGameViewportSizeForTests(m_originalWidth, m_originalHeight);
        } else {
            SetLatestGameViewportSizeForTests(0, 0);
        }
    }

  private:
    int m_originalWidth = 0;
    int m_originalHeight = 0;
    bool m_hadOriginal = false;
};

std::filesystem::path GetProfilesDirectoryForTests() {
    return std::filesystem::path(g_toolscreenPath) / "profiles";
}

std::filesystem::path GetProfilesMetadataPathForTests() {
    return std::filesystem::path(g_toolscreenPath) / "profiles.toml";
}

bool ContainsFileNameIgnoreCase(const std::vector<std::string>& fileNames, std::string_view expectedName) {
    for (const auto& fileName : fileNames) {
        if (EqualsIgnoreCase(fileName, std::string(expectedName))) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> GetTrackedProfileNamesSorted() {
    std::vector<std::string> names;
    names.reserve(g_profilesConfig.profiles.size());
    for (const auto& profile : g_profilesConfig.profiles) {
        names.push_back(profile.name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

Config LoadProfileConfigForTests(std::string_view profileName) {
    const std::filesystem::path profilePath = GetProfilesDirectoryForTests() / (std::string(profileName) + ".toml");
    Expect(std::filesystem::exists(profilePath),
           "Expected profile file to exist for '" + std::string(profileName) + "'.");

    Config profileConfig;
    Expect(LoadConfigFromTomlFile(profilePath.wstring(), profileConfig),
           "Expected profile TOML to remain loadable for '" + std::string(profileName) + "'.");
    return profileConfig;
}

void ExpectProfilesMetadataMatchesDisk(const std::vector<std::string>& expectedProfiles, const std::string& context) {
    Expect(LoadProfilesConfig(), context + " should keep profiles metadata loadable.");

    std::vector<std::string> diskProfiles;
    const std::filesystem::path profilesDir = GetProfilesDirectoryForTests();
    if (std::filesystem::exists(profilesDir)) {
        for (const auto& fileName : ListDirectoryFileNamesSorted(profilesDir)) {
            const std::filesystem::path path(fileName);
            if (EqualsIgnoreCase(path.extension().string(), ".toml")) {
                diskProfiles.push_back(path.stem().string());
            }
        }
    }
    std::sort(diskProfiles.begin(), diskProfiles.end());

    std::vector<std::string> trackedProfiles = GetTrackedProfileNamesSorted();
    ExpectVectorEquals(trackedProfiles, diskProfiles,
                       context + " should keep tracked profile metadata aligned with on-disk profile files.");

    std::vector<std::string> expectedSorted = expectedProfiles;
    std::sort(expectedSorted.begin(), expectedSorted.end());
    ExpectVectorEquals(trackedProfiles, expectedSorted, context + " should preserve the expected profile set.");

    bool foundActiveProfile = false;
    for (const auto& profileName : trackedProfiles) {
        if (EqualsIgnoreCase(profileName, g_profilesConfig.activeProfile)) {
            foundActiveProfile = true;
        }
        (void)LoadProfileConfigForTests(profileName);
    }

    Expect(foundActiveProfile, context + " should keep the active profile present in tracked metadata.");
}

Config MakeSingleModeProfileSnapshot(const std::string& modeId, int width, int height, const std::string& defaultMode = std::string()) {
    Config snapshot;
    Expect(LoadEmbeddedDefaultConfig(snapshot), "LoadEmbeddedDefaultConfig should succeed for single-mode profile fixtures.");

    snapshot.configVersion = GetConfigVersion();
    snapshot.modes.clear();

    ModeConfig mode;
    mode.id = modeId;
    mode.width = width;
    mode.height = height;
    mode.manualWidth = width;
    mode.manualHeight = height;
    snapshot.modes.push_back(mode);

    snapshot.defaultMode = defaultMode.empty() ? modeId : defaultMode;
    return snapshot;
}

void PublishCurrentModeForTests(const std::string& modeId) {
    std::lock_guard<std::mutex> lock(g_modeIdMutex);
    g_currentModeId = modeId;
    const int nextIndex = 1 - g_currentModeIdIndex.load(std::memory_order_relaxed);
    g_modeIdBuffers[nextIndex] = modeId;
    g_currentModeIdIndex.store(nextIndex, std::memory_order_release);
}

void ExpectPublishedViewportMatchesMode(const std::string& expectedModeId, const std::string& context) {
    auto cfgSnap = GetConfigSnapshot();
    Expect(cfgSnap != nullptr, context + " should have a published config snapshot.");

    const ModeConfig* expectedMode = GetModeFromSnapshot(*cfgSnap, expectedModeId);
    Expect(expectedMode != nullptr, context + " should resolve the expected mode in the published snapshot.");

    const ModeViewportInfo viewport = GetCurrentModeViewport_Internal();
    Expect(viewport.valid, context + " should expose a valid current viewport.");
    Expect(viewport.width == expectedMode->width, context + " should expose the expected viewport width.");
    Expect(viewport.height == expectedMode->height, context + " should expose the expected viewport height.");

    UpdateCachedViewportMode();
    const CachedModeViewport& cachedViewport = g_viewportModeCache[g_viewportModeCacheIndex.load(std::memory_order_acquire)];
    Expect(cachedViewport.valid, context + " should keep the cached viewport valid.");
    Expect(cachedViewport.width == expectedMode->width, context + " should cache the expected viewport width.");
    Expect(cachedViewport.height == expectedMode->height, context + " should cache the expected viewport height.");
}

    void RunSettingsMouseTranslationPrefersLiveViewportTest(TestRunMode runMode = TestRunMode::Automated) {
        (void)runMode;

        DummyWindow window(kWindowWidth, kWindowHeight, false);
        const std::filesystem::path root = PrepareCaseDirectory("settings_mouse_translation_prefers_live_viewport");
        ResetGlobalTestState(root);

        RECT clientRect{};
        Expect(GetClientRect(window.hwnd(), &clientRect) != FALSE,
            "Expected mouse translation regression test window to expose a client rect.");
        const int clientWidth = clientRect.right - clientRect.left;
        const int clientHeight = clientRect.bottom - clientRect.top;
        Expect(clientWidth > 0 && clientHeight > 0,
            "Expected mouse translation regression test window to expose a positive client size.");

        g_config = MakeSingleModeProfileSnapshot("MouseTranslate", 800, 600);
        g_configLoaded.store(true, std::memory_order_release);
        PublishConfigSnapshot();
        PublishCurrentModeForTests("MouseTranslate");

        UpdateCachedWindowMetricsFromSize(clientWidth + 240, clientHeight + 180);
        Expect(GetCachedWindowWidth() == clientWidth + 240,
            "Expected the mouse translation regression test to stage a stale cached client width.");
        Expect(GetCachedWindowHeight() == clientHeight + 180,
            "Expected the mouse translation regression test to stage a stale cached client height.");

        {
            ScopedCursorVisibilityOverride cursorVisible(true);
            ScopedLatestGameViewportSizeOverride liveViewport(640, 480);

            g_showGui.store(false, std::memory_order_release);

            LPARAM translatedLParam = MAKELPARAM(clientWidth / 2, clientHeight / 2);
            const InputHandlerResult result = HandleMouseCoordinateTranslationPhase(window.hwnd(), WM_MOUSEMOVE, 0, translatedLParam);
            const int translatedX = static_cast<int>(static_cast<short>(LOWORD(translatedLParam)));
            const int translatedY = static_cast<int>(static_cast<short>(HIWORD(translatedLParam)));

            Expect(!result.consumed,
                "Expected mouse coordinate translation to adjust WM_MOUSEMOVE in-place without consuming the message.");
            Expect(translatedX == 320,
                "Expected mouse coordinate translation to use the live game viewport width instead of a stale cached mode width.");
            Expect(translatedY == 240,
                "Expected mouse coordinate translation to use the live game viewport height instead of a stale cached mode height.");

            ScopedLatestGameViewportSizeOverride staleGuiViewport(1024, 768);

            g_showGui.store(true, std::memory_order_release);
            CloseSettingsGuiWindow();

            int viewportAfterCloseW = 0;
            int viewportAfterCloseH = 0;
            Expect(!GetLatestGameViewportSize(viewportAfterCloseW, viewportAfterCloseH),
                "Expected closing the settings GUI to invalidate any GUI-era latest game viewport size.");

            LPARAM translatedAfterClose = MAKELPARAM(clientWidth / 2, clientHeight / 2);
            const InputHandlerResult resultAfterClose =
                HandleMouseCoordinateTranslationPhase(window.hwnd(), WM_MOUSEMOVE, 0, translatedAfterClose);
            const int translatedAfterCloseX = static_cast<int>(static_cast<short>(LOWORD(translatedAfterClose)));
            const int translatedAfterCloseY = static_cast<int>(static_cast<short>(HIWORD(translatedAfterClose)));

            Expect(!resultAfterClose.consumed,
                "Expected post-close mouse translation to keep adjusting WM_MOUSEMOVE in-place without consuming the message.");
            Expect(translatedAfterCloseX == 400,
                "Expected post-close mouse translation to fall back to the mode width instead of a stale GUI-era viewport width.");
            Expect(translatedAfterCloseY == 300,
                "Expected post-close mouse translation to fall back to the mode height instead of a stale GUI-era viewport height.");
        }
    }

        void RunModeSwitchInvalidatesLatestViewportSizeTest(TestRunMode runMode = TestRunMode::Automated) {
            (void)runMode;

            DummyWindow window(kWindowWidth, kWindowHeight, false);
            const std::filesystem::path root = PrepareCaseDirectory("mode_switch_invalidates_latest_viewport_size");
            ResetGlobalTestState(root);

            RECT clientRect{};
            Expect(GetClientRect(window.hwnd(), &clientRect) != FALSE,
                "Expected mode-switch viewport invalidation test window to expose a client rect.");
            const int clientWidth = clientRect.right - clientRect.left;
            const int clientHeight = clientRect.bottom - clientRect.top;
            Expect(clientWidth > 0 && clientHeight > 0,
                "Expected mode-switch viewport invalidation test window to expose a positive client size.");

            Config snapshot;
            Expect(LoadEmbeddedDefaultConfig(snapshot),
                "LoadEmbeddedDefaultConfig should succeed for the mode-switch viewport invalidation fixture.");

            snapshot.configVersion = GetConfigVersion();
            snapshot.modes.clear();
            snapshot.defaultMode = "Fullscreen";

            ModeConfig fullscreenMode;
            fullscreenMode.id = "Fullscreen";
            fullscreenMode.width = 1600;
            fullscreenMode.height = 900;
            fullscreenMode.manualWidth = 1600;
            fullscreenMode.manualHeight = 900;
            fullscreenMode.stretch.enabled = true;
            fullscreenMode.stretch.x = 0;
            fullscreenMode.stretch.y = 0;
            fullscreenMode.stretch.width = 1600;
            fullscreenMode.stretch.height = 900;
            snapshot.modes.push_back(fullscreenMode);

            ModeConfig previousMode;
            previousMode.id = "MouseTranslate";
            previousMode.width = 800;
            previousMode.height = 600;
            previousMode.manualWidth = 800;
            previousMode.manualHeight = 600;
            snapshot.modes.push_back(previousMode);

            g_config = snapshot;
            g_configLoaded.store(true, std::memory_order_release);
            PublishConfigSnapshot();
            PublishCurrentModeForTests("MouseTranslate");

            {
             ScopedCursorVisibilityOverride cursorVisible(true);
             ScopedLatestGameViewportSizeOverride staleViewport(1280, 720);

             g_showGui.store(false, std::memory_order_release);

             Expect(SwitchToMode("Fullscreen", "test mode switch invalidates viewport", /*forceCut=*/true),
                 "Expected mode-switch viewport invalidation regression to switch to Fullscreen.");

             int viewportAfterSwitchW = 0;
             int viewportAfterSwitchH = 0;
             Expect(!GetLatestGameViewportSize(viewportAfterSwitchW, viewportAfterSwitchH),
                 "Expected SwitchToMode to invalidate the previous mode's live viewport size.");

             ModeViewportInfo resolvedViewport;
             Expect(ResolvePresentedGameViewport(resolvedViewport),
                 "Expected post-switch mouse translation regression to resolve the presented viewport.");
             Expect(resolvedViewport.width > 0 && resolvedViewport.height > 0,
                 "Expected post-switch mouse translation regression to resolve positive viewport dimensions.");

             const int mouseClientX = clientWidth / 2;
             const int mouseClientY = clientHeight / 2;
             const int expectedTranslatedX = static_cast<int>(static_cast<float>(mouseClientX) *
                 (static_cast<float>(resolvedViewport.width) / static_cast<float>(clientWidth)));
             const int expectedTranslatedY = static_cast<int>(static_cast<float>(mouseClientY) *
                 (static_cast<float>(resolvedViewport.height) / static_cast<float>(clientHeight)));

             LPARAM translatedLParam = MAKELPARAM(mouseClientX, mouseClientY);
             const InputHandlerResult result = HandleMouseCoordinateTranslationPhase(window.hwnd(), WM_MOUSEMOVE, 0, translatedLParam);
             const int translatedX = static_cast<int>(static_cast<short>(LOWORD(translatedLParam)));
             const int translatedY = static_cast<int>(static_cast<short>(HIWORD(translatedLParam)));

             Expect(!result.consumed,
                 "Expected post-switch mouse translation to adjust WM_MOUSEMOVE in-place without consuming the message.");
             Expect(translatedX == expectedTranslatedX,
                 "Expected post-switch mouse translation to use the resolved post-switch viewport width instead of a stale pre-switch viewport width.");
             Expect(translatedY == expectedTranslatedY,
                 "Expected post-switch mouse translation to use the resolved post-switch viewport height instead of a stale pre-switch viewport height.");
             Expect(translatedX != 640 || translatedY != 360,
                 "Expected post-switch mouse translation to stop using the stale pre-switch live viewport size.");
            }
        }

void RunProfileApplyFieldsRoundtripTest(TestRunMode runMode = TestRunMode::Automated) {
    (void)runMode;
    ResetProfileTestState("profile_apply_fields_roundtrip");

    g_config.defaultMode = "TestMode";
    g_config.mirrorGammaMode = MirrorGammaMode::AssumeLinear;
    g_config.mouseSensitivity = 3.5f;
    g_config.windowsMouseSpeed = 7;
    g_config.allowCursorEscape = true;
    g_config.confineCursor = true;
    g_config.useSystemKeyRepeat = true;
    g_config.keyRepeatStartDelay = 42;
    g_config.keyRepeatDelay = 2;
    g_config.autoBorderless = true;
    g_config.hideAnimationsInGame = true;
    g_config.limitCaptureFramerate = false;
    g_config.obsFramerate = 15;
    g_config.fpsLimit = 144;
    g_config.disableHookChaining = true;
    g_config.basicModeEnabled = true;
    g_config.fontPath = "fonts/TestFont.ttf";
    g_config.lang = "zh_CN";
    g_config.appearance.theme = "Monokai";
    g_config.guiHotkey = { VK_F8 };
    g_config.ninjabrainOverlay.enabled = false;
    g_config.ninjabrainOverlay.apiBaseUrl = "http://127.0.0.1:60000";

    ModeConfig mode;
    mode.id = "TestMode";
    mode.width = 800;
    mode.height = 600;
    g_config.modes.push_back(mode);

    MirrorConfig mirror;
    mirror.name = "TestMirror";
    g_config.mirrors.push_back(mirror);

    Config dst;
    ApplyProfileFields(g_config, dst);

    Expect(dst.defaultMode == "TestMode", "defaultMode should roundtrip via ApplyProfileFields.");
    Expect(dst.mirrorGammaMode == MirrorGammaMode::AssumeLinear, "mirrorGammaMode should roundtrip.");
    Expect(std::abs(dst.mouseSensitivity - 3.5f) < 0.01f, "mouseSensitivity should roundtrip.");
    Expect(dst.windowsMouseSpeed == 7, "windowsMouseSpeed should roundtrip.");
    Expect(dst.allowCursorEscape, "allowCursorEscape should roundtrip.");
    Expect(dst.confineCursor, "confineCursor should roundtrip.");
    Expect(dst.useSystemKeyRepeat, "useSystemKeyRepeat should roundtrip.");
    Expect(dst.keyRepeatStartDelay == 42, "keyRepeatStartDelay should roundtrip.");
    Expect(dst.keyRepeatDelay == 2, "keyRepeatDelay should roundtrip.");
    Expect(dst.autoBorderless == true, "autoBorderless should roundtrip.");
    Expect(dst.hideAnimationsInGame == true, "hideAnimationsInGame should roundtrip.");
    Expect(dst.limitCaptureFramerate == false, "limitCaptureFramerate should roundtrip.");
    Expect(dst.obsFramerate == 15, "obsFramerate should roundtrip.");
    Expect(dst.fpsLimit == 144, "fpsLimit should roundtrip.");
    Expect(dst.disableHookChaining, "disableHookChaining should roundtrip.");
    Expect(dst.modes.size() == g_config.modes.size(), "modes should roundtrip.");
    Expect(dst.mirrors.size() == g_config.mirrors.size(), "mirrors should roundtrip.");
    Expect(dst.basicModeEnabled, "basicModeEnabled should roundtrip.");
    Expect(dst.fontPath == "fonts/TestFont.ttf", "fontPath should roundtrip.");
    Expect(dst.lang == "zh_CN", "lang should roundtrip.");
    Expect(dst.appearance.theme == "Monokai", "appearance should roundtrip.");
    Expect(dst.guiHotkey.size() == 1 && dst.guiHotkey[0] == VK_F8, "guiHotkey should roundtrip.");
    Expect(!dst.ninjabrainOverlay.enabled, "ninjabrainOverlay should roundtrip.");
    Expect(dst.ninjabrainOverlay.apiBaseUrl == "http://127.0.0.1:60000", "ninjabrainOverlay settings should roundtrip.");

    ProfileSectionSelection mirrorsOnly;
    mirrorsOnly.modes = false;
    mirrorsOnly.images = false;
    mirrorsOnly.windowOverlays = false;
    mirrorsOnly.browserOverlays = false;
    mirrorsOnly.ninjabrainOverlay = false;
    mirrorsOnly.hotkeys = false;
    mirrorsOnly.inputsMouse = false;
    mirrorsOnly.captureWindow = false;
    mirrorsOnly.settings = false;
    mirrorsOnly.appearance = false;

    Config mirrorsOnlyDst;
    ApplyProfileFields(g_config, mirrorsOnlyDst, mirrorsOnly);

    Expect(mirrorsOnlyDst.mirrors.size() == g_config.mirrors.size(), "Selected mirror fields should copy with section filtering.");
    Expect(mirrorsOnlyDst.modes.empty(), "Unselected modes should not copy with section filtering.");
    Expect(mirrorsOnlyDst.windowOverlays.empty(), "Unselected window overlays should not copy with section filtering.");
    Expect(mirrorsOnlyDst.mouseSensitivity == 1.0f, "Unselected input/mouse settings should remain at defaults.");
    Expect(mirrorsOnlyDst.fpsLimit == Config().fpsLimit, "Unselected settings should remain at defaults.");
    Expect(mirrorsOnlyDst.basicModeEnabled == Config().basicModeEnabled, "Unselected appearance settings should remain at defaults.");
    Expect(mirrorsOnlyDst.ninjabrainOverlay.apiBaseUrl == Config().ninjabrainOverlay.apiBaseUrl,
        "Unselected Ninjabrain overlay settings should remain at defaults.");
}

void RunProfilesConfigRoundtripTest(TestRunMode runMode = TestRunMode::Automated) {
    (void)runMode;
    ResetProfileTestState("profiles_config_roundtrip");

    g_profilesConfig.activeProfile = "Custom";
    g_profilesConfig.profiles.clear();
    ProfileMetadata pm1;
    pm1.name = "Default";
    pm1.color[0] = 0.1f;
    pm1.color[1] = 0.2f;
    pm1.color[2] = 0.3f;
    pm1.sections.browserOverlays = false;
    pm1.sections.windowOverlays = false;
    pm1.sections.ninjabrainOverlay = false;
    g_profilesConfig.profiles.push_back(pm1);
    ProfileMetadata pm2;
    pm2.name = "Custom";
    pm2.color[0] = 0.9f;
    pm2.color[1] = 0.8f;
    pm2.color[2] = 0.7f;
    pm2.sections.mirrors = false;
    pm2.sections.inputsMouse = false;
    pm2.sections.appearance = false;
    g_profilesConfig.profiles.push_back(pm2);

    SaveProfilesConfig();

    g_profilesConfig = ProfilesConfig();

    const bool loaded = LoadProfilesConfig();
    Expect(loaded, "LoadProfilesConfig should succeed after SaveProfilesConfig.");
    Expect(g_profilesConfig.activeProfile == "Custom", "activeProfile should roundtrip.");
    Expect(g_profilesConfig.profiles.size() == 2, "profiles count should roundtrip.");
    Expect(g_profilesConfig.profiles[0].name == "Default", "First profile name should roundtrip.");
    Expect(std::abs(g_profilesConfig.profiles[0].color[0] - 0.1f) < 0.01f, "First profile color[0] should roundtrip.");
        Expect(!g_profilesConfig.profiles[0].sections.browserOverlays,
            "First profile browser overlay selection should roundtrip.");
        Expect(!g_profilesConfig.profiles[0].sections.windowOverlays,
            "First profile window overlay selection should roundtrip.");
        Expect(!g_profilesConfig.profiles[0].sections.ninjabrainOverlay,
            "First profile Ninjabrain overlay selection should roundtrip.");
    Expect(g_profilesConfig.profiles[1].name == "Custom", "Second profile name should roundtrip.");
    Expect(std::abs(g_profilesConfig.profiles[1].color[2] - 0.7f) < 0.01f, "Second profile color[2] should roundtrip.");
        Expect(!g_profilesConfig.profiles[1].sections.mirrors, "Second profile mirror selection should roundtrip.");
        Expect(!g_profilesConfig.profiles[1].sections.inputsMouse,
            "Second profile input/mouse selection should roundtrip.");
        Expect(!g_profilesConfig.profiles[1].sections.appearance,
            "Second profile appearance selection should roundtrip.");
}

void RunProfileNameValidationTest(TestRunMode runMode = TestRunMode::Automated) {
    (void)runMode;
    Expect(!IsValidProfileName(""), "Empty name should be invalid.");
    Expect(!IsValidProfileName("a/b"), "Name with / should be invalid.");
    Expect(!IsValidProfileName("a\\b"), "Name with \\ should be invalid.");
    Expect(!IsValidProfileName("a:b"), "Name with : should be invalid.");
    Expect(!IsValidProfileName("a*b"), "Name with * should be invalid.");
    Expect(!IsValidProfileName("a?b"), "Name with ? should be invalid.");
    Expect(!IsValidProfileName("CON"), "Windows reserved name should be invalid.");
    Expect(!IsValidProfileName("nul"), "Windows reserved name (lowercase) should be invalid.");
    Expect(!IsValidProfileName(".."), "Double dot should be invalid.");
    Expect(!IsValidProfileName("a..b"), "Name containing .. should be invalid.");
    Expect(!IsValidProfileName(" leading"), "Leading space should be invalid.");
    Expect(!IsValidProfileName("trailing "), "Trailing space should be invalid.");
    Expect(!IsValidProfileName("trailing."), "Trailing dot should be invalid.");
    Expect(IsValidProfileName("Default"), "Normal name should be valid.");
    Expect(IsValidProfileName("My Profile 2"), "Name with spaces and digits should be valid.");
    Expect(IsValidProfileName("PvP-Config"), "Name with hyphen should be valid.");
}

void RunProfileCreateDuplicateDeleteTest(TestRunMode runMode = TestRunMode::Automated) {
    (void)runMode;
    ResetProfileTestState("profile_create_dup_delete");

    Expect(g_profilesConfig.profiles.size() == 1, "Should start with 1 profile after LoadConfig migration.");
    Expect(g_profilesConfig.profiles[0].name == kDefaultProfileName, "First profile should be Default.");
    g_config.defaultMode = "CurrentProfileMode";

    const bool created = CreateNewProfile("Second");
    Expect(created, "CreateNewProfile should succeed.");
    Expect(g_profilesConfig.profiles.size() == 2, "Should have 2 profiles after create.");

    SwitchProfile("Second");
    Expect(g_config.defaultMode == "CurrentProfileMode",
        "CreateNewProfile should seed the new profile from the current merged config so switching is non-destructive.");

    g_configIsDirty.store(true, std::memory_order_release);
    SaveConfigImmediate();

    std::ifstream configIn(GetCurrentConfigPath(), std::ios::binary);
    Expect(configIn.is_open(), "Saved config.toml should remain readable after switching to a new profile.");
    std::ostringstream configBuffer;
    configBuffer << configIn.rdbuf();
    const std::string savedConfigToml = configBuffer.str();
    Expect(savedConfigToml.find("\nmode = []") == std::string::npos && savedConfigToml.rfind("mode = []", 0) != 0,
           "Saving with an active profile should not emit an empty top-level mode array assignment.");

    SwitchProfile(kDefaultProfileName);

    const bool dupCreated = CreateNewProfile("Second");
    Expect(!dupCreated, "CreateNewProfile with duplicate name should fail.");
    Expect(g_profilesConfig.profiles.size() == 2, "Profile count should not change on duplicate create.");

    const bool duped = DuplicateProfile("Second", "Third");
    Expect(duped, "DuplicateProfile should succeed.");
    Expect(g_profilesConfig.profiles.size() == 3, "Should have 3 profiles after duplicate.");

    const bool dupDuped = DuplicateProfile("Second", "Third");
    Expect(!dupDuped, "DuplicateProfile with existing dest name should fail.");

    g_profilesConfig.activeProfile = "Second";
    DeleteProfile(kDefaultProfileName);
    Expect(g_profilesConfig.profiles.size() == 2, "Should have 2 profiles after delete (not active).");

    const bool invalidCreated = CreateNewProfile("a/b");
    Expect(!invalidCreated, "CreateNewProfile with invalid name should fail.");
}

void RunProfileMigrateTest(TestRunMode runMode = TestRunMode::Automated) {
    (void)runMode;
    const std::filesystem::path root = PrepareCaseDirectory("profile_migrate");
    ResetGlobalTestState(root);
    g_profilesConfig = ProfilesConfig();

    g_config.defaultMode = "MigrateTest";
    g_config.mouseSensitivity = 5.0f;

    const bool migrated = MigrateToProfiles();
    Expect(migrated, "MigrateToProfiles should succeed on first call.");
    Expect(g_profilesConfig.profiles.size() == 1, "Should have 1 profile after migration.");
    Expect(g_profilesConfig.activeProfile == kDefaultProfileName, "Active profile should be Default.");

    const std::wstring profilePath = g_toolscreenPath + L"\\profiles\\" + Utf8ToWide(std::string(kDefaultProfileName)) + L".toml";
    Expect(std::filesystem::exists(profilePath), "Default profile file should exist on disk.");

    const bool migratedAgain = MigrateToProfiles();
    Expect(!migratedAgain, "MigrateToProfiles should return false if profiles dir already exists.");
}

void RunProfileDeleteGuardsTest(TestRunMode runMode = TestRunMode::Automated) {
    (void)runMode;
    ResetProfileTestState("profile_delete_guards");

    MigrateToProfiles();
    LoadProfilesConfig();

    Expect(g_profilesConfig.profiles.size() == 1, "Should start with 1 profile.");
    DeleteProfile(kDefaultProfileName);
    Expect(g_profilesConfig.profiles.size() == 1, "Should not delete last profile.");

    CreateNewProfile("Other");
    g_profilesConfig.activeProfile = kDefaultProfileName;
    DeleteProfile(kDefaultProfileName);
    Expect(g_profilesConfig.profiles.size() == 2, "Should not delete active profile.");

    DeleteProfile("Other");
    Expect(g_profilesConfig.profiles.size() == 1, "Should delete non-active profile.");
}

void RunProfileRenameTest(TestRunMode runMode = TestRunMode::Automated) {
    (void)runMode;
    ResetProfileTestState("profile_rename");

    MigrateToProfiles();
    LoadProfilesConfig();
    CreateNewProfile("OldName");

    g_profilesConfig.activeProfile = "OldName";
    const bool renamed = RenameProfile("OldName", "NewName");
    Expect(renamed, "RenameProfile should succeed.");
    Expect(g_profilesConfig.activeProfile == "NewName", "Active profile should be updated after rename.");

    bool found = false;
    for (const auto& pm : g_profilesConfig.profiles) {
        if (pm.name == "NewName") {
            found = true;
            break;
        }
    }
    Expect(found, "Renamed profile should exist in metadata.");

    const float renamedColor[3] = { 0.25f, 0.5f, 0.75f };
    ProfileSectionSelection renamedSections;
    renamedSections.browserOverlays = false;
    renamedSections.captureWindow = false;
    renamedSections.settings = false;
    const bool updated = UpdateProfileMetadata("NewName", "newname", renamedColor, renamedSections);
    Expect(updated, "UpdateProfileMetadata should allow case-only rename, color changes, and section updates.");
    Expect(g_profilesConfig.activeProfile == "newname", "Active profile should follow case-only rename updates.");

    bool foundUpdated = false;
    for (const auto& pm : g_profilesConfig.profiles) {
        if (pm.name == "newname") {
            foundUpdated = true;
            Expect(std::abs(pm.color[0] - renamedColor[0]) < 0.01f, "Updated profile color[0] should persist.");
            Expect(std::abs(pm.color[1] - renamedColor[1]) < 0.01f, "Updated profile color[1] should persist.");
            Expect(std::abs(pm.color[2] - renamedColor[2]) < 0.01f, "Updated profile color[2] should persist.");
            Expect(!pm.sections.browserOverlays, "Updated profile browser overlay selection should persist.");
            Expect(!pm.sections.captureWindow, "Updated profile capture/window selection should persist.");
            Expect(!pm.sections.settings, "Updated profile settings selection should persist.");
            break;
        }
    }
    Expect(foundUpdated, "Updated profile should exist after case-only rename.");

    const bool dupRename = RenameProfile("newname", kDefaultProfileName);
    Expect(!dupRename, "Rename to existing name should fail.");

    const bool invalidRename = RenameProfile("newname", "a/b");
    Expect(!invalidRename, "Rename to invalid name should fail.");
}

    void RunProfileSelectiveSwitchSharedFallbackTest(TestRunMode runMode = TestRunMode::Automated) {
        (void)runMode;
        ResetProfileTestState("profile_selective_switch_shared_fallback");

        auto setSingleMirror = [](const std::string& name) {
         g_config.mirrors.clear();
         MirrorConfig mirror;
         mirror.name = name;
         g_config.mirrors.push_back(mirror);
        };

        auto setSingleWindowOverlay = [](const std::string& name) {
         g_config.windowOverlays.clear();
         WindowOverlayConfig overlay;
         overlay.name = name;
         overlay.windowTitle = name;
         g_config.windowOverlays.push_back(overlay);
        };

        setSingleMirror("MirrorA");
        setSingleWindowOverlay("SharedOverlayA");
        g_config.ninjabrainOverlay.enabled = false;
        g_config.ninjabrainOverlay.apiBaseUrl = "http://127.0.0.1:52533";

        const float defaultColor[3] = { kDefaultProfileColor[0], kDefaultProfileColor[1], kDefaultProfileColor[2] };
        ProfileSectionSelection mirrorsOnly;
        mirrorsOnly.modes = false;
        mirrorsOnly.images = false;
        mirrorsOnly.windowOverlays = false;
        mirrorsOnly.browserOverlays = false;
        mirrorsOnly.ninjabrainOverlay = false;
        mirrorsOnly.hotkeys = false;
        mirrorsOnly.inputsMouse = false;
        mirrorsOnly.captureWindow = false;
        mirrorsOnly.settings = false;
        mirrorsOnly.appearance = false;
        Expect(UpdateProfileMetadata(kDefaultProfileName, kDefaultProfileName, defaultColor, mirrorsOnly),
            "Default profile should support narrowing its override sections.");
        Expect(g_sharedConfig.windowOverlays.size() == 1 && g_sharedConfig.windowOverlays[0].name == "SharedOverlayA",
            "Unselected window overlays should move into the shared config.");
        Expect(!g_sharedConfig.ninjabrainOverlay.enabled,
            "Unselected Ninjabrain overlay settings should stay in the shared config.");

        Expect(CreateNewProfile("OverlayOnly"), "CreateNewProfile should create the selective overlay profile.");
        SwitchProfile("OverlayOnly");

        setSingleMirror("MirrorSharedB");
        setSingleWindowOverlay("OverlayB");
        g_config.ninjabrainOverlay.enabled = true;
        g_config.ninjabrainOverlay.apiBaseUrl = "http://127.0.0.1:60000";

        const float overlayColor[3] = { 0.6f, 0.7f, 0.8f };
        ProfileSectionSelection overlayOnly;
        overlayOnly.modes = false;
        overlayOnly.mirrors = false;
        overlayOnly.images = false;
        overlayOnly.windowOverlays = true;
        overlayOnly.browserOverlays = false;
        overlayOnly.ninjabrainOverlay = true;
        overlayOnly.hotkeys = false;
        overlayOnly.inputsMouse = false;
        overlayOnly.captureWindow = false;
        overlayOnly.settings = false;
        overlayOnly.appearance = false;
        Expect(UpdateProfileMetadata("OverlayOnly", "OverlayOnly", overlayColor, overlayOnly),
            "Overlay-only profile should support selective overlay overrides.");

        Expect(g_sharedConfig.mirrors.size() == 1 && g_sharedConfig.mirrors[0].name == "MirrorSharedB",
            "Mirrors excluded from the active profile should remain in the shared config.");
        Expect(g_sharedConfig.windowOverlays.size() == 1 && g_sharedConfig.windowOverlays[0].name == "SharedOverlayA",
            "Window overlays still owned by the active profile should keep their shared fallback values.");
        Expect(!g_sharedConfig.ninjabrainOverlay.enabled,
            "Shared Ninjabrain overlay settings should remain untouched when the active profile owns them.");

        SwitchProfile(kDefaultProfileName);
        Expect(g_config.mirrors.size() == 1 && g_config.mirrors[0].name == "MirrorA",
            "Switching back to the mirrors-only profile should restore its mirror override.");
        Expect(g_config.windowOverlays.size() == 1 && g_config.windowOverlays[0].name == "SharedOverlayA",
            "Switching back to the mirrors-only profile should keep shared window overlays.");
        Expect(!g_config.ninjabrainOverlay.enabled,
            "Switching back to the mirrors-only profile should keep shared Ninjabrain overlay settings.");

        SwitchProfile("OverlayOnly");
        Expect(g_config.mirrors.size() == 1 && g_config.mirrors[0].name == "MirrorSharedB",
            "Switching to the overlay-only profile should fall back to shared mirrors.");
        Expect(g_config.windowOverlays.size() == 1 && g_config.windowOverlays[0].name == "OverlayB",
            "Switching to the overlay-only profile should apply its window overlay override.");
        Expect(g_config.ninjabrainOverlay.enabled, "Switching to the overlay-only profile should apply its Ninjabrain override.");
        Expect(g_config.ninjabrainOverlay.apiBaseUrl == "http://127.0.0.1:60000",
            "Switching to the overlay-only profile should restore its Ninjabrain overlay settings.");

        auto loadProfileSnapshot = [](const std::string& profileName) {
            Config snapshot;
            const std::wstring profilePath = g_toolscreenPath + L"\\profiles\\" + Utf8ToWide(profileName) + L".toml";
            Expect(LoadConfigFromTomlFile(profilePath, snapshot),
                "Profile TOML should remain loadable for '" + profileName + "'.");
            return snapshot;
        };

        Config defaultProfileSnapshot = loadProfileSnapshot(kDefaultProfileName);
        Expect(defaultProfileSnapshot.modes.empty(),
            "Mirrors-only profile TOML should not persist mode overrides.");
        Expect(defaultProfileSnapshot.mirrors.size() == 1 && defaultProfileSnapshot.mirrors[0].name == "MirrorA",
            "Mirrors-only profile TOML should persist its mirror override.");
        Expect(defaultProfileSnapshot.windowOverlays.empty(),
            "Mirrors-only profile TOML should not persist shared window overlays.");

        Config overlayProfileSnapshot = loadProfileSnapshot("OverlayOnly");
        Expect(overlayProfileSnapshot.mirrors.empty(),
            "Overlay-only profile TOML should not persist shared mirrors.");
        Expect(overlayProfileSnapshot.windowOverlays.size() == 1 && overlayProfileSnapshot.windowOverlays[0].name == "OverlayB",
            "Overlay-only profile TOML should persist its window overlay override.");
        Expect(overlayProfileSnapshot.ninjabrainOverlay.enabled,
            "Overlay-only profile TOML should persist its Ninjabrain overlay override.");
        Expect(overlayProfileSnapshot.ninjabrainOverlay.apiBaseUrl == "http://127.0.0.1:60000",
            "Overlay-only profile TOML should persist its Ninjabrain overlay settings.");

        SaveConfigImmediate();

        Config diskConfig;
        Expect(LoadConfigFromTomlFile(GetCurrentConfigPath().wstring(), diskConfig), "Shared config TOML should remain loadable.");
        Expect(diskConfig.mirrors.size() == 1 && diskConfig.mirrors[0].name == "MirrorSharedB",
            "config.toml should persist shared mirror values rather than the active profile override.");
        Expect(diskConfig.windowOverlays.size() == 1 && diskConfig.windowOverlays[0].name == "SharedOverlayA",
            "config.toml should persist shared window overlay values.");
        Expect(!diskConfig.ninjabrainOverlay.enabled,
            "config.toml should persist shared Ninjabrain overlay values.");

        ReloadConfigFromDisk();
        Expect(g_profilesConfig.activeProfile == "OverlayOnly", "Reload should preserve the active profile.");
        Expect(g_config.mirrors.size() == 1 && g_config.mirrors[0].name == "MirrorSharedB",
            "Reload should keep shared mirrors visible in the overlay-only profile.");
        Expect(g_config.windowOverlays.size() == 1 && g_config.windowOverlays[0].name == "OverlayB",
            "Reload should restore the overlay-only profile window overlay override.");
        Expect(g_config.ninjabrainOverlay.enabled,
            "Reload should restore the overlay-only profile Ninjabrain overlay override.");

        SwitchProfile(kDefaultProfileName);
        Expect(g_config.mirrors.size() == 1 && g_config.mirrors[0].name == "MirrorA",
            "Switching after reload should still restore the mirrors-only profile override.");
        Expect(g_config.windowOverlays.size() == 1 && g_config.windowOverlays[0].name == "SharedOverlayA",
            "Switching after reload should still use shared window overlays for the mirrors-only profile.");
        Expect(!g_config.ninjabrainOverlay.enabled,
            "Switching after reload should still use shared Ninjabrain overlay settings for the mirrors-only profile.");
    }

void RunProfileCaseInsensitiveCollisionTest(TestRunMode runMode = TestRunMode::Automated) {
    (void)runMode;
    ResetProfileTestState("profile_case_insensitive_collisions");

    Expect(!CreateNewProfile("default"), "CreateNewProfile should reject names that collide by case with an existing profile.");
    Expect(CreateNewProfile("Alpha"), "CreateNewProfile should allow a new unique profile.");
    Expect(!DuplicateProfile(kDefaultProfileName, "alpha"), "DuplicateProfile should reject case-insensitive destination collisions.");
    Expect(!RenameProfile(kDefaultProfileName, "ALPHA"), "RenameProfile should reject case-insensitive collisions with another profile.");
}

void RunProfileRecoverMissingMetadataTest(TestRunMode runMode = TestRunMode::Automated) {
    (void)runMode;
    ResetProfileTestState("profile_recover_missing_metadata");

    Expect(CreateNewProfile("Second"), "CreateNewProfile should succeed before metadata recovery test.");
    const std::filesystem::path metadataPath = GetProfilesMetadataPathForTests();
    Expect(std::filesystem::exists(metadataPath), "profiles.toml should exist before removal.");
    std::filesystem::remove(metadataPath);
    Expect(!std::filesystem::exists(metadataPath), "profiles.toml should be removed before recovery load.");

    g_profilesConfig = ProfilesConfig();
    LoadConfig();
    ExpectConfigLoadSucceeded("profile_recover_missing_metadata reload");

    Expect(std::filesystem::exists(metadataPath), "LoadConfig should rebuild missing profiles metadata.");
    Expect(g_profilesConfig.profiles.size() == 2, "Recovered metadata should include both on-disk profiles.");

    bool foundDefault = false;
    bool foundSecond = false;
    for (const auto& pm : g_profilesConfig.profiles) {
        if (pm.name == kDefaultProfileName) foundDefault = true;
        if (pm.name == "Second") foundSecond = true;
    }
    Expect(foundDefault, "Recovered metadata should include the Default profile.");
    Expect(foundSecond, "Recovered metadata should include the additional profile.");
    Expect(g_profilesConfig.activeProfile == kDefaultProfileName, "Recovered metadata should fall back to the Default active profile.");
}

void RunProfileAsyncSaveSkipDeletedProfileTest(TestRunMode runMode = TestRunMode::Automated) {
    (void)runMode;
    ResetProfileTestState("profile_async_save_skip_deleted_profile");

    Expect(CreateNewProfile("Other"), "CreateNewProfile should create the fallback profile.");

    g_config.defaultMode = "QueuedAsyncSave";
    g_configIsDirty = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    SaveConfig();

    SwitchProfile("Other");
    DeleteProfile(kDefaultProfileName);
    Expect(WaitForConfigSaveIdle(3000), "Background save should finish within the timeout.");

    const std::vector<std::string> profileFiles = ListDirectoryFileNamesSorted(GetProfilesDirectoryForTests());
    Expect(!ContainsFileNameIgnoreCase(profileFiles, "Default.toml"), "Async save should not recreate a deleted profile file.");

    Expect(LoadProfilesConfig(), "Profiles metadata should remain loadable after async save/delete interplay.");
    Expect(g_profilesConfig.profiles.size() == 1, "Only the surviving profile should remain in metadata after delete.");
    Expect(g_profilesConfig.profiles[0].name == "Other", "The remaining profile should be the fallback profile.");
}

void RunProfileSwitchNinjabrainAsyncStopTest(TestRunMode runMode = TestRunMode::Automated) {
    (void)runMode;
    ResetProfileTestState("profile_switch_ninjabrain_async_stop");

    Expect(CreateNewProfile("EnabledAsyncStop"), "CreateNewProfile should create the async-stop profile.");
    Expect(CreateNewProfile("DisabledAsyncStop"), "CreateNewProfile should create the disabled async-stop profile.");

    Config disabledSnapshot = g_config;
    disabledSnapshot.ninjabrainOverlay.enabled = false;
    disabledSnapshot.ninjabrainOverlay.apiBaseUrl = "http://127.0.0.1:52533";

    Config enabledSnapshot = disabledSnapshot;
    enabledSnapshot.ninjabrainOverlay.enabled = true;
    enabledSnapshot.ninjabrainOverlay.apiBaseUrl = "http://127.0.0.1:60000";

    Expect(SaveProfileSnapshot("EnabledAsyncStop", enabledSnapshot),
           "Saving the enabled Ninjabrain profile snapshot should succeed.");
    Expect(SaveProfileSnapshot("DisabledAsyncStop", disabledSnapshot),
           "Saving the disabled Ninjabrain profile snapshot should succeed.");

    g_config = enabledSnapshot;
    g_profilesConfig.activeProfile = "EnabledAsyncStop";
    PublishGuiConfigSnapshot();
    PublishCurrentModeForTests(g_config.defaultMode);

    StartNinjabrainClient();
    const NinjabrainApiStatus beforeStatus = GetNinjabrainClientStatus();
    Expect(beforeStatus.connectionState != NinjabrainApiConnectionState::Stopped,
           "The enabled profile should start the Ninjabrain client before switching away.");
    Expect(beforeStatus.apiBaseUrl == "http://127.0.0.1:60000",
           "The enabled profile should configure the expected Ninjabrain API URL before switching away.");

    SwitchProfile("DisabledAsyncStop");

    const NinjabrainApiStatus afterStatus = GetNinjabrainClientStatus();
    Expect(!g_config.ninjabrainOverlay.enabled,
           "Switching to the disabled profile should disable the Ninjabrain overlay.");
    Expect(afterStatus.connectionState == NinjabrainApiConnectionState::Stopped,
           "Switching to the disabled profile should stop the Ninjabrain client immediately.");
}

void RunProfileSwitchNinjabrainAsyncRestartTest(TestRunMode runMode = TestRunMode::Automated) {
    (void)runMode;
    ResetProfileTestState("profile_switch_ninjabrain_async_restart");

    Expect(CreateNewProfile("AsyncUrlA"), "CreateNewProfile should create the first Ninjabrain URL profile.");
    Expect(CreateNewProfile("AsyncUrlB"), "CreateNewProfile should create the second Ninjabrain URL profile.");

    Config baseSnapshot = g_config;
    baseSnapshot.ninjabrainOverlay.enabled = true;

    Config firstUrlSnapshot = baseSnapshot;
    firstUrlSnapshot.ninjabrainOverlay.apiBaseUrl = "http://127.0.0.1:60000";

    Config secondUrlSnapshot = baseSnapshot;
    secondUrlSnapshot.ninjabrainOverlay.apiBaseUrl = "http://127.0.0.1:60001";

    Expect(SaveProfileSnapshot("AsyncUrlA", firstUrlSnapshot),
           "Saving the first enabled Ninjabrain URL profile snapshot should succeed.");
    Expect(SaveProfileSnapshot("AsyncUrlB", secondUrlSnapshot),
           "Saving the second enabled Ninjabrain URL profile snapshot should succeed.");

    g_config = firstUrlSnapshot;
    g_profilesConfig.activeProfile = "AsyncUrlA";
    PublishGuiConfigSnapshot();
    PublishCurrentModeForTests(g_config.defaultMode);

    StartNinjabrainClient();
    const NinjabrainApiStatus beforeStatus = GetNinjabrainClientStatus();
    Expect(beforeStatus.connectionState != NinjabrainApiConnectionState::Stopped,
           "The first enabled profile should start the Ninjabrain client before the restart switch.");
    Expect(beforeStatus.apiBaseUrl == "http://127.0.0.1:60000",
           "The first enabled profile should configure the initial Ninjabrain API URL.");

    SwitchProfile("AsyncUrlB");

    const NinjabrainApiStatus afterStatus = GetNinjabrainClientStatus();
    Expect(g_config.ninjabrainOverlay.enabled,
           "Switching between enabled profiles should keep the Ninjabrain overlay enabled.");
    Expect(g_config.ninjabrainOverlay.apiBaseUrl == "http://127.0.0.1:60001",
           "Switching between enabled profiles should apply the destination Ninjabrain API URL.");
    Expect(afterStatus.connectionState != NinjabrainApiConnectionState::Stopped,
           "Switching between enabled profiles should leave the Ninjabrain client running.");
    Expect(afterStatus.apiBaseUrl == "http://127.0.0.1:60001",
           "Switching between enabled profiles should restart the client with the destination API URL.");

    g_config.ninjabrainOverlay.enabled = false;
    PublishGuiConfigSnapshot();
    StopNinjabrainClientAsync();
    std::this_thread::sleep_for(std::chrono::milliseconds(6500));
    StopNinjabrainClient();
}

void RunProfileSwitchInvalidDefaultModeFallbackTest(TestRunMode runMode = TestRunMode::Automated) {
    (void)runMode;
    ResetProfileTestState("profile_switch_invalid_default_mode_fallback");

    Expect(CreateNewProfile("ProfileA"), "CreateNewProfile should create the custom profile.");

    const Config defaultSnapshot = MakeSingleModeProfileSnapshot("Fullscreen", 1600, 900);
    Config invalidProfileSnapshot = MakeSingleModeProfileSnapshot("ProfileAOnly", 1111, 777, "MissingMode");

    Expect(SaveProfileSnapshot(kDefaultProfileName, defaultSnapshot), "Saving the default single-mode profile snapshot should succeed.");
    Expect(SaveProfileSnapshot("ProfileA", invalidProfileSnapshot), "Saving the invalid-default profile snapshot should succeed.");

    SwitchProfile("ProfileA");
    Expect(g_config.defaultMode == "MissingMode",
           "SwitchProfile should preserve the profile snapshot even when its stored default mode is missing.");
    Expect(GetPublishedCurrentModeId() == "MissingMode",
           "SwitchProfile should keep publishing the profile's stored mode ID even when readers need a fallback.");
    ExpectPublishedViewportMatchesMode("ProfileAOnly", "profile-switch-invalid-default-mode-fallback initial switch");

    SwitchProfile(kDefaultProfileName);
    Expect(g_config.defaultMode == "Fullscreen", "Switching back to Default should restore the Default profile mode.");
    ExpectPublishedViewportMatchesMode("Fullscreen", "profile-switch-invalid-default-mode-fallback default switch");

    SwitchProfile("ProfileA");
    Expect(g_config.defaultMode == "MissingMode",
           "Switching back to the invalid-default profile should still preserve its stored default mode.");
    Expect(GetPublishedCurrentModeId() == "MissingMode",
           "Switching back to the invalid-default profile should keep publishing the stored stale mode ID.");
    ExpectPublishedViewportMatchesMode("ProfileAOnly", "profile-switch-invalid-default-mode-fallback repeat switch");
}

void RunProfileSwitchReaderModeFallbackTest(TestRunMode runMode = TestRunMode::Automated) {
    (void)runMode;
    ResetProfileTestState("profile_switch_reader_mode_fallback");

    Expect(CreateNewProfile("ProfileA"), "CreateNewProfile should create the custom profile.");

    const Config defaultSnapshot = MakeSingleModeProfileSnapshot("Fullscreen", 1600, 900);
    const Config profileASnapshot = MakeSingleModeProfileSnapshot("ProfileAOnly", 1111, 777);

    Expect(SaveProfileSnapshot(kDefaultProfileName, defaultSnapshot), "Saving the default single-mode profile snapshot should succeed.");
    Expect(SaveProfileSnapshot("ProfileA", profileASnapshot), "Saving the custom single-mode profile snapshot should succeed.");

    SwitchProfile("ProfileA");
    SwitchProfile(kDefaultProfileName);
    SwitchProfile("ProfileA");

    Expect(g_config.defaultMode == "ProfileAOnly", "Switching back to ProfileA should restore its default mode.");
    Expect(GetPublishedCurrentModeId() == "ProfileAOnly", "Switching back to ProfileA should publish its default mode.");
    ExpectPublishedViewportMatchesMode("ProfileAOnly", "profile-switch-reader-mode-fallback steady state");

    PublishCurrentModeForTests("Fullscreen");

    auto cfgSnap = GetConfigSnapshot();
    Expect(cfgSnap != nullptr, "profile-switch-reader-mode-fallback should have a published config snapshot after switching.");
    std::string resolvedModeId;
    const ModeConfig* resolvedMode = GetModeFromSnapshotOrFallback(*cfgSnap, "Fullscreen", &resolvedModeId);
    Expect(resolvedMode != nullptr, "Reader mode fallback should resolve a mode from the active profile snapshot.");
    Expect(resolvedModeId == "ProfileAOnly",
           "Reader mode fallback should resolve the active profile's default mode when the published mode ID is stale.");

    ExpectPublishedViewportMatchesMode("ProfileAOnly", "profile-switch-reader-mode-fallback stale mode publish");

    PublishCurrentModeForTests("ProfileAOnly");
    Expect(GetPublishedCurrentModeId() == "ProfileAOnly", "The test should restore the published mode ID after the fallback assertion.");
}

void RunProfileSwitchConcurrentReadersTest(TestRunMode runMode = TestRunMode::Automated) {
    (void)runMode;
    ResetProfileTestState("profile_switch_concurrent_readers");

    Expect(CreateNewProfile("Second"), "CreateNewProfile should create the second profile.");
    Expect(CreateNewProfile("Third"), "CreateNewProfile should create the third profile.");

    const std::filesystem::path profilesDir = GetProfilesDirectoryForTests();
    const std::filesystem::path metadataPath = GetProfilesMetadataPathForTests();
    std::atomic<bool> stopReaders{ false };
    std::atomic<int> readFailures{ 0 };

    auto reader = [&]() {
        while (!stopReaders.load(std::memory_order_acquire)) {
            try {
                if (std::filesystem::exists(metadataPath)) {
                    std::ifstream metadataIn(metadataPath, std::ios::binary);
                    if (metadataIn.is_open()) {
                        auto tbl = toml::parse(metadataIn);
                        (void)tbl;
                    }
                }

                for (const char* name : { kDefaultProfileName, "Second", "Third" }) {
                    const std::filesystem::path profilePath = profilesDir / (std::string(name) + ".toml");
                    if (!std::filesystem::exists(profilePath)) {
                        continue;
                    }

                    std::ifstream profileIn(profilePath, std::ios::binary);
                    if (!profileIn.is_open()) {
                        continue;
                    }

                    auto tbl = toml::parse(profileIn);
                    (void)tbl;
                }
            } catch (...) {
                readFailures.fetch_add(1, std::memory_order_relaxed);
            }

            std::this_thread::yield();
        }
    };

    std::vector<std::thread> readers;
    for (int i = 0; i < 3; ++i) {
        readers.emplace_back(reader);
    }

    const std::array<std::string, 3> switchOrder = { kDefaultProfileName, "Second", "Third" };
    for (int i = 0; i < 24; ++i) {
        SwitchProfile(switchOrder[i % static_cast<int>(switchOrder.size())]);
    }

    stopReaders.store(true, std::memory_order_release);
    for (auto& readerThread : readers) {
        readerThread.join();
    }

    Expect(readFailures.load(std::memory_order_acquire) == 0,
        "Concurrent profile readers should not observe malformed TOML while switching profiles.");
    Expect(LoadProfilesConfig(), "Profiles metadata should remain loadable after concurrent switching.");
    Expect(g_profilesConfig.profiles.size() == 3, "All profiles should still be present after concurrent switching.");
    Expect(g_profilesConfig.activeProfile == "Third", "The final active profile should match the last switch target.");
}

void RunProfileSwitchConcurrentLifecycleTest(TestRunMode runMode = TestRunMode::Automated) {
    (void)runMode;
    ResetProfileTestState("profile_switch_concurrent_lifecycle");

    Expect(CreateNewProfile("Second"), "CreateNewProfile should create the second profile.");
    Expect(CreateNewProfile("Third"), "CreateNewProfile should create the third profile.");
    Expect(CreateNewProfile("ScratchSource"), "CreateNewProfile should create the scratch source profile.");

    std::atomic<bool> stopWorkers{ false };
    std::atomic<int> failureCount{ 0 };
    std::mutex failureMutex;
    std::string failureMessage;

    auto recordFailure = [&](const std::string& message) {
        if (failureCount.fetch_add(1, std::memory_order_relaxed) == 0) {
            std::lock_guard<std::mutex> lock(failureMutex);
            failureMessage = message;
        }
        stopWorkers.store(true, std::memory_order_release);
    };

    auto switcher = [&]() {
        try {
            const std::array<std::string, 3> switchOrder = { kDefaultProfileName, "Second", "Third" };
            for (int i = 0; i < 48 && !stopWorkers.load(std::memory_order_acquire); ++i) {
                SwitchProfile(switchOrder[i % static_cast<int>(switchOrder.size())]);
                std::this_thread::yield();
            }
        } catch (const std::exception& e) {
            recordFailure(std::string("Switch thread failed: ") + e.what());
        } catch (...) {
            recordFailure("Switch thread failed with an unknown exception.");
        }
    };

    auto lifecycleWorker = [&]() {
        try {
            for (int i = 0; i < 12 && !stopWorkers.load(std::memory_order_acquire); ++i) {
                const std::string duplicateName = "Scratch Copy " + std::to_string(i);
                const std::string renamedName = "Scratch Temp " + std::to_string(i);
                if (!DuplicateProfile("ScratchSource", duplicateName)) {
                    throw std::runtime_error("DuplicateProfile should succeed for lifecycle stress copy '" + duplicateName + "'.");
                }

                const float color[3] = {
                    0.1f + (0.02f * static_cast<float>(i)),
                    0.2f + (0.01f * static_cast<float>(i)),
                    0.3f + (0.015f * static_cast<float>(i)),
                };
                if (!UpdateProfileMetadata(duplicateName, renamedName, color, ProfileSectionSelection{})) {
                    throw std::runtime_error("UpdateProfileMetadata should succeed for lifecycle stress rename '" + renamedName + "'.");
                }

                DeleteProfile(renamedName);

                const std::vector<std::string> profileFiles = ListDirectoryFileNamesSorted(GetProfilesDirectoryForTests());
                if (ContainsFileNameIgnoreCase(profileFiles, renamedName + ".toml")) {
                    throw std::runtime_error("Deleted scratch lifecycle profile file should not remain on disk.");
                }

                std::this_thread::yield();
            }
        } catch (const std::exception& e) {
            recordFailure(std::string("Lifecycle worker failed: ") + e.what());
        } catch (...) {
            recordFailure("Lifecycle worker failed with an unknown exception.");
        }
    };

    std::thread switchThread(switcher);
    std::thread lifecycleThread(lifecycleWorker);
    switchThread.join();
    lifecycleThread.join();

    Expect(failureCount.load(std::memory_order_acquire) == 0,
           "Concurrent profile lifecycle operations should not fail while switching profiles." +
               (failureMessage.empty() ? std::string() : (" First failure: " + failureMessage)));
    ExpectProfilesMetadataMatchesDisk({ kDefaultProfileName, "Second", "Third", "ScratchSource" },
                                      "profile-switch-concurrent-lifecycle");
    Expect(EqualsIgnoreCase(g_profilesConfig.activeProfile, "Third"),
           "The final active profile should match the last switch target after lifecycle stress.");
}

void RunProfileSwitchConcurrentMetadataRebuildTest(TestRunMode runMode = TestRunMode::Automated) {
    (void)runMode;
    ResetProfileTestState("profile_switch_concurrent_metadata_rebuild");

    Expect(CreateNewProfile("Second"), "CreateNewProfile should create the second profile.");
    Expect(CreateNewProfile("Third"), "CreateNewProfile should create the third profile.");
    Expect(CreateNewProfile("Fourth"), "CreateNewProfile should create the fourth profile.");

    std::atomic<bool> stopWorkers{ false };
    std::atomic<int> failureCount{ 0 };
    std::mutex failureMutex;
    std::string failureMessage;

    auto recordFailure = [&](const std::string& message) {
        if (failureCount.fetch_add(1, std::memory_order_relaxed) == 0) {
            std::lock_guard<std::mutex> lock(failureMutex);
            failureMessage = message;
        }
        stopWorkers.store(true, std::memory_order_release);
    };

    auto switcher = [&]() {
        try {
            const std::array<std::string, 3> switchOrder = { kDefaultProfileName, "Second", "Third" };
            for (int i = 0; i < 48 && !stopWorkers.load(std::memory_order_acquire); ++i) {
                SwitchProfile(switchOrder[i % static_cast<int>(switchOrder.size())]);
                std::this_thread::yield();
            }
        } catch (const std::exception& e) {
            recordFailure(std::string("Switch thread failed: ") + e.what());
        } catch (...) {
            recordFailure("Switch thread failed with an unknown exception.");
        }
    };

    auto rebuildWorker = [&]() {
        try {
            const std::filesystem::path metadataPath = GetProfilesMetadataPathForTests();
            for (int i = 0; i < 16 && !stopWorkers.load(std::memory_order_acquire); ++i) {
                std::error_code removeError;
                std::filesystem::remove(metadataPath, removeError);

                if (!EnsureProfilesConfigReady()) {
                    throw std::runtime_error("EnsureProfilesConfigReady should rebuild metadata from on-disk profiles.");
                }
                if (!LoadProfilesConfig()) {
                    throw std::runtime_error("LoadProfilesConfig should succeed after metadata rebuild.");
                }

                std::this_thread::yield();
            }
        } catch (const std::exception& e) {
            recordFailure(std::string("Metadata rebuild worker failed: ") + e.what());
        } catch (...) {
            recordFailure("Metadata rebuild worker failed with an unknown exception.");
        }
    };

    std::thread switchThread(switcher);
    std::thread rebuildThread(rebuildWorker);
    switchThread.join();
    rebuildThread.join();

    Expect(failureCount.load(std::memory_order_acquire) == 0,
           "Concurrent metadata rebuilds should not fail while switching profiles." +
               (failureMessage.empty() ? std::string() : (" First failure: " + failureMessage)));
    ExpectProfilesMetadataMatchesDisk({ kDefaultProfileName, "Second", "Third", "Fourth" },
                                      "profile-switch-concurrent-metadata-rebuild");
    Expect(EqualsIgnoreCase(g_profilesConfig.activeProfile, "Third"),
           "The final active profile should match the last switch target after metadata rebuild stress.");
}

void RunProfileSwitchConcurrentSnapshotWritesTest(TestRunMode runMode = TestRunMode::Automated) {
    (void)runMode;
    ResetProfileTestState("profile_switch_concurrent_snapshot_writes");

    Expect(CreateNewProfile("Second"), "CreateNewProfile should create the second profile.");
    Expect(CreateNewProfile("Third"), "CreateNewProfile should create the third profile.");
    Expect(CreateNewProfile("Snapshot Target"), "CreateNewProfile should create the snapshot target profile.");

    std::atomic<bool> stopWorkers{ false };
    std::atomic<int> failureCount{ 0 };
    std::mutex failureMutex;
    std::string failureMessage;

    auto recordFailure = [&](const std::string& message) {
        if (failureCount.fetch_add(1, std::memory_order_relaxed) == 0) {
            std::lock_guard<std::mutex> lock(failureMutex);
            failureMessage = message;
        }
        stopWorkers.store(true, std::memory_order_release);
    };

    auto switcher = [&]() {
        try {
            const std::array<std::string, 3> switchOrder = { kDefaultProfileName, "Second", "Third" };
            for (int i = 0; i < 48 && !stopWorkers.load(std::memory_order_acquire); ++i) {
                SwitchProfile(switchOrder[i % static_cast<int>(switchOrder.size())]);
                std::this_thread::yield();
            }
        } catch (const std::exception& e) {
            recordFailure(std::string("Switch thread failed: ") + e.what());
        } catch (...) {
            recordFailure("Switch thread failed with an unknown exception.");
        }
    };

    constexpr int kSnapshotWriteCount = 18;
    auto snapshotWriter = [&]() {
        try {
            for (int i = 0; i < kSnapshotWriteCount && !stopWorkers.load(std::memory_order_acquire); ++i) {
                Config snapshot;
                if (!LoadEmbeddedDefaultConfig(snapshot)) {
                    throw std::runtime_error("LoadEmbeddedDefaultConfig should succeed for snapshot stress fixtures.");
                }

                snapshot.configVersion = GetConfigVersion();
                snapshot.defaultMode = "Fullscreen";
                snapshot.mouseSensitivity = 1.25f + (0.125f * static_cast<float>(i));
                snapshot.windowsMouseSpeed = 6 + (i % 5);
                snapshot.autoBorderless = (i % 2) == 0;
                snapshot.hideAnimationsInGame = (i % 3) == 0;

                if (!SaveProfileSnapshotIfTracked("Snapshot Target", snapshot)) {
                    throw std::runtime_error("SaveProfileSnapshotIfTracked should keep writing the tracked inactive profile.");
                }

                std::this_thread::yield();
            }
        } catch (const std::exception& e) {
            recordFailure(std::string("Snapshot writer failed: ") + e.what());
        } catch (...) {
            recordFailure("Snapshot writer failed with an unknown exception.");
        }
    };

    std::thread switchThread(switcher);
    std::thread snapshotThread(snapshotWriter);
    switchThread.join();
    snapshotThread.join();

    Expect(failureCount.load(std::memory_order_acquire) == 0,
           "Concurrent snapshot writes should not fail while switching profiles." +
               (failureMessage.empty() ? std::string() : (" First failure: " + failureMessage)));
    ExpectProfilesMetadataMatchesDisk({ kDefaultProfileName, "Second", "Third", "Snapshot Target" },
                                      "profile-switch-concurrent-snapshot-writes");
    Expect(EqualsIgnoreCase(g_profilesConfig.activeProfile, "Third"),
           "The final active profile should match the last switch target after snapshot stress.");

    const Config snapshotTarget = LoadProfileConfigForTests("Snapshot Target");
    const float expectedSensitivity = 1.25f + (0.125f * static_cast<float>(kSnapshotWriteCount - 1));
    Expect(std::abs(snapshotTarget.mouseSensitivity - expectedSensitivity) < 0.0001f,
           "The snapshot target should retain the last concurrent snapshot's mouse sensitivity.");
    Expect(snapshotTarget.windowsMouseSpeed == 6 + ((kSnapshotWriteCount - 1) % 5),
           "The snapshot target should retain the last concurrent snapshot's Windows mouse speed.");
    Expect(snapshotTarget.autoBorderless == (((kSnapshotWriteCount - 1) % 2) == 0),
           "The snapshot target should retain the last concurrent snapshot's autoBorderless flag.");
    Expect(snapshotTarget.hideAnimationsInGame == (((kSnapshotWriteCount - 1) % 3) == 0),
           "The snapshot target should retain the last concurrent snapshot's hideAnimationsInGame flag.");
    Expect(snapshotTarget.configVersion == GetConfigVersion(),
           "Concurrent snapshot writes should persist the current config version.");
}

void RunLogMultiInstanceLatestSuffixTest(TestRunMode runMode = TestRunMode::Automated) {
    (void)runMode;
    ProcessQueuedArchivedLogCompressions();

    const std::filesystem::path root = PrepareCaseDirectory("log_multi_instance_latest_suffix");
    ResetGlobalTestState(root);
    const std::filesystem::path logsDirectory = root / "logs";

    ScopedLogSessionClaim first(logsDirectory);
    ScopedLogSessionClaim second(logsDirectory);
    ScopedLogSessionClaim third(logsDirectory);

    Expect(std::filesystem::path(first.session().logFilePath).filename() == "latest.log",
           "Expected the first claimed session to use latest.log.");
    Expect(std::filesystem::path(second.session().logFilePath).filename() == "latest-1.log",
           "Expected the second claimed session to use latest-1.log.");
    Expect(std::filesystem::path(third.session().logFilePath).filename() == "latest-2.log",
           "Expected the third claimed session to use latest-2.log.");

    Expect(first.session().otherOpenInstances.empty(),
           "Expected the first claimed session to observe no earlier open instances.");
    Expect(second.session().otherOpenInstances.size() == 1,
           "Expected the second claimed session to observe the first open instance.");
    Expect(third.session().otherOpenInstances.size() == 2,
           "Expected the third claimed session to observe the two earlier open instances.");

    std::vector<std::string> thirdPeerNames;
    for (const LogInstanceInfo& peer : third.session().otherOpenInstances) {
        thirdPeerNames.push_back(std::filesystem::path(peer.logFilePath).filename().string());
    }
    std::sort(thirdPeerNames.begin(), thirdPeerNames.end());
    ExpectVectorEquals(thirdPeerNames, std::vector<std::string>{ "latest-1.log", "latest.log" },
                       "Expected the third claimed session to list both lower-numbered latest logs.");
}

void RunLogHeaderIncludesPeerInfoTest(TestRunMode runMode = TestRunMode::Automated) {
    (void)runMode;
    ProcessQueuedArchivedLogCompressions();

    const std::filesystem::path root = PrepareCaseDirectory("log_header_includes_peer_info");
    ResetGlobalTestState(root);
    const std::filesystem::path logsDirectory = root / "logs";

    ScopedLogSessionClaim first(logsDirectory);
    WriteTextFileUtf8(std::filesystem::path(first.session().logFilePath), BuildLogSessionHeader(first.session()));

    ScopedLogSessionClaim second(logsDirectory);
    WriteTextFileUtf8(std::filesystem::path(second.session().logFilePath), BuildLogSessionHeader(second.session()));

    const std::string firstHeader = ReadTextFileUtf8(std::filesystem::path(first.session().logFilePath));
    const std::string secondHeader = ReadTextFileUtf8(std::filesystem::path(second.session().logFilePath));

    Expect(firstHeader.find("Other open instances at startup: 0") != std::string::npos,
           "Expected the first log header to report zero other open instances.");
    Expect(secondHeader.find("Other open instances at startup: 1") != std::string::npos,
           "Expected the second log header to report the earlier open instance.");
    Expect(secondHeader.find("latest.log") != std::string::npos,
           "Expected the second log header to include the other instance's latest.log path.");
    Expect(secondHeader.find("Process ID: " + std::to_string(second.session().processId)) != std::string::npos,
           "Expected the log header to include the current process ID.");
}

void RunLogArchiveCollisionHandlingTest(TestRunMode runMode = TestRunMode::Automated) {
    (void)runMode;
    ProcessQueuedArchivedLogCompressions();

    const std::filesystem::path root = PrepareCaseDirectory("log_archive_collision_handling");
    ResetGlobalTestState(root);
    const std::filesystem::path logsDirectory = root / "logs";
    std::error_code error;
    std::filesystem::create_directories(logsDirectory, error);
    Expect(!error, "Failed to create log fixture directory.");

    const std::filesystem::path orphanLatest = logsDirectory / "latest.log";
    WriteTextFileUtf8(orphanLatest, "orphan latest log");
    SetLocalFileLastWriteTime(orphanLatest, 2024, 1, 2, 3, 4, 5);

    const std::filesystem::path staleOwnedLatest = logsDirectory / "latest-1.log";
    WriteTextFileUtf8(staleOwnedLatest, "stale owned latest log");
    SetLocalFileLastWriteTime(staleOwnedLatest, 2024, 1, 2, 3, 4, 5);

    WriteTextFileUtf8(logsDirectory / "latest-1.log.owner",
                      "pid=999999\nprocessStartFileTime=1\nlogFile=" + Narrow(staleOwnedLatest.wstring()) + "\n");

    const std::filesystem::path existingArchive0 = logsDirectory / "20240102_030405.log.gz";
    const std::filesystem::path existingArchive1 = logsDirectory / "20240102_030405_1.log.gz";
    WriteTextFileUtf8(existingArchive0, "existing-archive-zero");
    WriteTextFileUtf8(existingArchive1, "existing-archive-one");

    ScopedLogSessionClaim claim(logsDirectory);
    ProcessQueuedArchivedLogCompressions();

    const std::vector<std::string> fileNames = ListDirectoryFileNamesSorted(logsDirectory);
    Expect(ContainsFileName(fileNames, "20240102_030405.log.gz"),
           "Expected the existing base archive gzip to remain present.");
    Expect(ContainsFileName(fileNames, "20240102_030405_1.log.gz"),
           "Expected the existing suffixed archive gzip to remain present.");
    Expect(ContainsFileName(fileNames, "20240102_030405_2.log.gz"),
           "Expected the first archived latest log to move to the next free gzip suffix.");
    Expect(ContainsFileName(fileNames, "20240102_030405_3.log.gz"),
           "Expected the second archived latest log to avoid both existing and newly queued names.");

    Expect(ReadTextFileUtf8(existingArchive0) == "existing-archive-zero",
           "Expected archive collision handling to avoid overwriting the existing base gzip.");
    Expect(ReadTextFileUtf8(existingArchive1) == "existing-archive-one",
           "Expected archive collision handling to avoid overwriting the existing suffixed gzip.");
    Expect(std::filesystem::path(claim.session().logFilePath).filename() == "latest.log",
           "Expected a new active claim to still reuse latest.log after archiving stale files.");
}

void RunLogReleasedSlotReuseTest(TestRunMode runMode = TestRunMode::Automated) {
    (void)runMode;
    ProcessQueuedArchivedLogCompressions();

    const std::filesystem::path root = PrepareCaseDirectory("log_released_slot_reuse");
    ResetGlobalTestState(root);
    const std::filesystem::path logsDirectory = root / "logs";

    ScopedLogSessionClaim first(logsDirectory);
    WriteTextFileUtf8(std::filesystem::path(first.session().logFilePath), "released instance log contents");
    SetLocalFileLastWriteTime(std::filesystem::path(first.session().logFilePath), 2024, 1, 3, 4, 5, 6);

    ScopedLogSessionClaim second(logsDirectory);
    WriteTextFileUtf8(std::filesystem::path(second.session().logFilePath), "still-active instance log contents");

    first.Release();

    ScopedLogSessionClaim third(logsDirectory);
    ProcessQueuedArchivedLogCompressions();

    Expect(std::filesystem::path(third.session().logFilePath).filename() == "latest.log",
           "Expected the next claimed session to reuse latest.log after the old owner released it.");
    Expect(third.session().otherOpenInstances.size() == 1,
           "Expected the reused latest.log claim to still observe the other active instance.");
    Expect(std::filesystem::path(third.session().otherOpenInstances.front().logFilePath).filename() == "latest-1.log",
           "Expected the surviving active instance to remain on latest-1.log.");

    const std::vector<std::string> fileNames = ListDirectoryFileNamesSorted(logsDirectory);
    Expect(ContainsFileName(fileNames, "20240103_040506.log.gz"),
           "Expected the released latest.log file to be archived and compressed before reuse.");
    Expect(ContainsFileName(fileNames, "latest-1.log.owner"),
           "Expected the other active instance to retain its owner file.");
    Expect(!ContainsFileName(fileNames, "latest-2.log.owner"),
           "Expected slot reuse to avoid allocating a higher-numbered latest log unnecessarily.");
}

void RunProfilerExitedThreadLifetimeTest(TestRunMode runMode = TestRunMode::Automated) {
    (void)runMode;

    const std::filesystem::path root = PrepareCaseDirectory("profiler_exited_thread_lifetime");
    ResetGlobalTestState(root);

    Profiler& profiler = Profiler::GetInstance();
    profiler.Clear();
    profiler.SetEnabled(true);
    profiler.StartProcessingThread();

    std::atomic<bool> releaseWorker{ false };
    std::thread worker([&] {
        while (!releaseWorker.load(std::memory_order_acquire)) {
            PROFILE_SCOPE("Short-lived worker scope");
            std::this_thread::yield();
        }
    });
    for (int iteration = 0; iteration < 20; ++iteration) {
        profiler.Clear();
        std::this_thread::yield();
    }
    releaseWorker.store(true, std::memory_order_release);
    worker.join();

    // Processing after the worker's thread-local storage is destroyed must
    // not dereference a stale registry pointer.
    profiler.EndFrame();
    profiler.EndFrame();

    profiler.StopProcessingThread();
    profiler.Clear();
    profiler.SetEnabled(false);
}

void RunNinjabrainExtremePredictionPayloadTest(TestRunMode runMode = TestRunMode::Automated) {
    (void)runMode;

    NinjabrainData data;
    ApplyNinjabrainStrongholdEvent(
        R"({"resultType":"TRIANGULATION","playerPosition":{"xInOverworld":0.0,"zInOverworld":0.0,"horizontalAngle":1e300},"predictions":[{"chunkX":2147483647,"chunkZ":-2147483648,"certainty":0.5,"overworldDistance":1.0}]})",
        data,
        {});

    Expect(data.validPrediction, "Expected the extreme prediction payload to remain parseable.");
    Expect(data.strongholdX == (std::numeric_limits<int>::max)(),
           "Expected an overflowing positive chunk coordinate to saturate.");
    Expect(data.strongholdZ == (std::numeric_limits<int>::min)(),
           "Expected an overflowing negative chunk coordinate to saturate.");
    Expect(data.predictionAngles[0].valid && std::isfinite(data.predictionAngles[0].neededCorrection),
           "Expected huge angles to normalize to a finite value without an unbounded loop.");
}
