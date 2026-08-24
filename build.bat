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
set "PREBUILT_LIBLOGGER_DLL=%PREBUILT_LIBLOGGER_DIR%\liblogger_x64.dll"
set "PREBUILT_LIBLOGGER_PDB=%PREBUILT_LIBLOGGER_DIR%\liblogger_x64.pdb"

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
    set "PREBUILT_LIBLOGGER_DLL=%PREBUILT_LIBLOGGER_DIR%\liblogger_arm64.dll"
    set "PREBUILT_LIBLOGGER_PDB=%PREBUILT_LIBLOGGER_DIR%\liblogger_arm64.pdb"
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

if /I "%TARGET_ARCH%"=="arm64" (
    set "ARTIFACT_DIR=%SCRIPT_DIR%out\build-arm64\bin\%ARTIFACT_CONFIG_DIR%"
) else (
    set "ARTIFACT_DIR=%SCRIPT_DIR%out\build\bin\%ARTIFACT_CONFIG_DIR%"
)
set "CLI_TEST_RUNNER=%ARTIFACT_DIR%\toolscreen_gui_integration_tests.exe"

pushd "%SCRIPT_DIR%" >nul || exit /b 1

echo Downloading the latest signed liblogger artifacts...
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

if "%RUN_NINJABRAIN_MANUAL_TESTS%"=="1" goto :manual_ninjabrain_tests

echo Building DLL with preset %BUILD_PRESET%...
cmake --build --preset %BUILD_PRESET% --target Toolscreen
if errorlevel 1 (
    set "FAILURE_STEP=Build Toolscreen DLL"
    set "FAILURE_CODE=20"
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
    echo Building startup lifecycle test runner with preset %BUILD_PRESET%...
    cmake --build --preset %BUILD_PRESET% --target toolscreen_startup_lifecycle_tests
    if errorlevel 1 (
        set "FAILURE_STEP=Build startup lifecycle test runner"
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
    echo Running CLI integration test runner...
    "%CLI_TEST_RUNNER%" --run-all
    if errorlevel 1 (
        set "FAILURE_STEP=Run CLI integration test runner"
        set "FAILURE_CODE=27"
        goto :fail
    )
)

echo Copying DLL into EXE packaging inputs...
cmake --build --preset %BUILD_PRESET% --target toolscreen_prepare_easyinject_exe_inputs
if errorlevel 1 (
    set "FAILURE_STEP=Prepare EasyInject EXE inputs"
    set "FAILURE_CODE=30"
    goto :fail
)

echo Reconfiguring with preset %CONFIGURE_PRESET% so the EXE packaging step sees the copied DLL...
cmake --preset %CONFIGURE_PRESET% -DTOOLSCREEN_PREBUILT_LIBLOGGER_DLL_PATH="%PREBUILT_LIBLOGGER_DLL%" -DTOOLSCREEN_PREBUILT_LIBLOGGER_PDB_PATH="%PREBUILT_LIBLOGGER_PDB%"
if errorlevel 1 (
    set "FAILURE_STEP=Reconfigure preset %CONFIGURE_PRESET% for EXE packaging"
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
    echo Running CTest packaging smoke tests with preset %TEST_PRESET%...
    ctest --preset %TEST_PRESET% --exclude-regex "^toolscreen_integration_"
    if errorlevel 1 (
        set "FAILURE_STEP=Run CTest packaging smoke tests with preset %TEST_PRESET%"
        set "FAILURE_CODE=70"
        goto :fail
    )
)

popd >nul
exit /b 0

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
