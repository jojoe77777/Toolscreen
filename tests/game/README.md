# In-game tests

These tests run Toolscreen inside a real Minecraft client instead of a test executable.
`scripts/game_tests/run_game_tests.ps1` does the following:

1. It builds a Fabric dev-client project under `out/game-tests/<version>`. The Gradle wrapper comes from
   [fabric-example-mod](https://github.com/FabricMC/fabric-example-mod) at a pinned commit. The build files come from
   `tests/game/fabric/`, and the project has no mod sources.
2. It runs `gradlew runClient` with `TOOLSCREEN_GAME_TEST_*` environment variables set. The game JVM inherits them.
3. As soon as the game JVM starts, it injects `liblogger_x64.dll` and then `Toolscreen.dll` the way the
   EasyInjectBundled watcher does: `CreateRemoteThread(LoadLibraryW)`, then a 100 ms gap
   (`tests/game/injector/game_test_injector.cpp`).
4. Toolscreen sees `TOOLSCREEN_GAME_TEST_RESULTS` and starts the runner in `src/testing/game_test_mode.cpp` once its
   game-window hooks are installed. The runner queues steps onto the real render thread (inside the hooked
   `wglSwapBuffers` / `vkQueuePresentKHR`), sends real window messages, and writes JSON lines to `results.jsonl`.
5. The script stops the game, prints each result, and exits non-zero on any failure.

Toolscreen keeps its config and logs in `out/game-tests/<version>/run/toolscreen`, so these tests never touch your real
`%USERPROFILE%\.config\toolscreen`.

## Running

Build the DLL and the injector first:

```bash
cmake --build --preset debug --target Toolscreen toolscreen_game_test_injector toolscreen_game_test_overlay
```

Then run one client:

```bash
powershell -ExecutionPolicy Bypass -File scripts/game_tests/run_game_tests.ps1 -MinecraftVersion 1.16.1
```

You can also run every registered combination (1.16.1, 26.3 OpenGL, 26.3 Vulkan):

```bash
ctest --preset debug -R toolscreen_integration_game --output-on-failure
```

Gradle and the game need a JDK 21 or newer, because Loom 1.18 requires it, and 26.x clients need Java 25. The script uses
`-JavaHome`, then `TOOLSCREEN_GAME_TEST_JAVA_HOME`, then `JAVA_HOME`. The first run of each version downloads Minecraft.
The game window opens on your desktop while the tests run.

Useful options:

| Option | Purpose |
| --- | --- |
| `-GraphicsBackend opengl\|vulkan` | Renderer for 26.x clients (passed as `--graphicsBackend`). Older versions always use OpenGL. |
| `-Filter mode.,input.` | Run only tests whose name contains one of the comma-separated substrings. |
| `-ConfigFixture <file or folder>` | Seed the toolscreen folder before launch. A file becomes `config.toml` (e.g. `tests/game/fixtures/broken_config.toml` for the error screen); a folder is copied in whole (e.g. `tests/game/fixtures/corrupt_config_with_backup` for the automatic restore). |
| `-QuickPlayWorld <name>` | 26.x only. Loads straight into a world in `out/game-tests/<version>/run/saves` and holds the tests until it has loaded. Create the world once by hand: run with `-KeepGameOpen -Filter none` and use Singleplayer → Create New World. |
| `-WindowMode borderless\|fullscreen` | Runs the tests at monitor size, through Toolscreen's borderless toggle or the game's F11 fullscreen. |
| `-KeepGameOpen` | Leave the game running afterwards, for example to inspect it with `jcmd <pid> Thread.print`. |
| `-BuildDir <dir>` | Test a DLL from another build tree, such as a worktree of an older commit. |

## Adding a test

Add a function to `src/testing/game_test_mode.cpp` and list it in `kTests`. Use `Require` and `Skip` for outcomes,
`RunOnRenderThread` for anything that touches `g_config`, GL/Vulkan state or ImGui, and `WaitUntil` / `WaitForFrames`
rather than fixed sleeps. Name tests `<area>.<behaviour>` so `-Filter` can select them.
