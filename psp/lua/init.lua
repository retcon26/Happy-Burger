-- Retcon's Mod: the Lua library every script gets. It's built into the EBOOT (tools/embed_lua.py) and runs
-- before the gamemode, the autorun addons and the map's own script. A GMod-flavoured API on top of the
-- engine's functions (_E); docs/LUA.md has the whole list.
local E = _E

local function report(msg)
    E.error(tostring(msg))
end

-- Vector -----------------------------------------------------------------------------------------------

local Vec = {}
Vec.__index = Vec

function Vector(x, y, z)
    if type(x) == "table" then return setmetatable({x = x.x, y = x.y, z = x.z}, Vec) end
    return setmetatable({x = x or 0, y = y or 0, z = z or 0}, Vec)
end
local V = Vector

function isvector(v)
    return getmetatable(v) == Vec
end

Vec.__add = function(a, b) return V(a.x + b.x, a.y + b.y, a.z + b.z) end
Vec.__sub = function(a, b) return V(a.x - b.x, a.y - b.y, a.z - b.z) end
Vec.__mul = function(a, b)
    if type(a) == "number" then return V(b.x * a, b.y * a, b.z * a) end
    if type(b) == "number" then return V(a.x * b, a.y * b, a.z * b) end
    return V(a.x * b.x, a.y * b.y, a.z * b.z)
end
Vec.__div = function(a, b) return V(a.x / b, a.y / b, a.z / b) end
Vec.__unm = function(a) return V(-a.x, -a.y, -a.z) end
Vec.__eq = function(a, b) return a.x == b.x and a.y == b.y and a.z == b.z end
Vec.__tostring = function(a) return string.format("%.2f %.2f %.2f", a.x, a.y, a.z) end

function Vec:Length() return math.sqrt(self.x * self.x + self.y * self.y + self.z * self.z) end
function Vec:LengthSqr() return self.x * self.x + self.y * self.y + self.z * self.z end
function Vec:Length2D() return math.sqrt(self.x * self.x + self.y * self.y) end
function Vec:Distance(o) return (self - o):Length() end
function Vec:DistToSqr(o) return (self - o):LengthSqr() end
function Vec:Dot(o) return self.x * o.x + self.y * o.y + self.z * o.z end
function Vec:Cross(o) return V(self.y * o.z - self.z * o.y, self.z * o.x - self.x * o.z, self.x * o.y - self.y * o.x) end
function Vec:GetNormalized()
    local l = self:Length()
    if l < 1e-6 then return V(0, 0, 0) end
    return V(self.x / l, self.y / l, self.z / l)
end
function Vec:Normalize()
    local l = self:Length()
    if l > 1e-6 then self.x, self.y, self.z = self.x / l, self.y / l, self.z / l end
    return self
end
function Vec:Add(o) self.x, self.y, self.z = self.x + o.x, self.y + o.y, self.z + o.z return self end
function Vec:Sub(o) self.x, self.y, self.z = self.x - o.x, self.y - o.y, self.z - o.z return self end
function Vec:Mul(k) self.x, self.y, self.z = self.x * k, self.y * k, self.z * k return self end
function Vec:IsZero() return self.x == 0 and self.y == 0 and self.z == 0 end
function Vec:Unpack() return self.x, self.y, self.z end
function Vec:Yaw() return math.deg(math.atan(self.y, self.x)) end

-- Colours are ABGR numbers, the way the draw functions take them
function Color(r, g, b, a)
    a = a or 255
    return (math.floor(a) & 255) << 24 | (math.floor(b) & 255) << 16 | (math.floor(g) & 255) << 8 | (math.floor(r) & 255)
end

-- Helpers ----------------------------------------------------------------------------------------------

function math.Clamp(v, lo, hi)
    if v < lo then return lo end
    if v > hi then return hi end
    return v
end

function math.Round(v, decimals)
    local m = 10 ^ (decimals or 0)
    return math.floor(v * m + 0.5) / m
end

function math.Rand(a, b)
    return a + (b - a) * math.random()
end

function table.Count(t)
    local n = 0
    for _ in pairs(t) do n = n + 1 end
    return n
end

function table.HasValue(t, v)
    for _, x in pairs(t) do
        if x == v then return true end
    end
    return false
end

