# Retcon's Mod

A physics sandbox for the PSP, by Retcon. Current version: **1.9.1** (shown in the main menu's corner,
set in `psp/src/version.h`).

**Retcon's Mod is PSP Sandbox's new name.** The name is changing step by step: the game (the XMB, the menus, the
credits) and the docs say Retcon's Mod now, and the menus have the game's own logo instead of Garry's Mod's. The
repositories, the `PSP/GAME/PSPSandbox` folder and its save files keep their old names for now, so links and saves
keep working.

- **`psp/`**: the engine, written from scratch in C for the PSP. It draws with the GE (the PSP's graphics chip)
  directly and has its own physics, ragdolls, sound mixer, NPCs, multiplayer and Lua scripting.
- **`tools/bspconv/`**: a PC-side converter (Python). It reads maps, models, textures and sounds from your own
  Garry's Mod and Valve game installs and turns them into compact files the consoles can load.
- **[PS2-GMOD](https://github.com/retcon26/PS2-GMOD)** (a separate repo): the PS2 port, **no longer supported**
  from 1.8.0. It stays up as it was (on the 1.3.0 engine); new versions are for the PSP only.
- **[psp-sandbox-server](https://github.com/retcon26/psp-sandbox-server)** (a separate repo): a dedicated server.
  It runs this engine on a Linux PC or server, with nobody playing on it, and hosts games that PSPs join over Wi-Fi
  and the internet (see [Multiplayer](#multiplayer)).

**No game content is in this repository.** GMod's and Valve's maps, models, textures and sounds belong to their
owners. You make the game data yourself from the games you own (see [Building](#building)), and builds that
contain it are for your own use. Don't share them.

<p align="center">
  <img src="docs/multiplayer.jpg" width="450" alt="Two PSPs playing gm_construct together over ad-hoc multiplayer, each screen showing the other player's character, with the physics gun in first person">
  <br>
  <em>Two real PSPs in ad-hoc multiplayer on gm_construct: each screen shows the other player, with your own physics gun in first person.</em>
</p>

## What's in it

- **250 maps:** gm_construct and gm_flatgrass, the Half-Life 2 campaign, the original Half-Life campaign
  (converted from GoldSrc), HL2 Deathmatch, Half-Life Deathmatch: Source, Counter-Strike: Source, Day of Defeat:
  Source and Team Fortress 2 maps, plus **gm_construct_13** (see [Maps](#maps)).
- **About 3,200 props:** 191 hand-picked ones in 11 spawnlists, plus about 3,000 more pulled automatically from
  HL2 and its episodes, CS:S, DoD:S, Portal, Portal 2, Left 4 Dead 1 and 2, Fistful of Frags, Black Mesa and TF2,
  in about 160 spawnlists. The extra ones load from the memory stick only when spawned.
- **48 NPCs and 98 ragdolls**, including Team Fortress 2's nine mercenaries on both teams (also as player
  models), mingebags, and characters from Counter-Strike: Source and Half-Life: Source. On maps with a node graph
  NPCs find their way round walls, and the Face Poser and Finger Poser pose ragdolls' faces and hands.
- **14 weapons and 26 tool gun tools**, health kits, suit batteries and chargers, blood and bullet holes, a
  flashlight, an HEV suit, Lua addons and 4-player multiplayer with voice chat: ad-hoc, over Wi-Fi, or on a
  dedicated server.
- **Saves:** your builds saved to the memory stick and loaded back on any map (see [Saves](#saves)).
- **Drivable vehicles:** HL2's Jeep and Airboat, and seats to sit on your own contraptions (R switches all their
  thrusters and hoverballs). Spawn them from the Vehicles category; Use gets in and out.

## Targets

| System | RAM for games | Files used |
|---|---|---|
| PSP-2000 / 3000 / Go / Street (primary) | ~52 MB | full detail: `name.pmap`, `props.ppak`, `npcs.pnpc`, `sounds.psnd` |
| PSP-1000 | ~24 MB | `*.low.*`: half-size textures, coarser geometry, 16 kHz sound, no ragdoll-only characters |

The game measures free memory at startup and picks the right files. If a full-detail map doesn't fit when you
load it, it falls back to the map's low-detail version.

## Playing

`psp/make_release.ps1` lays the game out ready to copy: `psp/release/PSP/GAME/PSPSandbox/` holds `EBOOT.PBP`,
`CONTROLS.txt` and the `data` folder.

- **Real PSP** (needs custom firmware for homebrew): copy `release/PSP` to the root of the memory stick.
- **PPSSPP on PC:** File > Load, and pick `EBOOT.PBP` in that folder (the `data` folder must sit next to it).
  Set Settings > System > PSP model to **PSP-2000/3000** for full detail.
- **PPSSPP on iPhone / iPad:** there's no special version. Copy `release/PSP` into Files > On My iPhone >
  PPSSPP, set the PSP model as above, and start it from the Games tab. Apple doesn't allow PPSSPP's fast CPU
  mode on iOS, so lower Draw distance and Physics quality if it's slow.

The full data is about 2.85 GB (Windows shows 2.66 GB), 91% of it maps. To save space, delete maps you don't play
from `data/maps`: each map is `name.pmap` and `name.low.pmap`, plus `name.ppak` and `name.low.ppak` when it has
props of its own and `name.pnav` (its NPC node graph) when it has one.

## Controls

The full list is in [psp/CONTROLS.txt](psp/CONTROLS.txt).

| Action | PSP |
|---|---|
| Walk (swim in deep water) | Analog stick |
| Look | Triangle / Cross / Square / Circle |
| Fire / primary | R |
| Jump (noclip: fly up) | L |
| Next weapon | D-pad Left (tap) |
| Reload | D-pad Left (hold) |
| Use (doors, buttons, thrusters) | D-pad Right (tap) |
| Carry a light prop | D-pad Right (hold, on the prop) |
| Flashlight | D-pad Right (hold) |
| Undo the last spawn | D-pad Up |
| Crouch on/off (noclip: fly down) | D-pad Down |
| Spawn menu (L/R switch panel, X selects, O closes) | Select (tap) |
| Noclip on/off | Select (hold) |
| Pause menu | Start |

While you carry a prop, jumping, crouching, undo and switching weapons are off. The D-pad turns, pushes and
pulls the prop instead.

## Weapons

| Weapon | Controls |
|---|---|
| Hands | R picks up something light (up to 35 kg, close by) or drops it. L throws it. |
| Physics Gun | Hold R to grab any prop or any ragdoll limb, at any distance. L freezes it in place. D-pad turns it and moves it nearer or further. Hold X and push the circle pad to turn a prop on every axis: left and right turn it, up and down tip it away from you or towards you (you stand still meanwhile). |
| Gravity Gun | R pulls in a prop up to 250 kg from up to 1000 units and holds it, R again drops it, L launches it. Picking up something too heavy within 250 units punts it instead. |
| Tool Gun | R uses the tool picked in the spawn menu's Tools panel. |
| Crowbar, Pistol, .357, SMG, Shotgun, AR2 | R fires. Reload when empty (or turn on Options > Unlimited ammo). The AR2 is automatic and hits harder than the SMG. |
| Crossbow | R fires a bolt: fast, dropping a little over distance, 100 damage. Bolts stick in walls. It reloads after every shot. |
| RPG | R fires a rocket that steers towards wherever you aim until it hits (HL2's laser guidance), so keep aiming at the target. 3 rockets. |
| Grenade | R throws a grenade that bounces about and goes off after 2.5 seconds. 5 grenades. |
| Bug Bait | Tap R to throw it: antlions swarm where it lands. Hold R to squeeze it: antlions come to you and follow you. Once you've used it, antlions are on your side for the rest of the map and fight the Combine, zombies and mingebags (headcrabs and antlion guards aren't fooled). |

Explosions (rockets, grenades, the Explosive tool) hurt everyone near them, other players included.

Every weapon is shown with GMod's first-person model and the citizen's arms.

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
| | Hydraulic | Pick two points: a ram holds them apart. Use pushes it out to twice its length and pulls it back. |
| | Rope | Pick two points: they can't move further apart than the rope. |
| | Slider | Pick two points: the prop slides along the line between them. |
| | Weld | Pick a prop, then a second prop (or the world): joined rigidly. |
| | Winch | Pick two points: a rope that Use reels in, then lets out. |
| Mechanics | Button | Pick the prop it switches, then where the button goes. Use on the button switches everything joined to that prop. |
| | Emitter | Puts out sparks, smoke or fire (L picks which). |
| | Explosive | Blows the prop up: nearby props fly, NPCs and you get hurt. |
| | Hoverball | Holds the prop at the height where you put it. |
| | Thruster | Pushes the prop the way you shot it in. |
| | Wheel | Puts a tyre on the prop, on an axle with a motor. The motor drives the way you faced when you put it on, so put all the wheels on from behind the cart. |
| Poser | Face Poser | Gives the ragdoll you aim at an expression: Smile, Grin, Angry, Sad, Surprised, Scared, Pain, Shout or Eyes Shut (L picks; Neutral puts its own face back). HL2's and TF2's characters have faces to pose; masked ones (the Combine, the Pyro) don't. |
| | Finger Poser | Poses the ragdoll's hand you aim at (or both, aiming at the body): Fist, Point, Thumbs Up, Peace, Rock On, Open or OK (L picks; Natural puts it back). |
| | Inflator | Makes the ragdoll body part you aim at bigger (up to 4x). |
| | Deflator | Makes it smaller (down to 0.2x). |
| Render | Camera | R places a camera facing where you look; L switches your view through each camera and back to you. Cameras are props: move them with the physics gun or weld them to a car. |
| | Colour | Paints the prop. |
| | Lamp | A lamp that lights the world where it points, in the colour picked in the Tools panel. |
| | Light | A small light that lights everything round it, in the colour picked. |

Use (D-pad Right) on a prop switches what's on it on and off: thrusters, hoverballs, wheel motors, winches,
hydraulics, lamps, lights and emitters. Sitting in a seat, R switches all of them on the contraption. The two
nearest lamps or lights that are on light the world, on top of your flashlight.

## Spawn menu

- **Props:** 191 in 11 spawnlists (Construction, Junk, Furniture, Misc, Vehicles, Shapes, Street, Combine, Food,
  Lab, Items), then about 3,000 extra props from 12 games, each game's in its own spawnlists. Up to 40 spawned props
  per player, plus the map's own physics props.
- **Items:** HL2's Health Kit (+25 health), Health Vial (+10) and Suit Battery (+15 suit power): walk over one to
  take it, when you need it. The Health Charger and Suit Charger stay where you put them (move them with the
  physics gun, onto a wall): tap Use on one and it charges you until you're full, it's empty, you walk away or you
  tap again. A health charger holds 50 and a suit charger 75, and they slowly fill up again. Batteries and suit
  chargers need the HEV suit on (Options).
- **NPCs:** 29: zombies (classic, fast, poison, Zombine, Zombie Torso), headcrabs, antlions and an antlion guard,
  Combine soldiers, Elites, Prison Guards and Metrocops, rebels and medics, citizens, and HL2's main characters.
- **Ragdolls:** every NPC, plus 50 ragdoll-only characters: more HL2 people and creatures, CS:S's counter-terrorists,
  terrorists and hostages, Half-Life: Source's scientist, Barney, grunts and aliens, Gordon Freeman, DoD:S
  soldiers and Chell. (The PSP-1000 has room for the NPCs' ragdolls only.)

## Saves

**Pause menu > Saves** keeps your builds, like GMod's saves and duplicator. A save holds everything players
spawned: the props (with their colour, weight, frozen and no-collide), welds, ropes, ball sockets, elastics,
sliders, wheels, winches and hydraulics, thrusters, hoverballs, balloons, lamps, lights, buttons and emitters.
Saves are small text files in `data/saves/` on the memory stick (a PSP Go's internal storage).

- **Save the current build:** X, then name it with the on-screen keyboard.
- **X on a save:** loads it on any map, on the floor where you're looking and turned the way you face.
- **Square:** loads it exactly where it was built (on the map it was built on).
- **Triangle:** deletes it (press it twice).

One Undo takes a whole loaded build away. Props are found by name, so a save made with the full-detail packs loads
with the PSP-1000's too, and anything a prop pack doesn't have is skipped. In multiplayer only the host saves and
loads, since the host has the whole world.

## NPCs

- **Zombies, antlions and headcrabs** chase you and attack. Zombies wear their headcrabs, and when a zombie dies
  its crab jumps off and comes after you.
- **A thrown Sawblade cuts a classic zombie in half**, as in HL2: the legs fall as a ragdoll and the top half
  carries on crawling as a Zombie Torso, headcrab and all.
- **Combine** shoot at you, rebels and monsters. **Rebels and medics** follow you and fight them. **Citizens**
  wander and run away. Armed NPCs hold their guns the way HL2 bone-merges them.
- **TF2's mercenaries** come in RED and BLU. Spawn some of each and they fight each other, never you. Each one
  carries its own class's gun, fired with TF2's sounds:

  | Class | Gun |
  |---|---|
  | Scout | scattergun |
  | Soldier | rocket launcher (rockets blow up where they land and throw props about) |
  | Pyro | flamethrower |
  | Demoman | grenade launcher |
  | Heavy | minigun |
  | Engineer | shotgun |
  | Medic | syringe gun |
  | Sniper | sniper rifle |
  | Spy | revolver |

  They also speak with their own TF2 voice lines: a battle cry when they spot an enemy, plus pain and death
  lines. BLU shares RED's mesh and animations, so the second team only costs its textures.
- **Mingebags** behave like GMod's worst players. They run rings round you, bunny-hopping and swinging a
  crowbar now and then (it barely hurts). Every few seconds one spawns a bathtub, fridge or couch next to itself,
  throws a nearby prop at you, or says something in chat ("gg ez", "can i have admin"). Only their 16 newest props
  stay, and they don't count towards your prop limit. Zombies and the Combine go after them too.
- **Navigation:** on maps with a node graph (the `info_node`s mappers place for HL2's NPCs: gm_construct, the
  Half-Life 2 and Half-Life campaigns and 166 maps in all), an NPC that can't walk straight to where it's going
  follows the nodes round walls, through doorways and up stairs. If something blocks the way (a closed door, a
  pile of props) it tries another route. Citizens wander from node to node and run away along them. On maps
  without nodes, NPCs head straight for where they're going, as before.
- **Voices:** NPCs say pain, death and alert lines in their own voices.
- **Ragdolls:** NPCs fall as verlet ragdolls when they die. Shots and props push them, and the physics gun drags
  and freezes them. The Face Poser and Finger Poser pose their faces and hands (see [Weapons](#weapons)).
- **Options > NPC thinking** freezes them in place (GMod's `ai_disabled`), and **NPCs ignore players** stops them
  targeting or following you (`ai_ignoreplayers`).

## Workshop

Main menu > Workshop turns things on and off, like GMod's addon list. It has three tabs; L and R switch between
them, X switches the highlighted entry, and Triangle switches the whole tab (on Maps, the whole game):

- **Maps:** listed by game (Sandbox, Counter-Strike, Half-Life 2, HL2 Deathmatch, HL Deathmatch, Half-Life, Day of
  Defeat, Team Fortress 2), each with how many of its maps are on. X opens a game to show its maps, and Triangle
  switches the whole game. Maps that are off aren't offered in Start New Game or Host Game.
- **Addons:** the Lua addons in `data/lua/autorun`. Ones that are off don't run.
- **Custom:** characters from your GMod Workshop downloads (`game=workshop` in `tools/npcs.txt`). Ones that are
  off leave the spawn menu's Workshop list and the player models.

What's off is saved to `data/workshop.cfg`.

## Achievements

Main menu > Achievements lists 17, with progress bars for the counted ones. They're saved to
`data/achievements.cfg`, and a notification pops up in game when you unlock one.

| Achievement | How |
|---|---|
| Getting Started / Regular / Addict | Play for 10 minutes / an hour / 10 hours (time in games, menus don't count) |
| Not Alone | Play a game with at least one other real person (hosting or joining) |
| Full House | Play a game with all four players |
| The Real Deal | Play on a real PSP, not an emulator |
| Old Faithful / Slim and Bright / On the Go | Play on a PSP-1000 / PSP-2000 or 3000 / PSP Go |
| Creator | Spawn 100 props |
| Mingebag Hunter | Kill 10 mingebags |
| Explorer | Play on 10 different maps |
| Save Point | Save a build |
| Deja Vu | Load a saved build on a different map from the one it was built on |
| Online | Join a dedicated server |
| On the Air | Host a game over Wi-Fi that someone joins |
| Yes, I'm the real Retcon | Play on a dedicated server with your name set to Retcon |

The PSP doesn't let a game read its model directly, so the page's "Playing on" line works it out:
- **PPSSPP:** spotted by its settings files on the memory stick. The game only checks they exist and never reads them.
- **PSP-1000:** has 32 MB of RAM.
- **PSP Go:** has internal storage.
- **PSP-2000 or 3000:** anything else. These two can't be told apart.

## Options

Sound, look speed, invert look, God mode (also games you host), player model, third-person view, NPC thinking,
NPCs ignore players, unlimited ammo, HEV suit (100 suit power at spawn that takes 80% of the damage, as in HL2),
show HUD, crosshair, show FPS, target FPS, draw distance, LOD distance, physics quality, debug overlay, updates and
the credits.

**Target FPS** paces the game to 25, 30, 60 (the default) or 120 frames a second. 25 and 30 run steadier than a
frame rate that keeps dropping, with physics still in real time. The screen shows 60 a second at most, so 120
stops waiting for it: the game reacts sooner, but the picture can tear.

**LOD distance** is a slider for how far away props, the map's own props and NPCs are still drawn before they
pop out: 0.5x, 0.75x, 1x (the default), 1.5x or 2x the usual distance (2,400 units for props, 2,200 for NPCs,
and each map prop's own fade distance). Draw distance still sets the furthest anything is drawn. Further costs
frame rate in busy maps; nearer gains some.

### Updates

**Options > Updates** checks GitHub for a newer version over Wi-Fi and installs it, from the main menu (not
during a game). It connects with the first connection set up in the PSP's Network Settings, so set one up there
first and turn the WLAN switch on. If there's a newer version, the page shows what's new; X downloads it
(about 1.2 MB), checks it against GitHub's checksum and puts it in place of the game's `EBOOT.PBP`. Quit and
start the game again to play it.

- Only the game itself updates. The data (maps, props, NPCs, sounds) is built on a PC from your own games, so
  a version that needs new data says so and doesn't install: rebuild the data and the game on your PC instead.
- The version you had is kept as `EBOOT.OLD` in the game's folder. To go back, delete `EBOOT.PBP` and rename
  `EBOOT.OLD` to `EBOOT.PBP`.
- The download is checked against GitHub's certificate. The PSP's date doesn't need to be right (1.9.0 needed
  it: on 1.9.0, set the PSP's date and time first, then update).
- Each check writes what happened to `data/update.log`, which helps if an update fails.

## Maps

`tools/maps.txt` lists 250 maps, converted from your installs:

| From | Maps |
|---|---|
| Garry's Mod | gm_construct, gm_flatgrass |
| Half-Life 2 | the whole campaign (`d1_`, `d2_`, `d3_`) |
| Half-Life | the whole original campaign, converted from GoldSrc (`hl1_`) |
| HL2 Deathmatch | `dm_` maps |
| Half-Life Deathmatch: Source | crossfire, bounce, datacore, frenzy, stalkyard, undertow and more |
| Counter-Strike: Source | `cs_` and `de_` maps |
| Day of Defeat: Source | `dod_` maps |
| Team Fortress 2 | 22 official maps, one or more of each mode: 2Fort, Turbine, Dustbowl, Granary, Badwater, Gold Rush, Viaduct, Hydro, Hightower, Doomsday, Mannworks and more |

The list also has **gm_construct_13**: gm_construct at night, in thick dark fog. A black figure turns up in the
distance now and then, at the edge of your view. Look straight at it, or walk towards it, and it's gone. It reuses
gm_construct's files, so it takes no extra space.

## Multiplayer

Up to 4 players. The host runs the physics, NPCs, ragdolls and doors. Multiplayer has three ways to play:

- **Host Game / Join Game:** ad-hoc, PSP to PSP, no access point needed.
- **Host on Wi-Fi:** your PSP hosts through your access point, using the first connection set up in System
  Settings > Network Settings. Other PSPs on the same Wi-Fi find it in Online Servers.
- **Online Servers:** the games on your Wi-Fi, the dedicated servers on a server list, and servers you add
  yourself. Triangle adds a server by its address (a name or `1.2.3.4`, with `:port` if it isn't the usual
  18256), Square removes it and X joins. R sets the **server list**: its address, and every public server it
  knows shows up without typing theirs. Both are kept in `data/servers.cfg`.

A **dedicated server** ([psp-sandbox-server](https://github.com/retcon26/psp-sandbox-server)) runs this game's
engine on a Linux PC or a rented server, with nobody playing on it, so up to 3 PSPs can play there. It has a
console for kicking, chat, changing maps and loading saved builds. For players outside your network, forward UDP
port 18256 on your router to the PC that runs it.

A **server list** is the same program in list mode (`psp-sandbox-server --mode list`, UDP port 18257).
Dedicated servers set `list` to its address and tell it they're up every 30 seconds; PSPs ask it for the servers
of their own game version and then ask each server about its game, so the list only ever shows servers that are
answering.

Doors, buttons and triggers react to every player, not just the host, so TF2's spawn doors open for everyone.

**Multiplayer > Profile** is how other players see you:

- **Name:** X opens the PSP's on-screen keyboard. Leave it empty to use the PSP's nickname.
- **Player model.**
- **Player colour:** tints your player model and your name tag.
- **Physics gun colour:** the colour of your beam.

Everyone sees each other's names over their heads, and each other's physics gun beams in their colours.
Everyone needs the same version (the menu's corner shows it). Guns and the crowbar hurt other players (not with God
mode on), and everyone sees the same ragdolls: the host simulates them and sends their poses. Vehicles work for
everyone: the host drives them for whoever is inside.

**Voice chat (PSP):** hold D-pad Up to talk (a quick tap still undoes). It uses the PSP-3000's built-in
microphone, or a headset's on a PSP-2000. The mic is only switched on while you hold Up. Speech is sent as
4-bit ADPCM at 11 kHz (about 5.5 KB/s), and the host passes it on to everyone else.

Ad-hoc wifi between PSPs (or PPSSPP instances with ad-hoc networking set up), or Wi-Fi through an access point
(PPSSPP: with networking enabled).

## Lua addons

Every `.lua` file in `data/lua/autorun/` runs when a map loads, and `data/lua/maps/<map>.lua` runs on that map
only. The API is a small GMod-flavoured subset: `hook.Add` (`Think`, `PlayerUse`, `HUDPaint`), `timer.Simple`,
`ents.CreateProp/Remove/GetPos/SetPos/SetVelocity/SetColor/Freeze`, `player.GetPos/GetYaw/GetAimEntity`,
`draw.Text`, `print`. See [psp/data/lua/autorun/example.lua](psp/data/lua/autorun/example.lua): Use on a prop
launches it.

## Building

Everyone builds their own copy: the game data comes from your own Valve games and can't be shared, so this is
how you get the game. [Playing](#playing) then says how to copy it to a PSP or PPSSPP.

The game is built in two halves. The **data** (maps, props, characters, sounds) is converted on your PC from the
Valve games you own; none of it is in this repository. The **EBOOT** (the game program) is compiled with the PSP
SDK. Both happen in Ubuntu on Windows (WSL). In every command below, the code block says where to type it.

### 1. One-time setup

**Install Ubuntu.** In PowerShell (as administrator), then restart the PC:

```powershell
wsl --install -d Ubuntu
```

Open Ubuntu from the Start menu and pick a user name and password when it asks.

**Install the tools** the converter and the SDK need. In Ubuntu:

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake pkgconf libusb-0.1-4 fakeroot python3-numpy python3-pil mpg123 git
```

(`mpg123` decodes TF2's MP3 voice lines.)

**Install the PSP SDK** ([pspdev](https://github.com/pspdev/pspdev)) into `~/pspdev`. In Ubuntu:

```bash
cd ~
wget https://github.com/pspdev/pspdev/releases/latest/download/pspdev-ubuntu-latest-x86_64.tar.gz
tar -xzf pspdev-ubuntu-latest-x86_64.tar.gz
```

`psp/build.sh` finds it there by itself; you don't need to change your `PATH`.

**Install the games** through Steam: Garry's Mod at least. Half-Life 2, Half-Life (and Half-Life: Source),
Team Fortress 2, Counter-Strike: Source and Day of Defeat: Source add their maps, props and characters if you
have them.

**Tell the converter where Steam keeps them.** It looks in `D:\SteamLibrary` and in Steam's own folder on `C:`.
If your games are somewhere else, edit the paths at the top of
[tools/bspconv/steam.py](tools/bspconv/steam.py) (`LIBRARIES`) and `DEFAULT_GMOD` in
[tools/bspconv/\_\_main\_\_.py](tools/bspconv/__main__.py). Ubuntu sees `C:` as `/mnt/c`, but another drive
has to be connected each time Ubuntu starts. For `D:`, in Ubuntu:

```bash
sudo mkdir -p /mnt/d && sudo mount -t drvfs D: /mnt/d
```

**Get the code.** In Ubuntu:

```bash
git clone https://github.com/retcon26/psp-sandbox.git
cd psp-sandbox
```

### 2. Build everything

Close PPSSPP first: it locks the EBOOT while it runs. In Ubuntu, in the `psp-sandbox` folder:

```bash
bash tools/rebuild_all.sh
```

It converts every map in [tools/maps.txt](tools/maps.txt) (about 250, so expect a few hours), builds the
packs in `psp/data/` (props, characters, first-person weapons, sounds, fonts and icons), then compiles
`psp/EBOOT.PBP`. The extra props from the other games are a separate, longer step:

```bash
bash tools/bspconv.sh extras
```

### 3. Put it on a PSP or in PPSSPP

Make the folder to copy. In PowerShell, in the `psp-sandbox` folder:

```powershell
powershell -File psp/make_release.ps1
```

This makes `psp\release\PSP\GAME\PSPSandbox\`. Copy `psp\release\PSP` onto the memory stick (or into PPSSPP's
`memstick` folder), merging it with the `PSP` folder that's already there. Add `-Dev` for a smaller build
(`psp\release_dev\`, under 500 MB) with only the maps in [psp/dev_maps.txt](psp/dev_maps.txt), which is
quicker to copy and test with.

### Changing just one part

After the first full build you can redo one step on its own. Run these in Ubuntu, from the `psp-sandbox`
folder:

| To rebuild | Run | It reads |
|---|---|---|
| The game code only | `bash psp/build.sh` | `psp/src/` |
| One map | `tools/bspconv.sh convert <path to map.bsp>` | a `.bsp` (Source or GoldSrc) |
| Maps you've added to the list | `tools/convert_new.sh [jobs] [all]` | `tools/maps.txt` |
| NPC navigation, for maps already converted | `tools/bspconv.sh nav` | each map's `info_node`s |
| Props and weapon models | `tools/bspconv.sh props` | `tools/props.txt` |
| Extra props from the other games | `tools/bspconv.sh extras` | the installed games |
| NPCs and ragdolls | `tools/bspconv.sh npcs` | `tools/npcs.txt` |
| Animated first-person weapons | `tools/bspconv.sh vmodels` | `tools/props.txt` (its `vm` models) |
| Sounds | `tools/bspconv.sh sounds` | `tools/sounds.txt` |
| Fonts, spawn icons, map thumbnails | `tools/bspconv.sh ui` | the lists above |

Then run `make_release.ps1` again to update the release folder.

What each one makes:

- **Maps:** `psp/data/maps/<map>.pmap` and `<map>.low.pmap` (full detail and PSP-1000), the map's own props
  in `<map>.ppak`, and its NPC node graph in `<map>.pnav` (maps without `info_node`s don't get one, and NPCs
  there head straight for where they're going).
- **NPCs:** `npcs.pnpc` (full detail) and `npcs.low.pnpc` (PSP-1000), with the Face Poser's expressions baked
  from each face's flexes and, in the full-detail pack, hands with fingers for the Finger Poser. It also writes
  `npcs.ps2.pnpc` for the old PS2 port, which is no longer supported; the PSP doesn't use it.
- **Weapons:** GMod's `c_` weapon models, with the player's arms bone-merged on, go into `vmodels.pvmd` and
  `vmodels.low.pvmd` with their draw, idle, fire and reload animations, so the weapon in your hands moves.
  Without them the game shows the still models from the prop pack instead.

### Publishing a version

Options > Updates installs a version once it's published as a GitHub release with its `EBOOT.PBP`. After
bumping `GAME_VERSION` in `psp/src/version.h`, adding the version to `docs/CHANGELOG.md`, building, committing
and pushing, run this in PowerShell (it needs the [GitHub CLI](https://cli.github.com), signed in):

```powershell
powershell -File psp/publish_release.ps1
```

It makes the release `vX.Y.Z` with the changelog's section as its notes. Only `EBOOT.PBP` is published, never
game data. If a version changes the converter so that older data stops working, bump `GAME_DATA_VERSION` in
`version.h` too: PSPs with older data are then told to update on their PC.

## What gets converted

- **Map geometry:** brush faces and displacements become indexed triangles. Faces are cut on a world-aligned
  grid only where their lightmap has detail. Vertices are packed into 16 bytes (16-bit UV and position).
- **GoldSrc maps** (Half-Life): brushes are rebuilt from the collision hull's BSP tree, lighting is converted to
  Source's format, and textures come from the WAD files.
- **Lighting:** baked lightmaps become vertex colours (drawn with the GE's 2x colour doubling). Props are lit by
  the map's per-area ambient light plus the sun, with a shadow test toward the sun.
- **Textures:** VTF base textures are cut to 128px (or 64px), reduced to 256-colour paletted (T8), swizzled, and
  given mip levels. On the PSP the most-used ones are copied into the free ~1.2 MB of VRAM at load.
- **Visibility:** batches of triangles per 1024-unit cell and material, plus static props, each get a bitset of
  what every BSP cluster can see.
- **Collision:** map brushes (with bevels), displacement triangles as prism brushes, static brush entities, and
  the convex pieces of solid static props (so a door frame leaves its doorway open).
- **Entities:** sliding and swinging doors, buttons, fans and triggers, linked by the map's I/O outputs
  ("button OnPressed -> door Open"). Static props and the map's physics props come with their models.
- **Props:** MDL/VVD/VTX models at a LOD that fits a triangle budget (simplified when there's no low LOD), with
  real masses, surface types and convex collision hulls from `.phy`.
- **Weapons:** GMod's `c_` models with `c_arms_citizen` bone-merged onto them, posed in the idle animation and
  baked into one mesh. The physics gun is the gravity gun's model with its colours turned blue.
- **NPCs:** rigid-skinned models with idle, walk, run, attack and death animations baked into per-bone matrices,
  their headcrab and waist bones marked for zombies, and guns lined up on the hand bone as Source does. Faces get
  the Face Poser's expressions, baked by running the model's own flex rules and adding up its flexes' vertex
  offsets. Valve's low LODs merge the fingers into the hand, so the hands are taken from the coarsest LOD that
  still has finger bones and thinned out finger by finger (about 100 triangles a hand).
- **NPC navigation:** each map's `info_node`s are dropped to the floor and linked when a character-sized box
  can slide from one to the next with floor under it all the way, tested against the converted map's own
  collision. The game plans routes over the graph with A*.
- **Sound:** WAV (PCM and MS-ADPCM), mono, at 44.1 kHz (16 kHz on the PSP-1000), mixed in 16 voices with 3D panning.

## Physics and collision

- **Props:** boxes, spheres, cylinders or convex hulls, handled as surface sample points. Contacts use sequential
  impulses with friction, and there are welds, ball sockets, ropes, elastics, sliders, thrusters, hoverballs,
  buoyancy in water, sleeping and freezing.
- **Fast props** sweep their path, so they stop at thin walls instead of passing through.
- **Player and NPCs** collide with props' real rotated shapes. If a prop is pushed into you, you can walk out of it.
- **World traces** follow the BSP tree along the path, so long traces never miss walls.
- **Ragdolls:** verlet ragdolls with braced joints that collide with the world and props.
- **Blood and bullet holes:** shots and hits on flesh spray drops and leave HL2's blood splats; bullets leave
  holes in walls and props (on props they move with it), by the prop's material. They're drawn like HL2's
  modulate decals (the surface times the texture), so they're lit with the surface, and are lifted off it by more
  the further away they are, so the PSP's 16-bit depth buffer doesn't make them flicker. Splats that would hang
  over an edge are made smaller. Pause > Clean Up Props clears them.
- **Vehicles:** one rigid body each. The Jeep rides on four raycast wheels with springs, grip, steering and
  all-wheel drive; the Airboat on four floats that also sit on water, pushed by its fan and turned by its rudders.
- **Doors:** brush doors slide and swing, and Source's I/O wires them to buttons and triggers. That includes
  triggers' OnEndTouch outputs (TF2's spawn doors close behind you) and wildcard targets. Models parented to a
  door move with it, such as TF2's sliding door panels and handles. HL2's model doors (`prop_door_rotating`)
  swing open with Use, away from you, and double doors open together. Locked ones stay shut.

## Versions

The latest three. Every version, with dates, what's in each and which ones can play together, is in
[docs/CHANGELOG.md](docs/CHANGELOG.md).

| Version | Changes |
|---|---|
| 1.9.1 | Options > Updates works when the PSP's clock is wrong (1.9.0 said to check the date and time) |
| 1.9.0 | Options > Updates: downloads and installs new versions from GitHub over Wi-Fi; Options > LOD distance slider (how far props and NPCs are drawn); Wi-Fi errors say what went wrong (they all said "Offline") |
| 1.8.3 | Options > Target FPS: 25, 30, 60 or 120 frames a second; Select closes the spawn menu again (letting go of it opened the menu straight back up) |

## Roadmap

- [x] PSP renderer, camera, memory tier detection; full and low detail tiers
- [x] BSP / VPK / VMT / VTF readers; GoldSrc (Half-Life) maps
- [x] Collision and Source-style movement (jump, crouch, stairs, slopes, swimming, noclip)
- [x] Props with physics, constraints and water; physics gun, gravity gun, tool gun; spawn menu, undo
- [x] Doors, buttons, fans, triggers and outputs; static props; map physics props
- [x] Skybox textures and light styles
- [x] Lua scripting layer
- [x] NPCs with baked skeletal animation, voices, weapons and ragdolls; headcrabs; sawblade cuts
- [x] Weapons with first-person models; blood; flashlight; HEV suit
- [x] Thousands of extra props from other Source games
- [x] Multiplayer (PSP ad-hoc, PS2 LAN), up to 4, host-authoritative
- [x] PS2 port, with map geometry on the VU1 coprocessor
- [x] Camera tool; Poser tools (Inflator, Deflator); unlimited ammo and HEV suit options
- [x] PS2 speed: props and NPCs on VU1 too
- [x] Animated first-person weapons; PvP damage; synced ragdolls
- [x] Team Fortress 2's maps, props and characters
- [x] Drivable Jeep and Airboat; seats for contraptions; bullet holes and decals; voice chat
- [x] Working doors in TF2 and HL2 maps; mingebags; multiplayer profiles (name, player and physics gun colours) and
      name tags; achievements
- [x] TF2's mercenaries in RED and BLU teams with their guns and voice lines
- [x] More tools: Wheel, Winch, Hydraulic, Lamp, Light, Button and Emitter
- [x] Workshop menu: maps (by game), Lua addons and custom characters switched on and off
- [x] Save and load: builds saved to the memory stick and loaded back on any map
- [x] More weapons: AR2, crossbow, RPG, grenades and bug bait (antlions follow you)
- [x] Health kits, suit batteries, and health and suit chargers
- [x] NPC navigation along the map's own node graph (`info_node`), so NPCs walk round walls instead of into them
- [x] Face Poser expressions and Finger Poser hands for ragdolls; the physics gun turns props on every axis
- [x] Wi-Fi multiplayer (Host on Wi-Fi, Online Servers) and dedicated servers
      ([psp-sandbox-server](https://github.com/retcon26/psp-sandbox-server)); doors and triggers for every player
- [x] Server list: every public dedicated server in Online Servers, without typing addresses
- [x] Updates over Wi-Fi: Options > Updates installs new versions from GitHub

- [ ] **Effects:** sparks on metal, smoke, fire after explosions, muzzle smoke and tracer impacts
- [ ] **Lua gamemodes:** a bigger Lua API and small gamemodes built on it, like Prop Hunt and a zombie survival mode
- [ ] **More player customization:** model skins and bodygroups, on top of the player model and the player and
      physics gun colours
- [ ] **Quick chat:** a radial menu of short messages and gestures for multiplayer, since the consoles have no
      keyboard
- [ ] **Share builds:** send a saved build to the other players in a game (ad-hoc, Wi-Fi or a server), and let
      players who joined save and load builds too (only the host can now)
- [ ] **The rest of the rename:** the repositories, the `PSPSandbox` game folder (moving saves across) and the
      server's name, as Retcon's Mod
- [ ] **Bigger servers:** more than 3 players on a dedicated server (the game has 4 slots and the server keeps one
      empty), plus a password, map rotation and the build saved every few minutes
- [ ] **Server on Windows:** psp-sandbox-server built for Windows too (it runs on Linux and WSL now)
- [ ] **Day, night and weather:** a time-of-day option with fog and rain, built on gm_construct_13's fog
- [ ] **Screenshots:** a pause menu button that saves the screen to the memory stick
- [ ] **Workshop maps:** convert maps from your GMod Workshop downloads too (the converter already reads their
      .gma files for characters), listed under Workshop
