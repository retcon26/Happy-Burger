# Retcon's Mod: every version

Retcon's Mod (formerly PSP Sandbox) for the PSP, newest first. The version shows in the main menu's corner. The
README's table has only the latest three; this page has them all.

**Multiplayer** needs everyone on a version with the same network protocol (the "Plays with" column). Games of
another protocol don't show up in Join Game or Online Servers.

| Version | Date | Plays with | In short |
|---|---|---|---|
| [1.7.1](#171) | 2026-10-15 | 1.7.0 – 1.7.1 | Server list; the Retcon's Mod name and logo |
| [1.7.0](#170) | 2026-10-15 | 1.7.0 – 1.7.1 | Saves; Wi-Fi multiplayer and dedicated servers |
| [1.6.2](#162) | 2026-10-15 | 1.6.1 – 1.6.2 | Workshop page lists maps by game |
| [1.6.1](#161) | 2026-10-15 | 1.6.1 – 1.6.2 | Wheel, Winch, Hydraulic, Lamp, Light, Button and Emitter; Workshop menu |
| [1.6.0](#160) | 2026-10-15 | 1.6.0 | TF2's RED and BLU teams; Workshop characters |
| [1.5.3](#153) | 2026-10-15 | 1.5.3 | Doors, mingebags, profiles, achievements |
| [1.5.2](#152) | 2026-10-15 | 1.5.0 – 1.5.2 | Spawn menu grouped by game, Recent list |
| [1.5.1](#151) | 2026-10-14 | 1.5.0 – 1.5.2 | Voice chat |
| [1.5.0](#150) | 2026-10-14 | 1.5.0 – 1.5.2 | Drivable vehicles, seats, bullet holes |
| [1.4.0](#140) | 2026-10-14 | 1.3.0 – 1.4.0 | Team Fortress 2 |
| [1.3.0](#130) | 2026-10-14 | 1.3.0 – 1.4.0 | Animated weapons, PvP damage, synced ragdolls |
| [1.2.0](#120) | 2026-10-11 | 1.2.0 | NPC guns, sawblade cuts, Zombine |
| [1.1.0](#110) | before 2026-10-11 | not recorded | Camera tool |
| [1.0.1](#101) | before 2026-10-11 | not recorded | Ragdoll crash fixes |
| [1.0.0](#100) | before 2026-10-11 | not recorded | Version label, new options, more ragdolls |

## 1.7.1

- **Server list:** Online Servers shows every public dedicated server on a server list, without typing their
  addresses. **R** sets the list's address (kept in `data/servers.cfg`). A list only offers servers of your own
  game version, and each server is asked about its game directly, so stopped servers never show.
- **PSP Sandbox becomes Retcon's Mod:** the XMB name and icon, the loading screen, the menus and the credits.
- **Its own logo:** an orange R badge with "Retcon's MOD", drawn by the converter (`tools/bspconv/brand.py`)
  instead of Garry's Mod's logo. No Garry's Mod branding is left on screen ("A physics sandbox for the PSP"; the
  physics gun's default colour is just "Blue").
- **Achievement:** Yes, I'm the real Retcon: play on a dedicated server with your name set to Retcon.
- Server ([psp-sandbox-server](https://github.com/retcon26/psp-sandbox-server) 1.1.0): list mode, and game
  servers sign up to a list with `list` in `server.cfg`.

## 1.7.0

- **Saves (Pause menu > Saves):** everything spawned is saved to `data/saves/` on the memory stick: props with
  their colour, weight, frozen and no-collide, welds, ropes, ball sockets, elastics, sliders, wheels, winches,
  hydraulics, thrusters, hoverballs, balloons, lamps, lights, buttons and emitters. X loads a save on any map,
  where you're looking and turned the way you face; Square loads it where it was built; Triangle deletes it.
  One Undo takes a whole loaded build away.
- **Wi-Fi multiplayer:** Multiplayer > **Host on Wi-Fi** hosts through your access point; **Online Servers**
  lists the games on your Wi-Fi and servers you add by address (Triangle adds, Square removes).
- **Dedicated servers:** [psp-sandbox-server](https://github.com/retcon26/psp-sandbox-server) 1.0.0 runs the
  game on a Linux PC or a rented server with nobody playing on it, for up to 3 PSPs, with a console (status, say,
  kick, map, save, load, cleanup).
- Doors, buttons and triggers react to every player, not just the host (TF2's spawn doors open for everyone).
- **Achievements:** Save Point, Deja Vu, Online, On the Air.
- Network protocol 9.

## 1.6.2

- **Workshop page:** the Maps tab lists maps by game. X opens a game to show its maps; Triangle switches a whole
  game on or off; each game shows how many of its maps are on.
- Half-Life Deathmatch: Source's maps get their own game in Workshop and Start New Game.

## 1.6.1

- **More tools:** Wheel (a motorised wheel; Use switches its motor), Winch (a rope that reels in and lets out),
  Hydraulic (a ram that pushes out and pulls back), Lamp (a spotlight), Light (a coloured point light), Button
  (switches the contraption it's linked to) and Emitter (sparks, smoke or fire).
- Use switches everything on a prop; a seat or a button switches the whole contraption.
- **Main menu > Workshop:** maps, Lua addons and custom characters switched on and off (`data/workshop.cfg`).
- Network protocol 8.

## 1.6.0

- **TF2 teams:** the nine mercenaries in RED and BLU fight the other team and leave players alone. Each carries
  its class's gun with TF2's firing sounds (rockets and pipes burst where they land) and speaks its own TF2 voice
  lines.
- Characters from your GMod Workshop downloads can be added (listed under Workshop in the spawn menu).
- Network protocol 7.

## 1.5.3

- **Doors:** TF2's spawn doors open and close, with their models; props on a door move with it; HL2's model doors
  swing open with Use (double doors together, locked ones stay shut).
- **Mingebags:** an NPC that circles and bunny-hops round players, whacks with a crowbar, throws props and chats
  rubbish.
- **Multiplayer > Profile:** your name (on-screen keyboard), player model, player colour and physics gun colour;
  name tags over players; everyone sees each other's physics gun beams.
- **Achievements:** playtime, playing with others, playing on a real PSP (by model), props spawned, mingebags,
  maps played.
- Network protocol 6.

## 1.5.2

- **Spawn menu:** the other games' spawnlists grouped under a header per game, opened one at a time; a Recent
  list of the last 12 things spawned; a badge on props you can drive or sit in; all four icon columns fit.

## 1.5.1

- **Voice chat:** hold D-pad Up to talk in multiplayer, with the PSP-3000's built-in mic or a headset (a quick
  tap still undoes). The host passes speech on to everyone.

## 1.5.0

- **Drivable Jeep and Airboat:** the Jeep on four sprung wheels with a handbrake, the Airboat on floats that sit on
  water. Use gets in and out; the view follows from behind.
- **Seats** for riding your contraptions: R switches every thruster and hoverball joined to the seat.
- **Bullet holes** in walls and props; blood and holes use HL2's decal textures, with no more flicker.
- Network protocol 5.

## 1.4.0

- **Team Fortress 2:** 22 maps, the nine mercenaries as NPCs, ragdolls and player models, and about 450 of its
  props.

## 1.3.0

- **Animated first-person weapons:** draw, idle, fire and reload animations, with arms.
- **PvP damage:** guns and the crowbar hurt other players in multiplayer (not with God mode on).
- **Synced ragdolls:** everyone sees the same ragdolls; the host simulates them.
- PS2: props, NPCs and ragdolls on VU1 (about twice the frame rate).
- Network protocol 4.

## 1.2.0

- NPCs hold their guns properly; sawblades cut zombies in half; the Zombine and Zombie Torso as NPCs.
- Network protocol 3 (the first version published here; 1.0.0 to 1.1.0 came before the repository).

## 1.1.0

- **Camera tool:** place cameras and look through them.

## 1.0.1

- Fixes for ragdoll crashes on real PSPs.

## 1.0.0

- The version label in the main menu's corner; Unlimited ammo and HEV suit options; Counter-Strike: Source and
  Half-Life: Source ragdolls.

## Dedicated server ([psp-sandbox-server](https://github.com/retcon26/psp-sandbox-server))

| Server | For the game | Changes |
|---|---|---|
| 1.1.0 | 1.7.1 | Server lists (`--mode list`); game servers sign up to one (`list`, `public_address`); the Retcon's Mod name |
| 1.0.0 | 1.7.0 | First version: hosts a map with nobody playing on it, for up to 3 PSPs; console; `server.cfg` |
