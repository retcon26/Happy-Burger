# Retcon's Mod: every version

Retcon's Mod (formerly PSP Sandbox) for the PSP, newest first. The version shows in the main menu's corner. The
README's table has only the latest three; this page has them all.

**Multiplayer** needs everyone on a version with the same network protocol (the "Plays with" column). Games of
another protocol don't show up in Join Game or Online Servers.

| Version | Date | Plays with | In short |
|---|---|---|---|
| [1.14.0](#1140) | 2026-10-17 | 1.12.0 – 1.14.0 | Spawn with weapons option and a Weapons spawnlist; smaller maps |
| [1.13.2](#1132) | 2026-10-17 | 1.12.0 – 1.14.0 | New Game's map grid fits; too-big Workshop maps flagged |
| [1.13.1](#1131) | 2026-10-17 | 1.12.0 – 1.14.0 | Gamemodes on the Workshop's Addons tab; short-named Lua files found again |
| [1.13.0](#1130) | 2026-10-17 | 1.12.0 – 1.14.0 | Workshop maps |
| [1.12.0](#1120) | 2026-10-17 | 1.12.0 – 1.14.0 | Lua gamemodes: Prop Hunt, Zombie Survival, Deathmatch; a bigger Lua API |
| [1.11.0](#1110) | 2026-10-16 | 1.11.0 | Quick chat: messages and gestures |
| [1.10.1](#1101) | 2026-10-16 | 1.10.0 – 1.10.1 | Pause menu > Screenshot |
| [1.10.0](#1100) | 2026-10-16 | 1.10.0 – 1.10.1 | Ammo boxes; bigger servers with passwords, rotation, autosave; the rename's done |
| [1.9.1](#191) | 2026-10-16 | 1.8.2 – 1.9.1 | Updates work with a wrong PSP clock |
| [1.9.0](#190) | 2026-10-16 | 1.8.2 – 1.9.1 | Options > Updates: new versions from GitHub over Wi-Fi; LOD distance slider |
| [1.8.3](#183) | 2026-10-16 | 1.8.2 – 1.9.1 | Target FPS option; Select closes the spawn menu again |
| [1.8.2](#182) | 2026-10-16 | 1.8.2 – 1.9.1 | NPC navigation; Face Poser and Finger Poser; physics gun turns props on every axis |
| [1.8.1](#181) | 2026-10-16 | 1.8.1 | Health kits, suit batteries, health and suit chargers |
| [1.8.0](#180) | 2026-10-16 | 1.8.0 | More weapons: AR2, crossbow, RPG, grenades, bug bait; PS2 support ends |
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

## 1.14.0

- **Options > Spawn with weapons:** on (the default), you spawn with every weapon as before. Off, you start with
  the toolbox (Hands, Physics Gun, Gravity Gun and Tool Gun) and take the rest from the spawn menu's new
  **Weapons** list. Respawning starts you with the toolbox again; turning the option on hands you everything at
  once. Gamemodes give their own weapons either way, and their games have no Weapons list.
- **Weapons spawnlist:** all 14 weapons, after Workshop, with GMod's weapon icons (from `tools/bspconv.sh ui`;
  without them the tiles show the names). X puts one in your hands with a full load of ammo, or takes out one
  you already have.
- **Smaller maps** (map format v6, from the converter):
  - Visibility rows are run-length coded, and a row shared by several clusters is kept once. gm_bigcity's table
    went from 7.8 MB to 2.7 MB.
  - Terrain (displacement) collision keeps each triangle's bounding box instead of six bevel planes, which the
    game makes as it traces. pl_goldrush's collision planes alone were 6.6 MB.
  - Low-detail files: gm_bigcity 15.1 MB to 10.3 MB, pl_goldrush 16.0 MB to 13.1 MB, tc_hydro down to 13.7 MB.
    All three now fit a PSP (about 14.5 MB of room), and every map takes that much less RAM in the game, since
    it's held whole. Converted again, every one of the 264 stock and Workshop maps fits (tc_hydro is the biggest);
    the 63 that gain most come to about 110 MB less in low detail.
  - The game reads v5 maps too, so maps you converted before still work: convert them again
    (`tools/rebuild_all.sh`, which now redoes your Workshop maps too) for the smaller files. Older versions of
    the game can't load v6 maps; they say "not a v5 pmap".
- autoload.txt test lines: `qcat NAME` opens the spawn menu on a list, `qpick N` picks its Nth tile, `wsel` shows
  the weapon selector for 5 seconds.
- Network protocol 15, as 1.12.0 to 1.13.2.

## 1.13.2

- **Fixed:** Start New Game's second row of maps ran into the bottom line and the Start button (in any category
  with more than three maps); the rows sit closer now, with the map's name on its picture as before.
- **Workshop maps:** `tools/bspconv.sh workshop` (and `--list`) says "too big for a PSP" when a map's low-detail
  file is over about 14.5 MB: the game has room for about that much map once its packs are loaded (gm_valley's
  13.9 MB loads, gm_bigcity's 15.1 MB doesn't, and the game says so).
- autoload.txt `menu newgame Workshop` opens Start New Game on that category (for tests).
- Network protocol 15, as 1.12.0 to 1.13.1.

## 1.13.1

- **Gamemodes as addons:** Main menu > Workshop > Addons lists the gamemodes (by name, marked Gamemode) above the
  Lua addons. One that's off isn't offered by Start New Game's Triangle; joining a game that runs it still works.
- **Fixed:** Lua files whose names fit the old 8.3 form (`example.lua`, `prophunt.lua`) come back from the memory
  stick in capitals (`EXAMPLE.LUA`), on a PSP and in PPSSPP alike, so those addons didn't run or show in the
  Workshop, and such gamemodes in `data/lua/gamemodes` weren't found. Names are read case-blind now.
- Network protocol 15, as 1.12.0 and 1.13.0: they all play together, and with dedicated servers 1.3.0.

## 1.13.0

- **Workshop maps:** `tools/bspconv.sh workshop` converts the maps in your GMod Workshop downloads. Each addon
  with maps is unpacked once into `tools/workshop_cache`; its maps find their textures and models as in GMod (the
  map, its addon, your other Workshop addons, GMod, then Counter-Strike: Source, the HL2 episodes, TF2 and
  Half-Life: Source), get their NPC node graphs, and their thumbnails when the addon has one. `--list`, `--all`,
  `--jobs N`; `tools/rebuild_all.sh` does it too. The converter writes `data/maps/workshop.txt`, and the game lists
  those maps under a new **Workshop** category in Start New Game and Main menu > Workshop > Maps (room for 400
  maps now, up from 320).
- Network protocol 15, as 1.12.0: they play together, and dedicated servers 1.3.0 host Workshop maps too (give the
  server the converted files).

## 1.12.0

- **Gamemodes**, written in Lua: **Prop Hunt** (props hide as things on the map, hunters find them; shooting the
  wrong prop costs health; quick chat taunts; teams swap each round), **Zombie Survival** (ever bigger waves of
  zombies, more weapons each wave, the dead back when a wave's beaten, barricades with Use) and **Deathmatch**
  (first to 15 kills; Combine soldiers when you're alone). New Game's Triangle picks one (remembered in
  settings.cfg); joining plays the host's; Online Servers shows each server's.
- In a gamemode's game: Select held shows a **scoreboard** (kills, deaths, teams); dead players **watch** the
  others (L / R picks who); no spawn menu, tool gun, noclip, God mode or unlimited ammo; name tags only for team
  mates in team games; the pause menu has just Resume, Options, Screenshot and Disconnect.
- **A much bigger Lua API**, GMod-flavoured: `Vector`, `Color`, players (`player.GetAll`, `ply:Give`,
  `ply:SetTeam`, `ply:Spawn`, `ply:Freeze`, `ply:SetModel`, `ply:SetWalkSpeed`, frags and deaths...), props as
  objects (`ents.Create`, `ents.FindInSphere`), NPCs (`npc.Spawn`), teams, networked variables
  (`SetGlobalInt`, `ply:SetNWInt`), `timer.Create`, `PrintMessage`, `sound.Play`, `util.TraceLine`, `navmesh`,
  `draw.RoundedBox`, and gamemode hooks (`GM:PlayerSpawn`, `PlayerDeath`, `PlayerShouldTakeDamage`,
  `EntityTakeDamage`, `OnNPCKilled`, `PlayerUse`...). Your own gamemodes go in `data/lua/gamemodes`. All of it is
  in [docs/LUA.md](LUA.md). The game's library and gamemodes are built into the EBOOT (`psp/lua`), so Options >
  Updates brings them; Lua errors also go in `data/lua_errors.txt`. Old addons keep working (`PlayerUse` now
  gets the player and the prop).
- **Achievements:** Hide in Plain Sight, Survivor, Top Frag.
- **Fixed:** other players' names were drawn twice, the second time in white over the name tag (hiding its
  Profile colour) and through walls; `print` from Lua added a stray "userdata".
- Network protocol 15: 1.12.0 only plays with 1.12.0, and dedicated servers need 1.3.0.

## 1.11.0

- **Quick chat:** hold D-pad Down for a wheel of short messages and gestures (a tap still crouches). The
  circle pad picks one, L / R turns the page, letting go of Down sends it. Messages (Hello!, Thanks!, Yes,
  No, Follow me!, Wait here, Help!, Look at this!) go in everyone's chat with your name. Gestures (Wave,
  Salute, Yes, No, Come here, Cheer, Laugh, Dance) are GMod's own player animations: everyone sees your
  character do it, you too in the third-person view, and walking off stops it. HL2's people and the Combine
  can do them, TF2's mercenaries can't. The NPC pack carries them once (about 100 KB) with each player
  character's skeleton, so it needs rebuilding (`tools/bspconv.sh npcs`).
- **Fixed:** on a dedicated server with more than 3 players (1.10.0), the 4th to 7th were invisible and
  couldn't be hit.
- **Fixed:** the physics gun's hint sat on top of the health and suit boxes; Options' arrows ran past the
  highlight.
- Network protocol 14: 1.11.0 only plays with 1.11.0, and dedicated servers need 1.2.1.

## 1.10.1

- **Pause menu > Screenshot:** saves the screen as you see it, with the HUD and without the pause menu, as a
  PNG in `PSP/PHOTO/RetconsMod/` (the PSP Go's internal storage when the game runs from there). The XMB's Photo
  app shows them; they're named after the map, `gm_construct_0001.png` and on. A camera click (the city
  scanner's) when it's saved, with the sounds rebuilt (`tools/bspconv.sh sounds`).
- Network protocol 13, as 1.10.0: they play together, and dedicated servers 1.2.0 still work.

## 1.10.0

- **Ammo boxes** (Items spawnlist): Pistol Ammo (+20), .357 Ammo (+6), SMG Ammo (+45), Shotgun Shells (+20),
  AR2 Ammo (+20), Crossbow Bolts (+6), an RPG Round and a Grenade. Walk over one to take it when that gun isn't
  full; with Options > Unlimited ammo on they stay where they are. They need the props, sounds and icons rebuilt
  (`tools/bspconv.sh props`, `sounds`, `ui`).
- **Bigger servers:** a dedicated server takes up to 7 players (`max_players`; the game has 8 slots now, and a
  PSP that hosts still takes 4). `password` makes players type it in: Online Servers marks the server "locked".
  `maps` and `map_minutes` rotate the map, with a minute's warning, and every player's PSP joins the next map by
  itself. The players' build is saved every `autosave_minutes` (5), on a map change and on shutdown, and a map
  starts with its last autosave.
- **The rename's done:** the repositories are `retcons-mod` and `retcons-mod-server` (GitHub forwards the old
  links), the release folder is `PSP/GAME/RetconsMod`, and the server is `retcons-mod-server`. The first time
  the game runs from a new folder it brings your saves over from `PSP/GAME/PSPSandbox`.
- Achievements save to a temporary file first, so switching off mid-save no longer empties them.
- The Full House achievement is for four players or more.
- Network protocol 13: 1.10.0 only plays with 1.10.0, and dedicated servers need 1.2.0.

## 1.9.1

- **Options > Updates** works when the PSP's clock is wrong. 1.9.0 compared GitHub's certificates (which only
  last about 90 days) with the PSP's clock, which is often years out after the battery runs flat, and said to
  check the date and time. Now a certificate only fails on its dates if it had run out before the game was
  built; its signatures, the trusted roots and the name are checked as before. On 1.9.0, set the PSP's date
  and time, then Options > Updates installs 1.9.1.
- When the certificate check fails, `data/update.log` lists the certificates and why.
- Network protocol 12, as 1.8.2 to 1.9.0.

## 1.9.0

- **Options > Updates:** checks GitHub for a newer version over Wi-Fi and installs it, from the main menu. It
  shows what's new, downloads the game (about 1.2 MB) over HTTPS, checks it against GitHub's checksum and puts
  it in place of `EBOOT.PBP`, keeping the old one as `EBOOT.OLD`. Quit and start the game again to play the new
  version. Only the game updates: a version that needs new game data (built on a PC from your own games) says
  so and doesn't install. Each check is written to `data/update.log`.
- **Options > LOD distance:** a slider for how far props, the map's own props and NPCs are drawn before they
  pop out: 0.5x, 0.75x, 1x (as before), 1.5x or 2x. Draw distance still caps it.
- Versions from now on are published as GitHub releases (`psp/publish_release.ps1`), which is where the
  updater finds them. 1.9.0 is the first.
- Wi-Fi errors (Host on Wi-Fi, Online Servers) say what went wrong. They all said "Offline" before.
- Network protocol 12, as 1.8.2 and 1.8.3: they play together, and dedicated servers 1.1.1 still work.

## 1.8.3

- **Options > Target FPS:** 25, 30, 60 (the default, as before) or 120 frames a second. 25 and 30 wait for the
  screen, so the pace stays even (25 alternates two and three refreshes a frame); physics keeps real time at
  them on every Physics quality. The screen shows 60 a second at most, so 120 stops waiting for it and only
  caps the rate: the game reacts sooner, but the picture can tear.
- **Spawn menu:** Select closes it again. Pressing Select closed it, but letting go opened it straight back up,
  so only Circle worked.
- Network protocol 12, as 1.8.2: they play together, and dedicated servers 1.1.1 still work.

## 1.8.2

- **NPC navigation:** the converter links each map's node graph (the `info_node`s mappers place for HL2's NPCs;
  166 maps have them, gm_construct and the Half-Life 2 and Half-Life campaigns among them) into a small
  `<map>.pnav` file. An NPC that can't walk straight to where it's going follows the nodes round walls, through
  doorways and up stairs, planning again as its target moves. When something blocks the way (a closed door, a
  pile of props) it tries another route. Citizens wander from node to node and run away along them. On maps
  without nodes NPCs head straight for where they're going, as before.
- **Face Poser** (Tool Gun > Poser): gives the ragdoll you aim at an expression: Smile, Grin, Angry, Sad,
  Surprised, Scared, Pain, Shout or Eyes Shut. L picks one, and Neutral puts the ragdoll's own face back. The
  expressions are baked from each face's own flexes, so HL2's and TF2's characters each make them their own way.
- **Finger Poser:** poses the ragdoll's hand you aim at, or both if you aim at the body: Fist, Point, Thumbs Up,
  Peace, Rock On, Open or OK (Natural puts them back). The characters now have hands with fingers, taken from
  a more detailed version of each model (the PSP-1000's characters keep their simpler hands).
- **Physics gun:** hold X while you hold a prop and the circle pad turns it on every axis: left and right turn it,
  up and down tip it away from you or towards you. You stand still meanwhile.
- The README's build instructions are rewritten step by step, from installing Ubuntu to copying the game to a
  PSP.
- Network protocol 12.

## 1.8.1

- **Items** (a new spawnlist): HL2's Health Kit (+25 health), Health Vial (+10) and Suit Battery (+15 suit
  power). Walk over one to take it, when it would help.
- **Health Charger and Suit Charger:** they stay where you put them (frozen, like cameras; move them onto a wall
  with the physics gun). Tap Use on one and it charges you until you're full, it's empty, you walk away or you
  tap again. A health charger holds 50 and a suit charger 75, and they slowly fill up again.
- Batteries and suit chargers need Options > HEV suit on.
- Network protocol 11.

## 1.8.0

- **AR2:** an automatic rifle that hits harder than the SMG.
- **Crossbow:** fast bolts that drop a little over distance and do 100 damage; they stick in walls. It reloads
  after every shot.
- **RPG:** laser-guided rockets: each one steers towards wherever you aim until it hits. 3 rockets.
- **Grenades:** thrown, they bounce about with the physics and go off after 2.5 seconds. 5 grenades.
- **Bug bait:** tap R to throw it and antlions swarm where it lands; hold R to squeeze it and they come to you
  and follow you. From the first use until the map ends, antlions fight on your side, against the Combine,
  zombies and mingebags. Headcrabs and antlion guards aren't fooled.
- Explosions (rockets, grenades and the Explosive tool) hurt other players near them too, and clients see
  every explosion.
- The weapon selector's slots shrink to fit all 14 weapons.
- **The PS2 port is no longer supported.** [PS2-GMOD](https://github.com/retcon26/PS2-GMOD) stays up as it was
  (on the 1.3.0 engine); new versions are for the PSP only.
- Network protocol 10. Dedicated servers need the game 1.8.0 too (psp-sandbox-server's game submodule).

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
| 1.3.0 | 1.12.0 | Gamemodes: `gamemode` in `server.cfg` (or `--gamemode`), console `gamemode NAME`; built with Lua whenever the PC has Lua 5.4's headers; no build autosaves in a gamemode's game |
| 1.2.1 | 1.11.0 | The game 1.11.0's engine (quick chat; the 4th to 7th players show up and can be hit) |
| 1.2.0 | 1.10.0 | Renamed `retcons-mod-server`; up to 7 players (`max_players`), `password`, map rotation (`maps`, `map_minutes`, console `next`), autosaves (`autosave_minutes`, `autosave_resume`) |
| 1.1.1 | 1.8.2 | The game 1.8.2's engine (NPC navigation: the server's NPCs follow the maps' node graphs, `data/maps/<map>.pnav`) |
| 1.1.0 | 1.7.1 | Server lists (`--mode list`); game servers sign up to one (`list`, `public_address`); the Retcon's Mod name |
| 1.0.0 | 1.7.0 | First version: hosts a map with nobody playing on it, for up to 3 PSPs; console; `server.cfg` |
