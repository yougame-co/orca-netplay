#!/bin/bash
# Builds the macOS (arm64) app as a self-contained bundle and zips it as YG Dolphin.app
# (YG Baiji.app in 0.3.40 and 0.3.41, Orca.app before). CMake still names its bundle Orca.app,
# and the program inside stays
# Contents/MacOS/Orca: installed YouGame apps let only that path run in their sandbox.
#
#   Tools/orca-package-macos.sh [build-dir] [out-dir]
#     build-dir  default <repo>/build-orca-release (configured for a release on every run)
#     out-dir    default <build-dir>/package
#
# Without credentials the bundle stays unsigned (the executable keeps the linker's ad-hoc
# signature) and the zip is named *-unsigned.zip: it runs on this Mac, not through Gatekeeper on
# another. A release signs and notarizes, and needs all of:
#   CSC_LINK (Developer ID Application .p12) and CSC_KEY_PASSWORD
#   APPLE_API_KEY (.p8 path), APPLE_API_KEY_ID, APPLE_API_ISSUER
#   ORCA_SOURCE_URL: the public repository players can fetch the source from (GPL); its
#     yg-dolphin-<version> tag (yg-baiji-<version> in 0.3.40 and 0.3.41, orca-<version> before)
#     must point at HEAD, and the tree must be clean before and after the build.
# ORCA_INTERNAL=1 instead signs and notarizes an internal test build from the private repository:
# the same credentials and clean tree, but no ORCA_SOURCE_URL or tag. SOURCE.txt says it is not for
# public distribution and the zip is named *-internal.zip. Such a build only goes to testers.
# ORCA_BUNDLE_PUBLIC_KEY (64 lowercase hex digits) is the key flat bundles must be signed with
# (ORCA.md "Flat bundles"); unset, the build trusts none and refuses every bundle. The TEST key is
# never trusted in a package: the build turns it off and the executable is checked for it.
# ORCA_STAGING_BUILD=1 marks a staging build (YouGame's staging scripts set it): only such a build
# may pin the STAGING flat-bundle key, and it is never a public release. Any other build refuses
# that key, as ORCA_BUNDLE_PUBLIC_KEY and in the executable, the same way it refuses the TEST key.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${1:-$ROOT/build-orca-release}"
OUT_DIR="${2:-$BUILD_DIR/package}"
ENTITLEMENTS="$ROOT/Source/Core/DolphinNoGUI/Orca.entitlements"
BUILT="$BUILD_DIR/Binaries/Orca.app"
# What ships: the name players see. The program keeps its name (CFBundleExecutable).
APP="$BUILD_DIR/stage/YG Dolphin.app"
EXE="$APP/Contents/MacOS/Orca"

fail() {
  echo "orca-package: $*" >&2
  exit 1
}

[ "$(uname -s)" = Darwin ] || fail "macOS only"

