cmake_minimum_required(VERSION 3.21)

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()

if(POLICY CMP0177)
    cmake_policy(SET CMP0177 NEW)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/ToolscreenVersion.cmake")
set(TOOLSCREEN_VERSION "${TOOLSCREEN_VERSION_MAJOR}.${TOOLSCREEN_VERSION_MINOR}.${TOOLSCREEN_VERSION_PATCH}")

set(TOOLSCREEN_PROJECT_NAME "Toolscreen")
set(TOOLSCREEN_COMPANY_NAME "jojoe77777")
set(TOOLSCREEN_COPYRIGHT "Copyright (c) 2026 jojoe77777")
set(TOOLSCREEN_DLL_FILE_DESCRIPTION "Toolscreen hook DLL")
set(TOOLSCREEN_LIBLOGGER_FILE_DESCRIPTION "LibLogger")
set(TOOLSCREEN_INSTALLER_FILE_DESCRIPTION "Toolscreen installer")
set(TOOLSCREEN_DOWNLOADER_FILE_DESCRIPTION "Toolscreen downloader")

project(Toolscreen VERSION ${TOOLSCREEN_VERSION} LANGUAGES C CXX RC)

include(CTest)

set(TOOLSCREEN_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(TOOLSCREEN_GENERATED_VERSION_HEADER "${TOOLSCREEN_GENERATED_DIR}/version.h")
file(MAKE_DIRECTORY "${TOOLSCREEN_GENERATED_DIR}")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/cmake")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/common/version.h.in"
    "${TOOLSCREEN_GENERATED_VERSION_HEADER}"
    @ONLY
)

if(NOT WIN32)
    message(FATAL_ERROR "Toolscreen only supports Windows builds.")
endif()

option(TOOLSCREEN_ENABLE_JAR_PACKAGING "Enable the EasyInjectBundled jar packaging target." ON)
option(TOOLSCREEN_ENABLE_EXE_PACKAGING "Enable the EasyInjectBundled EXE packaging targets." ON)
option(TOOLSCREEN_BUILD_DOWNLOADER_ARTIFACTS "Build the downloader jar and downloader EXE artifacts." ON)
option(TOOLSCREEN_ENABLE_NINJABRAIN_API_MANUAL_TESTS "Enable the manual-only Ninjabrain API integration test target." OFF)
option(TOOLSCREEN_USE_SCCACHE_FRIENDLY_DEBUG_INFO "Use /Z7 instead of /Zi for MSVC release-style builds so compiler launcher caches can hit." OFF)
option(TOOLSCREEN_STAGE_LIBLOGGER_PDB "Stage the liblogger PDB into the build output directory." ON)
set(TOOLSCREEN_PACKAGING_DLL_PATH "" CACHE FILEPATH "Optional override DLL path used for jar and EXE packaging inputs")
set(TOOLSCREEN_PACKAGING_LIBLOGGER_PATH "" CACHE FILEPATH "Optional override liblogger DLL path used for jar and EXE packaging inputs")
set(TOOLSCREEN_PREBUILT_LIBLOGGER_DLL_PATH "" CACHE FILEPATH "Path to the signed prebuilt liblogger DLL to stage into build outputs")
set(TOOLSCREEN_PREBUILT_LIBLOGGER_PDB_PATH "" CACHE FILEPATH "Path to the matching prebuilt liblogger PDB to stage into build outputs")
option(TOOLSCREEN_BUILD_LIBLOGGER_FROM_SOURCE "Build liblogger locally instead of using a prebuilt signed artifact" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(TOOLSCREEN_ARTIFACT_ROOT "${CMAKE_BINARY_DIR}/bin")

function(toolscreen_configure_target_outputs target_name)
    foreach(config Debug Release RelWithDebInfo MinSizeRel)
        string(TOUPPER "${config}" config_upper)
        set_target_properties(${target_name} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY_${config_upper} "${TOOLSCREEN_ARTIFACT_ROOT}/${config}"
            LIBRARY_OUTPUT_DIRECTORY_${config_upper} "${TOOLSCREEN_ARTIFACT_ROOT}/${config}"
            ARCHIVE_OUTPUT_DIRECTORY_${config_upper} "${CMAKE_BINARY_DIR}/lib/${config}"
            PDB_OUTPUT_DIRECTORY_${config_upper} "${TOOLSCREEN_ARTIFACT_ROOT}/${config}"
            COMPILE_PDB_OUTPUT_DIRECTORY_${config_upper} "${CMAKE_BINARY_DIR}/obj/${config}"
        )
    endforeach()
endfunction()

function(toolscreen_enable_release_symbols target_name)
    if(MSVC)
        set(toolscreen_release_debug_info_flag "/Zi")
        if(TOOLSCREEN_USE_SCCACHE_FRIENDLY_DEBUG_INFO)
            set(toolscreen_release_debug_info_flag "/Z7")
        endif()

        target_compile_options(${target_name} PRIVATE
            $<$<CONFIG:Release>:${toolscreen_release_debug_info_flag}>
            $<$<CONFIG:MinSizeRel>:${toolscreen_release_debug_info_flag}>
        )

        if(TARGET ${target_name} AND NOT "${target_name}" MATCHES "_minhook$|_glew$|_imgui$")
            target_link_options(${target_name} PRIVATE
                $<$<CONFIG:Release>:/DEBUG:FULL>
                $<$<CONFIG:MinSizeRel>:/DEBUG:FULL>
            )
        endif()
    endif()
endfunction()

function(toolscreen_set_brand_version branding_file version_string)
    file(READ "${branding_file}" toolscreen_branding_contents)
    string(REPLACE "\r\n" "\n" toolscreen_branding_contents "${toolscreen_branding_contents}")

    if(toolscreen_branding_contents MATCHES "(^|\n)brand\\.version=")
        string(REGEX REPLACE "(^|\n)brand\\.version=[^\n]*" "\\1brand.version=${version_string}"
            toolscreen_branding_contents "${toolscreen_branding_contents}")
    else()
        string(APPEND toolscreen_branding_contents "\nbrand.version=${version_string}\n")
    endif()

    file(WRITE "${branding_file}" "${toolscreen_branding_contents}")
endfunction()

function(toolscreen_write_text_file_if_different file_path content)
    set(toolscreen_should_write TRUE)

    if(EXISTS "${file_path}")
        file(READ "${file_path}" toolscreen_existing_content)
        if(toolscreen_existing_content STREQUAL content)
            set(toolscreen_should_write FALSE)
        endif()
    endif()

    if(toolscreen_should_write)
        file(WRITE "${file_path}" "${content}")
    endif()
endfunction()

if(MSVC)
    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /O2 /Ob2 /GL")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /O2 /Ob2 /GL")
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /LTCG")
endif()

include(FetchContent)

FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/m417z/minhook-detours
    GIT_TAG v1.0.6
)

