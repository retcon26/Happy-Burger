-- Example addon. Every .lua file in data/lua/autorun runs when a map loads, and
-- data/lua/maps/<mapname>.lua runs only on that map.
--
-- API (a small GMod-flavoured subset):
--   hook.Add(event, name, fn)   events: "Think"(dt), "PlayerUse"(), "HUDPaint"()
--   timer.Simple(seconds, fn)   CurTime()   print(...)   MAP (current map name)
--   ents.CreateProp(name, x, y, z [, yaw]) -> id     names as in the spawn menu, e.g. "Wood Crate"
--   ents.Remove(id)  ents.IsValid(id)  ents.GetPos(id)  ents.SetPos(id, x, y, z)
--   ents.SetVelocity(id, x, y, z)  ents.SetColor(id, r, g, b)  ents.Freeze(id, bool)
--   player.GetPos()  player.GetYaw()  player.GetAimEntity() -> id or nil
--   draw.Text(x, y, text [, 0xAABBGGRR colour])     (inside "HUDPaint")

-- Press Use (D-pad Right) while aiming at a prop to launch it into the air.
hook.Add("PlayerUse", "launcher", function()
    local id = player.GetAimEntity()
    if id then
        ents.SetVelocity(id, 0, 0, 900)
        return true
    end
end)

-- A short welcome message a moment after the map loads.
timer.Simple(1.5, function()
    print("Welcome to " .. MAP .. "! Select opens the spawn menu.")
end)
