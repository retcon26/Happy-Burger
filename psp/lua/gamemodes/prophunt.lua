-- Prop Hunt: the props hide as things on the map, the hunters have three minutes to find them. While
-- the props hide the hunters are blindfolded; after that, every shot at a real prop costs them health.
-- Props: Use on a prop to become it (bigger ones have more health), quick chat to taunt. The teams swap
-- every round. Needs two players or more; on your own you can try out the disguises.
GM.Name = "Prop Hunt"
GM.TeamBased = true -- name tags only for your own team

TEAM_HUNTERS, TEAM_PROPS = 1, 2
team.SetUp(TEAM_HUNTERS, "Hunters", Color(80, 160, 255))
team.SetUp(TEAM_PROPS, "Props", Color(255, 140, 60))

local HIDE_TIME = 30
local HUNT_TIME = 180
local OVER_TIME = 8
local PENALTY = 5        -- health a hunter loses for shooting a real prop
local MAX_SIZE = 160     -- props bigger than this (in any direction) can't be become
local MIN_PROPS = 16     -- maps with fewer physics props get some scattered about

local SCATTER = {"Wood Crate", "Cardboard Box", "Oil Drum", "Blue Barrel", "Traffic Cone", "Watermelon", "Bucket",
                 "Plastic Crate", "Trash Can", "Chair", "Office Chair", "Dresser", "Lockers", "Monitor", "Doll",
                 "Paint Can", "Small Box", "Big Box", "Suitcase", "Toilet", "Kettle", "Computer", "Globe", "Tire",
                 "Soccer Ball", "File Cabinet", "Radiator", "Gas Can"}
local TAUNTS = {"vo_male_alert", "vo_female_alert", "vo_alyx_alert", "vo_barney_alert", "vo_heavy_alert",
                "vo_scout_alert", "vo_soldier_alert", "vo_medic_alert", "vo_spy_alert", "vo_engineer_alert",
                "balloon_pop", "vo_headcrab_alert"}

-- The rules (single player or the host) -------------------------------------------------------------------

local state = {phase = "waiting", round = 0, ends = 0, hunters_first = {}}

local function set_phase(phase, seconds)
    state.phase = phase
    state.ends = CurTime() + seconds
    SetGlobalString("ph_phase", phase)
    SetGlobalFloat("ph_end", state.ends)
    SetGlobalInt("ph_round", state.round)
end

local function alive_in(t)
    local n = 0
    for _, p in ipairs(team.GetPlayers(t)) do
        if p:Alive() then n = n + 1 end
    end
    return n
end

-- A prop's health goes with its size: a can 10, a barrel about 70, a big crate 200
local function prop_health(ent)
    local s = ent:GetSize()
    return math.Clamp(math.floor(s.x * s.y * s.z / 400), 10, 200)
end

local function become(ply, ent)
    local s = ent:GetSize()
    if math.max(s.x, s.y, s.z) > MAX_SIZE then
        ply:ChatPrint("That's too big to hide as")
        return false
    end
    if not ply:Disguise(ent) then return false end
    -- Keep the share of health they had
    local share = ply:Health() / math.max(1, ply:GetMaxHealth())
    local hp = prop_health(ent)
    ply:SetMaxHealth(hp)
    ply:SetHealth(math.max(1, math.floor(hp * share + 0.5)))
    return true
end

-- Something to be straight away: the nearest prop small enough
local function disguise_nearest(ply)
    local best, dist = nil, math.huge
    for _, e in ipairs(ents.FindInSphere(ply:GetPos(), 1000)) do
        local s, d = e:GetSize(), e:GetPos():Distance(ply:GetPos())
        if math.max(s.x, s.y, s.z) <= MAX_SIZE and d < dist then best, dist = e, d end
    end
    if best then become(ply, best) end
end

