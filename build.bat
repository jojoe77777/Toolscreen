@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
set "TARGET_ARCH=x64"
set "CONFIGURE_PRESET=vs2022-x64"
set "BUILD_PRESET=release"
set "TEST_PRESET=release"
set "RUN_TESTS=0"
set "RUN_NINJABRAIN_MANUAL_TESTS=0"
set "FAILURE_STEP="
set "FAILURE_CODE=1"
set "ARTIFACT_CONFIG_DIR=Release"
set "CLI_TEST_RUNNER="
set "PREBUILT_LIBLOGGER_DIR=%SCRIPT_DIR%out\prebuilt-liblogger"
set "PREBUILT_X64_LIBLOGGER_DLL=%PREBUILT_LIBLOGGER_DIR%\liblogger_x64.dll"
set "PREBUILT_X64_LIBLOGGER_PDB=%PREBUILT_LIBLOGGER_DIR%\liblogger_x64.pdb"
set "PREBUILT_ARM64_LIBLOGGER_DLL=%PREBUILT_LIBLOGGER_DIR%\liblogger_arm64.dll"
set "PREBUILT_ARM64_LIBLOGGER_PDB=%PREBUILT_LIBLOGGER_DIR%\liblogger_arm64.pdb"
set "PREBUILT_LIBLOGGER_DLL=%PREBUILT_X64_LIBLOGGER_DLL%"
set "PREBUILT_LIBLOGGER_PDB=%PREBUILT_X64_LIBLOGGER_PDB%"

for %%A in (%*) do (
    if /I "%%~A"=="release" (
        set "BUILD_PRESET=release"
        set "TEST_PRESET=release"
        set "ARTIFACT_CONFIG_DIR=Release"
    ) else if /I "%%~A"=="debug" (
        set "BUILD_PRESET=debug"
        set "TEST_PRESET=debug"
        set "ARTIFACT_CONFIG_DIR=Debug"
    ) else if /I "%%~A"=="--test" (
        set "RUN_TESTS=1"
    ) else if /I "%%~A"=="--manual-ninjabrain-tests" (
        set "RUN_NINJABRAIN_MANUAL_TESTS=1"
    ) else if /I "%%~A"=="arm64" (
        set "TARGET_ARCH=arm64"
    ) else (
        goto :usage
    )
)

if "%RUN_TESTS%"=="1" if "%RUN_NINJABRAIN_MANUAL_TESTS%"=="1" goto :usage

if /I "%TARGET_ARCH%"=="arm64" (
    set "CONFIGURE_PRESET=vs2022-arm64"
    set "PREBUILT_LIBLOGGER_DLL=%PREBUILT_ARM64_LIBLOGGER_DLL%"
    set "PREBUILT_LIBLOGGER_PDB=%PREBUILT_ARM64_LIBLOGGER_PDB%"
    if /I "%ARTIFACT_CONFIG_DIR%"=="Debug" (
        set "BUILD_PRESET=debug-arm64"
        set "TEST_PRESET=debug-arm64"
    ) else (
        set "BUILD_PRESET=release-arm64"
        set "TEST_PRESET=release-arm64"
    )
)

if "%RUN_NINJABRAIN_MANUAL_TESTS%"=="1" (
    set "CONFIGURE_PRESET=vs2022-%TARGET_ARCH%-ninjabrain-manual-tests"
    if /I "%ARTIFACT_CONFIG_DIR%"=="Debug" (
        if /I "%TARGET_ARCH%"=="arm64" (
            set "BUILD_PRESET=debug-arm64-ninjabrain-manual-tests"
            set "TEST_PRESET=debug-arm64-ninjabrain-manual-tests"
        ) else (
            set "BUILD_PRESET=debug-ninjabrain-manual-tests"
            set "TEST_PRESET=debug-ninjabrain-manual-tests"
        )
    ) else (
        if /I "%TARGET_ARCH%"=="arm64" (
            set "BUILD_PRESET=release-arm64-ninjabrain-manual-tests"
            set "TEST_PRESET=release-arm64-ninjabrain-manual-tests"
        ) else (
            set "BUILD_PRESET=release-ninjabrain-manual-tests"
            set "TEST_PRESET=release-ninjabrain-manual-tests"
        )
    )
)

if /I "%ARTIFACT_CONFIG_DIR%"=="Debug" (
    set "X64_BUILD_PRESET=debug"
    set "ARM64_BUILD_PRESET=debug-arm64"
) else (
    set "X64_BUILD_PRESET=release"
    set "ARM64_BUILD_PRESET=release-arm64"
)

