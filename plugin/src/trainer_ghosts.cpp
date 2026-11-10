// The ghosts: the props that show where the run's players were.
#include "trainer_internal.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "ghostmodel.h"

namespace trainer {

Rgb TeamColor(int team) {
	if (team == route::kTeamBlue) return {90, 175, 255};
	if (team == route::kTeamOrange) return {255, 160, 50};
	return {110, 255, 170};
}

// The colour a ghost of `team`'s robot is drawn in: the robot's own, or the one the player chose
Rgb GhostColor(int team) {
	if (g_set.ghostColorSet) return {g_set.ghostColor[0], g_set.ghostColor[1], g_set.ghostColor[2]};
	return TeamColor(team);
}

// Recolours the ghosts that exist (the colour or opacity changed mid-attempt)
void RecolorGhosts() {
	for (size_t a = 0; a < g.ghosts.size() && a < g.route.actors.size(); a++) {
		if (!g.ghosts[a].entIndex) continue;
		Rgb c = GhostColor(g.route.actors[a].team);
		engine::Script("::PP.Gc(%d,%d,%d,%d,%d)", static_cast<int>(a), c.r, c.g, c.b, g_set.ghostOpacity * 255 / 100);
	}
}

const char *TeamModel(int team) {
	if (team == route::kTeamBlue) return "models/player/ballbot/ballbot.mdl";
	if (team == route::kTeamOrange) return "models/player/eggbot/eggbot.mdl";
	return "models/player/chell/player.mdl";
}

// A ghost stands for every robot of the run that isn't being driven along that same run.
bool HasGhost(size_t actor) {
	if (!g_set.ghost) return false;
	const Slot &slot = g.slots[actor];
	return g.replaying || slot.human || !slot.entIndex;
}

void ClearGhosts() {
	engine::Script("::PP.ClearGhosts()");
	g.ghosts.clear();
}

// Whether a person other than the host is playing: somebody over the network, whose game doesn't have the
// plugin's own model files (a model a game can't load is drawn as a red ERROR sign)
bool RemotePerson() {
	static bool said = false;
	for (int i = 2; i <= engine::MaxClients(); i++) {
		engine::PlayerState p;
		if (!engine::GetPlayer(i, p) || p.bot) continue;
		if (!said && g_set.ghostStyle != kGhostSolid) engine::Print("A partner over the network sees the game's own models: ghosts are shown solid.\n");
		said = true;
		return true;
	}
	return false;
}

// The model a ghost of `team`'s robot is drawn with: the wireframe copy when the style asks for one, it could
// be made and everybody playing has the files (they are made on this computer), else the game's own model
std::string GhostModel(int team) {
	if (g_set.ghostStyle != kGhostSolid && !RemotePerson()) {
		std::string wire = ghostmodel::Wireframe(team, g_set.ghostStyle == kGhostBasic ? ghostmodel::kBasic : g_set.ghostStyle == kGhostWireLow ? ghostmodel::kCoarse : ghostmodel::kFull);
		if (!wire.empty()) return wire;
	}
	return TeamModel(team);
}

// A model the game hasn't seen in this level has to be registered ("precached") before anything may wear it,
// and the plugin's wireframe copies never are. The game's own prop_dynamic_create command registers the model
// of what it makes (a prop on the wall the player looks at); the helper script removes that prop when it
// makes the ghost, and falls back to the game's own model if the registration didn't happen (the player was
// looking at nothing).
void RegisterGhostModels() {
	if (g_set.ghostStyle == kGhostSolid) return;
	for (size_t a = 0; a < g.route.actors.size(); a++) {
		if (!HasGhost(a)) continue;
		std::string model = GhostModel(g.route.actors[a].team);
		if (model != TeamModel(g.route.actors[a].team) && !g.registeredModels.count(model)) {
			// (the command puts "models/" in front itself)
			engine::Execute("prop_dynamic_create %s", model.compare(0, 7, "models/") == 0 ? model.c_str() + 7 : model.c_str());
			g.registeredModels.insert(model);
		}
	}
}

void CreateGhosts() {
	g.ghosts.assign(g.route.actors.size(), Ghost{});
	for (size_t a = 0; a < g.route.actors.size(); a++) {
		if (!HasGhost(a)) continue;
		Rgb c = GhostColor(g.route.actors[a].team);
		engine::Script("::PP.Ghost(%d,\"%s\",\"%s\",%d,%d,%d)", static_cast<int>(a), GhostModel(g.route.actors[a].team).c_str(),
		               TeamModel(g.route.actors[a].team), c.r, c.g, c.b);
		if (g_set.ghostOpacity < 100) engine::Script("::PP.Gc(%d,%d,%d,%d,%d)", static_cast<int>(a), c.r, c.g, c.b, g_set.ghostOpacity * 255 / 100);
	}
}

// The smallest turn from angle `from` to angle `to`, in degrees (-180..180)
float AngleDifference(float to, float from) {
	float d = std::fmod(to - from, 360.0f);
	if (d > 180) d -= 360;
	if (d < -180) d += 360;
	return d;
}

// How a run's player moved at `tick`. A demo holds the velocity of the player who recorded it; for anyone else
// it is worked out from how far they got since the tick before.
Vec3 VelocityAt(const Route &route, int actor, int tick) {
	const Sample &now = route.At(actor, tick);
	if (Length(now.vel) > 0 || tick <= route.firstTick) return now.vel;
	const Sample &before = route.At(actor, tick - 1);
	if (!now.present || !before.present) return {};
	Vec3 travelled = Sub(now.pos, before.pos);
	constexpr float kPortalTrip = 64;  // further than anyone gets in a tick: a trip through a portal, not speed
	if (Length(travelled) > kPortalTrip) return {};
	return {travelled.x / route.tickInterval, travelled.y / route.tickInterval, travelled.z / route.tickInterval};
}

// The player models (ATLAS, P-body and Chell alike) blend their animations by "pose parameters", each a number
// between a lowest and a highest value. These are the ones a moving, aiming player uses, with the slot each has
// in the model and its range (read from the model files; the same in all three models).
struct PoseParameter {
	int slot;
	float lowest, highest;
};
constexpr PoseParameter kBodyYaw{2, -45, 45};    // the upper body's turn away from where the feet point
constexpr PoseParameter kBodyPitch{3, -45, 90};  // looking down ... up
constexpr PoseParameter kMoveSide{4, -1, 1};     // running sideways
constexpr PoseParameter kMoveAhead{5, -1, 1};    // running backwards ... forwards

void SetPose(float pose[engine::kPoseParameters], const PoseParameter &parameter, float value) {
	value = std::clamp(value, parameter.lowest, parameter.highest);
	pose[parameter.slot] = (value - parameter.lowest) / (parameter.highest - parameter.lowest);
}

// The animation for what a player is doing, as the game picks it (the names are the models' own; ATLAS,
// P-body and Chell all have them).
const char *GhostAnimation(const Sample &s, const Vec3 &velocity) {
	constexpr float kMoving = 2;    // (the game says 0.5; a speed worked out from positions jitters more than that)
	constexpr float kLeaving = 60;  // rising or falling this fast is being in the air (the ground flag comes late for
	                                // a player the demo only shows 20 times a second)
	float groundSpeed = Length({velocity.x, velocity.y, 0});
	if (!s.onGround || std::fabs(velocity.z) > kLeaving) return "portalgun_airwalk";
	if (s.ducked) return groundSpeed > kMoving ? "portalgun_crouchWalk" : "portalgun_crouch_idle";
	return groundSpeed > kMoving ? "portalgun_run" : "portalgun_standing_idle";
}

// The speed at which a model's running and crouch-walking animations cover ground at full stride (measured from
// the animations' own movement in the model files)
struct Stride {
	float run, crouchWalk;
};
Stride ModelStride(int team) {
	if (team == route::kTeamBlue) return {173.6f, 56.6f};
	if (team == route::kTeamOrange) return {175.0f, 65.9f};
	return {186.1f, 69.4f};
}

// Where a ghost's feet point. A player's body follows the view the way the game's own player animation does it:
// while moving it turns after the view, fast when far behind and easing in at the end; standing, it stays put
// until the view is more than 45 degrees aside and then follows in a 45 degree step.
void TurnFeet(Ghost &ghost, const Sample &s, const Vec3 &velocity, float dt) {
	constexpr float kTurnRate = 720, kFullRateBeyond = 60, kStandingSlack = 45;
	if (!ghost.placed) {
		ghost.placed = true;
		ghost.feetYaw = ghost.feetGoal = s.yaw;
		return;
	}
	if (Length(velocity) > 1) ghost.feetGoal = s.yaw;
	else if (std::fabs(AngleDifference(s.yaw, ghost.feetGoal)) > kStandingSlack)
		ghost.feetGoal += AngleDifference(s.yaw, ghost.feetGoal) > 0 ? kStandingSlack : -kStandingSlack;
	float behind = AngleDifference(ghost.feetGoal, ghost.feetYaw);
	float step = kTurnRate * dt * std::clamp(std::fabs(behind) / kFullRateBeyond, 0.01f, 1.0f);
	ghost.feetYaw = std::fabs(behind) <= step ? ghost.feetGoal : ghost.feetYaw + (behind > 0 ? step : -step);
}

// Puts the ghost of robot `a` where the run's player was at `tick`, moving the way that player moved: the legs
// run in the direction of travel with the stride that speed takes, the body follows the view with the lag a
// player's body has, and the upper body aims where the player aimed.
void AnimateGhost(size_t a, int tick) {
	Ghost &ghost = g.ghosts[a];
	const Sample &s = g.route.At(static_cast<int>(a), tick);
	Vec3 velocity = VelocityAt(g.route, static_cast<int>(a), tick);
	float groundSpeed = Length({velocity.x, velocity.y, 0});
	TurnFeet(ghost, s, velocity, g.route.tickInterval);

	// The animation changes once the movement has called for another one for a few ticks running (setting an
	// animation starts it from its beginning: a flicker between two would freeze the ghost on their first frames)
	constexpr int kSettleTicks = 3;
	const char *wanted = GhostAnimation(s, velocity);
	ghost.wantedTicks = wanted == ghost.wanted ? ghost.wantedTicks + 1 : 1;
	ghost.wanted = wanted;
	if (wanted != ghost.animation && (!ghost.animation || ghost.wantedTicks >= kSettleTicks)) {
		engine::Script("::PP.Ga(%d,\"%s\")", static_cast<int>(a), wanted);
		ghost.animation = wanted;
		ghost.rate = 1;
	}
	engine::Script("::PP.Gm(%d,%.2f,%.2f,%.2f,%.1f)", static_cast<int>(a), s.pos.x, s.pos.y, s.pos.z, ghost.feetYaw);

	if (!ghost.entIndex) return;
	float pose[engine::kPoseParameters];
	for (float &value : pose) value = 0.5f;  // (the slots a moving player doesn't use: the middle of their range)
	SetPose(pose, kBodyYaw, -AngleDifference(s.yaw, ghost.feetYaw));
	SetPose(pose, kBodyPitch, -s.pitch);
	// The legs: which way they run, seen from where the player looks, and how much of a full stride. (The game
	// never lets a moving player's stride fall under a quarter.)
	float ahead = 0, sideways = 0, rate = 1;
	bool walking = s.onGround && groundSpeed > 0.5f;
	if (walking) {
		const float kToRadians = 3.14159265f / 180;
		Stride full = ModelStride(g.route.actors[a].team);
		float stride = std::max(0.25f, groundSpeed / (s.ducked ? full.crouchWalk : full.run));
		float fromView = AngleDifference(std::atan2(velocity.y, velocity.x) / kToRadians, s.yaw) * kToRadians;
		ahead = std::cos(fromView) * std::min(stride, 1.0f);
		sideways = -std::sin(fromView) * std::min(stride, 1.0f);
		// Faster than a full stride covers (a speedrunner mostly is): the legs go faster rather than slide
		constexpr float kFastestLegs = 4;
		rate = std::clamp(stride, 1.0f, kFastestLegs);
	}
	SetPose(pose, kMoveAhead, ahead);
	SetPose(pose, kMoveSide, sideways);
	if (!g_noPose) engine::SetPoseParameters(ghost.entIndex, pose);
	if (std::fabs(rate - ghost.rate) > 0.05f) {
		engine::Script("::PP.Gr(%d,%.2f)", static_cast<int>(a), rate);
		ghost.rate = rate;
	}
}

void MoveGhosts() {
	if (g_noGhostMove) return;
	constexpr float kShowAgainDistance = kGhostHideDistance + 20;
	int tick = std::min(g.cursor, g.route.timerEnd);
	for (size_t a = 0; a < g.route.actors.size() && a < g.ghosts.size(); a++) {
		if (!HasGhost(a)) continue;
		const Sample &s = g.route.At(static_cast<int>(a), tick);
		if (!s.present) continue;
		// A ghost standing inside a player would fill their screen: it isn't drawn until they separate
		float nearest = 1e9f;
		for (const Slot &slot : g.slots) {
			engine::PlayerState state;
			if (slot.entIndex && engine::GetPlayer(slot.entIndex, state)) nearest = std::min(nearest, Distance(state.origin, s.pos));
		}
		Ghost &ghost = g.ghosts[a];
		bool hide = ghost.hidden ? nearest < kShowAgainDistance : nearest < kGhostHideDistance;
		if (hide != ghost.hidden) {
			engine::Script("::PP.Gv(%d,%d)", static_cast<int>(a), hide ? 0 : 1);
			ghost.hidden = hide;
		}
		AnimateGhost(a, tick);
	}
}

}  // namespace trainer
