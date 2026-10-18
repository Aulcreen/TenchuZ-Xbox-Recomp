#!/usr/bin/env bash
# Disassemble a range of the extracted PE image (game/default.pe).
# Usage: tools/dis.sh <start_hex> <stop_hex>      e.g. tools/dis.sh 0x82410E68 0x82410F00
# Produce game/default.pe first with: python tools/xex_extract.py game/default.xex game/default.pe
set -euo pipefail
export CYGWIN=nodosfilewarning
root="$(cd "$(dirname "$0")/.." && pwd)"
"$root/third_party/rexglue-sdk/tools/binutils/powerpc-none-elf-objdump.exe" \
  -D -b binary -m powerpc:common64 -EB --adjust-vma=0x82000000 \
  --start-address="$1" --stop-address="$2" "$root/game/default.pe" | grep -E '^\s*8'
