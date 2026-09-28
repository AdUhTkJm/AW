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
# Why the build tree sits in the Windows temp directory instead of build/:
# Ninja runs every MSVC command through cmd.exe with the build directory as the
# working directory, and cmd.exe cannot chdir to a UNC path. The WSL filesystem
# is reachable from Windows only as \\wsl.localhost\..., so cmd.exe started in a
# build tree inside it warns, falls back to C:\Windows, and resolves the
# relative /out: and CMakeFiles\... paths of the link step against the wrong
# directory. CMake then reports a broken toolchain ("cl.exe ... is not able to
# compile a simple test program") even though cl.exe is fine. Sources on the
# share are not affected, because cl.exe is handed absolute paths; only the
# build tree needs a drive letter. So the objects are built under the Windows
# temp directory and the DLLs are copied into the artifact directory at the end.
# Passing a /mnt/c/... artifact directory builds in place instead, with no copy.
#
# Prerequisites, all on the Windows side:
#   * Visual Studio with the "Desktop development with C++" workload. That is
#     what supplies VC/Tools/MSVC and VC/Auxiliary/Build/vcvars64.bat; the
#     workload is not installed by default, and the script says so rather than
#     failing deep inside CMake.
#   * CMake and Ninja. Visual Studio ships both under
#     Common7/IDE/CommonExtensions/Microsoft/CMake/, and this script prefers
#     those copies over anything on PATH.
#   * A Windows JDK. find_package(JNI) runs on the Windows side, so it reads the
#     Windows JAVA_HOME and cannot see a WSL-only JDK.
#   * third_party/ortools/windows-x86_64, laid out by
#     scripts/fetch-ortools.sh --platform windows.
#
# Usage: scripts/build-windows.sh [artifact-dir]
#
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "$0")/.." && pwd)"
ARTIFACT_DIR="${1:-${ROOT}/build/windows}"
ORTOOLS="${ROOT}/third_party/ortools/windows-x86_64"
VSWHERE="/mnt/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"

# cmd.exe inherits this shell's working directory, which lives on
# \\wsl.localhost\... and is therefore unusable as a Windows process directory.
# Starting it from a Windows directory silences the "UNC paths are not
# supported" banner and keeps %CD% predictable; every path below is absolute.
win_cmd() {
  ( cd /mnt/c && cmd.exe /d /c "$1" )
}

# bash's test builtins only resolve Unix paths, while vswhere and the .bat below
# speak Windows ones ("C:\..."). To bash those look like relative filenames, so
# a plain -f silently matches nothing and a present toolchain reads as missing.
win_exists() {
  local unix_path
  unix_path="$(wslpath -u "$1" 2>/dev/null)" || return 1
  [ -f "${unix_path}" ]
}

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
if ! win_exists "${VCVARS}"; then
  echo "found ${VS_INSTALL} but no ${VCVARS}" >&2
  exit 1
fi

# Prefer the CMake and Ninja that ship with Visual Studio so the versions match
# what the C++ workload was tested against.
CMAKE_EXE="${VS_INSTALL}/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
NINJA_DIR="${VS_INSTALL}/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja"
if ! win_exists "${CMAKE_EXE}"; then
  CMAKE_EXE="$(command -v cmake.exe 2>/dev/null || true)"
fi
if [ -z "${CMAKE_EXE}" ]; then
  echo "cmake.exe not found (looked in ${VS_INSTALL} and on PATH)" >&2
  exit 1
fi

# The .bat runs under cmd.exe, so cmake has to be spelled the Windows way. It
# already is when it came from Visual Studio; the PATH fallback is a /mnt/c/...
# path that cmd.exe cannot launch. Only feed wslpath genuine Unix paths: given a
# Windows one it silently rewrites ':' and '\' to their private-use lookalikes
# (U+F03A, U+F05C), which produces a command line cmd.exe splits in the middle.
case "${CMAKE_EXE}" in
  [A-Za-z]:[\\/]*) CMAKE_EXE_WIN="${CMAKE_EXE}" ;;
  *)               CMAKE_EXE_WIN="$(wslpath -w "${CMAKE_EXE}")" ;;
esac

# A /mnt/c/... artifact directory is already on a real Windows volume, so it can
# host the build tree directly. Anything else is inside the WSL filesystem and
# has to be mirrored to Windows temp (see the header).
case "${ARTIFACT_DIR}" in
  /mnt/*)
    BUILD_TREE_WIN="$(wslpath -w "${ARTIFACT_DIR}")"
    COPY_BACK=no
    ;;
  *)
    WIN_LOCAL_APPDATA="$(win_cmd 'echo %LOCALAPPDATA%' | tr -d '\r' | head -1)"
    if [ -z "${WIN_LOCAL_APPDATA}" ] || [ "${WIN_LOCAL_APPDATA#*%}" != "${WIN_LOCAL_APPDATA}" ]; then
      echo "could not read %LOCALAPPDATA% from Windows." >&2
      echo "Pass an artifact directory on a Windows volume instead, e.g." >&2
      echo "  scripts/build-windows.sh /mnt/c/Users/<you>/aw-build-windows" >&2
      exit 1
    fi
    BUILD_TREE_WIN="${WIN_LOCAL_APPDATA}\\Temp\\aw-build-windows"
    COPY_BACK=yes
    ;;
esac

mkdir -p "${ARTIFACT_DIR}"

# wslpath gives the \\wsl.localhost\... paths Windows tools need.
WIN_ROOT="$(wslpath -w "${ROOT}")"
WIN_ORTOOLS="$(wslpath -w "${ORTOOLS}")"

BAT="$(mktemp --suffix=.bat)"
trap 'rm -f "${BAT}"' EXIT
cat > "${BAT}" <<BATEOF
@echo off
setlocal
call "${VCVARS}" || exit /b 1
if exist "${NINJA_DIR}" set "PATH=${NINJA_DIR};%PATH%"
"${CMAKE_EXE_WIN}" -S "${WIN_ROOT}" -B "${BUILD_TREE_WIN}" -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DAW_ORTOOLS_ROOT="${WIN_ORTOOLS}" || exit /b 1
"${CMAKE_EXE_WIN}" --build "${BUILD_TREE_WIN}" || exit /b 1
BATEOF

echo "Visual Studio : ${VS_INSTALL}"
echo "CMake         : ${CMAKE_EXE}"
echo "OR-Tools      : ${ORTOOLS}"
echo "Build tree    : ${BUILD_TREE_WIN}"
echo "Artifacts     : ${ARTIFACT_DIR}"
echo
win_cmd "$(wslpath -w "${BAT}")"

if [ "${COPY_BACK}" = yes ]; then
  WSL_BUILD_TREE="$(wslpath -u "${BUILD_TREE_WIN}")"
  shopt -s nullglob
  DLLS=( "${WSL_BUILD_TREE}"/*.dll )
  shopt -u nullglob
  if [ ${#DLLS[@]} -gt 0 ]; then
    # aw_jni.dll plus the OR-Tools runtime the mod has to ship beside it.
    cp -f "${DLLS[@]}" "${ARTIFACT_DIR}/"
  fi
fi

echo
echo "artifacts in ${ARTIFACT_DIR}:"
ls -1 "${ARTIFACT_DIR}"/*.dll 2>/dev/null || echo "  (no dll produced)"
