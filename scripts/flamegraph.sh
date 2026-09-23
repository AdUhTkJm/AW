#!/usr/bin/env bash
#
# Turn an awr_inspect CPU profile into an SVG flamegraph.
#
#   scripts/flamegraph.sh [--static] [--title TITLE] <profile.prof> [binary]
#
# By default the SVG is interactive -- click a frame to zoom, Ctrl-F to search,
# Ctrl-I to toggle case -- courtesy of Brendan Gregg's flamegraph.pl. Run
# scripts/fetch-flamegraph.sh once to install it under third_party/flamegraph/,
# or point FLAMEGRAPH_PL at another copy (inferno-flamegraph is compatible).
# Without any interactive renderer the script falls back to the static, pure
# Python scripts/flamegraph.py; --static selects that fallback explicitly.
#
# `binary` defaults to build/awr_inspect and must be the one that produced the
# profile, because pprof uses its symbols to name the frames. The output is
# written next to the profile, with the extension replaced by .svg.
#
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "$0")/.." && pwd)"
TITLE=""
STATIC=0
POSITIONAL=()

while [ $# -gt 0 ]; do
  case "$1" in
    --static) STATIC=1; shift ;;
    --title)  TITLE="${2:?--title needs a value}"; shift 2 ;;
    -h|--help) sed -n '2,17p' "$0"; exit 0 ;;
    -*) echo "unknown option: $1" >&2; exit 2 ;;
    *) POSITIONAL+=("$1"); shift ;;
  esac
done

PROFILE="${POSITIONAL[0]:-}"
BINARY="${POSITIONAL[1]:-${ROOT}/build/awr_inspect}"

if [ -z "${PROFILE}" ]; then
  echo "usage: scripts/flamegraph.sh [--static] [--title TITLE] <profile.prof> [binary]" >&2
  exit 2
fi
if [ ! -f "${PROFILE}" ]; then
  echo "no such profile: ${PROFILE}" >&2
  exit 1
fi
if [ ! -x "${BINARY}" ]; then
  echo "no such binary: ${BINARY}" >&2
  exit 1
fi

# Default the title to the profile name so a directory of graphs stays readable.
if [ -z "${TITLE}" ]; then
  TITLE="$(basename "${PROFILE}")"
  TITLE="${TITLE%.prof}"
fi

PPROF="${PPROF:-}"
if [ -z "${PPROF}" ]; then
  PPROF="$(command -v pprof || command -v google-pprof || true)"
fi
if [ -z "${PPROF}" ]; then
  echo "pprof not found (Ubuntu: apt install google-perftools)" >&2
  echo "or set PPROF=/path/to/pprof" >&2
  exit 1
fi

# plan.prof -> plan.svg; a profile without an extension becomes plan.prof.svg.
case "${PROFILE}" in
  *.*) OUT="${PROFILE%.*}.svg" ;;
  *)   OUT="${PROFILE}.svg" ;;
esac

# flamegraph.pl and inferno-flamegraph both read folded stacks on stdin and
# write the SVG to stdout.
RENDERER=""
if [ "${STATIC}" -eq 0 ]; then
  for candidate in \
      "${FLAMEGRAPH_PL:-}" \
      "${ROOT}/third_party/flamegraph/flamegraph.pl" \
      "$(command -v flamegraph.pl || true)" \
      "$(command -v inferno-flamegraph || true)"; do
    if [ -n "${candidate}" ] && [ -x "${candidate}" ]; then
      RENDERER="${candidate}"
      break
    fi
  done
fi

if [ -n "${RENDERER}" ]; then
  "${PPROF}" --collapsed "${BINARY}" "${PROFILE}" \
    | "${RENDERER}" --title "${TITLE}" > "${OUT}"
  echo "wrote ${OUT} (interactive, via $(basename "${RENDERER}"))"
  exit 0
fi

if [ "${STATIC}" -eq 0 ]; then
  echo "note: no interactive renderer found, using the static fallback" >&2
  echo "      run scripts/fetch-flamegraph.sh for click-to-zoom and search" >&2
fi
"${PPROF}" --collapsed "${BINARY}" "${PROFILE}" \
  | python3 "${ROOT}/scripts/flamegraph.py" --title "${TITLE}" > "${OUT}"
echo "wrote ${OUT} (static SVG)"
