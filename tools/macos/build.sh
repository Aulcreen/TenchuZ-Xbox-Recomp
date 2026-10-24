#!/bin/bash
# Builds Tenchu Z for macOS on Apple Silicon and packages it as
# dist/TenchuZ-mac.dmg: Tenchu Z.app, plus "Set Up Game Files", which extracts
# a player's own disc image. The build needs the game files too (codegen
# recompiles game/default.xex), and installs them for this Mac's user.
#
#   tools/macos/build.sh [path/to/TenchuZ.iso]
#
# Needs: Xcode Command Line Tools (xcode-select --install). The other tools
# (cmake, ninja, the Vulkan loader, MoltenVK) come from Homebrew when it's
# installed, and from conda-forge into third_party/mac-deps otherwise (with
# micromamba, downloaded if it's missing; no password needed).
# The Steamworks files for System Link over Steam are in the repository
# (third_party/steamworks/sdk); the disc image is the player's own.
# JOBS=<n> sets the parallel compile jobs (default 4, for 8 GB of memory).

set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$root"

sdk_tag="nightly-20261002-bd833a2a"  # the SDK commit Windows builds with (bd833a2)
sdk_zip="rexglue-sdk-0.10.0.24-dev.gbd833a2-mac-arm64.zip"
sdk_dir="$root/third_party/rexglue-bin/mac-arm64"
preset="mac-arm64-release"
build_dir="$root/out/build/$preset"
support="$HOME/Library/Application Support/TenchuZ"

