# PSP Sandbox (and PS2GMOD)

A Garry's Mod–style sandbox for the PSP, with a port to the PS2. There are three parts:

- **`psp/`**: the engine, written from scratch in C for the PSP. It draws with the GE (the PSP's graphics chip)
  directly and has its own physics, sound mixer, NPCs, multiplayer and Lua scripting.
- **`tools/bspconv/`**: a PC-side converter (Python). It reads maps, models, textures and sounds from your own
  Garry's Mod install (and other Source games) and turns them into compact files the consoles can load.
- **`Desktop/ps2gmod/`** (a separate folder): the PS2 port. It runs the same engine code on the PS2 and reads
  the same converted data, packed onto a bootable disc image.

GMod's and Valve's maps, models, textures and sounds belong to their owners. The converter reads your local
installs, and the converted files are for your own use. Don't share builds that contain them.

## Targets

| System | RAM for games | Files used |
|---|---|---|
| PSP-2000 / 3000 / Go (primary) | ~52 MB | full detail: `name.pmap`, `props.ppak`, `npcs.pnpc`, `sounds.psnd` |
| PSP-1000 | ~24 MB | `*.low.*`: half-size textures, coarser geometry, 16 kHz sound |
| PS2 | 32 MB | full detail when it fits, otherwise the `.low` version of that map |

The game measures free memory at startup and picks the right files. If a full-detail map doesn't fit when you
load it, it falls back to the map's low-detail version.

## Setup (one time, already done on this PC)

1. WSL with Ubuntu, then in Ubuntu:
   `sudo apt-get install -y build-essential cmake pkgconf libusb-0.1-4 fakeroot python3-numpy python3-pil`
2. The PSP SDK (pspdev) in `~/pspdev`, and for the PS2 build the PS2 SDK (ps2dev) in `~/ps2dev`.
3. GMod is on D:, which Ubuntu doesn't see automatically. After each restart of Ubuntu, run:
   `sudo mkdir -p /mnt/d && sudo mount -t drvfs D: /mnt/d`

## Building

Close PPSSPP first (it locks the EBOOT). From PowerShell:

```
wsl -d Ubuntu -- bash "/mnt/c/Users/goodb/Desktop/gpsp mod/tools/rebuild_all.sh"
```

This converts every map in `tools/maps.txt`, builds the packs in `psp/data/`, and builds `psp/EBOOT.PBP`.
You can also run the steps on their own:

| Step | Command | Input |
|---|---|---|
| One map | `tools/bspconv.sh convert <map.bsp>` | a `.bsp` |
| Props and weapon models | `tools/bspconv.sh props` | `tools/props.txt` |
| NPCs and ragdolls | `tools/bspconv.sh npcs` | `tools/npcs.txt` |
| Sounds | `tools/bspconv.sh sounds` | `tools/sounds.txt` |
| Fonts, spawn icons, map thumbnails | `tools/bspconv.sh ui` | the three lists above |
| PSP EBOOT | `psp/build.sh` | `psp/src/` |
| PS2 disc image | `bash /mnt/c/Users/goodb/Desktop/ps2gmod/build.sh` | `ps2gmod/` and `psp/data/` |

The PS2 build compiles the ELF, packs `psp/data` into `DATA.PAK`, and writes a bootable `build/ps2mod.iso`.

Maps from other Source games convert too: HL2, HL2 Deathmatch, Counter-Strike: Source, Day of Defeat: Source and
Half-Life Deathmatch: Source. Their own textures and models are found next to the map. `maps.txt` lists 38 maps.

## Running

- **PPSSPP:** File > Load, and pick `psp/EBOOT.PBP`. Frame rates in the emulator don't reflect real hardware.
- **Real PSP** (needs custom firmware for homebrew): copy `EBOOT.PBP` and the `data` folder to
  `PSP/GAME/PSPSandbox/` on the memory stick.
- **PCSX2:** boot `ps2gmod/build/ps2mod.iso` (full BIOS boot works). The map's geometry is transformed on the PS2's VU1
  coprocessor; props, NPCs, anything near the camera and everything while the flashlight is on still go through the
  main CPU. If the world looks wrong, turn off Options > VU1 renderer.

## Controls

| Action | PSP | PS2 (DualShock 2) |
|---|---|---|
| Walk (swim in deep water) | Analog stick | Left stick |
| Look | Triangle / Cross / Square / Circle | Right stick |
| Fire / primary | R | R1 or R2 |
| Jump (noclip: fly up) | L | X, or L1 / L2 |
| Switch weapon | D-pad Left | D-pad Left |
| Use (doors, buttons) | D-pad Right (tap) | D-pad Right |
| Flashlight | D-pad Right (hold) | Square |
| Undo the last spawn | D-pad Up | D-pad Up |
| Crouch on/off (noclip: fly down) | D-pad Down | D-pad Down |
| Spawn menu (L/R switch panel, X selects, O closes) | Select (tap) | Select (tap) |
| Noclip on/off | Select (hold) | Select (hold) |
| Pause menu | Start | Start |

While you carry a prop, jumping, crouching, undo and switching weapons are off. The D-pad turns, pushes and
pulls the prop instead.

## Weapons

| Weapon | Controls |
|---|---|
| Hands | R picks up something light (up to 35 kg, close by) or drops it. L throws it. |
| Physics Gun | Hold R to grab anything, at any distance. L freezes it in place. D-pad turns it and moves it nearer or further. |
| Gravity Gun | PSP: R pulls in a prop up to 250 kg from up to 1000 units and holds it, R again drops it, L launches it. PS2: L1 picks up and drops, R1 punts. On both, picking up something too heavy within 250 units punts it instead. |
| Tool Gun | R uses the tool picked in the spawn menu's Tools panel. |
| Crowbar, Pistol, .357, SMG, Shotgun | R fires. They reload when empty. |

Every weapon is shown with GMod's first-person model and the citizen's arms, posed in its idle animation.

Tool gun tools (the Tools list scrolls):

| Group | Tool | What it does |
|---|---|---|
| Construction | Balloon | Ties up to 4 balloons that lift the prop. |
| | Duplicator | R on a prop copies it, R on the ground pastes the copy. |
| | No-Collide | The prop passes through other props (it still hits the world). R again undoes it. |
| | Remover | Deletes the prop, NPC or ragdoll you aim at. |
| | Weight | Steps the prop's mass through 1, 10, 50, 250 and 1000 kg. |
| Constraints | Ball Socket | Pick a prop, then what to attach it to: they share that point but swivel freely. |
| | Elastic | Pick two points: a stretchy band that pulls them together. |
| | Rope | Pick two points: they can't move further apart than the rope. |
| | Weld | Pick a prop, then a second prop (or the world): joined rigidly. |
| Mechanics | Explosive | Blows the prop up: nearby props fly, NPCs and you get hurt. |
| | Hoverball | Holds the prop at the height where you put it. |
| | Thruster | Pushes the prop the way you shot it in. |
| Render | Colour | Paints the prop. |

Use (D-pad Right) on a prop switches its thrusters and hoverballs on and off.

## Spawn menu

- **Props:** 137 in 10 spawnlists: Construction, Junk, Furniture, Misc, Vehicles, Shapes, Street, Combine, Food, Lab.
- **NPCs:** 24, including zombies, antlions, headcrabs, an antlion guard, Combine, rebels and HL2's main characters.
  Zombies and antlions chase and attack you; the rest wander and flee.
- **Ragdolls:** the same 24 characters, spawned limp.
- Up to 40 spawned props per player, plus the map's own physics props.

## Maps

Besides the converted maps, the list has **gm_construct_13**: gm_construct at night, in thick dark fog. A black
figure turns up in the distance now and then, at the edge of your view. Look straight at it, or walk towards it,
and it's gone. It reuses gm_construct's files, so it takes no extra space.

## Multiplayer

Up to 4 players. The host runs the physics, NPCs and doors. Pick Multiplayer > Host Game or Join Game.

- **PSP:** ad-hoc wifi between PSPs (or PPSSPP instances with ad-hoc networking set up).
- **PS2:** over a network adapter on the same local network (a fat PS2 with the adapter, a slim PS2, or PCSX2).
  The PS2 gets its address from your router by DHCP and finds hosts automatically. In PCSX2, turn the
  adapter on in Settings > Network & HDD, using your real network card (PCAP or TAP mode).

The PSP and PS2 versions can't play together.

## Lua addons

Every `.lua` file in `psp/data/lua/autorun/` runs when a map loads, and `psp/data/lua/maps/<map>.lua` runs on that
map only. The API is a small GMod-flavoured subset: `hook.Add` (`Think`, `PlayerUse`, `HUDPaint`),
`timer.Simple`, `ents.CreateProp/Remove/GetPos/SetPos/SetVelocity/SetColor/Freeze`,
`player.GetPos/GetYaw/GetAimEntity`, `draw.Text`, `print`. See `autorun/example.lua`: Use on a prop launches it.

## What gets converted

- **Map geometry:** brush faces and displacements become indexed triangles. Faces are cut on a world-aligned
  grid only where their lightmap has detail. Vertices are packed into 16 bytes (16-bit UV and position).
- **Lighting:** Source's baked lightmaps become vertex colours (drawn with the GE's 2x colour doubling).
  Props are lit by the map's per-area ambient light plus the sun, with a shadow test toward the sun.