function table.Random(t)
    local keys = {}
    for k in pairs(t) do keys[#keys + 1] = k end
    if #keys == 0 then return nil end
    local k = keys[math.random(#keys)]
    return t[k], k
end

function table.Shuffle(t)
    for i = #t, 2, -1 do
        local j = math.random(i)
        t[i], t[j] = t[j], t[i]
    end
    return t
end

function table.Copy(t)
    local c = {}
    for k, v in pairs(t) do c[k] = v end
    return c
end

function string.ToMinutesSeconds(s)
    s = math.max(0, math.floor(s))
    return string.format("%02d:%02d", s // 60, s % 60)
end

Msg = print
MsgN = print

function IsValid(x)
    if type(x) ~= "table" or not x.IsValid then return false end
    return x:IsValid()
end

TEXT_ALIGN_LEFT, TEXT_ALIGN_CENTER, TEXT_ALIGN_RIGHT = 0, 1, 2
HUD_PRINTNOTIFY, HUD_PRINTCONSOLE, HUD_PRINTTALK, HUD_PRINTCENTER = 1, 2, 3, 4
TEAM_UNASSIGNED = 0

function ScrW() return 480 end
function ScrH() return 272 end

-- hook: events for scripts, and the gamemode's methods after them (GMod's order) ------------------------

hook = {list = {}}

function hook.Add(ev, name, fn)
    hook.list[ev] = hook.list[ev] or {}
    hook.list[ev][name] = fn
end

function hook.Remove(ev, name)
    if hook.list[ev] then hook.list[ev][name] = nil end
end

function hook.GetTable()
    return hook.list
end

-- Runs ev's hooks until one returns something, then the gamemode's method if none did
function hook.Call(ev, gm, ...)
    local l = hook.list[ev]
    if l then
        for name, fn in pairs(l) do
            local ok, a, b, c = pcall(fn, ...)
            if not ok then
                report("hook " .. ev .. "/" .. tostring(name) .. ": " .. tostring(a))
                l[name] = nil
            elseif a ~= nil then
                return a, b, c
            end
        end
    end
    local method = gm and gm[ev]
    if method then
        local ok, a, b, c = pcall(method, gm, ...)
        if not ok then
            report("GM:" .. ev .. ": " .. tostring(a))
            return nil
        end
        return a, b, c
    end
end

function hook.Run(ev, ...)
    return hook.Call(ev, GAMEMODE, ...)
end

-- timer ------------------------------------------------------------------------------------------------

timer = {pending = {}, named = {}}

function timer.Simple(delay, fn)
    timer.pending[#timer.pending + 1] = {t = CurTime() + delay, fn = fn}
end

-- reps 0 repeats for ever
function timer.Create(name, delay, reps, fn)
    timer.named[name] = {t = CurTime() + delay, delay = delay, reps = reps or 1, fn = fn}
end

function timer.Remove(name)
    timer.named[name] = nil
end

function timer.Exists(name)
    return timer.named[name] ~= nil
end

function timer.TimeLeft(name)
    local t = timer.named[name]
    return t and math.max(0, t.t - CurTime()) or nil
end

local function run_timers()
    local now = CurTime()
    if #timer.pending > 0 then
        local list = timer.pending
        timer.pending = {} -- ones made by these callbacks go in the new list
        for _, t in ipairs(list) do
            if t.t <= now then
                local ok, e = pcall(t.fn)
                if not ok then report("timer: " .. tostring(e)) end
            else
                timer.pending[#timer.pending + 1] = t
            end
        end
    end
    local due = {}
    for name, t in pairs(timer.named) do
        if t.t <= now then due[#due + 1] = name end
    end
    for _, name in ipairs(due) do
        local t = timer.named[name]
        if t then
            local ok, e = pcall(t.fn)
            if not ok then report("timer " .. tostring(name) .. ": " .. tostring(e)) end
            if timer.named[name] == t then -- unless the callback removed or replaced it
                if t.reps == 1 then
                    timer.named[name] = nil
                else
                    if t.reps > 1 then t.reps = t.reps - 1 end
                    t.t = math.max(t.t + t.delay, now)
                end
            end
        end
    end
end

-- Networked variables: set where the rules run, read on every PSP --------------------------------------

local nw = {g = {}, p = {}}

local function nw_number(v) return math.floor(tonumber(v) or 0) end

function SetGlobalInt(k, v) nw.g[k] = nw_number(v) end
function SetGlobalFloat(k, v) nw.g[k] = tonumber(v) or 0 end
function SetGlobalString(k, v) nw.g[k] = tostring(v) end
function SetGlobalBool(k, v) nw.g[k] = v and true or false end

function GetGlobalInt(k, d)
    local v = nw.g[k]
    return type(v) == "number" and math.floor(v) or d or 0
end

function GetGlobalFloat(k, d)
    local v = nw.g[k]
    return type(v) == "number" and v or d or 0
end

function GetGlobalString(k, d)
    local v = nw.g[k]
    return v ~= nil and tostring(v) or d or ""
end

function GetGlobalBool(k, d)
    local v = nw.g[k]
    if v == nil then return d or false end
    return v == true
end

local function clean(s)
    return (tostring(s):gsub("[\t\n]", " "))
end

local function nw_put(out, who, k, v)
    local t = type(v)
    local enc
    if t == "number" then
        enc = "n" .. string.format("%.10g", v)
    elseif t == "boolean" then
        enc = v and "b1" or "b0"
    else
        enc = "s" .. clean(v)
    end
    out[#out + 1] = who .. "\t" .. clean(k) .. "\t" .. enc
end

function _nw_pack()
    local out = {}
    for k, v in pairs(nw.g) do nw_put(out, "G", k, v) end
    for s, t in pairs(nw.p) do
        for k, v in pairs(t) do nw_put(out, "P" .. s, k, v) end
    end
    return table.concat(out, "\n")
end

function _nw_unpack(str)
    local g, p = {}, {}
    for line in string.gmatch(str, "[^\n]+") do
        local who, k, v = string.match(line, "^([^\t]+)\t([^\t]*)\t(.*)$")
        if who then
            local kind, val = v:sub(1, 1), v:sub(2)
            local value
            if kind == "n" then
                value = tonumber(val)
            elseif kind == "b" then
                value = val == "1"
            elseif kind == "s" then
                value = val
            end
            if value ~= nil then
                if who == "G" then
                    g[k] = value
                else
                    local s = tonumber(who:sub(2))
                    if s then
                        p[s] = p[s] or {}
                        p[s][k] = value
                    end
                end
            end
        end
    end
    nw.g, nw.p = g, p
end

-- Players ----------------------------------------------------------------------------------------------

local Player = {}
Player.__index = Player
PLAYER = Player -- add your own methods: function PLAYER:Shout() ... end
local players = {}

local function pl(slot)
    if slot == nil or not E.pl_valid(slot) then return nil end
    local serial = E.pl_serial(slot)
    local p = players[slot]
    if not p or p._slot ~= slot or p._serial ~= serial then
        p = setmetatable({_slot = slot, _serial = serial}, Player)
        players[slot] = p
    end
    return p
end
_player_obj = pl

function Player:IsValid() return E.pl_valid(self._slot) and E.pl_serial(self._slot) == self._serial end
function Player:IsPlayer() return true end
function Player:IsNPC() return false end
function Player:UserID() return self._slot end
Player.EntIndex = Player.UserID
function Player:Nick() return E.pl_name(self._slot) end
Player.Name = Player.Nick
Player.GetName = Player.Nick
function Player:IsLocalPlayer() return E.pl_local() == self._slot end
Player.__tostring = function(self) return "Player [" .. self._slot .. "][" .. self:Nick() .. "]" end

function Player:Alive() return E.pl_health(self._slot) > 0 end
function Player:Health() return math.ceil(E.pl_health(self._slot)) end
function Player:SetHealth(hp) E.pl_set_health(self._slot, hp) end
function Player:GetMaxHealth() return E.pl_max_health(self._slot) end
function Player:SetMaxHealth(hp) E.pl_set_max_health(self._slot, hp) end
function Player:Kill() E.pl_kill(self._slot, false) end
function Player:KillSilent() E.pl_kill(self._slot, true) end
function Player:Spawn() E.pl_spawn(self._slot) end

function Player:GetPos() return V(E.pl_pos(self._slot)) end
function Player:SetPos(v, yaw) E.pl_set_pos(self._slot, v.x, v.y, v.z, yaw or self:GetYaw()) end
function Player:EyePos()
    local x, y, z = E.pl_view(self._slot)
    return V(x, y, z)
end
function Player:GetAimVector()
    local _, _, _, dx, dy, dz = E.pl_view(self._slot)
    return V(dx, dy, dz)
end
function Player:GetYaw()
    local _, _, _, _, _, _, yaw = E.pl_view(self._slot)
    return yaw
end
function Player:GetEyeTrace()
    local eye = self:EyePos()
    return util.TraceLine({start = eye, endpos = eye + self:GetAimVector() * 4096, filter = self})
end

function Player:Team() return E.pl_team(self._slot) end
function Player:SetTeam(t) E.pl_set_team(self._slot, t) end
function Player:Freeze(on) E.pl_set_flag(self._slot, "frozen", on ~= false) end
function Player:IsFrozen() return E.pl_flag(self._slot, "frozen") end
function Player:SetBlind(on) E.pl_set_flag(self._slot, "blind", on ~= false) end
function Player:IsBlind() return E.pl_flag(self._slot, "blind") end
function Player:SetNoTarget(on) E.pl_set_flag(self._slot, "notarget", on ~= false) end
-- Units a second, 250 being the usual
function Player:SetWalkSpeed(u) E.pl_set_speed(self._slot, u / 250) end
function Player:GetWalkSpeed() return E.pl_speed(self._slot) * 250 end
Player.SetRunSpeed = Player.SetWalkSpeed
Player.GetRunSpeed = Player.GetWalkSpeed
-- A character by its NPC name ("Zombie", "Combine"); nil or "" for the one they picked
function Player:SetModel(name) return E.pl_set_model(self._slot, name) end
function Player:GetModel() return E.pl_model(self._slot) end

-- Prop Hunt: look like a prop (an Entity), with its size
function Player:Disguise(ent)
    if not IsValid(ent) or not ent._ent then return false end
    return E.pl_disguise(self._slot, ent._i)
end
function Player:Undisguise() E.pl_undisguise(self._slot) end
function Player:GetDisguise() return E.pl_disguise_name(self._slot) end
function Player:IsDisguised() return E.pl_disguise_name(self._slot) ~= nil end

-- Weapons by name: hands, physgun, gravgun, toolgun, crowbar, pistol, 357, smg, shotgun, ar2, crossbow,
-- rpg, grenade, bugbait (GMod's weapon_smg1 and the rest work too)
function Player:Give(name)
    local bit = E.weapon_bit(name)
    if not bit then return false end
    E.pl_set_weapons(self._slot, E.pl_weapons(self._slot) | bit)
    return true
end
function Player:StripWeapon(name)
    local bit = E.weapon_bit(name)
    if bit then E.pl_set_weapons(self._slot, E.pl_weapons(self._slot) & ~bit) end
end
function Player:StripWeapons() E.pl_set_weapons(self._slot, 0) end
function Player:HasWeapon(name)
    local bit = E.weapon_bit(name)
    return bit ~= nil and E.pl_weapons(self._slot) & bit ~= 0
end
function Player:GetWeapons()
    local t, mask = {}, E.pl_weapons(self._slot)
    for i, id in ipairs(E.weapon_ids()) do
        if mask & (1 << (i - 1)) ~= 0 then t[#t + 1] = id end
    end
    return t
end
function Player:SelectWeapon(name) E.pl_select(self._slot, name) end
function Player:GetActiveWeapon() return E.pl_weapon(self._slot) end
function Player:GiveAmmo(amount, name) E.pl_give_ammo(self._slot, name, amount) end
function Player:RefillAmmo() E.pl_give_ammo(self._slot, nil, 0) end

function Player:ChatPrint(text) E.message(self._slot, HUD_PRINTTALK, tostring(text)) end
function Player:PrintMessage(kind, text) E.message(self._slot, kind, tostring(text)) end
function Player:EmitSound(name)
    local p = self:GetPos()
    E.sound(-1, name, p.x, p.y, p.z + 40)
end

local function pnw(self)
    nw.p[self._slot] = nw.p[self._slot] or {}
    return nw.p[self._slot]
end
function Player:SetNWInt(k, v) pnw(self)[k] = nw_number(v) end
function Player:SetNWFloat(k, v) pnw(self)[k] = tonumber(v) or 0 end
function Player:SetNWString(k, v) pnw(self)[k] = tostring(v) end
function Player:SetNWBool(k, v) pnw(self)[k] = v and true or false end
function Player:GetNWInt(k, d)
    local v = (nw.p[self._slot] or {})[k]
    return type(v) == "number" and math.floor(v) or d or 0
end
function Player:GetNWFloat(k, d)
    local v = (nw.p[self._slot] or {})[k]
    return type(v) == "number" and v or d or 0
end
function Player:GetNWString(k, d)
    local v = (nw.p[self._slot] or {})[k]
    return v ~= nil and tostring(v) or d or ""
end
function Player:GetNWBool(k, d)
    local v = (nw.p[self._slot] or {})[k]
    if v == nil then return d or false end
    return v == true
end

function Player:Frags() return self:GetNWInt("_frags") end
function Player:SetFrags(n) self:SetNWInt("_frags", n) end
function Player:AddFrags(n) self:SetNWInt("_frags", self:Frags() + (n or 1)) end
function Player:Deaths() return self:GetNWInt("_deaths") end
function Player:SetDeaths(n) self:SetNWInt("_deaths", n) end
function Player:AddDeaths(n) self:SetNWInt("_deaths", self:Deaths() + (n or 1)) end

player = {}

function player.GetAll()
    local t = {}
    for s = 0, MAX_PLAYERS - 1 do
        local p = pl(s)
        if p then t[#t + 1] = p end
    end
    return t
end

function player.GetCount()
    return #player.GetAll()
end

function player.GetBySlot(s)
    return pl(s)
end

function LocalPlayer()
    return pl(E.pl_local())
end

-- The first API's functions, about this PSP's player
function player.GetPos()
    local p = LocalPlayer()
    if not p then return 0, 0, 0 end
    return E.pl_pos(p._slot)
end

function player.GetYaw()
    local p = LocalPlayer()
    return p and p:GetYaw() or 0
end

function player.GetAimEntity()
    return E.aim_body()
end

-- Props ------------------------------------------------------------------------------------------------

local Entity = {}
Entity.__index = Entity
ENTITY = Entity
local ent_cache = {}

local function ent(i)
    if i == nil or not E.ent_valid(i) then return nil end
    local serial = E.ent_serial(i)
    local e = ent_cache[i]
    if not e or e._serial ~= serial then
        e = setmetatable({_i = i, _serial = serial, _ent = true}, Entity)
        ent_cache[i] = e
    end
    return e
end
_ent_obj = ent

function Entity:IsValid() return E.ent_valid(self._i) and E.ent_serial(self._i) == self._serial end
function Entity:EntIndex() return self._i end
function Entity:IsPlayer() return false end
function Entity:IsNPC() return false end
function Entity:GetClass() return "prop_physics" end
function Entity:IsMapProp() return E.ent_lib(self._i) == 1 end -- the map's own, not one spawned
function Entity:GetModel() return E.ent_model(self._i) end
function Entity:GetPos() return V(E.ent_pos(self._i)) end
function Entity:SetPos(v) E.ent_set_pos(self._i, v.x, v.y, v.z) end
function Entity:GetVelocity() return V(E.ent_vel(self._i)) end
function Entity:SetVelocity(v) E.ent_set_vel(self._i, v.x, v.y, v.z) end
function Entity:SetColor(c, g, b)
    if g then
        E.ent_color(self._i, c, g, b)
    else
        E.ent_color(self._i, c & 255, (c >> 8) & 255, (c >> 16) & 255)
    end
end
function Entity:Freeze(on) E.ent_freeze(self._i, on ~= false) end
function Entity:IsFrozen() return E.ent_frozen(self._i) end
function Entity:GetMass() return E.ent_mass(self._i) end
function Entity:OBBMins()
    local x, y, z = E.ent_bounds(self._i)
    return V(x, y, z)
end
function Entity:OBBMaxs()
    local _, _, _, x, y, z = E.ent_bounds(self._i)
    return V(x, y, z)
end
function Entity:GetSize() return self:OBBMaxs() - self:OBBMins() end
function Entity:Remove() E.ent_remove(self._i) end
function Entity:EmitSound(name)
    local p = self:GetPos()
    E.sound(-1, name, p.x, p.y, p.z)
end
Entity.__tostring = function(self) return "Entity [" .. self._i .. "][" .. self:GetModel() .. "]" end

ents = {}

-- By its spawn menu name ("Wooden Crate"); nil if there's no such prop (or this PSP doesn't run the rules)
function ents.Create(name, pos, yaw)
    return ent(E.ent_create(name, pos.x, pos.y, pos.z, yaw or 0))
end

function ents.GetAll()
    local t = {}
    for i = 0, E.ent_max() - 1 do
        local e = ent(i)
        if e then t[#t + 1] = e end
    end
    return t
end

function ents.GetByIndex(i)
    return ent(i)
end
_G.Entity = ents.GetByIndex -- GMod's Entity(i)

function ents.FindInSphere(pos, r)
    local t, r2 = {}, r * r
    for i = 0, E.ent_max() - 1 do
        if E.ent_valid(i) then
            local x, y, z = E.ent_pos(i)
            local dx, dy, dz = x - pos.x, y - pos.y, z - pos.z
            if dx * dx + dy * dy + dz * dz <= r2 then t[#t + 1] = ent(i) end
        end
    end
    return t
end

function ents.FindByModel(name)
    local t = {}
    for _, e in ipairs(ents.GetAll()) do
        if e:GetModel() == name then t[#t + 1] = e end
    end
    return t
end

-- The first API's functions take a prop's number (or an Entity)
local function idx(x)
    if type(x) == "table" then return x._i end
    return x
end
function ents.CreateProp(name, x, y, z, yaw) return E.ent_create(name, x, y, z, yaw or 0) end
function ents.Remove(id) E.ent_remove(idx(id)) end
function ents.IsValid(id) return E.ent_valid(idx(id)) end
function ents.GetPos(id) return E.ent_pos(idx(id)) end
function ents.SetPos(id, x, y, z) E.ent_set_pos(idx(id), x, y, z) end
function ents.SetVelocity(id, x, y, z) E.ent_set_vel(idx(id), x, y, z) end
function ents.SetColor(id, r, g, b) E.ent_color(idx(id), r, g, b) end
function ents.Freeze(id, on) E.ent_freeze(idx(id), on) end

-- NPCs -------------------------------------------------------------------------------------------------

local NPC = {}
NPC.__index = NPC
local npc_cache = {}

local function npcobj(i)
    if i == nil then return nil end
    local serial = E.npc_serial(i)
    if serial == 0 then return nil end
    local n = npc_cache[i]
    if not n or n._serial ~= serial then
        n = setmetatable({_i = i, _serial = serial, _npc = true}, NPC)
        npc_cache[i] = n
    end
    return n
end
_npc_obj = npcobj

function NPC:IsValid() return E.npc_serial(self._i) == self._serial end
function NPC:IsNPC() return true end
function NPC:IsPlayer() return false end
function NPC:EntIndex() return 1000 + self._i end
function NPC:Alive() return self:IsValid() and E.npc_info(self._i) == 2 end
function NPC:GetClass()
    local _, t = E.npc_info(self._i)
    return t or ""
end
function NPC:GetPos()
    local st, _, x, y, z = E.npc_info(self._i)
    if st == 0 then return V(0, 0, 0) end
    return V(x, y, z)
end
function NPC:Health()
    local st, _, _, _, _, _, hp = E.npc_info(self._i)
    return st == 0 and 0 or math.ceil(hp)
end
function NPC:SetHealth(hp) E.npc_set_health(self._i, hp) end
function NPC:SetPos(v) E.npc_set_pos(self._i, v.x, v.y, v.z) end
function NPC:TakeDamage(amount, attacker) E.npc_damage(self._i, amount, attacker and attacker._slot or -1) end
function NPC:Remove() E.npc_remove(self._i) end
NPC.__tostring = function(self) return "NPC [" .. self._i .. "][" .. self:GetClass() .. "]" end

npc = {}

-- By its spawn menu name ("Zombie", "Fast Zombie", "Combine"); nil if there's no room (16 at a time)
function npc.Spawn(type, pos, yaw)
    return npcobj(E.npc_spawn(type, pos.x, pos.y, pos.z, yaw or 0))
end

function npc.GetAll()
    local t = {}
    for i = 0, E.npc_max() - 1 do
        local n = npcobj(i)
        if n and n:Alive() then t[#t + 1] = n end
    end
    return t
end

function npc.Count()
    return #npc.GetAll()
end

function npc.GetTypes()
    return E.npc_types()
end

-- Teams ------------------------------------------------------------------------------------------------

team = {_t = {}}

function team.SetUp(id, name, color)
    team._t[id] = {name = name, color = color or Color(255, 255, 255)}
end

function team.GetName(id)
    local t = team._t[id]
    return t and t.name or ""
end

function team.GetColor(id)
    local t = team._t[id]
    return t and t.color or Color(255, 255, 255)
end

function team.GetAllTeams()
    return team._t
end

function team.GetPlayers(id)
    local t = {}
    for _, p in ipairs(player.GetAll()) do
        if p:Team() == id then t[#t + 1] = p end
    end
    return t
end

function team.NumPlayers(id)
    return #team.GetPlayers(id)
end

function team.GetScore(id) return GetGlobalInt("_score" .. id) end
function team.SetScore(id, n) SetGlobalInt("_score" .. id, n) end
function team.AddScore(id, n) team.SetScore(id, team.GetScore(id) + (n or 1)) end

-- The team with fewest players (of ids, or of all that are set up)
function team.BestAutoJoinTeam(ids)
    if not ids then
        ids = {}
        for id in pairs(team._t) do ids[#ids + 1] = id end
        table.sort(ids)
    end
    local best, fewest = ids[1], math.huge
    for _, id in ipairs(ids) do
        local n = team.NumPlayers(id)
        if n < fewest then best, fewest = id, n end
    end
    return best
end

-- Messages, sounds, traces, the map -------------------------------------------------------------------

-- To everyone: HUD_PRINTTALK in the chat, HUD_PRINTCENTER across the middle of the screen
function PrintMessage(kind, text)
    E.message(-1, kind, tostring(text))
end

sound = {}
-- Heard by everyone: at pos, or everywhere
function sound.Play(name, pos)
    if pos then
        E.sound(-1, name, pos.x, pos.y, pos.z)
    else
        E.sound(-1, name)
    end
end

surface = {}
-- Just this PSP
function surface.PlaySound(name)
    local s = E.pl_local()
    if s then E.sound(s, name) end
end

util = {}
-- {start = Vector, endpos = Vector, filter = a Player to go through}
function util.TraceLine(t)
    local skip = t.filter and t.filter._slot or -1
    local f, x, y, z, body, slot = E.trace(t.start.x, t.start.y, t.start.z, t.endpos.x, t.endpos.y, t.endpos.z, skip)
    local hit = body and ent(body) or slot and pl(slot) or nil
    return {Fraction = f, Hit = f < 1, HitPos = V(x, y, z), Entity = hit, HitWorld = f < 1 and hit == nil}
end

game = {}
function game.GetMap() return MAP end
function game.IsDedicated() return E.dedicated() end
-- Everything back as the map loaded: spawned props, NPCs and ragdolls gone, its own props where they were
function game.CleanUpMap() E.cleanup() end
function game.GetSpawnPoints()
    local t = {}
    for i = 0, E.spawns() - 1 do
        local x, y, z, yaw = E.spawn(i)
        t[#t + 1] = {pos = V(x, y, z), yaw = yaw}
    end
    return t
end
-- A built-in gamemode's achievement for this PSP's player ("hide_in_plain_sight", "survivor", "top_frag")
function game.UnlockAchievement(id) E.achieve(id) end

navmesh = {}
function navmesh.GetNodeCount() return E.nav_count() end
function navmesh.GetNode(i) return V(E.nav_node(i)) end
-- A random spot NPCs can walk to (the map's node graph), or nil on maps without one
function navmesh.GetRandomPoint()
    local n = E.nav_count()
    if n == 0 then return nil end
    return V(E.nav_node(math.random(n) - 1))
end

input = {}
-- This PSP's buttons: "select", "start", "up", "down", "left", "right", "l", "r", "cross", "circle", "square",
-- "triangle". The game uses nearly all of them; in a gamemode's game Select is free (the scoreboard).
function input.IsButtonDown(name) return E.button(name) end

engine = {}
function engine.ActiveGamemode() return GAMEMODE_NAME end

-- Gamemodes --------------------------------------------------------------------------------------------
-- A gamemode file fills in GM: its settings and methods (GM:PlayerSpawn...), called after any hooks.

function _gm_begin(name)
    GM = {Name = name, FolderName = name, Sandbox = false, AllowPickup = false, TeamBased = false, RespawnTime = -1}
end

function _gm_end()
    GAMEMODE = GM
    local flags = (GM.Sandbox and 1 or 0) | (GM.AllowPickup and 2 or 0) | (GM.TeamBased and 4 or 0)
    return flags, tonumber(GM.RespawnTime) or -1
end

-- What the engine calls ---------------------------------------------------------------------------------

function _think(dt)
    run_timers()
    hook.Run("Think", dt)
end

function _player_join(s)
    local p = pl(s)
    if p then hook.Run("PlayerInitialSpawn", p) end
end

function _player_spawn(s)
    local p = pl(s)
    if not p then return end
    hook.Run("PlayerSpawn", p)
    hook.Run("PlayerLoadout", p)
end

function _player_leave(s)
    local p = players[s]
    if p then hook.Run("PlayerDisconnected", p) end
    players[s] = nil
    nw.p[s] = nil
end

function _player_death(s, attacker)
    local victim = pl(s)
    if not victim then return end
    local a = attacker and pl(attacker) or nil
    hook.Run("PlayerDeath", victim, a, a)
end

-- EntityTakeDamage(ent, dmginfo): a player shot a prop
local DmgInfo = {}
DmgInfo.__index = DmgInfo
function DmgInfo:GetAttacker() return self.attacker end
function DmgInfo:GetDamage() return self.damage end

function _entity_damage(body, attacker, amount)
    local e = ent(body)
    if e then hook.Run("EntityTakeDamage", e, setmetatable({attacker = pl(attacker), damage = amount}, DmgInfo)) end
end

function _npc_killed(i, attacker)
    local n = npcobj(i)
    local a = attacker and attacker >= 0 and pl(attacker) or nil
    if n then hook.Run("OnNPCKilled", n, a, a) end
end

-- PlayerSelectSpawn(ply) returns a Vector (and a yaw), or nothing for one of the map's spawn points
function _select_spawn(s)
    local p = pl(s)
    if not p then return end
    local pos, yaw = hook.Run("PlayerSelectSpawn", p)
    if isvector(pos) then return pos.x, pos.y, pos.z, yaw or 0 end
end

-- The scoreboard: hold Select in a gamemode's game (GM:HUDDrawScoreBoard replaces it)
local function draw_scoreboard()
    local list = player.GetAll()
    table.sort(list, function(a, b)
        if a:Team() ~= b:Team() then return a:Team() < b:Team() end
        if a:Frags() ~= b:Frags() then return a:Frags() > b:Frags() end
        return a:UserID() < b:UserID()
    end)
    local w, row = 340, 18
    local h = 52 + #list * row
    local x, y = 240 - w / 2, math.max(10, 136 - h / 2)
    draw.RoundedBox(6, x, y, w, h, Color(20, 20, 20, 220))
    draw.Text(x + 12, y + 8, GAMEMODE.Name or GAMEMODE_NAME, Color(255, 220, 0), TEXT_ALIGN_LEFT, "bold")
    draw.Text(x + w - 12, y + 8, MAP, Color(180, 180, 180), TEXT_ALIGN_RIGHT)
    draw.Text(x + w - 70, y + 28, "Kills", Color(180, 180, 180), TEXT_ALIGN_RIGHT)
    draw.Text(x + w - 12, y + 28, "Deaths", Color(180, 180, 180), TEXT_ALIGN_RIGHT)
    for k, p in ipairs(list) do
        local ry = y + 44 + (k - 1) * row
        local c = team._t[p:Team()] and team.GetColor(p:Team()) or Color(255, 255, 255)
        if not p:Alive() then c = Color(140, 140, 140) end
        local name = p:Nick()
        if team._t[p:Team()] then name = name .. "  (" .. team.GetName(p:Team()) .. ")" end
        if p:IsLocalPlayer() then draw.Rect(x + 6, ry - 1, w - 12, row - 2, Color(255, 255, 255, 40)) end
        draw.Text(x + 12, ry, name, c)
        draw.Text(x + w - 70, ry, tostring(p:Frags()), c, TEXT_ALIGN_RIGHT)
        draw.Text(x + w - 12, ry, tostring(p:Deaths()), c, TEXT_ALIGN_RIGHT)
    end
end

function _hud()
    hook.Run("HUDPaint")
    if GAMEMODE and not GAMEMODE.Sandbox and input.IsButtonDown("select") then
        if GAMEMODE.HUDDrawScoreBoard then
            local ok, e = pcall(GAMEMODE.HUDDrawScoreBoard, GAMEMODE)
            if not ok then report("GM:HUDDrawScoreBoard: " .. tostring(e)) end
        else
            draw_scoreboard()
        end
    end
end
