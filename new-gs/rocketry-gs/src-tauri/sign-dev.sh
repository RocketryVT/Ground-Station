#!/usr/bin/env bash
# Re-sign the dev binary with camera/mic entitlements.
#
# `tauri dev` builds `target/debug/rocketry-gs` and ld only adhoc/linker-signs
# it with no entitlements, so WKWebView refuses the page camera/mic access
# ("Could not create a 'com.apple.webkit.camera' sandbox extension" ->
# NotAllowedError: No AVVideoCaptureSource device). We adhoc-sign it again WITH
# the entitlements. Run this AFTER `cargo build` and BEFORE `tauri dev` runs the
# binary (see the `app:dev` npm script) -- because the binary is already
# up-to-date, tauri's `cargo run` won't relink and our signature is preserved.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENTITLEMENTS="$SCRIPT_DIR/Entitlements.dev.plist"
BIN="${1:-$SCRIPT_DIR/target/debug/rocketry-gs}"

if [[ ! -f "$BIN" ]]; then
  echo "sign-dev: binary not found at $BIN (build it first)" >&2
  exit 1
fi

codesign --force --sign - --entitlements "$ENTITLEMENTS" "$BIN"
echo "sign-dev: signed $BIN with camera/mic entitlements"