- **Textures:** VTF base textures are cut to 128px (or 64px), reduced to 256-colour paletted (T8), swizzled,
  and given mip levels. On the PSP the most-used ones are copied into the free ~1.2 MB of VRAM at load.
- **Visibility:** batches of triangles per 1024-unit cell and material, plus static props, each get a bitset of
  what every BSP cluster can see.
- **Collision:** map brushes (with bevels), displacement triangles as prism brushes, static brush entities,
  and boxes for solid static props.
- **Entities:** sliding and swinging doors, buttons, fans and triggers, linked by the map's I/O outputs
  ("button OnPressed -> door Open"). Static props and the map's physics props come with their models.
- **Props:** MDL/VVD/VTX models at a LOD that fits a triangle budget (simplified when there's no low LOD), with
  real masses, surface types and convex collision hulls from `.phy`.
- **Weapons:** GMod's `c_` models with `c_arms_citizen` bone-merged onto them, posed at the first frame of the
  idle animation and baked into one mesh. The physics gun is the gravity gun's model with its colours turned blue.
- **NPCs:** rigid-skinned models with walk, run, attack and death animations baked into per-bone matrices.
  Eyeballs come from the full-detail model (Valve's low LODs drop them), with the iris projected the way
  Source's eye shader does it.
- **Sound:** WAV (PCM and MS-ADPCM), mono, at 44.1 kHz (16 kHz on the PSP-1000), mixed in 16 voices with 3D panning.

## Physics and collision

- **Props:** boxes, spheres, cylinders or convex hulls, handled as surface sample points. Contacts use sequential
  impulses with friction, and there are welds, ropes, buoyancy in water, sleeping and freezing.
- **Fast props:** fast-moving props (thrown, punted, or dragged with the physics gun) sweep their path, so they
  stop at thin walls instead of passing through.
- **Player and NPCs:** the player and NPCs collide with props' real rotated shapes, so a tilted plank or door
  blocks only where it is. If a prop is pushed into you, you can walk out of it.
- **World traces:** these follow the BSP tree along the path, so long traces (tool gun, spawning, shadows)
  never miss walls.
- **Ragdolls:** verlet ragdolls that collide with the world and props. The physics gun drags and freezes them.

## Roadmap

- [x] PSP renderer, camera, text overlay, memory tier detection
- [x] BSP / VPK / VMT / VTF readers; full and low detail tiers
- [x] Collision and Source-style movement (jump, crouch, stairs, slopes, swimming, noclip)
- [x] Props with physics; physics gun and tool gun (Remover, Colour, Balloon, Duplicator, Weld, Rope); spawn menu, undo
- [x] Rolling shapes, convex hulls, welds and ropes, water
- [x] Doors, buttons, fans, triggers and outputs; static props; map physics props; maps from other Source games
- [x] Skybox textures and light styles (flicker patterns, switchable lights)
- [x] Lua scripting layer
- [x] NPCs with baked skeletal animation; health, damage, death and respawn; ragdolls
- [x] Weapons: crowbar, pistol, .357, SMG, shotgun; gravity gun; hands
- [x] First-person weapon models with arms
- [x] Multiplayer (PSP ad-hoc, PS2 LAN), up to 4, host-authoritative
- [x] Flashlight; the haunted gm_construct_13
- [x] PS2 port: boots from disc, DualShock 2, network adapter
- [x] Tool gun: ball socket, elastic, no-collide, weight, thruster, hoverball, explosive
- [x] PS2: map geometry on the VU1 coprocessor
- [ ] PS2 speed: props and NPCs on VU1 too, and clipping on VU1
- [ ] Animated first-person weapons; NPC weapons; PvP damage; synced ragdolls
