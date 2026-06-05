#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_FILE="${SCRIPT_DIR}/Linux.cpp"
OUTPUT_DIR="${SCRIPT_DIR}/dist/linux"
PKG_CONFIG_BIN="${PKG_CONFIG_BIN:-pkg-config}"
READELF_BIN="${READELF_BIN:-readelf}"

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

detect_cpp_compiler() {
  local candidate

  for candidate in clang++ g++ c++; do
    if command_exists "${candidate}"; then
      echo "${candidate}"
      return 0
    fi
  done

  return 1
}

detect_cpp_compiler_for_target() {
  local target_name="$1"

  case "${target_name}" in
    x64)
      detect_cpp_compiler || true
      ;;
    x86)
      if command_exists i686-linux-gnu-g++; then
        echo "i686-linux-gnu-g++"
      else
        detect_cpp_compiler || true
      fi
      ;;
    arm64)
      if command_exists aarch64-linux-gnu-g++; then
        echo "aarch64-linux-gnu-g++"
      else
        detect_cpp_compiler || true
      fi
      ;;
    arm32)
      if command_exists arm-linux-gnueabihf-g++; then
        echo "arm-linux-gnueabihf-g++"
      else
        detect_cpp_compiler || true
      fi
      ;;
    *)
      return 1
      ;;
  esac
}

detect_default_libcrypto_archive() {
  local target_name="$1"

  case "${target_name}" in
    x64)
      [[ -f /usr/lib/x86_64-linux-gnu/libcrypto.a ]] && echo "/usr/lib/x86_64-linux-gnu/libcrypto.a"
      ;;
    x86)
      [[ -f /usr/lib/i386-linux-gnu/libcrypto.a ]] && echo "/usr/lib/i386-linux-gnu/libcrypto.a"
      ;;
    arm64)
      [[ -f /usr/lib/aarch64-linux-gnu/libcrypto.a ]] && echo "/usr/lib/aarch64-linux-gnu/libcrypto.a"
      ;;
    arm32)
      [[ -f /usr/lib/arm-linux-gnueabihf/libcrypto.a ]] && echo "/usr/lib/arm-linux-gnueabihf/libcrypto.a"
      ;;
  esac
}

detect_default_binutils_dir() {
  local target_name="$1"

  case "${target_name}" in
    x86)
      [[ -d /usr/i686-linux-gnu/bin ]] && echo "/usr/i686-linux-gnu/bin"
      ;;
    arm64)
      [[ -d /usr/aarch64-linux-gnu/bin ]] && echo "/usr/aarch64-linux-gnu/bin"
      ;;
    arm32)
      [[ -d /usr/arm-linux-gnueabihf/bin ]] && echo "/usr/arm-linux-gnueabihf/bin"
      ;;
  esac
}

compiler_supports_target_flag() {
  local compiler_name
  compiler_name="$(basename "$1")"

  case "${compiler_name}" in
    clang++*)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

detect_linker_flags() {
  local compiler_name
  compiler_name="$(basename "$1")"

  if ! command_exists ld.lld; then
    return 0
  fi

  case "${compiler_name}" in
    *-linux-gnu-g++|*-linux-gnueabihf-g++)
      return 0
      ;;
    *)
      echo "-fuse-ld=lld"
      ;;
  esac
}

DEFAULT_CPP_COMPILER="$(detect_cpp_compiler || true)"
CLANG_BIN="${CLANG_BIN:-${DEFAULT_CPP_COMPILER}}"
LIBLOGGER_VERSION="${LIBLOGGER_VERSION:-$(detect_liblogger_version)}"

detect_host_target() {
  case "$(uname -m)" in
    x86_64|amd64)
      echo "x64"
      ;;
    i386|i686)
      echo "x86"
      ;;
    aarch64|arm64)
      echo "arm64"
      ;;
    armv7l|armv7hl|armhf)
      echo "arm32"
      ;;
    *)
      echo "Unsupported host architecture: $(uname -m)" >&2
      exit 1
      ;;
  esac
}

detect_java_home() {
  if [[ -n "${JAVA_HOME:-}" && -f "${JAVA_HOME}/include/jni.h" ]]; then
    echo "${JAVA_HOME}"
    return 0
  fi

  if command_exists javac; then
    local javac_path
    local candidate_home

    javac_path="$(readlink -f "$(command -v javac)")"
    candidate_home="$(cd "$(dirname "${javac_path}")/.." && pwd)"
    if [[ -f "${candidate_home}/include/jni.h" ]]; then
      echo "${candidate_home}"
      return 0
    fi
  fi

  return 1
}

detect_openssl_cflags() {
  if [[ -n "${OPENSSL_CFLAGS:-}" ]]; then
    echo "${OPENSSL_CFLAGS}"
    return 0
  fi

  if command_exists "${PKG_CONFIG_BIN}" && "${PKG_CONFIG_BIN}" --exists openssl; then
    "${PKG_CONFIG_BIN}" --cflags openssl
    return 0
  fi

  return 1
}

