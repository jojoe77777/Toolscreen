struct TestCaseDefinition {
    const char* name;
    void (*run)(TestRunMode runMode);
    bool interactiveOnly = false;
};

struct TestGroupDefinition {
    const char* name;
    std::vector<std::string_view> prefixes;
    std::vector<std::string_view> explicitTestCaseNames;
};

struct ResolvedTestGroupDefinition {
    const char* name;
    std::vector<const TestCaseDefinition*> testCases;
};

struct ParallelTestGroupResult {
    std::string groupName;
    DWORD exitCode = 1;
    std::string stdoutText;
    std::string stderrText;
    std::string failureMessage;
};

struct RunningParallelTestGroup {
    ParallelTestGroupResult result;
    HANDLE processHandle = nullptr;
    std::filesystem::path stdoutPath;
    std::filesystem::path stderrPath;
    std::chrono::steady_clock::time_point launchTime;
};

const auto& GetTestCaseDefinitions() {
    static const std::vector<TestCaseDefinition> testCases = {
        {"config-default-load", &RunConfigDefaultLoadTest},
        {"log-multi-instance-latest-suffix", &RunLogMultiInstanceLatestSuffixTest},
        {"log-header-includes-peer-info", &RunLogHeaderIncludesPeerInfoTest},
        {"log-archive-collision-handling", &RunLogArchiveCollisionHandlingTest},
        {"log-released-slot-reuse", &RunLogReleasedSlotReuseTest},
        {"config-roundtrip", &RunConfigRoundtripTest},
        {"config-roundtrip-global-settings", &RunConfigRoundtripGlobalSettingsTest},
        {"config-roundtrip-modes", &RunConfigRoundtripModesTest},
        {"config-roundtrip-mirrors", &RunConfigRoundtripMirrorsTest},
        {"config-roundtrip-mirror-groups", &RunConfigRoundtripMirrorGroupsTest},
        {"config-roundtrip-images", &RunConfigRoundtripImagesTest},
        {"config-roundtrip-window-overlays", &RunConfigRoundtripWindowOverlaysTest},
        {"config-roundtrip-browser-overlays", &RunConfigRoundtripBrowserOverlaysTest},
        {"config-roundtrip-hotkeys", &RunConfigRoundtripHotkeysTest},
        {"config-roundtrip-sensitivity-hotkeys", &RunConfigRoundtripSensitivityHotkeysTest},
        {"config-roundtrip-cursors-eyezoom", &RunConfigRoundtripCursorsAndEyeZoomTest},
        {"config-roundtrip-key-rebinds-appearance", &RunConfigRoundtripKeyRebindsAndAppearanceTest},
        {"config-roundtrip-debug-settings", &RunConfigRoundtripDebugSettingsTest},
        {"config-load-embedded-ninjabrain-presets", &RunConfigLoadEmbeddedNinjabrainPresetsTest},
        {"config-load-bundled-font-paths-normalized", &RunConfigLoadBundledFontPathsNormalizedTest},
        {"config-load-missing-required-modes", &RunConfigLoadMissingRequiredModesTest},
        {"config-load-invalid-hotkey-mode-references", &RunConfigLoadInvalidHotkeyModeReferencesTest},
        {"config-load-relative-mode-dimensions", &RunConfigLoadRelativeModeDimensionsTest},
        {"config-load-expression-mode-dimensions", &RunConfigLoadExpressionModeDimensionsTest},
        {"config-load-legacy-version-upgrade", &RunConfigLoadLegacyVersionUpgradeTest},
        {"config-load-clamp-global-values", &RunConfigLoadClampGlobalValuesTest},
        {"config-load-clamp-global-values-system-key-repeat", &RunConfigLoadClampGlobalValuesSystemKeyRepeatTest},
        {"config-load-mode-default-dimensions-restored", &RunConfigLoadModeDefaultDimensionsRestoredTest},
        {"config-load-mode-source-lists-loaded", &RunConfigLoadModeSourceListsLoadedTest},
        {"config-downgrade-sources-compat", &RunConfigDowngradeSourcesCompatTest},
        {"config-startup-indicator", &RunStartupIndicatorConfigTest},
        {"config-migrate-version-applies", &RunConfigMigrateVersionAppliesTest},
        {"config-load-mode-percentage-dimensions-detected", &RunConfigLoadModePercentageDimensionsDetectedTest},
        {"config-load-mode-typed-sources-loaded", &RunConfigLoadModeTypedSourcesLoadedTest},
        {"config-load-mode-source-helpers", &RunConfigModeSourceHelpersTest},
        {"config-load-empty-main-hotkey-fallback", &RunConfigLoadEmptyMainHotkeyFallbackTest},
        {"config-load-missing-gui-hotkey-defaulted", &RunConfigLoadMissingGuiHotkeyDefaultedTest},
        {"config-load-empty-gui-hotkey-defaulted", &RunConfigLoadEmptyGuiHotkeyDefaultedTest},
        {"config-load-legacy-mirror-gamma-migrated", &RunConfigLoadLegacyMirrorGammaMigratedTest},
        {"config-load-mirror-capture-dimensions-clamped", &RunConfigLoadMirrorCaptureDimensionsClampedTest},
        {"config-load-eyezoom-clone-width-normalized", &RunConfigLoadEyeZoomCloneWidthNormalizedTest},
        {"config-load-eyezoom-overlay-width-defaulted", &RunConfigLoadEyeZoomOverlayWidthDefaultedTest},
        {"config-load-eyezoom-overlay-width-clamped", &RunConfigLoadEyeZoomOverlayWidthClampedTest},
        {"config-load-eyezoom-legacy-margins-migrated", &RunConfigLoadEyeZoomLegacyMarginsMigratedTest},
        {"config-load-eyezoom-legacy-custom-position-migrated", &RunConfigLoadEyeZoomLegacyCustomPositionMigratedTest},
        {"config-load-eyezoom-invalid-active-overlay-reset", &RunConfigLoadEyeZoomInvalidActiveOverlayResetTest},
        {"config-load-window-overlay-crop-migrated", &RunConfigLoadWindowOverlayCropMigratedTest},
        {"config-load-window-overlay-capture-method-migrated", &RunConfigLoadWindowOverlayCaptureMethodMigratedTest},
        {"config-load-key-rebind-unicode-string-parsed", &RunConfigLoadKeyRebindUnicodeStringParsedTest},
        {"config-load-key-rebind-escaped-unicode-string-parsed", &RunConfigLoadKeyRebindEscapedUnicodeStringParsedTest},
        {"config-load-key-rebind-hex-unicode-string-parsed", &RunConfigLoadKeyRebindHexUnicodeStringParsedTest},
        {"config-load-key-rebind-invalid-unicode-defaulted", &RunConfigLoadKeyRebindInvalidUnicodeDefaultedTest},
        {"config-load-key-rebind-legacy-show-indicator-migrated", &RunConfigLoadKeyRebindLegacyShowIndicatorMigratedTest},
        {"config-load-key-rebind-cannot-type-clears-typed-output", &RunConfigLoadKeyRebindCannotTypeClearsTypedOutputTest},
        {"config-load-key-rebind-shift-layer-caps-lock-parsed", &RunConfigLoadKeyRebindShiftLayerCapsLockParsedTest},
        {"config-load-key-rebind-shift-layer-caps-lock-defaulted", &RunConfigLoadKeyRebindShiftLayerCapsLockDefaultedTest},
        {"config-load-key-rebind-cursor-state-defaulted", &RunConfigLoadKeyRebindCursorStateDefaultedTest},
        {"config-publish-key-rebind-cannot-type-clears-typed-output", &RunConfigPublishKeyRebindCannotTypeClearsTypedOutputTest},
        {"hotkey-runtime-specific-shift-release-matches-exact-keyup", &RunHotkeyRuntimeSpecificShiftReleaseMatchesExactKeyupTest},
        {"hotkey-runtime-exclusion-detects-low-level-suppressed-key", &RunHotkeyRuntimeExclusionDetectsLowLevelSuppressedKeyTest},
        {"hotkey-runtime-exclusion-detects-suppressed-ctrl-shift", &RunHotkeyRuntimeExclusionDetectsSuppressedCtrlShiftTest},
        {"key-rebind-deep-suppression-eligibility", &RunKeyRebindDeepSuppressionEligibilityTest},
        {"key-rebind-runtime-full-forwarding", &RunKeyRebindRuntimeFullForwardingTest},
        {"key-rebind-runtime-split-vk-output", &RunKeyRebindRuntimeSplitVkOutputTest},
        {"key-rebind-runtime-split-unicode-output", &RunKeyRebindRuntimeSplitUnicodeOutputTest},
        {"key-rebind-runtime-unicode-output-ignores-vk", &RunKeyRebindRuntimeUnicodeOutputIgnoresVkTest},
        {"key-rebind-runtime-shift-layer-unicode-output-ignores-vk", &RunKeyRebindRuntimeShiftLayerUnicodeOutputIgnoresVkTest},
        {"key-rebind-runtime-shift-layer-shift-activated", &RunKeyRebindRuntimeShiftLayerShiftActivatedTest},
        {"key-rebind-runtime-shift-layer-caps-lock-activated", &RunKeyRebindRuntimeShiftLayerCapsLockActivatedTest},
        {"key-rebind-runtime-full-caps-lock-honored", &RunKeyRebindRuntimeFullCapsLockHonoredTest},
        {"key-rebind-runtime-split-caps-lock-ignored", &RunKeyRebindRuntimeSplitCapsLockIgnoredWithoutOptInTest},
        {"key-rebind-runtime-non-typable-trigger-consumes-char", &RunKeyRebindRuntimeNonTypableTriggerConsumesCharTest},
        {"key-rebind-runtime-trigger-disabled-still-types", &RunKeyRebindRuntimeTriggerDisabledStillTypesTest},
        {"key-rebind-runtime-types-disabled-still-triggers", &RunKeyRebindRuntimeTypesDisabledStillTriggersTest},
        {"key-rebind-runtime-shift-types-disabled-still-triggers", &RunKeyRebindRuntimeShiftTypesDisabledStillTriggersTest},
        {"key-rebind-runtime-mouse-source-emits-key-and-char", &RunKeyRebindRuntimeMouseSourceEmitsKeyAndCharTest},
        {"key-rebind-runtime-plain-key-output-released-on-teardown", &RunKeyRebindRuntimePlainKeyOutputReleasedOnTeardownTest},
        {"key-rebind-runtime-passthrough-source-released-on-enable", &RunKeyRebindRuntimePassthroughSourceReleasedOnEnableTest},
        {"hotkey-runtime-exclusion-honored-on-trigger-on-release", &RunHotkeyRuntimeExclusionHonoredOnTriggerOnReleaseTest},
        {"key-rebind-runtime-modifier-output-released-on-deactivate", &RunKeyRebindRuntimeModifierOutputReleasedOnDeactivateTest},
        {"key-rebind-runtime-suppressed-caps-lock-released-on-deactivate", &RunKeyRebindRuntimeSuppressedCapsLockReleasedOnDeactivateTest},
        {"key-rebind-runtime-custom-modifier-output-uses-synthetic-key", &RunKeyRebindRuntimeCustomModifierOutputUsesSyntheticKeyTest},
        {"key-rebind-runtime-wndproc-keeps-synthetic-modifier-held", &RunKeyRebindRuntimeWndProcKeepsSyntheticModifierHeldTest},
        {"key-rebind-runtime-modifier-source-dual-shift-release", &RunKeyRebindRuntimeModifierSourceDualShiftReleaseTest},
        {"key-rebind-runtime-disabled-rebind-ignored", &RunKeyRebindRuntimeDisabledRebindIgnoredTest},
        {"key-rebind-runtime-cursor-state-keyup-passthrough", &RunKeyRebindRuntimeCursorStateKeyupPassthroughTest},
        {"key-rebind-runtime-held-output-released-on-rebind-change", &RunKeyRebindRuntimeHeldOutputReleasedOnRebindChangeTest},
        {"key-rebind-runtime-held-output-released-on-no-match-up", &RunKeyRebindRuntimeHeldOutputReleasedOnNoMatchUpTest},
        {"key-rebind-runtime-held-output-released-on-mouse-output-up", &RunKeyRebindRuntimeHeldOutputReleasedOnMouseOutputUpTest},
        {"key-rebind-runtime-held-output-released-on-modifier-output-up", &RunKeyRebindRuntimeHeldOutputReleasedOnModifierOutputUpTest},
        {"key-rebind-runtime-held-output-released-on-gui-open", &RunKeyRebindRuntimeHeldOutputReleasedOnGuiOpenTest},
        {"key-rebind-runtime-cursor-state-priority-and-fallback", &RunKeyRebindRuntimeCursorStatePriorityAndFallbackTest},
        {"key-rebind-gui-keyboard-layout-full-bind-and-trigger", &RunKeyRebindGuiKeyboardLayoutFullBindAndTriggerTest},
        {"key-rebind-gui-keyboard-layout-split-bind-and-trigger", &RunKeyRebindGuiKeyboardLayoutSplitBindAndTriggerTest},
        {"key-rebind-gui-text-override-pick-rejects-non-typable-key", &RunKeyRebindGuiTextOverridePickRejectsNonTypableKeyTest},
        {"key-rebind-gui-keyboard-layout-disabled-output", &RunKeyRebindGuiKeyboardLayoutDisabledOutputTest},
        {"key-rebind-gui-keyboard-layout-split-disabled-targets", &RunKeyRebindGuiKeyboardLayoutSplitDisabledTargetsTest},
        {"key-rebind-gui-keyboard-layout-mouse-source-bind-and-trigger", &RunKeyRebindGuiKeyboardLayoutMouseSourceBindAndTriggerTest},
        {"key-rebind-gui-keyboard-layout-mouse-trigger-label-mapping", &RunKeyRebindGuiKeyboardLayoutMouseTriggerLabelMappingTest},
        {"key-rebind-gui-keyboard-layout-xbutton-trigger-label-mapping", &RunKeyRebindGuiKeyboardLayoutXButtonTriggerLabelMappingTest},
        {"key-rebind-gui-keyboard-layout-scroll-trigger-label-mapping", &RunKeyRebindGuiKeyboardLayoutScrollTriggerLabelMappingTest},
        {"key-rebind-gui-keyboard-layout-scroll-source-popup-options", &RunKeyRebindGuiKeyboardLayoutScrollSourcePopupOptionsTest},
        {"key-rebind-gui-keyboard-layout-cursor-state-override", &RunKeyRebindGuiKeyboardLayoutCursorStateOverrideTest},
        {"key-rebind-gui-keyboard-layout-add-custom-bind-button", &RunKeyRebindGuiKeyboardLayoutAddCustomBindButtonTest},
        {"key-rebind-gui-keyboard-layout-remove-custom-bind-button", &RunKeyRebindGuiKeyboardLayoutRemoveCustomBindButtonTest},
        {"key-rebind-gui-keyboard-layout-add-built-in-custom-bind-button", &RunKeyRebindGuiKeyboardLayoutAddBuiltInCustomBindButtonTest},
        {"key-rebind-gui-keyboard-layout-change-custom-input-picker", &RunKeyRebindGuiKeyboardLayoutChangeCustomInputPickerTest},
        {"key-rebind-gui-keyboard-layout-change-custom-input-capture", &RunKeyRebindGuiKeyboardLayoutChangeCustomInputCaptureTest},
        {"key-rebind-gui-keyboard-layout-full-bind-scan-picker-runtime", &RunKeyRebindGuiKeyboardLayoutFullBindScanPickerRuntimeTest},
        {"key-rebind-gui-keyboard-layout-full-bind-scan-picker-cannot-type", &RunKeyRebindGuiKeyboardLayoutFullBindScanPickerCannotTypeTest},
        {"key-rebind-gui-keyboard-layout-scan-picker-filter", &RunKeyRebindGuiKeyboardLayoutScanPickerFilterTest},
        {"key-rebind-gui-keyboard-layout-scan-picker-reset-to-default", &RunKeyRebindGuiKeyboardLayoutScanPickerResetToDefaultTest},
        {"config-load-fullscreen-stretch-repaired", &RunConfigLoadFullscreenStretchRepairedTest},
        {"config-load-fullscreen-manual-dimensions-preserved", &RunConfigLoadFullscreenManualDimensionsPreservedTest},
        {"config-load-fullscreen-relative-dimensions-preserved", &RunConfigLoadFullscreenRelativeDimensionsPreservedTest},
        {"fullscreen-relative-external-resize-skips-stale-resend", &RunFullscreenRelativeExternalResizeSkipsStaleResendTest},
        {"fullscreen-relative-os-wmsize-overrides-computed-dimensions", &RunFullscreenRelativeOsWmSizeOverridesComputedDimensionsTest},
        {"fullscreen-relative-display-dimensions-follow-window-resize", &RunFullscreenRelativeDisplayDimensionsFollowWindowResizeTest},
        {"fullscreen-relative-gui-publish-preserves-recalculated-size", &RunFullscreenRelativeGuiPublishPreservesRecalculatedSizeTest},
        {"config-load-preemptive-sync-existing-mode", &RunConfigLoadPreemptiveSyncExistingModeTest},
        {"config-load-thin-min-width-enforced", &RunConfigLoadThinMinWidthEnforcedTest},
        {"config-load-browser-overlay-defaults", &RunConfigLoadBrowserOverlayDefaultsTest},
        {"config-load-window-overlay-defaults", &RunConfigLoadWindowOverlayDefaultsTest},
        {"config-load-image-defaults", &RunConfigLoadImageDefaultsTest},
        {"config-load-hotkey-default-flags", &RunConfigLoadHotkeyDefaultFlagsTest},
        {"config-load-sensitivity-hotkey-default-flags", &RunConfigLoadSensitivityHotkeyDefaultFlagsTest},
        {"config-load-image-color-key-default-sensitivity", &RunConfigLoadImageColorKeyDefaultSensitivityTest},
        {"config-load-window-overlay-color-key-default-sensitivity", &RunConfigLoadWindowOverlayColorKeyDefaultSensitivityTest},
        {"config-load-browser-overlay-color-key-default-sensitivity", &RunConfigLoadBrowserOverlayColorKeyDefaultSensitivityTest},
        {"mode-mirror-render-screen-anchors", &RunModeMirrorRenderScreenAnchorsTest},
        {"mode-mirror-render-viewport-anchors", &RunModeMirrorRenderViewportAnchorsTest},
        {"mode-mirror-render-screen-anchor-size-matrix", &RunModeMirrorRenderScreenAnchorSizeMatrixTest},
        {"mode-mirror-render-viewport-anchor-size-matrix", &RunModeMirrorRenderViewportAnchorSizeMatrixTest},
        {"mode-mirror-render-raw-output-dynamic-border-size", &RunModeMirrorRenderRawOutputDynamicBorderSizeTest},
        {"mode-mirror-render-low-alpha-visibility", &RunModeMirrorRenderLowAlphaVisibilityTest},
        {"mode-mirror-group-render", &RunModeMirrorGroupRenderTest},
        {"mode-mirror-group-relative-position-resolution", &RunModeMirrorGroupRelativePositionResolutionTest},
        {"mode-mirror-group-slide-unit-transition", &RunModeMirrorGroupSlideUnitTransitionTest},
        {"mode-window-overlay-render", &RunModeWindowOverlayRenderTest},
        {"mode-window-overlay-render-resets-blend-equation", &RunModeWindowOverlayRenderResetsBlendEquationTest},
        {"mode-window-overlay-render-unbinds-sampler", &RunModeWindowOverlayRenderUnbindsSamplerTest},
        {"mode-browser-overlay-render", &RunModeBrowserOverlayRenderTest},
        {"mode-image-overlay-render-png", &RunModeImageOverlayRenderPngTest},
        {"mode-image-overlay-render-mpeg", &RunModeImageOverlayRenderMpegTest},
        {"mode-ninjabrain-overlay-render", &RunModeNinjabrainOverlayRenderTest},
        {"render-ninjabrain-information-message-translation-spans", &RunRenderNinjabrainInformationMessageTranslationSpansTest},
        {"rebind-indicator-renders-below-settings-gui", &RunRebindIndicatorRendersBelowSettingsGuiTest},
        {"config-error-gui", &RunConfigErrorGuiTest},
        {"profiler-unspecified-breakdown", &RunProfilerUnspecifiedBreakdownTest},
        {"settings-gui-basic", &RunSettingsGuiBasicTest},
        {"settings-gui-advanced", &RunSettingsGuiAdvancedTest},
        {"settings-mouse-translation-prefers-live-viewport", &RunSettingsMouseTranslationPrefersLiveViewportTest},
        {"mode-switch-invalidates-latest-viewport-size", &RunModeSwitchInvalidatesLatestViewportSizeTest},
        {"settings-search-subcategory-filtering", &RunSettingsSearchSubcategoryFilteringTest},
        {"settings-search-specific-options", &RunSettingsSearchSpecificOptionsTest},
        {"settings-search-reset-on-close", &RunSettingsSearchResetOnCloseTest},
        {"settings-tab-general-default", &RunSettingsTabGeneralDefaultTest},
        {"settings-tab-other-default", &RunSettingsTabOtherDefaultTest},
        {"settings-tab-supporters-default", &RunSettingsTabSupportersDefaultTest},
        {"settings-tab-modes-default", &RunSettingsTabModesDefaultTest},
        {"settings-tab-mirrors-default", &RunSettingsTabMirrorsDefaultTest},
        {"settings-tab-images-default", &RunSettingsTabImagesDefaultTest},
        {"settings-tab-window-overlays-default", &RunSettingsTabWindowOverlaysDefaultTest},
        {"settings-tab-browser-overlays-default", &RunSettingsTabBrowserOverlaysDefaultTest},
        {"settings-tab-hotkeys-default", &RunSettingsTabHotkeysDefaultTest},
        {"settings-tab-inputs-mouse-default", &RunSettingsTabInputsMouseDefaultTest},
        {"settings-tab-inputs-keyboard-default", &RunSettingsTabInputsKeyboardDefaultTest},
        {"settings-key-repeat-system-repeat-hides-local-controls", &RunSettingsKeyRepeatSystemRepeatHidesLocalControlsTest},
        {"settings-tab-settings-default", &RunSettingsTabSettingsDefaultTest},
        {"settings-tab-appearance-default", &RunSettingsTabAppearanceDefaultTest},
        {"settings-tab-misc-default", &RunSettingsTabMiscDefaultTest},
        {"settings-tab-general-populated", &RunSettingsTabGeneralPopulatedTest},
        {"settings-tab-other-populated", &RunSettingsTabOtherPopulatedTest},
        {"settings-tab-modes-populated", &RunSettingsTabModesPopulatedTest},
        {"settings-tab-mirrors-populated", &RunSettingsTabMirrorsPopulatedTest},
        {"settings-tab-images-populated", &RunSettingsTabImagesPopulatedTest},
        {"settings-tab-window-overlays-populated", &RunSettingsTabWindowOverlaysPopulatedTest},
        {"settings-tab-browser-overlays-populated", &RunSettingsTabBrowserOverlaysPopulatedTest},
        {"settings-tab-hotkeys-populated", &RunSettingsTabHotkeysPopulatedTest},
        {"settings-tab-inputs-mouse-populated", &RunSettingsTabInputsMousePopulatedTest},
        {"settings-tab-inputs-keyboard-populated", &RunSettingsTabInputsKeyboardPopulatedTest},
        {"settings-tab-settings-populated", &RunSettingsTabSettingsPopulatedTest},
        {"settings-tab-appearance-populated", &RunSettingsTabAppearancePopulatedTest},
        {"settings-tab-misc-populated", &RunSettingsTabMiscPopulatedTest},
        {"settings-tab-supporters-populated", &RunSettingsTabSupportersPopulatedTest},
        {"profile-apply-fields-roundtrip", &RunProfileApplyFieldsRoundtripTest},
        {"profile-profiles-config-roundtrip", &RunProfilesConfigRoundtripTest},
        {"profile-name-validation", &RunProfileNameValidationTest},
        {"profile-create-duplicate-delete", &RunProfileCreateDuplicateDeleteTest},
        {"profile-migrate", &RunProfileMigrateTest},
        {"profile-delete-guards", &RunProfileDeleteGuardsTest},
        {"profile-rename", &RunProfileRenameTest},
        {"profile-selective-switch-shared-fallback", &RunProfileSelectiveSwitchSharedFallbackTest},
        {"profile-case-insensitive-collisions", &RunProfileCaseInsensitiveCollisionTest},
        {"profile-recover-missing-metadata", &RunProfileRecoverMissingMetadataTest},
        {"profile-async-save-skip-deleted-profile", &RunProfileAsyncSaveSkipDeletedProfileTest},
        {"profile-switch-ninjabrain-async-stop", &RunProfileSwitchNinjabrainAsyncStopTest},
        {"profile-switch-ninjabrain-async-restart", &RunProfileSwitchNinjabrainAsyncRestartTest},
        {"profile-switch-invalid-default-mode-fallback", &RunProfileSwitchInvalidDefaultModeFallbackTest},
        {"profile-switch-reader-mode-fallback", &RunProfileSwitchReaderModeFallbackTest},
        {"profile-switch-concurrent-readers", &RunProfileSwitchConcurrentReadersTest},
        {"profile-switch-concurrent-lifecycle", &RunProfileSwitchConcurrentLifecycleTest},
        {"profile-switch-concurrent-metadata-rebuild", &RunProfileSwitchConcurrentMetadataRebuildTest},
        {"profile-switch-concurrent-snapshot-writes", &RunProfileSwitchConcurrentSnapshotWritesTest},
    };

    return testCases;
}

