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
  cp "$DUMPS/recipes.awr" temp/recipes-vanilla.awr
  [ -f "$DUMPS/recipes.names.tsv" ] && cp "$DUMPS/recipes.names.tsv" temp/recipes-vanilla.names.tsv
fi

# Run tests
# build/aw_tests

# Run a performance test
# build/awr_inspect temp/recipes-nast.awr --plan 'mekanism:purification_chamber' --profile temp/a.prof --time-limit 20 --workers 1
# scripts/flamegraph.sh temp/a.prof build/awr_inspect