detect_libcrypto_archive() {
  local explicit_path="$1"
  if [[ -n "${explicit_path}" ]]; then
    echo "${explicit_path}"
    return 0
  fi

  if command_exists "${PKG_CONFIG_BIN}" && "${PKG_CONFIG_BIN}" --exists openssl; then
    local libdir
    libdir="$(${PKG_CONFIG_BIN} --variable=libdir openssl 2>/dev/null || true)"
    if [[ -n "${libdir}" && -f "${libdir}/libcrypto.a" ]]; then
      echo "${libdir}/libcrypto.a"
      return 0
    fi
  fi

  return 1
}

append_flags() {
  local raw_flags="$1"
  local -n output_ref="$2"

  if [[ -n "${raw_flags}" ]]; then
    read -r -a parsed_flags <<< "${raw_flags}"
    output_ref+=("${parsed_flags[@]}")
  fi
}

HOST_TARGET="${HOST_TARGET:-$(detect_host_target)}"
LINUX_TARGETS="${LINUX_TARGETS:-x64 x86 arm64 arm32}"
LINUX_TARGETS="${LINUX_TARGETS//,/ }"

DEFAULT_JDK_HOME="$(detect_java_home || true)"
DEFAULT_LIBCRYPTO_ARCHIVE="$(detect_libcrypto_archive "" || true)"
OPENSSL_CFLAGS_RAW="$(detect_openssl_cflags || true)"

JDK_X64="${JDK_X64:-${DEFAULT_JDK_HOME}}"
JDK_X86="${JDK_X86:-${DEFAULT_JDK_HOME}}"
JDK_ARM64="${JDK_ARM64:-${DEFAULT_JDK_HOME}}"
JDK_ARM32="${JDK_ARM32:-${DEFAULT_JDK_HOME}}"

CXX_X64="${CXX_X64:-${CLANG_BIN:-$(detect_cpp_compiler_for_target x64 || true)}}"
CXX_X86="${CXX_X86:-$(detect_cpp_compiler_for_target x86 || true)}"
CXX_ARM64="${CXX_ARM64:-$(detect_cpp_compiler_for_target arm64 || true)}"
CXX_ARM32="${CXX_ARM32:-$(detect_cpp_compiler_for_target arm32 || true)}"

LIBCRYPTO_X64="${LIBCRYPTO_X64:-$(detect_default_libcrypto_archive x64 || true)}"
LIBCRYPTO_X86="${LIBCRYPTO_X86:-$(detect_default_libcrypto_archive x86 || true)}"
LIBCRYPTO_ARM64="${LIBCRYPTO_ARM64:-$(detect_default_libcrypto_archive arm64 || true)}"
LIBCRYPTO_ARM32="${LIBCRYPTO_ARM32:-$(detect_default_libcrypto_archive arm32 || true)}"

if [[ -z "${LIBCRYPTO_X64}" ]]; then
  LIBCRYPTO_X64="${DEFAULT_LIBCRYPTO_ARCHIVE}"
fi

STRIP_X64="${STRIP_X64:-strip}"
STRIP_X86="${STRIP_X86:-strip}"
if [[ "${HOST_TARGET}" == "arm64" ]]; then
  STRIP_ARM64="${STRIP_ARM64:-strip}"
else
  STRIP_ARM64="${STRIP_ARM64:-aarch64-linux-gnu-strip}"
fi
if [[ "${HOST_TARGET}" == "arm32" ]]; then
  STRIP_ARM32="${STRIP_ARM32:-strip}"
else
  STRIP_ARM32="${STRIP_ARM32:-arm-linux-gnueabihf-strip}"
fi

COMMON_FLAGS=(
  -std=c++17
  -O3
  -flto
  -fno-exceptions
  -fno-rtti
  -fvisibility=hidden
  -ffunction-sections
  -fdata-sections
  "-DLIBLOGGER_VERSION_STR=\"${LIBLOGGER_VERSION}\""
  -Wl,--gc-sections
  -Wl,--as-needed
  -static-libgcc
  -static-libstdc++
  -shared
  -fPIC
  -Wno-deprecated-declarations
)

DYNAMIC_LINK_FLAGS=(
  -Wl,-Bdynamic
  -ldl
  -pthread
  -lrt
)

OPENSSL_CFLAGS=()
append_flags "${OPENSSL_CFLAGS_RAW}" OPENSSL_CFLAGS

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

validate_runtime_deps() {
  local output_file="$1"

  if ! command_exists "${READELF_BIN}"; then
    echo "Skipping runtime dependency validation because ${READELF_BIN} is unavailable"
    return 0
  fi

  local needed
  local dep
  mapfile -t needed < <("${READELF_BIN}" -d "${output_file}" 2>/dev/null | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p')

  for dep in "${needed[@]}"; do
    case "${dep}" in
      libc.so.*|libdl.so.*|libpthread.so.*|librt.so.*|libm.so.*|libresolv.so.*|ld-linux*.so.*|ld-musl-*.so.*)
        ;;
      *)
        echo "Unsupported dynamic dependency in ${output_file}: ${dep}" >&2
        echo "Expected only glibc-family runtime libraries so the .so remains portable to Nix systems." >&2
        exit 1
        ;;
    esac
  done
}