const TestCaseDefinition* FindTestCaseDefinition(std::string_view testCaseName) {
    for (const TestCaseDefinition& testCase : GetTestCaseDefinitions()) {
        if (testCase.name == testCaseName) {
            return &testCase;
        }
    }

    return nullptr;
}

bool GroupIncludesTestCaseName(const TestGroupDefinition& testGroup, std::string_view testCaseName) {
    for (const std::string_view prefix : testGroup.prefixes) {
        if (testCaseName.starts_with(prefix)) {
            return true;
        }
    }

    for (const std::string_view explicitTestCaseName : testGroup.explicitTestCaseNames) {
        if (explicitTestCaseName == testCaseName) {
            return true;
        }
    }

    return false;
}

const auto& GetTestGroupDefinitions() {
    static const std::vector<TestGroupDefinition> testGroups = {
        {"config", {"config-default-", "config-roundtrip", "config-load-", "config-publish-", "config-downgrade-", "config-migrate-"},
            {"fullscreen-relative-external-resize-skips-stale-resend", "fullscreen-relative-os-wmsize-overrides-computed-dimensions",
             "fullscreen-relative-display-dimensions-follow-window-resize",
             "fullscreen-relative-gui-publish-preserves-recalculated-size"}},
        {"rebind", {"key-rebind-", "key-repeat-", "hotkey-runtime-"}, {}},
        {"render", {"mode-"}, {"rebind-indicator-renders-below-settings-gui", "render-ninjabrain-information-message-translation-spans"}},
        {"settings-and-ui", {"settings-"}, {"config-error-gui", "profiler-unspecified-breakdown"}},
        {"logs-and-profiles", {"log-", "profile-"}, {}},
    };

    return testGroups;
}

