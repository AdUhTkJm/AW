#!/usr/bin/env bash
#
# Build aw_jni.dll for Windows with MSVC, driven from WSL.
#
# Why MSVC and not a MinGW cross build: OR-Tools ships Windows binaries only for
# Visual Studio, only supports MSVC on Windows, and its own cross_compile.sh
# hardcodes CMAKE_SYSTEM_NAME Linux. A MinGW cross build of the library is not a
# supported configuration, so the library and the shim that links it both have
# to be MSVC. What can be automated from WSL is the invocation: this script
# calls into cmd.exe, sets up vcvars64, and runs the same CMakeLists.txt the
# Linux build uses.
#
# Prerequisites, all on the Windows side:
#   * Visual Studio with the "Desktop development with C++" workload. That is
#     what supplies VC/Tools/MSVC and VC/Auxiliary/Build/vcvars64.bat; the
#     workload is not installed by default, and the script says so rather than
#     failing deep inside CMake.
#   * CMake and Ninja. Visual Studio ships both under
#     Common7/IDE/CommonExtensions/Microsoft/CMake/, and this script prefers
#     those copies over anything on PATH.
#   * third_party/ortools/windows-x86_64, laid out by
#     scripts/fetch-ortools.sh --platform windows.
#
# Usage: scripts/build-windows.sh [build-dir]
#
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "$0")/.." && pwd)"
BUILD_DIR="${1:-${ROOT}/build/windows}"
ORTOOLS="${ROOT}/third_party/ortools/windows-x86_64"
VSWHERE="/mnt/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"

if [ ! -x "$(command -v cmd.exe 2>/dev/null || true)" ]; then
  echo "cmd.exe is not reachable from this WSL distribution." >&2
  echo "Enable Windows interop (it is on by default) or run the build on Windows." >&2
  exit 1
fi

if [ ! -f "${ORTOOLS}/lib/cmake/ortools/ortoolsConfig.cmake" ]; then
  echo "missing Windows OR-Tools at ${ORTOOLS}" >&2
  echo "run: scripts/fetch-ortools.sh --platform windows" >&2
  exit 1
fi

if [ ! -x "${VSWHERE}" ]; then
  echo "vswhere.exe not found at ${VSWHERE}; is Visual Studio installed?" >&2
  exit 1
fi

# Ask vswhere for an install that actually has the C++ toolset.
VS_INSTALL="$( "${VSWHERE}" -latest -products '*' \
  -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 \
  -property installationPath 2>/dev/null | tr -d '\r' | head -1 )"

if [ -z "${VS_INSTALL}" ]; then
  echo "No Visual Studio installation with the C++ toolset was found." >&2
  echo "Open the Visual Studio Installer and add the workload" >&2
  echo "  'Desktop development with C++'" >&2
  echo "then re-run this script. (Without it there is no vcvars64.bat and no" >&2
  echo "VC/Tools/MSVC, so there is nothing for CMake to build with.)" >&2
  exit 1
fi

VCVARS="${VS_INSTALL}/VC/Auxiliary/Build/vcvars64.bat"
if [ ! -f "${VCVARS}" ]; then
  echo "found ${VS_INSTALL} but no ${VCVARS}" >&2
  exit 1
fi

# Prefer the CMake and Ninja that ship with Visual Studio so the versions match
# what the C++ workload was tested against.
CMAKE_EXE="${VS_INSTALL}/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
NINJA_DIR="${VS_INSTALL}/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja"
if [ ! -f "${CMAKE_EXE}" ]; then
  CMAKE_EXE="$(command -v cmake.exe 2>/dev/null || true)"
fi
if [ -z "${CMAKE_EXE}" ] || [ ! -f "${CMAKE_EXE}" ]; then
  echo "cmake.exe not found (looked in ${VS_INSTALL} and on PATH)" >&2
  exit 1
fi

mkdir -p "${BUILD_DIR}"

# wslpath gives the \\wsl.localhost\... path Windows tools need.
WIN_ROOT="$(wslpath -w "${ROOT}")"
WIN_BUILD="$(wslpath -w "${BUILD_DIR}")"
WIN_ORTOOLS="$(wslpath -w "${ORTOOLS}")"

BAT="$(mktemp --suffix=.bat)"
trap 'rm -f "${BAT}"' EXIT
cat > "${BAT}" <<BATEOF
@echo off
setlocal
call "${VCVARS}" || exit /b 1
if exist "${NINJA_DIR}" set "PATH=${NINJA_DIR};%PATH%"
"${CMAKE_EXE}" -S "${WIN_ROOT}" -B "${WIN_BUILD}" -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DAW_ORTOOLS_ROOT="${WIN_ORTOOLS}" || exit /b 1
"${CMAKE_EXE}" --build "${WIN_BUILD}" || exit /b 1
BATEOF

echo "Visual Studio : ${VS_INSTALL}"
echo "CMake         : ${CMAKE_EXE}"
echo "OR-Tools      : ${ORTOOLS}"
echo "Build dir     : ${BUILD_DIR}"
echo
cmd.exe /c "$(wslpath -w "${BAT}")"
echo
echo "artifacts in ${BUILD_DIR}:"
ls -1 "${BUILD_DIR}"/*.dll 2>/dev/null || echo "  (no dll produced)"