set "X64_ARTIFACT_DIR=%SCRIPT_DIR%out\build\bin\%ARTIFACT_CONFIG_DIR%"
set "ARM64_ARTIFACT_DIR=%SCRIPT_DIR%out\build-arm64\bin\%ARTIFACT_CONFIG_DIR%"
set "X64_TOOLSCREEN_DLL=%X64_ARTIFACT_DIR%\Toolscreen.dll"
set "ARM64_TOOLSCREEN_DLL=%ARM64_ARTIFACT_DIR%\Toolscreen.dll"
set "X64_LIBLOGGER_DLL=%X64_ARTIFACT_DIR%\liblogger_x64.dll"
set "ARM64_LIBLOGGER_DLL=%ARM64_ARTIFACT_DIR%\liblogger_arm64.dll"

if /I "%TARGET_ARCH%"=="arm64" (
    set "ARTIFACT_DIR=%ARM64_ARTIFACT_DIR%"
    set "PACKAGING_DLL=%ARM64_TOOLSCREEN_DLL%"
    set "PACKAGING_LIBLOGGER_DLL=%ARM64_LIBLOGGER_DLL%"
) else (
    set "ARTIFACT_DIR=%X64_ARTIFACT_DIR%"
    set "PACKAGING_DLL=%X64_TOOLSCREEN_DLL%"
    set "PACKAGING_LIBLOGGER_DLL=%X64_LIBLOGGER_DLL%"
)
set "CLI_TEST_RUNNER=%ARTIFACT_DIR%\toolscreen_gui_integration_tests.exe"

set "HOST_ARCH="
for /F "usebackq delims=" %%H in (`powershell -NoProfile -Command "[System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()" 2^>nul`) do set "HOST_ARCH=%%H"
if not defined HOST_ARCH (
    set "HOST_ARCH=%PROCESSOR_ARCHITECTURE%"
    if defined PROCESSOR_ARCHITEW6432 set "HOST_ARCH=%PROCESSOR_ARCHITEW6432%"
)
set "CAN_RUN_TARGET=0"
if /I "%TARGET_ARCH%"=="x64" (
    if /I "%HOST_ARCH%"=="X64" set "CAN_RUN_TARGET=1"
    if /I "%HOST_ARCH%"=="AMD64" set "CAN_RUN_TARGET=1"
    if /I "%HOST_ARCH%"=="ARM64" set "CAN_RUN_TARGET=1"
) else if /I "%TARGET_ARCH%"=="arm64" (
    if /I "%HOST_ARCH%"=="ARM64" set "CAN_RUN_TARGET=1"
)

pushd "%SCRIPT_DIR%" >nul || exit /b 1

if "%RUN_NINJABRAIN_MANUAL_TESTS%"=="1" goto :configure_manual_ninjabrain_tests

echo Downloading the latest signed x64 and ARM64 liblogger artifacts...
powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%scripts\fetch_signed_liblogger.ps1" -DestinationDirectory "%PREBUILT_LIBLOGGER_DIR%" -Architecture all
if errorlevel 1 (
    set "FAILURE_STEP=Download signed liblogger artifacts"
    set "FAILURE_CODE=12"
    goto :fail
)

echo Configuring x64 core payload with packaging disabled...
cmake --preset vs2022-x64 ^
    -DTOOLSCREEN_ENABLE_JAR_PACKAGING=OFF ^
    -DTOOLSCREEN_ENABLE_EXE_PACKAGING=OFF ^
    -DTOOLSCREEN_BUILD_DOWNLOADER_ARTIFACTS=OFF ^
    -DTOOLSCREEN_PREBUILT_LIBLOGGER_DLL_PATH="%PREBUILT_X64_LIBLOGGER_DLL%" ^
    -DTOOLSCREEN_PREBUILT_LIBLOGGER_PDB_PATH="%PREBUILT_X64_LIBLOGGER_PDB%"
if errorlevel 1 (
    set "FAILURE_STEP=Configure x64 core payload"
    set "FAILURE_CODE=10"
    goto :fail
)

echo Building x64 core payload with preset %X64_BUILD_PRESET%...
cmake --build --preset %X64_BUILD_PRESET% --target Toolscreen
if errorlevel 1 (
    set "FAILURE_STEP=Build x64 Toolscreen DLL"
    set "FAILURE_CODE=20"
    goto :fail
)

echo Configuring ARM64 core payload with packaging disabled...
cmake --preset vs2022-arm64 ^
    -DTOOLSCREEN_ENABLE_JAR_PACKAGING=OFF ^
    -DTOOLSCREEN_ENABLE_EXE_PACKAGING=OFF ^
    -DTOOLSCREEN_BUILD_DOWNLOADER_ARTIFACTS=OFF ^
    -DTOOLSCREEN_PREBUILT_LIBLOGGER_DLL_PATH="%PREBUILT_ARM64_LIBLOGGER_DLL%" ^
    -DTOOLSCREEN_PREBUILT_LIBLOGGER_PDB_PATH="%PREBUILT_ARM64_LIBLOGGER_PDB%"
