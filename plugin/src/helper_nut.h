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
	registered = {}
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

// How well a player looking from `eye` along `aim` is looking at `thing`: the cosine of the angle between the
// aim and the direction to it, or -1 when it is out of reach
::PP.reach <- 140.0
::PP.LooksAt <- function(eye, aim, thing) {
	local to = thing.GetCenter() - eye
	local distance = to.Length()
	if (distance < 1 || distance > reach) return -1.0
	return to.Dot(aim) / distance
}

// A floor button pressed and released in the same moment, as a player pressed it with the "button glitch": both
// inputs go out with no delay, so the button sends its pressed output and then its released one within one tick
::PP.PressAndRelease <- function(name) {
	local b = Entities.FindByName(null, name)
	if (b == null || b.GetClassname().find("floor_button") == null) return
	EntFireByHandle(b, "PressIn", "", 0, null, null)
	EntFireByHandle(b, "PressOut", "", 0, null, null)
}

// Use something as a player: the thing of that kind (and name, if the run recorded one) that the player is
// looking at within reach. A player also reaches through a portal: for each pair of portals given (entity
// numbers, -1 for none), a look into one of them is carried on from the other. Buttons get pressed this way;
// what a recorded player picked up is moved by the trainer instead.
::PP.UseEnt <- function(index, classname, name, dx, dy, dz, a1, b1, a2, b2) {
	local p = Player(index)
	if (p == null) return
	// the views to judge from: the player's own, and the one through every portal they are looking into
	local views = [[p.EyePosition(), Vector(dx, dy, dz)]]
	foreach (pair in [[a1, b1], [b1, a1], [a2, b2], [b2, a2]]) {
		local near = (pair[0] > 0) ? Portal(pair[0]) : null
		local far = (pair[1] > 0) ? Portal(pair[1]) : null
		if (near == null || far == null) continue
		local view = ThroughPortal(views[0][0], views[0][1], near, far)
		if (view != null) views.append(view)
	}
	local best = null
	local bestDot = 0.5
	local nearest = null
	local nearestDistance = 0.0
	local e = null
	while (e = Entities.FindByClassname(e, classname)) {
		if (name != "" && e.GetName() != name) continue
		foreach (view in views) {
			local dot = LooksAt(view[0], view[1], e)
			if (dot > bestDot) {
				bestDot = dot
				best = e
			}
		}
		local distance = (e.GetCenter() - views[0][0]).Length()
		if (nearest == null || distance < nearestDistance) {
			nearest = e
			nearestDistance = distance
		}
	}
	// (a thing the run named, with nothing in view: the nearest of that name, as long as it is within reach)
	if (best == null && name != "" && nearest != null && nearestDistance <= reach) best = nearest
	if (best != null) EntFireByHandle(best, "Use", "", 0, p, p)
}