FetchContent_GetProperties(minhook)
if(NOT minhook_POPULATED)
    FetchContent_Populate(minhook)

    add_library(toolscreen_minhook STATIC
        ${minhook_SOURCE_DIR}/src/MinHook.c
        ${minhook_SOURCE_DIR}/src/SlimDetours/Disassembler.c
        ${minhook_SOURCE_DIR}/src/SlimDetours/InlineHook.c
        ${minhook_SOURCE_DIR}/src/SlimDetours/Instruction.c
        ${minhook_SOURCE_DIR}/src/SlimDetours/Memory.c
        ${minhook_SOURCE_DIR}/src/SlimDetours/Thread.c
        ${minhook_SOURCE_DIR}/src/SlimDetours/Trampoline.c
        ${minhook_SOURCE_DIR}/src/SlimDetours/Transaction.c
    )

    target_include_directories(toolscreen_minhook PUBLIC
        ${minhook_SOURCE_DIR}/src
        ${minhook_SOURCE_DIR}/src/phnt
    )

    set_target_properties(toolscreen_minhook PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()

    toolscreen_enable_release_symbols(toolscreen_minhook)

add_library(Toolscreen::minhook ALIAS toolscreen_minhook)

FetchContent_Declare(
    glew
    URL https://github.com/nigels-com/glew/releases/download/glew-2.2.0/glew-2.2.0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui
    GIT_TAG v1.92.6
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json
    GIT_TAG v3.12.0
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    cpp_httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib
    GIT_TAG v0.40.0
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    webview2_sdk
    URL https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/1.0.3650.58
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_GetProperties(glew)
if(NOT glew_POPULATED)
    FetchContent_Populate(glew)

    add_library(toolscreen_glew STATIC
        ${glew_SOURCE_DIR}/src/glew.c
    )

    target_include_directories(toolscreen_glew PUBLIC ${glew_SOURCE_DIR}/include)
    target_compile_definitions(toolscreen_glew PUBLIC GLEW_STATIC)

    set_target_properties(toolscreen_glew PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        OUTPUT_NAME "libglew32"
    )
endif()

    toolscreen_enable_release_symbols(toolscreen_glew)

add_library(Toolscreen::glew ALIAS toolscreen_glew)

FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
endif()

FetchContent_GetProperties(nlohmann_json)
if(NOT nlohmann_json_POPULATED)
    FetchContent_Populate(nlohmann_json)
endif()

FetchContent_GetProperties(cpp_httplib)
if(NOT cpp_httplib_POPULATED)
    FetchContent_Populate(cpp_httplib)
endif()

FetchContent_GetProperties(webview2_sdk)
if(NOT webview2_sdk_POPULATED)
    FetchContent_Populate(webview2_sdk)
endif()

set(TOOLSCREEN_WEBVIEW2_NATIVE_DIR "${webview2_sdk_SOURCE_DIR}/build/native")
set(TOOLSCREEN_WEBVIEW2_INCLUDE_DIR "${TOOLSCREEN_WEBVIEW2_NATIVE_DIR}/include")
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(TOOLSCREEN_WEBVIEW2_ARCH_DIR "x64")
else()
    set(TOOLSCREEN_WEBVIEW2_ARCH_DIR "x86")
endif()
set(TOOLSCREEN_WEBVIEW2_LOADER_STATIC_LIB "${TOOLSCREEN_WEBVIEW2_NATIVE_DIR}/${TOOLSCREEN_WEBVIEW2_ARCH_DIR}/WebView2LoaderStatic.lib")

if(NOT EXISTS "${TOOLSCREEN_WEBVIEW2_INCLUDE_DIR}/WebView2.h")
    message(FATAL_ERROR "WebView2 SDK headers were not found at ${TOOLSCREEN_WEBVIEW2_INCLUDE_DIR}")
endif()

if(NOT EXISTS "${TOOLSCREEN_WEBVIEW2_LOADER_STATIC_LIB}")
    message(FATAL_ERROR "WebView2LoaderStatic.lib was not found at ${TOOLSCREEN_WEBVIEW2_LOADER_STATIC_LIB}")
endif()

set(TOOLSCREEN_IMGUI_DIR "${imgui_SOURCE_DIR}")

add_library(toolscreen_imgui STATIC
    ${TOOLSCREEN_IMGUI_DIR}/imgui.cpp
    ${TOOLSCREEN_IMGUI_DIR}/imgui_draw.cpp
    ${TOOLSCREEN_IMGUI_DIR}/imgui_tables.cpp
    ${TOOLSCREEN_IMGUI_DIR}/imgui_widgets.cpp
    ${TOOLSCREEN_IMGUI_DIR}/imgui_demo.cpp
    ${TOOLSCREEN_IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp
    ${TOOLSCREEN_IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    ${TOOLSCREEN_IMGUI_DIR}/backends/imgui_impl_win32.cpp
)

target_include_directories(toolscreen_imgui PUBLIC
    ${TOOLSCREEN_IMGUI_DIR}
    ${TOOLSCREEN_IMGUI_DIR}/backends
    ${TOOLSCREEN_IMGUI_DIR}/misc/cpp
)

target_compile_definitions(toolscreen_imgui PUBLIC IMGUI_IMPL_OPENGL_LOADER_GLEW)

target_link_libraries(toolscreen_imgui PUBLIC
    Toolscreen::glew
    dwmapi
    gdi32
    imm32
    opengl32
)

set_target_properties(toolscreen_imgui PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

toolscreen_enable_release_symbols(toolscreen_imgui)

find_package(OpenGL REQUIRED)

set(TOOLSCREEN_MODULE_DIRS
    bootstrap
    common
    config
    features
    gui
    gui/tabs
    hooks
    platform
    render
    runtime
    third_party
)

set(TOOLSCREEN_SOURCES)
set(TOOLSCREEN_HEADERS)

foreach(module_dir IN LISTS TOOLSCREEN_MODULE_DIRS)
    file(GLOB module_sources CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/${module_dir}/*.cpp"
    )
    file(GLOB module_headers CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/${module_dir}/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/${module_dir}/*.inl"
    )

    list(APPEND TOOLSCREEN_SOURCES ${module_sources})
    list(APPEND TOOLSCREEN_HEADERS ${module_headers})
endforeach()

list(REMOVE_DUPLICATES TOOLSCREEN_SOURCES)
list(REMOVE_DUPLICATES TOOLSCREEN_HEADERS)

add_library(${PROJECT_NAME} SHARED
    ${TOOLSCREEN_SOURCES}
    ${TOOLSCREEN_HEADERS}
    ${TOOLSCREEN_GENERATED_VERSION_HEADER}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/resource.rc
)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/fonts"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/src/assets/fonts/Monocraft.ttf"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/fonts/Monocraft.ttf"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/src/assets/fonts/Minecraft.ttf"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/fonts/Minecraft.ttf"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/src/assets/fonts/OpenSans-Regular.ttf"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/fonts/OpenSans-Regular.ttf"
    COMMENT "Copying bundled fonts to output directory"
)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" PREFIX "Source Files" FILES
    ${TOOLSCREEN_SOURCES}
    ${TOOLSCREEN_HEADERS}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/resource.rc
)
source_group("Generated Files" FILES ${TOOLSCREEN_GENERATED_VERSION_HEADER})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${TOOLSCREEN_GENERATED_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${TOOLSCREEN_WEBVIEW2_INCLUDE_DIR}
    ${nlohmann_json_SOURCE_DIR}/single_include
    ${cpp_httplib_SOURCE_DIR}
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    TOOLSCREEN_EXPORTS
    CMAKE_BUILD
    GLEW_STATIC
    NOMINMAX
    UNICODE
    _UNICODE
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Toolscreen::minhook
    Toolscreen::glew
    toolscreen_imgui
    OpenGL::GL
    "${TOOLSCREEN_WEBVIEW2_LOADER_STATIC_LIB}"
    comdlg32
    dbghelp
    dwmapi
    gdi32
    imm32
    msimg32
    ntdll
    opengl32
    psapi
    shell32
    shlwapi
    user32
    version
    ws2_32
)

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W3
        /MP
        /EHa
        /bigobj
    )
endif()

toolscreen_configure_target_outputs(${PROJECT_NAME})
toolscreen_enable_release_symbols(${PROJECT_NAME})

set(TOOLSCREEN_LIBLOGGER_ARTIFACT_DLL_PATH "${TOOLSCREEN_ARTIFACT_ROOT}/$<CONFIG>/liblogger_x64.dll")
set(TOOLSCREEN_LIBLOGGER_ARTIFACT_PDB_PATH "${TOOLSCREEN_ARTIFACT_ROOT}/$<CONFIG>/liblogger_x64.pdb")
set(TOOLSCREEN_LIBLOGGER_BUILD_DEPENDENCY "")

if(NOT TOOLSCREEN_PREBUILT_LIBLOGGER_DLL_PATH)
    set(TOOLSCREEN_PREBUILT_LIBLOGGER_DLL_PATH "${CMAKE_CURRENT_SOURCE_DIR}/out/prebuilt-liblogger/liblogger_x64.dll")
endif()

if(NOT TOOLSCREEN_PREBUILT_LIBLOGGER_PDB_PATH)
    set(TOOLSCREEN_PREBUILT_LIBLOGGER_PDB_PATH "${CMAKE_CURRENT_SOURCE_DIR}/out/prebuilt-liblogger/liblogger_x64.pdb")
endif()

if(TOOLSCREEN_BUILD_LIBLOGGER_FROM_SOURCE)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/liblogger")
    set(TOOLSCREEN_LIBLOGGER_BUILD_DEPENDENCY liblogger_windows)
else()
    get_filename_component(TOOLSCREEN_PREBUILT_LIBLOGGER_DLL_PATH "${TOOLSCREEN_PREBUILT_LIBLOGGER_DLL_PATH}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    get_filename_component(TOOLSCREEN_PREBUILT_LIBLOGGER_PDB_PATH "${TOOLSCREEN_PREBUILT_LIBLOGGER_PDB_PATH}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    get_filename_component(TOOLSCREEN_PREBUILT_LIBLOGGER_DIR "${TOOLSCREEN_PREBUILT_LIBLOGGER_DLL_PATH}" DIRECTORY)
    get_filename_component(TOOLSCREEN_FETCH_LIBLOGGER_SCRIPT_PATH "${CMAKE_CURRENT_SOURCE_DIR}/scripts/fetch_signed_liblogger.ps1" ABSOLUTE)

    file(GENERATE
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/cmake/stage_prebuilt_liblogger_$<CONFIG>.cmake"
            CONTENT "set(SOURCE_DLL \"${TOOLSCREEN_PREBUILT_LIBLOGGER_DLL_PATH}\")\nset(SOURCE_PDB \"${TOOLSCREEN_PREBUILT_LIBLOGGER_PDB_PATH}\")\nset(SOURCE_DIR \"${TOOLSCREEN_PREBUILT_LIBLOGGER_DIR}\")\nset(FETCH_SCRIPT \"${TOOLSCREEN_FETCH_LIBLOGGER_SCRIPT_PATH}\")\nset(DEST_DLL \"${TOOLSCREEN_LIBLOGGER_ARTIFACT_DLL_PATH}\")\nset(DEST_PDB \"${TOOLSCREEN_LIBLOGGER_ARTIFACT_PDB_PATH}\")\nset(STAGE_PDB \"${TOOLSCREEN_STAGE_LIBLOGGER_PDB}\")\nget_filename_component(DEST_DIR \"\${DEST_DLL}\" DIRECTORY)\nfile(MAKE_DIRECTORY \"\${DEST_DIR}\")\nif(NOT EXISTS \"\${FETCH_SCRIPT}\")\n  message(FATAL_ERROR \"Missing liblogger fetch script at '\${FETCH_SCRIPT}'\")\nendif()\nexecute_process(COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File \"\${FETCH_SCRIPT}\" -DestinationDirectory \"\${SOURCE_DIR}\" RESULT_VARIABLE FETCH_RESULT)\nif(NOT FETCH_RESULT EQUAL 0)\n  message(FATAL_ERROR \"Failed to download signed liblogger artifacts into '\${SOURCE_DIR}'\")\nendif()\nif(NOT EXISTS \"\${SOURCE_DLL}\")\n  message(FATAL_ERROR \"Expected prebuilt liblogger DLL at '\${SOURCE_DLL}'\")\nendif()\nexecute_process(COMMAND \"${CMAKE_COMMAND}\" -E copy_if_different \"\${SOURCE_DLL}\" \"\${DEST_DLL}\" RESULT_VARIABLE COPY_DLL_RESULT)\nif(NOT COPY_DLL_RESULT EQUAL 0)\n  message(FATAL_ERROR \"Failed to stage prebuilt liblogger DLL from '\${SOURCE_DLL}' to '\${DEST_DLL}'\")\nendif()\nif(STAGE_PDB)\n  if(NOT EXISTS \"\${SOURCE_PDB}\")\n    message(FATAL_ERROR \"Expected prebuilt liblogger PDB at '\${SOURCE_PDB}'\")\n  endif()\n  execute_process(COMMAND \"${CMAKE_COMMAND}\" -E copy_if_different \"\${SOURCE_PDB}\" \"\${DEST_PDB}\" RESULT_VARIABLE COPY_PDB_RESULT)\n  if(NOT COPY_PDB_RESULT EQUAL 0)\n    message(FATAL_ERROR \"Failed to stage prebuilt liblogger PDB from '\${SOURCE_PDB}' to '\${DEST_PDB}'\")\n  endif()\nendif()\n"
    )

    set(TOOLSCREEN_LIBLOGGER_STAGE_DEPENDENCIES
        "${CMAKE_CURRENT_BINARY_DIR}/cmake/stage_prebuilt_liblogger_$<CONFIG>.cmake"
    )
    if(EXISTS "${TOOLSCREEN_FETCH_LIBLOGGER_SCRIPT_PATH}")
        list(APPEND TOOLSCREEN_LIBLOGGER_STAGE_DEPENDENCIES "${TOOLSCREEN_FETCH_LIBLOGGER_SCRIPT_PATH}")
    endif()

    set(TOOLSCREEN_LIBLOGGER_STAGE_BYPRODUCTS
        "${TOOLSCREEN_LIBLOGGER_ARTIFACT_DLL_PATH}"
    )
    if(TOOLSCREEN_STAGE_LIBLOGGER_PDB)
        list(APPEND TOOLSCREEN_LIBLOGGER_STAGE_BYPRODUCTS "${TOOLSCREEN_LIBLOGGER_ARTIFACT_PDB_PATH}")
    endif()

    add_custom_target(toolscreen_stage_liblogger_artifacts
        BYPRODUCTS ${TOOLSCREEN_LIBLOGGER_STAGE_BYPRODUCTS}
        COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_BINARY_DIR}/cmake/stage_prebuilt_liblogger_$<CONFIG>.cmake"
        DEPENDS ${TOOLSCREEN_LIBLOGGER_STAGE_DEPENDENCIES}
        COMMENT "Fetching and staging prebuilt liblogger artifacts"
        VERBATIM
    )

    set(TOOLSCREEN_LIBLOGGER_BUILD_DEPENDENCY toolscreen_stage_liblogger_artifacts)
endif()

if(TOOLSCREEN_LIBLOGGER_BUILD_DEPENDENCY)
    add_dependencies(${PROJECT_NAME} ${TOOLSCREEN_LIBLOGGER_BUILD_DEPENDENCY})
endif()

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -f "$<TARGET_FILE_DIR:${PROJECT_NAME}>/WebView2Loader.dll"
    COMMENT "Removing stale copied WebView2Loader.dll"
)

enable_testing()
file(GENERATE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/cmake/check_toolscreen_artifact_$<CONFIG>.cmake"
    CONTENT "if(NOT EXISTS \"$<TARGET_FILE:${PROJECT_NAME}>\")\n  message(FATAL_ERROR \"Expected built Toolscreen artifact at '$<TARGET_FILE:${PROJECT_NAME}>'\")\nendif()\nif(NOT EXISTS \"$<TARGET_PDB_FILE:${PROJECT_NAME}>\")\n  message(FATAL_ERROR \"Expected Toolscreen PDB at '$<TARGET_PDB_FILE:${PROJECT_NAME}>'\")\nendif()\n"
)

file(GENERATE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/cmake/check_liblogger_artifact_$<CONFIG>.cmake"
    CONTENT "set(EXPECT_PDB \"${TOOLSCREEN_STAGE_LIBLOGGER_PDB}\")\nif(NOT EXISTS \"${TOOLSCREEN_LIBLOGGER_ARTIFACT_DLL_PATH}\")\n  message(FATAL_ERROR \"Expected liblogger DLL at '${TOOLSCREEN_LIBLOGGER_ARTIFACT_DLL_PATH}'\")\nendif()\nif(EXPECT_PDB AND NOT EXISTS \"${TOOLSCREEN_LIBLOGGER_ARTIFACT_PDB_PATH}\")\n  message(FATAL_ERROR \"Expected liblogger PDB at '${TOOLSCREEN_LIBLOGGER_ARTIFACT_PDB_PATH}'\")\nendif()\n"
)

add_test(
    NAME toolscreen_artifact_exists
    COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_BINARY_DIR}/cmake/check_toolscreen_artifact_$<CONFIG>.cmake"
)

add_test(
    NAME liblogger_artifact_exists
    COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_BINARY_DIR}/cmake/check_liblogger_artifact_$<CONFIG>.cmake"
)

if(BUILD_TESTING AND TOOLSCREEN_ENABLE_NINJABRAIN_API_MANUAL_TESTS)
    set(TOOLSCREEN_NINJABRAIN_BOT_VERSION "1.5.2")
    set(TOOLSCREEN_NINJABRAIN_BOT_SHA256 "98099F6321A90D4ACEC13400E86D05F7AF8D60E5639C2F38427E968DC71450FF")
    set(TOOLSCREEN_NINJABRAIN_BOT_URL
        "https://github.com/Ninjabrain1/Ninjabrain-Bot/releases/download/${TOOLSCREEN_NINJABRAIN_BOT_VERSION}/Ninjabrain-Bot-${TOOLSCREEN_NINJABRAIN_BOT_VERSION}.jar")
    set(TOOLSCREEN_NINJABRAIN_BOT_JAR
        "${CMAKE_CURRENT_BINARY_DIR}/downloads/Ninjabrain-Bot-${TOOLSCREEN_NINJABRAIN_BOT_VERSION}.jar")

    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/downloads")

    set(TOOLSCREEN_NINJABRAIN_BOT_DOWNLOAD_REQUIRED ON)
    if(EXISTS "${TOOLSCREEN_NINJABRAIN_BOT_JAR}")
        file(SHA256 "${TOOLSCREEN_NINJABRAIN_BOT_JAR}" TOOLSCREEN_NINJABRAIN_BOT_EXISTING_SHA256)
        if(TOOLSCREEN_NINJABRAIN_BOT_EXISTING_SHA256 STREQUAL TOOLSCREEN_NINJABRAIN_BOT_SHA256)
            set(TOOLSCREEN_NINJABRAIN_BOT_DOWNLOAD_REQUIRED OFF)
        else()
            file(REMOVE "${TOOLSCREEN_NINJABRAIN_BOT_JAR}")
        endif()
    endif()

    if(TOOLSCREEN_NINJABRAIN_BOT_DOWNLOAD_REQUIRED)
        file(DOWNLOAD
            "${TOOLSCREEN_NINJABRAIN_BOT_URL}"
            "${TOOLSCREEN_NINJABRAIN_BOT_JAR}"
            EXPECTED_HASH "SHA256=${TOOLSCREEN_NINJABRAIN_BOT_SHA256}"
            TLS_VERIFY ON
            SHOW_PROGRESS
            STATUS TOOLSCREEN_NINJABRAIN_BOT_DOWNLOAD_STATUS
        )
        list(GET TOOLSCREEN_NINJABRAIN_BOT_DOWNLOAD_STATUS 0 TOOLSCREEN_NINJABRAIN_BOT_DOWNLOAD_STATUS_CODE)
        list(GET TOOLSCREEN_NINJABRAIN_BOT_DOWNLOAD_STATUS 1 TOOLSCREEN_NINJABRAIN_BOT_DOWNLOAD_STATUS_TEXT)
        if(NOT TOOLSCREEN_NINJABRAIN_BOT_DOWNLOAD_STATUS_CODE EQUAL 0)
            message(FATAL_ERROR
                "Failed to download Ninjabrain Bot ${TOOLSCREEN_NINJABRAIN_BOT_VERSION}: ${TOOLSCREEN_NINJABRAIN_BOT_DOWNLOAD_STATUS_TEXT}")
        endif()
    endif()

    find_package(Java COMPONENTS Runtime REQUIRED)

    string(REPLACE "\\" "\\\\" TOOLSCREEN_TEST_JAVA_EXECUTABLE_LITERAL "${Java_JAVA_EXECUTABLE}")
    string(REPLACE "\\" "\\\\" TOOLSCREEN_TEST_NINJABRAIN_BOT_JAR_LITERAL "${TOOLSCREEN_NINJABRAIN_BOT_JAR}")

    add_executable(toolscreen_ninjabrain_api_tests
        tests/ninjabrain_api_tests.cpp
        src/features/ninjabrain_api.cpp
        src/features/ninjabrain_data.cpp
    )

    target_include_directories(toolscreen_ninjabrain_api_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${nlohmann_json_SOURCE_DIR}/single_include
        ${cpp_httplib_SOURCE_DIR}
    )

    target_compile_definitions(toolscreen_ninjabrain_api_tests PRIVATE
        CMAKE_BUILD
        NOMINMAX
        UNICODE
        _UNICODE
        TOOLSCREEN_TEST_JAVA_EXECUTABLE="${TOOLSCREEN_TEST_JAVA_EXECUTABLE_LITERAL}"
        TOOLSCREEN_TEST_NINJABRAIN_BOT_JAR="${TOOLSCREEN_TEST_NINJABRAIN_BOT_JAR_LITERAL}"
    )

    target_link_libraries(toolscreen_ninjabrain_api_tests PRIVATE
        ws2_32
    )

    if(MSVC)
        target_compile_options(toolscreen_ninjabrain_api_tests PRIVATE
            /W3
            /MP
            /EHa
        )
    endif()

    toolscreen_configure_target_outputs(toolscreen_ninjabrain_api_tests)
    toolscreen_enable_release_symbols(toolscreen_ninjabrain_api_tests)

    set(TOOLSCREEN_NINJABRAIN_API_TEST_CASES
        boat_event_parses_snapshot
        connection_tracker_treats_blind_as_connected
        blind_event_parses_snapshot
        blind_event_disabled_clears_snapshot
        stronghold_failure_preserves_boat
        stronghold_clear_preserves_blind
        stronghold_event_parses_prediction_details
        stronghold_increment_recovery_without_correction_increments
        live_ninjabrain_api_http_server_smoke
        live_ninjabrain_api_session_reconnects
    )

    foreach(test_case IN LISTS TOOLSCREEN_NINJABRAIN_API_TEST_CASES)
        add_test(
            NAME toolscreen_ninjabrain_${test_case}
            COMMAND toolscreen_ninjabrain_api_tests --run ${test_case}
        )
        set_tests_properties(
            toolscreen_ninjabrain_${test_case}
            PROPERTIES LABELS "manual;ninjabrain"
        )
    endforeach()
endif()

if(TOOLSCREEN_ENABLE_JAR_PACKAGING OR TOOLSCREEN_ENABLE_EXE_PACKAGING)
    if(TOOLSCREEN_ENABLE_JAR_PACKAGING)
        find_package(Java COMPONENTS Development REQUIRED)

        find_program(TOOLSCREEN_MAVEN_EXECUTABLE NAMES mvn.cmd mvn)

        if(NOT TOOLSCREEN_MAVEN_EXECUTABLE)
            FetchContent_Declare(
                maven
                URL https://archive.apache.org/dist/maven/maven-3/3.9.9/binaries/apache-maven-3.9.9-bin.zip
                URL_HASH SHA512=8beac8d11ef208f1e2a8df0682b9448a9a363d2ad13ca74af43705549e72e74c9378823bf689287801cbbfc2f6ea9596201d19ccacfdfb682ee8a2ff4c4418ba
            )

            FetchContent_GetProperties(maven)
            if(NOT maven_POPULATED)
                FetchContent_Populate(maven)
            endif()

            set(TOOLSCREEN_MAVEN_EXECUTABLE "${maven_SOURCE_DIR}/bin/mvn.cmd")
        endif()
    endif()

    FetchContent_Declare(
        easyinjectbundled
        GIT_REPOSITORY https://github.com/jojoe77777/EasyInjectBundled.git
        GIT_TAG 4c93d4e63c3d8c4813bcaa68ebedec60dc555833
    )

    FetchContent_GetProperties(easyinjectbundled)
    if(NOT easyinjectbundled_POPULATED)
        FetchContent_Populate(easyinjectbundled)
    endif()

    file(REMOVE "${easyinjectbundled_SOURCE_DIR}/custom-dlls/WebView2Loader.dll")

    set(TOOLSCREEN_EASYINJECT_BRANDING_PROPERTIES "${easyinjectbundled_SOURCE_DIR}/branding.properties")
    toolscreen_set_brand_version("${TOOLSCREEN_EASYINJECT_BRANDING_PROPERTIES}" "${PROJECT_VERSION}")

    set(JAR_NAME "Toolscreen-${PROJECT_VERSION}-double-click-me.jar")
    set(JAR_OUTPUT "${TOOLSCREEN_ARTIFACT_ROOT}/$<CONFIG>/${JAR_NAME}")
    set(DOWNLOADER_JAR_NAME "toolscreen-downloader.jar")
    set(DOWNLOADER_JAR_OUTPUT "${TOOLSCREEN_ARTIFACT_ROOT}/$<CONFIG>/${DOWNLOADER_JAR_NAME}")
    set(EXE_NAME "Toolscreen-${PROJECT_VERSION}-double-click-me.exe")
    set(EXE_OUTPUT "${TOOLSCREEN_ARTIFACT_ROOT}/$<CONFIG>/${EXE_NAME}")
    set(DOWNLOADER_EXE_NAME "toolscreen-downloader.exe")
    set(DOWNLOADER_EXE_OUTPUT "${TOOLSCREEN_ARTIFACT_ROOT}/$<CONFIG>/${DOWNLOADER_EXE_NAME}")

    set(TOOLSCREEN_PACKAGING_DLL_SOURCE "$<TARGET_FILE:${PROJECT_NAME}>")
    if(TOOLSCREEN_PACKAGING_DLL_PATH)
        set(TOOLSCREEN_PACKAGING_DLL_SOURCE "${TOOLSCREEN_PACKAGING_DLL_PATH}")
    endif()

    set(TOOLSCREEN_PACKAGING_LOGGER_SOURCE "${TOOLSCREEN_LIBLOGGER_ARTIFACT_DLL_PATH}")
    if(TOOLSCREEN_PACKAGING_LIBLOGGER_PATH)
        set(TOOLSCREEN_PACKAGING_LOGGER_SOURCE "${TOOLSCREEN_PACKAGING_LIBLOGGER_PATH}")
    endif()

    set(TOOLSCREEN_PACKAGING_DEPENDENCIES)
    if(NOT TOOLSCREEN_PACKAGING_DLL_PATH)
        list(APPEND TOOLSCREEN_PACKAGING_DEPENDENCIES ${PROJECT_NAME})
    endif()
    if(TOOLSCREEN_LIBLOGGER_BUILD_DEPENDENCY AND NOT TOOLSCREEN_PACKAGING_LIBLOGGER_PATH)
        list(APPEND TOOLSCREEN_PACKAGING_DEPENDENCIES ${TOOLSCREEN_LIBLOGGER_BUILD_DEPENDENCY})
    endif()

    if(TOOLSCREEN_ENABLE_JAR_PACKAGING)
        set(TOOLSCREEN_JAR_TARGET_COMMANDS
            COMMAND ${CMAKE_COMMAND} -E make_directory "${easyinjectbundled_SOURCE_DIR}/custom-dlls"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${easyinjectbundled_SOURCE_DIR}/src/main/resources/dlls"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${TOOLSCREEN_ARTIFACT_ROOT}/$<CONFIG>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${TOOLSCREEN_PACKAGING_DLL_SOURCE}" "${easyinjectbundled_SOURCE_DIR}/custom-dlls/Toolscreen.dll"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${TOOLSCREEN_PACKAGING_LOGGER_SOURCE}" "${easyinjectbundled_SOURCE_DIR}/src/main/resources/dlls/liblogger_x64.dll"
            COMMAND ${CMAKE_COMMAND} -E rm -f "${easyinjectbundled_SOURCE_DIR}/custom-dlls/WebView2Loader.dll"
            COMMAND ${TOOLSCREEN_MAVEN_EXECUTABLE} -f "${easyinjectbundled_SOURCE_DIR}/pom.xml" -q --batch-mode --no-transfer-progress package
                    -Dbrand.name=Toolscreen
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${easyinjectbundled_SOURCE_DIR}/target/${JAR_NAME}" "${JAR_OUTPUT}"
        )
        set(TOOLSCREEN_JAR_TARGET_COMMENT "Building ${JAR_NAME}")

        if(TOOLSCREEN_BUILD_DOWNLOADER_ARTIFACTS)
            file(GENERATE
                OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/cmake/copy_toolscreen_downloader_jar_$<CONFIG>.cmake"
                CONTENT "set(NEW_DOWNLOADER_JAR \"${easyinjectbundled_SOURCE_DIR}/target/${DOWNLOADER_JAR_NAME}\")\nset(TYPO_DOWNLOADER_JAR \"${easyinjectbundled_SOURCE_DIR}/target/tioolscrteen-downloader.jar\")\nset(LEGACY_INSTALLER_JAR \"${easyinjectbundled_SOURCE_DIR}/target/toolscreen-installer.jar\")\nset(DOWNLOADER_JAR_OUTPUT \"${DOWNLOADER_JAR_OUTPUT}\")\nif(EXISTS \"${easyinjectbundled_SOURCE_DIR}/target/${DOWNLOADER_JAR_NAME}\")\n  set(DOWNLOADER_JAR_SOURCE \"${easyinjectbundled_SOURCE_DIR}/target/${DOWNLOADER_JAR_NAME}\")\nelseif(EXISTS \"${easyinjectbundled_SOURCE_DIR}/target/tioolscrteen-downloader.jar\")\n  set(DOWNLOADER_JAR_SOURCE \"${easyinjectbundled_SOURCE_DIR}/target/tioolscrteen-downloader.jar\")\nelseif(EXISTS \"${easyinjectbundled_SOURCE_DIR}/target/toolscreen-installer.jar\")\n  set(DOWNLOADER_JAR_SOURCE \"${easyinjectbundled_SOURCE_DIR}/target/toolscreen-installer.jar\")\nelse()\n  message(FATAL_ERROR \"Expected downloader jar at '${easyinjectbundled_SOURCE_DIR}/target/${DOWNLOADER_JAR_NAME}', typo downloader jar at '${easyinjectbundled_SOURCE_DIR}/target/tioolscrteen-downloader.jar', or legacy installer jar at '${easyinjectbundled_SOURCE_DIR}/target/toolscreen-installer.jar'\")\nendif()\nexecute_process(COMMAND \"${CMAKE_COMMAND}\" -E copy_if_different \"\${DOWNLOADER_JAR_SOURCE}\" \"\${DOWNLOADER_JAR_OUTPUT}\" RESULT_VARIABLE COPY_RESULT)\nif(NOT COPY_RESULT EQUAL 0)\n  message(FATAL_ERROR \"Failed to copy downloader jar from '\${DOWNLOADER_JAR_SOURCE}' to '\${DOWNLOADER_JAR_OUTPUT}'\")\nendif()\n"
            )

            list(APPEND TOOLSCREEN_JAR_TARGET_COMMANDS
                COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_BINARY_DIR}/cmake/copy_toolscreen_downloader_jar_$<CONFIG>.cmake"
            )
            set(TOOLSCREEN_JAR_TARGET_COMMENT "Building ${JAR_NAME} and ${DOWNLOADER_JAR_NAME}")
        endif()

        add_custom_target(jar ALL
            ${TOOLSCREEN_JAR_TARGET_COMMANDS}
            DEPENDS ${TOOLSCREEN_PACKAGING_DEPENDENCIES}
            COMMENT "${TOOLSCREEN_JAR_TARGET_COMMENT}"
            VERBATIM
        )
    endif()

    if(TOOLSCREEN_ENABLE_EXE_PACKAGING)
        FetchContent_Declare(
            easyinjectbundled_exe
            GIT_REPOSITORY https://github.com/jojoe77777/EasyInjectBundled.git
            GIT_TAG exe
            GIT_SHALLOW TRUE
        )

        FetchContent_GetProperties(easyinjectbundled_exe)
        if(NOT easyinjectbundled_exe_POPULATED)
            FetchContent_Populate(easyinjectbundled_exe)
        endif()

        file(REMOVE "${easyinjectbundled_exe_SOURCE_DIR}/custom-dlls/WebView2Loader.dll")

        set(TOOLSCREEN_EASYINJECT_EXE_BRANDING_PROPERTIES "${easyinjectbundled_exe_SOURCE_DIR}/branding.properties")
        toolscreen_set_brand_version("${TOOLSCREEN_EASYINJECT_EXE_BRANDING_PROPERTIES}" "${PROJECT_VERSION}")
        toolscreen_write_text_file_if_different(
            "${easyinjectbundled_exe_SOURCE_DIR}/exe/src/embedded_resources.rc.in"
            [=[#include <winres.h>

1 ICON "@APP_ICON_FILE_RC@"

VS_VERSION_INFO VERSIONINFO
 FILEVERSION @TOOLSCREEN_VERSION_MAJOR@,@TOOLSCREEN_VERSION_MINOR@,@TOOLSCREEN_VERSION_PATCH@,0
 PRODUCTVERSION @TOOLSCREEN_VERSION_MAJOR@,@TOOLSCREEN_VERSION_MINOR@,@TOOLSCREEN_VERSION_PATCH@,0
 FILEFLAGSMASK 0x3fL
#ifdef _DEBUG
 FILEFLAGS 0x1L
#else
 FILEFLAGS 0x0L
#endif
 FILEOS 0x40004L
 FILETYPE 0x1L
 FILESUBTYPE 0x0L
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "040904b0"
        BEGIN
            VALUE "CompanyName", "@TOOLSCREEN_COMPANY_NAME@\0"
            VALUE "FileDescription", "@TOOLSCREEN_INSTALLER_FILE_DESCRIPTION@\0"
            VALUE "FileVersion", "@TOOLSCREEN_VERSION@\0"
            VALUE "InternalName", "Toolscreen\0"
            VALUE "LegalCopyright", "@TOOLSCREEN_COPYRIGHT@\0"
            VALUE "OriginalFilename", "Toolscreen-@BRAND_VERSION@-double-click-me.exe\0"
            VALUE "ProductName", "@TOOLSCREEN_PROJECT_NAME@\0"
            VALUE "ProductVersion", "@TOOLSCREEN_VERSION@\0"
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x409, 1200
    END
END

@BRANDING_RESOURCE_ID@ RCDATA "@BRANDING_FILE_RC@"
@DLL_INDEX_RESOURCE_ID@ RCDATA "@EMBED_DLL_INDEX_FILE_RC@"
@EMBED_DLL_RESOURCE_LINES@
]=]
        )

        file(MAKE_DIRECTORY "${easyinjectbundled_exe_SOURCE_DIR}/custom-dlls")
        add_subdirectory("${easyinjectbundled_exe_SOURCE_DIR}/exe" "${CMAKE_CURRENT_BINARY_DIR}/easyinjectbundled-exe")
        toolscreen_configure_target_outputs(EasyInjectExe)

        add_custom_target(toolscreen_prepare_easyinject_exe_inputs
            COMMAND ${CMAKE_COMMAND} -E make_directory "${easyinjectbundled_exe_SOURCE_DIR}/custom-dlls"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${easyinjectbundled_exe_SOURCE_DIR}/src/main/resources/dlls"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${TOOLSCREEN_PACKAGING_DLL_SOURCE}" "${easyinjectbundled_exe_SOURCE_DIR}/custom-dlls/Toolscreen.dll"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${TOOLSCREEN_PACKAGING_LOGGER_SOURCE}" "${easyinjectbundled_exe_SOURCE_DIR}/src/main/resources/dlls/liblogger_x64.dll"
            COMMAND ${CMAKE_COMMAND} -E rm -f "${easyinjectbundled_exe_SOURCE_DIR}/custom-dlls/WebView2Loader.dll"
            DEPENDS ${TOOLSCREEN_PACKAGING_DEPENDENCIES}
            COMMENT "Preparing EasyInject exe packaging inputs"
            VERBATIM
        )

        add_dependencies(EasyInjectExe toolscreen_prepare_easyinject_exe_inputs)

        add_custom_target(installer_exe ALL
            DEPENDS EasyInjectExe
            COMMENT "Building ${EXE_NAME}"
        )

        file(GENERATE
            OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/cmake/check_toolscreen_exe_$<CONFIG>.cmake"
            CONTENT "if(NOT EXISTS \"${EXE_OUTPUT}\")\n  message(FATAL_ERROR \"Expected Toolscreen exe at '${EXE_OUTPUT}'\")\nendif()\n"
        )

        file(GENERATE
            OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/cmake/check_toolscreen_exe_embeds_dll_$<CONFIG>.cmake"
            CONTENT "set(EMBED_DLL_INDEX_FILE \"${CMAKE_CURRENT_BINARY_DIR}/easyinjectbundled-exe/embedded_dll_index.txt\")\nif(NOT EXISTS \"${CMAKE_CURRENT_BINARY_DIR}/easyinjectbundled-exe/embedded_dll_index.txt\")\n  message(FATAL_ERROR \"Expected EasyInject embedded DLL index at '${CMAKE_CURRENT_BINARY_DIR}/easyinjectbundled-exe/embedded_dll_index.txt'\")\nendif()\nfile(READ \"${CMAKE_CURRENT_BINARY_DIR}/easyinjectbundled-exe/embedded_dll_index.txt\" EMBED_DLL_INDEX_CONTENT)\nif(NOT EMBED_DLL_INDEX_CONTENT MATCHES \"Toolscreen[.]dll\")\n  message(FATAL_ERROR \"Expected EasyInject EXE packaging metadata to embed Toolscreen.dll\")\nendif()\n"
        )

        add_test(
            NAME toolscreen_exe_exists
            COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_BINARY_DIR}/cmake/check_toolscreen_exe_$<CONFIG>.cmake"
        )

        add_test(
            NAME toolscreen_exe_embeds_dll
            COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_BINARY_DIR}/cmake/check_toolscreen_exe_embeds_dll_$<CONFIG>.cmake"
        )

        if(TOOLSCREEN_BUILD_DOWNLOADER_ARTIFACTS)
            set(TOOLSCREEN_DOWNLOADER_SOURCE_DIR "${easyinjectbundled_SOURCE_DIR}/toolscreen-installer-exe")
            if(EXISTS "${easyinjectbundled_SOURCE_DIR}/toolscreen-downloader-exe/CMakeLists.txt")
                set(TOOLSCREEN_DOWNLOADER_SOURCE_DIR "${easyinjectbundled_SOURCE_DIR}/toolscreen-downloader-exe")
            elseif(EXISTS "${easyinjectbundled_SOURCE_DIR}/tioolscrteen-downloader-exe/CMakeLists.txt")
                set(TOOLSCREEN_DOWNLOADER_SOURCE_DIR "${easyinjectbundled_SOURCE_DIR}/tioolscrteen-downloader-exe")
            endif()

            toolscreen_write_text_file_if_different(
                "${TOOLSCREEN_DOWNLOADER_SOURCE_DIR}/src/app.rc.in"
                [=[#include <winres.h>

1 ICON "@APP_ICON_FILE_RC@"

VS_VERSION_INFO VERSIONINFO
 FILEVERSION @TOOLSCREEN_VERSION_MAJOR@,@TOOLSCREEN_VERSION_MINOR@,@TOOLSCREEN_VERSION_PATCH@,0
 PRODUCTVERSION @TOOLSCREEN_VERSION_MAJOR@,@TOOLSCREEN_VERSION_MINOR@,@TOOLSCREEN_VERSION_PATCH@,0
 FILEFLAGSMASK 0x3fL
#ifdef _DEBUG
 FILEFLAGS 0x1L
#else
 FILEFLAGS 0x0L
#endif
 FILEOS 0x40004L
 FILETYPE 0x1L
 FILESUBTYPE 0x0L
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "040904b0"
        BEGIN
            VALUE "CompanyName", "@TOOLSCREEN_COMPANY_NAME@\0"
            VALUE "FileDescription", "@TOOLSCREEN_DOWNLOADER_FILE_DESCRIPTION@\0"
            VALUE "FileVersion", "@TOOLSCREEN_VERSION@\0"
            VALUE "InternalName", "toolscreen-downloader\0"
            VALUE "LegalCopyright", "@TOOLSCREEN_COPYRIGHT@\0"
            VALUE "OriginalFilename", "toolscreen-downloader.exe\0"
            VALUE "ProductName", "@TOOLSCREEN_PROJECT_NAME@\0"
            VALUE "ProductVersion", "@TOOLSCREEN_VERSION@\0"
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x409, 1200
    END
END
]=]
            )

            add_subdirectory("${TOOLSCREEN_DOWNLOADER_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/easyinjectbundled-toolscreen-downloader-exe")

            if(TARGET ToolscreenDownloaderExe)
                set(TOOLSCREEN_DOWNLOADER_TARGET ToolscreenDownloaderExe)
            elseif(TARGET TioolscrteenDownloaderExe)
                set(TOOLSCREEN_DOWNLOADER_TARGET TioolscrteenDownloaderExe)
            elseif(TARGET ToolscreenInstallerExe)
                set(TOOLSCREEN_DOWNLOADER_TARGET ToolscreenInstallerExe)
            else()
                message(FATAL_ERROR "Could not find the EasyInject downloader executable target after adding ${TOOLSCREEN_DOWNLOADER_SOURCE_DIR}")
            endif()

            set_target_properties(${TOOLSCREEN_DOWNLOADER_TARGET} PROPERTIES OUTPUT_NAME "toolscreen-downloader")
            toolscreen_configure_target_outputs(${TOOLSCREEN_DOWNLOADER_TARGET})

            add_custom_target(downloader_exe ALL
                DEPENDS ${TOOLSCREEN_DOWNLOADER_TARGET}
                COMMENT "Building ${DOWNLOADER_EXE_NAME}"
            )

            file(GENERATE
                OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/cmake/check_toolscreen_downloader_exe_$<CONFIG>.cmake"
                CONTENT "if(NOT EXISTS \"${DOWNLOADER_EXE_OUTPUT}\")\n  message(FATAL_ERROR \"Expected Toolscreen downloader exe at '${DOWNLOADER_EXE_OUTPUT}'\")\nendif()\n"
            )

            add_test(
                NAME toolscreen_downloader_exe_exists
                COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_BINARY_DIR}/cmake/check_toolscreen_downloader_exe_$<CONFIG>.cmake"
            )
        endif()
    endif()

    if(TOOLSCREEN_ENABLE_JAR_PACKAGING)
        file(GENERATE
            OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/cmake/check_toolscreen_jar_$<CONFIG>.cmake"
            CONTENT "if(NOT EXISTS \"${JAR_OUTPUT}\")\n  message(FATAL_ERROR \"Expected Toolscreen jar at '${JAR_OUTPUT}'\")\nendif()\n"
        )

        add_test(
            NAME toolscreen_jar_exists
            COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_BINARY_DIR}/cmake/check_toolscreen_jar_$<CONFIG>.cmake"
        )

        if(TOOLSCREEN_BUILD_DOWNLOADER_ARTIFACTS)
            file(GENERATE
                OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/cmake/check_toolscreen_downloader_jar_$<CONFIG>.cmake"
                CONTENT "if(NOT EXISTS \"${DOWNLOADER_JAR_OUTPUT}\")\n  message(FATAL_ERROR \"Expected Toolscreen downloader jar at '${DOWNLOADER_JAR_OUTPUT}'\")\nendif()\n"
            )

            add_test(
                NAME toolscreen_downloader_jar_exists
                COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_BINARY_DIR}/cmake/check_toolscreen_downloader_jar_$<CONFIG>.cmake"
            )
        endif()
    endif()
endif()

set(TOOLSCREEN_GUI_INTEGRATION_SUITE_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/gui_integration_tests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/gui_integration/config_tests.inl
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/gui_integration/rebind_tests.inl
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/gui_integration/render_tests.inl
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/gui_integration/ui_and_log_tests.inl
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/gui_integration/runner.inl
)

add_executable(toolscreen_gui_integration_tests
    ${TOOLSCREEN_SOURCES}
    ${TOOLSCREEN_GENERATED_VERSION_HEADER}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/resource.rc
    ${TOOLSCREEN_GUI_INTEGRATION_SUITE_FILES}
)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/tests" PREFIX "Tests" FILES
    ${TOOLSCREEN_GUI_INTEGRATION_SUITE_FILES}
)

target_include_directories(toolscreen_gui_integration_tests PRIVATE
    ${TOOLSCREEN_GENERATED_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${TOOLSCREEN_WEBVIEW2_INCLUDE_DIR}
    ${nlohmann_json_SOURCE_DIR}/single_include
    ${cpp_httplib_SOURCE_DIR}
)

target_compile_definitions(toolscreen_gui_integration_tests PRIVATE
    CMAKE_BUILD
    GLEW_STATIC
    NOMINMAX
    TOOLSCREEN_GUI_INTEGRATION_TESTS
    UNICODE
    _UNICODE
)

target_link_libraries(toolscreen_gui_integration_tests PRIVATE
    Toolscreen::minhook
    Toolscreen::glew
    toolscreen_imgui
    OpenGL::GL
    "${TOOLSCREEN_WEBVIEW2_LOADER_STATIC_LIB}"
    comdlg32
    dbghelp
    dwmapi
    gdi32
    imm32
    msimg32
    ntdll
    opengl32
    psapi
    shell32
    shlwapi
    user32
    version
    winhttp
)

if(MSVC)
    target_compile_options(toolscreen_gui_integration_tests PRIVATE
        /W3
        /MP
        /EHa
        /bigobj
    )
endif()

toolscreen_configure_target_outputs(toolscreen_gui_integration_tests)
toolscreen_enable_release_symbols(toolscreen_gui_integration_tests)

set(TOOLSCREEN_GUI_INTEGRATION_TEST_CASES
    log-multi-instance-latest-suffix
    log-header-includes-peer-info
    log-archive-collision-handling
    log-released-slot-reuse
    config-default-load
    config-roundtrip
    config-roundtrip-global-settings
    config-roundtrip-modes
    config-roundtrip-mirrors
    config-roundtrip-mirror-groups
    config-roundtrip-images
    config-roundtrip-window-overlays
    config-roundtrip-browser-overlays
    config-roundtrip-hotkeys
    config-roundtrip-sensitivity-hotkeys
    config-roundtrip-cursors-eyezoom
    config-roundtrip-key-rebinds-appearance
    config-roundtrip-debug-settings
    config-load-embedded-ninjabrain-presets
    config-load-bundled-font-paths-normalized
    config-load-missing-required-modes
    config-load-invalid-hotkey-mode-references
    config-load-relative-mode-dimensions
    config-load-expression-mode-dimensions
    config-load-legacy-version-upgrade
    config-load-clamp-global-values
    config-load-mode-default-dimensions-restored
    config-load-mode-source-lists-loaded
    config-downgrade-sources-compat
    config-migrate-version-applies
    config-load-mode-percentage-dimensions-detected
    config-load-mode-typed-sources-loaded
    config-load-mode-source-helpers
    config-load-empty-main-hotkey-fallback
    config-load-missing-gui-hotkey-defaulted
    config-load-empty-gui-hotkey-defaulted
    config-load-legacy-mirror-gamma-migrated
    config-load-mirror-capture-dimensions-clamped
    config-load-eyezoom-clone-width-normalized
    config-load-eyezoom-overlay-width-defaulted
    config-load-eyezoom-overlay-width-clamped
    config-load-eyezoom-legacy-margins-migrated
    config-load-eyezoom-legacy-custom-position-migrated
    config-load-eyezoom-invalid-active-overlay-reset
    config-load-window-overlay-crop-migrated
    config-load-window-overlay-capture-method-migrated
    config-load-key-rebind-unicode-string-parsed
    config-load-key-rebind-escaped-unicode-string-parsed
    config-load-key-rebind-hex-unicode-string-parsed
    config-load-key-rebind-invalid-unicode-defaulted
    config-load-key-rebind-legacy-show-indicator-migrated
    config-load-key-rebind-cannot-type-clears-typed-output
    config-load-key-rebind-shift-layer-caps-lock-parsed
    config-load-key-rebind-shift-layer-caps-lock-defaulted
    config-load-key-rebind-cursor-state-defaulted
    config-publish-key-rebind-cannot-type-clears-typed-output
    key-rebind-runtime-full-forwarding
    key-rebind-runtime-split-vk-output
    key-rebind-runtime-split-unicode-output
    key-rebind-runtime-unicode-output-ignores-vk
    key-rebind-runtime-shift-layer-unicode-output-ignores-vk
    key-rebind-runtime-shift-layer-shift-activated
    key-rebind-runtime-shift-layer-caps-lock-activated
    key-rebind-runtime-full-caps-lock-honored
    key-rebind-runtime-split-caps-lock-ignored
    key-rebind-runtime-non-typable-trigger-consumes-char
    key-rebind-runtime-trigger-disabled-still-types
    key-rebind-runtime-types-disabled-still-triggers
    key-rebind-runtime-shift-types-disabled-still-triggers
    key-rebind-runtime-mouse-source-emits-key-and-char
    key-rebind-runtime-modifier-output-released-on-deactivate
    key-rebind-runtime-suppressed-caps-lock-released-on-deactivate
    key-rebind-runtime-custom-modifier-output-uses-synthetic-key
    key-rebind-runtime-wndproc-keeps-synthetic-modifier-held
    key-rebind-runtime-disabled-rebind-ignored
    key-rebind-runtime-cursor-state-keyup-passthrough
    key-rebind-runtime-held-output-released-on-rebind-change
    key-rebind-runtime-held-output-released-on-no-match-up
    key-rebind-runtime-held-output-released-on-mouse-output-up
    key-rebind-runtime-held-output-released-on-modifier-output-up
    key-rebind-runtime-held-output-released-on-gui-open
    key-rebind-runtime-cursor-state-priority-and-fallback
    key-rebind-gui-keyboard-layout-full-bind-and-trigger
    key-rebind-gui-keyboard-layout-split-bind-and-trigger
    key-rebind-gui-text-override-pick-rejects-non-typable-key
    key-rebind-gui-keyboard-layout-disabled-output
    key-rebind-gui-keyboard-layout-split-disabled-targets
    key-rebind-gui-keyboard-layout-mouse-source-bind-and-trigger
    key-rebind-gui-keyboard-layout-mouse-trigger-label-mapping
    key-rebind-gui-keyboard-layout-xbutton-trigger-label-mapping
    key-rebind-gui-keyboard-layout-scroll-trigger-label-mapping
    key-rebind-gui-keyboard-layout-scroll-source-popup-options
    key-rebind-gui-keyboard-layout-cursor-state-override
    key-rebind-gui-keyboard-layout-add-custom-bind-button
    key-rebind-gui-keyboard-layout-remove-custom-bind-button
    key-rebind-gui-keyboard-layout-add-built-in-custom-bind-button
    key-rebind-gui-keyboard-layout-change-custom-input-picker
    key-rebind-gui-keyboard-layout-change-custom-input-capture
    key-rebind-gui-keyboard-layout-full-bind-scan-picker-runtime
    key-rebind-gui-keyboard-layout-full-bind-scan-picker-cannot-type
    key-rebind-gui-keyboard-layout-scan-picker-filter
    key-rebind-gui-keyboard-layout-scan-picker-reset-to-default
    config-load-fullscreen-stretch-repaired
    config-load-fullscreen-manual-dimensions-preserved
    config-load-fullscreen-relative-dimensions-preserved
    fullscreen-relative-external-resize-skips-stale-resend
    fullscreen-relative-os-wmsize-overrides-computed-dimensions
    fullscreen-relative-display-dimensions-follow-window-resize
    fullscreen-relative-gui-publish-preserves-recalculated-size
    config-load-preemptive-sync-existing-mode
    config-load-thin-min-width-enforced
    config-load-browser-overlay-defaults
    config-load-window-overlay-defaults
    config-load-image-defaults
    config-load-hotkey-default-flags
    config-load-sensitivity-hotkey-default-flags
    config-load-image-color-key-default-sensitivity
    config-load-window-overlay-color-key-default-sensitivity
    config-load-browser-overlay-color-key-default-sensitivity
    mode-mirror-render-screen-anchors
    mode-mirror-render-viewport-anchors
    mode-mirror-render-screen-anchor-size-matrix
    mode-mirror-render-viewport-anchor-size-matrix
    mode-mirror-render-raw-output-dynamic-border-size
    mode-mirror-render-low-alpha-visibility
    mode-mirror-group-render
    mode-mirror-group-relative-position-resolution
    mode-mirror-group-slide-unit-transition
    mode-window-overlay-render
    mode-window-overlay-render-resets-blend-equation
    mode-window-overlay-render-unbinds-sampler
    mode-browser-overlay-render
    mode-image-overlay-render-png
    mode-image-overlay-render-mpeg
    mode-ninjabrain-overlay-render
    render-ninjabrain-information-message-translation-spans
    rebind-indicator-renders-below-settings-gui
    config-error-gui
    profiler-unspecified-breakdown
    settings-gui-basic
    settings-gui-advanced
    settings-key-repeat-system-repeat-hides-local-controls
    settings-mouse-translation-prefers-live-viewport
    mode-switch-invalidates-latest-viewport-size
    settings-search-subcategory-filtering
    settings-search-specific-options
    settings-search-reset-on-close
    settings-tab-general-default
    settings-tab-other-default
    settings-tab-supporters-default
    settings-tab-modes-default
    settings-tab-mirrors-default
    settings-tab-images-default
    settings-tab-window-overlays-default
    settings-tab-browser-overlays-default
    settings-tab-hotkeys-default
    settings-tab-inputs-mouse-default
    settings-tab-inputs-keyboard-default
    settings-tab-settings-default
    settings-tab-appearance-default
    settings-tab-misc-default
    settings-tab-general-populated
    settings-tab-other-populated
    settings-tab-modes-populated
    settings-tab-mirrors-populated
    settings-tab-images-populated
    settings-tab-window-overlays-populated
    settings-tab-browser-overlays-populated
    settings-tab-hotkeys-populated
    settings-tab-inputs-mouse-populated
    settings-tab-inputs-keyboard-populated
    settings-tab-settings-populated
    settings-tab-appearance-populated
    settings-tab-misc-populated
    settings-tab-supporters-populated
    profile-apply-fields-roundtrip
    profile-profiles-config-roundtrip
    profile-name-validation
    profile-create-duplicate-delete
    profile-migrate
    profile-delete-guards
    profile-rename
    profile-selective-switch-shared-fallback
    profile-case-insensitive-collisions
    profile-recover-missing-metadata
    profile-async-save-skip-deleted-profile
    profile-switch-ninjabrain-async-stop
    profile-switch-ninjabrain-async-restart
    profile-switch-invalid-default-mode-fallback
    profile-switch-reader-mode-fallback
    profile-switch-concurrent-readers
    profile-switch-concurrent-lifecycle
    profile-switch-concurrent-metadata-rebuild
    profile-switch-concurrent-snapshot-writes
)

set(TOOLSCREEN_GUI_INTEGRATION_CASE_TIMEOUT_SECONDS 120)
set(TOOLSCREEN_GUI_INTEGRATION_GROUP_TIMEOUT_SECONDS 300)

foreach(test_case IN LISTS TOOLSCREEN_GUI_INTEGRATION_TEST_CASES)
    string(REPLACE "-" "_" test_name_suffix "${test_case}")
    add_test(
        NAME toolscreen_integration_${test_name_suffix}
        COMMAND $<TARGET_FILE:toolscreen_gui_integration_tests> ${test_case}
    )
    set_tests_properties(
        toolscreen_integration_${test_name_suffix}
        PROPERTIES TIMEOUT ${TOOLSCREEN_GUI_INTEGRATION_CASE_TIMEOUT_SECONDS}
    )
endforeach()

set(TOOLSCREEN_GUI_INTEGRATION_TEST_GROUPS
    config
    rebind
    render
    settings-and-ui
    logs-and-profiles
)

foreach(test_group IN LISTS TOOLSCREEN_GUI_INTEGRATION_TEST_GROUPS)
    string(REPLACE "-" "_" test_group_suffix "${test_group}")
    add_test(
        NAME toolscreen_integration_group_${test_group_suffix}
        COMMAND $<TARGET_FILE:toolscreen_gui_integration_tests> --run-group ${test_group}
    )
    set_tests_properties(
        toolscreen_integration_group_${test_group_suffix}
        PROPERTIES
            LABELS "gui-integration;group;${test_group}"
            TIMEOUT ${TOOLSCREEN_GUI_INTEGRATION_GROUP_TIMEOUT_SECONDS}
    )
endforeach()

add_executable(toolscreen_interactive_create_tests
    tests/interactive_mirror_create_tests.cpp
    src/features/interactive_mirror_create.cpp
)

target_include_directories(toolscreen_interactive_create_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(toolscreen_interactive_create_tests PRIVATE
    NOMINMAX
    UNICODE
    _UNICODE
)

if(MSVC)
    target_compile_options(toolscreen_interactive_create_tests PRIVATE
        /W3
        /MP
        /EHsc
    )
endif()

toolscreen_configure_target_outputs(toolscreen_interactive_create_tests)
toolscreen_enable_release_symbols(toolscreen_interactive_create_tests)

set(TOOLSCREEN_INTERACTIVE_CREATE_TEST_CASES
    unscaled_viewport_maps_one_to_one
    scaled_viewport_divides_by_scale
    viewport_offset_subtracted_from_capture
    source_in_letterbox_gives_negative_input
    dest_rect_keeps_aspect_and_centers
    relative_to_screen_sets_fraction_and_anchor
    fixed_pixels_use_viewport_anchor
    tiny_source_clamped_to_min
    huge_dest_scale_clamped
)

foreach(test_case IN LISTS TOOLSCREEN_INTERACTIVE_CREATE_TEST_CASES)
    add_test(
        NAME toolscreen_interactive_create_${test_case}
        COMMAND $<TARGET_FILE:toolscreen_interactive_create_tests> --run ${test_case}
    )
endforeach()

add_executable(toolscreen_path_sanitize_tests
    tests/path_sanitize_tests.cpp
)

target_include_directories(toolscreen_path_sanitize_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(toolscreen_path_sanitize_tests PRIVATE
    NOMINMAX
    UNICODE
    _UNICODE
)

if(MSVC)
    target_compile_options(toolscreen_path_sanitize_tests PRIVATE
        /W3
        /MP
        /EHsc
    )
endif()

target_link_libraries(toolscreen_path_sanitize_tests PRIVATE
    shell32
    shlwapi
    ole32
)

toolscreen_configure_target_outputs(toolscreen_path_sanitize_tests)
toolscreen_enable_release_symbols(toolscreen_path_sanitize_tests)

set(TOOLSCREEN_PATH_SANITIZE_TEST_CASES
    home_prefix_collapses_to_tilde
    home_prefix_exact_match_collapses
    mid_string_users_occurrence_masked
    case_insensitive_on_drive_and_users
    non_home_path_untouched
    empty_string_passthrough
    home_prefix_does_not_partial_match_segment
    utf8_overload_uses_resolved_home
    filename_from_windows_path
    filename_from_forward_slash_path
    filename_no_separator_passthrough
    filename_empty_passthrough
)

foreach(test_case IN LISTS TOOLSCREEN_PATH_SANITIZE_TEST_CASES)
    add_test(
        NAME toolscreen_path_sanitize_${test_case}
        COMMAND $<TARGET_FILE:toolscreen_path_sanitize_tests> --run ${test_case}
    )
endforeach()

add_executable(toolscreen_background_fit_layout_tests
    tests/background_fit_layout_tests.cpp
    src/render/background_fit_layout.cpp
)

target_include_directories(toolscreen_background_fit_layout_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(toolscreen_background_fit_layout_tests PRIVATE
    NOMINMAX
    UNICODE
    _UNICODE
)

if(MSVC)
    target_compile_options(toolscreen_background_fit_layout_tests PRIVATE
        /W3
        /MP
        /EHsc
    )
endif()

toolscreen_configure_target_outputs(toolscreen_background_fit_layout_tests)
toolscreen_enable_release_symbols(toolscreen_background_fit_layout_tests)

set(TOOLSCREEN_BACKGROUND_FIT_TEST_CASES
    parse_normalizes_case_and_unknown
    to_string_round_trips
    backdrop_only_for_visible_modes
    stretch_fills_whole_area
    fit_letterboxes_and_centers
    fill_covers_and_crops
    center_uses_native_size_by_default
    center_scale_adjusts_size
    degenerate_dims_fall_back_to_area
    tile_grid_covers_area_centered
    tile_spacing_reduces_count
    tile_degenerate_and_capped
)

foreach(test_case IN LISTS TOOLSCREEN_BACKGROUND_FIT_TEST_CASES)
    add_test(
        NAME toolscreen_background_fit_${test_case}
        COMMAND $<TARGET_FILE:toolscreen_background_fit_layout_tests> --run ${test_case}
    )
endforeach()

add_executable(toolscreen_game_state_source_tests
    tests/game_state_source_tests.cpp
    src/features/game_state_source.cpp
)

target_include_directories(toolscreen_game_state_source_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${nlohmann_json_SOURCE_DIR}/single_include
)

target_compile_definitions(toolscreen_game_state_source_tests PRIVATE
    NOMINMAX
    UNICODE
    _UNICODE
)

if(MSVC)
    target_compile_options(toolscreen_game_state_source_tests PRIVATE
        /W3
        /MP
        /EHsc
    )
endif()

toolscreen_configure_target_outputs(toolscreen_game_state_source_tests)
toolscreen_enable_release_symbols(toolscreen_game_state_source_tests)

set(TOOLSCREEN_GAME_STATE_SOURCE_TEST_CASES
    stateoutput_normalizes_unpaused
    stateoutput_normalizes_paused_and_gamescreen
    stateoutput_generating_strips_suffix
    stateoutput_passes_through_canonical
    stateoutput_trims_whitespace
    stateoutput_unknown_returns_nullopt
    hermes_inworld_grabbed_when_no_screen
    hermes_inworld_free_when_screen
    hermes_title_when_no_world
    hermes_loading_screen_generating
    hermes_seedqueue_wall
    hermes_invalid_json_nullopt
    select_prefers_hermes
    select_falls_back_to_stateoutput
    select_none
    alive_fresh_same_pid_true
    alive_wrong_pid_false
    alive_stale_heartbeat_false
    alive_future_skew_within_threshold_true
)

foreach(test_case IN LISTS TOOLSCREEN_GAME_STATE_SOURCE_TEST_CASES)
    add_test(
        NAME toolscreen_game_state_source_${test_case}
        COMMAND $<TARGET_FILE:toolscreen_game_state_source_tests> --run ${test_case}
    )
endforeach()

