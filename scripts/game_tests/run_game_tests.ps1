<#
.SYNOPSIS
Runs Toolscreen's in-game tests against a real Minecraft dev client.

.DESCRIPTION
1. Builds a Fabric dev-client project from FabricMC/fabric-example-mod (pinned) under out/game-tests/<version>.
2. Launches it with `gradlew runClient` and TOOLSCREEN_GAME_TEST_* variables, which put Toolscreen in test mode.
3. Injects liblogger and Toolscreen.dll into the game JVM as soon as it starts, the same way EasyInjectBundled does.
4. Waits for the DLL to write its JSON-lines results, stops the game, and reports.

Build the DLL and injector first:
  cmake --build --preset debug --target Toolscreen toolscreen_game_test_injector toolscreen_game_test_overlay

.EXAMPLE
pwsh scripts/game_tests/run_game_tests.ps1 -MinecraftVersion 1.16.1
#>
[CmdletBinding()]
param(
    [string]$MinecraftVersion = "1.16.1",
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$Configuration = "Debug",
    [string]$BuildDir = "",
    # Defaults to toolscreen_game_test_injector.exe beside the DLL, then in this repo's own build output.
    [string]$InjectorPath = "",
    # JDK used to run Gradle and the game. Loom 1.18 needs Java 21+.
    [string]$JavaHome = "",
    # Renderer for 26.x clients (passed as --graphicsBackend). Versions before 26 always use OpenGL.
    [ValidateSet("opengl", "vulkan")]
    [string]$GraphicsBackend = "vulkan",
    # Comma-separated substrings; only tests whose name contains one of them run.
    [string]$Filter = "",
    [int]$LaunchTimeoutSeconds = 900,
    [int]$TestTimeoutSeconds = 300,
    [switch]$KeepGameOpen
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$FabricExampleModUrl = "https://github.com/FabricMC/fabric-example-mod"
$FabricExampleModCommit = "44465cb0eb83932c72ece5934d32ddfc758802ed"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
if (-not $BuildDir) { $BuildDir = Join-Path $RepoRoot "out\build" }
$BinDir = Join-Path $BuildDir "bin\$Configuration"
$WorkRoot = Join-Path $RepoRoot "out\game-tests"
$TemplateDir = Join-Path $RepoRoot "tests\game\fabric"
$ProjectDir = Join-Path $WorkRoot $MinecraftVersion
$RunDir = Join-Path $ProjectDir "run"
$ToolscreenDir = Join-Path $RunDir "toolscreen"
$StageDir = Join-Path $WorkRoot "dlls"
$ResultsPath = Join-Path $ProjectDir "results.jsonl"
$GradleLog = Join-Path $ProjectDir "gradle-run.log"

function Write-Step([string]$Message) { Write-Host "==> $Message" -ForegroundColor Cyan }

function Get-GameJavaProcesses {
    # The game JVM is launched by Loom's dev launch injector with an arg file inside this project's loom cache.
    $marker = (Join-Path $ProjectDir "build\loom-cache").ToLowerInvariant()
    Get-CimInstance Win32_Process -Filter "Name='java.exe' OR Name='javaw.exe'" |
        Where-Object { $_.CommandLine -and $_.CommandLine.ToLowerInvariant().Contains($marker) -and
                       $_.CommandLine -match "devlaunchinjector|KnotClient" }
}

function Stop-ProcessTree([int]$ProcessId) {
    Start-Process -FilePath "taskkill.exe" -ArgumentList "/PID", $ProcessId, "/T", "/F" -WindowStyle Hidden -Wait | Out-Null
}

# Rewriting an unchanged DLL makes antivirus rescan it on the next load, which can stall the injected
# LoadLibraryW for many seconds, so only copy files that actually changed.
function Copy-IfChanged([string]$Source, [string]$Destination) {
    if (Test-Path $Destination) {
        $src = Get-Item $Source
        $dst = Get-Item $Destination
        if ($src.Length -eq $dst.Length -and $src.LastWriteTimeUtc -eq $dst.LastWriteTimeUtc) { return }
    }
    Copy-Item -Force $Source $Destination
}

function Stop-LeftoverGames {
    foreach ($proc in @(Get-GameJavaProcesses)) {
        Write-Host "Stopping leftover game process $($proc.ProcessId)"
        Stop-ProcessTree $proc.ProcessId
    }
}

# ---- Inputs ----------------------------------------------------------------------------------------

$isModernVersion = -not $MinecraftVersion.StartsWith("1.")
$ExpectedBackend = if ($isModernVersion) { $GraphicsBackend } else { "opengl" }
$gradleArgs = "--no-daemon --console=plain runClient"
if ($isModernVersion) { $gradleArgs += " -PgraphicsBackend=$GraphicsBackend" }

$toolscreenDll = Join-Path $BinDir "Toolscreen.dll"
$loggerDll = Join-Path $BinDir "liblogger_x64.dll"
$layerJson = Join-Path $BinDir "VK_LAYER_TOOLSCREEN_obs_redirect.json"
$injectorExe = $InjectorPath
if (-not $injectorExe) {
    $injectorExe = Join-Path $BinDir "toolscreen_game_test_injector.exe"
    $repoInjector = Join-Path $RepoRoot "out\build\bin\$Configuration\toolscreen_game_test_injector.exe"
    if (-not (Test-Path $injectorExe) -and (Test-Path $repoInjector)) { $injectorExe = $repoInjector }
}
foreach ($required in @($toolscreenDll, $loggerDll, $layerJson, $injectorExe)) {
    if (-not (Test-Path $required)) {
        throw "Missing $required. Build first: cmake --build --preset $($Configuration.ToLowerInvariant()) --target Toolscreen toolscreen_game_test_injector toolscreen_game_test_overlay"
    }
}

if (-not $JavaHome) { $JavaHome = $env:TOOLSCREEN_GAME_TEST_JAVA_HOME }
if (-not $JavaHome) { $JavaHome = $env:JAVA_HOME }
if (-not $JavaHome -or -not (Test-Path (Join-Path $JavaHome "bin\java.exe"))) {
    throw "No JDK found. Pass -JavaHome or set TOOLSCREEN_GAME_TEST_JAVA_HOME (Loom 1.18 needs Java 21+)."
}
$javaRelease = Join-Path $JavaHome "release"
if (Test-Path $javaRelease) {
    $versionLine = Select-String -Path $javaRelease -Pattern '^JAVA_VERSION="(\d+)' | Select-Object -First 1
    if ($versionLine -and [int]$versionLine.Matches[0].Groups[1].Value -lt 21) {
        throw "$JavaHome is Java $($versionLine.Matches[0].Groups[1].Value); Loom 1.18 needs Java 21+ (26.x clients need 25)."
    }
}

# ---- Dev client project ------------------------------------------------------------------------------

Write-Step "Preparing Fabric dev client for Minecraft $MinecraftVersion ($ExpectedBackend)"
New-Item -ItemType Directory -Force -Path $WorkRoot | Out-Null
$templateRepo = Join-Path $WorkRoot "fabric-example-mod"
$haveTemplate = (Test-Path (Join-Path $templateRepo ".git")) -and
                ((& git -C $templateRepo rev-parse HEAD 2>$null) -eq $FabricExampleModCommit)
if (-not $haveTemplate) {
    if (Test-Path $templateRepo) { Remove-Item -Recurse -Force $templateRepo }
    & git clone --quiet $FabricExampleModUrl $templateRepo
    if ($LASTEXITCODE -ne 0) { throw "git clone of $FabricExampleModUrl failed" }
    & git -C $templateRepo checkout --quiet $FabricExampleModCommit
    if ($LASTEXITCODE -ne 0) { throw "git checkout $FabricExampleModCommit failed" }
}

New-Item -ItemType Directory -Force -Path $ProjectDir | Out-Null
# Only the Gradle wrapper comes from the example mod; the build files are ours and the project has no mod sources.
Copy-Item -Recurse -Force (Join-Path $templateRepo "gradle") $ProjectDir
Copy-Item -Force (Join-Path $templateRepo "gradlew"), (Join-Path $templateRepo "gradlew.bat") $ProjectDir
$buildTemplate = if ($isModernVersion) { "build.gradle" } else { "build.remap.gradle" }
Copy-Item -Force (Join-Path $TemplateDir $buildTemplate) (Join-Path $ProjectDir "build.gradle")
Copy-Item -Force (Join-Path $TemplateDir "settings.gradle") $ProjectDir
(Get-Content -Raw (Join-Path $TemplateDir "gradle.properties")).Replace("@MINECRAFT_VERSION@", $MinecraftVersion) |
    Set-Content -NoNewline -Encoding ascii (Join-Path $ProjectDir "gradle.properties")

Stop-LeftoverGames

# A toolscreen folder in the game directory makes Toolscreen keep its config and logs there instead of
# %USERPROFILE%\.config\toolscreen, so tests never touch the developer's real config.
if (Test-Path $ToolscreenDir) { Remove-Item -Recurse -Force $ToolscreenDir }
New-Item -ItemType Directory -Force -Path $ToolscreenDir | Out-Null

# Stage the payload the way EasyInjectBundled extracts it: Toolscreen.dll beside liblogger and the layer JSON.
New-Item -ItemType Directory -Force -Path $StageDir | Out-Null
Copy-IfChanged $loggerDll (Join-Path $StageDir "liblogger_x64.dll")
Copy-IfChanged $toolscreenDll (Join-Path $StageDir "Toolscreen.dll")
Copy-IfChanged $layerJson (Join-Path $StageDir "VK_LAYER_TOOLSCREEN_obs_redirect.json")
# Two copies of the stand-in overlay, so the hook-chain test can unload one and chain behind a distinct second one.
$overlayDll = Join-Path $BinDir "toolscreen_game_test_overlay.dll"
if (Test-Path $overlayDll) {
    Copy-IfChanged $overlayDll (Join-Path $StageDir "game_test_overlay_a.dll")
    Copy-IfChanged $overlayDll (Join-Path $StageDir "game_test_overlay_b.dll")
}
$toolscreenPdb = Join-Path $BinDir "Toolscreen.pdb"
if (Test-Path $toolscreenPdb) { Copy-IfChanged $toolscreenPdb (Join-Path $StageDir "Toolscreen.pdb") }

Remove-Item -Force -ErrorAction SilentlyContinue $ResultsPath

# ---- Launch ------------------------------------------------------------------------------------------

Write-Step "Launching dev client (first run downloads Minecraft; log: $GradleLog)"
$env:JAVA_HOME = $JavaHome
$env:TOOLSCREEN_GAME_TEST_RESULTS = $ResultsPath
$env:TOOLSCREEN_GAME_TEST_EXPECTED_VERSION = $MinecraftVersion
$env:TOOLSCREEN_GAME_TEST_EXPECTED_BACKEND = $ExpectedBackend
$env:TOOLSCREEN_GAME_TEST_TOOLSCREEN_DIR = $ToolscreenDir
$env:TOOLSCREEN_GAME_TEST_FILTER = $Filter

$gradle = Start-Process -FilePath "cmd.exe" -WorkingDirectory $ProjectDir -PassThru -WindowStyle Hidden `
    -ArgumentList "/c", "`"`"$(Join-Path $ProjectDir 'gradlew.bat')`" $gradleArgs > `"$GradleLog`" 2>&1`""

$exitCode = 1
try {
    $gamePid = $null
    $deadline = (Get-Date).AddSeconds($LaunchTimeoutSeconds)
    while (-not $gamePid) {
        if ($gradle.HasExited) { throw "Gradle exited ($($gradle.ExitCode)) before the game started. See $GradleLog" }
        if ((Get-Date) -gt $deadline) { throw "The game JVM did not start within $LaunchTimeoutSeconds s. See $GradleLog" }
        $game = @(Get-GameJavaProcesses) | Select-Object -First 1
        if ($game) { $gamePid = [int]$game.ProcessId } else { Start-Sleep -Milliseconds 100 }
    }

    # Inject straight away, before the window exists, like the launcher watcher (Vulkan capture depends on it).
    Write-Step "Injecting into game process $gamePid"
    & $injectorExe --pid $gamePid --dll (Join-Path $StageDir "liblogger_x64.dll") --dll (Join-Path $StageDir "Toolscreen.dll")
    if ($LASTEXITCODE -ne 0) { throw "Injection failed (exit $LASTEXITCODE)" }

    Write-Step "Waiting for in-game test results"
    $deadline = (Get-Date).AddSeconds($TestTimeoutSeconds)
    $done = $false
    while (-not $done) {
        if (-not (Get-Process -Id $gamePid -ErrorAction SilentlyContinue)) { throw "The game exited before the tests finished. See $GradleLog" }
        if ((Get-Date) -gt $deadline) { throw "Tests did not finish within $TestTimeoutSeconds s." }
        if (Test-Path $ResultsPath) {
            $done = [bool](Select-String -Path $ResultsPath -Pattern '"event":"done"' -SimpleMatch -Quiet)
        }
        if (-not $done) { Start-Sleep -Milliseconds 250 }
    }
    $exitCode = 0
}
catch {
    Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
}
finally {
    if (-not $KeepGameOpen) {
        Stop-ProcessTree $gradle.Id
        Stop-LeftoverGames
    }
}

# ---- Report ------------------------------------------------------------------------------------------

$failed = 0
if (Test-Path $ResultsPath) {
    foreach ($line in Get-Content $ResultsPath) {
        $entry = $line | ConvertFrom-Json
        switch ($entry.event) {
            "test" {
                $color = switch ($entry.status) { "pass" { "Green" } "skip" { "Yellow" } default { "Red" } }
                $suffix = if ($entry.message) { " - $($entry.message)" } else { "" }
                Write-Host ("{0,-5} {1} ({2} ms){3}" -f $entry.status.ToUpperInvariant(), $entry.name, $entry.durationMs, $suffix) -ForegroundColor $color
                if ($entry.status -eq "fail") { $failed++ }
            }
            "done" { Write-Host "Passed $($entry.passed), failed $($entry.failed), skipped $($entry.skipped)" }
            "error" { Write-Host "Runner error: $($entry.message)" -ForegroundColor Red; $failed++ }
        }
    }
} else {
    Write-Host "No results were written." -ForegroundColor Red
}
Write-Host "Toolscreen log: $(Join-Path $ToolscreenDir 'logs\latest.log')"

if ($exitCode -ne 0 -or $failed -gt 0) { exit 1 }
exit 0