# No build-machine paths in the binary: __FILE__ (asserts, logs) and debug info name the sources from
# the checkout ("./Source/..."), not from the builder's home folder (0.3.30's executable had 400-odd).
# The flags are a plain string on the compilers' command lines, so the paths can't hold a space.
case "$ROOT$BUILD_DIR" in
*[[:space:]]*) fail "the checkout or build path has a space; build from one without" ;;
esac
PREFIX_MAP="-ffile-prefix-map=$ROOT=. -fdebug-prefix-map=$ROOT=."
case "$BUILD_DIR/" in
"$ROOT"/*) ;;
*) PREFIX_MAP="$PREFIX_MAP -ffile-prefix-map=$BUILD_DIR=build -fdebug-prefix-map=$BUILD_DIR=build" ;;
esac

# The bundle-flat TEST key (Source/Core/Core/Orca/Bundle.cpp).
TEST_BUNDLE_KEY=afb9463d5c16f0f617432d76f75f37a8c649e36d7c626088ba00b19dea115a43
# The STAGING flat-bundle key's public half (staging builds only, 2026-10-12; its private half never
# leaves the lead's staging folder). orca-package-linux.sh and orca/bundle-key-guard.py carry it too.
STAGING_BUNDLE_KEY=8422b8b28b82aff024427d77e6c72a38d9ea75bcb6533fe706c61006f742c0ab
STAGING=0
case "${ORCA_STAGING_BUILD:-}" in
"" | 0) ;;
1) STAGING=1 ;;
*) fail "ORCA_STAGING_BUILD is 1 or unset, not ${ORCA_STAGING_BUILD}" ;;
esac
BUNDLE_KEY="${ORCA_BUNDLE_PUBLIC_KEY:-}"
if [ -n "$BUNDLE_KEY" ]; then
  [[ "$BUNDLE_KEY" =~ ^[0-9a-f]{64}$ ]] || fail "ORCA_BUNDLE_PUBLIC_KEY is not 64 lowercase hex digits"
  [ "$BUNDLE_KEY" != "$TEST_BUNDLE_KEY" ] || fail "ORCA_BUNDLE_PUBLIC_KEY is the TEST key"
  [ "$BUNDLE_KEY" != "$STAGING_BUNDLE_KEY" ] || [ "$STAGING" = 1 ] ||
    fail "ORCA_BUNDLE_PUBLIC_KEY is the STAGING key: only a staging build (ORCA_STAGING_BUILD=1) pins it"
fi

RELEASE=0
INTERNAL=0
case "${ORCA_INTERNAL:-}" in
"" | 0) ;;
1) INTERNAL=1 ;;
*) fail "ORCA_INTERNAL is 1 or unset, not ${ORCA_INTERNAL}" ;;
esac
if [ "$INTERNAL" = 1 ]; then
  [ -z "${ORCA_SOURCE_URL:-}" ] || fail "ORCA_INTERNAL=1 is a private build; unset ORCA_SOURCE_URL"
  for var in CSC_LINK CSC_KEY_PASSWORD APPLE_API_KEY APPLE_API_KEY_ID APPLE_API_ISSUER; do
    [ -n "${!var:-}" ] || fail "an internal build is signed and needs $var"
  done
  RELEASE=1
elif [ -n "${CSC_LINK:-}${CSC_KEY_PASSWORD:-}${APPLE_API_KEY:-}${APPLE_API_KEY_ID:-}${APPLE_API_ISSUER:-}" ]; then
  for var in CSC_LINK CSC_KEY_PASSWORD APPLE_API_KEY APPLE_API_KEY_ID APPLE_API_ISSUER ORCA_SOURCE_URL; do
    [ -n "${!var:-}" ] || fail "a signed build is a release and also needs $var"
  done
  RELEASE=1
fi

COMMIT="$(git -C "$ROOT" rev-parse HEAD)"
# Changed or untracked paths. A git status that fails (a broken submodule link, say) counts as
# dirty, never as clean.
tree_state() {
  local out
  out="$(git -C "$ROOT" status --porcelain --untracked-files=normal)" ||
    { echo "git status failed"; return 0; }
  printf '%s' "$out"
}
STATE="$(tree_state)"
DIRTY=""
if [ "$STATE" = "git status failed" ]; then
  DIRTY=" (tree state unknown: git status failed)"
elif [ -n "$STATE" ]; then
  DIRTY=" (with local changes)"
fi
# yg-dolphin-<version> from 0.3.42; yg-baiji-<version> in 0.3.40 and 0.3.41; orca-<version> before.
TAG="$(git -C "$ROOT" tag --points-at HEAD --list 'yg-dolphin-*' | head -n 1)"
[ -n "$TAG" ] || TAG="$(git -C "$ROOT" tag --points-at HEAD --list 'yg-baiji-*' | head -n 1)"
[ -n "$TAG" ] || TAG="$(git -C "$ROOT" tag --points-at HEAD --list 'orca-*' | head -n 1)"
TAG_VERSION="${TAG#yg-dolphin-}"
TAG_VERSION="${TAG_VERSION#yg-baiji-}"
TAG_VERSION="${TAG_VERSION#orca-}"
VERSION="${ORCA_VERSION:-$TAG_VERSION}"
VERSION="${VERSION:-0.3.63}"
# Info.plist versions are one to three integers: 0.2.0-rc1 shows as 0.2.0.
PLIST_VERSION="$(echo "$VERSION" | grep -Eo '^[0-9]+(\.[0-9]+){0,2}' || true)"
PLIST_VERSION="${PLIST_VERSION:-0.3.63}"

if [ "$RELEASE" = 1 ]; then
  [ -z "$DIRTY" ] || fail "a signed build needs a clean tree (untracked files included)$DIRTY"
fi
[ "$STAGING" = 0 ] || [ "$RELEASE" = 0 ] || [ "$INTERNAL" = 1 ] ||
  fail "a staging build (ORCA_STAGING_BUILD=1) is never a public release"
if [ "$RELEASE" = 1 ] && [ "$INTERNAL" = 0 ]; then
  [ -n "$TAG" ] || fail "a release needs its version tag on HEAD"
  # Anonymous, so a private repository can't pass: players must be able to fetch it.
  PUBLISHED="$(GIT_TERMINAL_PROMPT=0 git -c credential.helper= ls-remote "$ORCA_SOURCE_URL" \
    "refs/tags/$TAG" "refs/tags/$TAG^{}" 2>/dev/null | awk '{print $1}' | tail -n 1)" ||
    fail "can't read $ORCA_SOURCE_URL without credentials"
  [ "$PUBLISHED" = "$COMMIT" ] ||
    fail "$TAG at $ORCA_SOURCE_URL is ${PUBLISHED:-missing}, not HEAD ($COMMIT)"
fi

# A release links nothing outside macOS: bundled libraries (not Homebrew's), macOS's own iconv (so
# no LGPL code is bundled), no frame-dump encoder and none of the frontends' extras. Configured on
# every run so an older cache can't slip other settings in, and the bundle is rebuilt from nothing
# so files deleted from Sys don't linger in it.
rm -rf "$BUILT" "$BUILD_DIR/stage"
cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
  -DENABLE_QT=OFF -DENABLE_NOGUI=ON -DORCA_MACOS_BUNDLE=ON -DORCA_VERSION="$PLIST_VERSION" \
  -DUSE_SYSTEM_LIBS=OFF -DUSE_SYSTEM_ICONV=ON -DENCODE_FRAMEDUMPS=OFF -DENABLE_LLVM=OFF \
  -DENABLE_ANALYTICS=OFF -DENABLE_AUTOUPDATE=OFF -DUSE_DISCORD_PRESENCE=OFF \
  -DUSE_RETRO_ACHIEVEMENTS=OFF -DUSE_MGBA=OFF -DUSE_UPNP=OFF -DENABLE_VULKAN=OFF \
  -DENABLE_CLI_TOOL=OFF -DENABLE_TESTS=OFF -DUSE_SANITIZERS=OFF \
  -DORCA_BUNDLE_TRUST_TEST_KEY=OFF -DORCA_BUNDLE_PUBLIC_KEY="$BUNDLE_KEY" \
  -DCMAKE_C_FLAGS="$PREFIX_MAP" -DCMAKE_CXX_FLAGS="$PREFIX_MAP" \
  -DCMAKE_OBJC_FLAGS="$PREFIX_MAP" -DCMAKE_OBJCXX_FLAGS="$PREFIX_MAP" >/dev/null
grep -q '^ORCA_BUNDLE_TRUST_TEST_KEY:BOOL=OFF$' "$BUILD_DIR/CMakeCache.txt" ||
  fail "CMake's cache still trusts the bundle-flat TEST key"
cmake --build "$BUILD_DIR" --target dolphin-nogui

if [ "$RELEASE" = 1 ]; then
  # The source must not have moved under the build.
  [ "$(git -C "$ROOT" rev-parse HEAD)" = "$COMMIT" ] && [ -z "$(tree_state)" ] ||
    fail "the tree changed during the build; start again from a clean checkout of ${TAG:-$COMMIT}"
fi

[ -d "$BUILT" ] || fail "no $BUILT"
mkdir -p "$BUILD_DIR/stage"
ditto "$BUILT" "$APP"
[ -x "$EXE" ] || fail "no $EXE"
# strings on its own, so a strings that fails can't read as "no leaks".
STRINGS_OUT="$(mktemp)"
strings -a "$EXE" > "$STRINGS_OUT" || { rm -f "$STRINGS_OUT"; fail "strings could not read $EXE"; }
[ -s "$STRINGS_OUT" ] || { rm -f "$STRINGS_OUT"; fail "strings found nothing in $EXE"; }
LEAKS="$(grep -F -e "$ROOT/" -e "$BUILD_DIR/" -e "${HOME:-$ROOT}/" "$STRINGS_OUT" | sort -u | head -n 5 || true)"
# Flat bundles: never the TEST key, the STAGING key only in a staging build, and the pinned key when
# there is one (Bundle.cpp keeps them as text).
if grep -qF "$TEST_BUNDLE_KEY" "$STRINGS_OUT"; then
  rm -f "$STRINGS_OUT"
  fail "the executable trusts the bundle-flat TEST key"
fi
if [ "$STAGING" = 0 ] && grep -qF "$STAGING_BUNDLE_KEY" "$STRINGS_OUT"; then
  rm -f "$STRINGS_OUT"
  fail "the executable trusts the STAGING bundle key, and this is not a staging build"
fi
if [ -n "$BUNDLE_KEY" ] && ! grep -qF "$BUNDLE_KEY" "$STRINGS_OUT"; then
  rm -f "$STRINGS_OUT"
  fail "the executable doesn't hold ORCA_BUNDLE_PUBLIC_KEY"
fi
echo "orca-package: flat bundles: ${BUNDLE_KEY:-off (no ORCA_BUNDLE_PUBLIC_KEY: every bundle is refused)}$([ "$STAGING" = 0 ] || echo " (STAGING build)")"
rm -f "$STRINGS_OUT"
[ -z "$LEAKS" ] || fail "the executable still names build-machine paths, e.g.:
$LEAKS"
[ -d "$APP/Contents/Resources/Sys" ] && [ ! -L "$APP/Contents/Resources/Sys" ] ||
  fail "Sys is missing or a link in $APP"

# Self-contained: every library the executable loads ships with macOS.
FOREIGN="$(otool -L "$EXE" | tail -n +2 | awk '{print $1}' | grep -v -e '^/usr/lib/' -e '^/System/' || true)"
[ -z "$FOREIGN" ] || fail "Orca links libraries that don't ship with macOS:
$FOREIGN"

# What the GPL asks to travel with the binary: where the source is, and the bundled licences.
RESOURCES="$APP/Contents/Resources"
if [ "$INTERNAL" = 1 ]; then
  SOURCE_LINE="Internal test build of YG Dolphin $COMMIT from a private repository. Not for public distribution."
else
  SOURCE_LINE="Source:  ${ORCA_SOURCE_URL:-not published (a development build)}"
fi
cat >"$RESOURCES/SOURCE.txt" <<EOF
YG Dolphin $VERSION for macOS (arm64)

YG Dolphin (formerly Orca) is free software under the GNU General Public License; see COPYING and
LICENSES/.
It is based on the Dolphin emulator source (https://github.com/dolphin-emu/dolphin).

$SOURCE_LINE
Commit:  $COMMIT$DIRTY
Tag:     ${TAG:-none}

CMake settings:
$(grep -E '^(CMAKE_BUILD_TYPE|CMAKE_OSX_ARCHITECTURES|CMAKE_OSX_DEPLOYMENT_TARGET|ENABLE_[A-Z_]+|USE_[A-Z_]+|ENCODE_FRAMEDUMPS|ORCA_[A-Z_]+):' \
  "$BUILD_DIR/CMakeCache.txt" | sed 's/^/  /')
EOF
rm -rf "$RESOURCES/THIRD_PARTY"
mkdir -p "$RESOURCES/THIRD_PARTY"
cp "$ROOT/Externals/licenses.md" "$RESOURCES/THIRD_PARTY/"
for dir in "$ROOT"/Externals/*/; do
  name="$(basename "$dir")"
  case "$name" in
  # Not in the app (libiconv: macOS's own is used).
  Qt | MoltenVK | Vulkan-Headers | VulkanMemoryAllocator | FFmpeg-bin | discord-rpc | gtest | mGBA | \
    miniupnpc | rcheevos | libadrenotools | wil | OpenAL | gettext | libiconv) continue ;;
  esac
  find "$dir" -maxdepth 2 -type f \
    \( -iname 'LICENSE*' -o -iname 'LICENCE*' -o -iname 'COPYING*' -o -iname 'COPYRIGHT*' \) |
    while read -r file; do
      mkdir -p "$RESOURCES/THIRD_PARTY/$name"
      cp "$file" "$RESOURCES/THIRD_PARTY/$name/"
    done
