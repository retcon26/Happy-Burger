-- Happy Burger: run the burger joint. Customers come in off the street and order at the counter. Cook
-- and build what they ordered: grill a raw patty (pink, cooked, overdone, burnt), then on a cutting board
-- stack a bottom bun, the toppings they asked for, the patty and a top bun, and the burger's made. Hot dogs
-- are a grilled sausage in a bun; fries come out of the fryer; sodas out of the machine. Put each meal on
-- the customer's tray before they lose patience. Make the day's quota or you're fired. Hit a customer with
-- a flying burger and they storm off; from day 4 mingebags come in too. Alone or together (one shared till).
--
-- The kitchen is built out of props (tools/burgerkit/props.py) on the burger joint's layout
-- (tools/burgerkit/level.py: the counter's line is x = 110 from the spawn, the door x = 640). On any other
-- map it's built on an open bit of floor near a spawn point instead. Customers walk with NPC:SetGoal.
GM.Name = "Happy Burger"
GM.AllowPickup = true -- D-pad Right on food carries it too
GM.HandsUse = true    -- R on a station (the fridge, the bun rack...) takes from it; on food, picks it up
GM.RespawnTime = 3
GM.ShowHealth = false -- nobody gets hurt: no health or suit boxes

-- Tuning ---------------------------------------------------------------------------------------------

local TICK = 0.2        -- the rules run this often
local PREP_TIME = 15    -- before the first customer of the day
local SUMMARY_TIME = 10 -- the end-of-day page
local FIRED_TIME = 12
local MAX_FOOD = 28     -- loose food at once, the oldest thrown out past that (room for 224 props, the map's included)
local WINDOWS = 3       -- serving windows on the counter
local FRY_TIME = 5

-- What customers order: B burger (with toppings), H hot dog, F fries, S soda
local MENU = {
    B = {name = "Burger", many = "Burgers", meal = "burger", price = 6},
    H = {name = "Hot Dog", one = "a Hot Dog", many = "Hot Dogs", meal = "hotdog", price = 5},
    F = {name = "Fries", one = "some Fries", many = "portions of Fries", meal = "fries", price = 3},
    S = {name = "Soda", one = "a Soda", many = "Sodas", meal = "soda", price = 2},
}
local TOPPINGS = {C = "cheese", L = "lettuce", T = "tomato"} -- in the order burgers are stacked and named
local TOPPING_PRICE = 1

-- Everything that can lie about the kitchen: its prop and name; grill times (cooked, overdone, burnt) and
-- the colour it browns to; topping letter; meal (what gets served)
local FOOD = {
    patty = {prop = "Patty", name = "Patty", cook = {8, 16, 22}, tint = {140, 100, 85}},
    sausage = {prop = "Sausage", name = "Sausage", cook = {5, 11, 16}, tint = {205, 135, 110}},
    bun_bottom = {prop = "Bottom Bun", name = "Bottom bun"},
    bun_top = {prop = "Top Bun", name = "Top bun"},
    dog_bun = {prop = "Hot Dog Bun", name = "Hot dog bun"},
    cheese = {prop = "Cheese", name = "Cheese", topping = "C"},
    lettuce = {prop = "Lettuce", name = "Lettuce", topping = "L"},
    tomato = {prop = "Tomato", name = "Tomato", topping = "T"},
    burger = {name = "Burger", meal = true}, -- prop: "Burger" plus its toppings ("Burger CL")
    hotdog = {prop = "Hot Dog", name = "Hot Dog", meal = true},
    fries = {prop = "Fries", name = "Fries", meal = true},
    soda = {prop = "Soda", name = "Soda", meal = true},
}

-- What's on the menu from which day, and how many toppings a burger can have
local function menu(day)
    local m = {"B", "S"}
    if day >= 2 then m[#m + 1] = "F" end
    if day >= 3 then m[#m + 1] = "H" end
    return m
end
local function max_toppings(day) return math.min(3, day) end

local function day_length(day) return math.min(240, 120 + 15 * day) end
local function spawn_gap(day) return math.max(10, 26 - 2.5 * day) end
local function quota(day) return 20 + 15 * (day - 1) end
local function patience(day, items) return math.max(45, 80 - 4 * day) + 14 * items end

local NAMES = {
    male = {"Larry", "Dmitri", "Kev", "Odell", "Pat", "Sal", "Vic", "Gus", "Mo", "Hank", "Raj", "Theo"},
    female = {"Barb", "Marge", "Ruth", "Tina", "Wendy", "Ivy", "Nadia", "Pat", "Lou", "Mei", "Rosa", "June"},
}
-- Who comes in (tools/burgerkit/people.py's cast): {npc type, name or nil for a random one, voice, how
-- often}. VIPs tip double and wait less.
local REGULARS = {
    {"Customer", nil, "male", 4}, {"Customer 2", nil, "male", 4}, {"Customer 3", nil, "female", 4},
    {"Customer 4", nil, "female", 4}, {"Customer 5", nil, "male", 3}, {"Customer 6", nil, "female", 4},
    {"Customer 7", nil, "male", 4}, {"Grandpa", "Grandpa Joe", "male", 1},
}
local VIPS = {{"Food Critic", "The Food Critic", "male"}, {"Big Boss", "Mr. Moneybags", "male"}}

local LINES = {
    order = {"Can I get %s?", "%s, please.", "I'll have %s.", "Hi! %s.", "%s. And make it quick."},
    happy = {"Thanks!", "Delicious.", "Best in town.", "Now that's a burger.", "Perfect."},
    angry = {"Forget it!", "I'm leaving!", "Worst service ever.", "I'll eat at the taco truck."},
    hit = {"Ow! Hey!", "What's your problem?!", "You threw that at me!"},
    raw = {"This is RAW!", "It's still pink!", "Are you trying to poison me?"},
    burnt = {"This is charcoal!", "It's burnt to a crisp!"},
    wrong = {"I didn't order that.", "That's not mine.", "Wrong order!"},
    toppings = {"That's not how I ordered it.", "Read the ticket! Wrong toppings.", "I asked for it my way."},
    unmade = {"Where's the rest of it?", "I'm not eating that on its own.", "Is this a joke?"},
    minge = {"gg ez", "can i have admin", "lol", "hurry up noob", "free burger?"},
}

-- The kitchen units' footprint and height (tools/burgerkit/props.py UNIT and UNIT_H)
local C = 48
local H = 40

-- Where everything is (the server) ----------------------------------------------------------------------

local site       -- the kitchen's origin, facing and floor (and on the burger joint, its door)
local stations = {} -- body index -> {kind = "fridge" | "buns" | "cheese" | ... | "counter", e}
local food = {}     -- body index -> {e, kind, cook (seconds on the grill), top (a burger's toppings), done}
local windows = {}  -- 1..WINDOWS: {tray (where food goes), stand (where the customer waits), yaw}
local boards = {}   -- the cutting boards' tops, where burgers and hot dogs are put together
local slots = {}    -- 1..WINDOWS: the customer at that window, or nil
local leaving = {}  -- customers on their way out
local fryer = {busy_until = 0}
local next_bun = {} -- player -> "bun_top" when their next bun from the bun rack is a top one
local grill_box     -- the grill's top centre
local trash_pos
local state = {phase = "prep", day = 1, ends = 0, next_customer = 0, money = 0, today = 0, rep = 3, served = 0, lost = 0}

local function pick(t) return t[math.random(#t)] end

local function W(x, y, z)
    return site.o + site.f * x + site.l * y + Vector(0, 0, z or 0)
end

local function wrap_yaw(off) return (site.yaw + off) % 360 end

local function floor_at(p)
    local tr = util.TraceLine({start = p + Vector(0, 0, 60), endpos = p - Vector(0, 0, 200)})
    if tr.Hit and tr.Fraction > 0 then return tr.HitPos.z end
end

local function clear_dist(from, dir, len)
    local tr = util.TraceLine({start = from, endpos = from + dir * len})
    return tr.Fraction * len
end

-- An open, flat bit of floor near a spawn point (or anywhere on the map's node graph): room in front for
-- the customers to walk up, and to the sides and back for the kitchen
local function find_site()
    if MAP == "burgerjoint" then
        -- Its own layout: the kitchen at the first spawn, the door 640 ahead, the street beyond
        local sp = game.GetSpawnPoints()[1]
        local yaw = sp.yaw
        local f = Vector(math.cos(math.rad(yaw)), math.sin(math.rad(yaw)), 0)
        return {o = Vector(sp.pos.x, sp.pos.y, sp.pos.z - 1), yaw = yaw, f = f, l = Vector(-f.y, f.x, 0), front = 600,
                door = 600, street = 790, decor = true}
    end
    local cands = {}
    for _, sp in ipairs(game.GetSpawnPoints()) do cands[#cands + 1] = {pos = sp.pos, yaw = sp.yaw} end
    for _ = 1, 10 do
        local p = navmesh.GetRandomPoint()
        if p then cands[#cands + 1] = {pos = p, yaw = math.random(0, 3) * 90} end
    end
    local best, best_score
    for _, cd in ipairs(cands) do
        local z = floor_at(cd.pos)
        if z then
            local o = Vector(cd.pos.x, cd.pos.y, z)
            for turn = 0, 270, 90 do
                local yaw = (cd.yaw + turn) % 360
                local f = Vector(math.cos(math.rad(yaw)), math.sin(math.rad(yaw)), 0)
                local l = Vector(-f.y, f.x, 0)
                local eye = o + Vector(0, 0, 40)
                local front = clear_dist(eye, f, 700)
                local side = math.min(clear_dist(eye, l, 260), clear_dist(eye, l * -1, 260))
                local back = clear_dist(eye, f * -1, 160)
                local flat = 0
                for _, xy in ipairs({{-110, -190}, {-110, 190}, {120, -160}, {120, 160}, {300, 0}, {0, 0}}) do
                    local fz = floor_at(o + f * xy[1] + l * xy[2])
                    if fz and math.abs(fz - z) < 10 then flat = flat + 1 end
                end
                local score = math.min(front, 700) * 1.5 + side * 2 + back * 2 + flat * 150
                if not best_score or score > best_score then
                    best_score = score
                    best = {o = o, yaw = yaw, f = f, l = l, front = front}
                end
            end
        end
    end
    return best
end

-- A frozen prop with its bottom at height z above the kitchen floor
local function place(name, x, y, z, yaw, r, g, b)
    local e = ents.Create(name, W(x, y, (z or 0) + 40), wrap_yaw(yaw or 0))
    if not e then return nil end
    local half = e:OBBMaxs().z
    e:SetPos(W(x, y, (z or 0) + half))
    e:Freeze(true)
    if r then e:SetColor(r, g, b) end
    return e
end

local function station(e, kind)
    if e then stations[e:EntIndex()] = {kind = kind, e = e} end
    return e
end

local decor = {} -- the dining room's loose bits (sauce bottles, plants), put back each day

local function set_decor()
    for _, e in ipairs(decor) do
        if e:IsValid() then e:Remove() end
    end
    decor = {}
    if not site.decor then return end
    local function put(name, x, y, z)
        local e = ents.Create(name, W(x, y, z), wrap_yaw(math.random(0, 359)))
        if e then decor[#decor + 1] = e end
    end
    -- On the booths' tables (level.py: at x 300 and 470, against the side walls)
    for _, x in ipairs({300, 470}) do
        for _, y in ipairs({223, -223}) do
            put("Ketchup", x - 4, y - 6, 37)
            put("Mustard", x + 4, y + 6, 37)
        end
    end
    put("Plant", 615, 225, 18)
    put("Plant", 615, -225, 18)
    put("Wet Floor Sign", 30, 110, 14)
end

-- A cutting board on a counter unit at (x, y)
local function board_at(x, y)
    station(place("Counter", x, y, 0, 0), "counter")
    local b = station(place("Cutting Board", x, y, H, 0), "board")
    if b then boards[#boards + 1] = W(x, y, H + 1.6) end
end

local function build_kitchen()
    stations, food, windows, boards, slots, leaving = {}, {}, {}, {}, {}, {}
    fryer = {busy_until = 0}
    -- The counter: seven units across, with a yellow tray at each window
    for k = -3, 3 do station(place("Counter", 110, k * C, 0, 0), "counter") end
    -- Windows left to right as you look out over the counter (+y is your left)
    for w = 1, WINDOWS do
        local y = (2 - w) * 2 * C
        station(place("Tray", 110, y, H), "counter")
        windows[w] = {tray = W(110, y, H + 4), stand = W(110 + C / 2 + 36, y), yaw = wrap_yaw(180)}
    end
    -- Its ends, so customers stay out front
    for _, y in ipairs({4 * C, -4 * C}) do station(place("Counter", 110, y, 0, 0), "counter") end
    -- The line along the back wall, right to left: patties, sausages, the grill, buns, toppings, a board,
    -- the fryer and where its fries come out
    station(place("Fridge", -80, -4 * C, 0, 0), "fridge")
    station(place("Freezer", -80, -3 * C, 0, 0), "dogs")
    station(place("Grill", -80, -1.5 * C, 0, 0), "grill")
    grill_box = W(-80, -1.5 * C, H)
    station(place("Counter", -80, 0, 0, 0), "counter")
    station(place("Bun Rack", -80, -11, H, 0), "buns")
    station(place("Dog Bun Box", -80, 12, H, 0), "dogbuns")
    station(place("Counter", -80, C, 0, 0), "counter")
    station(place("Cheese Bin", -80, C - 15, H, 0), "cheese")
    station(place("Lettuce Bin", -80, C, H, 0), "lettuce")
    station(place("Tomato Bin", -80, C + 15, H, 0), "tomato")
    board_at(-80, 2 * C)
    station(place("Fryer", -80, 3 * C, 0, 0), "fryer")
    station(place("Counter", -80, 4 * C, 0, 0), "counter")
    -- A second board by the side wall (two cooks can build at once), the soda machine and the bin
    board_at(20, 4 * C)
    station(place("Soda Machine", 20, -4 * C, 0, 90), "soda")
    station(place("Trash Can", 70, -205, 0, 0), "trash")
    trash_pos = W(70, -205, 30)
    set_decor()
end

-- Food ---------------------------------------------------------------------------------------------------

local function food_count()
    local n = 0
    for i, f in pairs(food) do
        if f.e:IsValid() then n = n + 1 else food[i] = nil end
    end
    return n
end

local on_grill

-- At the limit, the oldest food that's not on the grill goes off and is thrown out
local function make_room()
    if food_count() < MAX_FOOD then return true end
    local oldest
    for _, f in pairs(food) do
        if not on_grill(f.e:GetPos()) and (not oldest or f.born < oldest.born) then oldest = f end
    end
    if not oldest then return false end
    food[oldest.e:EntIndex()] = nil
    oldest.e:Remove()
    return true
end

local function spawn_food(kind, pos, extra)
    if not make_room() then return nil end
    local prop = FOOD[kind].prop
    if kind == "burger" then prop = "Burger" .. (extra.top ~= "" and " " .. extra.top or "") end
    local e = ents.Create(prop, pos, math.random(0, 359))
    if not e then return nil end
    local f = {e = e, kind = kind, cook = 0, born = CurTime()}
    for k, v in pairs(extra or {}) do f[k] = v end
    food[e:EntIndex()] = f
    return e, f
end

-- 0 raw, 1 cooked, 2 overdone, 3 burnt. Grilled things by their time on the grill; a burger or hot dog
-- is as done as the patty or sausage it was made with; everything else is just fine.
local function doneness(f)
    local t = FOOD[f.kind].cook
    if not t then return f.done or 1 end
    if f.cook < t[1] then return 0 elseif f.cook < t[2] then return 1 elseif f.cook < t[3] then return 2 end
    return 3
end

local function lerp(a, b, k) return math.floor(a + (b - a) * k) end

-- Raw meat is pink (its texture) and browns as it cooks, then darkens; meals show how done their meat was
local MEAL_TINT = {[0] = {255, 175, 165}, [1] = {255, 255, 255}, [2] = {200, 170, 150}, [3] = {110, 95, 90}}
local function tint(f)
    local def = FOOD[f.kind]
    local d = doneness(f)
    if def.cook then
        local c = def.tint
        if d == 0 then
            local k = f.cook / def.cook[1]
            f.e:SetColor(lerp(255, c[1], k), lerp(255, c[2], k), lerp(255, c[3], k))
        elseif d == 1 then
            f.e:SetColor(c[1], c[2], c[3])
        elseif d == 2 then
            f.e:SetColor(math.floor(c[1] * 0.68), math.floor(c[2] * 0.68), math.floor(c[3] * 0.68))
        else
            f.e:SetColor(40, 36, 36)
        end
    elseif def.meal and f.done then
        local c = MEAL_TINT[d]
        f.e:SetColor(c[1], c[2], c[3])
    end
end

function on_grill(p)
    local d = p - grill_box
    local x, y = d:Dot(site.f), d:Dot(site.l)
    return math.abs(x) <= C / 2 + 4 and math.abs(y) <= C + 4 and d.z > -6 and d.z < 30
end

local function on_board(p, b)
    local d = p - b
    return math.abs(d:Dot(site.f)) <= 19 and math.abs(d:Dot(site.l)) <= 19 and d.z > -4 and d.z < 30
end

local function toppings_text(top)
    if top == "" then return "plain" end
    local names = {}
    for k in top:gmatch(".") do names[#names + 1] = TOPPINGS[k] end
    if #names == 1 then return names[1] end
    return table.concat(names, ", ", 1, #names - 1) .. " and " .. names[#names]
end

local function food_name(f)
    local def = FOOD[f.kind]
    local n = def.name
    if f.kind == "burger" then n = n .. " (" .. toppings_text(f.top) .. ")" end
    local d = doneness(f)
    if def.cook then
        if d == 0 then return n .. ": raw (" .. math.floor(100 * f.cook / def.cook[1]) .. "% cooked)" end
        return n .. ": " .. ({"cooked!", "overdone", "BURNT"})[d]
    end
    if f.done then return n .. ({[0] = ": RAW inside", [1] = "", [2] = ": overdone", [3] = ": BURNT"})[d] end
    return n
end

-- Puts together what's on each cutting board: a bottom bun, a patty and a top bun make a burger (with any
-- cheese, lettuce and tomato that's there too); a hot dog bun and a sausage make a hot dog
local function assemble()
    for _, b in ipairs(boards) do
        local here = {}
        for _, f in pairs(food) do
            if f.e:IsValid() and not FOOD[f.kind].meal and on_board(f.e:GetPos(), b) then
                here[f.kind] = here[f.kind] or {}
                table.insert(here[f.kind], f)
            end
        end
        local function take(kind)
            local f = here[kind] and table.remove(here[kind], 1)
            if f then
                food[f.e:EntIndex()] = nil
                f.e:Remove()
            end
            return f
        end
        local made, top, meat
        if here.bun_bottom and here.patty and here.bun_top then
            top = ""
            for _, k in ipairs({"cheese", "lettuce", "tomato"}) do
                if here[k] then
                    top = top .. FOOD[k].topping
                    take(k)
                end
            end
            take("bun_bottom")
            take("bun_top")
            meat = take("patty")
            made = "burger"
        elseif here.dog_bun and here.sausage then
            take("dog_bun")
            meat = take("sausage")
            made = "hotdog"
        end
        if made then
            local _, f = spawn_food(made, b + Vector(0, 0, 8), {top = top, done = doneness(meat)})
            if f then tint(f) end
            sound.Play("assemble", b)
        end
    end
end

-- Customers ----------------------------------------------------------------------------------------------

local function say(c, text)
    PrintMessage(HUD_PRINTTALK, c.name .. ": " .. text)
end

local function voice(c, what)
    if c.n:IsValid() then sound.Play("vo_" .. c.voice .. "_" .. what, c.n:GetPos()) end
end

-- Walks them along a path of points, the last one where they stop and face yaw
local function walk(c, path, yaw, run)
    c.path, c.goal_yaw, c.run = path, yaw, run
    local last = #path == 1
    c.n:SetGoal(path[1], last and yaw or 0, run)
end

-- On to the next point if they've reached this one; true once they're at the end
local function arrived(c)
    if not c.n:AtGoal() then return false end
    if #c.path <= 1 then return true end
    if c.state == "walking" and site.door then sound.Play("door_bell", c.path[1]) end -- in through the door
    table.remove(c.path, 1)
    walk(c, c.path, c.goal_yaw, c.run)
    return false
end

-- Where customers come from: the street, then in through the door (or, on other maps, just out front)
local function way_in(stand)
    if site.door then
        return {W(site.street, math.random(-300, 300), 4), W(site.door, math.random(-16, 16)), stand}
    end
    return {W(math.max(260, site.front - 40), math.random(-250, 250), 8), stand}
end

local function way_out()
    if site.door then
        return {W(site.door, math.random(-16, 16)), W(site.street + 60, math.random(-500, 500), 4)}
    end
    return {W(math.max(260, site.front - 40), math.random(-250, 250), 8)}
end

-- What they say they want: "a Burger with cheese and lettuce, a plain Burger and 2 Sodas"
local function order_text(order)
    local counts, keys, list = {}, {}, {}
    for _, it in ipairs(order) do
        local key = it.k .. (it.top or "")
        if not counts[key] then keys[#keys + 1] = it end
        counts[key] = (counts[key] or 0) + 1
    end
    for _, it in ipairs(keys) do
        local n = counts[it.k .. (it.top or "")]
        local m = MENU[it.k]
        if it.k == "B" then
            local what = it.top == "" and "plain " or ""
            local with = it.top ~= "" and " with " .. toppings_text(it.top) or ""
            list[#list + 1] = (n > 1 and n .. " " .. what .. m.many or "a " .. what .. m.name) .. with
        else
            list[#list + 1] = n > 1 and n .. " " .. m.many or m.one
        end
    end
    if #list == 1 then return list[1] end
    return table.concat(list, ", ", 1, #list - 1) .. " and " .. list[#list]
end

local function random_toppings(day)
    local all = {"C", "L", "T"}
    local chosen = {}
    for _ = 1, math.random(0, max_toppings(day)) do chosen[table.remove(all, math.random(#all))] = true end
    local top = ""
    for _, k in ipairs({"C", "L", "T"}) do
        if chosen[k] then top = top .. k end
    end
    return top
end

local function new_order(day)
    local m = menu(day)
    local size = math.random(1, math.min(4, 1 + math.ceil(day / 2)))
    local order = {}
    -- Nearly everyone wants something off the grill
    local mains = day >= 3 and {"B", "B", "H"} or {"B"}
    if math.random() < 0.85 then order[1] = {k = pick(mains)} end
    while #order < size do order[#order + 1] = {k = pick(m)} end
    for _, it in ipairs(order) do
        if it.k == "B" then it.top = random_toppings(day) end
    end
    table.sort(order, function(a, b) return a.k < b.k end)
    return order
end

local function weighted(list)
    local total = 0
    for _, r in ipairs(list) do total = total + r[4] end
    local x = math.random() * total
    for _, r in ipairs(list) do
        x = x - r[4]
        if x <= 0 then return r end
    end
    return list[1]
end

local function spawn_customer(w)
    local day = state.day
    local who, vip, minge = weighted(REGULARS), false, false
    local r = math.random()
    if day >= 4 and r < 0.2 then
        who, minge = {"Mingebag", "Mingebag", "male"}, true
    elseif day >= 5 and r < 0.3 then
        who, vip = pick(VIPS), true
    end
    local path = way_in(windows[w].stand)
    local n = npc.Spawn(who[1], path[1], wrap_yaw(180))
    if not n then return end
    table.remove(path, 1)
    local order = new_order(day)
    local c = {
        n = n, name = who[2] or pick(NAMES[who[3]]), voice = who[3], order = order, got = {}, window = w,
        state = "walking", vip = vip, minge = minge, hp = n:Health(), spawned = CurTime(),
    }
    c.pat_max = patience(day, #order) * ((vip or minge) and 0.6 or 1)
    slots[w] = c
    walk(c, path, windows[w].yaw, false)
end

local function rep(delta)
    state.rep = math.Clamp(state.rep + delta, 0, 5)
end

local function send_away(c, how)
    slots[c.window] = nil
    c.state = "leaving"
    c.left = CurTime()
    if how == "angry" and c.minge then
        -- Let it loose: a mingebag on the rampage for a while
        c.n:ClearGoal()
        c.rampage = true
    else
        walk(c, way_out(), wrap_yaw(0), how ~= "happy")
    end
    leaving[#leaving + 1] = c
end

local function item_price(it)
    return MENU[it.k].price + (it.top and #it.top * TOPPING_PRICE or 0)
end

local function pay(c)
    local base, full = 0, true
    for i, it in ipairs(c.order) do
        local q = c.got[i]
        base = base + item_price(it) * (q == 2 and 0.5 or 1)
        if q == 2 then full = false end
    end
    local left = math.max(0, c.pat_end - CurTime()) / c.pat_max
    local tip = base * 0.5 * left * (state.rep / 3) * (c.vip and 2 or 1)
    local total = math.floor(base + tip + 0.5)
    state.money = state.money + total
    state.today = state.today + total
    state.served = state.served + 1
    rep(full and 0.25 or 0.05)
    say(c, c.minge and pick(LINES.minge) or full and pick(LINES.happy) or "A bit overdone... whatever.")
    PrintMessage(HUD_PRINTCENTER, "+$" .. total .. (tip >= 1 and "  (tip $" .. math.floor(tip + 0.5) .. ")" or ""))
    sound.Play("cash", windows[c.window].tray)
    voice(c, "happy")
    send_away(c, "happy")
end

local function storm_off(c, line, no_blame)
    state.lost = state.lost + 1
    if not no_blame then rep(-0.5) end
    say(c, line)
    voice(c, "alert")
    send_away(c, "angry")
end

-- Bounces food back off the tray towards the kitchen
local function reject(f, c, line)
    f.e:SetVelocity(site.f * -160 + Vector(0, 0, 160))
    f.bounced = CurTime()
    c.pat_end = c.pat_end - 8
    say(c, line)
    voice(c, "pain")
    sound.Play("reject", f.e:GetPos())
end

local function try_serve(c, f)
    if not FOOD[f.kind].meal then return reject(f, c, pick(LINES.unmade)) end
    local d = doneness(f)
    local right_kind = false
    for i, it in ipairs(c.order) do
        if not c.got[i] and MENU[it.k].meal == f.kind then
            right_kind = true
            if it.k ~= "B" or it.top == f.top then
                if d == 0 then return reject(f, c, pick(LINES.raw)) end
                if d == 3 then return reject(f, c, pick(LINES.burnt)) end
                c.got[i] = d == 2 and 2 or 1
                food[f.e:EntIndex()] = nil
                f.e:Remove()
                sound.Play("serve", windows[c.window].tray)
                for j = 1, #c.order do
                    if not c.got[j] then return end
                end
                return pay(c)
            end
        end
    end
    reject(f, c, pick(right_kind and LINES.toppings or LINES.wrong))
end

-- The day ------------------------------------------------------------------------------------------------

local function set_phase(phase, seconds)
    state.phase = phase
    state.ends = CurTime() + seconds
end

local function clear_food()
    for _, f in pairs(food) do
        if f.e:IsValid() then f.e:Remove() end
    end
    food = {}
end

local function clear_customers()
    for w = 1, WINDOWS do
        if slots[w] and slots[w].n:IsValid() then slots[w].n:Remove() end
        slots[w] = nil
    end
    for _, c in ipairs(leaving) do
        if c.n:IsValid() then c.n:Remove() end
    end
    leaving = {}
    for _, n in ipairs(npc.GetAll()) do n:Remove() end -- rampaging mingebags too
end

local function start_day()
    clear_food()
    state.today = 0
    state.served, state.lost = 0, 0
    set_phase("prep", PREP_TIME)
    local m = {}
    for _, k in ipairs(menu(state.day)) do m[#m + 1] = MENU[k].name end
    PrintMessage(HUD_PRINTCENTER, "Day " .. state.day .. ": make $" .. quota(state.day))
    PrintMessage(HUD_PRINTTALK, "On the menu: " .. table.concat(m, ", "))
    if state.day == 2 then PrintMessage(HUD_PRINTTALK, "New: Fries. R on the fryer, then wait for the carton.") end
    if state.day == 3 then
        PrintMessage(HUD_PRINTTALK, "New: Hot Dogs. Grill a sausage from the freezer, then put it and a bun on a board.")
    end
    if state.day == 4 then PrintMessage(HUD_PRINTTALK, "Watch out: mingebags have found the place.") end
    if state.day == 5 then PrintMessage(HUD_PRINTTALK, "VIPs from now on: double tips, no patience.") end
    if site then set_decor() end
end

local function new_game()
    game.CleanUpMap()
    site = find_site()
    if not site then
        PrintMessage(HUD_PRINTCENTER, "No room for a burger stand on this map")
        return
    end
    build_kitchen()
    state.day, state.money, state.rep = 1, 0, 3
    for _, p in ipairs(player.GetAll()) do p:Spawn() end
    start_day()
    PrintMessage(HUD_PRINTTALK, "R takes from the fridge, buns and bins, and picks food up and puts it down. "
        .. "Grill a patty until it's brown; on a cutting board stack a bottom bun, the ticket's toppings, the patty "
        .. "and a top bun; put the burger on the customer's tray. L throws.")
end

local function end_day()
    if state.today >= quota(state.day) then
        PrintMessage(HUD_PRINTCENTER, "Day " .. state.day .. " done! $" .. state.today .. " of $" .. quota(state.day))
        sound.Play("day_done")
        set_phase("summary", SUMMARY_TIME)
    else
        PrintMessage(HUD_PRINTCENTER, "You're fired! $" .. state.today .. " of $" .. quota(state.day))
        sound.Play("fired")
        set_phase("fired", FIRED_TIME)
    end
end

local function anyone_waiting()
    for w = 1, WINDOWS do
        if slots[w] then return true end
    end
    return false
end

local function tick_customers(now)
    for w = 1, WINDOWS do
        local c = slots[w]
        if c then
            if not c.n:IsValid() or not c.n:Alive() then
                slots[w] = nil -- killed (OnNPCKilled counts it)
            elseif c.n:Health() < c.hp then
                storm_off(c, pick(LINES.hit))
                rep(-0.5)
            elseif c.state == "walking" and (arrived(c) or now - c.spawned > 30) then
                c.state = "waiting"
                c.pat_end = now + c.pat_max
                local line = string.format(pick(LINES.order), order_text(c.order))
                say(c, line:sub(1, 1):upper() .. line:sub(2))
                voice(c, "order")
            elseif c.state == "waiting" and now >= c.pat_end then
                storm_off(c, c.minge and "this place sucks lol" or pick(LINES.angry))
            end
        end
    end
    for i = #leaving, 1, -1 do
        local c = leaving[i]
        local done = not c.n:IsValid() or not c.n:Alive()
        if not done and c.rampage then
            done = now - c.left > 20
        elseif not done then
            done = arrived(c) or now - c.left > 30
        end
        if done then
            if c.n:IsValid() then c.n:Remove() end
            table.remove(leaving, i)
        end
    end
end

local function tick_food()
    for i, f in pairs(food) do
        if not f.e:IsValid() then
            food[i] = nil
        else
            local p = f.e:GetPos()
            local grilling = FOOD[f.kind].cook and on_grill(p)
            if grilling then
                if not f.grilling then sound.Play("sizzle", p) end -- just put on
                local before = doneness(f)
                f.cook = f.cook + TICK
                local after = doneness(f)
                tint(f)
                if after ~= before then sound.Play(after == 1 and "ding" or "sizzle", p) end
            end
            f.grilling = grilling
            if p:Distance(trash_pos) < 40 or p.z < site.o.z - 300 or p:Distance(site.o) > 2500 then
                if p:Distance(trash_pos) < 40 then sound.Play("trash", p) end
                f.e:Remove()
                food[i] = nil
            else
                for w = 1, WINDOWS do
                    local c = slots[w]
                    local tray = windows[w].tray
                    local d = p - tray
                    local fresh = not f.bounced or CurTime() - f.bounced > 1.5
                    if c and fresh and c.state == "waiting" and math.abs(d.z) < 24 and d:Length2D() < 30 then
                        try_serve(c, f)
                        break
                    end
                end
            end
        end
    end
    assemble()
    -- Fries
    if fryer.busy_until > 0 and CurTime() >= fryer.busy_until then
        fryer.busy_until = 0
        spawn_food("fries", W(-80, 4 * C, H + 10))
        sound.Play("fries_ready", W(-80, 4 * C, H))
    end
end

local STATION_HINTS = {
    fridge = "Fridge: R for a raw patty",
    buns = "Buns: R for a bun",
    cheese = "Cheese: R for a slice",
    lettuce = "Lettuce: R for a leaf",
    tomato = "Tomato: R for a slice",
    board = "Cutting board: bottom bun, toppings, patty, top bun",
    soda = "Soda machine: R for a soda",
    trash = "Bin: drop food here",
    grill = "Grill: cook patties and sausages on top",
    counter = "",
}

-- What each player's looking at, for their HUD
local function tick_hints()
    for _, p in ipairs(player.GetAll()) do
        local tr = p:GetEyeTrace()
        local e = tr.Entity
        local hint = ""
        if e and not e:IsPlayer() and tr.HitPos:Distance(p:EyePos()) < 160 then
            local f = food[e:EntIndex()]
            local s = stations[e:EntIndex()]
            if f then
                hint = food_name(f) .. "   (R: pick up / put down, L: throw)"
            elseif s then
                if s.kind == "dogs" then
                    hint = state.day >= 3 and "Freezer: R for a raw sausage" or "Hot dogs: from day 3"
                elseif s.kind == "dogbuns" then
                    hint = state.day >= 3 and "Hot dog buns: R for one" or "Hot dogs: from day 3"
                elseif s.kind == "buns" then
                    hint = "Buns: R for a " .. ((next_bun[p] or "bun_bottom") == "bun_top" and "top" or "bottom") .. " bun"
                elseif s.kind == "fryer" then
                    hint = state.day < 2 and "Fryer: from day 2" or fryer.busy_until > 0 and "Frying..."
                        or "Fryer: R to fry fries"
                else
                    hint = STATION_HINTS[s.kind] or ""
                end
            end
        end
        p:SetNWString("cb_hint", hint)
    end
end

-- A window's ticket for the HUD: name|items|got|state|patience end|patience|vip, items as "B:CL,S,F"
local function publish()
    SetGlobalString("cb_phase", state.phase)
    SetGlobalFloat("cb_end", state.ends)
    SetGlobalInt("cb_day", state.day)
    SetGlobalInt("cb_money", state.money)
    SetGlobalInt("cb_today", state.today)
    SetGlobalInt("cb_quota", quota(state.day))
    SetGlobalFloat("cb_rep", state.rep)
    SetGlobalInt("cb_served", state.served)
    SetGlobalInt("cb_lost", state.lost)
    for w = 1, WINDOWS do
        local c = slots[w]
        local s = ""
        if c then
            local items, got = {}, {}
            for i, it in ipairs(c.order) do
                items[i] = it.k .. (it.top and ":" .. it.top or "")
                got[i] = c.got[i] or 0
            end
            s = table.concat({c.name, table.concat(items, ","), table.concat(got), c.state,
                              string.format("%.1f", c.pat_end or 0), string.format("%.1f", c.pat_max),
                              c.vip and "1" or "0"}, "|")
        end
        SetGlobalString("cb_s" .. w, s)
    end
end

local function tick()
    if not site then return end
    local now = CurTime()
    tick_customers(now)
    tick_food()
    tick_hints()
    if state.rep <= 0 and state.phase ~= "fired" then
        PrintMessage(HUD_PRINTCENTER, "Zero stars. The health inspector shut you down!")
        set_phase("fired", FIRED_TIME)
    end
    if state.phase == "prep" and now >= state.ends then
        set_phase("open", day_length(state.day))
        state.next_customer = now + 2
        PrintMessage(HUD_PRINTCENTER, "We're open!")
        sound.Play("button")
    elseif state.phase == "open" then
        if now >= state.next_customer then
            local free = {}
            for w = 1, WINDOWS do
                if not slots[w] then free[#free + 1] = w end
            end
            if #free > 0 then spawn_customer(pick(free)) end
            state.next_customer = now + spawn_gap(state.day) * math.Rand(0.7, 1.3)
        end
        if now >= state.ends then
            set_phase("closing", 180) -- long enough for the last customer's patience to run out
            PrintMessage(HUD_PRINTCENTER, "Closing time: serve who's left")
        end
    elseif state.phase == "closing" and (not anyone_waiting() or now >= state.ends) then
        for w = 1, WINDOWS do
            if slots[w] then storm_off(slots[w], "You're closing?! Ugh.", true) end
        end
        end_day()
    elseif state.phase == "summary" and now >= state.ends then
        state.day = state.day + 1
        start_day()
    elseif state.phase == "fired" and now >= state.ends then
        clear_customers()
        new_game()
    end
    publish()
end

-- Gamemode hooks -----------------------------------------------------------------------------------------

function GM:Initialize()
    if not SERVER then return end
    new_game()
    timer.Create("cb_tick", TICK, 0, tick)
end

function GM:PlayerSelectSpawn(ply)
    if site then return W(0, (ply:UserID() % 3 - 1) * 50, 4), site.yaw end
end

function GM:PlayerSpawn(ply)
    ply:StripWeapons() -- just the hands: R picks up and drops, L throws
    ply:SetWalkSpeed(220)
    ply:SetModel("Chef")
end

function GM:PlayerShouldTakeDamage(ply, attacker)
    return false -- mingebags' crowbars only annoy
end

-- What each station hands out: {food kind, from which day, sound}. The bun rack gives a bottom bun, then
-- a top bun, then a bottom bun... (next_bun, per player)
local GIVES = {
    fridge = {"patty", 1, "fridge"}, dogs = {"sausage", 3, "fridge"}, buns = {"bun_bottom", 1, "ingredient"},
    dogbuns = {"dog_bun", 3, "ingredient"}, cheese = {"cheese", 1, "ingredient"},
    lettuce = {"lettuce", 1, "ingredient"}, tomato = {"tomato", 1, "ingredient"}, soda = {"soda", 1, "dispense"},
}

function GM:PlayerUse(ply, ent)
    if not ent or not site then return end
    local s = stations[ent:EntIndex()]
    if not s then return end
    local give = GIVES[s.kind]
    if give then
        if state.day < give[2] then
            ply:PrintMessage(HUD_PRINTCENTER, "Hot dogs are on the menu from day " .. give[2])
            return true
        end
        local kind = give[1]
        if s.kind == "buns" then
            kind = next_bun[ply] or "bun_bottom"
            next_bun[ply] = kind == "bun_bottom" and "bun_top" or "bun_bottom"
        end
        -- Straight into their hands; failing that (another player's PSP carries for them), in front of
        -- them, short of the station (not inside it, where the physics would push it through the floor)
        local eye, aim = ply:EyePos(), ply:GetAimVector()
        local tr = util.TraceLine({start = eye, endpos = eye + aim * 60, filter = ply})
        local e = spawn_food(kind, eye + aim * math.Clamp(tr.Fraction * 60 - 10, 4, 20))
        if e then ply:PickUp(e) end
        sound.Play(give[3], ent:GetPos())
        return true
    elseif s.kind == "fryer" then
        if state.day < 2 then
            ply:PrintMessage(HUD_PRINTCENTER, "Fries are on the menu from day 2")
        elseif fryer.busy_until == 0 then
            fryer.busy_until = CurTime() + FRY_TIME
            sound.Play("fryer", ent:GetPos())
        end
        return true
    end
end

function GM:OnNPCKilled(n, attacker)
    local function lost(c)
        if c.n:EntIndex() ~= n:EntIndex() then return false end
        PrintMessage(HUD_PRINTCENTER, "You killed a customer! That's going in the reviews.")
        rep(-1.5)
        state.lost = state.lost + 1
        return true
    end
    for w = 1, WINDOWS do
        if slots[w] and lost(slots[w]) then slots[w] = nil end
    end
    for i = #leaving, 1, -1 do
        if lost(leaving[i]) then table.remove(leaving, i) end
    end
end

-- Every PSP: the HUD -------------------------------------------------------------------------------------

local WHITE, DIM, GOLD = Color(255, 255, 255), Color(170, 170, 170), Color(255, 210, 40)
local PAPER, INK, TICKED = Color(245, 240, 225, 235), Color(40, 30, 20), Color(30, 150, 50)
local CHIPS = {C = Color(250, 195, 40), L = Color(80, 175, 60), T = Color(215, 50, 40)}

local function split(s, sep)
    local out = {}
    for part in (s .. sep):gmatch("(.-)" .. sep) do out[#out + 1] = part end
    return out
end

local function ticket(x, slot)
    local s = GetGlobalString("cb_s" .. slot, "")
    if s == "" then return end
    local name, items, got, st, pat_end, pat_max, vip = s:match("^(.-)|(.-)|(.-)|(.-)|(.-)|(.-)|(.-)$")
    if not name then return end
    items = split(items, ",")
    local h = 30 + #items * 12
    draw.RoundedBox(4, x, 24, 150, h, PAPER)
    draw.Text(x + 6, 26, name .. (vip == "1" and " (VIP)" or ""), INK, TEXT_ALIGN_LEFT, "bold")
    if st == "walking" then
        draw.Text(x + 144, 27, "coming...", Color(110, 100, 90), TEXT_ALIGN_RIGHT)
    end
    for i, item in ipairs(items) do
        local k, top = item:sub(1, 1), item:sub(3)
        local done = got:sub(i, i) ~= "0"
        local y = 30 + i * 12
        local label = MENU[k] and MENU[k].name or k
        if k == "B" and top == "" then label = "Burger, plain" end
        draw.Text(x + 10, y, (done and "[x] " or "[ ] ") .. label, done and TICKED or INK)
        -- A burger's toppings as coloured chips: cheese, lettuce, tomato
        local cx = x + 140
        for t in top:reverse():gmatch(".") do
            draw.RoundedBox(2, cx - 10, y + 1, 11, 10, CHIPS[t] or INK)
            draw.Text(cx - 4.5, y, t, WHITE, TEXT_ALIGN_CENTER)
            cx = cx - 13
        end
    end
    if st == "waiting" then
        local left = math.Clamp((tonumber(pat_end) - CurTime()) / tonumber(pat_max), 0, 1)
        local col = left > 0.5 and Color(60, 190, 60) or left > 0.25 and Color(230, 180, 30) or Color(220, 50, 40)
        draw.Rect(x + 6, 24 + h - 8, 138, 4, Color(0, 0, 0, 80))
        draw.Rect(x + 6, 24 + h - 8, math.floor(138 * left), 4, col)
    end
end

local function stars(x, y, value)
    for i = 1, 5 do
        local fill = math.Clamp(value - (i - 1), 0, 1)
        draw.Rect(x + (i - 1) * 11, y, 9, 9, Color(0, 0, 0, 150))
        if fill > 0 then draw.Rect(x + (i - 1) * 11 + 1, y + 1, math.floor(7 * fill + 0.5), 7, GOLD) end
    end
end

local PHASES = {prep = "Opening in", open = "Open", closing = "Closing", summary = "Day over", fired = "FIRED"}

function GM:HUDPaint()
    local me = LocalPlayer()
    if not me then return end
    local phase = GetGlobalString("cb_phase", "prep")
    local left = math.max(0, GetGlobalFloat("cb_end") - CurTime())
    -- The top bar
    draw.RoundedBox(4, 4, 2, 472, 19, Color(0, 0, 0, 160))
    draw.Text(10, 5, "Day " .. GetGlobalInt("cb_day", 1), GOLD, TEXT_ALIGN_LEFT, "bold")
    draw.Text(60, 5, (PHASES[phase] or phase) .. " " .. string.ToMinutesSeconds(left), WHITE)
    local today, q = GetGlobalInt("cb_today"), GetGlobalInt("cb_quota", 20)
    draw.Text(240, 5, "Today $" .. today .. " / $" .. q, today >= q and Color(90, 220, 90) or WHITE, TEXT_ALIGN_CENTER)
    draw.Text(408, 5, "$" .. GetGlobalInt("cb_money"), GOLD, TEXT_ALIGN_RIGHT, "bold")
    stars(414, 7, GetGlobalFloat("cb_rep", 3))
    -- Order tickets, one per window, left to right as you see them
    for w = 1, WINDOWS do ticket(8 + (w - 1) * 158, w) end
    -- What you're looking at
    local hint = me:GetNWString("cb_hint", "")
    if hint ~= "" then
        local tw = draw.TextWidth(hint) + 16
        draw.RoundedBox(4, 240 - tw / 2, 226, tw, 16, Color(0, 0, 0, 160))
        draw.Text(240, 228, hint, WHITE, TEXT_ALIGN_CENTER)
    end
    -- The end of the day
    if phase == "summary" or phase == "fired" then
        draw.RoundedBox(6, 120, 90, 240, 78, Color(0, 0, 0, 200))
        draw.Text(240, 96, phase == "fired" and "YOU'RE FIRED" or "Day " .. GetGlobalInt("cb_day") .. " complete",
                  phase == "fired" and Color(230, 60, 50) or GOLD, TEXT_ALIGN_CENTER, "large")
        draw.Text(240, 122, "Earned $" .. today .. " (quota $" .. q .. ")", WHITE, TEXT_ALIGN_CENTER)
        draw.Text(240, 136, GetGlobalInt("cb_served") .. " served, " .. GetGlobalInt("cb_lost") .. " walked out",
                  DIM, TEXT_ALIGN_CENTER)
        draw.Text(240, 150, phase == "fired" and "Starting over..." or "Next day soon", DIM, TEXT_ALIGN_CENTER)
    end
end

function GM:HUDDrawScoreBoard()
    draw.RoundedBox(6, 140, 70, 200, 30 + 14 * player.GetCount(), Color(0, 0, 0, 200))
    draw.Text(240, 74, "Happy Burger staff", GOLD, TEXT_ALIGN_CENTER, "bold")
    for i, p in ipairs(player.GetAll()) do draw.Text(150, 80 + 14 * i, p:Nick(), WHITE) end
end
