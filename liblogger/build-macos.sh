#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_FILE="${SCRIPT_DIR}/Mac.cpp"
OUTPUT_DIR="${SCRIPT_DIR}/dist/macos"
FINAL_OUTPUT_NAME="${MACOS_OUTPUT_NAME:-liblogger.dylib}"
FINAL_OUTPUT_FILE="${OUTPUT_DIR}/${FINAL_OUTPUT_NAME}"
FINAL_INSTALL_NAME="${MACOS_INSTALL_NAME:-@rpath/${FINAL_OUTPUT_NAME}}"
MACOS_TARGETS="${MACOS_TARGETS:-x64 arm64}"
MACOS_TARGETS="${MACOS_TARGETS//,/ }"
MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-11.0}"
BUILD_DIR=""

detect_liblogger_version() {
  local version_file="${SCRIPT_DIR}/LibLoggerVersion.cmake"
  local major
  local minor
  local patch

  if [[ ! -f "${version_file}" ]]; then
    echo "1.0.2"
    return 0
  fi

  major="$(sed -nE 's/^set\(LIBLOGGER_VERSION_MAJOR[[:space:]]+([0-9]+)\)$/\1/p' "${version_file}" | head -n 1)"
  minor="$(sed -nE 's/^set\(LIBLOGGER_VERSION_MINOR[[:space:]]+([0-9]+)\)$/\1/p' "${version_file}" | head -n 1)"
  patch="$(sed -nE 's/^set\(LIBLOGGER_VERSION_PATCH[[:space:]]+([0-9]+)\)$/\1/p' "${version_file}" | head -n 1)"

  if [[ -n "${major}" && -n "${minor}" && -n "${patch}" ]]; then
    echo "${major}.${minor}.${patch}"
  else
    echo "1.0.2"
  fi
}

command_exists() {
  command -v "$1" >/dev/null 2>&1
}

detect_java_home() {
  if [[ -n "${JAVA_HOME:-}" && -f "${JAVA_HOME}/include/jni.h" ]]; then
    echo "${JAVA_HOME}"
    return 0
  fi

  if [[ -x /usr/libexec/java_home ]]; then
    local detected_java_home
    detected_java_home="$(/usr/libexec/java_home 2>/dev/null || true)"
    if [[ -n "${detected_java_home}" && -f "${detected_java_home}/include/jni.h" ]]; then
      echo "${detected_java_home}"
      return 0
    fi
  fi

  return 1
}

require_file() {
  local path="$1"
  local description="$2"

  if [[ ! -f "${path}" ]]; then
    echo "Missing ${description}: ${path}" >&2
    exit 1
  fi
}

require_command() {
  local tool="$1"
  local description="$2"

  if command_exists "${tool}"; then
    return 0
  fi

  if [[ -x "${tool}" ]]; then
    return 0
  fi

  echo "Missing ${description}: ${tool}" >&2
  exit 1
}

validate_architecture() {
  local output_file="$1"
  shift
  local actual_arches
  local expected_arch

  actual_arches="$(lipo -archs "${output_file}")"
  for expected_arch in "$@"; do
    if [[ " ${actual_arches} " != *" ${expected_arch} "* ]]; then
      echo "Unexpected architectures in ${output_file}: ${actual_arches}" >&2
      exit 1
    fi
  done
}

validate_runtime_deps() {
  local output_file="$1"
  local dep

  while IFS= read -r dep; do
    [[ -n "${dep}" ]] || continue
    if [[ "${dep}" == "${FINAL_INSTALL_NAME}" ]]; then
      continue
    fi
    case "${dep}" in
      /usr/lib/*|/System/Library/*)
        ;;
      *)
        echo "Unsupported dynamic dependency in ${output_file}: ${dep}" >&2
        echo "macOS dylibs cannot statically link Apple system frameworks or libSystem with the stock Xcode toolchain." >&2
        echo "This build only permits system-provided runtime dependencies; all non-system dependencies must be statically linked or removed." >&2
        exit 1
        ;;
    esac
  done < <("${OTOOL_BIN}" -L "${output_file}" | sed -En 's/^[[:space:]]+([^[:space:]]+).*/\1/p')
}

cleanup() {
  if [[ -n "${BUILD_DIR}" && -d "${BUILD_DIR}" ]]; then
    rm -rf "${BUILD_DIR}"
  fi
}

