-- Deathmatch: everyone against everyone with HL2's guns. First to 15 kills (or the most after ten minutes)
-- wins, then a new game starts. Alone, Combine soldiers fill in. The smallest of the game's gamemodes: a
-- good one to copy when you write your own (docs/LUA.md).
GM.Name = "Deathmatch"
GM.RespawnTime = 3 -- X respawns you this long after you die

local FRAG_LIMIT = 15
local TIME_LIMIT = 600
local BOTS = 3 -- Combine soldiers when you're on your own

local function new_game()
    SetGlobalFloat("dm_end", CurTime() + TIME_LIMIT)
    SetGlobalInt("dm_winner", 0)
    for _, p in ipairs(player.GetAll()) do
        p:SetFrags(0)
        p:SetDeaths(0)
        p:Freeze(false)
        p:Spawn()
    end
end

local function finish(winner)
    if GetGlobalInt("dm_winner") ~= 0 then return end
    if winner then
        PrintMessage(HUD_PRINTCENTER, winner:Nick() .. " wins with " .. winner:Frags() .. " kills!")
        if player.GetCount() >= 2 then SetGlobalInt("dm_winner", winner:UserID() + 1) else SetGlobalInt("dm_winner", -1) end
    else
        PrintMessage(HUD_PRINTCENTER, "Nobody wins")
        SetGlobalInt("dm_winner", -1)
    end
    for _, p in ipairs(player.GetAll()) do p:Freeze(true) end
    timer.Simple(8, new_game)
end

-- Where to come back: the spawn point (or spot on the map's node graph) furthest from everyone else
function GM:PlayerSelectSpawn(ply)
    local spots = game.GetSpawnPoints()
    for _ = 1, 10 do
        local p = navmesh.GetRandomPoint()
        if p then spots[#spots + 1] = {pos = p, yaw = math.random(0, 359)} end
    end
    local best, far = nil, -1
    for _, sp in ipairs(spots) do
        local near = math.huge
        for _, o in ipairs(player.GetAll()) do
            if o ~= ply and o:Alive() then near = math.min(near, o:GetPos():Distance(sp.pos)) end
        end
        if near > far then best, far = sp, near end
    end
    if best then return best.pos, best.yaw end
end

function GM:PlayerSpawn(ply)
    ply:StripWeapons()
    for _, w in ipairs({"crowbar", "pistol", "smg", "shotgun", "357", "grenade"}) do ply:Give(w) end
    ply:SelectWeapon("smg")
end

function GM:PlayerDeath(victim, inflictor, attacker)
    victim:AddDeaths(1)
    if IsValid(attacker) and attacker ~= victim then
        attacker:AddFrags(1)
        PrintMessage(HUD_PRINTTALK, attacker:Nick() .. " killed " .. victim:Nick())
        if attacker:Frags() >= FRAG_LIMIT then finish(attacker) end
    else
        victim:AddFrags(-1)
        PrintMessage(HUD_PRINTTALK, victim:Nick() .. " died")
    end
end

function GM:OnNPCKilled(soldier, attacker)
    if IsValid(attacker) then
        attacker:AddFrags(1)
        if attacker:Frags() >= FRAG_LIMIT then finish(attacker) end
    end
end

function GM:Initialize()
    if not SERVER then return end
    new_game()
    timer.Create("dm_tick", 1, 0, function()
        if GetGlobalInt("dm_winner") == 0 and CurTime() >= GetGlobalFloat("dm_end") then
            local best
            for _, p in ipairs(player.GetAll()) do
                if not best or p:Frags() > best:Frags() then best = p end
            end
            finish(best)
        end
        -- On your own: soldiers to fight (gone once someone joins)
        local alone = player.GetCount() == 1
        if alone and npc.Count() < BOTS and GetGlobalInt("dm_winner") == 0 then
            local me = player.GetAll()[1]
            local spot = navmesh.GetRandomPoint()
            if spot and me and spot:Distance(me:GetPos()) > 500 then npc.Spawn("Combine", spot, math.random(0, 359)) end
        elseif not alone then
            for _, n in ipairs(npc.GetAll()) do n:Remove() end
        end
    end)
end

-- Every PSP: the HUD -------------------------------------------------------------------------------------

local unlocked = false

function GM:HUDPaint()
    local me = LocalPlayer()
    if not me then return end
    local left = math.max(0, GetGlobalFloat("dm_end") - CurTime())
    local leader
    for _, p in ipairs(player.GetAll()) do
        if not leader or p:Frags() > leader:Frags() then leader = p end
    end
    draw.RoundedBox(6, 180, 4, 120, 36, Color(0, 0, 0, 150))
    draw.Text(240, 6, string.ToMinutesSeconds(left), Color(255, 220, 0), TEXT_ALIGN_CENTER, "bold")
    draw.Text(240, 22, "Kills: " .. me:Frags() .. " / " .. FRAG_LIMIT, Color(255, 255, 255), TEXT_ALIGN_CENTER)
    if leader and leader ~= me then
        draw.Text(12, 6, "Leader: " .. leader:Nick() .. " (" .. leader:Frags() .. ")", Color(200, 200, 200))
    end
    if not unlocked and GetGlobalInt("dm_winner") == me:UserID() + 1 then
        unlocked = true
        game.UnlockAchievement("top_frag")
    end
end
