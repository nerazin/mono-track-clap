#!/bin/sh
set -eu
cd "$(dirname "$0")"
bundle="build/Mono Track.clap"
mkdir -p "$bundle/Contents/MacOS"
cp Info.plist "$bundle/Contents/Info.plist"
flags="-O2 -Wall -Wextra -Wno-unused-function -fvisibility=hidden -mmacosx-version-min=11.0 -arch arm64 -arch x86_64 -I vendor/clap/include"
mkdir -p build/obj
if commit=$(git rev-parse --short HEAD 2>/dev/null); then
  git diff --quiet HEAD -- || commit="$commit+"   # built from uncommitted changes
else
  commit=unknown
fi
xcrun clang -std=c11 -Wpedantic $flags -c source/mono_track_clap.c -o build/obj/clap.o
xcrun clang -std=c11 -fobjc-arc $flags "-DMT_COMMIT=\"$commit\"" -c source/mono_track_gui.m -o build/obj/gui.o
xcrun clang -mmacosx-version-min=11.0 -arch arm64 -arch x86_64 -bundle build/obj/clap.o build/obj/gui.o \
  -framework Cocoa -framework QuartzCore -Wl,-exported_symbol,_clap_entry \
  -o "$bundle/Contents/MacOS/Mono Track"
codesign --force --sign - --timestamp=none "$bundle"
printf 'Built: %s\n' "$bundle"
