cmake_minimum_required(VERSION 3.21)

if(POLICY CMP0091)
  cmake_policy(SET CMP0091 NEW)
endif()

include(FetchContent)

set(LIBLOGGER_DEFAULT_VERSION_MAJOR 1)
set(LIBLOGGER_DEFAULT_VERSION_MINOR 1)
set(LIBLOGGER_DEFAULT_VERSION_PATCH 0)

if(NOT (DEFINED LIBLOGGER_VERSION_MAJOR AND DEFINED LIBLOGGER_VERSION_MINOR AND DEFINED LIBLOGGER_VERSION_PATCH))
  set(LIBLOGGER_VERSION_FILE "${CMAKE_CURRENT_LIST_DIR}/LibLoggerVersion.cmake")
  if(EXISTS "${LIBLOGGER_VERSION_FILE}")
    include("${LIBLOGGER_VERSION_FILE}")
  endif()
endif()

if(NOT DEFINED LIBLOGGER_VERSION_MAJOR)
  set(LIBLOGGER_VERSION_MAJOR "${LIBLOGGER_DEFAULT_VERSION_MAJOR}")
  set(LIBLOGGER_VERSION_MINOR "${LIBLOGGER_DEFAULT_VERSION_MINOR}")
  set(LIBLOGGER_VERSION_PATCH "${LIBLOGGER_DEFAULT_VERSION_PATCH}")
endif()

set(LIBLOGGER_VERSION_STRING "${LIBLOGGER_VERSION_MAJOR}.${LIBLOGGER_VERSION_MINOR}.${LIBLOGGER_VERSION_PATCH}")

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(LibLogger VERSION ${LIBLOGGER_VERSION_STRING} LANGUAGES C CXX RC)
endif()

if(NOT WIN32)
  message(FATAL_ERROR "This CMake project only supports Windows builds. Use build.sh for Linux or build-macos.sh for macOS.")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(LIBLOGGER_PLATFORM_NAME "${CMAKE_VS_PLATFORM_NAME}")
if(NOT LIBLOGGER_PLATFORM_NAME)
  set(LIBLOGGER_PLATFORM_NAME "${CMAKE_SYSTEM_PROCESSOR}")
endif()

set(LIBLOGGER_OUTPUT_ARCH "x64")
if(LIBLOGGER_PLATFORM_NAME STREQUAL "Win32" OR LIBLOGGER_PLATFORM_NAME STREQUAL "x86")
  set(LIBLOGGER_OUTPUT_ARCH "x86")
elseif(LIBLOGGER_PLATFORM_NAME STREQUAL "ARM64" OR LIBLOGGER_PLATFORM_NAME STREQUAL "arm64")
  set(LIBLOGGER_OUTPUT_ARCH "arm64")
elseif(LIBLOGGER_PLATFORM_NAME STREQUAL "x64" OR LIBLOGGER_PLATFORM_NAME STREQUAL "AMD64" OR LIBLOGGER_PLATFORM_NAME STREQUAL "x86_64")
  set(LIBLOGGER_OUTPUT_ARCH "x64")
else()
  string(TOLOWER "${LIBLOGGER_PLATFORM_NAME}" LIBLOGGER_OUTPUT_ARCH)
endif()

set(LIBLOGGER_OUTPUT_BASENAME "liblogger_${LIBLOGGER_OUTPUT_ARCH}")
set(LIBLOGGER_COMPANY_NAME "jojoe77777")
set(LIBLOGGER_PRODUCT_NAME "LibLogger")
set(LIBLOGGER_COPYRIGHT "Copyright (c) 2026 jojoe77777")
set(LIBLOGGER_FILE_DESCRIPTION "LibLogger")

set(LIBLOGGER_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(LIBLOGGER_GENERATED_RESOURCE_FILE "${LIBLOGGER_GENERATED_DIR}/liblogger.rc")
file(MAKE_DIRECTORY "${LIBLOGGER_GENERATED_DIR}")
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/liblogger.rc.in"
  "${LIBLOGGER_GENERATED_RESOURCE_FILE}"
  @ONLY
)