step() { printf '\n== %s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; exit 1; }

[ "$(uname -s)" = Darwin ] || fail "run this on a Mac"
[ "$(uname -m)" = arm64 ] || fail "this builds for Apple Silicon (arm64)"

step "Tools"
if ! xcode-select -p >/dev/null 2>&1; then
  xcode-select --install || true
  fail "install the Command Line Developer Tools (a window just opened), then run this again"
fi
if ! command -v brew >/dev/null 2>&1; then
  for b in /opt/homebrew/bin/brew /usr/local/bin/brew; do [ -x "$b" ] && eval "$("$b" shellenv)"; done
fi
vulkan_args=()
if command -v brew >/dev/null 2>&1; then
  for formula in cmake ninja vulkan-loader molten-vk; do
    brew list --formula "$formula" >/dev/null 2>&1 || brew install "$formula"
  done
else
  # No Homebrew: the same tools from conda-forge, in a folder of their own.
  deps="$root/third_party/mac-deps"
  export MAMBA_ROOT_PREFIX="$root/third_party/mamba-root"
  mamba="$(command -v micromamba || true)"
  if [ -z "$mamba" ]; then
    mamba="$root/third_party/micromamba/micromamba"
    if [ ! -x "$mamba" ]; then
      mkdir -p "$(dirname "$mamba")"
      curl -fL --progress-bar -o "$mamba" \
        "https://github.com/mamba-org/micromamba-releases/releases/latest/download/micromamba-osx-arm64"
      chmod +x "$mamba"
    fi
  fi
  if [ ! -f "$deps/lib/libMoltenVK.dylib" ] || [ ! -x "$deps/bin/cmake" ] || [ ! -x "$deps/bin/ninja" ]; then
    "$mamba" create -y -q -p "$deps" -c conda-forge cmake ninja libvulkan-loader moltenvk
  fi
  export PATH="$deps/bin:$PATH"
  vulkan_args=(-DTENCHUZ_VULKAN_LOADER="$deps/lib/libvulkan.1.dylib"
               -DTENCHUZ_MOLTENVK="$deps/lib/libMoltenVK.dylib")
  echo "Homebrew not found: using conda-forge tools in $deps"
fi
command -v python3 >/dev/null 2>&1 || fail "python3 is missing (it comes with the Command Line Tools)"

step "ReXGlue SDK ($sdk_tag)"
if [ ! -x "$sdk_dir/bin/rexglue" ]; then
  mkdir -p "$root/third_party/rexglue-bin"
  tmp="$(mktemp -d)"
  curl -fL --progress-bar -o "$tmp/$sdk_zip" \
    "https://github.com/rexglue/rexglue-sdk/releases/download/$sdk_tag/$sdk_zip"
  unzip -q -o "$tmp/$sdk_zip" -d "$root/third_party/rexglue-bin"
  rm -rf "$tmp"
fi
echo "$sdk_dir"

step "Game files"
if [ ! -f "$root/game/default.xex" ]; then
  [ $# -ge 1 ] || fail "no game/default.xex yet: pass your Tenchu Z disc image, tools/macos/build.sh path/to/TenchuZ.iso"
  python3 "$root/tools/xiso.py" extract "$1" "$root/game"
fi
[ -f "$root/game/default.xex" ] || fail "game/default.xex is missing after extraction"
echo "$root/game"

if [ -f "$root/third_party/steamworks/sdk/public/steam/steam_api.h" ] || \
   [ -f "$root/third_party/steamworks/public/steam/steam_api.h" ]; then
  echo "Steamworks SDK found: System Link over Steam is on"
else
  echo "No Steamworks SDK in third_party/steamworks: System Link works between instances on this Mac only"
fi

step "Build ($preset)"
# The first configure needs codegen's source list (later builds re-run codegen
# themselves).
[ -f "$root/generated/default/sources.cmake" ] || "$sdk_dir/bin/rexglue" codegen tenchuz_manifest.toml
cmake --preset "$preset" -DCMAKE_PREFIX_PATH="$sdk_dir" -DCMAKE_OSX_DEPLOYMENT_TARGET=13.3 \
  ${vulkan_args[@]+"${vulkan_args[@]}"}
cmake --build --preset "$preset" --parallel "${JOBS:-4}"
app="$build_dir/tenchuz.app"
[ -d "$app" ] || fail "the build made no $app"

step "Package"
stage="$root/dist/mac/Tenchu Z"
rm -rf "$root/dist/mac"
mkdir -p "$stage/.setup"
ditto "$app" "$stage/Tenchu Z.app"
macos="$stage/Tenchu Z.app/Contents/MacOS"
# conda-forge's MoltenVK links @rpath/libc++.1.dylib, which isn't in the
# bundle: use the system's (a no-op for Homebrew's, which links it already).
while IFS= read -r -d '' lib; do
  if otool -L "$lib" | grep -q '@rpath/libc++\.1\.dylib'; then
    install_name_tool -change @rpath/libc++.1.dylib /usr/lib/libc++.1.dylib "$lib" 2>/dev/null
  fi
done < <(find "$macos" -name '*.dylib' -type f -print0)
# Everything in the bundle needs a signature on Apple Silicon (ad-hoc here).
# codesign --deep refuses the bundle (the Vulkan ICD folder under
# Contents/MacOS isn't code), so sign each library on its own. The executable
# keeps the linker's ad-hoc signature.
while IFS= read -r -d '' lib; do
  codesign --force --sign - "$lib"
done < <(find "$macos" -name '*.dylib' -type f -print0)
codesign -d "$macos/tenchuz" >/dev/null 2>&1 || fail "tenchuz has no signature"
cp "$root/tools/macos/Set Up Game Files.command" "$stage/"
chmod +x "$stage/Set Up Game Files.command"
cp "$root/tools/xiso.py" "$stage/.setup/"
ln -s /Applications "$stage/Applications"
dmg="$root/dist/TenchuZ-mac.dmg"
rm -f "$dmg"
hdiutil create -quiet -volname "Tenchu Z" -srcfolder "$stage" -ov -format UDZO "$dmg"
echo "$dmg"

step "Game files for this Mac"
if [ ! -f "$support/game/default.xex" ]; then
  mkdir -p "$support"
  ditto "$root/game" "$support/game"
fi
echo "$support/game"

cat <<EOF

Done. Open dist/TenchuZ-mac.dmg and drag Tenchu Z to Applications.
This Mac already has the game files; on another Mac, run "Set Up Game Files"
from the disk image first. Settings: $support/tenchuz.toml, logs: $support/logs.
EOF
