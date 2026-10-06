#!/bin/bash
# Gradle for the CrashCraft Fabric mod (fabric/build/libs/crashcraft-<version>.jar).
#   bash gradle_mc.sh build          bash gradle_mc.sh runClient   (a dev Minecraft with the mod)
# JAVA_HOME must be a Java 25 JDK (Prism's java-runtime-epsilon works); local.env can set it.
HERE="$(cd "$(dirname "$0")" && pwd)"
[ -f "$HERE/local.env" ] && . "$HERE/local.env"
cd "$HERE/fabric" && ./gradlew --no-daemon "$@"