build_target() {
  local target_name="$1"
  local clang_arch="$2"
  local output_file="${BUILD_DIR}/liblogger_${target_name}.dylib"

  echo "Building ${target_name}"

  "${CLANG_BIN}" \
    -std=c++17 \
    -O3 \
    -fno-exceptions \
    -fno-rtti \
    -fvisibility=hidden \
    -ffunction-sections \
    -fdata-sections \
    "-DLIBLOGGER_VERSION_STR=\"${LIBLOGGER_VERSION}\"" \
    -dynamiclib \
    -fPIC \
    -Wno-deprecated-declarations \
    -arch "${clang_arch}" \
    -isysroot "${SDK_PATH}" \
    -mmacosx-version-min="${MACOSX_DEPLOYMENT_TARGET}" \
    -Wl,-dead_strip \
    -Wl,-install_name,"${FINAL_INSTALL_NAME}" \
    -o "${output_file}" \
    "${SOURCE_FILE}" \
    -I"${JAVA_HOME_DETECTED}/include" \
    -I"${JAVA_HOME_DETECTED}/include/darwin" \
    -framework CoreFoundation \
    -framework Security

  "${STRIP_BIN}" -x "${output_file}"
  validate_runtime_deps "${output_file}"
  validate_architecture "${output_file}" "${clang_arch}"
}

create_universal_output() {
  local -a built_outputs=("$@")

  if [[ ${#built_outputs[@]} -eq 0 ]]; then
    echo "No macOS targets were built" >&2
    exit 1
  fi

  if [[ ${#built_outputs[@]} -eq 1 ]]; then
    mv "${built_outputs[0]}" "${FINAL_OUTPUT_FILE}"
  else
    lipo -create "${built_outputs[@]}" -output "${FINAL_OUTPUT_FILE}"
  fi

  validate_runtime_deps "${FINAL_OUTPUT_FILE}"
}

JAVA_HOME_DETECTED="$(detect_java_home || true)"
CLANG_BIN="$(xcrun --sdk macosx --find clang++)"
SDK_PATH="$(xcrun --sdk macosx --show-sdk-path)"
STRIP_BIN="$(xcrun --sdk macosx --find strip)"
OTOOL_BIN="$(xcrun --sdk macosx --find otool)"
LIBLOGGER_VERSION="${LIBLOGGER_VERSION:-$(detect_liblogger_version)}"

require_command "${CLANG_BIN}" "clang++"
require_command "${STRIP_BIN}" "strip"
require_command "${OTOOL_BIN}" "otool"
require_command "lipo" "lipo"
require_file "${SOURCE_FILE}" "macOS source file"
require_file "${JAVA_HOME_DETECTED}/include/jni.h" "JDK headers"
require_file "${JAVA_HOME_DETECTED}/include/darwin/jni_md.h" "macOS JDK headers"

mkdir -p "${OUTPUT_DIR}"
rm -f "${OUTPUT_DIR}"/*.dylib
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/liblogger-macos.XXXXXX")"
trap cleanup EXIT

built_outputs=()

for target_name in ${MACOS_TARGETS}; do
  case "${target_name}" in
    x64)
      build_target "x64" "x86_64"
      built_outputs+=("${BUILD_DIR}/liblogger_x64.dylib")
      ;;
    arm64)
      build_target "arm64" "arm64"
      built_outputs+=("${BUILD_DIR}/liblogger_arm64.dylib")
      ;;
    *)
      echo "Unsupported MACOS_TARGETS entry: ${target_name}" >&2
      exit 1
      ;;
  esac
done

create_universal_output "${built_outputs[@]}"

if [[ " ${MACOS_TARGETS} " == *" x64 "* && " ${MACOS_TARGETS} " == *" arm64 "* ]]; then
  validate_architecture "${FINAL_OUTPUT_FILE}" "x86_64" "arm64"
else
  if [[ " ${MACOS_TARGETS} " == *" x64 "* ]]; then
    validate_architecture "${FINAL_OUTPUT_FILE}" "x86_64"
  fi
  if [[ " ${MACOS_TARGETS} " == *" arm64 "* ]]; then
    validate_architecture "${FINAL_OUTPUT_FILE}" "arm64"
  fi
fi

echo "Built macOS LibLogger binary: ${FINAL_OUTPUT_FILE}"