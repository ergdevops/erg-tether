#!/bin/bash
# Build the installer package that the Homebrew cask downloads.
set -euo pipefail

cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION=$(cat VERSION)
OUT="dist/ErgTether-$VERSION.pkg"

command -v pkgbuild >/dev/null || { echo "pkgbuild not found" >&2; exit 1; }

make >/dev/null
[ -x erg-tetherd ]   || { echo "erg-tetherd missing" >&2; exit 1; }
[ -d ErgTether.app ] || { echo "ErgTether.app missing" >&2; exit 1; }

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

install -d "$STAGE/usr/local/libexec" "$STAGE/Library/LaunchDaemons" "$STAGE/Library/LaunchAgents"
install -m 755 erg-tetherd "$STAGE/usr/local/libexec/erg-tetherd"

# ditto, not cp -R: it is the only copy that reproduces a bundle faithfully.
ditto ErgTether.app "$STAGE/usr/local/libexec/ErgTether.app"

# Strip what extended attributes we can. macOS adds com.apple.provenance to
# every file and will not let a normal user remove it, so pkgbuild still emits
# ._ AppleDouble sidecars for it and prints "write: Permission denied" while
# doing so. Both are cosmetic: the signature on the extracted payload verifies
# (--deep --strict) and the binaries run, which is what matters.
xattr -cr "$STAGE" 2>/dev/null || true

# Re-sign in place so the signature covers exactly what ships.
codesign --force --sign - "$STAGE/usr/local/libexec/ErgTether.app"
install -m 644 launchd/com.ergtether.daemon.plist "$STAGE/Library/LaunchDaemons/"
install -m 644 launchd/com.ergtether.bar.plist    "$STAGE/Library/LaunchAgents/"

mkdir -p dist
pkgbuild \
    --root "$STAGE" \
    --scripts packaging/scripts \
    --identifier com.ergtether.pkg \
    --version "$VERSION" \
    --install-location / \
    --ownership recommended \
    "$OUT" >/dev/null

SHA=$(shasum -a 256 "$OUT" | awk '{print $1}')
echo "$OUT"
echo "sha256  $SHA"
echo
echo "Next:"
echo "  1. gh release create v$VERSION $OUT -R ergdevops/erg-tether"
echo "  2. update version and sha256 in the tap's Casks/ergtether.rb"
