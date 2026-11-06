// The VScript half of the plugin: small functions the trainer calls through the game's "script" command to move
// players, show ghosts and text, and draw the route. Written to <game>/scripts/vscripts/p2practice/helper.nut and
// loaded once per map. It is a new file under its own folder; it replaces none of the game's scripts.
#pragma once

namespace helper {

constexpr const char *kRelativePath = "scripts/vscripts/p2practice/helper.nut";
constexpr const char *kIncludeName = "p2practice/helper";

constexpr const char *kSource = R"nut(// P2 Practice helper. Written by the plugin each time it loads; edits here are overwritten.
::PP <- {
	players = {}
	ghosts = {}
	texts = {}
}

// The player with this entity index, or null
::PP.Player <- function(index) {
	if (index in players && players[index] != null && players[index].IsValid()) return players[index]
	players.clear()
	local e = null
	while (e = Entities.FindByClassname(e, "player")) players[e.entindex()] <- e
	return (index in players) ? players[index] : null
}

// Put a player somewhere: position, aim, velocity
::PP.Mv <- function(index, x, y, z, pitch, yaw, vx, vy, vz) {
	local p = Player(index)
	if (p == null) return
	p.SetOrigin(Vector(x, y, z))
	p.SetAngles(pitch, yaw, 0)
	p.SetVelocity(Vector(vx, vy, vz))
}

// Use something as a player: what the run named if it has a name (the nearest one of several), else the nearest
// thing of that kind the player is aiming at within reach. Buttons get pressed, cubes picked up.
::PP.UseEnt <- function(index, classname, name, dx, dy, dz) {
	local p = Player(index)
	if (p == null) return
	local eye = p.EyePosition()
	local best = null
	local e = null
	if (name != "") {
		local bestDistance = 0.0
		while (e = Entities.FindByName(e, name)) {
			if (e.GetClassname() != classname) continue
			local distance = (e.GetCenter() - eye).Length()
			if (best == null || distance < bestDistance) {
				best = e
				bestDistance = distance
			}
		}
	} else {
		local aim = Vector(dx, dy, dz)
		local bestDot = 0.5
		while (e = Entities.FindByClassnameWithin(e, classname, eye, 140)) {
			local to = e.GetCenter() - eye
			local distance = to.Length()
			if (distance < 1) continue
			local dot = to.Dot(aim) / distance
			if (dot > bestDot) {
				bestDot = dot
				best = e
			}
		}
	}
	if (best != null) EntFireByHandle(best, "Use", "", 0, p, p)
}

// The player lets go of what they carry. The game holds a carried object through a "player_pickup" entity per
// carrying player; removing it releases the object where it is, with the speed it has.
::PP.Drop <- function(index) {
	local p = Player(index)
	if (p == null) return
	local mine = null
	local only = null
	local count = 0
	local c = null
	while (c = Entities.FindByClassname(c, "player_pickup")) {
		count++
		only = c
		if (c.GetOwner() == p || c.GetMoveParent() == p) mine = c
	}
	if (mine == null && count == 1) mine = only
	if (mine != null) EntFireByHandle(mine, "Kill", "", 0, null, null)
}

// Which live object stands for each of the run's objects. Entity numbers differ from load to load for anything
// a dropper spawns, and objects have no names of their own, so a recorded object takes the nearest live object
// of its kind that no other recorded object has taken, at the moment it is first needed (normally when it first
// appears in the run, when both are at the same spot), and keeps it from then on.
::PP.objects <- {}
::PP.Claim <- function(id, classname, x, y, z) {
	if (id in objects && objects[id] != null && objects[id].IsValid()) return objects[id]
	local spot = Vector(x, y, z)
	local best = null
	local bestDistance = 400.0
	local e = null
	while (e = Entities.FindByClassname(e, classname)) {
		local taken = false
		foreach (other in objects) if (other == e) taken = true
		if (taken) continue
		local distance = (e.GetOrigin() - spot).Length()
		if (distance < bestDistance) {
			best = e
			bestDistance = distance
		}
	}
	if (best != null) objects[id] <- best
	return best
}

::PP.ForgetObjects <- function() {
	objects.clear()
}

// An object that has come to rest in the run: its live counterpart is put there if it lies somewhere else
::PP.Rest <- function(id, classname, x, y, z, pitch, yaw, roll) {
	local e = Claim(id, classname, x, y, z)
	local spot = Vector(x, y, z)
	if (e == null || (e.GetOrigin() - spot).Length() < 12) return
	e.SetOrigin(spot)
	e.SetAngles(pitch, yaw, roll)
	e.SetVelocity(Vector(0, 0, 0))
}

// The player picks up (uses) the live counterpart of one of the run's objects
::PP.UseObject <- function(index, id, classname, x, y, z) {
	local p = Player(index)
	local e = Claim(id, classname, x, y, z)
	if (p != null && e != null) EntFireByHandle(e, "Use", "", 0, p, p)
}

// The recorded player pressed "use": press the button they were looking at, if there is one within reach
::PP.Use <- function(index, dx, dy, dz) {
	local p = Player(index)
	if (p == null) return
	local eye = p.EyePosition()
	local aim = Vector(dx, dy, dz)
	local best = null
	local bestDot = 0.7
	foreach (classname in ["prop_button", "prop_under_button", "func_button"]) {
		local e = null
		while (e = Entities.FindByClassnameWithin(e, classname, eye, 110)) {
			local to = e.GetCenter() - eye
			local distance = to.Length()
			if (distance < 1) continue
			local dot = to.Dot(aim) / distance
			if (dot > bestDot) {
				bestDot = dot
				best = e
			}
		}
	}
	if (best != null) EntFireByHandle(best, "Press", "", 0, p, p)
}

// A player the trainer is moving can't be hurt (a turret, a laser or goo on the way would otherwise kill them
// and break the replay): all damage to them is filtered out until they are handed back
::PP.Shield <- function(index, on) {
	local p = Player(index)
	if (p == null) return
	if (on && Entities.FindByName(null, "p2practice_nodamage") == null) {
		local filter = Entities.CreateByClassname("filter_activator_name")
		filter.__KeyValueFromString("targetname", "p2practice_nodamage")
		filter.__KeyValueFromString("filtername", "p2practice_nobody")
	}
	EntFireByHandle(p, "SetDamageFilter", on ? "p2practice_nodamage" : "", 0, null, null)
}

::PP.Team <- function(index, team) {
	local p = Player(index)
	if (p != null) p.SetTeam(team)
}

// A copy of a player model, in one flat colour, that the trainer moves along the recorded run. It is never
// spawned as a prop, so it has no collision: players, portals and lasers pass through it.
::PP.Ghost <- function(id, model, r, g, b) {
	if (id in ghosts && ghosts[id] != null && ghosts[id].IsValid()) return
	local e = CreateProp("prop_dynamic", Vector(0, 0, -16000), model, 1)
	e.__KeyValueFromString("targetname", "p2practice_ghost" + id)
	e.__KeyValueFromString("rendercolor", r + " " + g + " " + b)
	e.__KeyValueFromInt("disableshadows", 1)
	ghosts[id] <- e
}

::PP.Gm <- function(id, x, y, z, yaw) {
	if (!(id in ghosts) || ghosts[id] == null || !ghosts[id].IsValid()) return
	ghosts[id].SetOrigin(Vector(x, y, z))
	ghosts[id].SetAngles(0, yaw, 0)
}

::PP.ClearGhosts <- function() {
	foreach (id, e in ghosts) if (e != null && e.IsValid()) e.Destroy()
	ghosts.clear()
}

// On-screen text: one game_text per (player, channel)
::PP.Text <- function(index, channel, x, y, r, g, b) {
	local key = index * 8 + channel
	local t = (key in texts) ? texts[key] : null
	if (t == null || !t.IsValid()) {
		t = Entities.CreateByClassname("game_text")
		t.__KeyValueFromString("targetname", "p2practice_text" + key)
		t.__KeyValueFromInt("channel", channel)
		t.__KeyValueFromInt("effect", 0)
		t.__KeyValueFromFloat("fadein", 0)
		t.__KeyValueFromFloat("fadeout", 0.15)
		t.__KeyValueFromFloat("holdtime", 0.35)
		texts[key] <- t
	}
	t.__KeyValueFromFloat("x", x)
	t.__KeyValueFromFloat("y", y)
	t.__KeyValueFromString("color", r + " " + g + " " + b)
}

::PP.Say <- function(index, channel, hold, message) {
	local key = index * 8 + channel
	if (!(key in texts) || texts[key] == null || !texts[key].IsValid()) return
	local p = Player(index)
	if (p == null) return
	local t = texts[key]
	t.__KeyValueFromFloat("holdtime", hold)
	t.__KeyValueFromString("message", message)
	EntFireByHandle(t, "Display", "", 0, p, p)
}

::PP.ClearTexts <- function() {
	foreach (key, t in texts) if (t != null && t.IsValid()) t.Destroy()
	texts.clear()
}

// The portal with this entity index, or null
::PP.Portal <- function(index) {
	local e = null
	while (e = Entities.FindByClassname(e, "prop_portal")) if (e.entindex() == index) return e
	return null
}

// Open one of a player's own portals somewhere, as if they had shot it there
::PP.PortalMove <- function(index, x, y, z, pitch, yaw, roll, team) {
	local portal = Portal(index)
	if (portal == null) return
	if (team != 0) portal.SetTeam(team)
	EntFireByHandle(portal, "SetActivatedState", "1", 0, null, null)
	EntFireByHandle(portal, "NewLocation", x + " " + y + " " + z + " " + pitch + " " + yaw + " " + roll, 0, null, null)
}

::PP.PortalClose <- function(index) {
	local portal = Portal(index)
	if (portal != null) EntFireByHandle(portal, "SetActivatedState", "0", 0, null, null)
}

// A portal placed by the trainer takes the colour of the player whose portal it is
::PP.PortalTeam <- function(x, y, z, team) {
	local portal = Entities.FindByClassnameNearest("prop_portal", Vector(x, y, z), 16)
	if (portal != null) portal.SetTeam(team)
}

// Close every portal that isn't at one of the positions in `keep` (filled by the trainer just before)
::PP.keep <- []
::PP.CloseOthers <- function() {
	local e = null
	while (e = Entities.FindByClassname(e, "prop_portal")) {
		local wanted = false
		foreach (v in keep) if ((e.GetOrigin() - v).Length() < 16) wanted = true
		if (!wanted) EntFireByHandle(e, "SetActivatedState", "0", 0, null, null)
	}
	keep.clear()
}

::PP.ClosePortalAt <- function(x, y, z) {
	local portal = Entities.FindByClassnameNearest("prop_portal", Vector(x, y, z), 16)
	if (portal != null) EntFireByHandle(portal, "SetActivatedState", "0", 0, null, null)
}

::PP.Line <- function(x1, y1, z1, x2, y2, z2, r, g, b, seconds) {
	DebugDrawLine(Vector(x1, y1, z1), Vector(x2, y2, z2), r, g, b, false, seconds)
}

::PP.Box <- function(x, y, z, size, r, g, b, seconds) {
	DebugDrawBox(Vector(x, y, z), Vector(-size, -size, -size), Vector(size, size, size), r, g, b, 60, seconds)
}

::PP.ready <- true
)nut";

}  // namespace helper
