#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
: "${ANDROID_HOME:?Set ANDROID_HOME to the Android SDK}"
jar="$ANDROID_HOME/platforms/android-34/android.jar"
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
javac -source 8 -target 8 -cp "$jar" -d "$out" src/GazelleHdmiProfiles.java
"$ANDROID_HOME/build-tools/34.0.0/d8" --min-api 28 --lib "$jar" --output "$out" "$out/GazelleHdmiProfiles.class"
mkdir -p bin
cp "$out/classes.dex" bin/gazelle-hdmi-profiles.dex
