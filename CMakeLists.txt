cmake_minimum_required(VERSION 3.15)

# Set policies to avoid warnings
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)  # Use extraction timestamp for URL downloads
endif()
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)  # Allow FetchContent_Populate for custom builds
endif()
if(POLICY CMP0177)
    cmake_policy(SET CMP0177 NEW)  # Normalize install paths
endif()

# Read version from version.h
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/version.h" VERSION_FILE)
string(REGEX REPLACE ".*TOOLSCREEN_VERSION_MAJOR[ \t]+([0-9]+).*" "\\1" TOOLSCREEN_VERSION_MAJOR "${VERSION_FILE}")
string(REGEX REPLACE ".*TOOLSCREEN_VERSION_MINOR[ \t]+([0-9]+).*" "\\1" TOOLSCREEN_VERSION_MINOR "${VERSION_FILE}")
string(REGEX REPLACE ".*TOOLSCREEN_VERSION_PATCH[ \t]+([0-9]+).*" "\\1" TOOLSCREEN_VERSION_PATCH "${VERSION_FILE}")
set(TOOLSCREEN_VERSION ${TOOLSCREEN_VERSION_MAJOR}.${TOOLSCREEN_VERSION_MINOR}.${TOOLSCREEN_VERSION_PATCH})

project(Toolscreen VERSION ${TOOLSCREEN_VERSION})

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)

FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG master
)
FetchContent_Declare(
    glew
    URL https://github.com/nigels-com/glew/releases/download/glew-2.2.0/glew-2.2.0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.9
)
FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG v3.4.0
)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)

FetchContent_MakeAvailable(minhook tomlplusplus nlohmann_json)
FetchContent_GetProperties(glew)
if(NOT glew_POPULATED)
    FetchContent_Populate(glew)
    add_library(glew_static STATIC
        ${glew_SOURCE_DIR}/src/glew.c
    )
    target_include_directories(glew_static PUBLIC ${glew_SOURCE_DIR}/include)
    target_compile_definitions(glew_static PUBLIC GLEW_STATIC)
    set_target_properties(glew_static PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    add_library(GLEW::GLEW ALIAS glew_static)
endif()
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
    add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    )
    target_include_directories(imgui PUBLIC 
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
        ${imgui_SOURCE_DIR}/misc/cpp
    )
    target_compile_definitions(imgui PUBLIC IMGUI_IMPL_OPENGL_LOADER_GLEW)
    set_target_properties(imgui PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()

find_package(OpenGL REQUIRED)

# Note: GLOB is convenient but CMake won't auto-detect new files.
# Re-run cmake when adding new source files.
file(GLOB_RECURSE SOURCES "src/*.cpp")
file(GLOB_RECURSE HEADERS "src/*.h" "src/*.inl")

# Create the DLL
add_library(${PROJECT_NAME} SHARED
    ${SOURCES}
    ${HEADERS}
    src/Toolscreen.rc
)

# Output directories
set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

# Include directories
target_include_directories(${PROJECT_NAME} PRIVATE src)

# Link libraries
target_link_libraries(${PROJECT_NAME} PRIVATE
    minhook
    GLEW::GLEW
    imgui
    tomlplusplus::tomlplusplus
    nlohmann_json::nlohmann_json
    OpenGL::GL
)

# Compiler definitions
target_compile_definitions(${PROJECT_NAME} PRIVATE
    TOOLSCREEN_EXPORTS
    GLEW_STATIC
    UNICODE
)

# MSVC-specific settings
if(MSVC)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        comdlg32 msimg32 dbghelp psapi shlwapi
    )

    target_compile_options(${PROJECT_NAME} PRIVATE
        /W3          # Warning level 3
        /MP          # Multi-processor compilation
        /EHa         
    )
endif()



# JAR packaging (requires 7-Zip)
set(JAR_NAME "Toolscreen-${PROJECT_VERSION}-double-click-me.jar")
set(INSTALLER_JAR "${CMAKE_CURRENT_SOURCE_DIR}/installer.jar")
set(JAR_OUTPUT "${CMAKE_BINARY_DIR}/${JAR_NAME}")
set(JAR_TEMP_DIR "${CMAKE_BINARY_DIR}/jar_temp")
set(BRANDING_CONFIGURED "${CMAKE_BINARY_DIR}/branding.properties")

# Generate configured branding.properties at configure time
file(GENERATE OUTPUT "${BRANDING_CONFIGURED}" CONTENT "\
# Branding Configuration for EasyInjectBundled

# Edit these values to customize the output JAR name and branding.
# After editing, run build.bat to create a new JAR with your branding.

# The name of your application (used in JAR filename)
brand.name=Toolscreen

# Version number (automatically set from version.h)
brand.version=${PROJECT_VERSION}
")

find_program(SEVENZIP_EXECUTABLE 
    NAMES 7z 7za
    PATHS 
        "C:/Program Files/7-Zip"
        "C:/Program Files (x86)/7-Zip"
        "$ENV{ProgramFiles}/7-Zip"
        "$ENV{ProgramFiles\(x86\)}/7-Zip"
)

if(SEVENZIP_EXECUTABLE)
    add_custom_command(
        OUTPUT "${JAR_OUTPUT}"
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${JAR_TEMP_DIR}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${JAR_TEMP_DIR}"
        COMMAND ${SEVENZIP_EXECUTABLE} x "-o${JAR_TEMP_DIR}" "${INSTALLER_JAR}" -y
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${PROJECT_NAME}>" "${JAR_TEMP_DIR}/dlls/Toolscreen.dll"
        COMMAND ${CMAKE_COMMAND} -E copy "${BRANDING_CONFIGURED}" "${JAR_TEMP_DIR}/branding.properties"
        COMMAND ${CMAKE_COMMAND} -E chdir "${JAR_TEMP_DIR}" ${SEVENZIP_EXECUTABLE} a -tzip "${JAR_OUTPUT}" .
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${JAR_TEMP_DIR}"
        DEPENDS ${PROJECT_NAME} "${INSTALLER_JAR}" "${BRANDING_CONFIGURED}"
        COMMENT "Packaging ${JAR_NAME}"
        VERBATIM
    )

    add_custom_target(jar ALL DEPENDS "${JAR_OUTPUT}")
    install(FILES "${JAR_OUTPUT}" DESTINATION "${CMAKE_CURRENT_SOURCE_DIR}/..")
    
    message(STATUS "JAR packaging enabled: ${JAR_NAME}")
else()
    message(WARNING "7-Zip not found - JAR packaging disabled. Install from https://www.7-zip.org/ and ensure it's in your PATH.")
endif()