set(LIBLOGGER_LOCAL_JDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include/jdk")

if(EXISTS "${LIBLOGGER_LOCAL_JDK_DIR}/jni.h" AND EXISTS "${LIBLOGGER_LOCAL_JDK_DIR}/win32/jni_md.h")
  set(LIBLOGGER_JNI_INCLUDE_DIRS
    "${LIBLOGGER_LOCAL_JDK_DIR}"
    "${LIBLOGGER_LOCAL_JDK_DIR}/win32"
  )
else()
  find_package(JNI REQUIRED)
  set(LIBLOGGER_JNI_INCLUDE_DIRS ${JNI_INCLUDE_DIRS})
endif()

set(LIBLOGGER_MINHOOK_TARGET "")
set(LIBLOGGER_LOCAL_MINHOOK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include/minhook-detours")

if(TARGET Toolscreen::minhook)
  set(LIBLOGGER_MINHOOK_TARGET Toolscreen::minhook)
elseif(TARGET toolscreen_minhook)
  set(LIBLOGGER_MINHOOK_TARGET toolscreen_minhook)
else()
  if(EXISTS "${LIBLOGGER_LOCAL_MINHOOK_DIR}/MinHook.c")
    set(LIBLOGGER_MINHOOK_SOURCE_DIR "${LIBLOGGER_LOCAL_MINHOOK_DIR}")
  else()
    FetchContent_Declare(
      minhook
      GIT_REPOSITORY https://github.com/m417z/minhook-detours
      GIT_TAG v1.0.6
      GIT_SHALLOW TRUE
    )

    FetchContent_GetProperties(minhook)
    if(NOT minhook_POPULATED)
      FetchContent_Populate(minhook)
    endif()

    set(LIBLOGGER_MINHOOK_SOURCE_DIR "${minhook_SOURCE_DIR}/src")
  endif()

  add_library(liblogger_minhook STATIC
    "${LIBLOGGER_MINHOOK_SOURCE_DIR}/MinHook.c"
    "${LIBLOGGER_MINHOOK_SOURCE_DIR}/SlimDetours/Disassembler.c"
    "${LIBLOGGER_MINHOOK_SOURCE_DIR}/SlimDetours/InlineHook.c"
    "${LIBLOGGER_MINHOOK_SOURCE_DIR}/SlimDetours/Instruction.c"
    "${LIBLOGGER_MINHOOK_SOURCE_DIR}/SlimDetours/Memory.c"
    "${LIBLOGGER_MINHOOK_SOURCE_DIR}/SlimDetours/Thread.c"
    "${LIBLOGGER_MINHOOK_SOURCE_DIR}/SlimDetours/Trampoline.c"
    "${LIBLOGGER_MINHOOK_SOURCE_DIR}/SlimDetours/Transaction.c"
  )

  target_include_directories(liblogger_minhook PUBLIC
    "${LIBLOGGER_MINHOOK_SOURCE_DIR}"
    "${LIBLOGGER_MINHOOK_SOURCE_DIR}/phnt"
  )

  set_target_properties(liblogger_minhook PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
  )

  set(LIBLOGGER_MINHOOK_TARGET liblogger_minhook)
endif()

add_library(liblogger_windows SHARED
  Windows.cpp
  "${LIBLOGGER_GENERATED_RESOURCE_FILE}"
)

add_library(LibLogger::windows ALIAS liblogger_windows)

target_compile_features(liblogger_windows PRIVATE cxx_std_17)

target_include_directories(liblogger_windows PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  ${LIBLOGGER_JNI_INCLUDE_DIRS}
)

target_compile_definitions(liblogger_windows PRIVATE
  LIBLOGGER_VERSION_STR=\"${LIBLOGGER_VERSION_STRING}\"
  UNICODE
  _UNICODE
  MAGNIFIERDLL_EXPORTS
  MH_STATIC
  _WINDOWS
  _USRDLL
  _WINDLL
  $<$<CONFIG:Debug>:_DEBUG>
  $<$<CONFIG:Release>:NDEBUG>
)

target_compile_options(liblogger_windows PRIVATE
  /GS
  /Gy
  /MP
  /W3
  /fp:precise
  $<$<COMPILE_LANGUAGE:CXX>:/EHsc>
  $<$<CONFIG:Release>:/GF>
  $<$<CONFIG:Release>:/Oi>
  $<$<CONFIG:Release>:/Ot>
  $<$<CONFIG:Release>:/Oy>
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_compile_options(liblogger_windows PRIVATE
    -Wno-microsoft-static-assert
    -Wno-gnu-folding-constant
    -Wno-nonportable-include-path
    -Wno-microsoft-cast
    -Wno-microsoft-enum-forward-reference
    -Wno-pragma-pack
    -Wno-visibility
    -Wno-sizeof-array-div
    -Wno-switch
  )
endif()

target_link_libraries(liblogger_windows PRIVATE
  ${LIBLOGGER_MINHOOK_TARGET}
  bcrypt
  Crypt32
  ntdll
  user32
  wintrust
)

target_link_options(liblogger_windows PRIVATE
  /NOIMPLIB
  /SUBSYSTEM:WINDOWS
  $<$<CONFIG:Release>:/OPT:REF>
  $<$<CONFIG:Release>:/OPT:ICF>
)

set_target_properties(liblogger_windows PROPERTIES
  OUTPUT_NAME "${LIBLOGGER_OUTPUT_BASENAME}"
  PREFIX ""
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
  INTERPROCEDURAL_OPTIMIZATION_RELEASE ON
)

if(COMMAND toolscreen_configure_target_outputs)
  toolscreen_configure_target_outputs(liblogger_windows)
else()
  set_target_properties(liblogger_windows PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/dist/windows"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/dist/windows"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/dist/windows"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/dist/windows"
    PDB_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/pdb"
  )
endif()

if(COMMAND toolscreen_enable_release_symbols)
  toolscreen_enable_release_symbols(liblogger_windows)
elseif(MSVC)
  target_compile_options(liblogger_windows PRIVATE
    $<$<CONFIG:Release>:/Zi>
    $<$<CONFIG:MinSizeRel>:/Zi>
  )
  target_link_options(liblogger_windows PRIVATE
    $<$<CONFIG:Release>:/DEBUG:FULL>
    $<$<CONFIG:MinSizeRel>:/DEBUG:FULL>
  )
endif()
