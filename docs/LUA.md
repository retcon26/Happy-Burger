# Lua: addons and gamemodes

Retcon's Mod runs Lua 5.4 with a GMod-flavoured API. If you've written GMod Lua, most of this will look
familiar: `hook.Add`, `timer.Simple`, `player.GetAll`, `ply:Give`, `GM:PlayerSpawn`, `draw.RoundedBox`,
`Vector`... There are fewer functions, some work a little differently (the notes say where), and a few are this
game's own.

- [Where scripts go](#where-scripts-go)
- [Server and client](#server-and-client)
- [Gamemodes](#gamemodes): writing one, the `GM` settings, the hooks
- [Reference](#reference): every library and method
- [The built-in gamemodes](#the-built-in-gamemodes)
- [Limits and tips](#limits-and-tips)

## Where scripts go

All in the game's `data/lua` folder (`PSP/GAME/RetconsMod/data/lua` on the memory stick).

| File | Runs |
|---|---|
| `autorun/*.lua` | On every map, after the gamemode. Main menu > Workshop > Addons turns each one on or off. |
| `maps/<map>.lua` | Only on that map, after the addons. |
| `gamemodes/<name>.lua` | When that gamemode is picked: New Game's Triangle, or `gamemode <name>` on a server. |

The game's own library (`psp/lua/init.lua`, which builds this API) and its gamemodes (`psp/lua/gamemodes`) are
built into the EBOOT, so an update through Options > Updates brings them too. A file of yours in
`data/lua/gamemodes` with the same name as one of the game's replaces it: copy one out of
[psp/lua/gamemodes](../psp/lua/gamemodes) to change it.

`include("gamemodes/base.lua")` runs another file (yours in `data/lua` first, then the game's own) and returns
what it returns.

Errors show on screen and go in `data/lua_errors.txt`. A hook that errors is removed (a gamemode's own methods
stay, and the same error is only reported once in a row).

## Server and client

Like GMod, a game has a **server** that runs the rules and **clients** that show them:

- **Single player:** your PSP is both. `SERVER` and `CLIENT` are both true.
- **Hosting:** the host is both; the PSPs that join are clients (`SERVER` false, `CLIENT` true).
- **Dedicated server:** the server is just a server (`CLIENT` false: no HUD, no `LocalPlayer()`).

Every PSP in a game runs the same scripts. Functions that change the game (spawning, `ply:SetTeam`,
`ply:Give`, `SetGlobalInt`...) only work on the server; on a client they do nothing, and the server's changes
reach the clients by themselves. A gamemode's rules usually go in `if SERVER then` blocks or in hooks only the
server calls; its HUD goes in `HUDPaint`, which only clients call.

Everyone in a game needs the gamemode. The game's own come with it; for one of yours, every PSP needs the same
file (the server doesn't send it). A PSP without it still plays by the server's rules, just without that
gamemode's HUD.

## Gamemodes

A gamemode file fills in the `GM` table: a few settings, then methods the game calls. After the file has run,
`GM` is also `GAMEMODE`.

```lua
GM.Name = "Last Man Standing"  -- shown in New Game, the server list and the scoreboard
GM.RespawnTime = -1            -- the dead stay dead until the gamemode spawns them

function GM:PlayerSpawn(ply)
    ply:StripWeapons()
    ply:Give("crowbar")
    ply:Give("shotgun")
end

function GM:PlayerDeath(victim, inflictor, attacker)
    local alive = {}
    for _, p in ipairs(player.GetAll()) do
        if p:Alive() then alive[#alive + 1] = p end
    end
    if #alive == 1 then
        PrintMessage(HUD_PRINTCENTER, alive[1]:Nick() .. " is the last one standing!")
        timer.Simple(5, function()
            for _, p in ipairs(player.GetAll()) do p:Spawn() end
        end)
    end
end

function GM:HUDPaint()
    draw.Text(240, 8, "Last Man Standing", Color(255, 220, 0), TEXT_ALIGN_CENTER, "bold")
end
```

Save it as `data/lua/gamemodes/lastman.lua`; Triangle on the New Game screen now offers it.

### Settings

| Setting | Default | |
|---|---|---|
| `GM.Name` | the file name | Its name in menus. Written as `GM.Name = "..."` on a line of its own, the menus can read it without running the file. |
| `GM.Sandbox` | `false` | `true` keeps the sandbox: spawn menu, tool gun, physics gun, noclip, God mode and the rest. `false` (a game) turns them off: Select shows the scoreboard instead, players carry only what you `Give` them (and their hands), and nobody can spawn or build. |
| `GM.AllowPickup` | `false` | Use picks up light props (Zombie Survival's barricades). Otherwise Use goes to `PlayerUse`, then doors and buttons. |
| `GM.TeamBased` | `false` | Name tags only show for your own team. |
| `GM.ShowHealth` | `true` | **Ours:** `false` hides the health and suit boxes (a game where nobody gets hurt). |
| `GM.HandsUse` | `false` | **Ours:** `true` makes the hands' R use what it can't pick up (a frozen prop: Happy Burger's fridge, bun rack...), as Use does. |
| `GM.RespawnTime` | `-1` | Seconds after dying when X respawns you. `-1`: only when the gamemode calls `ply:Spawn()`. |

In a gamemode's game the Options God mode, Unlimited ammo, NPC thinking off and NPCs ignore players don't apply,
and the pause menu has no saves, noclip or cleanup.

### Hooks

The game calls these on the gamemode (`function GM:PlayerSpawn(ply)`) and on hooks added with `hook.Add` (which
run first: if one returns something, the gamemode's isn't called).

Where the rules run (`SERVER`):

| Hook | When | Return |
|---|---|---|
| `Initialize()` | The map's loaded and every script has run (every PSP) | |
| `Think(dt)` | Every frame (every PSP) | |
| `PlayerInitialSpawn(ply)` | Someone joined (and the host's own player at the start) | |
| `PlayerSpawn(ply)` | Someone (re)spawned: give them their loadout here | |
| `PlayerLoadout(ply)` | Straight after `PlayerSpawn` | |
| `PlayerSelectSpawn(ply)` | Where they'll spawn | a `Vector` (and a yaw), or nothing for one of the map's spawn points |
| `PlayerDeath(victim, inflictor, attacker)` | Someone died. `attacker` is the player who did it, or nil (the world, an NPC, a fall) | |
| `PlayerDisconnected(ply)` | Someone left | |
| `PlayerShouldTakeDamage(ply, attacker)` | A player or explosion is about to hurt them (`attacker` may be nil) | `false` stops it |
| `EntityTakeDamage(ent, dmginfo)` | A player shot a prop. `dmginfo:GetAttacker()`, `dmginfo:GetDamage()` | |
| `OnNPCKilled(npc, attacker, inflictor)` | An NPC died (`attacker`: a player, or nil) | |
| `PlayerUse(ply, ent)` | Use pressed, `ent` the prop in reach (or nil). In the sandbox it comes after doors and buttons; in a game, before. | `true` if you handled it |
| `PlayerQuickChat(ply, text)` | A quick chat message (Prop Hunt's taunts) | `true` keeps it out of the chat |

On every PSP with a player (`CLIENT`):

| Hook | When |
|---|---|
| `HUDPaint()` | Every frame, after the game's HUD: draw with `draw.*` |
| `HUDDrawScoreBoard()` | A gamemode method only: replaces the scoreboard (Select held, in a game) |

## Reference

### Basics

| | |
|---|---|
| `print(...)` / `Msg` / `MsgN` | A line on screen (and in a dedicated server's log) |
| `CurTime()` | Seconds since the map started |
| `FrameTime()` | This frame's length in seconds |
| `SERVER`, `CLIENT` | See [Server and client](#server-and-client) |
| `MAP` | The map's name |
| `GAMEMODE_NAME`, `engine.ActiveGamemode()` | The gamemode's file name (`"sandbox"` for none) |
| `DIFFICULTY` | **Ours:** the difficulty picked before the game (Happy Burger's `"tutorial"`, `"easy"`, `"medium"`, `"hard"` or `"extreme"`; `"medium"` if none). Only the PSP that picked it knows it: share it with `SetGlobalString` for the others' HUD |
| `GM`, `GAMEMODE` | The gamemode's table (nil in the sandbox) |
| `MAX_PLAYERS` | Player slots: 8 |
| `IsValid(x)` | x is a player, prop or NPC that still exists |
| `include(path)` | See [Where scripts go](#where-scripts-go) |

### Vector

`Vector(x, y, z)`, with `+ - * /` (a number or another vector), `==`, `tostring`, and `v.x v.y v.z`.
Methods: `Length()`, `LengthSqr()`, `Length2D()`, `Distance(v)`, `DistToSqr(v)`, `Dot(v)`, `Cross(v)`,
`GetNormalized()`, `Normalize()`, `Add(v)`, `Sub(v)`, `Mul(n)`, `IsZero()`, `Unpack()`, `Yaw()` (degrees, round
the up axis). `isvector(x)`. Units are the maps' own: a player is 72 tall.

### Colours

`Color(r, g, b [, a])` makes a colour as a number (ABGR, the way the draw functions take it). Unlike GMod's it's
not a table: there's no `.r`.

### hook and timer

| | |
|---|---|
| `hook.Add(event, name, fn)` / `hook.Remove(event, name)` / `hook.GetTable()` | |
| `hook.Run(event, ...)` | Calls the hooks, then `GAMEMODE`'s method |
| `hook.Call(event, gm, ...)` | The same with any table as the gamemode |
| `timer.Simple(seconds, fn)` | Once |
| `timer.Create(name, seconds, reps, fn)` | `reps` times (0: for ever); the same name replaces it |
| `timer.Remove(name)` / `timer.Exists(name)` / `timer.TimeLeft(name)` | |

### Players

`player.GetAll()`, `player.GetCount()`, `player.GetBySlot(slot)`, and `LocalPlayer()` (this PSP's player; nil
on a dedicated server). Players are the same object for as long as they stay, so they work as table keys.

| Method | |
|---|---|
| `Nick()` / `Name()` | |
| `UserID()` / `EntIndex()` | Their slot, 0 to 7 (0 is the host's, on a PSP host) |
| `IsValid()`, `IsPlayer()` (true), `IsNPC()` (false), `IsLocalPlayer()` | |
| `Alive()`, `Health()`, `SetHealth(hp)`, `GetMaxHealth()`, `SetMaxHealth(hp)` | Max health is what they spawn with |
| `Kill()` / `KillSilent()` | `KillSilent` skips `PlayerDeath` (to watch until the next round) |
| `Spawn()` | Alive again at a spawn point (`PlayerSelectSpawn`), with `PlayerSpawn` and `PlayerLoadout` |
| `GetPos()`, `SetPos(v [, yaw])` | Feet. `SetPos` moves them as they are |
| `EyePos()`, `GetAimVector()`, `GetYaw()`, `GetEyeTrace()` | |
| `Team()`, `SetTeam(id)` | 0 is `TEAM_UNASSIGNED` |
| `Freeze(on)` / `IsFrozen()` | Can't move (they can still look round and shoot) |
| `SetBlind(on)` / `IsBlind()` | **Ours:** a black screen (the HUD still draws over it) |
| `SetNoTarget(on)` | NPCs leave them alone |
| `SetWalkSpeed(u)` / `GetWalkSpeed()` | Units a second, 250 being the usual (`SetRunSpeed` is the same: there's no running) |
| `SetModel(name)` / `GetModel()` | **Ours:** a character by its spawn menu name (`"Zombie"`, `"Combine"`); nil or `""` for their own |
| `Give(weapon)`, `StripWeapon(weapon)`, `StripWeapons()`, `HasWeapon(weapon)`, `GetWeapons()` | Hands always stay |
| `SelectWeapon(weapon)`, `GetActiveWeapon()` | `GetActiveWeapon` gives the weapon's name, not an entity |
| `GiveAmmo(amount, weapon)`, `RefillAmmo()` | `RefillAmmo` (ours): full clips and spare ammo for everything they carry |
| `PickUp(ent)` | **Ours:** the prop into their hands, if they hold nothing. Only for the player on the PSP running the rules (single player, or the host's own); `false` otherwise. |
| `Disguise(ent)`, `Undisguise()`, `GetDisguise()`, `IsDisguised()` | **Ours (Prop Hunt):** they look like that prop to everyone, are shot as it, move in a box its size, and see themselves from behind. `GetDisguise` gives its model name |
| `ChatPrint(text)`, `PrintMessage(HUD_PRINTTALK or HUD_PRINTCENTER, text)` | To them only |
| `EmitSound(name)` | Everyone hears it from where they are |
| `Frags()`, `SetFrags(n)`, `AddFrags(n)`, `Deaths()`, `SetDeaths(n)`, `AddDeaths(n)` | The scoreboard's columns |
| `SetNWInt/Float/String/Bool(key, v)`, `GetNWInt/Float/String/Bool(key, default)` | Networked: see below |

**Weapons** by name: `hands`, `physgun`, `gravgun`, `toolgun`, `crowbar`, `pistol`, `357`, `smg`, `shotgun`,
`ar2`, `crossbow`, `rpg`, `grenade`, `bugbait`. GMod's names work too (`weapon_smg1`, `weapon_physcannon`...).

Add your own methods to `PLAYER` (the players' metatable): `function PLAYER:IsHunter() ... end`.

### Teams

`team.SetUp(id, name, color)` in the gamemode's file (every PSP runs it, so everyone knows the names), then
`team.GetName(id)`, `team.GetColor(id)`, `team.GetAllTeams()`, `team.GetPlayers(id)`, `team.NumPlayers(id)`,
`team.GetScore(id)`, `team.SetScore(id, n)`, `team.AddScore(id, n)`, `team.BestAutoJoinTeam([ids])` (the one
with fewest players).

### Networked variables

The server's values, on every PSP: `SetGlobalInt/Float/String/Bool(key, value)` and
`GetGlobalInt/Float/String/Bool(key, default)`, and the same per player with `ply:SetNWInt(...)`. Use them for
what the HUD shows: round times (`SetGlobalFloat("round_end", CurTime() + 180)`, then
`GetGlobalFloat("round_end") - CurTime()` on a client), scores, whose turn it is. They're sent four times a
second; keep them to a few dozen short ones (about 1 KB in all).

### Messages and sounds

| | |
|---|---|
| `PrintMessage(HUD_PRINTTALK, text)` | A chat line for everyone |
| `PrintMessage(HUD_PRINTCENTER, text)` | Big text across the middle of everyone's screen for 4 seconds |
| `sound.Play(name [, pos])` | Everyone hears it: at `pos`, or everywhere |
| `surface.PlaySound(name)` | Just this PSP |

Sounds are the game's own by name (`tools/sounds.txt`): `button`, `spawn`, `explode`, `balloon_pop`,
`charge_done_suit`, `vo_zombie_alert`, `vo_combine_alert`, `vo_heavy_alert`... A name with several sounds picks
one at random.

### Props

`ents.Create(name, pos [, yaw])` spawns a prop by its spawn menu name (`"Wood Crate"`, `"Oil Drum"`) and returns
it (nil if there's no such prop). `ents.GetAll()`, `ents.GetByIndex(i)` / `Entity(i)`, `ents.FindInSphere(pos,
radius)`, `ents.FindByModel(name)`.

| Method | |
|---|---|
| `IsValid()`, `EntIndex()`, `GetClass()` (`"prop_physics"`), `IsMapProp()` | `IsMapProp` (ours): the map's own, not spawned |
| `GetModel()` | Its spawn menu name (the map's own props: their model path) |
| `GetPos()`, `SetPos(v)`, `GetVelocity()`, `SetVelocity(v)` | |
| `SetColor(Color(...))` or `SetColor(r, g, b)` | |
| `Freeze(on)`, `IsFrozen()`, `GetMass()` | |
| `OBBMins()`, `OBBMaxs()`, `GetSize()` | Its box about its centre; `GetSize` (ours) is maxs - mins |
| `Remove()`, `EmitSound(name)` | |

The first API's functions still work and take a prop's number: `ents.CreateProp(name, x, y, z, yaw)`,
`ents.Remove/IsValid/GetPos/SetPos/SetVelocity/SetColor/Freeze`, `player.GetPos()`, `player.GetYaw()`,
`player.GetAimEntity()`.

### NPCs

`npc.Spawn(name, pos [, yaw])` by spawn menu name (`"Zombie"`, `"Fast Zombie"`, `"Combine"`), nil when there's no
room (16 at a time, headcrabs and torsos included). `npc.GetAll()` (the living ones), `npc.Count()`,
`npc.GetTypes()`.

Methods: `IsValid()`, `Alive()`, `GetClass()`, `GetPos()`, `SetPos(v)`, `Health()`, `SetHealth(hp)`,
`TakeDamage(amount [, attacker])`, `Remove()`, `IsNPC()`, `EntIndex()`.

**Ours (Happy Burger's engine):** `SetGoal(pos [, yaw [, run]])` walks it to `pos` (round walls along the map's
node graph, running if `run`), then it stands there facing `yaw`. Its own AI is off meanwhile: it doesn't
wander, flee or fight, though it can still be hurt. `AtGoal()` is true once it's there, and `ClearGoal()`
hands it back to its AI. Retcon's Mod 1.14.1 and earlier don't have them: check `if n.SetGoal then`.

### The map

| | |
|---|---|
| `game.GetMap()` | |
| `game.GetSpawnPoints()` | `{pos = Vector, yaw = degrees}` for each of the map's player spawns |
| `game.CleanUpMap()` | Everything as the map loaded: spawned props, NPCs and ragdolls gone, its own props back |
| `game.IsDedicated()` | |
| `game.UnlockAchievement(id)` | The game's gamemodes' achievements, for this PSP's player |
| `navmesh.GetNodeCount()`, `navmesh.GetNode(i)`, `navmesh.GetRandomPoint()` | The map's NPC node graph: places to walk to (`GetRandomPoint` is nil on maps without one) |
| `util.TraceLine({start = v, endpos = v, filter = ply})` | `{Hit, HitPos, Fraction, Entity, HitWorld}`; `Entity` is a prop or player |

### Drawing (in `HUDPaint`)

The screen is 480 x 272 (`ScrW()`, `ScrH()`).

| | |
|---|---|
| `draw.Text(x, y, text [, color [, align [, font]]])` / `draw.SimpleText` | `align`: `TEXT_ALIGN_LEFT`, `TEXT_ALIGN_CENTER`, `TEXT_ALIGN_RIGHT`; `font`: `"small"`, `"bold"`, `"large"`, `"title"` |
| `draw.Rect(x, y, w, h, color)` | |
| `draw.RoundedBox(radius, x, y, w, h, color)` | |
| `draw.TextWidth(text [, font])` | |
| `input.IsButtonDown(name)` | `"select"`, `"start"`, `"up"`, `"down"`, `"left"`, `"right"`, `"l"`, `"r"`, `"cross"`, `"circle"`, `"square"`, `"triangle"`. Nearly all of them are the game's; in a game Select is free (the scoreboard). |

## The built-in gamemodes

Their source is in [psp/lua/gamemodes](../psp/lua/gamemodes): copy one into `data/lua/gamemodes` to change it.

- **Prop Hunt** (`prophunt`): props hide as things on the map while the hunters are blindfolded for 30 seconds,
  then the hunters have three minutes to find them; every shot at a real prop costs a hunter 5 health. Props
  press Use on a prop to become it (bigger ones have more health, up to 200) and taunt with quick chat. Teams swap
  each round. On maps with few physics props, some are scattered about. Two players or more; alone, you can try
  out the disguises.
- **Zombie Survival** (`zombiesurvival`): waves of zombies, bigger and nastier each time (fast zombies from wave
  3, poison zombies from 5, zombines from 7), and more weapons with each wave. Beat a wave and the dead come back,
  with ammo and supplies about; lose everyone at once and it starts again. Use picks up light props to block
  doors. Alone or together.
- **Happy Burger** (`burger`): run a burger stand. The kitchen is built from props near a spawn point;
  citizens walk up to the counter (`NPC:SetGoal`) and order. Grill patties and hot dogs, fry fries, grab sodas,
  and put the food on their tray before their patience runs out. A day's quota or you're fired. Alone or
  together.
- **Deathmatch** (`deathmatch`): everyone against everyone; first to 15 kills, or the most after ten minutes. X
  respawns you after 3 seconds. Alone, Combine soldiers fill in. The shortest of the three: a good one to start
  from.

## Limits and tips

- The PSP's CPU is slow next to a PC's: keep `Think` and `HUDPaint` light, and use `timer.Create` for anything
  that doesn't need every frame (the game's gamemodes check their rules twice a second).
- 16 NPCs at a time (headcrabs from zombies and Zombie Torsos included), about 40 spawned props.
- Players can't type, so there's no `PlayerSay`; `PlayerQuickChat` gets their quick chat messages.
- On a dedicated server the game needs to be built with Lua (it is when the PC has Lua 5.4's headers; see the
  server's README), or a gamemode falls back to the sandbox.
- Test on your own first: single player runs the server and the client side.
