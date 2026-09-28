#!/usr/bin/env bash
#
# Download the mozillazg/pinyin-data dump and place it where CMakeLists.txt
# looks for it:
#
#   third_party/pinyin.txt
#
# scripts/gen-pinyin.py turns it into build/generated/pinyin/Pinyin.h, which is
# compiled into aw_search. The file is a plain U+XXXX: reading list per line, so
# like nlohmann/json it is vendored into third_party/ (which is gitignored)
# rather than fetched at configure time, keeping the build offline once present.
#
# Usage:
#   scripts/fetch-pinyin.sh                 # pinned version
#   scripts/fetch-pinyin.sh --version 0.15.0
#
set -euo pipefail

VERSION="0.15.0"

while [ $# -gt 0 ]; do
  case "$1" in
    --version) VERSION="${2:?--version needs a value}"; shift 2 ;;
    -h|--help) sed -n '2,18p' "$0"; exit 0 ;;
    *) echo "unexpected argument: $1" >&2; exit 2 ;;
  esac
done

ROOT="$(cd -- "$(dirname -- "$0")/.." && pwd)"
DEST_DIR="${ROOT}/third_party"
CACHE_DIR="${DEST_DIR}/pinyin-data/downloads"
OUT="${DEST_DIR}/pinyin.txt"

if [ -f "${OUT}" ]; then
  echo "already present: ${OUT}"
  exit 0
fi

mkdir -p "${CACHE_DIR}"

ASSET="pinyin.txt"
URL="https://raw.githubusercontent.com/mozillazg/pinyin-data/v${VERSION}/${ASSET}"
ARCHIVE="${CACHE_DIR}/pinyin-v${VERSION}.txt"

if [ ! -f "${ARCHIVE}" ]; then
  echo "downloading ${ASSET} v${VERSION}"
  curl -fL --output "${ARCHIVE}" "${URL}"
fi

# A GitHub error page is small; the real dump is not. Catch a truncated or
# redirected download before it becomes a confusing generator error.
if [ "$(wc -c < "${ARCHIVE}")" -lt 500000 ]; then
  echo "downloaded file is too small to be pinyin.txt; check ${URL}" >&2
  exit 1
fi

# The generator only understands the U+XXXX: readings  # character line shape.
if ! grep -q '^U+4E00: ' "${ARCHIVE}"; then
  echo "downloaded file does not look like the pinyin-data dump; check ${URL}" >&2
  exit 1
fi

cp "${ARCHIVE}" "${OUT}"
echo "installed pinyin-data v${VERSION} into ${OUT}"
