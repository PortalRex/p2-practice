#include "trainer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "engine.h"
#include "helper_nut.h"
#include "route.h"

namespace trainer {
namespace {

using engine::Vec3;
using route::Route;
using route::Sample;

// ---------------------------------------------------------------------------------------------------------------
// Settings (prac_set)
// ---------------------------------------------------------------------------------------------------------------

enum ResetMode { kResetAuto, kResetSoft, kResetReload };

struct Settings {
	float leadIn = 1.5f;     // seconds the run plays itself before the players take over
	bool ghost = true;       // show the recorded players
	bool hud = true;         // on-screen clock, delta, part
	bool path = true;        // draw the route and its portal shots in the world (seen by the host)
	bool keys = true;        // show the keys the recorded player holds
	bool loop = false;       // start again by itself after the finish
	float autoReset = 0;     // retry by itself when this many seconds behind (0 = never)
	float speed = 1;         // game speed while practising
	int role = 0;            // solo co-op: the robot the player plays (team), 0 = the one they are
	bool partner = true;     // solo co-op: drive the other robot through its half of the route
	int reset = kResetAuto;  // how to get back to a part's start: auto, soft (teleport only), reload (map restart)
	bool snapshots = false;  // retry through ChocolateLoading's memory snapshots when that plugin is running
};
Settings g_set;

// ---------------------------------------------------------------------------------------------------------------
// Session state
// ---------------------------------------------------------------------------------------------------------------

enum class Mode {
	Idle,            // nothing running (a route may be loaded)
	Reloading,       // waiting for the map restart we asked for
	WaitForPlayers,  // map is up again, players still spawning
	CatchUp,         // replaying the run quickly from its start so the map's doors and triggers are as the run had them
	LeadIn,          // the run plays itself up to the handover
	Run,             // the players are in control
	Watch,           // the run plays itself to the end
	Done,            // finished: the result stays up for a moment
};

// One player of the route and whoever stands in for them now
struct Slot {
	int entIndex = 0;    // live player's entity index, 0 when nobody plays this robot
	bool human = false;  // a person controls it (otherwise the plugin drives it)
	int progress = 0;    // the route tick this player is level with
	int partStart = 0;   // run clock (cursor) when they entered their current part
	int part = 0;        // the part they are in
	bool finished = false;
	int finishCursor = 0;
};

struct Session {
	Route route;         // the run being practised against
	bool hasRoute = false;
	Route replay;        // another run that drives the players against it (prac_replay)
	bool replaying = false;

	Mode mode = Mode::Idle;
	bool watch = false;
	int cursor = 0;      // "now" on the recorded run's clock, in its demo ticks
	int handover = 0;    // tick at which the players take over
	int leadStart = 0;   // tick at which the lead-in begins
	int segment = 0;     // the part this attempt starts from
	std::vector<Slot> slots;

