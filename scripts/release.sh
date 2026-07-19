#!/usr/bin/env bash
#
# scripts/release.sh — build, sign, notarize, and package a Developer-ID
# release of Comic Chat for macOS.
#
# One-time setup (Tim runs this once, on his machine, before the first
# release):
#
#   xcrun notarytool store-credentials comicchat-notary \
#       --apple-id "<your Apple ID email>" \
#       --team-id "<your 10-char Team ID>" \
#       --password "<app-specific password, from appleid.apple.com>"
#
# This stores credentials in the keychain under the profile name
# "comicchat-notary" (override with NOTARY_PROFILE). After that, this script
# needs no secrets on the command line or in the environment.
#
# Usage:
#   scripts/release.sh              # full chain: archive, export, notarize, staple, dmg
#   NOTARY_PROFILE=my-profile scripts/release.sh
#
# Requirements:
#   - A "Developer ID Application" signing identity installed in your
#     keychain. Without one, this script still runs the archive+export
#     steps (ad-hoc signed) so the build itself can be verified, then stops
#     before notarization with a clear message.

set -euo pipefail

# ---------------------------------------------------------------------------
# Setup
# ---------------------------------------------------------------------------

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

PROJECT="$REPO_ROOT/macos/ComicChat/ComicChat.xcodeproj"
SCHEME="ComicChat"
BUILD_DIR="$REPO_ROOT/build"
ARCHIVE_PATH="$BUILD_DIR/ComicChat.xcarchive"
EXPORT_DIR="$BUILD_DIR/export"
APP_NAME="ComicChat.app"
NOTARY_PROFILE="${NOTARY_PROFILE:-comicchat-notary}"

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m warning:\033[0m %s\n' "$*" >&2; }
fail() { printf '\033[1;31m error:\033[0m %s\n' "$*" >&2; exit 1; }

mkdir -p "$BUILD_DIR"

# ---------------------------------------------------------------------------
# Determine signing identity
# ---------------------------------------------------------------------------

DEVELOPER_ID_IDENTITY="$(security find-identity -v -p codesigning 2>/dev/null \
    | grep 'Developer ID Application' \
    | head -1 \
    | sed -E 's/^[[:space:]]*[0-9]+\) [A-F0-9]+ "(.*)"$/\1/' || true)"

if [[ -z "$DEVELOPER_ID_IDENTITY" ]]; then
    warn "No \"Developer ID Application\" signing identity found in your keychain."
    warn "Falling back to ad-hoc signing (CODE_SIGN_IDENTITY=\"-\") so the"
    warn "archive and export steps can still be verified locally."
    warn ""
    warn "To produce a distributable, notarizable build, install your"
    warn "Developer ID Application certificate (Xcode > Settings > Accounts >"
    warn "Manage Certificates, or download it from developer.apple.com) and"
    warn "re-run this script."
    CODE_SIGN_IDENTITY="-"
    CAN_NOTARIZE=0
else
    log "Using signing identity: $DEVELOPER_ID_IDENTITY"
    CODE_SIGN_IDENTITY="$DEVELOPER_ID_IDENTITY"
    CAN_NOTARIZE=1
fi

# ---------------------------------------------------------------------------
# 1. Archive
# ---------------------------------------------------------------------------

log "Archiving $SCHEME (Release)..."
rm -rf "$ARCHIVE_PATH"

xcodebuild \
    -project "$PROJECT" \
    -scheme "$SCHEME" \
    -configuration Release \
    archive \
    -archivePath "$ARCHIVE_PATH" \
    CODE_SIGN_IDENTITY="$CODE_SIGN_IDENTITY" \
    OTHER_CODE_SIGN_FLAGS="--timestamp" \
    ENABLE_HARDENED_RUNTIME=YES \
    || fail "Archive failed. See build output above."

[[ -d "$ARCHIVE_PATH" ]] || fail "Archive step reported success but $ARCHIVE_PATH is missing."
log "Archive created at $ARCHIVE_PATH"

# ---------------------------------------------------------------------------
# 2. Export the .app from the archive
# ---------------------------------------------------------------------------

log "Exporting .app from archive..."
rm -rf "$EXPORT_DIR"
mkdir -p "$EXPORT_DIR"

EXPORT_METHOD="developer-id"
if [[ "$CAN_NOTARIZE" -eq 0 ]]; then
    EXPORT_METHOD="mac-application"
fi

EXPORT_PLIST="$BUILD_DIR/ExportOptions.plist"
cat > "$EXPORT_PLIST" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>method</key>
    <string>$EXPORT_METHOD</string>
    <key>signingStyle</key>
    <string>automatic</string>
</dict>
</plist>
PLIST

xcodebuild \
    -exportArchive \
    -archivePath "$ARCHIVE_PATH" \
    -exportPath "$EXPORT_DIR" \
    -exportOptionsPlist "$EXPORT_PLIST" \
    || fail "Export failed. See build output above."

APP_PATH="$EXPORT_DIR/$APP_NAME"
[[ -d "$APP_PATH" ]] || fail "Export step reported success but $APP_PATH is missing."
log "Exported app at $APP_PATH"

# Read the version from the built app's Info.plist (source of truth for the
# DMG filename below), via PlistBuddy on the exported product.
VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$APP_PATH/Contents/Info.plist" 2>/dev/null || true)"
if [[ -z "$VERSION" ]]; then
    warn "Could not read CFBundleShortVersionString from the exported app; defaulting to 0.0.0."
    VERSION="0.0.0"
fi
log "Release version: $VERSION"

if [[ "$CAN_NOTARIZE" -eq 0 ]]; then
    log "Build and export verified successfully (ad-hoc signed)."
    warn "Stopping before notarization: no Developer ID Application certificate."
    warn "Install your Developer ID cert and re-run to notarize, staple, and produce the DMG."
    exit 0
fi

# ---------------------------------------------------------------------------
# 3. Notarize
# ---------------------------------------------------------------------------
#
# NOTE: notarization is NOT run automatically by this script as configured
# for the developer running the automated build/export check above — it
# requires Tim's Apple ID credentials via the "comicchat-notary" keychain
# profile (see the one-time setup comment at the top of this file). Run this
# script as Tim, with the profile configured, to actually notarize.

ZIP_PATH="$BUILD_DIR/ComicChat-notarize.zip"
log "Zipping app for notarization submission..."
rm -f "$ZIP_PATH"
ditto -c -k --keepParent "$APP_PATH" "$ZIP_PATH" \
    || fail "Failed to zip $APP_PATH for notarization."

log "Submitting to Apple notary service (profile: $NOTARY_PROFILE)..."
xcrun notarytool submit "$ZIP_PATH" \
    --keychain-profile "$NOTARY_PROFILE" \
    --wait \
    || fail "Notarization submission failed. Run 'xcrun notarytool log <submission-id> --keychain-profile \"$NOTARY_PROFILE\"' for details."

# ---------------------------------------------------------------------------
# 4. Staple
# ---------------------------------------------------------------------------

log "Stapling notarization ticket..."
xcrun stapler staple "$APP_PATH" \
    || fail "Stapling failed."

# ---------------------------------------------------------------------------
# 5. Build DMG
# ---------------------------------------------------------------------------

DMG_PATH="$BUILD_DIR/ComicChat-$VERSION.dmg"
log "Building DMG at $DMG_PATH..."
rm -f "$DMG_PATH"

hdiutil create \
    -volname "Comic Chat" \
    -srcfolder "$APP_PATH" \
    -ov \
    -format UDZO \
    "$DMG_PATH" \
    || fail "DMG creation failed."

log "Done. Notarized, stapled release DMG: $DMG_PATH"