if errorlevel 1 (
    set "FAILURE_STEP=Configure ARM64 core payload"
    set "FAILURE_CODE=15"
    goto :fail
)

echo Building ARM64 core payload with preset %ARM64_BUILD_PRESET%...
cmake --build --preset %ARM64_BUILD_PRESET% --target Toolscreen
if errorlevel 1 (
    set "FAILURE_STEP=Build ARM64 Toolscreen DLL"
    set "FAILURE_CODE=22"
    goto :fail
)

echo Building GUI integration test runner with preset %BUILD_PRESET%...
cmake --build --preset %BUILD_PRESET% --target toolscreen_gui_integration_tests
if errorlevel 1 (
    set "FAILURE_STEP=Build GUI integration test runner"
    set "FAILURE_CODE=25"
    goto :fail
)

if "%RUN_TESTS%"=="1" (
    echo Building all CTest runners with preset %BUILD_PRESET%...
    cmake --build --preset %BUILD_PRESET% --target ^
        toolscreen_interactive_create_tests ^
        toolscreen_game_state_source_tests ^
        toolscreen_path_sanitize_tests ^
        toolscreen_background_fit_layout_tests ^
        toolscreen_startup_lifecycle_tests ^
        toolscreen_expression_parser_tests ^
        toolscreen_base64_tests
    if errorlevel 1 (
        set "FAILURE_STEP=Build CTest runners"
        set "FAILURE_CODE=28"
        goto :fail
    )
)

if not exist "%CLI_TEST_RUNNER%" (
    set "FAILURE_STEP=Locate GUI integration test runner"
    set "FAILURE_CODE=26"
    goto :fail
)

if "%RUN_TESTS%"=="1" (
    if "%CAN_RUN_TARGET%"=="1" (
        echo Running CLI integration test runner...
        "%CLI_TEST_RUNNER%" --run-all
        if errorlevel 1 (
            set "FAILURE_STEP=Run CLI integration test runner"
            set "FAILURE_CODE=27"
            goto :fail
        )
    ) else (
        echo Skipping %TARGET_ARCH% CLI integration tests on incompatible host architecture %HOST_ARCH%.
    )
)

echo Configuring universal packaging with preset %CONFIGURE_PRESET%...
cmake --preset %CONFIGURE_PRESET% ^
    -DTOOLSCREEN_ENABLE_JAR_PACKAGING=ON ^
    -DTOOLSCREEN_ENABLE_EXE_PACKAGING=ON ^
    -DTOOLSCREEN_BUILD_DOWNLOADER_ARTIFACTS=ON ^
    -DTOOLSCREEN_PREBUILT_LIBLOGGER_DLL_PATH="%PREBUILT_LIBLOGGER_DLL%" ^
    -DTOOLSCREEN_PREBUILT_LIBLOGGER_PDB_PATH="%PREBUILT_LIBLOGGER_PDB%" ^
    -DTOOLSCREEN_PACKAGING_DLL_PATH="%PACKAGING_DLL%" ^
    -DTOOLSCREEN_PACKAGING_LIBLOGGER_PATH="%PACKAGING_LIBLOGGER_DLL%" ^
    -DTOOLSCREEN_PACKAGING_X64_DLL_PATH="%X64_TOOLSCREEN_DLL%" ^
    -DTOOLSCREEN_PACKAGING_ARM64_DLL_PATH="%ARM64_TOOLSCREEN_DLL%" ^
    -DTOOLSCREEN_PACKAGING_X64_LIBLOGGER_PATH="%X64_LIBLOGGER_DLL%" ^
    -DTOOLSCREEN_PACKAGING_ARM64_LIBLOGGER_PATH="%ARM64_LIBLOGGER_DLL%"
if errorlevel 1 (
    set "FAILURE_STEP=Configure universal packaging with preset %CONFIGURE_PRESET%"
    set "FAILURE_CODE=40"
    goto :fail
)

echo Removing stale packaged artifacts...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "$patterns = @('Toolscreen-*-double-click-me.jar', 'Toolscreen-*-double-click-me.exe', 'toolscreen-downloader.jar', 'toolscreen-downloader.exe', 'tioolscrteen-downloader.jar', 'tioolscrteen-downloader.exe'); foreach ($pattern in $patterns) { Get-ChildItem -Path '%ARTIFACT_DIR%' -File -Filter $pattern -ErrorAction SilentlyContinue | Remove-Item -Force }"
if errorlevel 1 (
        set "FAILURE_STEP=Remove stale packaged artifacts"
        set "FAILURE_CODE=45"
        goto :fail
)

