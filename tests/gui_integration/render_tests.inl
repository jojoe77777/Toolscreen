void RunModeMirrorRenderScreenAnchorsTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    if (!g_hasModernGL) { std::cout << "SKIP (no GL 3.3+)" << std::endl; return; }

    const std::filesystem::path root = PrepareCaseDirectory("mode_mirror_render_screen_anchors");
    ResetGlobalTestState(root);

    constexpr char kModeId[] = "Mirror Screen Anchors Mode";

    MirrorConfig topLeftMirror = MakeMirrorRenderTestConfig("Top Left Mirror", 18, 12, "topLeftScreen", 30, 40, 4.0f);
    MirrorConfig topRightMirror = MakeMirrorRenderTestConfig("Top Right Mirror", 15, 15, "topRightScreen", 55, 35, 4.0f);
    MirrorConfig bottomLeftMirror = MakeMirrorRenderTestConfig("Bottom Left Mirror", 20, 10, "bottomLeftScreen", 70, 45, 4.0f);
    MirrorConfig bottomRightMirror = MakeMirrorRenderTestConfig("Bottom Right Mirror", 21, 14, "bottomRightScreen", 50, 60, 4.0f);

    ModeConfig mode;
    mode.id = kModeId;
    mode.width = kWindowWidth;
    mode.height = kWindowHeight;
    mode.manualWidth = kWindowWidth;
    mode.manualHeight = kWindowHeight;
    mode.sources = { { ModeSourceType::Mirror, topLeftMirror.name }, { ModeSourceType::Mirror, topRightMirror.name },
                     { ModeSourceType::Mirror, bottomLeftMirror.name }, { ModeSourceType::Mirror, bottomRightMirror.name } };

    g_config.defaultMode = kModeId;
    g_config.mirrors = { topLeftMirror, topRightMirror, bottomLeftMirror, bottomRightMirror };
    g_config.modes = { mode };
    g_configLoaded.store(true, std::memory_order_release);

    InitializeMirrorRenderTestResources();

    const SurfaceSize surface = GetWindowClientSize(window.hwnd());
    ScopedTexture2D sourceTexture(surface.width, surface.height, MakeSolidRgbaPixels(surface.width, surface.height, 0, 255, 0));

    auto renderAndAssert = [&](DummyWindow& targetWindow) {
        RenderModeOverlayFrame(targetWindow, g_config, g_config.modes.front(), sourceTexture.id());
        if (runMode == TestRunMode::Automated) {
            std::vector<MirrorConfig> activeMirrors;
            std::vector<ImageConfig> unusedImages;
            std::vector<const WindowOverlayConfig*> unusedWindowOverlays;
            std::vector<const BrowserOverlayConfig*> unusedBrowserOverlays;
            CollectActiveElementsForMode(g_config, kModeId, false, 0, activeMirrors, unusedImages, unusedWindowOverlays,
                                         unusedBrowserOverlays);

            Expect(activeMirrors.size() == 4, "Expected all direct mirror sources to resolve for the screen-anchor test.");
            Expect(activeMirrors[0].name == topLeftMirror.name, "Expected the first direct mirror source to remain ordered.");
            Expect(activeMirrors[1].name == topRightMirror.name, "Expected the second direct mirror source to remain ordered.");
            Expect(activeMirrors[2].name == bottomLeftMirror.name, "Expected the third direct mirror source to remain ordered.");
            Expect(activeMirrors[3].name == bottomRightMirror.name, "Expected the fourth direct mirror source to remain ordered.");

            const ExpectedMirrorRect expectedTopLeftRect = ComputeExpectedMirrorRect(activeMirrors[0], surface.width, surface.height,
                                                                                     0, 0, surface.width, surface.height);
            const ExpectedMirrorRect expectedTopRightRect = ComputeExpectedMirrorRect(activeMirrors[1], surface.width, surface.height,
                                                                                      0, 0, surface.width, surface.height);
            const ExpectedMirrorRect expectedBottomLeftRect = ComputeExpectedMirrorRect(activeMirrors[2], surface.width, surface.height,
                                                                                        0, 0, surface.width, surface.height);
            const ExpectedMirrorRect expectedBottomRightRect = ComputeExpectedMirrorRect(activeMirrors[3], surface.width,
                                                                                         surface.height, 0, 0, surface.width,
                                                                                         surface.height);
            const ExpectedMirrorRect topLeftRect = GetCachedMirrorRect(activeMirrors[0].name);
            const ExpectedMirrorRect topRightRect = GetCachedMirrorRect(activeMirrors[1].name);
            const ExpectedMirrorRect bottomLeftRect = GetCachedMirrorRect(activeMirrors[2].name);
            const ExpectedMirrorRect bottomRightRect = GetCachedMirrorRect(activeMirrors[3].name);

            ExpectMirrorRectNear(topLeftRect, expectedTopLeftRect, "Top-left mirror cached bounds");
            ExpectMirrorRectNear(topRightRect, expectedTopRightRect, "Top-right mirror cached bounds");
            ExpectMirrorRectNear(bottomLeftRect, expectedBottomLeftRect, "Bottom-left mirror cached bounds");
            ExpectMirrorRectNear(bottomRightRect, expectedBottomRightRect, "Bottom-right mirror cached bounds");

            ExpectSolidColorRect(topLeftRect, surface.height, kExpectedMirrorRenderGreen,
                                 "Expected the top-left screen mirror to draw the staged game texture.");
            ExpectSolidColorRect(topRightRect, surface.height, kExpectedMirrorRenderGreen,
                                 "Expected the top-right screen mirror to draw the staged game texture.");
            ExpectSolidColorRect(bottomLeftRect, surface.height, kExpectedMirrorRenderGreen,
                                 "Expected the bottom-left screen mirror to draw the staged game texture.");
            ExpectSolidColorRect(bottomRightRect, surface.height, kExpectedMirrorRenderGreen,
                                 "Expected the bottom-right screen mirror to draw the staged game texture.");

            ExpectBackgroundPixel(topLeftRect.x - 2, topLeftRect.y + topLeftRect.height / 2, surface.height,
                                  "Expected pixels just left of the top-left mirror to remain background.");
            ExpectBackgroundPixel(topRightRect.x + topRightRect.width + 2, topRightRect.y + topRightRect.height / 2, surface.height,
                                  "Expected pixels just right of the top-right mirror to remain background.");
            ExpectBackgroundPixel(bottomLeftRect.x - 2, bottomLeftRect.y + bottomLeftRect.height / 2, surface.height,
                                  "Expected pixels just left of the bottom-left mirror to remain background.");
            ExpectBackgroundPixel(bottomRightRect.x + bottomRightRect.width / 2, bottomRightRect.y - 2, surface.height,
                                  "Expected pixels just above the bottom-right mirror to remain background.");
        }
    };

    if (runMode == TestRunMode::Visual) {
        RunVisualLoop(window, "mode-mirror-render-screen-anchors", [&](DummyWindow& visualWindow) { renderAndAssert(visualWindow); });
    } else {
        renderAndAssert(window);
    }

    CleanupBrowserOverlayCache();
    CleanupWindowOverlayCache();
    CleanupGPUResources();
    CleanupShaders();
}

void RunModeMirrorRenderViewportAnchorsTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    if (!g_hasModernGL) { std::cout << "SKIP (no GL 3.3+)" << std::endl; return; }

    const std::filesystem::path root = PrepareCaseDirectory("mode_mirror_render_viewport_anchors");
    ResetGlobalTestState(root);

    constexpr char kModeId[] = "Mirror Viewport Anchors Mode";

    MirrorConfig centerMirror = MakeMirrorRenderTestConfig("Center Viewport Mirror", 15, 12, "centerViewport", 25, -30, 5.0f);
    MirrorConfig topRightMirror = MakeMirrorRenderTestConfig("Top Right Viewport Mirror", 12, 10, "topRightViewport", 35, 24, 6.0f);

    ModeConfig mode;
    mode.id = kModeId;
    mode.width = kWindowWidth;
    mode.height = kWindowHeight;
    mode.manualWidth = kWindowWidth;
    mode.manualHeight = kWindowHeight;
    mode.sources = { { ModeSourceType::Mirror, centerMirror.name }, { ModeSourceType::Mirror, topRightMirror.name } };

    g_config.defaultMode = kModeId;
    g_config.mirrors = { centerMirror, topRightMirror };
    g_config.modes = { mode };
    g_configLoaded.store(true, std::memory_order_release);

    InitializeMirrorRenderTestResources();

    const SurfaceSize surface = GetWindowClientSize(window.hwnd());
    ScopedTexture2D sourceTexture(surface.width, surface.height, MakeSolidRgbaPixels(surface.width, surface.height, 0, 255, 0));

    auto renderAndAssert = [&](DummyWindow& targetWindow) {
        RenderModeOverlayFrame(targetWindow, g_config, g_config.modes.front(), sourceTexture.id());
        if (runMode == TestRunMode::Automated) {
            std::vector<MirrorConfig> activeMirrors;
            std::vector<ImageConfig> unusedImages;
            std::vector<const WindowOverlayConfig*> unusedWindowOverlays;
            std::vector<const BrowserOverlayConfig*> unusedBrowserOverlays;
            CollectActiveElementsForMode(g_config, kModeId, false, 0, activeMirrors, unusedImages, unusedWindowOverlays,
                                         unusedBrowserOverlays);

            Expect(activeMirrors.size() == 2, "Expected both viewport-relative mirrors to resolve for the viewport-anchor test.");

            const ExpectedMirrorRect expectedCenterRect = ComputeExpectedMirrorRect(activeMirrors[0], surface.width, surface.height,
                                                                                    0, 0, surface.width, surface.height);
            const ExpectedMirrorRect expectedTopRightRect = ComputeExpectedMirrorRect(activeMirrors[1], surface.width,
                                                                                      surface.height, 0, 0, surface.width,
                                                                                      surface.height);
            const ExpectedMirrorRect centerRect = GetCachedMirrorRect(activeMirrors[0].name);
            const ExpectedMirrorRect topRightRect = GetCachedMirrorRect(activeMirrors[1].name);

            ExpectMirrorRectNear(centerRect, expectedCenterRect, "Center-viewport mirror cached bounds", 3);
            ExpectMirrorRectNear(topRightRect, expectedTopRightRect, "Top-right viewport mirror cached bounds", 3);

            ExpectSolidColorRect(centerRect, surface.height, kExpectedMirrorRenderGreen,
                                 "Expected the center-viewport mirror to render at the viewport center offset.");
            ExpectSolidColorRect(topRightRect, surface.height, kExpectedMirrorRenderGreen,
                                 "Expected the top-right viewport mirror to render at the viewport anchor.");

            ExpectBackgroundPixel(centerRect.x + centerRect.width / 2, centerRect.y - 2, surface.height,
                                  "Expected pixels above the center-viewport mirror to remain background.");
            ExpectBackgroundPixel(topRightRect.x - 2, topRightRect.y + topRightRect.height / 2, surface.height,
                                  "Expected pixels left of the top-right viewport mirror to remain background.");
        }
    };

    if (runMode == TestRunMode::Visual) {
        RunVisualLoop(window, "mode-mirror-render-viewport-anchors", [&](DummyWindow& visualWindow) { renderAndAssert(visualWindow); });
    } else {
        renderAndAssert(window);
    }

    CleanupBrowserOverlayCache();
    CleanupWindowOverlayCache();
    CleanupGPUResources();
    CleanupShaders();
}

