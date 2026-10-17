-- Example addon. Every .lua file in data/lua/autorun runs when a map loads (Main menu > Workshop turns
-- them on and off), and data/lua/maps/<mapname>.lua runs only on that map. docs/LUA.md has the whole API:
-- players, props, NPCs, teams, timers, hooks, HUD drawing, and how to write a gamemode.
--
-- A few of the basics:
--   hook.Add(event, name, fn)   events like "Think"(dt), "PlayerUse"(ply, ent), "PlayerSpawn"(ply), "HUDPaint"()
--   timer.Simple(seconds, fn)   CurTime()   print(...)   MAP (the map's name)   SERVER / CLIENT
--   ents.Create(name, pos [, yaw]) -> Entity    names as in the spawn menu, e.g. ents.Create("Wood Crate", Vector(0, 0, 100))
--   ent:GetPos()  ent:SetVelocity(Vector(x, y, z))  ent:SetColor(r, g, b)  ent:Freeze(true)  ent:Remove()
--   player.GetAll()  LocalPlayer()  ply:Nick()  ply:Health()  ply:GetPos()  ply:Give("smg")
--   draw.Text(x, y, text [, Color(r, g, b) [, TEXT_ALIGN_CENTER [, "bold"]]])   (inside "HUDPaint")

-- Press Use (D-pad Right) while aiming at a prop to launch it into the air. Only in the sandbox: hooks
-- run before a gamemode's own methods, and Prop Hunt's props need Use to hide.
hook.Add("PlayerUse", "launcher", function(ply, ent)
    if GAMEMODE then return end
    if IsValid(ent) then
        ent:SetVelocity(Vector(0, 0, 900))
        return true
    end
end)

-- A short welcome message a moment after the map loads (GAMEMODE is set when one's running).
timer.Simple(1.5, function()
    if GAMEMODE then
        print("Welcome to " .. GAMEMODE.Name .. " on " .. MAP .. "! Hold Select for the scores.")
    else
        print("Welcome to " .. MAP .. "! Select opens the spawn menu.")
    end
end)
