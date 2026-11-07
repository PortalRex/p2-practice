#include "trainer.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "engine.h"
#include "helper_nut.h"
#include "ghostmodel.h"
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

enum GhostStyle { kGhostSolid = 0, kGhostWire = 1, kGhostWireLow = 2, kGhostBasic = 3 };

struct Settings {
	float leadIn = 1.5f;     // seconds the run plays itself before the players take over
	bool ghost = true;       // show the recorded players
	bool challenge = true;   // practice every run in Challenge Mode (flags, the game's timer), not only runs made in it
	bool ghostColorSet = false;  // the ghosts in one colour of the player's choosing rather than their robots' colours
	int ghostColor[3] = {255, 255, 255};
	int ghostOpacity = 100;      // percent; below 100 the ghosts are drawn see-through
	int ghostStyle = kGhostBasic;  // how: kGhostSolid, kGhostWire, kGhostWireLow, kGhostBasic
	bool hud = true;         // on-screen clock, delta, part
	bool path = true;        // draw the route and its portal shots in the world (seen by the host)
	bool keys = true;        // show the keys the recorded player holds
	bool loop = false;       // start again by itself after the finish
	float autoReset = 0;     // retry by itself when this many seconds behind (0 = never)
	float speed = 1;         // game speed while practicing
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

// A ghost: the prop that shows where one of the run's players was
struct Ghost {
	int entIndex = 0;                 // the prop's entity number, once the script that made it has reported it
	bool placed = false;              // false until it has been put somewhere (its body then faces where it looks)
	float feetYaw = 0;                // where its body faces; the upper body turns from there to where it looks
	float feetGoal = 0;               // where its body is turning to
	bool hidden = false;              // not drawn: it stands inside somebody
	const char *animation = nullptr;  // the sequence it is playing
	const char *wanted = nullptr;     // the sequence its movement calls for, and for how many ticks it has
	int wantedTicks = 0;
	float rate = 1;                   // how fast the sequence plays
};

struct Session {
	Route route;         // the run being practiced against
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
	// ChocolateLoading snapshot of the level at the start of a lead-in (see TakeSnapshotIfDue)
	int snapshotHandover = -1;    // the handover its attempt had, -1: no snapshot
	bool snapshotWatch = false;
	int snapshotCursor = 0;       // the run tick the snapshot was taken at
	int snapshotIn = 0;           // ticks until the snapshot of the lead-in that just began is taken (0: none due)
	bool snapshotLoading = false; // snap_load has been asked for: the level's tick count jumps back when it is done
	int lastTick = 0;
	bool savesOff = false;        // the game's saving was switched off for practice (and is switched back on at the end)
	bool levelTouched = false;    // the trainer has worked in this map load: nothing done in it may count as a run
	bool resumeAfterLoad = false; // the level went away in the middle of an attempt: start it again when it is back
	bool botPartner = false;      // a split-screen bot plays the other robot: the person's view fills the screen
	int waitTicks = 0;
	std::vector<Ghost> ghosts;  // one per robot of the run
	std::set<std::string> registeredModels;  // ghost models registered with the game in this map load
	std::string recordNext;  // development: a demo name to start recording under when the next attempt begins
	// Challenge Mode's leaderboard screens (see ExpectGameScreen)
	std::chrono::steady_clock::time_point closeScreensUntil{};
	int pausedFrames = 0;
	bool startScreenClosed = false;  // this level's start board has been closed
	bool ranSinceLoad = false;       // the level has simulated a frame since it was loaded
	bool catapultMade = false;       // this level has the trainer's catapult (see MakeCatapult)
	bool flagsUp = false;            // the Challenge Mode finish flags stood when the attempt began (see WatchFlags)
	bool flagsGone = false;          // a finish took them: the next attempt needs a fresh level
	int doneTicks = 0;
	int attempts = 0;
	std::vector<int> objectLastDriven;  // per route object: the last tick a driven player carried it
	std::vector<int> useKeyDown;        // players (entity indices) whose use key the trainer is holding this tick
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
// real ones. With sv_cheats on, the game records no Challenge Mode score and SAR submits nothing to the boards.
void EnsureCheats() {
	// No saves while practicing: the level's own autosaves would capture a level full of the trainer's doing, and
	// a death would then load one of those instead of starting the level again.
	if (!g.savesOff) {
		engine::Execute("save_disable 1");
		g.savesOff = true;
	}
	g.levelTouched = true;
	if (engine::CheatsOn()) return;
	engine::Execute("sv_cheats 1");
	engine::Print("sv_cheats turned on for practice.\n");
}

bool ChallengeMode();

// In Challenge Mode the game opens its leaderboard a few seconds into every level load and again at the finish
// flags, and pauses for it until a button is pressed. During practice nobody is waiting to read those: around
// the moments they come up, a pause is answered by closing them (what their own Continue button does, and SAR's
// sar_disable_challenge_stats_hud). A pause at any other time is the player's own.
void ExpectFinishScreen() {
	if (!ChallengeMode()) return;
	g.closeScreensUntil = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	g.pausedFrames = 0;
}

bool GameScreenDue() {
	if (std::chrono::steady_clock::now() <= g.closeScreensUntil) return true;
	if (g.mode == Mode::Idle || !ChallengeMode()) return false;
	// The level-start board comes up once, some seconds into the level (three in most, ten in some) ...
	constexpr float kStartScreenSeconds = 20;
	if (!g.startScreenClosed && engine::TickCount() * engine::TickInterval() < kStartScreenSeconds) return true;
	// ... and the finish board when somebody reaches the flags: about when the run did, or when a person has
	// come nearly as far as the run went
	constexpr int kNearFinishTicks = 90;
	int nearFinish = g.route.timerEnd - kNearFinishTicks;
	if (g.cursor >= nearFinish) return true;
	for (const Slot &slot : g.slots)
		if (slot.entIndex && slot.progress >= nearFinish) return true;
	return false;
}

void OnPausedFrame() {
	// (a level being loaded doesn't simulate either, before anything ran in it: no board is up then, and
	// taking those frames for one would count the start board as closed before it comes)
	if (!g.ranSinceLoad || !GameScreenDue()) return;
	int frame = g.pausedFrames++;
	if (frame % 20 != 0) return;
	// (the level-start board closes on its own command; the finish board is a menu window)
	engine::Execute(frame % 40 == 0 ? "-leaderboard 4" : "gameui_allowescape; gameui_hide");
}

void OnRunningFrame() {
	// (in co-op the finish board comes up without pausing the game: it is closed on running frames)
	if (engine::MaxClients() > 1 && std::chrono::steady_clock::now() <= g.closeScreensUntil && engine::TickCount() % 15 == 0)
		engine::Execute("gameui_allowescape; gameui_hide");
	g.ranSinceLoad = true;
	if (g.pausedFrames == 0) return;
	// The screen came and went: any later pause is the player's
	g.pausedFrames = 0;
	g.closeScreensUntil = {};
	g.startScreenClosed = true;
}

// A level the trainer has worked in never counts, whatever happens in it afterwards (practice stopped, cheats
// switched off by hand): cheats are put back on until the level is left or loaded again.
void KeepCheatsOn() {
	if (g.levelTouched && !engine::CheatsOn()) engine::Execute("sv_cheats 1");
}

void MovePlayer(int entIndex, const Sample &s, const Vec3 &velocity) {
	engine::Script("::PP.Mv(%d,%.2f,%.2f,%.2f,%.2f,%.2f,%.1f,%.1f,%.1f)", entIndex, s.pos.x, s.pos.y, s.pos.z, s.pitch, s.yaw,
	               velocity.x, velocity.y, velocity.z);
}

// The game moves players in frames. In co-op a frame is one tick. In single player the game simulates two
// ticks at once on every even tick and nothing on the odd ones in between ("alternate ticks"), which is why
// everything in a single-player demo happens in pairs of ticks.
int FrameTicks() {
	return engine::MaxClients() == 1 && engine::CvarFloat("sv_alternateticks", 0) != 0 ? 2 : 1;
}

// Whether the game simulates a frame right after this tick's plugin call. (Single player: the even ticks, as
// the plugin's tick counter sees them. Development: "prac_debug parity 1" tries the odd ones.)
int g_frameStartParity = 0;
bool g_noPose = false;  // development: "prac_debug nopose 1" leaves the ghosts' pose parameters alone
bool g_noGhostMove = false;  // development: "prac_debug noghostmove 1" leaves the ghosts where they are
bool FrameStartsNow() {
	return FrameTicks() == 1 || engine::TickCount() % 2 == g_frameStartParity;
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

// Text goes to people only. A split-screen partner nobody plays shares the person's screen, where its copy of
// every line would be drawn over theirs.
bool ShowsHud(const Slot &slot) {
	return g_set.hud && slot.entIndex && slot.human;
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

// Which live player stands in for each of `route`'s robots (0: nobody). `route` is the practiced run, or the
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

// The run that moves the players while they are driven: the practiced one, or the one replayed against it. Both
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

// Draws each played robot's line through the level from the handover to the finish, and where its portals go.
// These are debug overlays: only the player hosting the game sees them.
void DrawRoute() {
	engine::Execute("clear_debug_overlays");
	if (!g_set.path) return;
	const float kForever = 100000;
	int from = g.handover, to = g.route.timerEnd;
	int step = std::max(2, (to - from) / 300);
	for (size_t a = 0; a < g.route.actors.size(); a++) {
		if (!g.slots[a].human && !g.replaying) continue;
		Rgb c = TeamColor(g.route.actors[a].team);
		for (int tick = from; tick + step <= to; tick += step) {
			const Sample &p = g.route.At(static_cast<int>(a), tick), &q = g.route.At(static_cast<int>(a), tick + step);
			if (!p.present || !q.present || Distance(p.pos, q.pos) > 64 + 3500 * step * g.route.tickInterval) continue;
			// A debug line is one pixel wide. Three of them, a unit apart sideways and upwards, read as a band.
			float dx = q.pos.x - p.pos.x, dy = q.pos.y - p.pos.y;
			float length = std::sqrt(dx * dx + dy * dy);
			float sx = length > 0.01f ? -dy / length : 1, sy = length > 0.01f ? dx / length : 0;
			static const float offsets[3][2] = {{0, 0}, {1, 0}, {-1, 1}};  // sideways, up
			for (const auto &offset : offsets)
				engine::Script("::PP.Line(%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%d,%d,%d,%.0f)", p.pos.x + sx * offset[0], p.pos.y + sy * offset[0],
				               p.pos.z + 4 + offset[1], q.pos.x + sx * offset[0], q.pos.y + sy * offset[0], q.pos.z + 4 + offset[1], c.r, c.g, c.b,
				               kForever);
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

// How fast a run's player moved over the ground at `tick`
float GroundSpeed(const Route &route, int actor, int tick) {
	Vec3 velocity = VelocityAt(route, actor, tick);
	return Length({velocity.x, velocity.y, 0});
}

std::string SignedTime(float seconds) {
	char text[32];
	snprintf(text, sizeof(text), "%+.2f", seconds);
	return text;
}

// `text`, cut to `limit` characters at a word's end where one is near, with dots to say so.
std::string Shortened(const std::string &text, size_t limit) {
	if (text.size() <= limit) return text;
	size_t cut = text.rfind(' ', limit - 3);
	if (cut == std::string::npos || cut < limit / 2) cut = limit - 3;
	return text.substr(0, cut) + "...";
}

std::string TitleText() {
	// The game's text line holds about 62 characters at this size; what is over runs off the screen (both ends,
	// since it is centred). With the screen split between two players, each half holds fewer. The part's name
	// gets what the rest of the line leaves.
	constexpr size_t kLineLimit = 62, kSplitLineLimit = 50, kShortestName = 12;
	bool split = engine::MaxClients() > 1 && !g.botPartner;
	size_t lineLimit = split ? kSplitLineLimit : kLineLimit;
	int index = g.route.SegmentAt(std::min(g.cursor, g.route.timerEnd));
	char start[160];
	snprintf(start, sizeof(start), "%s   record %s   part %d/%d: ", g.route.name.c_str(), route::FormatTime(g.route.RunTime()).c_str(), index + 1,
	         static_cast<int>(g.route.segments.size()));
	size_t used = strlen(start);
	return start + Shortened(g.route.segments[index].name, std::max(kShortestName, lineLimit > used ? lineLimit - used : 0));
}

void UpdateHud() {
	if (!g_set.hud) return;
	const float hold = kHudInterval * g.route.tickInterval + 0.25f;
	const float dt = g.route.tickInterval;
	std::string title = TitleText();
	for (size_t a = 0; a < g.slots.size(); a++) {
		const Slot &slot = g.slots[a];
		if (!ShowsHud(slot)) continue;
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
		// the portal colours: blue while ahead of the run, orange while behind
		SetText(ent, kChClock, -1, 0.07f, ahead ? 60 : 255, ahead ? 170 : 150, ahead ? 255 : 30);
		Say(ent, kChClock, hold, route::FormatTime(g.route.TimeAt(clock)) + "   " + SignedTime(delta));

		engine::PlayerState state;
		if (engine::GetPlayer(ent, state)) {
			// (a replayed run carries the player along: its speed is the replayed run's, the player's own is zero)
			float speed = g.replaying ? GroundSpeed(Driver(), static_cast<int>(a), DriverTick(g.cursor))
			                          : Length({state.velocity.x, state.velocity.y, 0});
			char text[96];
			snprintf(text, sizeof(text), "speed %.0f   record %.0f", speed, GroundSpeed(g.route, static_cast<int>(a), slot.progress));
			Say(ent, kChSpeed, hold, text);
		}
	}
}

void Flash(float seconds, const std::string &text) {
	for (const Slot &slot : g.slots)
		if (ShowsHud(slot)) Say(slot.entIndex, kChFlash, seconds, text);
}

// ---------------------------------------------------------------------------------------------------------------
// Starting, resetting, stopping
// ---------------------------------------------------------------------------------------------------------------

void MoveObjects(bool all);
void MakeCatapult();
void ShieldPlayers(bool onlyDriven);
void ReleaseUseKeys();
void DropCarried(const Slot &slot);

// The console command that loads the route's map, for messages
std::string LoadCommand() {
	return g.route.coop ? "ss_map " + g.route.map + " to play it alone with a partner that knows the route" : "map " + g.route.map;
}

bool MapMatches() {
	return g.hasRoute && _stricmp(engine::MapName(), g.route.map.c_str()) == 0;
}

// The first tick every player of the route exists
// Which tick of the practiced run the live level's own clock is at. A level's lifts, doors and timers run on
// the server's tick count from the map's load, and the demo says what that count was when its recording began:
// where the two counts agree, everything the level does by itself happens exactly when it did in the run.
// kNoClock in co-op (there the level waits for both players before it starts, a different wait every time)
// and for demos that don't carry the count.
constexpr int kNoClock = INT_MIN;
constexpr const char *kEndNode = "challenge_mode_end_node";  // the Challenge Mode flags
int LevelClock() {
	const Route &driver = Driver();
	if (driver.coop || driver.levelTickAtZero == p2demo::Demo::kNoServerTick) return kNoClock;
	int driverTick = engine::TickCount() - driver.levelTickAtZero;
	return g.replaying ? driverTick - g.replay.timerStart + g.route.timerStart : driverTick;
}

// The least the run plays itself before the handover (a sixth of a second), however late the level's clock is
constexpr int kShortestLeadTicks = 10;

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
		if (slot.entIndex) {
			engine::Script("::PP.Shield(%d,0)", slot.entIndex);
			engine::SetGodMode(slot.entIndex, false);
		}
	ClearGhosts();
	engine::Script("::PP.ClearTexts()");
	engine::FlushScript();
	engine::Execute("clear_debug_overlays");
}

void Stop(bool announce) {
	ReleaseUseKeys();
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
	g.snapshotLoading = true;
	return true;
}

// Puts everyone on the route at the lead-in's start and lets the run play itself to the handover.
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
	if (!g.recordNext.empty()) {
		engine::Execute("record %s", g.recordNext.c_str());
		g.recordNext.clear();
	}
	std::string problem;
	if (!AssignSlots(problem)) {
		engine::Warn("Can't start: %s.\n", problem.c_str());
		g.mode = Mode::Idle;
		return;
	}
	// Hands empty: what somebody still carries from an earlier attempt would stay out of the trainer's reach
	for (const Slot &slot : g.slots)
		if (slot.entIndex) DropCarried(slot);
	// Alone with a split-screen bot partner: the person's view takes the whole screen (the game, and SAR on every
	// load, would otherwise split it in two)
	g.botPartner = false;
	for (const Slot &slot : g.slots)
		if (slot.entIndex && !slot.human) g.botPartner = true;
	if (g.botPartner) engine::Execute("ss_force_primary_fullscreen 1");
	ShieldPlayers(false);
	ClearGhosts();
	RegisterGhostModels();
	CreateGhosts();
	MakeCatapult();
	g.flagsUp = ChallengeMode() && engine::EntityExists(kEndNode);
	for (const Slot &slot : g.slots)
		if (ShowsHud(slot)) SetupHud(slot.entIndex);

	int common = FirstCommonTick(g.route);
	g.leadStart = std::max(common, g.handover - LeadTicks());
	g.attempts++;

	// Where the level's own clock is still short of the handover, the run's clock joins it: the stand-ins then
	// ride the lift exactly as the run's players did, and step out as its doors open.
	int clock = LevelClock();
	bool joined = clock != kNoClock && clock >= common && clock <= g.handover - kShortestLeadTicks;
	if (joined) g.leadStart = std::max(g.leadStart, clock);
	bool unseen = g_set.reset != kResetSoft && g.worldReached < g.leadStart && WorldEventsBetween(DriverTick(g.worldReached), DriverTick(g.leadStart));
	if ((joined && clock < g.leadStart) || unseen) {
		// The map hasn't seen what the run did before this point (or its clock isn't there yet): play that
		// stretch through quickly
		g.cursor = joined ? clock : std::max(common, g.worldReached + 1);
		SetPortals(Driver(), DriverTick(g.cursor));
		g.objectLastDriven.clear();
		MoveObjects(true);
		g.mode = Mode::CatchUp;
		SetSpeed(8);
		return;
	}
	BeginLeadIn();
}

// A route is practiced in the mode it was run in. Challenge Mode is where the finish flags and the game's own
// clock are, where no saves are kept and a death starts the level again; the game picks the mode when the
// players spawn.
bool WrongLevelMode() {
	std::vector<LivePlayer> players = LivePlayers();
	return !players.empty() && players.front().state.challenge != ChallengeMode();
}

// Loads the level again, in the route's mode. (The game's own restart_level keeps the mode the player is in.)
void ReloadLevel() {
	g.mode = Mode::Reloading;
	Flash(2, "restarting the level...");
	engine::FlushScript();
	engine::Execute("sv_bonus_challenge %d", ChallengeMode() ? 1 : 0);
	engine::Execute("changelevel %s", g.route.map.c_str());
}

// A handover in the moment after a trip through a portal would put a player half inside the exit portal without
// the game having taken them through it, and the game then pushes them out somewhere. Such a handover is moved
// to just before the trip: the player's own first steps carry them through, as they carried the run's player.
int BeforePortalTrip(int handover) {
	constexpr int kClearOfPortal = 8;  // ticks after a trip until a player has left the portal behind
	constexpr int kBeforeTrip = 3;     // how long before the trip the player takes over
	for (int pass = 0; pass < 4; pass++) {  // (a trip may follow another closely)
		int moved = handover;
		for (const route::Event &event : g.route.events)
			if (event.kind == route::EventKind::Teleport && handover >= event.tick && handover < event.tick + kClearOfPortal) moved = event.tick - 1 - kBeforeTrip;
		if (moved == handover) break;
		handover = moved;
	}
	return handover;
}

// Starts an attempt whose handover is at `handover`; reloads the map first when its state is past that point.
void Start(int handover, bool watch) {
	if (!g.hasRoute) {
		engine::Warn("No route loaded. Use prac_load <demo> [partner's demo].\n");
		return;
	}
	if (!MapMatches()) {
		engine::Warn("The route is for %s: load it first (%s).\n", g.route.name.c_str(), LoadCommand().c_str());
		return;
	}
	handover = std::clamp(BeforePortalTrip(handover), FirstCommonTick(g.route), g.route.timerEnd - 1);
	if (RestoreSnapshot(handover, watch)) return;
	RestoreSpeed();
	g.watch = watch;
	g.handover = handover;
	g.segment = g.route.SegmentAt(g.handover);
	int leadStart = std::max(FirstCommonTick(g.route), g.handover - LeadTicks());

	bool past = g.worldReached > leadStart && WorldEventsBetween(DriverTick(leadStart), DriverTick(g.worldReached));
	// (the level's own clock can't be turned back either: past the attempt's start, only a fresh level has it right)
	int clock = LevelClock();
	bool late = clock != kNoClock && clock > g.handover - kShortestLeadTicks;
	if (g_set.reset == kResetReload || WrongLevelMode() || g.flagsGone || (g_set.reset == kResetAuto && (past || late))) {
		ReloadLevel();
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

// Makes a player let go of what is in their hands (from an earlier attempt), the way a player does: by pressing
// the use key. (Removing the game's pick-up controller instead leaves the object floating without gravity.) The
// key is pressed with a console command: "+use" for the player at this computer, "cmd2 +use" for the second
// split-screen player, and released on the next tick. A partner on another computer has no key the host can
// press: for them the controller is removed, which is the best that can be done from here.
void DropCarried(const Slot &slot) {
	engine::PlayerState state;
	if (!engine::GetPlayer(slot.entIndex, state) || state.held < 0) return;
	if (slot.entIndex == 1) engine::Execute("+use");
	else if (state.bot) engine::Execute("cmd2 +use");
	else {
		engine::Script("::PP.Drop(%d)", slot.entIndex);
		return;
	}
	g.useKeyDown.push_back(slot.entIndex);
}

// Releases the use keys pressed on the previous tick.
void ReleaseUseKeys() {
	for (int entIndex : g.useKeyDown) engine::Execute(entIndex == 1 ? "-use" : "cmd2 -use");
	g.useKeyDown.clear();
}

// Things a player presses, as opposed to things a player picks up
bool IsButton(const std::string &className) {
	return className.find("button") != std::string::npos;
}

// The direction a recorded player looks in
Vec3 AimDirection(const Sample &s) {
	const float kToRadians = 3.14159265f / 180;
	float pitch = s.pitch * kToRadians, yaw = s.yaw * kToRadians;
	return {std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), -std::sin(pitch)};
}

void DriveSlots() {
	ReleaseUseKeys();
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
		// A driven player is not put where the run's player was at every tick. It is put where that player was
		// when the game's frame began, with that player's speed, and the game's own movement then carries it
		// through the frame as it carried the run's player. So it touches what that player touched, in the same
		// frame and the same order: a button pressed and released within one frame (which leaves its door open
		// for good), a trigger skipped at speed, a trip through a portal. Where no frame follows this tick, it
		// is put where the run's player was now, which is where the next frame begins.
		int from = FrameStartsNow() && tick > driver.firstTick ? tick - 1 : tick;
		const Sample &start = driver.At(actor, from);
		MovePlayer(player, start.present ? start : s, VelocityAt(driver, actor, start.present ? from : tick));

		// The buttons the recorded player pressed on this tick, the driven one presses too. (What the recorded
		// player picked up is not picked up: the trainer moves the run's objects itself, see MoveObjects.)
		const Sample &before = driver.At(actor, tick - 1);
		Vec3 aim = AimDirection(s);
		if (driver.hasUses) {
			for (const route::UseEvent &use : driver.uses) {
				if (use.tick != tick || use.actor != actor || !IsButton(use.className)) continue;
				// (with everyone's portals: a player presses buttons through them)
				int portals[4] = {-1, -1, -1, -1};
				size_t pairs = 0;
				for (const Slot &slot : g.slots)
					if (slot.entIndex && pairs < 2 && engine::GetPlayerPortals(slot.entIndex, portals[pairs * 2], portals[pairs * 2 + 1])) pairs++;
				engine::Script("::PP.UseEnt(%d,\"%s\",\"%s\",%.4f,%.4f,%.4f,%d,%d,%d,%d)", player, use.className.c_str(), use.target.c_str(), aim.x, aim.y,
				               aim.z, portals[0], portals[1], portals[2], portals[3]);
			}
		} else if (s.buttons >= 0 && (s.buttons & p2demo::IN_USE) && !(before.buttons >= 0 && (before.buttons & p2demo::IN_USE))) {
			// no record of what was used: a button in front of them, if there is one
			engine::Script("::PP.Use(%d,%.4f,%.4f,%.4f)", player, aim.x, aim.y, aim.z);
		}
		// Hands. In co-op the game makes a player who flies fast catch what they are looking at, without any key:
		// it did so for the run's player, and it does so for the driven one. While the run's player carries
		// something that is as it should be; as soon as they don't, the driven player lets go.
		engine::PlayerState live;
		if (s.held < 0 && engine::GetPlayer(player, live) && live.held > 0) DropCarried(g.slots[i]);
	}
}

// Whether the level is practiced in Challenge Mode: a run made in it always; any other run too unless the
// setting says otherwise (a chamber run is a Challenge Mode run as far as the boards are concerned, and the
// flags give the time)
bool ChallengeMode() {
	return g.route.challenge || g_set.challenge;
}

// Whether an object can be given a speed: the physics props (cubes, balls, boxes). A turret is a character
// with a physics shadow, which the game's catapult doesn't launch.
bool Launchable(const std::string &className) {
	return className.compare(0, 5, "prop_") == 0;
}

// One catapult per level, for giving objects a speed (the helper's Launch): made with the game's own
// ent_create, which spawns it (wherever the host looks; the helper moves it onto an object when needed).
void MakeCatapult() {
	if (g.catapultMade) return;
	g.catapultMade = true;
	engine::Execute("ent_create trigger_catapult");
	engine::Script("::PP.gravity <- %g;::PP.Catapult()", engine::CvarFloat("sv_gravity", 600));
}

// Whether the trainer moves a recorded object this tick. While the run plays itself, all of them. Once a
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

// Keeps the run's objects (cubes, turrets, balls, ...) on the path they took in the run. Each is tied to its
// live counterpart when it first appears. Then:
// - What a player carries in the run is put on its recorded spot every tick, with the recorded turn and speed
//   (driven players hold nothing themselves): a laser cube points exactly where it pointed.
// - What moves on its own in the run (thrown, rolling, riding a funnel, settling on a button) is the game's to
//   move, here as there; it is put back on the recorded path only when it has strayed from it, and given the
//   run's speed again (a prop; the game can't be told a turret's speed). What a player lets go of is put on
//   its way at once: the throw is the game's from there. Buttons, ball sockets and catapults keep hold of an
//   object that is left alone.
// - What lies still in the run is looked at now and then, and put back if something has pushed it away.
// With `all` (the start of a replayed stretch) every object is tied and put in place.
void MoveObjects(bool all) {
	constexpr float kSlack = 4;            // how far an object moving on its own may be from the run's path
	constexpr int kLookAtResting = 30;     // ticks between looks at an object that lies still
	constexpr float kMovingSpeed = 30;     // slower than this, an object is left to settle on its own
	const Route &driver = Driver();
	int tick = DriverTick(g.cursor);
	if (g.objectLastDriven.size() != driver.movers.size()) g.objectLastDriven.assign(driver.movers.size(), -1);
	if (all) engine::Script("::PP.ForgetObjects()");
	// what the people in the game have in their hands is theirs
	int inHands[2] = {-1, -1};
	size_t hands = 0;
	for (const Slot &slot : g.slots) {
		engine::PlayerState state;
		if (slot.entIndex && hands < 2 && engine::GetPlayer(slot.entIndex, state) && state.held > 0) inHands[hands++] = state.held;
	}
	const float dt = driver.tickInterval;
	for (size_t i = 0; i < driver.movers.size(); i++) {
		const route::Mover &mover = driver.movers[i];
		Vec3 pos, ang;
		if (!mover.At(tick, pos, ang)) continue;
		int id = static_cast<int>(i);
		if (all || tick == mover.keys.front().tick)
			engine::Script("::PP.Claim(%d,\"%s\",%.1f,%.1f,%.1f)", id, mover.className.c_str(), pos.x, pos.y, pos.z);
		if (!LooksAfterObject(i, tick)) continue;

		bool carried = false, carriedBefore = false;
		for (size_t a = 0; a < driver.actors.size(); a++) {
			if (driver.At(static_cast<int>(a), tick).held == mover.entIndex) carried = true;
			if (driver.At(static_cast<int>(a), tick - 1).held == mover.entIndex) carriedBefore = true;
		}
		bool released = carriedBefore && !carried;
		Vec3 before = pos, beforeAng = ang, after = pos, afterAng = ang;
		bool existed = mover.At(tick - 1, before, beforeAng);
		bool moving = !existed || Distance(before, pos) > 0.01f;
		bool turning = !existed || Distance(beforeAng, ang) > 0.5f;
		if (!all && !carried && !released && !moving && !turning && tick % kLookAtResting != 0) continue;

		// (its speed: where it is a tick later; none across a trip through a portal)
		constexpr float kPortalTrip = 64;
		Vec3 velocity{};
		if (mover.At(tick + 1, after, afterAng) && Distance(pos, after) < kPortalTrip) velocity = {(after.x - pos.x) / dt, (after.y - pos.y) / dt, (after.z - pos.z) / dt};
		// (how an object lies matters once it has stopped turning: a rolling ball may roll its own way)
		float slack = (all || carried || released) ? 0 : kSlack;
		bool launch = !carried && Launchable(mover.className) && Length(velocity) > kMovingSpeed;
		engine::Script("::PP.Pose(%d,\"%s\",%.2f,%.2f,%.2f,%.1f,%.1f,%.1f,%.0f,%.0f,%.0f,%d,%d,%.0f,%d,%d)", id, mover.className.c_str(), pos.x, pos.y, pos.z,
		               ang.x, ang.y, ang.z, velocity.x, velocity.y, velocity.z, inHands[0], inHands[1], slack, turning ? 0 : 1, launch ? 1 : 0);
	}
}

// At the handover a person gets what their robot holds in the run at that moment: the object is right in front
// of them (the trainer has just put it there), and they pick it up as if they had pressed the use key.
void HandOverObjects() {
	if (g.replaying) return;
	for (size_t i = 0; i < g.slots.size(); i++) {
		const Slot &slot = g.slots[i];
		if (!slot.entIndex || !slot.human) continue;
		int held = g.route.At(static_cast<int>(i), g.handover).held;
		for (size_t m = 0; m < g.route.movers.size() && held >= 0; m++) {
			const route::Mover &mover = g.route.movers[m];
			Vec3 pos, ang;
			if (mover.entIndex != held || !mover.At(g.handover, pos, ang)) continue;
			engine::PlayerState live;
			if (engine::GetPlayer(slot.entIndex, live) && live.held > 0) continue;  // (already in their hands: the game's own catch)
			engine::Script("::PP.UseObject(%d,%d,\"%s\",%.1f,%.1f,%.1f)", slot.entIndex, static_cast<int>(m), mover.className.c_str(), pos.x, pos.y, pos.z);
		}
	}
}

// A player the trainer is moving can't be hurt: a turret, a laser, goo or a crusher on the way would kill them
// and break the replay. Two guards, since the game has more than one way to hurt: a damage filter that lets
// nothing through, and its own god mode. `onlyDriven`: the people are on their own again; bots stay protected.
void ShieldPlayers(bool onlyDriven) {
	for (const Slot &slot : g.slots) {
		if (!slot.entIndex) continue;
		bool on = !onlyDriven || !slot.human || g.replaying;
		engine::Script("::PP.Shield(%d,%d)", slot.entIndex, on ? 1 : 0);
		engine::SetGodMode(slot.entIndex, on);
	}
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

		// (while the Challenge Mode flags stand, they are the finish: see WatchFlags)
		const Sample &end = g.route.At(static_cast<int>(a), g.route.timerEnd);
		float toEnd = Distance(state.origin, end.pos);
		bool arrived = g.replaying ? g.cursor >= LastDrivenTick()
		                           : !g.flagsUp && (toEnd < kFinishRadius || (slot.progress >= g.route.timerEnd - 2 && toEnd < 2 * kFinishRadius));
		if (arrived) {
			slot.finished = true;
			slot.finishCursor = g.cursor;
			slot.progress = g.route.timerEnd;  // level with the run's finish: the gap shown is now the gap in time
		}
	}
}

// The attempt is over: the time it would have made, next to the run's. From a part, the run's time up to the
// handover counts as the person's own (what the clock has been showing all along).
void Finish(bool atFlags) {
	const float dt = g.route.tickInterval;
	int finish = 0;
	for (const Slot &slot : g.slots)
		if (slot.finished) finish = std::max(finish, slot.finishCursor);
	int ticks = finish - g.handover;
	int record = g.route.timerEnd - g.handover;
	bool best = false;
	if (!g.replaying) RecordBest(g.bestFromPart, g.segment, ticks, best);
	char text[160];
	snprintf(text, sizeof(text), "%s  %s  (%s)%s", atFlags ? "FLAGS" : "FINISH", route::FormatTime(g.route.TimeAt(finish)).c_str(),
	         SignedTime((ticks - record) * dt).c_str(), best && g.attempts > 1 ? "   personal best" : "");
	Flash(4, text);
	engine::Print("%s from part %d: %s%s, record %s (%s)\n", g.replaying ? "Replayed run" : "Attempt", g.segment + 1,
	              route::FormatTime(g.route.TimeAt(finish)).c_str(), atFlags ? " at the flags" : "", route::FormatTime(g.route.RunTime()).c_str(),
	              SignedTime((ticks - record) * dt).c_str());
	g.mode = Mode::Done;
	g.doneTicks = 0;
	ExpectFinishScreen();
}

// In Challenge Mode the level's finish is its flags: the game puts an end node at the exit, ends the timer when
// a player touches it (in co-op, when both have), and removes it. With cheats on the game shows no time, so
// the attempt's own is shown instead: what the flags would have said. The node is watched for its removal (the
// touch was in the tick just simulated); a finish takes the flags away for good, so the next attempt reloads.
bool WatchFlags() {
	if (!g.flagsUp || engine::EntityExists(kEndNode)) return false;
	g.flagsUp = false;
	g.flagsGone = true;
	return true;
}

void FinishAtFlags() {
	for (Slot &slot : g.slots) {
		if (!slot.entIndex || !slot.human || slot.finished) continue;
		slot.finished = true;
		slot.finishCursor = g.cursor - 1;
		slot.progress = g.route.timerEnd;
	}
	UpdateHud();
	Finish(true);
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
	if (g.cursor > g.handover && FrameStartsNow()) {
		// The players have just been given the run's state at the handover, and a frame begins: it is theirs
		g.handover = g.cursor - 1;  // (in single player the handover falls on the tick before a frame)
		g.mode = Mode::Run;
		ShieldPlayers(true);  // the people are on their own again; bots stay protected
		HandOverObjects();
		Flash(0.8f, g.replaying ? "replay" : "GO");
	}
	g.cursor++;
}

void TickWatch() {
	TakeSnapshotIfDue();
	WatchFlags();
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
		ExpectFinishScreen();
	}
}

// Development check, written to the trace only: one frame of the game after the handover, how far each person
// is from where the run's player was by then, and how different their speed is. Both should be near zero: the
// player took over exactly what the run had, and nobody has pressed a key yet. (The run's own next state, not
// a straight line from the handover: a player about to land, bounce or go through a portal does so in between.)
void CheckHandover() {
	int next = g.handover + FrameTicks();
	for (size_t a = 0; a < g.slots.size(); a++) {
		const Slot &slot = g.slots[a];
		engine::PlayerState state;
		if (!slot.human || !engine::GetPlayer(slot.entIndex, state)) continue;
		const Sample &expected = g.route.At(static_cast<int>(a), next);
		engine::Debug("handover: player %d is %.1f units and %.1f u/s from the run", slot.entIndex, Distance(state.origin, expected.pos),
		              Distance(state.velocity, VelocityAt(g.route, static_cast<int>(a), next)));
	}
}

void TickRun() {
	if (g.cursor == g.handover + 2 && !g.replaying) CheckHandover();
	DriveSlots();
	MoveObjects(false);
	PlacePortals();
	MoveGhosts();
	MarkWorld();
	TrackPlayers();
	if (g.cursor % kHudInterval == 0) UpdateHud();
	if (WatchFlags() && !g.replaying) {
		FinishAtFlags();
		return;
	}
	if (AllTrackedFinished()) {
		UpdateHud();
		Finish(false);
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
	// After a restart: wait until everyone is back and standing in the level. Meanwhile the person's view is put
	// back to full screen again and again: the game and SAR both reset it to split while the level loads.
	if (g.botPartner && engine::TickCount() % 8 == 0) engine::Execute("ss_force_primary_fullscreen 1");
	size_t needed = g.route.coop ? 2 : 1;
	std::vector<LivePlayer> players = LivePlayers();
	size_t alive = 0;
	for (const LivePlayer &p : players) alive += p.state.alive ? 1 : 0;
	if (alive < std::min(needed, players.size()) || players.empty()) {
		g.waitTicks = 0;
		return;
	}
	// With the level's clock to go by, the attempt starts as soon as that clock reaches the run's first tick;
	// without, once everyone has stood in the level for a moment.
	int clock = LevelClock();
	++g.waitTicks;
	if (clock == kNoClock ? g.waitTicks < 45 : (g.waitTicks < 8 || clock < FirstCommonTick(g.route))) return;
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
		engine::Print("prac_load <demo> [partner's demo] - load the run to practice against\n");
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
	if (MapMatches()) engine::Print("Ready. prac_start [part] to practice, prac_watch [part] to ride along.\n");
	else engine::Print("Load %s to practice it (%s).\n", g.route.name.c_str(), LoadCommand().c_str());
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
		engine::Print("prac_goto <seconds> - practice from that time of the run\n");
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
	engine::Print("Will %s from part %d as soon as the map is running.\n", g.autoWatch ? "watch" : "practice", g.autoPart + 1);
}

void CmdReplay(const sdk::CCommand &args) {
	if (args.ArgC() < 2 || !g.hasRoute) {
		engine::Print("prac_replay <demo> [partner's demo] - play another run in the level against the loaded one\n");
		return;
	}
	Route other;
	if (!LoadRoute(args, other)) return;
	if (other.map != g.route.map) {
		engine::Warn("That run is on %s, the loaded one on %s.\n", other.name.c_str(), g.route.name.c_str());
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
		engine::Warn("That run is on %s, the loaded one on %s.\n", other.name.c_str(), g.route.name.c_str());
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
		engine::Print("prac_role <blue|orange|auto> - the robot you play when practicing co-op alone (now: %s)\n",
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
	engine::Print("  ghoststyle %s  how: basic (wireframe, the large shapes), wirelow (the coarse mesh), wire (the full mesh) or solid\n",
	              g_set.ghostStyle == kGhostSolid ? "solid" : g_set.ghostStyle == kGhostWire ? "wire" : g_set.ghostStyle == kGhostWireLow ? "wirelow" : "basic");
	engine::Print("  hud %d           clock, delta and part on screen\n", g_set.hud);
	engine::Print("  keys %d          show the keys the record holds\n", g_set.keys);
	engine::Print("  path %d          draw the route and portal shots in the level (host only)\n", g_set.path);
	engine::Print("  partner %d       alone in co-op: a bot plays the other robot's half\n", g_set.partner);
	engine::Print("  loop %d          start again by itself after the finish\n", g_set.loop);
	engine::Print("  autoreset %.1f   retry by itself when this many seconds behind (0 = off)\n", g_set.autoReset);
	engine::Print("  speed %.2f       game speed while practicing\n", g_set.speed);
	engine::Print("  reset %s       back to a part's start: auto, soft (teleport only) or reload (restart the level)\n", resets[g_set.reset]);
	engine::Print("  challenge %d     practice every run in Challenge Mode (the flags end it); 0: only runs made in it\n", g_set.challenge);
	if (g_set.ghostColorSet) engine::Print("  ghostcolor %d %d %d  the ghosts' colour (red green blue), or team for their robots' colours\n", g_set.ghostColor[0], g_set.ghostColor[1], g_set.ghostColor[2]);
	else engine::Print("  ghostcolor team   the ghosts' colour: their robots' colours, or three numbers 0-255 (red green blue)\n");
	engine::Print("  ghostopacity %d  how solid the ghosts are drawn, in percent\n", g_set.ghostOpacity);
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
	else if (name == "ghoststyle") g_set.ghostStyle = value == "solid" ? kGhostSolid : value == "wire" ? kGhostWire : value == "wirelow" ? kGhostWireLow : kGhostBasic;
	else if (name == "hud") g_set.hud = on;
	else if (name == "keys") g_set.keys = on;
	else if (name == "path") g_set.path = on;
	else if (name == "partner") g_set.partner = on;
	else if (name == "loop") g_set.loop = on;
	else if (name == "autoreset") g_set.autoReset = std::max(0.0f, number);
	else if (name == "speed") g_set.speed = std::clamp(number, 0.05f, 4.0f);
	else if (name == "reset") g_set.reset = value == "soft" ? kResetSoft : value == "reload" ? kResetReload : kResetAuto;
	else if (name == "challenge") g_set.challenge = on;
	else if (name == "ghostcolor") {
		// "team", or three numbers 0-255 (as one quoted value or three arguments)
		std::string text = value;
		for (int i = 3; i < args.ArgC(); i++) text += std::string(" ") + args.Arg(i);
		int r, gg, b;
		if (text == "team") g_set.ghostColorSet = false;
		else if (sscanf(text.c_str(), "%d %d %d", &r, &gg, &b) == 3) {
			g_set.ghostColorSet = true;
			g_set.ghostColor[0] = std::clamp(r, 0, 255);
			g_set.ghostColor[1] = std::clamp(gg, 0, 255);
			g_set.ghostColor[2] = std::clamp(b, 0, 255);
		} else {
			engine::Warn("ghostcolor takes \"team\" or three numbers 0-255 (red green blue).\n");
			return;
		}
		if (g.helperLoaded) RecolorGhosts();
	}
	else if (name == "ghostopacity") {
		g_set.ghostOpacity = std::clamp(static_cast<int>(number), 0, 100);
		if (g.helperLoaded) RecolorGhosts();
	}
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
	if (args.ArgC() >= 3 && !strcmp(args.Arg(1), "exec")) {
		// (development: a console line run as the player, from a console that has no player of its own)
		engine::Execute("%s", args.ArgS() + strlen("exec "));
		return;
	}
	if (args.ArgC() >= 3 && !strcmp(args.Arg(1), "noghostmove")) {
		g_noGhostMove = atoi(args.Arg(2)) != 0;
		return;
	}
	if (args.ArgC() >= 3 && !strcmp(args.Arg(1), "nopose")) {
		g_noPose = atoi(args.Arg(2)) != 0;
		return;
	}
	if (args.ArgC() >= 3 && !strcmp(args.Arg(1), "parity")) {
		g_frameStartParity = atoi(args.Arg(2)) & 1;
		return;
	}
	if (args.ArgC() >= 4 && !strcmp(args.Arg(1), "ghost")) {
		// (from the helper script: the entity number of the ghost it made for a robot)
		size_t actor = static_cast<size_t>(atoi(args.Arg(2)));
		if (actor < g.ghosts.size()) g.ghosts[actor].entIndex = atoi(args.Arg(3));
		return;
	}
	if (args.ArgC() >= 3 && !strcmp(args.Arg(1), "record")) {
		// (for the map tests: the demo of the next attempt, begun the moment the attempt does, after any reload)
		g.recordNext = args.Arg(2);
		return;
	}
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
	engine::Print("P2 Practice: practice against a demo.\n");
	engine::Print("  prac_load <demo> [partner's demo]   the run to practice against\n");
	engine::Print("  prac_info                           its parts and events\n");
	engine::Print("  prac_start [part]                   practice from a part (the run plays itself up to it, then you take over)\n");
	engine::Print("  prac_goto <seconds>                 practice from any time of the run\n");
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
	{"prac_load", CmdLoad, "prac_load <demo> [partner's demo] - load the run to practice against", false},
	{"prac_info", CmdInfo, "prac_info - the loaded run's parts and events", true},
	{"prac_start", CmdStart, "prac_start [part] - practice from a part of the run", true},
	{"prac_goto", CmdGoto, "prac_goto <seconds> - practice from a time of the run", true},
	{"prac_retry", CmdRetry, "prac_retry - the same attempt again", true},
	{"prac_next", CmdNext, "prac_next - practice the next part", true},
	{"prac_prev", CmdPrev, "prac_prev - practice the previous part", true},
	{"prac_watch", CmdWatch, "prac_watch [part] - ride along with the recorded run", true},
	{"prac_stop", CmdStop, "prac_stop - stop practicing", true},
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
	g.snapshotLoading = false;
	g.helperLoaded = false;
	g.levelTouched = false;
	g.startScreenClosed = false;
	g.ranSinceLoad = false;
	g.catapultMade = false;
	g.flagsUp = false;
	g.flagsGone = false;
	g.registeredModels.clear();
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
	// A snapshot restore takes a few frames to come: the trainer runs on meanwhile, so when the level's clock
	// jumps back to the snapshot's, the run's cursor is set back to where the snapshot was taken
	int tick = engine::TickCount();
	if (g.snapshotLoading && tick < g.lastTick) {
		g.cursor = g.snapshotCursor;
		g.snapshotLoading = false;
	}
	g.lastTick = tick;
	if (!simulating) {
		OnPausedFrame();
		return;
	}
	OnRunningFrame();
	KeepCheatsOn();
	if (g.mode == Mode::Reloading) return;
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