void RunModeMirrorRenderScreenAnchorSizeMatrixTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    if (!g_hasModernGL) { std::cout << "SKIP (no GL 3.3+)" << std::endl; return; }

    const std::filesystem::path root = PrepareCaseDirectory("mode_mirror_render_screen_anchor_size_matrix");
    ResetGlobalTestState(root);

    constexpr char kModeId[] = "Mirror Screen Anchor Size Matrix Mode";

    const std::vector<MirrorAnchorRenderScenario> scenarios = {
        {
            { "screen-medium", 257, 193, 0, 0, 257, 193 },
            {
                { "Screen Medium Top Left", "topLeftScreen", 7, 5, 9, 11, 3.0f },
                { "Screen Medium Top Right Pixel", "topRightScreen", 1, 1, 13, 17, 2.0f },
                { "Screen Medium Bottom Left", "bottomLeftScreen", 2, 5, 7, 9, 2.0f },
                { "Screen Medium Bottom Right", "bottomRightScreen", 5, 2, 15, 14, 3.0f },
                { "Screen Medium Center", "centerScreen", 4, 3, 5, -7, 4.0f },
            },
            true,
        },
        {
            { "screen-small", 149, 113, 0, 0, 149, 113 },
            {
                { "Screen Small Top Left", "topLeftScreen", 1, 1, 2, 2, 4.0f },
                { "Screen Small Top Right", "topRightScreen", 1, 1, 3, 2, 4.0f },
                { "Screen Small Bottom Left", "bottomLeftScreen", 1, 1, 2, 2, 4.0f },
                { "Screen Small Bottom Right", "bottomRightScreen", 1, 1, 2, 2, 4.0f },
                { "Screen Small Center", "centerScreen", 1, 1, 0, 0, 5.0f },
            },
            true,
        },
        {
            { "screen-single-pixel", 1, 1, 0, 0, 1, 1 },
            {
                { "Screen Pixel Top Left", "topLeftScreen", 1, 1, 0, 0, 1.0f },
                { "Screen Pixel Top Right", "topRightScreen", 1, 1, 0, 0, 1.0f },
                { "Screen Pixel Bottom Left", "bottomLeftScreen", 1, 1, 0, 0, 1.0f },
                { "Screen Pixel Bottom Right", "bottomRightScreen", 1, 1, 0, 0, 1.0f },
                { "Screen Pixel Center", "centerScreen", 1, 1, 0, 0, 1.0f },
            },
            false,
        },
    };

    for (const MirrorAnchorRenderScenario& scenario : scenarios) {
        g_config = Config();

        ModeConfig mode;
        mode.id = kModeId;
        mode.width = (std::max)(1, scenario.geometry.gameW);
        mode.height = (std::max)(1, scenario.geometry.gameH);
        mode.manualWidth = mode.width;
        mode.manualHeight = mode.height;

        g_config.defaultMode = kModeId;
        g_config.mirrors.clear();
        g_config.mirrors.reserve(scenario.mirrors.size());
        mode.sources.reserve(scenario.mirrors.size());
        for (const MirrorAnchorCaseDefinition& mirrorCase : scenario.mirrors) {
            g_config.mirrors.push_back(MakeMirrorRenderTestConfig(mirrorCase.name, mirrorCase.captureWidth,
                                                                  mirrorCase.captureHeight, mirrorCase.relativeTo,
                                                                  mirrorCase.outputX, mirrorCase.outputY, mirrorCase.scale));
            mode.sources.push_back({ ModeSourceType::Mirror, mirrorCase.name });
        }

        g_config.modes = { mode };
        g_configLoaded.store(true, std::memory_order_release);

        InitializeMirrorRenderTestResources();
        auto assertScenario = [&](const SimulatedOverlayGeometry& geometry, const SurfaceSize& surface) {
            std::vector<MirrorConfig> activeMirrors;
            std::vector<ImageConfig> unusedImages;
            std::vector<const WindowOverlayConfig*> unusedWindowOverlays;
            std::vector<const BrowserOverlayConfig*> unusedBrowserOverlays;
            CollectActiveElementsForMode(g_config, kModeId, false, 0, activeMirrors, unusedImages, unusedWindowOverlays,
                                         unusedBrowserOverlays);

            Expect(activeMirrors.size() == scenario.mirrors.size(),
                   geometry.label + " should resolve every screen-anchor mirror in the scenario.");

            for (const MirrorConfig& activeMirror : activeMirrors) {
                ExpectMirrorRenderMatchesExpectedPlacement(activeMirror, geometry, surface,
                                                           geometry.label + " render output for " + activeMirror.name,
                                                           scenario.expectVisibleRender);
            }
        };

        if (scenario.expectVisibleRender) {
            DummyWindow scenarioWindow(scenario.geometry.fullW, scenario.geometry.fullH, false);
            const SurfaceSize surface = GetWindowClientSize(scenarioWindow.hwnd());
            SimulatedOverlayGeometry geometry = scenario.geometry;
            geometry.fullW = surface.width;
            geometry.fullH = surface.height;
            geometry.gameW = surface.width;
            geometry.gameH = surface.height;

            ScopedTexture2D sourceTexture(surface.width, surface.height,
                                          MakeSolidRgbaPixels(surface.width, surface.height, 0, 255, 0));

            RenderModeOverlayFrameWithGeometry(scenarioWindow, g_config, g_config.modes.front(), geometry,
                                               sourceTexture.id(), [&](const SurfaceSize& renderSurface) {
                assertScenario(geometry, renderSurface);
            });
        } else {
            ScopedTexture2D sourceTexture((std::max)(1, scenario.geometry.fullW), (std::max)(1, scenario.geometry.fullH),
                                          MakeSolidRgbaPixels((std::max)(1, scenario.geometry.fullW),
                                                              (std::max)(1, scenario.geometry.fullH), 0, 255, 0));

            RenderModeOverlayFrameToSimulatedSurface(window, g_config, g_config.modes.front(), scenario.geometry,
                                                     sourceTexture.id(), [&](const SurfaceSize& surface) {
                assertScenario(scenario.geometry, surface);
            });
        }
    }

    CleanupBrowserOverlayCache();
    CleanupWindowOverlayCache();
    CleanupGPUResources();
    CleanupShaders();
}

void RunModeMirrorRenderViewportAnchorSizeMatrixTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    if (!g_hasModernGL) { std::cout << "SKIP (no GL 3.3+)" << std::endl; return; }

    const std::filesystem::path root = PrepareCaseDirectory("mode_mirror_render_viewport_anchor_size_matrix");
    ResetGlobalTestState(root);

    constexpr char kModeId[] = "Mirror Viewport Anchor Size Matrix Mode";

    const std::vector<MirrorAnchorRenderScenario> scenarios = {
        {
            { "viewport-medium", 301, 211, 47, 39, 167, 109 },
            {
                { "Viewport Medium Top Left", "topLeftViewport", 7, 5, 6, 8, 3.0f },
                { "Viewport Medium Top Right Pixel", "topRightViewport", 1, 1, 11, 9, 2.0f },
                { "Viewport Medium Bottom Left", "bottomLeftViewport", 3, 4, 7, 6, 2.0f },
                { "Viewport Medium Bottom Right", "bottomRightViewport", 4, 2, 9, 5, 3.0f },
                { "Viewport Medium Center", "centerViewport", 5, 3, 4, -6, 2.0f },
            },
            true,
        },
        {
            { "viewport-small", 171, 139, 23, 19, 83, 61 },
            {
                { "Viewport Small Top Left", "topLeftViewport", 1, 1, 2, 2, 4.0f },
                { "Viewport Small Top Right", "topRightViewport", 1, 1, 2, 2, 4.0f },
                { "Viewport Small Bottom Left", "bottomLeftViewport", 1, 1, 2, 2, 4.0f },
                { "Viewport Small Bottom Right", "bottomRightViewport", 1, 1, 2, 2, 4.0f },
                { "Viewport Small Center", "centerViewport", 1, 1, 0, 0, 5.0f },
            },
            true,
        },
        {
            { "viewport-single-pixel", 17, 15, 8, 7, 1, 1 },
            {
                { "Viewport Pixel Top Left", "topLeftViewport", 1, 1, 0, 0, 1.0f },
                { "Viewport Pixel Top Right", "topRightViewport", 1, 1, 0, 0, 1.0f },
                { "Viewport Pixel Bottom Left", "bottomLeftViewport", 1, 1, 0, 0, 1.0f },
                { "Viewport Pixel Bottom Right", "bottomRightViewport", 1, 1, 0, 0, 1.0f },
                { "Viewport Pixel Center", "centerViewport", 1, 1, 0, 0, 1.0f },
            },
            false,
        },
    };

    for (const MirrorAnchorRenderScenario& scenario : scenarios) {
        g_config = Config();

        ModeConfig mode;
        mode.id = kModeId;
        mode.width = (std::max)(1, scenario.geometry.gameW);
        mode.height = (std::max)(1, scenario.geometry.gameH);
        mode.manualWidth = mode.width;
        mode.manualHeight = mode.height;

        g_config.defaultMode = kModeId;
        g_config.mirrors.clear();
        g_config.mirrors.reserve(scenario.mirrors.size());
        mode.sources.reserve(scenario.mirrors.size());
        for (const MirrorAnchorCaseDefinition& mirrorCase : scenario.mirrors) {
            g_config.mirrors.push_back(MakeMirrorRenderTestConfig(mirrorCase.name, mirrorCase.captureWidth,
                                                                  mirrorCase.captureHeight, mirrorCase.relativeTo,
                                                                  mirrorCase.outputX, mirrorCase.outputY, mirrorCase.scale));
            mode.sources.push_back({ ModeSourceType::Mirror, mirrorCase.name });
        }

        g_config.modes = { mode };
        g_configLoaded.store(true, std::memory_order_release);

        InitializeMirrorRenderTestResources();
        auto assertScenario = [&](const SimulatedOverlayGeometry& geometry, const SurfaceSize& surface) {
            std::vector<MirrorConfig> activeMirrors;
            std::vector<ImageConfig> unusedImages;
            std::vector<const WindowOverlayConfig*> unusedWindowOverlays;
            std::vector<const BrowserOverlayConfig*> unusedBrowserOverlays;
            CollectActiveElementsForMode(g_config, kModeId, false, 0, activeMirrors, unusedImages, unusedWindowOverlays,
                                         unusedBrowserOverlays);

            Expect(activeMirrors.size() == scenario.mirrors.size(),
                   geometry.label + " should resolve every viewport-anchor mirror in the scenario.");

            for (const MirrorConfig& activeMirror : activeMirrors) {
                ExpectMirrorRenderMatchesExpectedPlacement(activeMirror, geometry, surface,
                                                           geometry.label + " render output for " + activeMirror.name,
                                                           scenario.expectVisibleRender);
            }
        };

        if (scenario.expectVisibleRender) {
            DummyWindow scenarioWindow(scenario.geometry.fullW, scenario.geometry.fullH, false);
            const SurfaceSize surface = GetWindowClientSize(scenarioWindow.hwnd());
            SimulatedOverlayGeometry geometry = scenario.geometry;
            geometry.fullW = surface.width;
            geometry.fullH = surface.height;

            ScopedTexture2D sourceTexture(surface.width, surface.height,
                                          MakeSolidRgbaPixels(surface.width, surface.height, 0, 255, 0));

            RenderModeOverlayFrameWithGeometry(scenarioWindow, g_config, g_config.modes.front(), geometry,
                                               sourceTexture.id(), [&](const SurfaceSize& renderSurface) {
                assertScenario(geometry, renderSurface);
            });
        } else {
            ScopedTexture2D sourceTexture((std::max)(1, scenario.geometry.fullW), (std::max)(1, scenario.geometry.fullH),
                                          MakeSolidRgbaPixels((std::max)(1, scenario.geometry.fullW),
                                                              (std::max)(1, scenario.geometry.fullH), 0, 255, 0));

            RenderModeOverlayFrameToSimulatedSurface(window, g_config, g_config.modes.front(), scenario.geometry,
                                                     sourceTexture.id(), [&](const SurfaceSize& surface) {
                assertScenario(scenario.geometry, surface);
            });
        }
    }

    CleanupBrowserOverlayCache();
    CleanupWindowOverlayCache();
    CleanupGPUResources();
    CleanupShaders();
}

