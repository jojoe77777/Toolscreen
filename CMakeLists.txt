cmake_minimum_required(VERSION 3.15)

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

find_package(minhook CONFIG REQUIRED)
find_package(GLEW REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
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
    minhook::minhook
    GLEW::GLEW
    imgui::imgui
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
    install(FILES "${JAR_OUTPUT}" DESTINATION "${CMAKE_SOURCE_DIR}/..")
    
    message(STATUS "JAR packaging enabled: ${JAR_NAME}")
else()
    message(WARNING "7-Zip not found - JAR packaging disabled. Install from https://www.7-zip.org/ and ensure it's in your PATH.")
endif()