local function enough_props()
    local n = 0
    for _, e in ipairs(ents.GetAll()) do
        local s = e:GetSize()
        if math.max(s.x, s.y, s.z) <= MAX_SIZE then n = n + 1 end
    end
    local spots = game.GetSpawnPoints()
    for k = n + 1, MIN_PROPS do
        -- Half about the map (its node graph, where there is one), half near where everyone starts
        local spot = k % 2 == 0 and navmesh.GetRandomPoint() or nil
        if not spot then
            local sp = spots[math.random(#spots)].pos
            local a, d = math.random() * 2 * math.pi, math.Rand(80, 500)
            spot = sp + Vector(math.cos(a) * d, math.sin(a) * d, 0)
            local tr = util.TraceLine({start = spot + Vector(0, 0, 100), endpos = spot - Vector(0, 0, 300)})
            spot = tr.Hit and tr.Fraction > 0 and tr.HitPos or nil
        end
        if spot then ents.Create(SCATTER[math.random(#SCATTER)], spot + Vector(0, 0, 30), math.random(0, 359)) end
    end
end

local function start_round()
    local all = player.GetAll()
    if #all < 2 then
        if state.phase ~= "waiting" or not state.scattered then
            state.scattered = true
            enough_props() -- something to try being while you wait
        end
        set_phase("waiting", 0)
        for _, p in ipairs(all) do
            p:SetTeam(TEAM_PROPS)
            if not p:Alive() then
                p:Spawn()
            elseif not p:IsDisguised() then
                disguise_nearest(p)
            end
        end
        return
    end
    state.round = state.round + 1
    game.CleanUpMap()
    enough_props()
    -- The teams swap each round; the first round is a random half
    table.Shuffle(all)
    local hunters = math.max(1, #all // 2)
    for k, p in ipairs(all) do
        local t
        if state.round == 1 or p:Team() == TEAM_UNASSIGNED then
            t = k <= hunters and TEAM_HUNTERS or TEAM_PROPS
        else
            t = p:Team() == TEAM_HUNTERS and TEAM_PROPS or TEAM_HUNTERS
        end
        p:SetTeam(t)
    end
    if team.NumPlayers(TEAM_HUNTERS) == 0 then all[1]:SetTeam(TEAM_HUNTERS) end
    if team.NumPlayers(TEAM_PROPS) == 0 then all[#all]:SetTeam(TEAM_PROPS) end
    set_phase("hiding", HIDE_TIME)
    for _, p in ipairs(all) do p:Spawn() end
    PrintMessage(HUD_PRINTCENTER, "Round " .. state.round .. ": props, hide!")
    sound.Play("button")
end

local function end_round(winner, why)
    if state.phase == "over" or state.phase == "waiting" then return end
    team.AddScore(winner, 1)
    SetGlobalInt("ph_winner", winner)
    SetGlobalInt("ph_won_round", state.round)
    PrintMessage(HUD_PRINTCENTER, team.GetName(winner) .. " win! " .. why)
    sound.Play(winner == TEAM_PROPS and "balloon_pop" or "charge_done_suit")
    for _, p in ipairs(player.GetAll()) do
        p:Freeze(false)
        p:SetBlind(false)
    end
    set_phase("over", OVER_TIME)
end

local function start_hunt()
    set_phase("hunting", HUNT_TIME)
    for _, p in ipairs(team.GetPlayers(TEAM_HUNTERS)) do
        p:Freeze(false)
        p:SetBlind(false)
    end
    PrintMessage(HUD_PRINTCENTER, "Ready or not, here they come!")
    sound.Play("vo_combine_alert")
end

local function check_winner()
    if state.phase ~= "hiding" and state.phase ~= "hunting" then return end
    if alive_in(TEAM_PROPS) == 0 then
        end_round(TEAM_HUNTERS, "Every prop was found")
    elseif alive_in(TEAM_HUNTERS) == 0 then
        end_round(TEAM_PROPS, "The hunters are out")
    end
end

local function tick()
    local now = CurTime()
    if state.phase == "waiting" then
        if player.GetCount() >= 2 then start_round() end
    elseif state.phase == "hiding" and now >= state.ends then
        start_hunt()
    elseif state.phase == "hunting" and now >= state.ends then
        end_round(TEAM_PROPS, "Time's up")
    elseif state.phase == "over" and now >= state.ends then
        start_round()
    end
    check_winner()
end

function GM:Initialize()
    if not SERVER then return end
    set_phase("waiting", 0)
    timer.Create("ph_tick", 0.5, 0, tick)
    timer.Simple(0.5, start_round)
end

function GM:PlayerInitialSpawn(ply)
    if state.phase == "hiding" or state.phase == "hunting" then
        -- A round's on: watch it, then play the next
        ply:SetTeam(TEAM_UNASSIGNED)
        ply:KillSilent()
        ply:ChatPrint("A round is on: you'll play in the next one")
    elseif state.phase == "waiting" then
        ply:SetTeam(TEAM_PROPS)
    end
end

function GM:PlayerSpawn(ply)
    ply:Undisguise()
    ply:StripWeapons()
    ply:SetMaxHealth(100)
    ply:SetHealth(100) -- not the last disguise's
    ply:Freeze(false)
    ply:SetBlind(false)
    if ply:Team() == TEAM_HUNTERS then
        ply:SetModel("Combine")
        ply:Give("crowbar")
        ply:Give("smg")
        ply:Give("shotgun")
        ply:Give("grenade")
        ply:SelectWeapon("smg")
        if state.phase == "hiding" then
            ply:Freeze(true)
            ply:SetBlind(true)
        end
    else
        ply:SetModel(nil)
        disguise_nearest(ply)
    end
end

-- Props become what they Use
function GM:PlayerUse(ply, ent)
    if ply:Team() ~= TEAM_PROPS or not ply:Alive() or not IsValid(ent) or state.phase == "over" then return end
    if become(ply, ent) then
        ply:EmitSound("physgun_drop")
        return true
    end
end

function GM:PlayerShouldTakeDamage(ply, attacker)
    if not IsValid(attacker) or attacker == ply then return true end
    if state.phase ~= "hunting" then return false end
    return attacker:Team() ~= ply:Team()
end

-- Shooting the wrong prop hurts the hunter: once a shot (a shotgun's pellets all land in the same frame)
local penalised = {}
function GM:EntityTakeDamage(ent, dmg)
    local hunter = dmg:GetAttacker()
    if state.phase ~= "hunting" or not IsValid(hunter) or hunter:Team() ~= TEAM_HUNTERS or not hunter:Alive() then return end
    if penalised[hunter] == CurTime() then return end
    penalised[hunter] = CurTime()
    local hp = hunter:Health() - PENALTY
    if hp <= 0 then
        PrintMessage(HUD_PRINTTALK, hunter:Nick() .. " shot one prop too many")
        hunter:Kill()
    else
        hunter:SetHealth(hp)
    end
end

function GM:PlayerDeath(ply, inflictor, attacker)
    ply:AddDeaths(1)
    ply:Undisguise()
    if IsValid(attacker) and attacker ~= ply then
        attacker:AddFrags(1)
        PrintMessage(HUD_PRINTTALK, attacker:Nick() .. " found " .. ply:Nick())
    end
    check_winner()
end

function GM:PlayerDisconnected(ply)
    timer.Simple(0, function()
        if player.GetCount() < 2 then
            if state.phase ~= "waiting" then start_round() end -- back to waiting
        else
            check_winner()
        end
    end)
end

-- Taunts: a prop's quick chat makes a noise where it hides
function GM:PlayerQuickChat(ply, text)
    if ply:Team() == TEAM_PROPS and ply:Alive() then ply:EmitSound(TAUNTS[math.random(#TAUNTS)]) end
end

-- Every PSP: the HUD -------------------------------------------------------------------------------------

local WHITE, GREY = Color(255, 255, 255), Color(200, 200, 200)
local seen_win = 0

function GM:HUDPaint()
    local phase = GetGlobalString("ph_phase", "waiting")
    local left = math.max(0, GetGlobalFloat("ph_end") - CurTime())
    local me = LocalPlayer()
    local line
    if phase == "waiting" then
        line = "Waiting for players (2 or more)"
    elseif phase == "hiding" then
        line = "Props are hiding: " .. string.ToMinutesSeconds(left)
    elseif phase == "hunting" then
        line = "Hunt: " .. string.ToMinutesSeconds(left) .. "   Props left: " .. alive_in(TEAM_PROPS)
    else
        line = "Next round in " .. math.ceil(left)
    end
    local score = "Hunters " .. team.GetScore(TEAM_HUNTERS) .. " - " .. team.GetScore(TEAM_PROPS) .. " Props"
    draw.RoundedBox(6, 130, 4, 220, 36, Color(0, 0, 0, 150))
    draw.Text(240, 6, score, WHITE, TEXT_ALIGN_CENTER, "bold")
    draw.Text(240, 22, line, GREY, TEXT_ALIGN_CENTER)
    if not me then return end
    local t = me:Team()
    if team.GetAllTeams()[t] then
        draw.Text(12, 6, "You: " .. team.GetName(t), team.GetColor(t), TEXT_ALIGN_LEFT, "bold")
    end
    if me:IsBlind() then
        draw.Text(240, 120, "You're blindfolded while the props hide", WHITE, TEXT_ALIGN_CENTER, "large")
        draw.Text(240, 146, math.ceil(left) .. "", WHITE, TEXT_ALIGN_CENTER, "title")
    elseif t == TEAM_PROPS and me:Alive() then
        local as = me:GetDisguise()
        draw.Text(240, 196, (as and ("You're a prop. ") or "") .. "Use: become what you look at    Quick chat: taunt",
                  GREY, TEXT_ALIGN_CENTER)
    end
    -- A round the props won, while you were one of them and still hidden
    local won = GetGlobalInt("ph_won_round")
    if won ~= seen_win and GetGlobalString("ph_phase") == "over" then
        seen_win = won
        if GetGlobalInt("ph_winner") == TEAM_PROPS and t == TEAM_PROPS and me:Alive() then
            game.UnlockAchievement("hide_in_plain_sight")
        end
    end
end
