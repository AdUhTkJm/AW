#!/usr/bin/env bash
#
# Download an OR-Tools C++ release and lay it out where CMakeLists.txt looks
# for it:
#
#   third_party/ortools/linux-x86_64/
#   third_party/ortools/windows-x86_64/
#
# The build consumes the archive through its CMake package, not by pointing
# -I/-L at the archive directly. That matters: the package is what injects
# OR_PROTO_DLL=, without which the generated protobuf headers do not compile.
#
# Usage:
#   scripts/fetch-ortools.sh                       # host platform
#   scripts/fetch-ortools.sh --platform windows    # the MSVC build, from WSL
#   scripts/fetch-ortools.sh --ubuntu 22.04
#
set -euo pipefail

VERSION="9.15.6755"
TAG="v9.15"
UBUNTU="24.04"
PLATFORM=""

while [ $# -gt 0 ]; do
  case "$1" in
    --platform) PLATFORM="${2:?--platform needs linux or windows}"; shift 2 ;;
    --version)  VERSION="${2:?--version needs a value}"; shift 2 ;;
    --tag)      TAG="${2:?--tag needs a value}"; shift 2 ;;
    --ubuntu)   UBUNTU="${2:?--ubuntu needs a value}"; shift 2 ;;
    -h|--help)  sed -n '2,18p' "$0"; exit 0 ;;
    *) echo "unexpected argument: $1" >&2; exit 2 ;;
  esac
done

if [ -z "${PLATFORM}" ]; then
  case "$(uname -s)" in
    Linux) PLATFORM="linux" ;;
    *) echo "cannot guess the platform; pass --platform linux|windows" >&2; exit 2 ;;
  esac
fi

ROOT="$(cd -- "$(dirname -- "$0")/.." && pwd)"
DEST_DIR="${ROOT}/third_party/ortools"
CACHE_DIR="${DEST_DIR}/downloads"
mkdir -p "${CACHE_DIR}"

case "${PLATFORM}" in
  linux)
    ASSET="or-tools_amd64_ubuntu-${UBUNTU}_cpp_v${VERSION}.tar.gz"
    OUT="${DEST_DIR}/linux-x86_64"
    ;;
  windows)
    # OR-Tools only ships MSVC x86_64 archives for Windows, and only MSVC is
    # supported there. A MinGW cross build of the library is not a supported
    # configuration.
    ASSET="or-tools_x64_VisualStudio2022_cpp_v${VERSION}.zip"
    OUT="${DEST_DIR}/windows-x86_64"
    ;;
  *) echo "unknown platform: ${PLATFORM}" >&2; exit 2 ;;
esac

if [ -f "${OUT}/lib/cmake/ortools/ortoolsConfig.cmake" ]; then
  echo "already present: ${OUT}"
  exit 0
fi

URL="https://github.com/google/or-tools/releases/download/${TAG}/${ASSET}"
ARCHIVE="${CACHE_DIR}/${ASSET}"
if [ ! -f "${ARCHIVE}" ]; then
  echo "downloading ${ASSET}"
  curl -fL --output "${ARCHIVE}" "${URL}"
fi

TMP="$(mktemp -d "${DEST_DIR}/.extract.XXXXXX")"
trap 'rm -rf "${TMP}"' EXIT
mkdir -p "${OUT}"

case "${ASSET}" in
  *.tar.gz) tar -xzf "${ARCHIVE}" -C "${TMP}" ;;
  *.zip)    unzip -q "${ARCHIVE}" -d "${TMP}" ;;
esac

# The archive holds a single versioned directory; flatten it.
INNER="$(find "${TMP}" -mindepth 1 -maxdepth 1 -type d | head -1)"
if [ -z "${INNER}" ]; then
  echo "unexpected archive layout in ${ASSET}" >&2
  exit 1
fi
cp -a "${INNER}/." "${OUT}/"

# Examples dominate the extracted size and nothing here uses them.
rm -rf "${OUT}/examples"

echo "installed ${ASSET} into ${OUT}"
