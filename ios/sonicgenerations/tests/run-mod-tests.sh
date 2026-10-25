#!/usr/bin/env bash
# Builds and runs tests/mod_loader_tests.cpp with sanitizers on a Mac.
#   bash tests/run-mod-tests.sh "<extracted disc folder>/disk"
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${TMPDIR:-/tmp}/sg-mod-tests"
xcrun clang++ -std=c++20 -O1 -g -fsanitize=address,undefined -DSG_MODS_STANDALONE \
  -I"$HERE/../rexglue-sdk/include" \
  "$HERE"/src/mods/{cri_utf,crilayla,cpk_overlay,hedge_archive,ini,mod_database,mod_loader}.cpp \
  "$HERE/tests/mod_loader_tests.cpp" -o "$OUT"
"$OUT" "$1" "${2:-${TMPDIR:-/tmp}/sg-mod-fixtures}"