void RunModeMirrorRenderRawOutputDynamicBorderSizeTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    if (!g_hasModernGL) { std::cout << "SKIP (no GL 3.3+)" << std::endl; return; }

    const std::filesystem::path root = PrepareCaseDirectory("mode_mirror_render_raw_output_dynamic_border_size");
    ResetGlobalTestState(root);

    constexpr char kModeId[] = "Mirror Raw Output Dynamic Border Size";

    MirrorConfig mirror = MakeMirrorRenderTestConfig("Raw Output Dynamic Border Mirror", 17, 11, "topLeftScreen", 28, 24, 1.0f);
    mirror.output.separateScale = true;
    mirror.output.scaleX = 3.0f;
    mirror.output.scaleY = 2.0f;
    mirror.border.dynamicThickness = 5;

    ModeConfig mode;
    mode.id = kModeId;
    mode.width = 257;
    mode.height = 211;
    mode.manualWidth = mode.width;
    mode.manualHeight = mode.height;
    mode.sources = { { ModeSourceType::Mirror, mirror.name } };

    g_config.defaultMode = kModeId;
    g_config.mirrors = { mirror };
    g_config.modes = { mode };
    g_configLoaded.store(true, std::memory_order_release);

    InitializeMirrorRenderTestResources();

    const SimulatedOverlayGeometry geometry{ "raw-output-dynamic-border", 257, 211, 0, 0, 257, 211 };
    ScopedTexture2D sourceTexture(geometry.fullW, geometry.fullH,
                                  MakeSolidRgbaPixels(geometry.fullW, geometry.fullH, 0, 255, 0));

    auto renderAndAssert = [&](DummyWindow& targetWindow) {
        RenderModeOverlayFrameToSimulatedSurface(targetWindow, g_config, g_config.modes.front(), geometry, sourceTexture.id(),
                                                 [&](const SurfaceSize& surface) {
            if (runMode != TestRunMode::Automated) {
                return;
            }

            std::vector<MirrorConfig> activeMirrors;
            std::vector<ImageConfig> unusedImages;
            std::vector<const WindowOverlayConfig*> unusedWindowOverlays;
            std::vector<const BrowserOverlayConfig*> unusedBrowserOverlays;
            CollectActiveElementsForMode(g_config, kModeId, false, 0, activeMirrors, unusedImages, unusedWindowOverlays,
                                         unusedBrowserOverlays, geometry.fullW, geometry.fullH);

            Expect(activeMirrors.size() == 1,
                   "Expected the raw-output regression test to resolve exactly one active mirror.");
            const ExpectedMirrorRect expectedRect = ComputeExpectedMirrorRect(activeMirrors[0], surface.width, surface.height,
                                                                              geometry.gameX, geometry.gameY, geometry.gameW,
                                                                              geometry.gameH);
            const ExpectedMirrorRect actualRect = GetCachedMirrorRect(activeMirrors[0].name);

            ExpectMirrorRectNear(actualRect, expectedRect,
                                 "Raw-output dynamic-border mirror cached bounds should ignore border padding.", 0);
            ExpectSolidColorRect(expectedRect, surface.height, kExpectedMirrorRenderGreen,
                                 "Expected the raw-output dynamic-border mirror interior to remain unstretched.");

            const int outsideX = expectedRect.x + expectedRect.width + 3;
            if (outsideX < surface.width) {
                ExpectBackgroundPixel(outsideX, expectedRect.y + expectedRect.height / 2, surface.height,
                                      "Expected pixels just outside the raw-output mirror width to remain background.");
            }
        });
    };

    if (runMode == TestRunMode::Visual) {
        RunVisualLoop(window, "mode-mirror-render-raw-output-dynamic-border-size",
                      [&](DummyWindow& visualWindow) { renderAndAssert(visualWindow); });
    } else {
        renderAndAssert(window);
    }

    CleanupBrowserOverlayCache();
    CleanupWindowOverlayCache();
    CleanupGPUResources();
    CleanupShaders();
}

void RunModeMirrorRenderLowAlphaVisibilityTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    if (!g_hasModernGL) { std::cout << "SKIP (no GL 3.3+)" << std::endl; return; }

    const std::filesystem::path root = PrepareCaseDirectory("mode_mirror_render_low_alpha_visibility");
    ResetGlobalTestState(root);

    constexpr char kModeId[] = "Mirror Low Alpha Visibility";

    MirrorConfig compositeMirror = MakeMirrorRenderTestConfig("Low Opacity Composite Mirror", 18, 12, "topLeftScreen", 24, 32, 4.0f);
    compositeMirror.rawOutput = false;
    compositeMirror.opacity = 0.4f;
    compositeMirror.border.dynamicThickness = 1;
    compositeMirror.colors.targetColors = { { 0.0f, 1.0f, 0.0f, 1.0f } };
    compositeMirror.colors.output = { 0.0f, 1.0f, 0.0f, 1.0f };
    compositeMirror.colors.border = { 1.0f, 0.2f, 0.2f, 1.0f };
    compositeMirror.colorSensitivity = 0.2f;

    MirrorConfig finalTargetMirror = MakeMirrorRenderTestConfig("Low Alpha Final Target Mirror", 18, 12, "topLeftScreen", 170, 32, 4.0f);
    finalTargetMirror.rawOutput = false;
    finalTargetMirror.opacity = 1.0f;
    finalTargetMirror.border.dynamicThickness = 2;
    finalTargetMirror.colors.targetColors = { { 0.0f, 1.0f, 0.0f, 1.0f } };
    finalTargetMirror.colors.output = { 0.0f, 1.0f, 0.0f, 0.4f };
    finalTargetMirror.colors.border = { 1.0f, 0.8f, 0.1f, 1.0f };
    finalTargetMirror.colorSensitivity = 0.2f;

    ModeConfig mode;
    mode.id = kModeId;
    mode.width = 320;
    mode.height = 200;
    mode.manualWidth = mode.width;
    mode.manualHeight = mode.height;
    mode.sources = { { ModeSourceType::Mirror, compositeMirror.name }, { ModeSourceType::Mirror, finalTargetMirror.name } };

    g_config.defaultMode = kModeId;
    g_config.mirrors = { compositeMirror, finalTargetMirror };
    g_config.modes = { mode };
    g_configLoaded.store(true, std::memory_order_release);

    InitializeMirrorRenderTestResources();

    const SimulatedOverlayGeometry geometry{ "low-alpha-mirrors", 320, 200, 0, 0, 320, 200 };
    ScopedTexture2D sourceTexture(geometry.fullW, geometry.fullH,
                                  MakeSolidRgbaPixels(geometry.fullW, geometry.fullH, 0, 255, 0));

    auto renderAndAssert = [&](DummyWindow& targetWindow) {
        RenderModeOverlayFrameToSimulatedSurface(targetWindow, g_config, g_config.modes.front(), geometry, sourceTexture.id(),
                                                 [&](const SurfaceSize& surface) {
            if (runMode != TestRunMode::Automated) {
                return;
            }

            std::vector<MirrorConfig> activeMirrors;
            std::vector<ImageConfig> unusedImages;
            std::vector<const WindowOverlayConfig*> unusedWindowOverlays;
            std::vector<const BrowserOverlayConfig*> unusedBrowserOverlays;
            CollectActiveElementsForMode(g_config, kModeId, false, 0, activeMirrors, unusedImages, unusedWindowOverlays,
                                         unusedBrowserOverlays, geometry.fullW, geometry.fullH);

            Expect(activeMirrors.size() == 2,
                   "Expected both low-alpha mirrors to resolve for the low-alpha visibility regression test.");

            const ExpectedMirrorRect compositeRect = GetCachedMirrorRect(compositeMirror.name);
            const ExpectedMirrorRect finalTargetRect = GetCachedMirrorRect(finalTargetMirror.name);

            Expect(compositeRect.width > 0 && compositeRect.height > 0,
                   "Expected the low-opacity composite mirror to cache a positive output size.");
            Expect(finalTargetRect.width > 0 && finalTargetRect.height > 0,
                   "Expected the low-alpha final-target mirror to cache a positive output size.");

            ExpectFramebufferPixelChannelDominance(compositeRect.x + compositeRect.width / 2,
                                                   compositeRect.y + compositeRect.height / 2, surface.height, 1, 0.32f,
                                                   0.18f,
                                                   "Expected the dynamic-composite low-opacity mirror center to remain visible.");
            ExpectFramebufferPixelChannelDominance(finalTargetRect.x + finalTargetRect.width / 2,
                                                   finalTargetRect.y + finalTargetRect.height / 2, surface.height, 1, 0.32f,
                                                   0.18f,
                                                   "Expected the final-target low-alpha mirror center to remain visible.");

            if (compositeRect.x > 2) {
                ExpectBackgroundPixel(compositeRect.x - 2, compositeRect.y + compositeRect.height / 2, surface.height,
                                      "Expected pixels left of the low-opacity composite mirror to remain background.");
            }
            const int finalTargetOutsideX = finalTargetRect.x + finalTargetRect.width + 2;
            if (finalTargetOutsideX < surface.width) {
                ExpectBackgroundPixel(finalTargetOutsideX, finalTargetRect.y + finalTargetRect.height / 2, surface.height,
                                      "Expected pixels right of the low-alpha final-target mirror to remain background.");
            }
        });
    };

    if (runMode == TestRunMode::Visual) {
        RunVisualLoop(window, "mode-mirror-render-low-alpha-visibility",
                      [&](DummyWindow& visualWindow) { renderAndAssert(visualWindow); });
    } else {
        renderAndAssert(window);
    }

    CleanupBrowserOverlayCache();
    CleanupWindowOverlayCache();
    CleanupGPUResources();
    CleanupShaders();
}

void RunModeMirrorGroupRenderTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    if (!g_hasModernGL) { std::cout << "SKIP (no GL 3.3+)" << std::endl; return; }

    const std::filesystem::path root = PrepareCaseDirectory("mode_mirror_group_render");
    ResetGlobalTestState(root);

    constexpr char kModeId[] = "Mirror Group Render Mode";
    constexpr char kGroupName[] = "Mirror Group Under Test";

    MirrorConfig leftMirror = MakeMirrorRenderTestConfig("Group Left Mirror", 24, 16, "topLeftScreen", 0, 0, 4.0f);
    MirrorConfig disabledMirror = MakeMirrorRenderTestConfig("Disabled Group Mirror", 14, 14, "topLeftScreen", 0, 0, 6.0f);

    MirrorGroupConfig group;
    group.name = kGroupName;
    group.output.x = 300;
    group.output.y = 220;
    group.output.relativeTo = "topLeftScreen";
    group.output.separateScale = true;
    group.output.scaleX = 1.0f;
    group.output.scaleY = 1.0f;
    group.mirrors = {
        { leftMirror.name, true, 0.5f, 0.5f, 10, 20 },
        { disabledMirror.name, false, 1.0f, 1.0f, 210, 30 },
    };

    ModeConfig mode;
    mode.id = kModeId;
    mode.width = kWindowWidth;
    mode.height = kWindowHeight;
    mode.manualWidth = kWindowWidth;
    mode.manualHeight = kWindowHeight;
    mode.sources = { { ModeSourceType::MirrorGroup, kGroupName } };

    g_config.defaultMode = kModeId;
    g_config.mirrors = { leftMirror, disabledMirror };
    g_config.mirrorGroups = { group };
    g_config.modes = { mode };
    g_configLoaded.store(true, std::memory_order_release);

    InitializeMirrorRenderTestResources();

    const SurfaceSize surface = GetWindowClientSize(window.hwnd());
    ScopedTexture2D sourceTexture(surface.width, surface.height, MakeSolidRgbaPixels(surface.width, surface.height, 0, 255, 0));

    auto renderAndAssert = [&](DummyWindow& targetWindow) {
        RenderModeOverlayFrame(targetWindow, g_config, g_config.modes.front(), sourceTexture.id());
        if (runMode == TestRunMode::Automated) {
            std::vector<MirrorConfig> activeMirrors;
            std::vector<ImageConfig> unusedImages;
            std::vector<const WindowOverlayConfig*> unusedWindowOverlays;
            std::vector<const BrowserOverlayConfig*> unusedBrowserOverlays;
            CollectActiveElementsForMode(g_config, kModeId, false, 0, activeMirrors, unusedImages, unusedWindowOverlays,
                                         unusedBrowserOverlays);

            Expect(activeMirrors.size() == 1, "Expected only enabled group mirrors to resolve for the mirror-group render test.");
            Expect(activeMirrors[0].name == leftMirror.name, "Expected the enabled group mirror to preserve group order.");
            ExpectFloatNear(activeMirrors[0].output.scaleX, group.mirrors[0].widthPercent,
                            "Expected a grouped mirror to take its width from the group item alone, not its own 4x output scale.");
            ExpectFloatNear(activeMirrors[0].output.scaleY, group.mirrors[0].heightPercent,
                            "Expected a grouped mirror to take its height from the group item alone, not its own 4x output scale.");

            const ExpectedMirrorRect expectedLeftRect = ComputeExpectedMirrorRect(activeMirrors[0], surface.width, surface.height, 0, 0,
                                                                                  surface.width, surface.height);
            const ExpectedMirrorRect leftRect = GetCachedMirrorRect(leftMirror.name);
            const MirrorConfig disabledGroupMirror = BuildExpectedGroupedMirrorConfig(disabledMirror, group, group.mirrors[1]);
            const ExpectedMirrorRect disabledRect = ComputeExpectedMirrorRect(disabledGroupMirror, surface.width, surface.height,
                                                                              0, 0, surface.width, surface.height);

            Expect(leftRect.x == expectedLeftRect.x && leftRect.y == expectedLeftRect.y && leftRect.width == expectedLeftRect.width &&
                       leftRect.height == expectedLeftRect.height,
                   "Expected the first mirror-group member cached bounds to match the grouped placement math.");

            ExpectSolidColorRect(leftRect, surface.height, kExpectedMirrorRenderGreen,
                                 "Expected the enabled mirror-group member to render its staged texture.");

            ExpectBackgroundPixel(leftRect.x - 2, leftRect.y + leftRect.height / 2, surface.height,
                                  "Expected pixels left of the enabled group mirror to remain background.");
            ExpectBackgroundPixel(disabledRect.x + disabledRect.width / 2, disabledRect.y + disabledRect.height / 2, surface.height,
                                  "Expected the disabled mirror-group item to remain absent from the render output.");
        }
    };

    if (runMode == TestRunMode::Visual) {
        RunVisualLoop(window, "mode-mirror-group-render", [&](DummyWindow& visualWindow) { renderAndAssert(visualWindow); });
    } else {
        renderAndAssert(window);
    }

    CleanupBrowserOverlayCache();
    CleanupWindowOverlayCache();
    CleanupGPUResources();
    CleanupShaders();
}

