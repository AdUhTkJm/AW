#!/bin/zsh
JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64 cmake -S . -B build -G Ninja
cmake --build build
cp /mnt/d/IdeaProjects/AppliedWheelchair/runs/client/aw/recipes.awr ./recipes.awr
