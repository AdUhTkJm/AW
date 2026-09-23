#!/bin/zsh
# Linux (WSL) build and test. OR-Tools is picked up from third_party/ortools;
# run scripts/fetch-ortools.sh once if that directory is empty.
#
# The Windows DLL is a separate path: scripts/build-windows.sh.

set -e

JAVA_HOME=${JAVA_HOME:-/usr/lib/jvm/java-21-openjdk-amd64} cmake -S . -B build -G Ninja
cmake --build build

# Refresh the sample corpus when a client run is available.
DUMPS=/mnt/d/IdeaProjects/AppliedWheelchair/runs/client/aw
if [ -f "$DUMPS/recipes.awr" ]; then
  cp "$DUMPS/recipes.awr" temp/recipes-small.awr
  [ -f "$DUMPS/recipes.names.tsv" ] && cp "$DUMPS/recipes.names.tsv" temp/recipes-small.names.tsv
fi

# Run tests
build/aw_tests

# Sanity check the sample corpus, including a plan through CP-SAT.
if [ -f temp/recipes-small.awr ]; then
  build/awr_inspect --check temp/recipes-small.awr
fi