// The view a player has through a portal: where their eye and aim are, seen from the other portal's side. Null
// when the aim doesn't go into the portal's opening.
::PP.ThroughPortal <- function(eye, aim, near, far) {
	local forward = near.GetForwardVector()
	local left = near.GetLeftVector()
	local up = near.GetUpVector()
	local offset = eye - near.GetOrigin()
	local x = offset.Dot(forward)
	local ax = aim.Dot(forward)
	if (x <= 0 || ax >= -0.01) return null  // behind the portal, or not looking into it
	local ay = aim.Dot(left)
	local az = aim.Dot(up)
	local y = offset.Dot(left)
	local z = offset.Dot(up)
	// where the aim meets the portal's plane: inside the opening (64 wide, 108 high)?
	local t = x / -ax
	if (t > reach || fabs(y + ay * t) > 34 || fabs(z + az * t) > 56) return null
	// a portal shows the other one's side turned half round its upright axis
	local farForward = far.GetForwardVector()
	local farLeft = far.GetLeftVector()
	local farUp = far.GetUpVector()
	return [far.GetOrigin() - farForward * x - farLeft * y + farUp * z, farForward * -ax - farLeft * ay + farUp * az]
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

// The smallest turn between two angles, in degrees
::PP.AngleApart <- function(a, b) {
	local d = fabs(a - b) % 360.0
	return d > 180 ? 360 - d : d
}

)nut"
// (the compiler takes a string literal of at most 16 KB at a time: the script continues in a second piece)
R"nut(
// ---- giving an object a speed ----
// A script can place a physics object but not set its speed (SetVelocity writes a field the physics engine
// doesn't read), and an object put somewhere stays asleep there, even in mid-air, until something wakes it. The
// game's catapult triggers can set a speed: one launches whatever enters it towards a target point at a set
// speed, with the velocity v = D*s/|D| + (0, 0, g*|D|/(2s)) for the offset D to the target, the speed s and the
// gravity g. With s = 2|D| that is v = 2D + (0, 0, g/4), so the target for a wanted v is D = (v - (0, 0, g/4))/2.
// (A direction launch is no use: for objects the game takes the speed itself as the upward velocity.)
// The plugin makes one catapult per level with the game's own ent_create (a script can't spawn an entity);
// it is found here, set up and kept disabled, and moved onto an object for the moment of a launch.
::PP.catapult <- null
::PP.target <- null
::PP.gravity <- 600.0  // (sv_gravity; the plugin keeps this current)
::PP.Catapult <- function() {
	if (catapult != null && catapult.IsValid()) return catapult
	local e = null
	while (e = Entities.FindByClassname(e, "trigger_catapult")) if (e.GetName() == "") catapult = e
	if (catapult == null) return null
	catapult.__KeyValueFromString("targetname", "p2practice_catapult")
	catapult.__KeyValueFromInt("spawnflags", 8)  // physics objects only
	catapult.__KeyValueFromInt("solid", 2)
	catapult.__KeyValueFromInt("useThresholdCheck", 0)
	catapult.__KeyValueFromInt("useExactVelocity", 0)
	catapult.__KeyValueFromInt("applyAngularImpulse", 0)
	catapult.SetSize(Vector(-40, -40, -40), Vector(40, 40, 40))
	target = Entities.CreateByClassname("info_target")
	target.__KeyValueFromString("targetname", "p2practice_catapult_target")
	EntFireByHandle(catapult, "SetLaunchTarget", "p2practice_catapult_target", 0, null, null)
	EntFireByHandle(catapult, "Disable", "", 0, null, null)
	return catapult
}

::PP.Launch <- function(e, vx, vy, vz) {
	local c = Catapult()
	if (c == null || target == null || !target.IsValid()) return
	local at = e.GetOrigin()
	local d = Vector(vx * 0.5, vy * 0.5, (vz - gravity * 0.25) * 0.5)
	if (d.Length() < 1) return
	target.SetOrigin(at + d)
	c.SetOrigin(at)
	EntFireByHandle(c, "SetPhysicsSpeed", (d.Length() * 2).tostring(), 0, null, null)
	EntFireByHandle(c, "Enable", "", 0, null, null)
	EntFireByHandle(e, "Wake", "", 0, null, null)  // (the catapult takes what moves into it)
	EntFireByHandle(c, "Disable", "", 0.05, null, null)
}