const auto& GetResolvedTestGroupDefinitions() {
    static const std::vector<ResolvedTestGroupDefinition> resolvedTestGroups = []() {
        const auto& testCases = GetTestCaseDefinitions();
        const auto& testGroups = GetTestGroupDefinitions();

        std::vector<size_t> assignmentCounts(testCases.size(), 0);
        std::vector<ResolvedTestGroupDefinition> resolvedGroups;
        resolvedGroups.reserve(testGroups.size());

        for (const TestGroupDefinition& testGroup : testGroups) {
            ResolvedTestGroupDefinition resolvedGroup;
            resolvedGroup.name = testGroup.name;

            for (size_t index = 0; index < testCases.size(); ++index) {
                const TestCaseDefinition& testCase = testCases[index];
                if (testCase.interactiveOnly) {
                    continue;
                }
                if (!GroupIncludesTestCaseName(testGroup, testCase.name)) {
                    continue;
                }

                resolvedGroup.testCases.push_back(&testCase);
                ++assignmentCounts[index];
            }

            if (resolvedGroup.testCases.empty()) {
                throw std::runtime_error("GUI integration test group has no cases: " + std::string(testGroup.name));
            }

            resolvedGroups.push_back(std::move(resolvedGroup));
        }

        for (size_t index = 0; index < testCases.size(); ++index) {
            if (testCases[index].interactiveOnly) {
                continue;
            }
            if (assignmentCounts[index] == 1) {
                continue;
            }

            const std::string testCaseName = testCases[index].name;
            if (assignmentCounts[index] == 0) {
                throw std::runtime_error("GUI integration test case is not assigned to a CI group: " + testCaseName);
            }

            throw std::runtime_error("GUI integration test case is assigned to multiple CI groups: " + testCaseName);
        }

        return resolvedGroups;
    }();

    return resolvedTestGroups;
}

