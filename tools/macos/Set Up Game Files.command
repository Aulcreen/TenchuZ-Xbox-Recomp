#!/bin/bash
# Tenchu Z for macOS: extracts your own Tenchu Z disc image (.iso) to
# ~/Library/Application Support/TenchuZ/game, where Tenchu Z.app reads it.
# Double-click it on the Tenchu Z disk image.

set -u
here="$(cd "$(dirname "$0")" && pwd)"
dest="$HOME/Library/Application Support/TenchuZ/game"

iso="$(osascript -e 'POSIX path of (choose file with prompt "Choose your Tenchu Z disc image (.iso)" of type {"iso", "public.iso-image"})' 2>/dev/null)" || exit 0
[ -n "$iso" ] || exit 0

if ! /usr/bin/python3 -c "" 2>/dev/null; then
  osascript -e 'display alert "Python 3 is needed" message "macOS will offer to install the Command Line Developer Tools. Install them, then open Set Up Game Files again."'
  xcode-select --install 2>/dev/null
  exit 1
fi

echo "Extracting $iso"
echo "to $dest"
mkdir -p "$dest"
if /usr/bin/python3 "$here/.setup/xiso.py" extract "$iso" "$dest" && [ -f "$dest/default.xex" ]; then
  osascript -e 'display dialog "Tenchu Z is ready. Open Tenchu Z from Applications." buttons {"OK"} default button 1 with title "Tenchu Z"'
else
  osascript -e 'display alert "That disc image could not be read" message "Choose a Tenchu Z (Xbox 360) disc image."'
  exit 1
fi