build_target() {
  local name="$1"
  local compiler_bin="$2"
  local compiler_mode="$3"
  local jdk_home="$4"
  local strip_bin="$5"
  local libcrypto_archive="$6"
  local binutils_dir
  local linker_flags_raw
  local -a linker_flags=()

  local output_file="${OUTPUT_DIR}/liblogger_${name}.so"
  binutils_dir="$(detect_default_binutils_dir "${name}" || true)"
  linker_flags_raw="$(detect_linker_flags "${compiler_bin}" || true)"
  append_flags "${linker_flags_raw}" linker_flags

  echo "Building ${name}"

  require_command "${compiler_bin}" "compiler for ${name}"
  require_command "${strip_bin}" "strip tool"
  require_file "${jdk_home}/include/jni.h" "JDK headers"
  require_file "${libcrypto_archive}" "static libcrypto archive"

  local cmd=(
    "${compiler_bin}"
    "${linker_flags[@]}"
    "${COMMON_FLAGS[@]}"
    -o "${output_file}"
    "${SOURCE_FILE}"
    -I"${jdk_home}/include"
    -I"${jdk_home}/include/linux"
    "${OPENSSL_CFLAGS[@]}"
    -Wl,-Bstatic
    "${libcrypto_archive}"
    "${DYNAMIC_LINK_FLAGS[@]}"
  )

  if [[ -n "${binutils_dir}" ]]; then
    cmd=(
      "${cmd[@]:0:1}"
      "-B${binutils_dir}"
      "${cmd[@]:1}"
    )
  fi

  case "${compiler_mode}" in
    x86-m32)
      cmd=(
        "${compiler_bin}"
        "${linker_flags[@]}"
        -m32
        ${binutils_dir:+"-B${binutils_dir}"}
        "${COMMON_FLAGS[@]}"
        -o "${output_file}"
        "${SOURCE_FILE}"
        -I"${jdk_home}/include"
        -I"${jdk_home}/include/linux"
        "${OPENSSL_CFLAGS[@]}"
        -Wl,-Bstatic
        "${libcrypto_archive}"
        "${DYNAMIC_LINK_FLAGS[@]}"
      )
      ;;
    i386-pc-linux-gnu|aarch64-linux-gnu|arm-linux-gnueabihf)
      if compiler_supports_target_flag "${compiler_bin}"; then
        cmd=(
          "${compiler_bin}"
          "${linker_flags[@]}"
          --target="${compiler_mode}"
          ${binutils_dir:+"-B${binutils_dir}"}
          "${COMMON_FLAGS[@]}"
          -o "${output_file}"
          "${SOURCE_FILE}"
          -I"${jdk_home}/include"
          -I"${jdk_home}/include/linux"
          "${OPENSSL_CFLAGS[@]}"
          -Wl,-Bstatic
          "${libcrypto_archive}"
          "${DYNAMIC_LINK_FLAGS[@]}"
        )
      fi
      ;;
  esac

  "${cmd[@]}"
  validate_runtime_deps "${output_file}"
  "${strip_bin}" --strip-unneeded "${output_file}"
}

mkdir -p "${OUTPUT_DIR}"

if [[ ! -f "${SOURCE_FILE}" ]]; then
  echo "Missing source file: ${SOURCE_FILE}" >&2
  exit 1
fi

for target_name in ${LINUX_TARGETS}; do
  case "${target_name}" in
    x64)
      build_target "x64" "${CXX_X64}" "" "${JDK_X64}" "${STRIP_X64}" "${LIBCRYPTO_X64}"
      ;;
    x86)
      if [[ "$(basename "${CXX_X86}")" == "i686-linux-gnu-g++" ]]; then
        build_target "x86" "${CXX_X86}" "" "${JDK_X86}" "${STRIP_X86}" "${LIBCRYPTO_X86}"
      else
        build_target "x86" "${CXX_X86}" "x86-m32" "${JDK_X86}" "${STRIP_X86}" "${LIBCRYPTO_X86}"
      fi
      ;;
    arm64)
      if compiler_supports_target_flag "${CXX_ARM64}"; then
        build_target "arm64" "${CXX_ARM64}" "aarch64-linux-gnu" "${JDK_ARM64}" "${STRIP_ARM64}" "${LIBCRYPTO_ARM64}"
      else
        build_target "arm64" "${CXX_ARM64}" "" "${JDK_ARM64}" "${STRIP_ARM64}" "${LIBCRYPTO_ARM64}"
      fi
      ;;
    arm32)
      if compiler_supports_target_flag "${CXX_ARM32}"; then
        build_target "arm32" "${CXX_ARM32}" "arm-linux-gnueabihf" "${JDK_ARM32}" "${STRIP_ARM32}" "${LIBCRYPTO_ARM32}"
      else
        build_target "arm32" "${CXX_ARM32}" "" "${JDK_ARM32}" "${STRIP_ARM32}" "${LIBCRYPTO_ARM32}"
      fi
      ;;
    *)
      echo "Unsupported LINUX_TARGETS entry: ${target_name}" >&2
      exit 1
      ;;
  esac
done

echo "Built Linux LibLogger binaries in ${OUTPUT_DIR}"