echo Building EXE package with preset %BUILD_PRESET%...
cmake --build --preset %BUILD_PRESET% --target installer_exe
if errorlevel 1 (
    set "FAILURE_STEP=Build installer EXE package"
    set "FAILURE_CODE=50"
    goto :fail
)

echo Building Toolscreen downloader EXE with preset %BUILD_PRESET%...
cmake --build --preset %BUILD_PRESET% --target downloader_exe
if errorlevel 1 (
    set "FAILURE_STEP=Build Toolscreen downloader EXE"
    set "FAILURE_CODE=55"
    goto :fail
)

echo Building JAR package with preset %BUILD_PRESET%...
cmake --build --preset %BUILD_PRESET% --target jar
if errorlevel 1 (
    set "FAILURE_STEP=Build JAR package"
    set "FAILURE_CODE=60"
    goto :fail
)

echo Verifying PE metadata for signed artifacts...
powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%scripts\verify_pe_metadata.ps1" -ArtifactDirectory "%ARTIFACT_DIR%"
if errorlevel 1 (
    set "FAILURE_STEP=Verify PE metadata for signing"
    set "FAILURE_CODE=62"
    goto :fail
)

if "%RUN_TESTS%"=="1" (
    if "%CAN_RUN_TARGET%"=="1" (
        echo Running CTest packaging and native smoke tests with preset %TEST_PRESET%...
        ctest --preset %TEST_PRESET% --exclude-regex "^toolscreen_integration_"
        if errorlevel 1 (
            set "FAILURE_STEP=Run CTest packaging and native smoke tests with preset %TEST_PRESET%"
            set "FAILURE_CODE=70"
            goto :fail
        )
    ) else (
        echo Running architecture-independent CTest artifact checks with preset %TEST_PRESET%...
        ctest --preset %TEST_PRESET% --tests-regex "^(toolscreen_artifact_exists|liblogger_artifact_exists|toolscreen_exe_exists|toolscreen_exe_embeds_dll|toolscreen_downloader_exe_exists|toolscreen_jar_exists|toolscreen_downloader_jar_exists)$"
        if errorlevel 1 (
            set "FAILURE_STEP=Run architecture-independent CTest artifact checks with preset %TEST_PRESET%"
            set "FAILURE_CODE=70"
            goto :fail
        )
    )
)

popd >nul
exit /b 0

:configure_manual_ninjabrain_tests
echo Downloading the latest signed %TARGET_ARCH% liblogger artifacts...
powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%scripts\fetch_signed_liblogger.ps1" -DestinationDirectory "%PREBUILT_LIBLOGGER_DIR%" -Architecture "%TARGET_ARCH%"
if errorlevel 1 (
    set "FAILURE_STEP=Download signed liblogger artifacts"
    set "FAILURE_CODE=12"
    goto :fail
)

echo Configuring with preset %CONFIGURE_PRESET%...
cmake --preset %CONFIGURE_PRESET% -DTOOLSCREEN_PREBUILT_LIBLOGGER_DLL_PATH="%PREBUILT_LIBLOGGER_DLL%" -DTOOLSCREEN_PREBUILT_LIBLOGGER_PDB_PATH="%PREBUILT_LIBLOGGER_PDB%"
if errorlevel 1 (
    set "FAILURE_STEP=Configure preset %CONFIGURE_PRESET%"
    set "FAILURE_CODE=10"
    goto :fail
)

:manual_ninjabrain_tests
echo Building manual Ninjabrain API tests with preset %BUILD_PRESET%...
cmake --build --preset %BUILD_PRESET% --target toolscreen_ninjabrain_api_tests
if errorlevel 1 (
    set "FAILURE_STEP=Build manual Ninjabrain API test executable"
    set "FAILURE_CODE=65"
    goto :fail
)

echo Running manual Ninjabrain API tests with preset %TEST_PRESET%...
ctest --preset %TEST_PRESET%
if errorlevel 1 (
    set "FAILURE_STEP=Run manual Ninjabrain CTest preset %TEST_PRESET%"
    set "FAILURE_CODE=70"
    goto :fail
)

popd >nul
exit /b 0

:usage
echo Usage: build.bat [release^|debug] [arm64] [--test^|--manual-ninjabrain-tests]
popd >nul 2>nul
exit /b 2

:fail
if defined FAILURE_STEP echo Build failed during: %FAILURE_STEP%
popd >nul
exit /b %FAILURE_CODE%
