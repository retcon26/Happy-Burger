#!/bin/bash
# Maps crash addresses from PPSSPP's crash screen to source lines: psp/addr2line.sh 08805aac 0880403c ...
export PATH="$PATH:${PSPDEV:-$HOME/pspdev}/bin"
cd "$(dirname "$0")"
for a in "$@"; do
  psp-addr2line -f -C -e sandbox.elf "0x$a"
done
