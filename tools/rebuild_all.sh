#!/bin/bash
# Converts every map in tools/maps.txt and your GMod Workshop maps, the prop and sound packs, then builds
# psp/EBOOT.PBP.
# Run from PowerShell:  wsl -d Ubuntu -- bash "/mnt/c/Users/goodb/Desktop/gpsp mod/tools/rebuild_all.sh"
# Close PPSSPP first: it locks EBOOT.PBP while the game runs.
set -e
cd "$(dirname "$0")"
if [ ! -d /mnt/d/SteamLibrary ]; then
  echo "D: isn't connected. In Ubuntu run:  sudo mkdir -p /mnt/d && sudo mount -t drvfs D: /mnt/d"
  exit 1
fi
while IFS= read -r line; do
  line="${line%%#*}"
  line="$(echo "$line" | sed 's/[[:space:]]*$//')"
  [ -z "$line" ] && continue
  python3 -m bspconv convert "$line"
done < maps.txt
python3 -m bspconv workshop --no-ui  # maps from your GMod Workshop downloads (none: nothing to do)
python3 -m bspconv props
python3 -m bspconv vmodels
python3 -m bspconv sounds
python3 -m bspconv ui
bash ../psp/build.sh
echo "All done."
