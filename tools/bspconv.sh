#!/bin/bash
# Runs the converter inside WSL: tools/bspconv.sh info /mnt/d/.../gm_construct.bsp
cd "$(dirname "$0")" && exec python3 -m bspconv "$@"