const ResolvedTestGroupDefinition* FindTestGroupDefinition(std::string_view testGroupName) {
    for (const ResolvedTestGroupDefinition& testGroup : GetResolvedTestGroupDefinitions()) {
        if (std::string_view(testGroup.name) == testGroupName) {
            return &testGroup;
        }
    }

    return nullptr;
}

std::string DescribeWindowsError(const DWORD errorCode) {
    LPSTR messageBuffer = nullptr;
    const DWORD messageLength = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        errorCode,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&messageBuffer),
        0,
        nullptr);

    std::string message;
    if (messageLength == 0 || messageBuffer == nullptr) {
        message = "Windows error " + std::to_string(errorCode);
    } else {
        message.assign(messageBuffer, messageLength);
        while (!message.empty() && (message.back() == '\r' || message.back() == '\n')) {
            message.pop_back();
        }
    }

    if (messageBuffer != nullptr) {
        LocalFree(messageBuffer);
    }

    return message;
}

std::wstring GetCurrentExecutablePath() {
    std::wstring executablePath(MAX_PATH, L'\0');

    while (true) {
        const DWORD copiedLength = GetModuleFileNameW(nullptr, executablePath.data(), static_cast<DWORD>(executablePath.size()));
        if (copiedLength == 0) {
            throw std::runtime_error("Failed to query current executable path: " + DescribeWindowsError(GetLastError()));
        }

        if (copiedLength < executablePath.size() - 1) {
            executablePath.resize(copiedLength);
            return executablePath;
        }

        executablePath.resize(executablePath.size() * 2);
    }
}