void RunModeMirrorGroupRelativePositionResolutionTest(TestRunMode runMode = TestRunMode::Automated) {
    (void)runMode;

    const std::filesystem::path root = PrepareCaseDirectory("mode_mirror_group_relative_position_resolution");
    ResetGlobalTestState(root);

    constexpr char kModeId[] = "Mirror Group Relative Position Resolution";
    constexpr char kGroupName[] = "Relative Screen Mirror Group";
    constexpr int kOffsetX = 12;
    constexpr int kOffsetY = -8;

    MirrorConfig mirror = MakeMirrorRenderTestConfig("Relative Group Mirror", 24, 16, "topLeftScreen", 0, 0, 4.0f);

    MirrorGroupConfig group;
    group.name = kGroupName;
    group.output.relativeTo = "topLeftScreen";
    group.output.useRelativePosition = true;
    group.output.relativeX = 0.25f;
    group.output.relativeY = 0.5f;
    group.mirrors = {
        { mirror.name, true, 1.0f, 1.0f, kOffsetX, kOffsetY },
    };

    ModeConfig mode;
    mode.id = kModeId;
    mode.width = 200;
    mode.height = 100;
    mode.manualWidth = 200;
    mode.manualHeight = 100;
    mode.sources = { { ModeSourceType::MirrorGroup, kGroupName } };

    g_config.defaultMode = kModeId;
    g_config.mirrors = { mirror };
    g_config.mirrorGroups = { group };
    g_config.modes = { mode };
    g_configLoaded.store(true, std::memory_order_release);

    std::vector<MirrorConfig> sourceMirrors;
    std::vector<ImageConfig> unusedSourceImages;
    std::vector<const WindowOverlayConfig*> unusedSourceWindowOverlays;
    std::vector<const BrowserOverlayConfig*> unusedSourceBrowserOverlays;
    CollectActiveElementsForMode(g_config, kModeId, false, 0, sourceMirrors, unusedSourceImages, unusedSourceWindowOverlays,
                                 unusedSourceBrowserOverlays, 200, 100);

    std::vector<MirrorConfig> targetMirrors;
    std::vector<ImageConfig> unusedTargetImages;
    std::vector<const WindowOverlayConfig*> unusedTargetWindowOverlays;
    std::vector<const BrowserOverlayConfig*> unusedTargetBrowserOverlays;
    CollectActiveElementsForMode(g_config, kModeId, false, 0, targetMirrors, unusedTargetImages, unusedTargetWindowOverlays,
                                 unusedTargetBrowserOverlays, 400, 200);

    Expect(sourceMirrors.size() == 1,
           "Expected one grouped mirror to resolve for the source relative-position collection.");
    Expect(targetMirrors.size() == 1,
           "Expected one grouped mirror to resolve for the target relative-position collection.");

    Expect(sourceMirrors[0].output.x == 62,
           "Expected source grouped mirror X to use the explicit source screen width before adding the item offset.");
    Expect(sourceMirrors[0].output.y == 42,
           "Expected source grouped mirror Y to use the explicit source screen height before adding the item offset.");
    Expect(targetMirrors[0].output.x == 112,
           "Expected target grouped mirror X to use the explicit target screen width before adding the item offset.");
    Expect(targetMirrors[0].output.y == 92,
           "Expected target grouped mirror Y to use the explicit target screen height before adding the item offset.");
    Expect(sourceMirrors[0].output.relativeTo == group.output.relativeTo,
           "Expected grouped mirrors to preserve the mirror-group anchor when resolving explicit screen sizes.");
}

    void RunModeMirrorGroupSlideUnitTransitionTest(TestRunMode runMode = TestRunMode::Automated) {
        DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
        if (!g_hasModernGL) { std::cout << "SKIP (no GL 3.3+)" << std::endl; return; }

        const std::filesystem::path root = PrepareCaseDirectory("mode_mirror_group_slide_unit_transition");
        ResetGlobalTestState(root);

        constexpr char kSourceModeId[] = "Mirror Group Slide Source";
        constexpr char kTargetModeId[] = "Mirror Group Slide Target";
        constexpr char kGroupName[] = "Sliding Mirror Group";
        constexpr float kTransitionProgress = 0.5f;

        MirrorConfig leftMirror = MakeMirrorRenderTestConfig("Sliding Group Left Mirror", 20, 12, "topLeftViewport", 0, 0, 4.0f);
        MirrorConfig rightMirror = MakeMirrorRenderTestConfig("Sliding Group Right Mirror", 20, 12, "topLeftViewport", 0, 0, 4.0f);
        leftMirror.input = { { 0, 0, "topLeftScreen" } };
        rightMirror.input = { { 40, 0, "topLeftScreen" } };

        MirrorGroupConfig group;
        group.name = kGroupName;
        group.output.x = 80;
        group.output.y = 60;
        group.output.relativeTo = "topLeftViewport";
        group.mirrors = {
         { leftMirror.name, true, 1.0f, 1.0f, 0, 0 },
         { rightMirror.name, true, 1.0f, 1.0f, 120, 0 },
        };

        ModeConfig sourceMode;
        sourceMode.id = kSourceModeId;
        sourceMode.width = 700;
        sourceMode.height = 420;
        sourceMode.manualWidth = 700;
        sourceMode.manualHeight = 420;

        ModeConfig targetMode;
        targetMode.id = kTargetModeId;
        targetMode.width = 1000;
        targetMode.height = 600;
        targetMode.manualWidth = 1000;
        targetMode.manualHeight = 600;
        targetMode.slideMirrorsIn = true;
        targetMode.sources = { { ModeSourceType::MirrorGroup, kGroupName } };

        g_config.defaultMode = kTargetModeId;
        g_config.mirrors = { leftMirror, rightMirror };
        g_config.mirrorGroups = { group };
        g_config.modes = { sourceMode, targetMode };
        g_configLoaded.store(true, std::memory_order_release);
        PublishConfigSnapshot();

        InitializeMirrorRenderTestResources();

        const SurfaceSize surface = GetWindowClientSize(window.hwnd());
        std::vector<unsigned char> sourcePixels(static_cast<size_t>(surface.width) * static_cast<size_t>(surface.height) * 4u, 0);
        auto fillVerticalStripe = [&](int startX, int stripeWidth, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
            for (int y = 0; y < surface.height; ++y) {
                for (int x = startX; x < (std::min)(surface.width, startX + stripeWidth); ++x) {
                    const size_t pixelIndex = (static_cast<size_t>(y) * static_cast<size_t>(surface.width) + static_cast<size_t>(x)) * 4u;
                    sourcePixels[pixelIndex + 0] = r;
                    sourcePixels[pixelIndex + 1] = g;
                    sourcePixels[pixelIndex + 2] = b;
                    sourcePixels[pixelIndex + 3] = 255;
                }
            }
        };
        fillVerticalStripe(0, 20, 0, 255, 0);
        fillVerticalStripe(40, 20, 0, 0, 255);
        ScopedTexture2D sourceTexture(surface.width, surface.height, sourcePixels);

        struct ScopedCachedGameTextureOverride {
         GLuint previousTextureId = 0;

         explicit ScopedCachedGameTextureOverride(GLuint textureId) {
             previousTextureId = g_cachedGameTextureId.load(std::memory_order_acquire);
             g_cachedGameTextureId.store(textureId, std::memory_order_release);
         }

         ~ScopedCachedGameTextureOverride() {
             g_cachedGameTextureId.store(previousTextureId, std::memory_order_release);
         }
        } scopedGameTexture(sourceTexture.id());

        const int fromX = GetCenteredAxisOffset(surface.width, sourceMode.width);
        const int fromY = GetCenteredAxisOffset(surface.height, sourceMode.height);
        const int toX = GetCenteredAxisOffset(surface.width, targetMode.width);
        const int toY = GetCenteredAxisOffset(surface.height, targetMode.height);

        ViewportTransitionSnapshot snapshot;
        snapshot.active = true;
        snapshot.isBounceTransition = true;
        snapshot.fromModeId = kSourceModeId;
        snapshot.toModeId = kTargetModeId;
        snapshot.fromWidth = sourceMode.width;
        snapshot.fromHeight = sourceMode.height;
        snapshot.fromX = fromX;
        snapshot.fromY = fromY;
        snapshot.currentWidth = static_cast<int>(sourceMode.width + (targetMode.width - sourceMode.width) * kTransitionProgress);
        snapshot.currentHeight = static_cast<int>(sourceMode.height + (targetMode.height - sourceMode.height) * kTransitionProgress);
        snapshot.currentX = static_cast<int>(fromX + (toX - fromX) * kTransitionProgress);
        snapshot.currentY = static_cast<int>(fromY + (toY - fromY) * kTransitionProgress);
        snapshot.toWidth = targetMode.width;
        snapshot.toHeight = targetMode.height;
        snapshot.toX = toX;
        snapshot.toY = toY;
        snapshot.fromNativeWidth = surface.width;
        snapshot.fromNativeHeight = surface.height;
        snapshot.toNativeWidth = surface.width;
        snapshot.toNativeHeight = surface.height;
        snapshot.gameTransition = GameTransitionType::Bounce;
        snapshot.overlayTransition = OverlayTransitionType::Cut;
        snapshot.backgroundTransition = BackgroundTransitionType::Cut;
        snapshot.progress = kTransitionProgress;
        snapshot.moveProgress = kTransitionProgress;

        struct ScopedViewportTransitionSnapshot {
         ViewportTransitionSnapshot previousSnapshots[2];
         int previousIndex = 0;

         explicit ScopedViewportTransitionSnapshot(const ViewportTransitionSnapshot& activeSnapshot) {
             previousSnapshots[0] = g_viewportTransitionSnapshots[0];
             previousSnapshots[1] = g_viewportTransitionSnapshots[1];
             previousIndex = g_viewportTransitionSnapshotIndex.load(std::memory_order_acquire);
             g_viewportTransitionSnapshots[0] = activeSnapshot;
             g_viewportTransitionSnapshots[1] = activeSnapshot;
             g_viewportTransitionSnapshotIndex.store(0, std::memory_order_release);
         }

         ~ScopedViewportTransitionSnapshot() {
             g_viewportTransitionSnapshots[0] = previousSnapshots[0];
             g_viewportTransitionSnapshots[1] = previousSnapshots[1];
             g_viewportTransitionSnapshotIndex.store(previousIndex, std::memory_order_release);
         }
        } scopedTransitionSnapshot(snapshot);

        auto renderAndAssert = [&](DummyWindow& targetWindow) {
         Expect(targetWindow.PrepareRenderSurface(),
             "GUI integration test window closed unexpectedly while rendering the grouped slide transition case.");

         GLState state{};
         SaveGLState(&state);
         RenderMode(&targetMode, state, surface.width, surface.height, false, false);
         glFinish();

         if (runMode == TestRunMode::Automated) {
             std::vector<MirrorConfig> activeMirrors;
             std::vector<ImageConfig> unusedImages;
             std::vector<const WindowOverlayConfig*> unusedWindowOverlays;
             std::vector<const BrowserOverlayConfig*> unusedBrowserOverlays;
             CollectActiveElementsForMode(g_config, kTargetModeId, false, 0, activeMirrors, unusedImages, unusedWindowOverlays,
                              unusedBrowserOverlays, surface.width, surface.height);

             Expect(activeMirrors.size() == 2,
                 "Expected both grouped mirrors to resolve for the grouped slide transition regression test.");
             Expect(activeMirrors[0].name == leftMirror.name,
                 "Expected the grouped slide regression to preserve the first mirror ordering.");
             Expect(activeMirrors[1].name == rightMirror.name,
                 "Expected the grouped slide regression to preserve the second mirror ordering.");

             const ExpectedMirrorRect expectedLeftRect = ComputeExpectedMirrorRect(activeMirrors[0], surface.width, surface.height,
                                                     toX, toY, targetMode.width, targetMode.height);
             const ExpectedMirrorRect expectedRightRect = ComputeExpectedMirrorRect(activeMirrors[1], surface.width, surface.height,
                                                      toX, toY, targetMode.width, targetMode.height);
                 const Color expectedRightColor{ 0.0f, 0.0f, 1.0f, 1.0f };
                 struct ColorSpan {
                  int start = -1;
                  int end = -1;
                 };

                 auto findHorizontalSpan = [&](const Color& expectedColor, int scanY, const std::string& label) {
                  ColorSpan span;
                  for (int x = 0; x < surface.width; ++x) {
                      if (IsColorNear(ReadFramebufferPixelColor(x, scanY, surface.height), expectedColor)) {
                       if (span.start < 0) {
                        span.start = x;
                       }
                       span.end = x;
                      } else if (span.start >= 0) {
                       break;
                      }
                  }

                  Expect(span.start >= 0 && span.end >= span.start,
                      "Expected to find a visible horizontal color span for " + label + ".");
                  return span;
                 };

                 auto findVerticalSpan = [&](const Color& expectedColor, int scanX, const std::string& label) {
                  ColorSpan span;
                  for (int y = 0; y < surface.height; ++y) {
                      if (IsColorNear(ReadFramebufferPixelColor(scanX, y, surface.height), expectedColor)) {
                       if (span.start < 0) {
                        span.start = y;
                       }
                       span.end = y;
                      } else if (span.start >= 0) {
                       break;
                      }
                  }

                  Expect(span.start >= 0 && span.end >= span.start,
                      "Expected to find a visible vertical color span for " + label + ".");
                  return span;
                 };

                 const int scanY = expectedLeftRect.y + expectedLeftRect.height / 2;
                 const ColorSpan leftHorizontalSpan = findHorizontalSpan(kExpectedMirrorRenderGreen, scanY, leftMirror.name);
                 const ColorSpan rightHorizontalSpan = findHorizontalSpan(expectedRightColor, scanY, rightMirror.name);
                 const int leftCenterX = leftHorizontalSpan.start + (leftHorizontalSpan.end - leftHorizontalSpan.start) / 2;
                 const int rightCenterX = rightHorizontalSpan.start + (rightHorizontalSpan.end - rightHorizontalSpan.start) / 2;
                 const ColorSpan leftVerticalSpan = findVerticalSpan(kExpectedMirrorRenderGreen, leftCenterX, leftMirror.name);
                 const ColorSpan rightVerticalSpan = findVerticalSpan(expectedRightColor, rightCenterX, rightMirror.name);

                 const ExpectedMirrorRect actualLeftRect{
                  leftMirror.name,
                  leftHorizontalSpan.start,
                  leftVerticalSpan.start,
                  leftHorizontalSpan.end - leftHorizontalSpan.start + 1,
                  leftVerticalSpan.end - leftVerticalSpan.start + 1,
                 };
                 const ExpectedMirrorRect actualRightRect{
                  rightMirror.name,
                  rightHorizontalSpan.start,
                  rightVerticalSpan.start,
                  rightHorizontalSpan.end - rightHorizontalSpan.start + 1,
                  rightVerticalSpan.end - rightVerticalSpan.start + 1,
                 };

                 Expect(std::abs(actualLeftRect.y - expectedLeftRect.y) <= 2,
                     "Expected the left grouped mirror to keep its target Y position during slide-in.");
                 Expect(std::abs(actualRightRect.y - expectedRightRect.y) <= 2,
                     "Expected the right grouped mirror to keep its target Y position during slide-in.");
                 Expect(std::abs(actualLeftRect.width - expectedLeftRect.width) <= 2,
                     "Expected the left grouped mirror width to stay stable during slide-in.");
                 Expect(std::abs(actualRightRect.width - expectedRightRect.width) <= 2,
                     "Expected the right grouped mirror width to stay stable during slide-in.");
                 Expect(std::abs(actualLeftRect.height - expectedLeftRect.height) <= 2,
                     "Expected the left grouped mirror height to stay stable during slide-in.");
                 Expect(std::abs(actualRightRect.height - expectedRightRect.height) <= 2,
                     "Expected the right grouped mirror height to stay stable during slide-in.");

             const int leftDeltaX = actualLeftRect.x - expectedLeftRect.x;
             const int rightDeltaX = actualRightRect.x - expectedRightRect.x;
             Expect(leftDeltaX < -10,
                 "Expected the left grouped mirror to still be sliding in from off-screen at mid-transition.");
             Expect(rightDeltaX < -10,
                 "Expected the right grouped mirror to still be sliding in from off-screen at mid-transition.");
             Expect(std::abs(leftDeltaX - rightDeltaX) <= 2,
                 "Expected grouped mirrors to share the same slide translation during a mode transition.");

             const int actualSpacing = actualRightRect.x - actualLeftRect.x;
             const int expectedSpacing = expectedRightRect.x - expectedLeftRect.x;
             Expect(std::abs(actualSpacing - expectedSpacing) <= 2,
                 "Expected grouped mirrors to preserve their relative X spacing while the group slides in.");
         }
        };

        if (runMode == TestRunMode::Visual) {
         RunVisualLoop(window, "mode-mirror-group-slide-unit-transition",
                 [&](DummyWindow& visualWindow) { renderAndAssert(visualWindow); });
        } else {
         renderAndAssert(window);
        }

        CleanupBrowserOverlayCache();
        CleanupWindowOverlayCache();
        CleanupGPUResources();
        CleanupShaders();
    }

void RunModeWindowOverlayRenderTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    if (!g_hasModernGL) { std::cout << "SKIP (no GL 3.3+)" << std::endl; return; }

    const std::filesystem::path root = PrepareCaseDirectory("mode_window_overlay_render");
    ResetGlobalTestState(root);

    constexpr char kModeId[] = "Window Overlay Render Mode";
    constexpr char kOverlayName[] = "Window Overlay Render";
    constexpr int kOverlayX = 48;
    constexpr int kOverlayY = 64;

    WindowOverlayConfig overlay;
    overlay.name = kOverlayName;
    overlay.x = kOverlayX;
    overlay.y = kOverlayY;
    overlay.scale = 16.0f;
    overlay.relativeTo = "topLeftScreen";
    overlay.opacity = 1.0f;
    overlay.onlyOnMyScreen = false;
    overlay.pixelatedScaling = true;
    overlay.background.enabled = false;
    overlay.border.enabled = false;

    ModeConfig mode;
    mode.id = kModeId;
    mode.width = kWindowWidth;
    mode.height = kWindowHeight;
    mode.manualWidth = kWindowWidth;
    mode.manualHeight = kWindowHeight;
    mode.sources = { { ModeSourceType::WindowOverlay, kOverlayName } };

    g_config.defaultMode = kModeId;
    g_config.windowOverlays = { overlay };
    g_config.modes = { mode };
    g_configLoaded.store(true, std::memory_order_release);

    ResetOverlayRenderTestResources();
    Expect(StageWindowOverlayTestFrame(overlay, MakeSolidRgbaPixels(2, 2, 255, 64, 32), 2, 2),
           "Failed to stage synthetic window overlay pixels for integration testing.");

    auto renderAndAssert = [&](DummyWindow& targetWindow) {
        RenderModeOverlayFrame(targetWindow, g_config, g_config.modes.front());
        if (runMode == TestRunMode::Automated) {
            ExpectFramebufferPixelColorNear(kOverlayX + 12, kOverlayY + 12, GetCachedWindowHeight(),
                                            { 1.0f, 64.0f / 255.0f, 32.0f / 255.0f, 1.0f },
                                            "Expected the mode-assigned window overlay to render its staged texture color.");
        }
    };

    if (runMode == TestRunMode::Visual) {
        RunVisualLoop(window, "mode-window-overlay-render", [&](DummyWindow& visualWindow) { renderAndAssert(visualWindow); });
    } else {
        renderAndAssert(window);
    }

    CleanupBrowserOverlayCache();
    CleanupWindowOverlayCache();
    CleanupGPUResources();
    CleanupShaders();
}

void RunModeBrowserOverlayRenderTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    if (!g_hasModernGL) { std::cout << "SKIP (no GL 3.3+)" << std::endl; return; }

    const std::filesystem::path root = PrepareCaseDirectory("mode_browser_overlay_render");
    ResetGlobalTestState(root);

    constexpr char kModeId[] = "Browser Overlay Render Mode";
    constexpr char kOverlayName[] = "Browser Overlay Render";
    constexpr int kOverlayX = 132;
    constexpr int kOverlayY = 96;

    BrowserOverlayConfig overlay;
    overlay.name = kOverlayName;
    overlay.url = "https://example.com/render-test";
    overlay.browserWidth = 2;
    overlay.browserHeight = 2;
    overlay.x = kOverlayX;
    overlay.y = kOverlayY;
    overlay.scale = 18.0f;
    overlay.relativeTo = "topLeftScreen";
    overlay.opacity = 1.0f;
    overlay.onlyOnMyScreen = false;
    overlay.pixelatedScaling = true;
    overlay.background.enabled = false;
    overlay.border.enabled = false;

    ModeConfig mode;
    mode.id = kModeId;
    mode.width = kWindowWidth;
    mode.height = kWindowHeight;
    mode.manualWidth = kWindowWidth;
    mode.manualHeight = kWindowHeight;
    mode.sources = { { ModeSourceType::BrowserOverlay, kOverlayName } };

    g_config.defaultMode = kModeId;
    g_config.browserOverlays = { overlay };
    g_config.modes = { mode };
    g_configLoaded.store(true, std::memory_order_release);

    ResetOverlayRenderTestResources();
    Expect(StageBrowserOverlayTestFrame(overlay, MakeSolidRgbaPixels(2, 2, 32, 192, 96), 2, 2),
           "Failed to stage synthetic browser overlay pixels for integration testing.");

    auto renderAndAssert = [&](DummyWindow& targetWindow) {
        RenderModeOverlayFrame(targetWindow, g_config, g_config.modes.front());
        if (runMode == TestRunMode::Automated) {
            ExpectFramebufferPixelColorNear(kOverlayX + 12, kOverlayY + 12, GetCachedWindowHeight(),
                                            { 32.0f / 255.0f, 192.0f / 255.0f, 96.0f / 255.0f, 1.0f },
                                            "Expected the mode-assigned browser overlay to render its staged texture color.");
        }
    };

    if (runMode == TestRunMode::Visual) {
        RunVisualLoop(window, "mode-browser-overlay-render", [&](DummyWindow& visualWindow) { renderAndAssert(visualWindow); });
    } else {
        renderAndAssert(window);
    }

    CleanupBrowserOverlayCache();
    CleanupWindowOverlayCache();
    CleanupGPUResources();
    CleanupShaders();
}

void RunModeWindowOverlayRenderResetsBlendEquationTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    if (!g_hasModernGL) { std::cout << "SKIP (no GL 3.3+)" << std::endl; return; }

    const std::filesystem::path root = PrepareCaseDirectory("mode_window_overlay_render_resets_blend_equation");
    ResetGlobalTestState(root);

    constexpr char kModeId[] = "Window Overlay Hostile Blend Mode";
    constexpr char kOverlayName[] = "Window Overlay Hostile Blend";
    constexpr int kOverlayX = 88;
    constexpr int kOverlayY = 120;

    WindowOverlayConfig overlay;
    overlay.name = kOverlayName;
    overlay.x = kOverlayX;
    overlay.y = kOverlayY;
    overlay.scale = 20.0f;
    overlay.relativeTo = "topLeftScreen";
    overlay.opacity = 1.0f;
    overlay.onlyOnMyScreen = false;
    overlay.pixelatedScaling = true;
    overlay.background.enabled = false;
    overlay.border.enabled = false;

    ModeConfig mode;
    mode.id = kModeId;
    mode.width = kWindowWidth;
    mode.height = kWindowHeight;
    mode.manualWidth = kWindowWidth;
    mode.manualHeight = kWindowHeight;
    mode.sources = { { ModeSourceType::WindowOverlay, kOverlayName } };

    g_config.defaultMode = kModeId;
    g_config.windowOverlays = { overlay };
    g_config.modes = { mode };
    g_configLoaded.store(true, std::memory_order_release);

    ResetOverlayRenderTestResources();
    Expect(StageWindowOverlayTestFrame(overlay, MakeSolidRgbaPixels(2, 2, 255, 64, 32), 2, 2),
           "Failed to stage hostile-blend window overlay pixels for integration testing.");

    auto renderAndAssert = [&](DummyWindow& targetWindow) {
        glEnable(GL_BLEND);
        glBlendEquationSeparate(GL_FUNC_REVERSE_SUBTRACT, GL_FUNC_ADD);

        RenderModeOverlayFrame(targetWindow, g_config, g_config.modes.front());
        if (runMode == TestRunMode::Automated) {
            ExpectFramebufferPixelColorNear(kOverlayX + 12, kOverlayY + 12, GetCachedWindowHeight(),
                                            { 1.0f, 64.0f / 255.0f, 32.0f / 255.0f, 1.0f },
                                            "Expected the overlay pass to reset hostile blend equations before drawing overlays.");
        }
    };

    if (runMode == TestRunMode::Visual) {
        RunVisualLoop(window, "mode-window-overlay-render-resets-blend-equation",
                      [&](DummyWindow& visualWindow) { renderAndAssert(visualWindow); });
    } else {
        renderAndAssert(window);
    }

    CleanupBrowserOverlayCache();
    CleanupWindowOverlayCache();
    CleanupGPUResources();
    CleanupShaders();
}

void RunModeWindowOverlayRenderUnbindsSamplerTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    if (!g_hasModernGL) { std::cout << "SKIP (no GL 3.3+)" << std::endl; return; }
    if (!(GLEW_VERSION_3_3 || GLEW_ARB_sampler_objects)) {
        std::cout << "SKIP (no sampler object support)" << std::endl;
        return;
    }

    const std::filesystem::path root = PrepareCaseDirectory("mode_window_overlay_render_unbinds_sampler");
    ResetGlobalTestState(root);

    constexpr char kModeId[] = "Window Overlay Hostile Sampler Mode";
    constexpr char kOverlayName[] = "Window Overlay Hostile Sampler";
    constexpr int kOverlayX = 104;
    constexpr int kOverlayY = 136;

    WindowOverlayConfig overlay;
    overlay.name = kOverlayName;
    overlay.x = kOverlayX;
    overlay.y = kOverlayY;
    overlay.scale = 20.0f;
    overlay.relativeTo = "topLeftScreen";
    overlay.opacity = 1.0f;
    overlay.onlyOnMyScreen = false;
    overlay.pixelatedScaling = true;
    overlay.background.enabled = false;
    overlay.border.enabled = false;

    ModeConfig mode;
    mode.id = kModeId;
    mode.width = kWindowWidth;
    mode.height = kWindowHeight;
    mode.manualWidth = kWindowWidth;
    mode.manualHeight = kWindowHeight;
    mode.sources = { { ModeSourceType::WindowOverlay, kOverlayName } };

    g_config.defaultMode = kModeId;
    g_config.windowOverlays = { overlay };
    g_config.modes = { mode };
    g_configLoaded.store(true, std::memory_order_release);

    ResetOverlayRenderTestResources();
    Expect(StageWindowOverlayTestFrame(overlay, MakeSolidRgbaPixels(2, 2, 255, 64, 32), 2, 2),
           "Failed to stage hostile-sampler window overlay pixels for integration testing.");

    auto renderAndAssert = [&](DummyWindow& targetWindow) {
        GLuint hostileSampler = 0;
        glGenSamplers(1, &hostileSampler);
        Expect(hostileSampler != 0, "Failed to create a hostile sampler object for integration testing.");

        glSamplerParameteri(hostileSampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glSamplerParameteri(hostileSampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindSampler(0, hostileSampler);

        RenderModeOverlayFrame(targetWindow, g_config, g_config.modes.front());

        glBindSampler(0, 0);
        glDeleteSamplers(1, &hostileSampler);

        if (runMode == TestRunMode::Automated) {
            ExpectFramebufferPixelColorNear(kOverlayX + 12, kOverlayY + 12, GetCachedWindowHeight(),
                                            { 1.0f, 64.0f / 255.0f, 32.0f / 255.0f, 1.0f },
                                            "Expected the overlay pass to unbind hostile sampler objects before sampling textures.");
        }
    };

    if (runMode == TestRunMode::Visual) {
        RunVisualLoop(window, "mode-window-overlay-render-unbinds-sampler",
                      [&](DummyWindow& visualWindow) { renderAndAssert(visualWindow); });
    } else {
        renderAndAssert(window);
    }

    CleanupBrowserOverlayCache();
    CleanupWindowOverlayCache();
    CleanupGPUResources();
    CleanupShaders();
}

void RunModeImageOverlayRenderPngTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    if (!g_hasModernGL) { std::cout << "SKIP (no GL 3.3+)" << std::endl; return; }

    const std::filesystem::path root = PrepareCaseDirectory("mode_image_overlay_render_png");
    ResetGlobalTestState(root);

    constexpr char kModeId[] = "Image Overlay Render PNG Mode";
    constexpr char kImageName[] = "PNG Overlay Render";
    constexpr char kMirrorName[] = "PNG Overlay Mirror";
    constexpr int kImageX = 84;
    constexpr int kImageY = 72;

    const std::filesystem::path relativeFixturePath = std::filesystem::path("fixtures") / "render-fixture.png";
    WriteEmbeddedFixtureToDisk(root, relativeFixturePath, kEmbeddedPngFixtureBase64);

    ImageConfig image = MakeTopLeftImageRenderTestConfig(kImageName, relativeFixturePath.generic_string(), kImageX, kImageY, 20.0f);

    ModeConfig mode;
    mode.id = kModeId;
    mode.width = kWindowWidth;
    mode.height = kWindowHeight;
    mode.manualWidth = kWindowWidth;
    mode.manualHeight = kWindowHeight;
    mode.sources = { { ModeSourceType::Mirror, kMirrorName }, { ModeSourceType::Image, kImageName } };

    MirrorConfig mirror = MakeMirrorRenderTestConfig(kMirrorName, 1, 1, "bottomRightScreen", 0, 0, 1.0f);

    g_config.defaultMode = kModeId;
    g_config.mirrors = { mirror };
    g_config.images = { image };
    g_config.modes = { mode };
    g_configLoaded.store(true, std::memory_order_release);

    LoadImageFixtureForRenderTest(window, image);
    ScopedTexture2D sourceTexture(1, 1, MakeSolidRgbaPixels(1, 1, 0, 0, 0));

    auto renderAndAssert = [&](DummyWindow& targetWindow) {
        RenderModeOverlayFrame(targetWindow, g_config, g_config.modes.front(), sourceTexture.id());
        if (runMode == TestRunMode::Automated) {
            ExpectFramebufferPixelColorNear(kImageX + 10, kImageY + 10, GetCachedWindowHeight(), kExpectedPngFixtureColor,
                                            "Expected the PNG image overlay fixture to render its decoded texture color.");
        }
    };

    if (runMode == TestRunMode::Visual) {
        RunVisualLoop(window, "mode-image-overlay-render-png", [&](DummyWindow& visualWindow) { renderAndAssert(visualWindow); });
    } else {
        renderAndAssert(window);
    }

    CleanupBrowserOverlayCache();
    CleanupWindowOverlayCache();
    CleanupGPUResources();
    CleanupShaders();
}

void RunModeImageOverlayRenderMpegTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    if (!g_hasModernGL) { std::cout << "SKIP (no GL 3.3+)" << std::endl; return; }

    const std::filesystem::path root = PrepareCaseDirectory("mode_image_overlay_render_mpeg");
    ResetGlobalTestState(root);

    constexpr char kModeId[] = "Image Overlay Render MPEG Mode";
    constexpr char kImageName[] = "MPEG Overlay Render";
    constexpr char kMirrorName[] = "MPEG Overlay Mirror";
    constexpr int kImageX = 156;
    constexpr int kImageY = 118;

    const std::filesystem::path relativeFixturePath = std::filesystem::path("fixtures") / "render-fixture.mpg";
    WriteEmbeddedFixtureToDisk(root, relativeFixturePath, kEmbeddedMpegFixtureBase64);

    ImageConfig image = MakeTopLeftImageRenderTestConfig(kImageName, relativeFixturePath.generic_string(), kImageX, kImageY, 12.0f);

    ModeConfig mode;
    mode.id = kModeId;
    mode.width = kWindowWidth;
    mode.height = kWindowHeight;
    mode.manualWidth = kWindowWidth;
    mode.manualHeight = kWindowHeight;
    mode.sources = { { ModeSourceType::Mirror, kMirrorName }, { ModeSourceType::Image, kImageName } };

    MirrorConfig mirror = MakeMirrorRenderTestConfig(kMirrorName, 1, 1, "bottomRightScreen", 0, 0, 1.0f);

    g_config.defaultMode = kModeId;
    g_config.mirrors = { mirror };
    g_config.images = { image };
    g_config.modes = { mode };
    g_configLoaded.store(true, std::memory_order_release);

    LoadImageFixtureForRenderTest(window, image);
    ScopedTexture2D sourceTexture(1, 1, MakeSolidRgbaPixels(1, 1, 0, 0, 0));

    auto renderAndAssert = [&](DummyWindow& targetWindow) {
        RenderModeOverlayFrame(targetWindow, g_config, g_config.modes.front(), sourceTexture.id());
        if (runMode == TestRunMode::Automated) {
            ExpectFramebufferPixelChannelDominance(kImageX + 8, kImageY + 8, GetCachedWindowHeight(), 0, 0.35f, 0.10f,
                                                   "Expected the first MPEG fixture frame to remain red-dominant.");

            Sleep(650);

            RenderModeOverlayFrame(targetWindow, g_config, g_config.modes.front(), sourceTexture.id());
            ExpectFramebufferPixelChannelDominance(kImageX + 8, kImageY + 8, GetCachedWindowHeight(), 2, 0.35f, 0.10f,
                                                   "Expected the MPEG fixture playback to advance to a blue-dominant frame.");
        }
    };

    if (runMode == TestRunMode::Visual) {
        RunVisualLoop(window, "mode-image-overlay-render-mpeg", [&](DummyWindow& visualWindow) { renderAndAssert(visualWindow); });
    } else {
        renderAndAssert(window);
    }

    CleanupBrowserOverlayCache();
    CleanupWindowOverlayCache();
    CleanupGPUResources();
    CleanupShaders();
}

void RunModeNinjabrainOverlayRenderTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    if (!g_hasModernGL) { std::cout << "SKIP (no GL 3.3+)" << std::endl; return; }

    const std::filesystem::path root = PrepareCaseDirectory("mode_ninjabrain_overlay_render");
    ResetGlobalTestState(root);

    constexpr char kModeId[] = "Ninjabrain Overlay Render Mode";

    ModeConfig mode;
    mode.id = kModeId;
    mode.width = kWindowWidth;
    mode.height = kWindowHeight;
    mode.manualWidth = kWindowWidth;
    mode.manualHeight = kWindowHeight;

    g_config.defaultMode = kModeId;
    g_config.modes = { mode };
    g_ninjabrainOverlayVisible.store(true, std::memory_order_release);

    auto& nb = g_config.ninjabrainOverlay;
    nb.enabled = true;
    nb.allowedModes = { kModeId };
    nb.relativeTo = "topLeftScreen";
    nb.x = 48;
    nb.y = 40;
    nb.overlayScale = 0.72f;
    nb.overlayOpacity = 1.0f;
    nb.bgEnabled = true;
    nb.bgOpacity = 1.0f;
    nb.showThrowDetails = true;
    nb.staticColumnWidths = true;
    nb.shownPredictions = 5;
    nb.onlyOnMyScreen = false;
    nb.onlyOnObs = false;

    g_configLoaded.store(true, std::memory_order_release);
    PublishConfigSnapshot();

    NinjabrainData data;
    data.resultType = "TRIANGULATION";
    data.validPrediction = true;
    data.predictionCount = 5;
    data.eyeCount = 2;
    data.hasPlayerPos = true;
    data.playerX = 50.33;
    data.playerZ = 319.07;
    data.playerHorizontalAngle = 20.72;

    data.predictions[0] = { -46, 148, 0.601, 2202.0 };
    data.predictions[1] = { -32, 111, 0.399, 1569.0 };
    data.predictions[2] = { -49, 156, 0.0, 2339.0 };
    data.predictions[3] = { -35, 119, 0.0, 1706.0 };
    data.predictions[4] = { -43, 140, 0.0, 2065.0 };

    data.predictionAngles[0] = { 20.26, -106.8, true };
    data.predictionAngles[1] = { 20.07, -106.9, true };
    data.predictionAngles[2] = { 20.27, -106.7, true };
    data.predictionAngles[3] = { 20.11, -106.9, true };
    data.predictionAngles[4] = { 20.24, -106.8, true };

    data.throws[0].xInOverworld = 50.33;
    data.throws[0].zInOverworld = 319.07;
    data.throws[0].hasPosition = true;
    data.throws[0].angle = 20.97;
    data.throws[0].angleWithoutCorrection = 20.72;
    data.throws[0].correction = 0.25;
    data.throws[0].error = -0.0032;
    data.throws[0].correctionIncrements = 2;
    data.throws[0].hasCorrectionIncrements = true;
    data.throws[0].type = "NORMAL";

    data.throws[1].xInOverworld = 59.91;
    data.throws[1].zInOverworld = 470.84;
    data.throws[1].hasPosition = true;
    data.throws[1].angle = 21.34;
    data.throws[1].angleWithoutCorrection = 21.09;
    data.throws[1].correction = -0.25;
    data.throws[1].error = 0.0015;
    data.throws[1].correctionIncrements = -2;
    data.throws[1].hasCorrectionIncrements = true;
    data.throws[1].type = "NORMAL";

    data.lastAngle = data.throws[1].angle;
    data.prevAngle = data.throws[0].angle;
    data.hasAngleChange = true;
    data.lastAngleWithoutCorrection = data.throws[1].angleWithoutCorrection;
    data.lastCorrection = data.throws[1].correction;
    data.lastThrowError = data.throws[1].error;
    data.hasCorrection = true;
    data.hasThrowError = true;
    data.hasNetherAngle = true;
    data.netherAngle = data.throws[1].angle;
    data.netherAngleDiff = data.throws[1].angle - data.throws[0].angle;
    data.strongholdX = data.predictions[0].chunkX * 16 + 4;
    data.strongholdZ = data.predictions[0].chunkZ * 16 + 4;
    data.distance = data.predictions[0].overworldDistance;
    data.certainty = data.predictions[0].certainty;

    data.informationMessageCount = 2;
    data.informationMessages[0].severity = "WARNING";
    data.informationMessages[0].type = "MISMEASURE";
    data.informationMessages[0].message =
        "Detected unusually large errors, you probably mismeasured or your standard deviation is too low.";
    data.informationMessages[1].severity = "INFO";
    data.informationMessages[1].type = "NEXT_THROW_DIRECTION";
    data.informationMessages[1].message =
        "Go left 1 blocks, or right 1 blocks, for ~95% certainty after next measurement.";

    PublishNinjabrainData(data);

        auto configSnapshot = GetConfigSnapshot();
        Expect(configSnapshot != nullptr, "Expected the Ninjabrain render fixture to publish a config snapshot.");
        Expect(configSnapshot->ninjabrainOverlay.enabled,
            "Expected the published Ninjabrain overlay config snapshot to stay enabled.");
        Expect(configSnapshot->ninjabrainOverlay.allowedModes.size() == 1 &&
             configSnapshot->ninjabrainOverlay.allowedModes.front() == kModeId,
            "Expected the published Ninjabrain overlay config snapshot to keep its allowed mode.");

        auto dataSnapshot = GetNinjabrainDataSnapshot();
        Expect(dataSnapshot != nullptr, "Expected the Ninjabrain render fixture to publish a data snapshot.");
        Expect(dataSnapshot->validPrediction && dataSnapshot->resultType == "TRIANGULATION",
            "Expected the published Ninjabrain render fixture data to contain triangulation results.");
        Expect(dataSnapshot->lastUpdateTime != std::chrono::steady_clock::time_point{},
            "Expected the published Ninjabrain render fixture data to record a freshness timestamp.");
        if (const char* renderFailure = GetNinjabrainOverlayRenderEligibilityFailureForIntegrationTest(kModeId)) {
            Expect(false, std::string("Expected the Ninjabrain render fixture to be eligible for rendering, but it was blocked: ") +
                              renderFailure);
        }

    const SurfaceSize surface = GetWindowClientSize(window.hwnd());
    const int sampleX = nb.x + 24;
    const int sampleY = nb.y + 24;

    auto renderAndSample = [&](DummyWindow& targetWindow) {
        RenderModeOverlayFrame(targetWindow, g_config, g_config.modes.front());
        glFinish();
        return ReadFramebufferPixelColor(sampleX, sampleY, surface.height);
    };

    auto renderWithoutOverlayAndSample = [&](DummyWindow& targetWindow) {
        const bool rendered = RenderModeOverlayFrame(targetWindow, g_config, g_config.modes.front(), 0, false, false);
        Expect(!rendered, "Expected stale Ninjabrain overlay data to skip overlay rendering.");
        glFinish();
        return ReadFramebufferPixelColor(sampleX, sampleY, surface.height);
    };

    if (runMode == TestRunMode::Visual) {
        RunVisualLoop(window, "mode-ninjabrain-overlay-render", [&](DummyWindow& visualWindow) {
            (void)renderAndSample(visualWindow);
            visualWindow.PresentSurface();
        });
    } else {
        const Color sample = renderAndSample(window);
        Expect(!IsColorNear(sample, kExpectedRenderSurfaceClear),
               "Expected the NinjaBrain overlay preview fixture to draw an opaque panel into the test surface.");

        nb.hideIfStale = true;
        nb.hideIfStaleDelaySeconds = 5;
        PublishConfigSnapshot();

        NinjabrainData staleData = data;
        staleData.lastUpdateTime = std::chrono::steady_clock::now() - std::chrono::seconds(nb.hideIfStaleDelaySeconds + 1);
        PublishNinjabrainData(std::move(staleData));

        const Color staleSample = renderWithoutOverlayAndSample(window);
        Expect(IsColorNear(staleSample, kExpectedRenderSurfaceClear),
               "Expected the Ninjabrain overlay preview fixture to hide when its data is stale.");
    }

    PublishNinjabrainData(NinjabrainData{});
    CleanupBrowserOverlayCache();
    CleanupWindowOverlayCache();
    CleanupGPUResources();
    CleanupShaders();
}

