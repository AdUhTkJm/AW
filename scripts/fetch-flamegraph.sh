#!/usr/bin/env bash
#
# Download Brendan Gregg's FlameGraph toolkit and lay it out where
# scripts/flamegraph.sh looks for it:
#
#   third_party/flamegraph/flamegraph.pl
#
# flamegraph.pl is what makes the SVG interactive: click a frame to zoom,
# Ctrl-F to search, Ctrl-I to toggle case. No Python package provides that
# behaviour for arbitrary folded stacks, so the renderer is fetched here.
#
# FlameGraph is CDDL-1.0. It is fetched rather than committed so the repository
# does not redistribute it; after fetching, see third_party/flamegraph/LICENSE.
#
# Usage:
#   scripts/fetch-flamegraph.sh
#   scripts/fetch-flamegraph.sh --tag v1.0
#
set -euo pipefail

TAG="v1.0"

while [ $# -gt 0 ]; do
  case "$1" in
    --tag) TAG="${2:?--tag needs a value}"; shift 2 ;;
    -h|--help) sed -n '2,17p' "$0"; exit 0 ;;
    *) echo "unexpected argument: $1" >&2; exit 2 ;;
  esac
done

ROOT="$(cd -- "$(dirname -- "$0")/.." && pwd)"
DEST_DIR="${ROOT}/third_party/flamegraph"
CACHE_DIR="${DEST_DIR}/downloads"

if [ -f "${DEST_DIR}/flamegraph.pl" ]; then
  echo "already present: ${DEST_DIR}/flamegraph.pl"
  exit 0
fi

mkdir -p "${CACHE_DIR}"
ASSET="FlameGraph-${TAG}.tar.gz"
URL="https://github.com/brendangregg/FlameGraph/archive/refs/tags/${TAG}.tar.gz"
ARCHIVE="${CACHE_DIR}/${ASSET}"

if [ ! -f "${ARCHIVE}" ]; then
  echo "downloading ${ASSET}"
  curl -fL --output "${ARCHIVE}" "${URL}"
fi

TMP="$(mktemp -d "${DEST_DIR}/.extract.XXXXXX")"
trap 'rm -rf "${TMP}"' EXIT
mkdir -p "${DEST_DIR}"
tar -xzf "${ARCHIVE}" -C "${TMP}"

# The archive holds a single versioned directory; flatten it.
INNER="$(find "${TMP}" -mindepth 1 -maxdepth 1 -type d | head -1)"
if [ -z "${INNER}" ]; then
  echo "unexpected archive layout in ${ASSET}" >&2
  exit 1
fi
cp -a "${INNER}/." "${DEST_DIR}/"

echo "installed FlameGraph ${TAG} into ${DEST_DIR}"
echo "renderer: ${DEST_DIR}/flamegraph.pl"
