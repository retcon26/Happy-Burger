-- Zombie Survival: waves of zombies, each bigger and nastier than the last. Survive a wave and everyone who
-- died comes back, with more ammo and a few supplies about. Lose everyone at once and it starts again.
-- Plays alone or with others (they're on your side: no friendly fire). Use picks up light props, so you
-- can block the doors.
GM.Name = "Zombie Survival"
GM.AllowPickup = true -- Use picks up light props: barricades
-- (GM.RespawnTime stays -1: the dead come back when the wave's beaten)

local BREAK_TIME = 20   -- seconds between waves
local FIRST_BREAK = 12  -- before the first
local OVER_TIME = 10    -- after everyone's died
local MAX_ALIVE = 10    -- zombies at once (the game has room for 16 NPCs, headcrabs and torsos included)

-- What each wave sends: more of them, and from wave 3 faster and tougher kinds
local function wave_size(wave)
    return 4 + wave * 2
end

local function zombie_type(wave)
    local r = math.random()
    if wave >= 7 and r < 0.15 then return "Zombine" end
    if wave >= 5 and r < 0.30 then return "Poison Zombie" end
    if wave >= 3 and r < 0.55 then return "Fast Zombie" end
    return "Zombie"
end

-- What you carry grows with the waves
local function loadout(ply, wave)
    ply:Give("crowbar")
    ply:Give("pistol")
    ply:Give("smg")
    if wave >= 3 then ply:Give("shotgun") end
    if wave >= 5 then
        ply:Give("ar2")
        ply:Give("grenade")
    end
    if wave >= 8 then ply:Give("357") end
end

local SUPPLIES = {"Health Kit", "Health Kit", "SMG Ammo", "Pistol Ammo", "Shotgun Shells", "Suit Battery", "AR2 Ammo"}

-- The rules (single player or the host) -------------------------------------------------------------------

local state = {phase = "break", wave = 1, to_spawn = 0, ends = 0}

local function set_phase(phase, seconds)
    state.phase = phase
    state.ends = CurTime() + seconds
    SetGlobalString("zs_phase", phase)
    SetGlobalFloat("zs_end", state.ends)
    SetGlobalInt("zs_wave", state.wave)
end

local function living()
    local t = {}
    for _, p in ipairs(player.GetAll()) do
        if p:Alive() then t[#t + 1] = p end
    end
    return t
end

-- Somewhere 650 to 1100 units from a living player (close enough that they come for you), on the map's
-- node graph if it has one
local function spawn_spot()
    local alive = living()
    if #alive == 0 then return nil end
    for _ = 1, 16 do
        local target = alive[math.random(#alive)]:GetPos()
        local spot = navmesh.GetRandomPoint()
        if not spot then
            local a = math.random() * 2 * math.pi
            local d = math.Rand(650, 1000)
            spot = target + Vector(math.cos(a) * d, math.sin(a) * d, 0)
            -- Only where there's open floor: a clear line from up high down to it
            local tr = util.TraceLine({start = spot + Vector(0, 0, 200), endpos = spot - Vector(0, 0, 200)})
            if tr.Fraction <= 0 or not tr.Hit then spot = nil else spot = tr.HitPos + Vector(0, 0, 8) end
        end
        if spot then
            local near = math.huge
            for _, p in ipairs(alive) do near = math.min(near, p:GetPos():Distance(spot)) end
            if near > 650 and near < 1100 then return spot end
        end
    end
    -- None found: one of the map's spawn points away from everyone
    local best, far = nil, 0
    for _, sp in ipairs(game.GetSpawnPoints()) do
        local near = math.huge
        for _, p in ipairs(alive) do near = math.min(near, p:GetPos():Distance(sp.pos)) end
        if near > far then best, far = sp.pos, near end
    end
    return best
end

local function zombies_alive()
    return npc.Count()
end

local function scatter_supplies()
    local alive = living()
    for _ = 1, 2 + #player.GetAll() do
        local near = alive[math.random(math.max(1, #alive))]
        local spot = navmesh.GetRandomPoint()
        if near and (not spot or spot:Distance(near:GetPos()) > 700) then
            local a = math.random() * 2 * math.pi
            spot = near:GetPos() + Vector(math.cos(a) * 80, math.sin(a) * 80, 24)
        end
        if spot then ents.Create(SUPPLIES[math.random(#SUPPLIES)], spot + Vector(0, 0, 16)) end
    end
end

local function start_wave()
    state.to_spawn = wave_size(state.wave)
    set_phase("wave", 0)
    SetGlobalInt("zs_left", state.to_spawn)
    PrintMessage(HUD_PRINTCENTER, "Wave " .. state.wave .. ": here they come!")
    sound.Play("vo_zombie_alert")
end

local function wave_beaten()
    PrintMessage(HUD_PRINTCENTER, "Wave " .. state.wave .. " survived!")
    sound.Play("charge_done_suit")
    SetGlobalInt("zs_survived", state.wave)
    state.wave = state.wave + 1
    -- Everyone back, with full ammo and the next wave's weapons
    for _, p in ipairs(player.GetAll()) do
        if p:Alive() then
            loadout(p, state.wave)
            p:RefillAmmo()
            p:SetHealth(math.min(p:GetMaxHealth(), p:Health() + 25))
        else
            p:Spawn()
        end
    end
    scatter_supplies()
    set_phase("break", BREAK_TIME)
end

local function new_game()
    game.CleanUpMap()
    state.wave = 1
    SetGlobalInt("zs_survived", 0)
    for _, p in ipairs(player.GetAll()) do
        p:SetFrags(0)
        p:SetDeaths(0)
        p:Spawn()
    end
    set_phase("break", FIRST_BREAK)
end

local function tick()
    local now = CurTime()
    if state.phase == "break" and now >= state.ends then
        start_wave()
    elseif state.phase == "wave" then
        local alive = zombies_alive()
        while state.to_spawn > 0 and alive < MAX_ALIVE do
            local spot = spawn_spot()
            if not spot or not npc.Spawn(zombie_type(state.wave), spot, math.random(0, 359)) then break end
            state.to_spawn = state.to_spawn - 1
            alive = alive + 1
        end
        SetGlobalInt("zs_left", state.to_spawn + alive)
        if #living() == 0 and player.GetCount() > 0 then
            PrintMessage(HUD_PRINTCENTER, "The zombies got everyone. Waves survived: " .. (state.wave - 1))
            sound.Play("vo_male_die")
            set_phase("over", OVER_TIME)
        elseif state.to_spawn == 0 and alive == 0 then
            wave_beaten()
        end
    elseif state.phase == "over" and now >= state.ends then
        new_game()
    end
end

function GM:Initialize()
    if not SERVER then return end
    set_phase("break", FIRST_BREAK)
    timer.Create("zs_tick", 0.5, 0, tick)
end

function GM:PlayerInitialSpawn(ply)
    -- Mid-wave: watch until it's beaten
    if state.phase ~= "break" then
        ply:KillSilent()
        ply:ChatPrint("A wave is on: you'll join in when it's beaten")
    end
end

function GM:PlayerSpawn(ply)
    ply:StripWeapons()
    loadout(ply, state.wave)
    ply:SelectWeapon("smg")
end

-- On the same side: players can't hurt each other
function GM:PlayerShouldTakeDamage(ply, attacker)
    if IsValid(attacker) and attacker:IsPlayer() and attacker ~= ply then return false end
    return true
end

function GM:PlayerDeath(ply, inflictor, attacker)
    ply:AddDeaths(1)
    PrintMessage(HUD_PRINTTALK, ply:Nick() .. " was overrun")
end

function GM:OnNPCKilled(zombie, attacker)
    if IsValid(attacker) then attacker:AddFrags(1) end
end

-- Every PSP: the HUD -------------------------------------------------------------------------------------

local YELLOW, WHITE, GREY = Color(255, 220, 0), Color(255, 255, 255), Color(200, 200, 200)
local unlocked = false

function GM:HUDPaint()
    local phase, wave = GetGlobalString("zs_phase", "break"), GetGlobalInt("zs_wave", 1)
    local left = math.max(0, GetGlobalFloat("zs_end") - CurTime())
    local line
    if phase == "wave" then
        line = GetGlobalInt("zs_left") .. " zombies left"
    elseif phase == "break" then
        line = "Next wave in " .. string.ToMinutesSeconds(left)
    else
        line = "Starting again in " .. math.ceil(left)
    end
    draw.RoundedBox(6, 170, 4, 140, 36, Color(0, 0, 0, 150))
    draw.Text(240, 6, "WAVE " .. wave, YELLOW, TEXT_ALIGN_CENTER, "bold")
    draw.Text(240, 22, line, WHITE, TEXT_ALIGN_CENTER)
    local me = LocalPlayer()
    if me then
        draw.Text(12, 6, "Kills: " .. me:Frags(), GREY)
        if not unlocked and GetGlobalInt("zs_survived") >= 10 and me:Alive() then
            unlocked = true
            game.UnlockAchievement("survivor")
        end
    end
end