void RunRenderNinjabrainInformationMessageTranslationSpansTest(TestRunMode runMode = TestRunMode::Automated) {
    (void)runMode;

    struct RestoreEnglishTranslation {
        ~RestoreEnglishTranslation() {
            if (!LoadTranslation("en")) {
                std::cerr << "WARN: Failed to reload embedded English translations after the Ninjabrain information-message translation test." << std::endl;
            }
        }
    } restoreEnglishTranslation;

    Expect(LoadTranslation("zh_CN"), "Expected embedded zh_CN translations to load for the Ninjabrain information-message translation test.");

    NinjabrainInformationMessage combinedCertaintyMessage;
    combinedCertaintyMessage.severity = "INFO";
    combinedCertaintyMessage.type = "COMBINED_CERTAINTY";
    combinedCertaintyMessage.message =
        "Nether coords (-221, 223) have <span style=\"color:#00CE29;\">+84.3%</span> chance to hit the stronghold (it is between the top 2 offsets).";

    const NinjabrainFormattedInformationMessage formattedCombinedCertainty =
        FormatNinjabrainInformationMessage(combinedCertaintyMessage);
    Expect(
        formattedCombinedCertainty.plainText == "下界坐标 (-221, 223) 有 +84.3% 的概率命中要塞（这在最大的两个偏移点之间）。",
        "Expected the combined-certainty information message to translate through the overlay formatter while preserving the span text.");
    Expect(formattedCombinedCertainty.runs.size() >= 3,
           "Expected the combined-certainty information message formatter to split the translated text into multiple styled runs.");

    bool foundGreenCertaintyRun = false;
    for (const NinjabrainInformationTextRun& run : formattedCombinedCertainty.runs) {
        if (!run.hasColor || run.text != "+84.3%") {
            continue;
        }

        foundGreenCertaintyRun = run.colorRgb == 0x00CE29;
        if (foundGreenCertaintyRun) {
            break;
        }
    }
    Expect(foundGreenCertaintyRun,
           "Expected the translated combined-certainty information message to preserve the original green span color.");

    NinjabrainInformationMessage nextThrowMessage;
    nextThrowMessage.severity = "INFO";
    nextThrowMessage.type = "NEXT_THROW_DIRECTION";
    nextThrowMessage.message = "Go left 1 blocks, or right 3 blocks, for ~95% certainty after next measurement.";

    const NinjabrainFormattedInformationMessage formattedNextThrow =
        FormatNinjabrainInformationMessage(nextThrowMessage);
    Expect(
        formattedNextThrow.plainText == "向左 1 个方块，或者向右 3 个方块，在下次测量时将有 ~95% 的准确性。",
        "Expected the next-throw information message to translate through the overlay formatter.");
    Expect(formattedNextThrow.runs.size() == 1 && !formattedNextThrow.runs[0].hasColor,
           "Expected the next-throw information message to remain a single unstyled run after translation.");
}

void RunGlStateRestoresPixelStoreTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    if (!g_hasModernGL) { std::cout << "SKIP (no GL 3.3+)" << std::endl; return; }

    struct PixelStoreValues {
        GLint packRowLength;
        GLint packAlignment;
        GLint unpackRowLength;
        GLint unpackSkipPixels;
        GLint unpackSkipRows;
        GLint unpackAlignment;
    };
    auto apply = [](const PixelStoreValues& values) {
        glPixelStorei(GL_PACK_ROW_LENGTH, values.packRowLength);
        glPixelStorei(GL_PACK_ALIGNMENT, values.packAlignment);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, values.unpackRowLength);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, values.unpackSkipPixels);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, values.unpackSkipRows);
        glPixelStorei(GL_UNPACK_ALIGNMENT, values.unpackAlignment);
    };
    auto read = []() {
        PixelStoreValues values{};
        glGetIntegerv(GL_PACK_ROW_LENGTH, &values.packRowLength);
        glGetIntegerv(GL_PACK_ALIGNMENT, &values.packAlignment);
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &values.unpackRowLength);
        glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &values.unpackSkipPixels);
        glGetIntegerv(GL_UNPACK_SKIP_ROWS, &values.unpackSkipRows);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &values.unpackAlignment);
        return values;
    };

    const PixelStoreValues gameValues{ 7, 8, 128, 3, 5, 2 };
    const PixelStoreValues scrambledValues{ 0, 1, 0, 0, 0, 4 };

    apply(gameValues);
    GLState saved{};
    SaveGLState(&saved);
    apply(scrambledValues);
    RestoreGLState(saved);
    const PixelStoreValues restored = read();
    apply(PixelStoreValues{ 0, 4, 0, 0, 0, 4 });

    Expect(restored.packRowLength == gameValues.packRowLength, "Expected RestoreGLState to restore GL_PACK_ROW_LENGTH.");
    Expect(restored.packAlignment == gameValues.packAlignment, "Expected RestoreGLState to restore GL_PACK_ALIGNMENT.");
    Expect(restored.unpackRowLength == gameValues.unpackRowLength, "Expected RestoreGLState to restore GL_UNPACK_ROW_LENGTH.");
    Expect(restored.unpackSkipPixels == gameValues.unpackSkipPixels, "Expected RestoreGLState to restore GL_UNPACK_SKIP_PIXELS.");
    Expect(restored.unpackSkipRows == gameValues.unpackSkipRows, "Expected RestoreGLState to restore GL_UNPACK_SKIP_ROWS.");
    Expect(restored.unpackAlignment == gameValues.unpackAlignment, "Expected RestoreGLState to restore GL_UNPACK_ALIGNMENT.");
}

void RunScopedUnpackStateTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    if (!g_hasModernGL) { std::cout << "SKIP (no GL 3.3+)" << std::endl; return; }

    constexpr GLint kGameRowLength = 128;
    constexpr GLint kGameSkipPixels = 3;
    constexpr GLint kGameSkipRows = 5;
    constexpr GLint kGameAlignment = 1;
    constexpr GLint kScopedAlignment = 4;

    auto readUnpack = [](GLint (&values)[4]) {
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &values[0]);
        glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &values[1]);
        glGetIntegerv(GL_UNPACK_SKIP_ROWS, &values[2]);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &values[3]);
    };

    glPixelStorei(GL_UNPACK_ROW_LENGTH, kGameRowLength);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, kGameSkipPixels);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, kGameSkipRows);
    glPixelStorei(GL_UNPACK_ALIGNMENT, kGameAlignment);

    GLint inside[4] = {};
    {
        gloverlay::ScopedUnpackState scoped(kScopedAlignment);
        readUnpack(inside);
    }
    GLint after[4] = {};
    readUnpack(after);

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, kScopedAlignment);

    Expect(inside[0] == 0 && inside[1] == 0 && inside[2] == 0, "Expected ScopedUnpackState to clear row length and skip values while in scope.");
    Expect(inside[3] == kScopedAlignment, "Expected ScopedUnpackState to apply the requested alignment while in scope.");
    Expect(after[0] == kGameRowLength && after[1] == kGameSkipPixels && after[2] == kGameSkipRows,
           "Expected ScopedUnpackState to restore row length and skip values on scope exit.");
    Expect(after[3] == kGameAlignment, "Expected ScopedUnpackState to restore the unpack alignment on scope exit.");
}

void RunRebindIndicatorRendersBelowSettingsGuiTest(TestRunMode runMode = TestRunMode::Automated) {
    DummyWindow window(kWindowWidth, kWindowHeight, runMode == TestRunMode::Visual);
    if (!g_hasModernGL) { std::cout << "SKIP (no GL 3.3+)" << std::endl; return; }

    const std::filesystem::path root = PrepareCaseDirectory("rebind_indicator_renders_below_settings_gui");
    ResetGlobalTestState(root);

    LoadConfig();
    Expect(!g_configLoadFailed.load(std::memory_order_acquire),
        "Expected config load to succeed before rendering the rebind indicator below the settings GUI.");

    const std::filesystem::path relativeFixturePath = std::filesystem::path("fixtures") / "rebind-indicator-large.bmp";
    WriteSolidBmpFixtureToDisk(root, relativeFixturePath, 640, 640, 32, 192, 96);

    g_config.basicModeEnabled = false;
    g_config.keyRebinds.enabled = false;
    g_config.keyRebinds.indicatorPosition = 1;
    g_config.keyRebinds.indicatorImageEnabled.clear();
    g_config.keyRebinds.indicatorImageDisabled = relativeFixturePath.generic_string();
    g_configIsDirty.store(false, std::memory_order_release);

    const std::string inputsTab = tr("tabs.inputs");
    const std::string keyboardSubTab = tr("inputs.keyboard");

    const SurfaceSize surface = GetWindowClientSize(window.hwnd());
    const float guiScale = (std::max)(1.0f,
                                      std::round(((std::min)(surface.width / 1920.0f, surface.height / 1080.0f)) * 4.0f) / 4.0f);
    const int guiWidth = static_cast<int>(850.0f * guiScale);
    const int guiHeight = static_cast<int>(650.0f * guiScale);
    const int guiX = (surface.width - guiWidth) / 2;
    const int guiY = (surface.height - guiHeight) / 2;

    const float indicatorScale = static_cast<float>(surface.height) / 1080.0f;
    const int indicatorWidth = static_cast<int>(std::lround(640.0f * indicatorScale));
    const int indicatorHeight = static_cast<int>(std::lround(640.0f * indicatorScale));
    const int indicatorMargin = static_cast<int>(std::lround(10.0f * indicatorScale));
    const int indicatorX = surface.width - indicatorWidth - indicatorMargin;
    const int indicatorY = indicatorMargin;

    const int overlapLeft = (std::max)(guiX, indicatorX);
    const int overlapTop = (std::max)(guiY, indicatorY);
    const int overlapRight = (std::min)(guiX + guiWidth - 1, indicatorX + indicatorWidth - 1);
    const int overlapBottom = (std::min)(guiY + guiHeight - 1, indicatorY + indicatorHeight - 1);
    Expect(overlapLeft <= overlapRight && overlapTop <= overlapBottom,
           "Expected the large rebind indicator fixture to overlap the centered settings GUI.");

    const int sampleX = overlapLeft + (overlapRight - overlapLeft) / 2;
    const int sampleY = overlapTop + (overlapBottom - overlapTop) / 2;

    auto renderAndSample = [&](DummyWindow& targetWindow) {
        g_guiNeedsRecenter.store(true, std::memory_order_release);
        ScopedTabSelection scopedSelection(inputsTab.c_str(), keyboardSubTab.c_str());
        const ModeConfig& fullscreenMode = FindModeOrThrow("Fullscreen");
        RenderModeOverlayFrame(targetWindow, g_config, fullscreenMode, 0, true);
        glFinish();
        return ReadFramebufferPixelColor(sampleX, sampleY, surface.height);
    };

    if (runMode == TestRunMode::Visual) {
        g_config.keyRebinds.indicatorMode = 2;
        InvalidateRebindIndicatorTexture();
        RunVisualLoop(window, "rebind-indicator-renders-below-settings-gui",
                      [&](DummyWindow& visualWindow) {
                          (void)renderAndSample(visualWindow);
                          visualWindow.PresentSurface();
                      });
        return;
    }

    g_config.keyRebinds.indicatorMode = 0;
    InvalidateRebindIndicatorTexture();
    const Color baselineSample = renderAndSample(window);
    Expect(!IsColorNear(baselineSample, kExpectedRenderSurfaceClear),
           "Expected the sampled point to fall within the rendered settings GUI before drawing the rebind indicator overlay.");

    g_config.keyRebinds.indicatorMode = 2;
    InvalidateRebindIndicatorTexture();
    const Color overlaySample = renderAndSample(window);
    Expect(!IsColorNear(overlaySample, kExpectedPngFixtureColor),
            "Expected the overlapping sampled pixel to stay below the raw rebind indicator color once the settings GUI renders above it.");
        Expect(std::fabs(overlaySample.g - baselineSample.g) < 0.12f,
            "Expected the settings GUI to remain visually dominant over the rebind indicator at the overlapping sample point.");
}