	// The furthest tick of the run this map load has lived through: the map's own state (doors, panels, relays)
	// is as the run had it there.
	int worldReached = 0;
	bool helperLoaded = false;  // this map load
	bool speedChanged = false;
	// ChocolateLoading snapshot of the level at the start of a lead-in (see TakeSnapshot)
	int snapshotHandover = -1;    // the handover its attempt had, -1: no snapshot
	bool snapshotWatch = false;
	int snapshotCursor = 0;       // the run tick the snapshot was taken at
	int snapshotIn = 0;           // ticks until the snapshot of the lead-in that just began is taken (0: none due)
	bool savesOff = false;        // the game's saving was switched off for practice (and is switched back on at the end)
	bool resumeAfterLoad = false; // the level went away in the middle of an attempt: start it again when it is back
	int waitTicks = 0;
	int doneTicks = 0;
	int attempts = 0;
	std::vector<int> objectLastDriven;  // per route object: the last tick a driven player carried it
	int autoPart = -1;   // prac_auto: start this part by itself once the route's map is up (-1 = not armed)
	bool autoWatch = false;
	std::map<int, int> bestFromPart;  // part an attempt started from -> best ticks to the finish
	std::map<int, int> bestPart;      // part -> best ticks through it
};
Session g;

constexpr int kHudInterval = 6;  // ticks between text updates
constexpr float kFinishRadius = 56;
constexpr float kGhostHideDistance = 44;

// Text channels (game_text has six)
// (the game draws channel 3 in a larger font than the others: the clock gets it)
enum Channel { kChTitle = 0, kChSpeed = 1, kChKeys = 2, kChClock = 3, kChFlash = 4, kChSplit = 5 };

Vec3 Sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float Length(Vec3 a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
float Distance(Vec3 a, Vec3 b) { return Length(Sub(a, b)); }

int LeadTicks() { return static_cast<int>(g_set.leadIn / g.route.tickInterval + 0.5f); }

// ---------------------------------------------------------------------------------------------------------------
// Acting on the game (all through the helper script and console commands)
// ---------------------------------------------------------------------------------------------------------------

// Loads the helper script into this map's VScript machine (once per map load).
void EnsureHelper() {
	if (g.helperLoaded) return;
	engine::CallCommand("script_execute", helper::kIncludeName);
	g.helperLoaded = true;
}

// Practice moves players and places portals, so it runs with cheats on: runs made this way can't be mistaken for
// real ones (SAR and the boards flag sv_cheats).
void EnsureCheats() {
	// No saves while practising: the level's own autosaves would capture a level full of the trainer's doing, and
	// a death would then load one of those instead of starting the level again.
	if (!g.savesOff) {
		engine::Execute("save_disable 1");
		g.savesOff = true;
	}
	if (engine::CheatsOn()) return;
	engine::Execute("sv_cheats 1");
	engine::Print("sv_cheats turned on for practice.\n");
}

void MovePlayer(int entIndex, const Sample &s) {
	engine::Script("::PP.Mv(%d,%.2f,%.2f,%.2f,%.2f,%.2f,%.1f,%.1f,%.1f)", entIndex, s.pos.x, s.pos.y, s.pos.z, s.pitch, s.yaw,
	               s.vel.x, s.vel.y, s.vel.z);
}

std::string Quote(const std::string &text) {
	std::string out = "\"";
	for (char c : text) {
		if (c == '"' || c == '\\') out += '\\';
		if (c == '\n') out += "\\n";
		else out += c;
	}
	return out + "\"";
}

void Say(int entIndex, int channel, float hold, const std::string &text) {
	engine::Script("::PP.Say(%d,%d,%.2f,%s)", entIndex, channel, hold, Quote(text).c_str());
}

void SetText(int entIndex, int channel, float x, float y, int r, int g_, int b) {
	engine::Script("::PP.Text(%d,%d,%.3f,%.3f,%d,%d,%d)", entIndex, channel, x, y, r, g_, b);
}

void SetupHud(int entIndex) {
	SetText(entIndex, kChTitle, -1, 0.03f, 235, 235, 235);
	SetText(entIndex, kChClock, -1, 0.07f, 255, 255, 255);
	SetText(entIndex, kChSplit, -1, 0.15f, 235, 235, 235);
	SetText(entIndex, kChFlash, -1, 0.30f, 255, 220, 80);
	SetText(entIndex, kChKeys, -1, 0.74f, 255, 230, 140);
	SetText(entIndex, kChSpeed, -1, 0.78f, 200, 220, 235);
}

// Portals the trainer opened through the console because nobody in the game owns them (a robot nobody plays):
// where they are, so they can be closed again.
std::vector<Vec3> g_orphanPortals;

// The live portal a recorded portal change applies to: one of the two portals of the player who stands in for
// the robot that shot it. 0 when nobody plays that robot (or the map itself placed the portal).
int LivePortal(const route::PortalChange &change, const std::vector<int> &playerOfActor) {
	if (change.actor < 0 || change.actor >= static_cast<int>(playerOfActor.size()) || !playerOfActor[change.actor]) return 0;
	int primary, secondary;
	if (!engine::GetPlayerPortals(playerOfActor[change.actor], primary, secondary)) return 0;
	int portal = change.portal2 ? secondary : primary;
	return portal > 0 ? portal : 0;
}

// Which live player stands in for each of `route`'s robots (0: nobody). `route` is the practised run, or the
// run replayed against it, whose robots are matched by team.
std::vector<int> PlayersOf(const Route &route);

void ApplyPortalChange(const Route &route, const route::PortalChange &change) {
	const std::vector<int> players = PlayersOf(route);
	int team = change.actor >= 0 ? route.actors[change.actor].team : 0;
	if (int portal = LivePortal(change, players)) {
		// the player's own portal: moved as if they had shot it there, so their next shot pairs with it
		if (change.active)
			engine::Script("::PP.PortalMove(%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d)", portal, change.pos.x, change.pos.y, change.pos.z, change.ang.x,
			               change.ang.y, change.ang.z, team);
		else
			engine::Script("::PP.PortalClose(%d)", portal);
		return;
	}
	if (change.actor < 0 && !route.coop) return;  // single player: a portal the map itself opens; the map will
	if (change.active) {
		// No live portal to move. In single player that is the player's own pair before its first shot (the gun
		// makes its portals on demand; they are pair 0). In co-op it is the pair of a robot nobody plays: it gets a
		// pair of its own. Either way the console command places it.
		int pair = route.coop ? 40 + std::max(0, change.actor) : 0;
		char args[192];
		snprintf(args, sizeof(args), "%d %d %.3f %.3f %.3f %.3f %.3f %.3f", pair, change.portal2 ? 1 : 0, change.pos.x,
		         change.pos.y, change.pos.z, change.ang.x, change.ang.y, change.ang.z);
		engine::CallCommand("portal_place", args);
		if (team) engine::Script("::PP.PortalTeam(%.1f,%.1f,%.1f,%d)", change.pos.x, change.pos.y, change.pos.z, team);
		if (route.coop) g_orphanPortals.push_back(change.pos);
		return;
	}
	// Closing: the portal is where its previous change left it
	for (auto it = route.portals.rbegin(); it != route.portals.rend(); ++it)
		if (it->tick < change.tick && it->portalEnt == change.portalEnt && it->active) {
			engine::Script("::PP.ClosePortalAt(%.1f,%.1f,%.1f)", it->pos.x, it->pos.y, it->pos.z);
			break;
		}
}

// Opens the portals the run had open at `tick` and closes the others the trainer is responsible for: the
// players' own pairs and the ones it placed itself. Portals the map placed are left alone.
void SetPortals(const Route &route, int tick) {
	for (const Vec3 &pos : g_orphanPortals) engine::Script("::PP.ClosePortalAt(%.1f,%.1f,%.1f)", pos.x, pos.y, pos.z);
	g_orphanPortals.clear();
	std::map<int, const route::PortalChange *> state;  // by demo portal entity
	for (const auto &change : route.portals)
		if (change.tick <= tick) state[change.portalEnt] = &change;
	std::vector<int> open;  // live portals that stay open
	const std::vector<int> players = PlayersOf(route);
	for (const auto &[entity, change] : state) {
		if (!change->active) continue;
		if (int portal = LivePortal(*change, players)) open.push_back(portal);
	}
	for (int player : players) {
		int pair[2];
		if (!player || !engine::GetPlayerPortals(player, pair[0], pair[1])) continue;
		for (int portal : pair)
			if (portal > 0 && std::find(open.begin(), open.end(), portal) == open.end()) engine::Script("::PP.PortalClose(%d)", portal);
	}
	// closes are queued inputs: flush them out before the opens so a portal isn't closed after being placed
	for (const auto &[entity, change] : state)
		if (change->active) ApplyPortalChange(route, *change);
}

// ---------------------------------------------------------------------------------------------------------------
// Who plays what
// ---------------------------------------------------------------------------------------------------------------

struct LivePlayer {
	int entIndex;
	engine::PlayerState state;
};

std::vector<LivePlayer> LivePlayers() {
	std::vector<LivePlayer> players;
	for (int i = 1; i <= engine::MaxClients(); i++) {
		engine::PlayerState state;
		if (engine::GetPlayer(i, state)) players.push_back({i, state});
	}
	return players;
}

// Matches the route's players to the players in the game. Solo with a split-screen partner, the person gets the
// robot they asked for (prac_role) and the bot gets the other.
bool AssignSlots(std::string &problem) {
	std::vector<LivePlayer> players = LivePlayers();
	if (players.empty()) {
		problem = "no players in the game yet";
		return false;
	}
	g.slots.assign(g.route.actors.size(), Slot{});
	if (!g.route.coop) {
		g.slots[0].entIndex = players[0].entIndex;
		g.slots[0].human = true;
		return true;
	}
	LivePlayer *person = nullptr, *bot = nullptr;
	int people = 0;
	for (LivePlayer &p : players) {
		if (p.state.bot) bot = &p;
		else {
			person = &p;
			people++;
		}
	}
	if (people == 1 && bot && g_set.role && person->state.team != g_set.role) {
		int other = g_set.role == route::kTeamBlue ? route::kTeamOrange : route::kTeamBlue;
		EnsureHelper();
		engine::Script("::PP.Team(%d,%d)", person->entIndex, g_set.role);
		engine::Script("::PP.Team(%d,%d)", bot->entIndex, other);
		engine::FlushScript();
		players = LivePlayers();
	}
	for (size_t a = 0; a < g.route.actors.size(); a++)
		for (const LivePlayer &p : players)
			if (p.state.team == g.route.actors[a].team) {
				if (p.state.bot && !g_set.partner && !g.replaying) continue;  // left alone: its robot is shown as a ghost
				g.slots[a].entIndex = p.entIndex;
				g.slots[a].human = !p.state.bot;
			}
	bool anyone = false;
	for (const Slot &slot : g.slots) anyone |= slot.entIndex != 0;
	if (!anyone) {
		problem = "nobody in the game is on a team this route has";
		return false;
	}
	return true;
}

// Whether the plugin, not a person, moves this slot's player right now.
bool Driven(const Slot &slot) {
	if (!slot.entIndex) return false;
	if (g.replaying || g.mode == Mode::LeadIn || g.mode == Mode::Watch || g.mode == Mode::CatchUp) return true;
	return !slot.human;
}

// The run that moves the players while they are driven: the practised one, or the one replayed against it. Both
// clocks are lined up at their timer starts.
const Route &Driver() { return g.replaying ? g.replay : g.route; }
int DriverTick(int cursor) { return g.replaying ? cursor - g.route.timerStart + g.replay.timerStart : cursor; }

int DriverActor(size_t slotIndex) {
	if (!g.replaying) return static_cast<int>(slotIndex);
	int actor = g.replay.ActorForTeam(g.route.actors[slotIndex].team);
	return actor >= 0 ? actor : (slotIndex < g.replay.actors.size() ? static_cast<int>(slotIndex) : -1);
}

std::vector<int> PlayersOf(const Route &route) {
	std::vector<int> players(route.actors.size(), 0);
	for (size_t a = 0; a < route.actors.size(); a++)
		for (size_t i = 0; i < g.slots.size() && i < g.route.actors.size(); i++)
			if (g.slots[i].entIndex && (g.route.actors[i].team == route.actors[a].team)) players[a] = g.slots[i].entIndex;
	return players;
}

int LastDrivenTick() { return g.replaying ? g.route.timerStart + (g.replay.timerEnd - g.replay.timerStart) : g.route.timerEnd; }

// ---------------------------------------------------------------------------------------------------------------
// Ghosts, route drawing
// ---------------------------------------------------------------------------------------------------------------

struct Rgb {
	int r, g, b;
};

Rgb TeamColor(int team) {
	if (team == route::kTeamBlue) return {90, 175, 255};
	if (team == route::kTeamOrange) return {255, 160, 50};
	return {110, 255, 170};
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

void CreateGhosts() {
	for (size_t a = 0; a < g.route.actors.size(); a++) {
		if (!HasGhost(a)) continue;
		Rgb c = TeamColor(g.route.actors[a].team);
		engine::Script("::PP.Ghost(%d,\"%s\",%d,%d,%d)", static_cast<int>(a), TeamModel(g.route.actors[a].team), c.r, c.g, c.b);
	}
}

void MoveGhosts() {
	int tick = std::min(g.cursor, g.route.timerEnd);
	for (size_t a = 0; a < g.route.actors.size(); a++) {
		if (!HasGhost(a)) continue;
		const Sample &s = g.route.At(static_cast<int>(a), tick);
		if (!s.present) continue;
		// A ghost standing inside a player would fill their screen: it stays out of sight until they separate
		bool inFace = false;
		for (const Slot &slot : g.slots) {
			engine::PlayerState state;
			if (slot.entIndex && engine::GetPlayer(slot.entIndex, state) && !state.bot && Distance(state.origin, s.pos) < kGhostHideDistance) inFace = true;
		}
		engine::Script("::PP.Gm(%d,%.2f,%.2f,%.2f,%.1f)", static_cast<int>(a), s.pos.x, s.pos.y, inFace ? s.pos.z - 20000 : s.pos.z, s.yaw);
	}
}

// Draws each played robot's line through the level from the handover to the finish, and where its portals go.
// These are debug overlays: only the player hosting the game sees them.
void DrawRoute() {
	engine::Execute("clear_debug_overlays");
	if (!g_set.path) return;
	const float kForever = 100000;
	int from = g.handover, to = g.route.timerEnd;
	int step = std::max(2, (to - from) / 500);
	for (size_t a = 0; a < g.route.actors.size(); a++) {
		if (!g.slots[a].human && !g.replaying) continue;
		Rgb c = TeamColor(g.route.actors[a].team);
		for (int tick = from; tick + step <= to; tick += step) {
			const Sample &p = g.route.At(static_cast<int>(a), tick), &q = g.route.At(static_cast<int>(a), tick + step);
			if (!p.present || !q.present || Distance(p.pos, q.pos) > 64 + 3500 * step * g.route.tickInterval) continue;
			engine::Script("::PP.Line(%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%d,%d,%d,%.0f)", p.pos.x, p.pos.y, p.pos.z + 4, q.pos.x, q.pos.y, q.pos.z + 4,
			               c.r, c.g, c.b, kForever);
		}
		for (const auto &portal : g.route.portals) {
			if (!portal.active || portal.actor != static_cast<int>(a) || portal.tick < from) continue;
			const Sample &shooter = g.route.At(static_cast<int>(a), portal.tick);
			// a small cross where the portal goes, and the line of the shot from the shooter's eye
			for (int axis = 0; axis < 3; axis++) {
				Vec3 lo = portal.pos, hi = portal.pos;
				(&lo.x)[axis] -= 14;
				(&hi.x)[axis] += 14;
				engine::Script("::PP.Line(%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,255,255,255,%.0f)", lo.x, lo.y, lo.z, hi.x, hi.y, hi.z, kForever);
			}
			engine::Script("::PP.Line(%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,255,255,255,%.0f)", shooter.pos.x, shooter.pos.y, shooter.pos.z + 56, portal.pos.x,
			               portal.pos.y, portal.pos.z, kForever);
		}
	}
}

// ---------------------------------------------------------------------------------------------------------------
// On-screen text
// ---------------------------------------------------------------------------------------------------------------

std::string KeysText(int buttons) {
	if (buttons < 0) return "";
	auto key = [buttons](int bit, const char *name) { return std::string((buttons & bit) ? name : "-"); };
	return "WR keys   " + key(p2demo::IN_FORWARD, "W") + " " + key(p2demo::IN_MOVELEFT, "A") + " " + key(p2demo::IN_BACK, "S") + " " +
	       key(p2demo::IN_MOVERIGHT, "D") + "   " + key(p2demo::IN_JUMP, "JUMP") + " " + key(p2demo::IN_DUCK, "DUCK") + "   " +
	       key(p2demo::IN_ATTACK, "M1") + " " + key(p2demo::IN_ATTACK2, "M2") + " " + key(p2demo::IN_USE, "USE");
}

std::string SignedTime(float seconds) {
	char text[32];
	snprintf(text, sizeof(text), "%+.2f", seconds);
	return text;
}

std::string TitleText() {
	const route::Segment &part = g.route.segments[g.route.SegmentAt(std::min(g.cursor, g.route.timerEnd))];
	char text[256];
	snprintf(text, sizeof(text), "%s   record %s   part %d/%d: %s", g.route.map.c_str(), route::FormatTime(g.route.RunTime()).c_str(),
	         g.route.SegmentAt(std::min(g.cursor, g.route.timerEnd)) + 1, static_cast<int>(g.route.segments.size()), part.name.c_str());
	return text;
}

void UpdateHud() {
	if (!g_set.hud) return;
	const float hold = kHudInterval * g.route.tickInterval + 0.25f;
	const float dt = g.route.tickInterval;
	std::string title = TitleText();
	for (size_t a = 0; a < g.slots.size(); a++) {
		const Slot &slot = g.slots[a];
		if (!slot.entIndex || (!slot.human && !g.replaying)) continue;
		int ent = slot.entIndex;
		Say(ent, kChTitle, hold, title);

		const Sample &wr = g.route.At(static_cast<int>(a), std::min(g.cursor, g.route.timerEnd));
		if (g_set.keys && g.route.actors[a].hasInputs) Say(ent, kChKeys, hold, KeysText(wr.buttons));

		if (g.mode == Mode::LeadIn || g.mode == Mode::CatchUp) {
			float left = (g.handover - g.cursor) * dt;
			char text[64];
			if (g.mode == Mode::CatchUp) snprintf(text, sizeof(text), "setting up the level...");
			else if (g.replaying) snprintf(text, sizeof(text), "replay starts in %.1f", left);
			else snprintf(text, sizeof(text), "you take over in %.1f", left);
			Say(ent, kChFlash, hold, text);
			if (g.mode == Mode::LeadIn) Say(ent, kChClock, hold, route::FormatTime(std::max(0.0f, g.route.TimeAt(g.cursor))));
			continue;
		}
		if (g.mode == Mode::Watch) {
			Say(ent, kChClock, hold, route::FormatTime(g.route.TimeAt(std::min(g.cursor, g.route.timerEnd))) + "   watching the record");
			continue;
		}

		// Run: clock, and how far ahead of or behind the record this player is
		int clock = slot.finished ? slot.finishCursor : g.cursor;
		float delta = (clock - slot.progress) * dt;
		bool ahead = delta < 0;
		SetText(ent, kChClock, -1, 0.07f, ahead ? 110 : 255, ahead ? 255 : 120, ahead ? 140 : 110);
		Say(ent, kChClock, hold, route::FormatTime(g.route.TimeAt(clock)) + "   " + SignedTime(delta));

		engine::PlayerState state;
		if (engine::GetPlayer(ent, state)) {
			const Sample &level = g.route.At(static_cast<int>(a), slot.progress);
			char text[96];
			snprintf(text, sizeof(text), "speed %.0f   record %.0f", Length({state.velocity.x, state.velocity.y, 0}), Length({level.vel.x, level.vel.y, 0}));
			Say(ent, kChSpeed, hold, text);
		}
	}
}

void Flash(float seconds, const std::string &text) {
	if (!g_set.hud) return;
	for (const Slot &slot : g.slots)
		if (slot.entIndex && (slot.human || g.replaying)) Say(slot.entIndex, kChFlash, seconds, text);
}

// ---------------------------------------------------------------------------------------------------------------
// Starting, resetting, stopping
// ---------------------------------------------------------------------------------------------------------------

void MoveObjects(bool all);
void ShieldPlayers(bool onlyDriven);

bool MapMatches() {
	return g.hasRoute && _stricmp(engine::MapName(), g.route.map.c_str()) == 0;
}

// The first tick every player of the route exists
int FirstCommonTick(const Route &route) {
	int first = route.firstTick;
	for (const auto &actor : route.actors)
		for (size_t i = 0; i < actor.samples.size(); i++)
			if (actor.samples[i].present) {
				first = std::max(first, route.firstTick + static_cast<int>(i));
				break;
			}
	return first;
}

void RestoreSpeed() {
	if (!g.speedChanged) return;
	engine::Execute("host_timescale 1");
	g.speedChanged = false;
}

void SetSpeed(float scale) {
	engine::Execute("host_timescale %g", scale);
	g.speedChanged = scale != 1;
}

void ClearScene() {
	if (!g.helperLoaded) return;
	for (const Slot &slot : g.slots)
		if (slot.entIndex) engine::Script("::PP.Shield(%d,0)", slot.entIndex);
	engine::Script("::PP.ClearGhosts()");
	engine::Script("::PP.ClearTexts()");
	engine::FlushScript();
	engine::Execute("clear_debug_overlays");
}

void Stop(bool announce) {
	bool wasActive = g.mode != Mode::Idle;
	g.mode = Mode::Idle;
	g.resumeAfterLoad = false;
	if (g.savesOff) {
		engine::Execute("save_disable 0");
		g.savesOff = false;
	}
	RestoreSpeed();
	ClearScene();
	if (announce && wasActive) engine::Print("Practice stopped.\n");
}

// Whether the map has to catch up with (or go back before) the run to be as the run had it at `tick`.
bool WorldEventsBetween(int after, int upTo) {
	const Route &route = Driver();
	for (int tick : route.worldTicks)
		if (tick > after && tick <= upTo) return true;
	return false;
}

// Puts everyone on the route at the lead-in's start and lets the run play itself to the handover.
// ChocolateLoading (github.com/Blenderiste09/ChocolateLoading) keeps whole game states in memory: "snap_save"
// stores one, "snap_load" brings it back in a few milliseconds. When the player runs the game through it and has
// turned "prac_set snapshots 1" on, the trainer stores the level a moment into each lead-in and a retry of the
// same attempt returns to that instead of restarting the level and replaying the run up to the part.
constexpr const char *kSnapshotName = "p2practice";
constexpr int kSnapshotDelayTicks = 3;  // lets the portals and objects just placed (queued inputs) take effect first

bool SnapshotsUsable() {
	return g_set.snapshots && !g.replaying && engine::CommandExists("snap_save") && engine::CommandExists("snap_load");
}

// Called every tick of a lead-in: takes the snapshot when it is due. The trainer's own bookkeeping is set before
// the snapshot is stored, so that it holds the same values whether or not a restore brings this plugin's memory
// back along with the game's.
void TakeSnapshotIfDue() {
	if (g.snapshotIn <= 0 || --g.snapshotIn > 0) return;
	if (!SnapshotsUsable()) return;
	g.snapshotHandover = g.handover;
	g.snapshotWatch = g.watch;
	g.snapshotCursor = g.cursor;
	engine::FlushScript();
	engine::Execute("snap_save %s", kSnapshotName);
}

// Returns to the snapshot of this very attempt, if there is one. True when it did.
bool RestoreSnapshot(int handover, bool watch) {
	if (!SnapshotsUsable() || g.snapshotHandover != handover || g.snapshotWatch != watch || g.slots.empty()) return false;
	g.watch = watch;
	g.handover = handover;
	g.segment = g.route.SegmentAt(handover);
	g.cursor = g.snapshotCursor;
	g.mode = watch ? Mode::Watch : Mode::LeadIn;
	g.snapshotIn = 0;
	g.objectLastDriven.clear();
	g.attempts++;
	for (Slot &slot : g.slots) {
		slot.progress = handover;
		slot.partStart = handover;
		slot.part = g.route.SegmentAt(handover);
		slot.finished = false;
	}
	engine::FlushScript();
	engine::Execute("snap_load %s", kSnapshotName);
	return true;
}

void BeginLeadIn() {
	g.snapshotIn = SnapshotsUsable() ? kSnapshotDelayTicks : 0;
	g.cursor = g.leadStart;
	SetPortals(Driver(), DriverTick(g.cursor));
	g.objectLastDriven.clear();
	MoveObjects(true);
	g.mode = g.watch ? Mode::Watch : Mode::LeadIn;
	SetSpeed(g_set.speed);
	for (Slot &slot : g.slots) {
		slot.progress = g.handover;
		slot.partStart = g.handover;
		slot.part = g.route.SegmentAt(g.handover);
		slot.finished = false;
	}
	DrawRoute();
}

// Gets the map into the state the run had at the lead-in's start, then begins the lead-in.
void Approach() {
	EnsureCheats();
	EnsureHelper();
	std::string problem;
	if (!AssignSlots(problem)) {
		engine::Warn("Can't start: %s.\n", problem.c_str());
		g.mode = Mode::Idle;
		return;
	}
	// Alone with a split-screen bot partner: the person's view takes the whole screen (the game, and SAR on every
	// load, would otherwise split it in two)
	for (const Slot &slot : g.slots)
		if (slot.entIndex && !slot.human) engine::Execute("ss_force_primary_fullscreen 1");
	ShieldPlayers(false);
	engine::Script("::PP.ClearGhosts()");
	CreateGhosts();
	for (const Slot &slot : g.slots)
		if (slot.entIndex && g_set.hud) SetupHud(slot.entIndex);

	int common = FirstCommonTick(g.route);
	g.leadStart = std::max(common, g.handover - LeadTicks());
	if (g.watch) g.leadStart = std::max(common, g.route.segments[g.segment].startTick - LeadTicks());
	g.attempts++;

	if (g_set.reset != kResetSoft && g.worldReached < g.leadStart && WorldEventsBetween(DriverTick(g.worldReached), DriverTick(g.leadStart))) {
		// The map hasn't seen what the run did before this point: play that stretch through quickly
		g.cursor = std::max(common, g.worldReached + 1);
		SetPortals(Driver(), DriverTick(g.cursor));
		g.objectLastDriven.clear();
		MoveObjects(true);
		g.mode = Mode::CatchUp;
		SetSpeed(8);
		return;
	}
	BeginLeadIn();
}

// Starts an attempt whose handover is at `handover`; reloads the map first when its state is past that point.
void Start(int handover, bool watch) {
	if (!g.hasRoute) {
		engine::Warn("No route loaded. Use prac_load <demo> [partner's demo].\n");
		return;
	}
	if (!MapMatches()) {
		engine::Warn("The route is for %s; load that map first (co-op alone: ss_map %s).\n", g.route.map.c_str(), g.route.map.c_str());
		return;
	}
	handover = std::clamp(handover, FirstCommonTick(g.route), g.route.timerEnd - 1);
	if (RestoreSnapshot(handover, watch)) return;
	RestoreSpeed();
	g.watch = watch;
	g.handover = handover;
	g.segment = g.route.SegmentAt(g.handover);
	int leadStart = std::max(FirstCommonTick(g.route), g.handover - LeadTicks());

	bool past = g.worldReached > leadStart && WorldEventsBetween(DriverTick(leadStart), DriverTick(g.worldReached));
	if (g_set.reset == kResetReload || (g_set.reset == kResetAuto && past)) {
		g.mode = Mode::Reloading;
		Flash(2, "restarting the level...");
		engine::FlushScript();
		engine::Execute("restart_level");
		return;
	}
	Approach();
}

void StartPart(int part, bool watch) {
	if (!g.hasRoute) {
		engine::Warn("No route loaded. Use prac_load <demo> [partner's demo].\n");
		return;
	}
	part = std::clamp(part, 0, static_cast<int>(g.route.segments.size()) - 1);
	Start(g.route.segments[part].startTick, watch);
}

// ---------------------------------------------------------------------------------------------------------------
// Per-tick work
// ---------------------------------------------------------------------------------------------------------------

// The direction a recorded player looks in
Vec3 AimDirection(const Sample &s) {
	const float kToRadians = 3.14159265f / 180;
	float pitch = s.pitch * kToRadians, yaw = s.yaw * kToRadians;
	return {std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), -std::sin(pitch)};
}

void DriveSlots() {
	const Route &driver = Driver();
	int tick = DriverTick(g.cursor);
	if (tick > driver.lastTick) return;
	for (size_t i = 0; i < g.slots.size(); i++) {
		if (!Driven(g.slots[i])) continue;
		int actor = DriverActor(i);
		if (actor < 0) continue;
		const Sample &s = driver.At(actor, tick);
		if (!s.present) continue;
		int player = g.slots[i].entIndex;
		MovePlayer(player, s);

		// What the recorded player used on this tick, the driven one uses too: buttons get pressed, cubes picked up
		const Sample &before = driver.At(actor, tick - 1);
		Vec3 aim = AimDirection(s);
		if (driver.hasUses) {
			for (const route::UseEvent &use : driver.uses) {
				if (use.tick != tick || use.actor != actor) continue;
				// One of the run's objects (what the player holds a moment later): its live counterpart is picked up.
				// Anything else (a button): the thing of that name or kind they are looking at.
				int object = -1;
				for (int ahead = 1; ahead <= 8 && object < 0; ahead++) {
					int held = driver.At(actor, tick + ahead).held;
					for (size_t m = 0; m < driver.movers.size() && held >= 0; m++)
						if (driver.movers[m].entIndex == held && driver.movers[m].className == use.className) object = static_cast<int>(m);
				}
				Vec3 pos, ang;
				if (object >= 0 && driver.movers[object].At(tick, pos, ang))
					engine::Script("::PP.UseObject(%d,%d,\"%s\",%.1f,%.1f,%.1f)", player, object, use.className.c_str(), pos.x, pos.y, pos.z);
				else
					engine::Script("::PP.UseEnt(%d,\"%s\",\"%s\",%.4f,%.4f,%.4f)", player, use.className.c_str(), use.target.c_str(), aim.x, aim.y, aim.z);
			}
		} else if (s.buttons >= 0 && (s.buttons & p2demo::IN_USE) && !(before.buttons >= 0 && (before.buttons & p2demo::IN_USE))) {
			// no record of what was used: a button in front of them, if there is one
			engine::Script("::PP.Use(%d,%.4f,%.4f,%.4f)", player, aim.x, aim.y, aim.z);
		}
		// ... and lets go when the recorded player did
		if (before.held >= 0 && s.held < 0 && tick > driver.firstTick) engine::Script("::PP.Drop(%d)", player);
	}
}

// Whether the trainer looks after a recorded object this tick. While the run plays itself, all of them. Once a
// person is in control, only what a driven player (the bot partner) carries or has let go of: the rest is the
// person's to move.
bool LooksAfterObject(size_t index, int tick) {
	const Route &driver = Driver();
	if (g.replaying || g.mode != Mode::Run) return true;
	int entity = driver.movers[index].entIndex;
	for (size_t i = 0; i < g.slots.size(); i++) {
		int actor = DriverActor(i);
		if (!Driven(g.slots[i]) || actor < 0) continue;
		if (driver.At(actor, tick).held == entity) g.objectLastDriven[index] = tick;
	}
	return g.objectLastDriven[index] >= 0;
}

// Keeps the run's objects (cubes, turrets, ...) where the run had them, without fighting the game's physics.
// Carrying is done by the driven players themselves: they pick up and drop as the recorded ones did. Here each
// object is tied to its live counterpart when it first appears, and corrected once it has come to rest in the
// run: if the live one lies somewhere else, it is put on the recorded spot. With `all` (the start of a replayed
// stretch) every object is tied and everything already at rest is corrected.
void MoveObjects(bool all) {
	constexpr int kSettledTicks = 20;
	const Route &driver = Driver();
	int tick = DriverTick(g.cursor);
	if (g.objectLastDriven.size() != driver.movers.size()) g.objectLastDriven.assign(driver.movers.size(), -1);
	if (all) engine::Script("::PP.ForgetObjects()");
	for (size_t i = 0; i < driver.movers.size(); i++) {
		const route::Mover &mover = driver.movers[i];
		Vec3 pos, ang;
		if (!mover.At(tick, pos, ang)) continue;
		int id = static_cast<int>(i);
		if (all || tick == mover.keys.front().tick)
			engine::Script("::PP.Claim(%d,\"%s\",%.1f,%.1f,%.1f)", id, mover.className.c_str(), pos.x, pos.y, pos.z);
		if (!LooksAfterObject(i, tick)) continue;
		int since = mover.RestingSince(tick);
		if (since < 0) continue;
		if (all ? tick - since >= kSettledTicks : tick - since == kSettledTicks)
			engine::Script("::PP.Rest(%d,\"%s\",%.2f,%.2f,%.2f,%.1f,%.1f,%.1f)", id, mover.className.c_str(), pos.x, pos.y, pos.z, ang.x, ang.y, ang.z);
	}
}

void ShieldPlayers(bool onlyDriven) {
	for (const Slot &slot : g.slots)
		if (slot.entIndex) engine::Script("::PP.Shield(%d,%d)", slot.entIndex, (!onlyDriven || !slot.human || g.replaying) ? 1 : 0);
}

// Portal shots of this tick. Shots by a player who is in control are theirs to make; everyone else's are placed.
void PlacePortals() {
	const Route &driver = Driver();
	int tick = DriverTick(g.cursor);
	for (const auto &change : driver.portals) {
		if (change.tick != tick) continue;
		if (g.mode == Mode::Run && !g.replaying && change.actor >= 0 && change.actor < static_cast<int>(g.slots.size()) &&
		    g.slots[change.actor].human)
			continue;
		ApplyPortalChange(driver, change);
	}
}

// The route tick a player is level with: the point of the recorded path they are at. Candidates are the ticks
// from where they were a tick ago to two seconds on; each is scored by its distance to the player, plus a cost
// for skipping far ahead (the path may come back to the same spot later), plus a slight pull towards the
// record's own clock (where the record stood still, standing there too means being level with it, not behind).
int FindProgress(int actor, Vec3 pos, int previous) {
	constexpr int kBack = 3, kAhead = 120, kFreeAhead = 8;
	constexpr float kSkipCost = 1.5f;    // units per tick skipped beyond the free ones
	constexpr float kClockPull = 0.05f;  // units per tick away from the record's clock
	constexpr float kLost = 300;
	int best = previous;
	float bestScore = 1e30f, bestDistance = 1e30f;
	int from = std::max(g.route.firstTick, previous - kBack), to = std::min(g.route.timerEnd, previous + kAhead);
	for (int tick = from; tick <= to; tick++) {
		const Sample &s = g.route.At(actor, tick);
		if (!s.present) continue;
		float distance = Distance(s.pos, pos);
		float score = distance + kSkipCost * std::max(0, tick - previous - kFreeAhead) + kClockPull * std::abs(tick - std::min(g.cursor, g.route.timerEnd));
		if (score < bestScore) {
			bestScore = score;
			bestDistance = distance;
			best = tick;
		}
	}
	if (bestDistance <= kLost) return best;
	// Nowhere near that stretch (a shortcut, a fall, a different way round): wherever on the route is nearest
	for (int tick = g.handover; tick <= g.route.timerEnd; tick++) {
		const Sample &s = g.route.At(actor, tick);
		if (!s.present) continue;
		float distance = Distance(s.pos, pos);
		if (distance < bestDistance) {
			bestDistance = distance;
			best = tick;
		}
	}
	return best;
}

void RecordBest(std::map<int, int> &table, int key, int ticks, bool &isBest) {
	auto it = table.find(key);
	isBest = it == table.end() || ticks < it->second;
	if (isBest) table[key] = ticks;
}

void TrackPlayers() {
	const float dt = g.route.tickInterval;
	for (size_t a = 0; a < g.slots.size(); a++) {
		Slot &slot = g.slots[a];
		if (!slot.entIndex || slot.finished || (!slot.human && !g.replaying)) continue;
		engine::PlayerState state;
		if (!engine::GetPlayer(slot.entIndex, state)) continue;
		slot.progress = FindProgress(static_cast<int>(a), state.origin, slot.progress);

		// Entering the next part: report this one
		while (slot.part + 1 < static_cast<int>(g.route.segments.size()) && slot.progress >= g.route.segments[slot.part].endTick) {
			const route::Segment &part = g.route.segments[slot.part];
			int ticks = g.cursor - slot.partStart;
			int record = part.endTick - std::max(part.startTick, g.handover);
			bool best = false;
			if (!g.replaying && part.startTick >= g.handover) RecordBest(g.bestPart, slot.part, ticks, best);
			char text[128];
			snprintf(text, sizeof(text), "part %d   %s  (%s)%s", slot.part + 1, route::FormatTime(ticks * dt).c_str(),
			         SignedTime((ticks - record) * dt).c_str(), best && g.attempts > 1 ? "  best" : "");
			if (g_set.hud) Say(slot.entIndex, kChSplit, 2.5f, text);
			slot.part++;
			slot.partStart = g.cursor;
		}

		const Sample &end = g.route.At(static_cast<int>(a), g.route.timerEnd);
		float toEnd = Distance(state.origin, end.pos);
		bool arrived = g.replaying ? g.cursor >= LastDrivenTick() : (toEnd < kFinishRadius || (slot.progress >= g.route.timerEnd - 2 && toEnd < 2 * kFinishRadius));
		if (arrived) {
			slot.finished = true;
			slot.finishCursor = g.cursor;
			if (!g.replaying) slot.progress = g.route.timerEnd;
		}
	}
}

void Finish() {
	const float dt = g.route.tickInterval;
	int finish = 0;
	for (const Slot &slot : g.slots)
		if (slot.finished) finish = std::max(finish, slot.finishCursor);
	int ticks = finish - g.handover;
	int record = g.route.timerEnd - g.handover;
	bool best = false;
	if (!g.replaying) RecordBest(g.bestFromPart, g.segment, ticks, best);
	char text[160];
	snprintf(text, sizeof(text), "FINISH  %s  (%s)%s", route::FormatTime(g.route.TimeAt(finish)).c_str(), SignedTime((ticks - record) * dt).c_str(),
	         best && g.attempts > 1 ? "   personal best" : "");
	Flash(4, text);
	engine::Print("%s from part %d: %s, record %s (%s)\n", g.replaying ? "Replayed run" : "Attempt", g.segment + 1,
	              route::FormatTime(g.route.TimeAt(finish)).c_str(), route::FormatTime(g.route.RunTime()).c_str(), SignedTime((ticks - record) * dt).c_str());
	g.mode = Mode::Done;
	g.doneTicks = 0;
}

bool AllTrackedFinished() {
	bool any = false;
	for (const Slot &slot : g.slots) {
		if (!slot.entIndex || (!slot.human && !g.replaying)) continue;
		if (!slot.finished) return false;
		any = true;
	}
	return any;
}

void MarkWorld() {
	g.worldReached = std::max(g.worldReached, std::min(g.cursor, g.route.lastTick));
}

void TickCatchUp() {
	DriveSlots();
	MoveObjects(false);
	PlacePortals();
	MarkWorld();
	if (g.cursor % kHudInterval == 0) UpdateHud();
	if (++g.cursor >= g.leadStart) BeginLeadIn();
}

void TickLeadIn() {
	TakeSnapshotIfDue();
	DriveSlots();
	MoveObjects(false);
	PlacePortals();
	MoveGhosts();
	MarkWorld();
	if (g.cursor % kHudInterval == 0) UpdateHud();
	if (g.cursor >= g.handover) {
		// This tick's state was the last one given: from the next tick the players move themselves
		g.mode = Mode::Run;
		ShieldPlayers(true);  // the people are on their own again; bots stay protected
		Flash(0.8f, g.replaying ? "replay" : "GO");
	}
	g.cursor++;
}

void TickWatch() {
	TakeSnapshotIfDue();
	DriveSlots();
	MoveObjects(false);
	PlacePortals();
	MoveGhosts();
	MarkWorld();
	if (g.cursor % kHudInterval == 0) UpdateHud();
	if (++g.cursor > g.route.timerEnd) {
		Flash(3, "end of the record: " + route::FormatTime(g.route.RunTime()));
		engine::Print("Watched the run to its end (%s).%c", route::FormatTime(g.route.RunTime()).c_str(), 10);
		g.mode = Mode::Done;
		g.doneTicks = 0;
	}
}

// Development check, written to the trace only: one tick after the handover, how far each person is from where
// the run's state at the handover puts them (position carried one tick on its velocity) and how different their
// speed is. Both should be near zero: the player took over exactly what the run had.
void CheckHandover() {
	const float dt = g.route.tickInterval;
	for (size_t a = 0; a < g.slots.size(); a++) {
		const Slot &slot = g.slots[a];
		engine::PlayerState state;
		if (!slot.human || !engine::GetPlayer(slot.entIndex, state)) continue;
		const Sample &s = g.route.At(static_cast<int>(a), g.handover);
		Vec3 expected = {s.pos.x + s.vel.x * dt, s.pos.y + s.vel.y * dt, s.pos.z + s.vel.z * dt};
		engine::Debug("handover: player %d is %.1f units and %.1f u/s from the run", slot.entIndex, Distance(state.origin, expected),
		              Distance(state.velocity, s.vel));
	}
}

void TickRun() {
	if (g.cursor == g.handover + 1 && !g.replaying) CheckHandover();
	DriveSlots();
	MoveObjects(false);
	PlacePortals();
	MoveGhosts();
	MarkWorld();
	TrackPlayers();
	if (g.cursor % kHudInterval == 0) UpdateHud();
	if (AllTrackedFinished()) {
		UpdateHud();
		Finish();
		return;
	}
	if (g_set.autoReset > 0 && !g.replaying) {
		for (const Slot &slot : g.slots)
			if (slot.human && !slot.finished && (g.cursor - slot.progress) * g.route.tickInterval > g_set.autoReset) {
				Start(g.handover, false);
				return;
			}
	}
	g.cursor++;
}

void TickDone() {
	const int kShowTicks = static_cast<int>(3 / g.route.tickInterval);
	if (++g.doneTicks < kShowTicks) return;
	if (g_set.loop) {
		Start(g.handover, g.watch);
		return;
	}
	g.mode = Mode::Idle;
	RestoreSpeed();
}

void TickWaitForPlayers() {
	// After a restart: wait until everyone is back and standing in the level
	size_t needed = g.route.coop ? 2 : 1;
	std::vector<LivePlayer> players = LivePlayers();
	size_t alive = 0;
	for (const LivePlayer &p : players) alive += p.state.alive ? 1 : 0;
	if (alive < std::min(needed, players.size()) || players.empty()) {
		g.waitTicks = 0;
		return;
	}
	// with a bot partner the person's view fills the screen again as soon as the level is back
	if (g.waitTicks == 0)
		for (const LivePlayer &p : players)
			if (p.state.bot) engine::Execute("ss_force_primary_fullscreen 1");
	if (++g.waitTicks < 45) return;
	Approach();
}

// prac_auto: once the route's map is running and everyone has spawned, start by itself (the launcher uses this).
void TickAutoStart() {
	if (g.autoPart < 0 || !MapMatches()) return;
	std::vector<LivePlayer> players = LivePlayers();
	size_t alive = 0;
	for (const LivePlayer &p : players) alive += p.state.alive ? 1 : 0;
	if (alive < (g.route.coop ? 2u : 1u)) {
		g.waitTicks = 0;
		return;
	}
	if (++g.waitTicks < 90) return;
	int part = g.autoPart;
	g.autoPart = -1;
	g.replaying = false;
	StartPart(part, g.autoWatch);
}

// ---------------------------------------------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------------------------------------------

// A demo named on the console: as given, or under the game folder, or its demos folder; ".dem" may be left out.
std::string FindDemo(const std::string &name) {
	namespace fs = std::filesystem;
	std::string game = engine::GameDirectory();
	const std::string candidates[] = {name, name + ".dem", game + "/" + name, game + "/" + name + ".dem", game + "/demos/" + name,
	                                  game + "/demos/" + name + ".dem"};
	std::error_code ec;
	for (const std::string &path : candidates)
		if (fs::is_regular_file(fs::u8path(path), ec)) return path;
	return "";
}

bool LoadRoute(const sdk::CCommand &args, Route &out) {
	std::vector<std::string> paths;
	for (int i = 1; i < args.ArgC() && i <= 2; i++) {
		std::string path = FindDemo(args.Arg(i));
		if (path.empty()) {
			engine::Warn("Demo not found: %s\n", args.Arg(i));
			return false;
		}
		paths.push_back(path);
	}
	std::string error;
	if (!route::Load(paths, out, error)) {
		engine::Warn("Couldn't read the run: %s\n", error.c_str());
		return false;
	}
	return true;
}

void PrintLines(const std::string &text) {
	size_t start = 0;
	while (start < text.size()) {
		size_t end = text.find('\n', start);
		if (end == std::string::npos) end = text.size();
		engine::Print("%s\n", text.substr(start, end - start).c_str());
		start = end + 1;
	}
}

void CmdLoad(const sdk::CCommand &args) {
	if (args.ArgC() < 2) {
		engine::Print("prac_load <demo> [partner's demo] - load the run to practise against\n");
		return;
	}
	Route route;
	if (!LoadRoute(args, route)) return;
	Stop(false);
	g.route = std::move(route);
	g.hasRoute = true;
	g.replaying = false;
	g.bestFromPart.clear();
	g.bestPart.clear();
	g.attempts = 0;
	g.snapshotHandover = -1;
	g.worldReached = g.route.firstTick - 1;
	PrintLines(route::Describe(g.route));
	if (MapMatches()) engine::Print("Ready. prac_start [part] to practise, prac_watch [part] to ride along.\n");
	else engine::Print("Load %s to practise it (co-op alone: ss_map %s).\n", g.route.map.c_str(), g.route.map.c_str());
}

void CmdInfo(const sdk::CCommand &) {
	if (!g.hasRoute) {
		engine::Print("No route loaded.\n");
		return;
	}
	PrintLines(route::Describe(g.route));
	for (const auto &event : g.route.events)
		engine::Print("  %6s  %s\n", route::FormatTime(g.route.TimeAt(event.tick)).c_str(), event.label.c_str());
}

int PartArgument(const sdk::CCommand &args, int fallback) {
	return args.ArgC() >= 2 ? atoi(args.Arg(1)) - 1 : fallback;
}

void CmdStart(const sdk::CCommand &args) {
	g.replaying = false;
	StartPart(PartArgument(args, g.hasRoute && g.attempts ? g.segment : 0), false);
}

void CmdWatch(const sdk::CCommand &args) {
	g.replaying = false;
	StartPart(PartArgument(args, 0), true);
}

void CmdGoto(const sdk::CCommand &args) {
	if (args.ArgC() < 2 || !g.hasRoute) {
		engine::Print("prac_goto <seconds> - practise from that time of the run\n");
		return;
	}
	g.replaying = false;
	Start(g.route.timerStart + static_cast<int>(atof(args.Arg(1)) / g.route.tickInterval + 0.5f), false);
}

void CmdRetry(const sdk::CCommand &) {
	if (g.hasRoute) Start(g.attempts ? g.handover : g.route.segments[0].startTick, g.watch);
}

void CmdNext(const sdk::CCommand &) {
	g.replaying = false;
	StartPart(g.segment + 1, false);
}

void CmdPrev(const sdk::CCommand &) {
	g.replaying = false;
	StartPart(g.segment - 1, false);
}

void CmdStop(const sdk::CCommand &) {
	Stop(true);
}

void CmdAuto(const sdk::CCommand &args) {
	std::string first = args.ArgC() >= 2 ? args.Arg(1) : "";
	if (first.empty() || first == "off") {
		g.autoPart = -1;
		engine::Print("prac_auto <part> | watch <part> | off - start by itself when the route's map is up (now: off)\n");
		return;
	}
	g.autoWatch = first == "watch";
	g.autoPart = std::max(0, atoi(g.autoWatch ? args.Arg(2) : args.Arg(1)) - 1);
	g.waitTicks = 0;
	engine::Print("Will %s from part %d as soon as the map is running.\n", g.autoWatch ? "watch" : "practise", g.autoPart + 1);
}

void CmdReplay(const sdk::CCommand &args) {
	if (args.ArgC() < 2 || !g.hasRoute) {
		engine::Print("prac_replay <demo> [partner's demo] - play another run in the level against the loaded one\n");
		return;
	}
	Route other;
	if (!LoadRoute(args, other)) return;
	if (other.map != g.route.map) {
		engine::Warn("That run is on %s, the loaded one on %s.\n", other.map.c_str(), g.route.map.c_str());
		return;
	}
	g.replay = std::move(other);
	g.replaying = true;
	engine::Print("Replaying %s (%s) against the loaded run (%s).\n", g.replay.files.empty() ? "run" : g.replay.files[0].c_str(),
	              route::FormatTime(g.replay.RunTime()).c_str(), route::FormatTime(g.route.RunTime()).c_str());
	// The players are driven the whole way, so the "handover" is the end; the lead-in starts before the first part
	g.worldReached = g.route.lastTick;  // unknown relation to this run's world: start from a fresh level
	Start(g.route.segments[0].startTick, false);
}

// Prints, part by part, where another run gains or loses against the loaded one. Needs no game: it compares the
// two recordings.
void CmdCompare(const sdk::CCommand &args) {
	if (args.ArgC() < 2 || !g.hasRoute) {
		engine::Print("prac_compare <demo> [partner's demo] - part-by-part times of another run against the loaded one\n");
		return;
	}
	Route other;
	if (!LoadRoute(args, other)) return;
	if (other.map != g.route.map) {
		engine::Warn("That run is on %s, the loaded one on %s.\n", other.map.c_str(), g.route.map.c_str());
		return;
	}
	const float dt = g.route.tickInterval;
	engine::Print("%-5s %-44s %8s %8s %8s\n", "part", "", "record", "this run", "diff");
	int previous = other.timerStart;
	int previousRecord = g.route.timerStart;
	for (size_t i = 0; i < g.route.segments.size(); i++) {
		const route::Segment &part = g.route.segments[i];
		int reached = other.timerEnd;
		if (i + 1 < g.route.segments.size()) {
			// When the other run's players are closest to where the record's players are at this part's end
			int window = std::min(other.timerEnd, previous + 3 * (part.endTick - previousRecord) + 180);
			float best = 1e30f;
			for (int tick = previous; tick <= window; tick++) {
				float total = 0;
				for (size_t a = 0; a < g.route.actors.size(); a++) {
					int actor = other.ActorForTeam(g.route.actors[a].team);
					if (actor < 0) continue;
					total += Distance(other.At(actor, tick).pos, g.route.At(static_cast<int>(a), part.endTick).pos);
				}
				if (total < best) {
					best = total;
					reached = tick;
				}
			}
		}
		int record = part.endTick - previousRecord, mine = reached - previous;
		engine::Print("%-5d %-44.44s %8s %8s %8s\n", static_cast<int>(i) + 1, part.name.c_str(), route::FormatTime(record * dt).c_str(),
		              route::FormatTime(mine * dt).c_str(), SignedTime((mine - record) * dt).c_str());
		previous = reached;
		previousRecord = part.endTick;
	}
	engine::Print("%-5s %-44s %8s %8s %8s\n", "total", "", route::FormatTime(g.route.RunTime()).c_str(), route::FormatTime(other.RunTime()).c_str(),
	              SignedTime(other.RunTime() - g.route.RunTime()).c_str());
}

void CmdRole(const sdk::CCommand &args) {
	std::string role = args.ArgC() >= 2 ? args.Arg(1) : "";
	if (role == "blue" || role == "atlas") g_set.role = route::kTeamBlue;
	else if (role == "orange" || role == "pbody" || role == "p-body") g_set.role = route::kTeamOrange;
	else if (role == "auto") g_set.role = 0;
	else {
		engine::Print("prac_role <blue|orange|auto> - the robot you play when practising co-op alone (now: %s)\n",
		              g_set.role == route::kTeamBlue ? "blue" : g_set.role == route::kTeamOrange ? "orange" : "auto");
		return;
	}
	engine::Print("You play %s; the partner bot plays the other robot.\n", g_set.role ? route::TeamName(g_set.role) : "the robot you spawn as");
}

void PrintSettings() {
	static const char *const resets[] = {"auto", "soft", "reload"};
	engine::Print("prac_set <name> <value>:\n");
	engine::Print("  leadin %.1f      seconds the run plays itself before you take over\n", g_set.leadIn);
	engine::Print("  ghost %d         show the recorded players\n", g_set.ghost);
	engine::Print("  hud %d           clock, delta and part on screen\n", g_set.hud);
	engine::Print("  keys %d          show the keys the record holds\n", g_set.keys);
	engine::Print("  path %d          draw the route and portal shots in the level (host only)\n", g_set.path);
	engine::Print("  partner %d       alone in co-op: a bot plays the other robot's half\n", g_set.partner);
	engine::Print("  loop %d          start again by itself after the finish\n", g_set.loop);
	engine::Print("  autoreset %.1f   retry by itself when this many seconds behind (0 = off)\n", g_set.autoReset);
	engine::Print("  speed %.2f       game speed while practising\n", g_set.speed);
	engine::Print("  reset %s       back to a part's start: auto, soft (teleport only) or reload (restart the level)\n", resets[g_set.reset]);
	engine::Print("  snapshots %d     retry instantly through ChocolateLoading's snapshots when it is running (experimental)\n", g_set.snapshots);
}

void CmdSet(const sdk::CCommand &args) {
	if (args.ArgC() < 3) {
		PrintSettings();
		return;
	}
	std::string name = args.Arg(1), value = args.Arg(2);
	float number = static_cast<float>(atof(value.c_str()));
	bool on = number != 0;
	if (name == "leadin") g_set.leadIn = std::clamp(number, 0.0f, 10.0f);
	else if (name == "ghost") g_set.ghost = on;
	else if (name == "hud") g_set.hud = on;
	else if (name == "keys") g_set.keys = on;
	else if (name == "path") g_set.path = on;
	else if (name == "partner") g_set.partner = on;
	else if (name == "loop") g_set.loop = on;
	else if (name == "autoreset") g_set.autoReset = std::max(0.0f, number);
	else if (name == "speed") g_set.speed = std::clamp(number, 0.05f, 4.0f);
	else if (name == "reset") g_set.reset = value == "soft" ? kResetSoft : value == "reload" ? kResetReload : kResetAuto;
	else if (name == "snapshots") {
		g_set.snapshots = on;
		g.snapshotHandover = -1;
		if (on && !engine::CommandExists("snap_save"))
			engine::Print("ChocolateLoading isn't running (no snap_save command): retries will restart the level as usual.\n");
	}
	else {
		engine::Warn("No setting named %s.\n", name.c_str());
		PrintSettings();
	}
}

// Development aid: what the plugin reads from the game right now
void CmdDebug(const sdk::CCommand &args) {
	if (args.ArgC() >= 2 && !strcmp(args.Arg(1), "raw")) {
		engine::PrintRawState();
		return;
	}
	static const char *const modes[] = {"idle", "reloading", "waiting for players", "catching up", "lead-in", "run", "watch", "done"};
	engine::Print("map %s, tick %d, mode %s, cursor %d, handover %d, world reached %d\n", engine::MapName(), engine::TickCount(),
	              modes[static_cast<int>(g.mode)], g.cursor, g.handover, g.worldReached);
	for (const LivePlayer &p : LivePlayers())
		engine::Print("player %d: team %d %s%s pos %.1f %.1f %.1f vel %.1f %.1f %.1f aim %.1f %.1f flags %d\n", p.entIndex, p.state.team,
		              p.state.bot ? "bot" : "person", p.state.alive ? "" : " (dead)", p.state.origin.x, p.state.origin.y, p.state.origin.z,
		              p.state.velocity.x, p.state.velocity.y, p.state.velocity.z, p.state.pitch, p.state.yaw, p.state.flags);
	for (size_t a = 0; a < g.slots.size(); a++)
		engine::Print("slot %d (%s): player %d %s progress %d part %d%s\n", static_cast<int>(a), route::TeamName(g.route.actors[a].team),
		              g.slots[a].entIndex, g.slots[a].human ? "person" : "driven", g.slots[a].progress, g.slots[a].part + 1,
		              g.slots[a].finished ? " finished" : "");
}

void CmdHelp(const sdk::CCommand &) {
	engine::Print("P2 Practice: practise against a demo.\n");
	engine::Print("  prac_load <demo> [partner's demo]   the run to practise against\n");
	engine::Print("  prac_info                           its parts and events\n");
	engine::Print("  prac_start [part]                   practise from a part (the run plays itself up to it, then you take over)\n");
	engine::Print("  prac_goto <seconds>                 practise from any time of the run\n");
	engine::Print("  prac_retry / prac_next / prac_prev  again, next part, previous part\n");
	engine::Print("  prac_watch [part]                   ride along with the recorded run\n");
	engine::Print("  prac_role <blue|orange|auto>        co-op alone: which robot you play; a bot plays the other\n");
	engine::Print("  prac_replay <demo> [partner's]      play another run in the level against the loaded one\n");
	engine::Print("  prac_compare <demo> [partner's]     part-by-part times of another run against the loaded one\n");
	engine::Print("  prac_set [name value]               settings\n");
	engine::Print("  prac_auto <part> | watch <part>     start by itself when the route's map is up (the launcher uses this)\n");
	engine::Print("  prac_stop                           stop\n");
}

struct Command {
	const char *name;
	sdk::CommandCallback callback;
	const char *help;
	bool partnerMayUse;  // safe for the co-op partner to run on the host
};

const Command kCommands[] = {
	{"prac_load", CmdLoad, "prac_load <demo> [partner's demo] - load the run to practise against", false},
	{"prac_info", CmdInfo, "prac_info - the loaded run's parts and events", true},
	{"prac_start", CmdStart, "prac_start [part] - practise from a part of the run", true},
	{"prac_goto", CmdGoto, "prac_goto <seconds> - practise from a time of the run", true},
	{"prac_retry", CmdRetry, "prac_retry - the same attempt again", true},
	{"prac_next", CmdNext, "prac_next - practise the next part", true},
	{"prac_prev", CmdPrev, "prac_prev - practise the previous part", true},
	{"prac_watch", CmdWatch, "prac_watch [part] - ride along with the recorded run", true},
	{"prac_stop", CmdStop, "prac_stop - stop practising", true},
	{"prac_auto", CmdAuto, "prac_auto <part> | watch <part> | off - start by itself when the route's map is up", false},
	{"prac_role", CmdRole, "prac_role <blue|orange|auto> - the robot you play when alone in co-op", false},
	{"prac_replay", CmdReplay, "prac_replay <demo> [partner's demo] - play another run against the loaded one", false},
	{"prac_compare", CmdCompare, "prac_compare <demo> [partner's demo] - part-by-part comparison with the loaded run", false},
	{"prac_set", CmdSet, "prac_set [name value] - settings", false},
	{"prac_help", CmdHelp, "prac_help - list the practice commands", true},
	{"prac_debug", CmdDebug, "prac_debug - print what the plugin reads from the game (for bug reports)", false},
};

void WriteHelperScript() {
	namespace fs = std::filesystem;
	std::error_code ec;
	fs::path path = fs::u8path(engine::GameDirectory()) / helper::kRelativePath;
	fs::create_directories(path.parent_path(), ec);
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	file << helper::kSource;
	if (!file) engine::Warn("Couldn't write %s; practice won't work.\n", path.u8string().c_str());
}

}  // namespace

void Init() {
	for (const Command &command : kCommands)
		if (!engine::RegisterCommand(command.name, command.callback, command.help)) engine::Warn("Couldn't register %s.\n", command.name);
	WriteHelperScript();
}

void Shutdown() {
	Stop(false);
	g = Session{};
}

void OnLevelInit(const char *mapName) {
	// A new map load: nothing made in the old one exists any more
	g.snapshotHandover = -1;
	g.snapshotIn = 0;
	g.helperLoaded = false;
	g.slots.clear();
	g.speedChanged = false;
	if (g.hasRoute) g.worldReached = g.route.firstTick - 1;
	// The restart we asked for, or the same level coming back by itself in the middle of an attempt (the player
	// died, or the level restarted itself): the attempt starts again as soon as everyone is in.
	bool sameMap = g.hasRoute && mapName && _stricmp(mapName, g.route.map.c_str()) == 0;
	if (sameMap && (g.mode == Mode::Reloading || g.resumeAfterLoad)) {
		if (g.resumeAfterLoad) engine::Print("The level restarted during the attempt: starting it again.\n");
		g.mode = Mode::WaitForPlayers;
		g.waitTicks = 0;
	} else {
		g.mode = Mode::Idle;
	}
	g.resumeAfterLoad = false;
}

void OnLevelShutdown() {
	g.resumeAfterLoad = g.mode == Mode::CatchUp || g.mode == Mode::LeadIn || g.mode == Mode::Run || g.mode == Mode::Watch;
	if (g.mode != Mode::Reloading) g.mode = Mode::Idle;
	g.helperLoaded = false;
	g.slots.clear();
}

void OnFrame(bool simulating) {
	if (!simulating || g.mode == Mode::Reloading) return;
	if (g.mode == Mode::Idle) {
		if (g.hasRoute && g.autoPart >= 0) {
			TickAutoStart();
			engine::FlushScript();
		}
		return;
	}
	switch (g.mode) {
	case Mode::WaitForPlayers: TickWaitForPlayers(); break;
	case Mode::CatchUp: TickCatchUp(); break;
	case Mode::LeadIn: TickLeadIn(); break;
	case Mode::Run: TickRun(); break;
	case Mode::Watch: TickWatch(); break;
	case Mode::Done: TickDone(); break;
	default: break;
	}
	engine::FlushScript();
}

bool OnClientCommand(const sdk::CCommand &args) {
	for (const Command &command : kCommands)
		if (command.partnerMayUse && !_stricmp(args.Arg(0), command.name)) {
			command.callback(args);
			engine::FlushScript();
			return true;
		}
	return false;
}

}  // namespace trainer