done

mkdir -p "$OUT_DIR"
if [ "$RELEASE" = 0 ]; then
  ZIP="$OUT_DIR/yg-dolphin-$VERSION-macos-arm64-unsigned.zip"
  rm -f "$ZIP"
  ditto -c -k --keepParent "$APP" "$ZIP"
  echo "orca-package: no signing credentials; YG Dolphin.app stays unsigned"
  echo "orca-package: $ZIP"
  shasum -a 256 "$ZIP"
  exit 0
fi

KEYCHAIN_DIR="$(mktemp -d)"
KEYCHAIN="$KEYCHAIN_DIR/orca-signing.keychain-db"
KEYCHAIN_PASSWORD="$(uuidgen)"
OLD_KEYCHAINS="$(security list-keychains -d user | tr -d '"')"
cleanup() {
  security delete-keychain "$KEYCHAIN" || true
  # shellcheck disable=SC2086
  security list-keychains -d user -s $OLD_KEYCHAINS || true
  rm -rf "$KEYCHAIN_DIR"
}
trap cleanup EXIT
security create-keychain -p "$KEYCHAIN_PASSWORD" "$KEYCHAIN"
security set-keychain-settings -lut 3600 "$KEYCHAIN"
security unlock-keychain -p "$KEYCHAIN_PASSWORD" "$KEYCHAIN"
security import "$CSC_LINK" -k "$KEYCHAIN" -P "$CSC_KEY_PASSWORD" -T /usr/bin/codesign
security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k "$KEYCHAIN_PASSWORD" \
  "$KEYCHAIN" >/dev/null