std::string ReadTextFileIfExists(const std::filesystem::path& filePath) {
    std::ifstream stream(filePath, std::ios::binary);
    if (!stream.is_open()) {
        return {};
    }

    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

RunningParallelTestGroup LaunchTestGroupInChildProcess(std::string_view testGroupName, const std::filesystem::path& logDirectory);
ParallelTestGroupResult CompleteTestGroupInChildProcess(RunningParallelTestGroup& runningGroup);

ParallelTestGroupResult RunTestGroupInChildProcess(std::string_view testGroupName, const std::filesystem::path& logDirectory) {
    RunningParallelTestGroup runningGroup = LaunchTestGroupInChildProcess(testGroupName, logDirectory);
    WaitForSingleObject(runningGroup.processHandle, INFINITE);
    return CompleteTestGroupInChildProcess(runningGroup);
}

RunningParallelTestGroup LaunchTestGroupInChildProcess(std::string_view testGroupName, const std::filesystem::path& logDirectory) {
    RunningParallelTestGroup runningGroup;
    runningGroup.result.groupName = std::string(testGroupName);
    runningGroup.launchTime = std::chrono::steady_clock::now();

    SECURITY_ATTRIBUTES securityAttributes{};
    securityAttributes.nLength = sizeof(securityAttributes);
    securityAttributes.bInheritHandle = TRUE;

    runningGroup.stdoutPath = logDirectory / (runningGroup.result.groupName + ".stdout.log");
    runningGroup.stderrPath = logDirectory / (runningGroup.result.groupName + ".stderr.log");

    HANDLE stdoutHandle = CreateFileW(runningGroup.stdoutPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
                                      &securityAttributes, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (stdoutHandle == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("Failed to open stdout log for group '" + runningGroup.result.groupName + "': " +
                                 DescribeWindowsError(GetLastError()));
    }

    HANDLE stderrHandle = CreateFileW(runningGroup.stderrPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
                                      &securityAttributes, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (stderrHandle == INVALID_HANDLE_VALUE) {
        CloseHandle(stdoutHandle);
        throw std::runtime_error("Failed to open stderr log for group '" + runningGroup.result.groupName + "': " +
                                 DescribeWindowsError(GetLastError()));
    }

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESTDHANDLES;
    startupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startupInfo.hStdOutput = stdoutHandle;
    startupInfo.hStdError = stderrHandle;

    PROCESS_INFORMATION processInfo{};
    const std::wstring executablePath = GetCurrentExecutablePath();
    const std::wstring workingDirectory = std::filesystem::current_path().wstring();
    std::wstring commandLine = L"toolscreen_gui_integration_tests.exe --run-group ";
    commandLine += Utf8ToWide(runningGroup.result.groupName);

    std::vector<wchar_t> commandLineBuffer(commandLine.begin(), commandLine.end());
    commandLineBuffer.push_back(L'\0');

    const BOOL created = CreateProcessW(executablePath.c_str(), commandLineBuffer.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                        nullptr, workingDirectory.c_str(), &startupInfo, &processInfo);

    const DWORD createProcessError = created ? ERROR_SUCCESS : GetLastError();
    CloseHandle(stdoutHandle);
    CloseHandle(stderrHandle);

    if (!created) {
        throw std::runtime_error("Failed to launch GUI integration group '" + runningGroup.result.groupName + "': " +
                                 DescribeWindowsError(createProcessError));
    }

    CloseHandle(processInfo.hThread);
    runningGroup.processHandle = processInfo.hProcess;
    return runningGroup;
}

ParallelTestGroupResult CompleteTestGroupInChildProcess(RunningParallelTestGroup& runningGroup) {
    if (runningGroup.processHandle == nullptr) {
        throw std::runtime_error("GUI integration group process handle is missing for group '" + runningGroup.result.groupName + "'.");
    }

    if (GetExitCodeProcess(runningGroup.processHandle, &runningGroup.result.exitCode) == FALSE) {
        const DWORD errorCode = GetLastError();
        CloseHandle(runningGroup.processHandle);
        runningGroup.processHandle = nullptr;
        throw std::runtime_error("Failed to query exit code for GUI integration group '" + runningGroup.result.groupName + "': " +
                                 DescribeWindowsError(errorCode));
    }

    CloseHandle(runningGroup.processHandle);
    runningGroup.processHandle = nullptr;

    runningGroup.result.stdoutText = ReadTextFileIfExists(runningGroup.stdoutPath);
    runningGroup.result.stderrText = ReadTextFileIfExists(runningGroup.stderrPath);
    return runningGroup.result;
}

void PrintParallelTestGroupResult(const ParallelTestGroupResult& result) {
    std::cout << "===== BEGIN GROUP " << result.groupName << " =====" << std::endl;

    if (!result.stdoutText.empty()) {
        std::cout << result.stdoutText;
        if (!result.stdoutText.ends_with('\n')) {
            std::cout << std::endl;
        }
    }

    if (!result.stderrText.empty()) {
        std::cerr << result.stderrText;
        if (!result.stderrText.ends_with('\n')) {
            std::cerr << std::endl;
        }
    }

    if (!result.failureMessage.empty()) {
        std::cerr << "FAIL: " << result.failureMessage << std::endl;
    }

    std::cout << "===== END GROUP " << result.groupName << " =====" << std::endl;
}

void RunTestCaseByName(std::string_view testCaseName, TestRunMode runMode = TestRunMode::Automated);
void RunTestGroupByName(std::string_view testGroupName);
void RunAllTestGroupsInParallel();
void RunAllTestCases();

void PrintTestCaseList(std::ostream& stream) {
    stream << "Available test cases:" << std::endl;
    for (const TestCaseDefinition& testCase : GetTestCaseDefinitions()) {
        stream << "  " << testCase.name << std::endl;
    }
}

void PrintTestGroupList(std::ostream& stream) {
    stream << "Available test groups:" << std::endl;
    for (const ResolvedTestGroupDefinition& testGroup : GetResolvedTestGroupDefinitions()) {
        stream << "  " << testGroup.name << " (" << testGroup.testCases.size() << " cases)" << std::endl;
    }
}

int FindDefaultVisualTestCaseIndex() {
    const auto& testCases = GetTestCaseDefinitions();
    for (size_t i = 0; i < testCases.size(); ++i) {
        if (std::string_view(testCases[i].name) == kDefaultVisualTestCase) {
            return static_cast<int>(i);
        }
    }

    return 0;
}

enum class LauncherAction {
    None,
    RunSelectedAutomated,
    RunSelectedVisual,
    RunAllAutomated,
    Exit,
};

struct LauncherState {
    int selectedTestCaseIndex = FindDefaultVisualTestCaseIndex();
    std::string lastStatus = "Choose a test case and a run mode.";
};

LauncherAction RenderLauncherFrame(LauncherState& launcherState) {
    const auto& testCases = GetTestCaseDefinitions();
    LauncherAction action = LauncherAction::None;

    ImGui::SetNextWindowPos(ImVec2(40.0f, 40.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(620.0f, 420.0f), ImGuiCond_Always);

    if (ImGui::Begin("Toolscreen GUI Integration Test Runner", nullptr,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextWrapped("Select a test case, then choose whether to run it interactively or as an automated pass/fail check.");
        ImGui::Spacing();

        if (ImGui::BeginListBox("##gui-test-cases", ImVec2(-1.0f, 180.0f))) {
            for (int i = 0; i < static_cast<int>(testCases.size()); ++i) {
                const bool isSelected = launcherState.selectedTestCaseIndex == i;
                if (ImGui::Selectable(testCases[i].name, isSelected)) {
                    launcherState.selectedTestCaseIndex = i;
                }
                if (isSelected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndListBox();
        }

        ImGui::Spacing();
        ImGui::Text("Selected: %s", testCases[launcherState.selectedTestCaseIndex].name);

        if (ImGui::Button("Run Selected Visual", ImVec2(190.0f, 0.0f))) {
            action = LauncherAction::RunSelectedVisual;
        }
        ImGui::SameLine();
        if (ImGui::Button("Run Selected Automated", ImVec2(190.0f, 0.0f))) {
            action = LauncherAction::RunSelectedAutomated;
        }
        ImGui::SameLine();
        if (ImGui::Button("Run All Automated", ImVec2(190.0f, 0.0f))) {
            action = LauncherAction::RunAllAutomated;
        }

        ImGui::Spacing();
        if (ImGui::Button("Close Launcher", ImVec2(190.0f, 0.0f))) {
            action = LauncherAction::Exit;
        }

        ImGui::Separator();
        ImGui::TextWrapped("%s", launcherState.lastStatus.c_str());
    }
    ImGui::End();

    return action;
}

std::string ExecuteLauncherAction(DummyWindow& launcherWindow, const LauncherAction action, const LauncherState& launcherState) {
    const auto& testCases = GetTestCaseDefinitions();
    const char* selectedTestCaseName = testCases[launcherState.selectedTestCaseIndex].name;

    auto runSingle = [&](const TestRunMode runMode) {
        const bool hideLauncher = runMode == TestRunMode::Visual;
        if (hideLauncher) {
            launcherWindow.Show(false);
        }

        HandleImGuiContextReset();

        try {
            RunTestCaseByName(selectedTestCaseName, runMode);
        } catch (...) {
            g_minecraftHwnd.store(launcherWindow.hwnd(), std::memory_order_release);
            launcherWindow.SetTitle("Toolscreen GUI Integration Tests - Launcher");
            if (hideLauncher) {
                launcherWindow.Show(true);
            }
            throw;
        }

        g_minecraftHwnd.store(launcherWindow.hwnd(), std::memory_order_release);
        launcherWindow.SetTitle("Toolscreen GUI Integration Tests - Launcher");
        if (hideLauncher) {
            launcherWindow.Show(true);
        }

        return std::string("PASS ") + selectedTestCaseName + (runMode == TestRunMode::Visual ? " [visual]" : " [automated]");
    };

    switch (action) {
        case LauncherAction::RunSelectedAutomated:
            return runSingle(TestRunMode::Automated);
        case LauncherAction::RunSelectedVisual:
            return runSingle(TestRunMode::Visual);
        case LauncherAction::RunAllAutomated:
            HandleImGuiContextReset();
            RunAllTestCases();
            g_minecraftHwnd.store(launcherWindow.hwnd(), std::memory_order_release);
            launcherWindow.SetTitle("Toolscreen GUI Integration Tests - Launcher");
            return "PASS all automated test cases";
        case LauncherAction::Exit:
            return "Launcher closed.";
        case LauncherAction::None:
            break;
    }

    return launcherState.lastStatus;
}

void RunLauncherGui() {
    DummyWindow launcherWindow(kWindowWidth, kWindowHeight, true);
    launcherWindow.SetTitle("Toolscreen GUI Integration Tests - Launcher");

    LauncherState launcherState;
    while (launcherWindow.PumpMessages()) {
        if (!launcherWindow.BeginFrame()) {
            break;
        }

        const LauncherAction action = RenderLauncherFrame(launcherState);
        launcherWindow.EndFrame();

        if (action == LauncherAction::Exit) {
            break;
        }

        if (action != LauncherAction::None) {
            try {
                launcherState.lastStatus = ExecuteLauncherAction(launcherWindow, action, launcherState);
            } catch (const std::exception& ex) {
                launcherState.lastStatus = std::string("FAIL: ") + ex.what();
            }
        }

        Sleep(16);
    }
}

void PrintUsage(std::ostream& stream) {
    stream << "Usage:" << std::endl;
    stream << "  toolscreen_gui_integration_tests" << std::endl;
    stream << "  toolscreen_gui_integration_tests --run-all" << std::endl;
    stream << "  toolscreen_gui_integration_tests --run-group <group-name>" << std::endl;
    stream << "  toolscreen_gui_integration_tests --run-groups-parallel" << std::endl;
    stream << "  toolscreen_gui_integration_tests <test-case>" << std::endl;
    stream << "  toolscreen_gui_integration_tests --visual [<test-case>]" << std::endl;
    stream << "  toolscreen_gui_integration_tests --list" << std::endl;
    stream << "  toolscreen_gui_integration_tests --list-groups" << std::endl;
    stream << "  toolscreen_gui_integration_tests --help" << std::endl;
    stream << std::endl;
    stream << "No arguments opens a launcher GUI where you can choose which test mode to run." << std::endl;
    stream << "Use --run-all for pure CLI pass/fail execution of every test case." << std::endl;
    stream << "Use --run-group to execute one CI-oriented batch of test cases in declaration order." << std::endl;
    stream << "Use --run-groups-parallel to execute all CI groups concurrently from one parent runner process." << std::endl;
    stream << "Visual mode keeps the dummy Win32/WGL window open so the GUI can be inspected interactively." << std::endl;
    stream << "If no visual test case is provided, it defaults to " << kDefaultVisualTestCase << "." << std::endl;
    stream << std::endl;
    PrintTestGroupList(stream);
    stream << std::endl;
    PrintTestCaseList(stream);
}

struct CommandLineOptions {
    bool openLauncher = false;
    bool showUsage = false;
    bool listOnly = false;
    bool listGroupsOnly = false;
    bool runAll = false;
    bool runGroupsParallel = false;
    TestRunMode runMode = TestRunMode::Automated;
    std::string groupName;
    std::string testCaseName;
};

CommandLineOptions ParseCommandLine(int argc, char** argv) {
    if (argc == 1) {
        CommandLineOptions options;
        options.openLauncher = true;
        return options;
    }

    if (argc > 3) {
        throw std::runtime_error("Expected at most two arguments.");
    }

    const std::string firstArg = argv[1];
    if (firstArg == "--help" || firstArg == "-h") {
        if (argc != 2) {
            throw std::runtime_error("--help does not accept additional arguments.");
        }

        CommandLineOptions options;
        options.showUsage = true;
        return options;
    }

    if (firstArg == "--list") {
        if (argc != 2) {
            throw std::runtime_error("--list does not accept additional arguments.");
        }

        CommandLineOptions options;
        options.listOnly = true;
        return options;
    }

    if (firstArg == "--list-groups") {
        if (argc != 2) {
            throw std::runtime_error("--list-groups does not accept additional arguments.");
        }

        CommandLineOptions options;
        options.listGroupsOnly = true;
        return options;
    }

    if (firstArg == "--run-all") {
        if (argc != 2) {
            throw std::runtime_error("--run-all does not accept additional arguments.");
        }

        CommandLineOptions options;
        options.runAll = true;
        return options;
    }

    if (firstArg == "--run-groups-parallel") {
        if (argc != 2) {
            throw std::runtime_error("--run-groups-parallel does not accept additional arguments.");
        }

        CommandLineOptions options;
        options.runGroupsParallel = true;
        return options;
    }

    if (firstArg == "--run-group") {
        if (argc != 3) {
            throw std::runtime_error("--run-group requires exactly one group name.");
        }

        CommandLineOptions options;
        options.groupName = argv[2];
        return options;
    }

    if (firstArg == "--visual") {
        CommandLineOptions options;
        options.runMode = TestRunMode::Visual;
        options.testCaseName = argc == 3 ? argv[2] : kDefaultVisualTestCase;
        if (options.testCaseName == "all") {
            throw std::runtime_error("Visual mode requires a single test case.");
        }

        return options;
    }

    if (argc != 2) {
        throw std::runtime_error("Unexpected extra arguments.");
    }

    if (firstArg == "all") {
        CommandLineOptions options;
        options.runAll = true;
        return options;
    }

    CommandLineOptions options;
    options.testCaseName = firstArg;
    return options;
}

void RunTestCaseByName(std::string_view testCaseName, TestRunMode runMode) {
    const TestCaseDefinition* testCase = FindTestCaseDefinition(testCaseName);
    if (testCase == nullptr) {
        throw std::runtime_error("Unknown test case: " + std::string(testCaseName));
    }

    std::cout << "RUN " << testCase->name;
    if (runMode == TestRunMode::Visual) {
        std::cout << " [visual]";
    }
    std::cout << std::endl;

    testCase->run(runMode);
    std::cout << "PASS " << testCase->name;
    if (runMode == TestRunMode::Visual) {
        std::cout << " [visual]";
    }
    std::cout << std::endl;
}

void RunTestGroupByName(std::string_view testGroupName) {
    const ResolvedTestGroupDefinition* testGroup = FindTestGroupDefinition(testGroupName);
    if (testGroup == nullptr) {
        throw std::runtime_error("Unknown test group: " + std::string(testGroupName));
    }

    std::cout << "Running GUI integration test group '" << testGroup->name << "' with "
              << testGroup->testCases.size() << " cases." << std::endl;

    for (const TestCaseDefinition* testCase : testGroup->testCases) {
        RunTestCaseByName(testCase->name);
    }

    std::cout << "PASS group " << testGroup->name << std::endl;
}

void RunAllTestGroupsInParallel() {
    const auto& testGroups = GetResolvedTestGroupDefinitions();

    std::cout << "Running all GUI integration test groups in parallel." << std::endl;
    PrintTestGroupList(std::cout);

    const std::filesystem::path logDirectory = std::filesystem::temp_directory_path() /
                                              "toolscreen_gui_integration_parallel" /
                                              std::filesystem::path(std::to_string(GetCurrentProcessId()));
    std::error_code directoryError;
    std::filesystem::remove_all(logDirectory, directoryError);
    directoryError.clear();
    std::filesystem::create_directories(logDirectory, directoryError);
    if (directoryError) {
        throw std::runtime_error("Failed to create parallel GUI integration log directory: " + Narrow(logDirectory.wstring()));
    }

    std::vector<RunningParallelTestGroup> runningGroups;
    runningGroups.reserve(testGroups.size());
    for (const ResolvedTestGroupDefinition& testGroup : testGroups) {
        runningGroups.push_back(LaunchTestGroupInChildProcess(testGroup.name, logDirectory));
    }

    constexpr DWORD kHeartbeatIntervalMs = 30000;
    constexpr auto kGroupTimeout = std::chrono::minutes(5);
    size_t completedGroups = 0;
    std::vector<std::string> failedGroups;
    while (completedGroups < runningGroups.size()) {
        const auto now = std::chrono::steady_clock::now();
        for (RunningParallelTestGroup& runningGroup : runningGroups) {
            if (runningGroup.processHandle == nullptr || now - runningGroup.launchTime < kGroupTimeout) {
                continue;
            }

            runningGroup.result.failureMessage =
                "Timed out after " + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(kGroupTimeout).count()) +
                " seconds while running GUI integration group '" + runningGroup.result.groupName + "'.";

            if (!TerminateProcess(runningGroup.processHandle, ERROR_TIMEOUT)) {
                runningGroup.result.failureMessage +=
                    " Failed to terminate the child process: " + DescribeWindowsError(GetLastError());
            } else {
                WaitForSingleObject(runningGroup.processHandle, 5000);
            }

            try {
                const ParallelTestGroupResult result = CompleteTestGroupInChildProcess(runningGroup);
                PrintParallelTestGroupResult(result);
            } catch (const std::exception& ex) {
                runningGroup.result.failureMessage += " " + std::string(ex.what());
                if (runningGroup.processHandle != nullptr) {
                    CloseHandle(runningGroup.processHandle);
                    runningGroup.processHandle = nullptr;
                }
                PrintParallelTestGroupResult(runningGroup.result);
            }

            failedGroups.push_back(runningGroup.result.groupName);
            ++completedGroups;
        }

        std::vector<HANDLE> waitHandles;
        waitHandles.reserve(runningGroups.size() - completedGroups);
        for (const RunningParallelTestGroup& runningGroup : runningGroups) {
            if (runningGroup.processHandle != nullptr) {
                waitHandles.push_back(runningGroup.processHandle);
            }
        }

        if (waitHandles.empty()) {
            break;
        }

        const DWORD waitResult = WaitForMultipleObjects(static_cast<DWORD>(waitHandles.size()), waitHandles.data(), FALSE,
                                                        kHeartbeatIntervalMs);
        if (waitResult == WAIT_TIMEOUT) {
            std::cout << "Still running GUI integration groups:";
            for (const RunningParallelTestGroup& runningGroup : runningGroups) {
                if (runningGroup.processHandle != nullptr) {
                    std::cout << ' ' << runningGroup.result.groupName;
                }
            }
            std::cout << std::endl;
            continue;
        }

        if (waitResult == WAIT_FAILED) {
            throw std::runtime_error("Failed while waiting for parallel GUI integration groups: " +
                                     DescribeWindowsError(GetLastError()));
        }

        const DWORD completedHandleIndex = waitResult - WAIT_OBJECT_0;
        if (completedHandleIndex >= waitHandles.size()) {
            throw std::runtime_error("Unexpected wait result while running parallel GUI integration groups: " +
                                     std::to_string(waitResult));
        }

        const HANDLE completedHandle = waitHandles[completedHandleIndex];
        for (RunningParallelTestGroup& runningGroup : runningGroups) {
            if (runningGroup.processHandle != completedHandle) {
                continue;
            }

            try {
                const ParallelTestGroupResult result = CompleteTestGroupInChildProcess(runningGroup);
                PrintParallelTestGroupResult(result);
                if (!result.failureMessage.empty() || result.exitCode != 0) {
                    failedGroups.push_back(result.groupName);
                }
            } catch (const std::exception& ex) {
                runningGroup.result.failureMessage = ex.what();
                runningGroup.result.exitCode = 1;
                if (runningGroup.processHandle != nullptr) {
                    CloseHandle(runningGroup.processHandle);
                    runningGroup.processHandle = nullptr;
                }
                PrintParallelTestGroupResult(runningGroup.result);
                failedGroups.push_back(runningGroup.result.groupName);
            }

            ++completedGroups;
            break;
        }
    }

    std::error_code cleanupError;
    std::filesystem::remove_all(logDirectory, cleanupError);

    if (!failedGroups.empty()) {
        std::string failureSummary;
        for (size_t index = 0; index < failedGroups.size(); ++index) {
            if (index > 0) {
                failureSummary += ", ";
            }
            failureSummary += failedGroups[index];
        }

        throw std::runtime_error("Parallel GUI integration groups failed: " + failureSummary);
    }

    std::cout << "PASS all GUI integration test groups [parallel]" << std::endl;
}

void RunAllTestCases() {
    for (const TestCaseDefinition& testCase : GetTestCaseDefinitions()) {
        if (testCase.interactiveOnly) {
            continue;
        }
        RunTestCaseByName(testCase.name);
    }
}

bool ShouldPauseForTransientConsole() {
    DWORD consoleProcessIds[2]{};
    return GetConsoleProcessList(consoleProcessIds, static_cast<DWORD>(std::size(consoleProcessIds))) == 1;
}

void PauseForTransientConsole() {
    if (!ShouldPauseForTransientConsole()) {
        return;
    }

    std::cerr << "Press Enter to close..." << std::flush;
    std::string ignored;
    std::getline(std::cin, ignored);
}

class ScopedTestProcessCleanup {
  public:
    ~ScopedTestProcessCleanup() {
        try {
            StopNinjabrainClient();
            FlushLogs();

            std::lock_guard<std::mutex> lock(g_logFileMutex);
            if (logFile.is_open()) {
                logFile.close();
            }
            logFile.clear();
        } catch (...) {
        }
    }
};

} // namespace

int main(int argc, char** argv) {
    try {
        ScopedTestProcessCleanup cleanup;
        EnsureProcessDpiAwareness();
        const CommandLineOptions options = ParseCommandLine(argc, argv);

        if (options.openLauncher) {
            RunLauncherGui();
            return 0;
        }

        if (options.showUsage) {
            PrintUsage(std::cout);
            return 0;
        }

        if (options.listOnly) {
            PrintTestCaseList(std::cout);
            return 0;
        }

        if (options.listGroupsOnly) {
            PrintTestGroupList(std::cout);
            return 0;
        }

        if (options.runAll) {
            std::cout << "Running all GUI integration tests." << std::endl;
            PrintTestCaseList(std::cout);
            RunAllTestCases();
            return 0;
        }

        if (options.runGroupsParallel) {
            RunAllTestGroupsInParallel();
            return 0;
        }

        if (!options.groupName.empty()) {
            RunTestGroupByName(options.groupName);
            return 0;
        }

        RunTestCaseByName(options.testCaseName, options.runMode);
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "FAIL: " << ex.what() << std::endl;
        PrintUsage(std::cerr);
        PauseForTransientConsole();
        return 1;
    }
}