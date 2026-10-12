#!/bin/bash
# Builds EBOOT.PBP inside WSL. Usage: psp/build.sh [clean]
set -e
export PSPDEV="${PSPDEV:-$HOME/pspdev}"
export PATH="$PATH:$PSPDEV/bin"
cd "$(dirname "$0")"
if [ "$1" = "clean" ]; then
  make clean
  exit 0
fi
make -j"$(nproc)"
