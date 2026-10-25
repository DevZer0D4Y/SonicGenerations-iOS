#!/usr/bin/env bash
# Builds Sonic Generations for iOS from your own default.xex: the ReXGlue
# code generator (for the Mac) and runtime (for iOS), the game's C++ from your
# executable, the app, and an unsigned IPA.
#
#   bash ios/build.sh <default.xex, or the extracted disc folder holding it>
#
#   JOBS   build parallelism (default 2)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SDK="$HERE/rexglue-sdk"
APP="$HERE/sonicgenerations"
JOBS="${JOBS:-2}"
case "$JOBS" in ''|*[!0-9]*|0) echo 'JOBS must be a positive integer.' >&2; exit 1;; esac

source_xex="${1:?usage: bash ios/build.sh <default.xex or extracted disc folder>}"
# The disc names it Default.xex.
[ -d "$source_xex" ] && source_xex="$(find "$source_xex" -maxdepth 1 -iname default.xex | head -n 1)"
[ -f "$source_xex" ] || { echo "default.xex not found: $source_xex" >&2; exit 1; }

[ "$(uname -s)" = Darwin ] || { echo 'Build on macOS with Xcode installed.' >&2; exit 1; }
for tool in cmake ninja python3 xcrun codesign zip; do
  command -v "$tool" >/dev/null || { echo "Required tool missing: $tool" >&2; exit 1; }
done
xcrun --sdk iphoneos --show-sdk-path >/dev/null

# The executable is read from ios/game_xex/ (ignored by git, never committed).
mkdir -p "$HERE/game_xex"
cp "$source_xex" "$HERE/game_xex/default.xex"

(
  cd "$SDK"
  # The code generator, a Mac program, with the runtime library it links.
  cmake --preset mac-arm64 -DREXGLUE_REGISTER_PACKAGE=OFF -DREXGLUE_ENABLE_TRACY=OFF
  cmake --build out/build/mac-arm64 --config Release --target rexglue --parallel "$JOBS"
  # The runtime for iOS.
  cmake --preset ios-arm64 -DREXGLUE_REGISTER_PACKAGE=OFF
  cmake --build out/build/ios-arm64 --config Release --parallel "$JOBS"
  cmake --install out/build/ios-arm64 --config Release
)
export REXGLUE_HOST_TOOL="$SDK/out/mac-arm64/Release/rexglue"
export REXGLUE_IOS_SDK="$SDK/out/install/ios-arm64"

(
  cd "$APP"
  "$REXGLUE_HOST_TOOL" codegen sonicgenerations_manifest.toml
  cmake --preset ios-arm64-release
  cmake --build --preset ios-arm64-release --parallel "$JOBS"
  bash ios/make-ipa.sh
)
echo "IPA ready: $APP/out/Sonic-Generations-iOS.ipa"