# shellcheck disable=SC2086
security list-keychains -d user -s "$KEYCHAIN" $OLD_KEYCHAINS
IDENTITY="$(security find-identity -v -p codesigning "$KEYCHAIN" |
  awk '/Developer ID Application/ {print $2; exit}')"
[ -n "$IDENTITY" ] || fail "no valid Developer ID Application identity in CSC_LINK (is Apple's \
Developer ID Certification Authority G2 certificate in a keychain on this Mac?)"
codesign --force --options runtime --timestamp --entitlements "$ENTITLEMENTS" \
  --keychain "$KEYCHAIN" --sign "$IDENTITY" "$APP"
codesign --verify --strict --verbose=2 "$APP"

ZIP="$OUT_DIR/yg-dolphin-$VERSION-macos-arm64.zip"
[ "$INTERNAL" = 0 ] || ZIP="$OUT_DIR/yg-dolphin-$VERSION-macos-arm64-internal.zip"
RESULT="$KEYCHAIN_DIR/notary.json"
rm -f "$ZIP"
ditto -c -k --keepParent "$APP" "$ZIP"
xcrun notarytool submit "$ZIP" --key "$APPLE_API_KEY" --key-id "$APPLE_API_KEY_ID" \
  --issuer "$APPLE_API_ISSUER" --wait --output-format json >"$RESULT" || true
STATUS="$(plutil -extract status raw -o - "$RESULT" 2>/dev/null || echo unknown)"
if [ "$STATUS" != Accepted ]; then
  SUBMISSION="$(plutil -extract id raw -o - "$RESULT" 2>/dev/null || true)"
  [ -z "$SUBMISSION" ] || xcrun notarytool log "$SUBMISSION" --key "$APPLE_API_KEY" \
    --key-id "$APPLE_API_KEY_ID" --issuer "$APPLE_API_ISSUER" >&2 || true
  rm -f "$ZIP"
  fail "notarization: $STATUS"
fi
xcrun stapler staple "$APP"
spctl --assess --type execute --verbose "$APP"
rm -f "$ZIP"
ditto -c -k --keepParent "$APP" "$ZIP"
echo "orca-package: $ZIP"
shasum -a 256 "$ZIP"