// One of the run's objects is put where the run had it this tick, moving as it moved. With `slack` above zero
// it is left alone while it is within that distance of the spot (and, with `turnMatters`, turned the same way
// to within a few degrees): an object the game's physics is already carrying along the run's path is not
// disturbed, which matters to buttons and catapults that have hold of it. `held1` and `held2` are the entity
// numbers of what the people in the game have in their hands (-1: nothing): those are never touched, since
// moving an object out of a player's hands breaks the game's physics. With `launch`, an object that is put
// in place is given the run's speed too (the physics carries it on from there).
::PP.Pose <- function(id, classname, x, y, z, pitch, yaw, roll, vx, vy, vz, held1, held2, slack, turnMatters, launch) {
	local e = Claim(id, classname, x, y, z)
	if (e == null) return
	local index = e.entindex()
	if (index == held1 || index == held2) return
	local spot = Vector(x, y, z)
	if (slack > 0 && (e.GetOrigin() - spot).Length() <= slack) {
		if (!turnMatters) return
		local angles = e.GetAngles()
		if (AngleApart(angles.x, pitch) < 5 && AngleApart(angles.y, yaw) < 5 && AngleApart(angles.z, roll) < 5) return
	}
	e.SetOrigin(spot)
	e.SetAngles(pitch, yaw, roll)
	if (launch) Launch(e, vx, vy, vz)
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
// spawned as a prop, so it has no collision: players, portals and lasers pass through it. The plugin is told
// the prop's entity number (it sets the model's pose there, which no script function does).
::PP.Ghost <- function(id, model, fallback, r, g, b) {
	if (id in ghosts && ghosts[id] != null && ghosts[id].IsValid()) return
	// A model of the plugin's own is only usable once something in the level wears it (see the plugin's
	// RegisterGhostModels); that something is removed here. Without it, the game's own model is used.
	if (model != fallback && !(model in registered)) {
		local worn = null
		local e = null
		while (e = Entities.FindByModel(e, model)) if (e.GetName() == "") worn = e
		if (worn == null) model = fallback
		else {
			worn.Destroy()
			registered[model] <- true
		}
	}
	local e = CreateProp("prop_dynamic", Vector(0, 0, -16000), model, 1)
	e.__KeyValueFromString("targetname", "p2practice_ghost" + id)
	e.__KeyValueFromString("rendercolor", r + " " + g + " " + b)
	e.__KeyValueFromInt("disableshadows", 1)
	e.__KeyValueFromInt("SuppressAnimSounds", 1)  // (the robots' animations carry footstep and servo sounds)
	ghosts[id] <- e
	SendToConsole("prac_debug ghost " + id + " " + e.entindex())
}

::PP.Gm <- function(id, x, y, z, yaw) {
	if (!(id in ghosts) || ghosts[id] == null || !ghosts[id].IsValid()) return
	ghosts[id].SetOrigin(Vector(x, y, z))
	ghosts[id].SetAngles(0, yaw, 0)
}

// The animation a ghost plays from now on, over and over
::PP.Ga <- function(id, sequence) {
	if (!(id in ghosts) || ghosts[id] == null || !ghosts[id].IsValid()) return
	EntFireByHandle(ghosts[id], "SetDefaultAnimation", sequence, 0, null, null)
	EntFireByHandle(ghosts[id], "SetAnimation", sequence, 0, null, null)
}

// The screen black (the run is being replayed quickly to the part) or back: the game's own screen fade,
// through a fade entity of the plugin's. `on`: fade out at once and stay; else fade in from black.
::PP.fade <- null
::PP.Dim <- function(on) {
	if (fade == null || !fade.IsValid()) {
		fade = Entities.CreateByClassname("env_fade")
		fade.__KeyValueFromString("targetname", "p2practice_fade")
		fade.__KeyValueFromString("rendercolor", "0 0 0")
		fade.__KeyValueFromInt("renderamt", 255)
	}
	fade.__KeyValueFromFloat("duration", on ? 0.08 : 0.5)
	fade.__KeyValueFromFloat("holdtime", 0)
	fade.__KeyValueFromInt("spawnflags", on ? 8 : 1)
	EntFireByHandle(fade, "Fade", "", 0, null, null)
}

// A ghost's colour and opacity (0-255; below 255 it is drawn see-through, in the game's translucent pass)
::PP.Gc <- function(id, r, g, b, a) {
	if (!(id in ghosts) || ghosts[id] == null || !ghosts[id].IsValid()) return
	ghosts[id].__KeyValueFromString("rendercolor", r + " " + g + " " + b)
	ghosts[id].__KeyValueFromInt("renderamt", a)
	ghosts[id].__KeyValueFromInt("rendermode", a < 255 ? 2 : 0)
}

// How fast a ghost's animation plays (1: as made)
::PP.Gr <- function(id, rate) {
	if (!(id in ghosts) || ghosts[id] == null || !ghosts[id].IsValid()) return
	EntFireByHandle(ghosts[id], "SetPlaybackRate", rate.tostring(), 0, null, null)
}

// Whether a ghost is drawn
::PP.Gv <- function(id, visible) {
	if (!(id in ghosts) || ghosts[id] == null || !ghosts[id].IsValid()) return
	EntFireByHandle(ghosts[id], visible ? "EnableDraw" : "DisableDraw", "", 0, null, null)
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
