#!/usr/bin/env bash
#
# Download the nlohmann/json single header and place it where CMakeLists.txt
# looks for it:
#
#   third_party/nlohmann/nlohmann/json.hpp
#
# Only the JNI option loader uses JSON (src/OptionsJson.cpp). The recipe payload
# stays binary: it is large and cold, while the options are a few dozen scalars
# a human edits in the mod config. Vendoring the header rather than adding a
# build-time fetch keeps the Windows build offline and reproducible.
#
# Usage:
#   scripts/fetch-nlohmann.sh                 # pinned version
#   scripts/fetch-nlohmann.sh --version 3.11.3
#
set -euo pipefail

VERSION="3.11.3"

while [ $# -gt 0 ]; do
  case "$1" in
    --version) VERSION="${2:?--version needs a value}"; shift 2 ;;
    -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
    *) echo "unexpected argument: $1" >&2; exit 2 ;;
  esac
done

ROOT="$(cd -- "$(dirname -- "$0")/.." && pwd)"
DEST_DIR="${ROOT}/third_party/nlohmann"
CACHE_DIR="${DEST_DIR}/downloads"
OUT="${DEST_DIR}/nlohmann/json.hpp"

if [ -f "${OUT}" ]; then
  echo "already present: ${OUT}"
  exit 0
fi

mkdir -p "${CACHE_DIR}" "${DEST_DIR}/nlohmann"

ASSET="json.hpp"
URL="https://github.com/nlohmann/json/releases/download/v${VERSION}/${ASSET}"
ARCHIVE="${CACHE_DIR}/json-v${VERSION}.hpp"

if [ ! -f "${ARCHIVE}" ]; then
  echo "downloading ${ASSET} v${VERSION}"
  curl -fL --output "${ARCHIVE}" "${URL}"
fi

# A GitHub error page is small; the real header is not. Catch a truncated or
# redirected download before it becomes a confusing compile error.
if [ "$(wc -c < "${ARCHIVE}")" -lt 100000 ]; then
  echo "downloaded file is too small to be json.hpp; check ${URL}" >&2
  exit 1
fi

cp "${ARCHIVE}" "${OUT}"
echo "installed nlohmann/json v${VERSION} into ${OUT}"
