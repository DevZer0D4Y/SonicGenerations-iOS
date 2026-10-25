#!/usr/bin/env bash
# Package the built app as an unsigned .ipa for a sideloading signer (ESign,
# AltStore, SideStore, Sideloadly, TrollStore), which signs it with its own
# certificate.
#
#   APP   built bundle (default: out/build/ios-arm64-release/sonicgenerations.app)
#   OUT   output path (default: out/Sonic-Generations-iOS.ipa)
#
# The payload is checked before it is zipped: no game files, no provisioning
# profile or certificate, and no build machine paths in the binaries.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
APP="${APP:-$HERE/../out/build/ios-arm64-release/sonicgenerations.app}"
OUT="${OUT:-$HERE/../out/Sonic-Generations-iOS.ipa}"

[ -d "$APP" ] || { echo "Built app not found at $APP - run the build first." >&2; exit 1; }

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/Payload"
# Clone the local APFS bundle to avoid duplicating the large executable.
# Fall back on filesystems that do not support copy-on-write clones.
if ! cp -cR "$APP" "$STAGE/Payload/" 2>/dev/null; then
  cp -R "$APP" "$STAGE/Payload/"
fi
BUNDLE="$STAGE/Payload/$(basename "$APP")"

# Strip anything that identifies the builder: a provisioning profile lists the
# team, name and device UDIDs, and a signature names the certificate.
rm -f "$BUNDLE/embedded.mobileprovision"
rm -rf "$BUNDLE/_CodeSignature"
find "$BUNDLE" \( -name '.DS_Store' -o -name '._*' \) -delete

# Never ship game data or signing material.
found="$(find "$BUNDLE" -type f \( -iname '*.iso' -o -iname '*.xiso' -o -iname '*.xex' \
           -o -iname '*.pac' -o -iname '*.mobileprovision' -o -iname '*.p12' -o -iname '*.cer' \) | head -5)"
if [ -n "$found" ]; then
  echo "Refusing to package game or signing files:" >&2
  echo "$found" >&2
  exit 1
fi
# Source file names end up in log and assert messages; they must not name
# the build machine's folders.
for binary in "$BUNDLE/$(basename "$APP" .app)" "$BUNDLE"/Frameworks/*.dylib; do
  if strings -a "$binary" | grep -qE '/Users/|/Volumes/|/private/var/folders/'; then
    echo "Refusing to package $(basename "$binary"): it contains build machine paths:" >&2
    strings -a "$binary" | grep -E '/Users/|/Volumes/|/private/var/folders/' | head -3 >&2
    exit 1
  fi
done

# Ad hoc signature: names no certificate or team; the signer replaces it.
for d in "$BUNDLE"/Frameworks/*.dylib; do codesign -f -s - "$d"; done
# ESign can sign a main executable without a local ad hoc signature.
# ADHOC_MAIN_SIGN=0 is useful when local codesign cannot process the guest binary.
if [ "${ADHOC_MAIN_SIGN:-1}" = 1 ]; then
  codesign -f -s - --entitlements "$HERE/sonicgenerations.entitlements" "$BUNDLE"
fi

mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"
(cd "$STAGE" && zip -qry "$OUT" Payload)
echo "Wrote $OUT"
