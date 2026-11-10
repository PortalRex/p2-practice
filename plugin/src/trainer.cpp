// The practice session: who plays what, starting and stopping an attempt, and what happens every frame.
#include "trainer_internal.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "helper_nut.h"
#include "perf.h"

namespace trainer {

Settings g_set;

Session g;

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

// In Challenge Mode the game opens its leaderboard a few seconds into every level load, and pauses for it until
// a button is pressed. That one would stall the run playing itself up to the part, so while an attempt is being
// set up, a pause in the level's first seconds is answered by closing the board (what its own Continue button
// does, and SAR's sar_disable_challenge_stats_hud). The board at the finish flags is the game's own moment and
// is left alone, as is any other pause: the menu is the player's.
bool GameScreenDue() {
	if (!ChallengeMode() || g.startScreenClosed) return false;
	if (g.mode == Mode::Idle || g.mode == Mode::Done || g.mode == Mode::Reloading) return false;
	// (the board comes up once, some seconds into the level: three in most, ten in some)
	constexpr float kStartScreenSeconds = 20;
	return engine::TickCount() * engine::TickInterval() < kStartScreenSeconds;
}

void OnPausedFrame() {
	// (a level being loaded doesn't simulate either, before anything ran in it: no board is up then, and
	// taking those frames for one would count the start board as closed before it comes)
	if (!g.ranSinceLoad) return;
	DropSnapshotAtFlags();
	// The finish board pauses the game the moment the flags are touched: the finish is noticed here, so that
	// the time is worked out from that moment (it is shown when the game runs again)
	if (g.mode == Mode::Run && !g.replaying && WatchFlags()) FinishAtFlags();
	if (!GameScreenDue()) return;
	int frame = g.pausedFrames++;
	if (frame % 20 != 0) return;
	engine::Execute(frame % 40 == 0 ? "-leaderboard 4" : "gameui_allowescape; gameui_hide");
}

void OnRunningFrame() {
	g.ranSinceLoad = true;
	if (g.pausedFrames == 0) return;
	// The start board came and went: any later pause is the player's
	g.pausedFrames = 0;
	g.startScreenClosed = true;
}

// A level the trainer has worked in never counts, whatever happens in it afterwards (practice stopped, cheats
// switched off by hand): cheats are put back on until the level is left or loaded again.
void KeepCheatsOn() {
	if (g.levelTouched && !engine::CheatsOn()) engine::Execute("sv_cheats 1");
}

int g_positionDecimals = 4;  // development: "prac_debug decimals 2" sends positions as the trainer used to (rounded)

// (Positions go to the game exactly: rounded to hundredths, a player the run had a hair off a wall was put into
// it, and the game then holds a player it finds inside something until a random nudge frees them: the person hung
// in the Flings arrival tube after the handover, the bot partner skipped moves.)
void MovePlayer(int entIndex, const Sample &s, const Vec3 &velocity) {
	int d = g_positionDecimals;
	engine::Script("::PP.Mv(%d,%.*f,%.*f,%.*f,%.3f,%.3f,%.2f,%.2f,%.2f)", entIndex, d, s.pos.x, d, s.pos.y, d, s.pos.z, s.pitch, s.yaw,
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
bool g_scanEdicts = false;  // development: "prac_debug edicts 1" checks the edict table every frame (snapshot restores)
int g_lastBadEdict = -1;

// Development: reports a corrupted edict the moment it appears (and the table's size after a snapshot restore)
void ScanEdicts(int tick, bool restored) {
	char detail[200] = "";
	int inUse, highest;
	int bad = engine::FindBadEdict(detail, sizeof(detail), inUse, highest);
	char changes[600];
	if (engine::NoteEdictChanges(changes, sizeof(changes)) > 0) engine::Print("[edicts] tick %d:%s\n", tick, changes);
	if (restored) engine::Print("[edicts] tick %d, right after the restore: %d in use, highest %d%s%s\n", tick, inUse, highest, bad >= 0 ? ", BAD " : "", bad >= 0 ? detail : "");
	if (bad == g_lastBadEdict) return;
	g_lastBadEdict = bad;
	if (bad >= 0) engine::Print("[edicts] tick %d: %s (%d in use, highest %d)\n", tick, detail, inUse, highest);
	else engine::Print("[edicts] tick %d: all %d edicts readable (highest %d)\n", tick, inUse, highest);
}
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

// Which live player stands in for each of `route`'s robots (0: nobody). `route` is the practiced run, or the
// run replayed against it, whose robots are matched by team.
std::vector<int> PlayersOf(const Route &route) {
	std::vector<int> players(route.actors.size(), 0);
	for (size_t a = 0; a < route.actors.size(); a++)
		for (size_t i = 0; i < g.slots.size() && i < g.route.actors.size(); i++)
			if (g.slots[i].entIndex && (g.route.actors[i].team == route.actors[a].team)) players[a] = g.slots[i].entIndex;
	return players;
}

int LastDrivenTick() { return g.replaying ? g.route.timerStart + (g.replay.timerEnd - g.replay.timerStart) : g.route.timerEnd; }

// The console command that loads the route's map, for messages
std::string LoadCommand() {
	return g.route.coop ? "ss_map " + g.route.map + " to play it alone with a partner that knows the route" : "map " + g.route.map;
}

bool MapMatches() {
	return g.hasRoute && _stricmp(engine::MapName(), g.route.map.c_str()) == 0;
}

// Which tick of the practiced run the live level's own clock is at. A level's lifts, doors and timers run on
// the server's tick count from the map's load, and the demo says what that count was when its recording began:
// where the two counts agree, everything the level does by itself happens exactly when it did in the run.
// kNoClock for demos that don't carry the count.
//
// In co-op the machinery that starts with the map runs on that count too (Bridge Testing's conveyor of light
// bridges, Gel Maze's sprayers), but the players arrive at a different moment of it every time: the level waits
// for both. Where the live players are in before the run's were, the attempt waits for the clock and everything
// is in step; where they are a little late, it joins the clock and the run's first moments are skipped; where
// they are too late for that (a start by hand long after the load, a retry without a save state), the attempt
// runs off the clock as it always did in co-op, and the timed machinery is met at another point of its round.
// A level restart (prac_set reset reload) or a save state brings it back in step.
constexpr int kNoClock = INT_MIN;

int LevelClock() {
	const Route &driver = Driver();
	if (driver.levelTickAtZero == p2demo::Demo::kNoServerTick) return kNoClock;
	int driverTick = engine::TickCount() - driver.levelTickAtZero;
	return g.replaying ? driverTick - g.replay.timerStart + g.route.timerStart : driverTick;
}

// The least the run plays itself before the handover (a sixth of a second), however late the level's clock is
constexpr int kShortestLeadTicks = 10;

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

// Takes the catch-up's black screen away again (a fade back in), where there is one
void Undim() {
	if (!g.dimmed) return;
	engine::Script("::PP.Dim(0)");
	g.dimmed = false;
}

// Takes the trainer's protection off everyone again
void UnshieldPlayers() {
	for (const Slot &slot : g.slots)
		if (slot.entIndex) {
			engine::Script("::PP.Shield(%d,0)", slot.entIndex);
			engine::SetGodMode(slot.entIndex, false);
		}
}

void ClearScene() {
	ClearRoute();
	if (!g.helperLoaded) return;
	UnshieldPlayers();
	ClearGhosts();
	Undim();
	engine::Script("::PP.ClearTexts()");
	engine::FlushScript();
}

// Gives the game's own settings back as they were: saving, the game's speed, its worker threads
void ReleaseGameSettings() {
	if (g.savesOff) {
		engine::Execute("save_disable 0");
		g.savesOff = false;
	}
	RestoreSpeed();
	ReleaseThreads();
}

// Alone with a split-screen bot partner, the person's view takes the whole screen. The game splits it in two again
// at moments of its own (every time a robot spawns, which a save state coming back counts as; a level's start in
// Challenge Mode), and both views would show until the trainer noticed: so during an attempt the cvar is held at 1,
// and the game's "ss_force_primary_fullscreen 0" does nothing. While a level loads it is not held, only put back
// at the next frame: SAR starts its co-op timer when the game sets it to 0 there.
void KeepWholeScreen(bool held) {
	constexpr const char *kCvar = "ss_force_primary_fullscreen";
	if (g.botPartner && held) engine::HoldCvarAtLeast(kCvar, 1);
	else engine::ReleaseCvar(kCvar);
	if (g.botPartner && engine::CvarFloat(kCvar, 1) == 0) engine::Execute("%s 1", kCvar);
}

// Whether an attempt can't be started right now because a save state is being stored or brought back
// (ChocolateLoading holds one request at a time: a second one would replace it halfway). Says so.
bool BusyRestoring() {
	if (!g.save.Busy()) return false;
	engine::Print("A save state is on its way: try again in a moment.\n");
	return true;
}

void Stop(bool announce) {
	ReleaseUseKeys();
	SetDuckKey(false);
	bool wasActive = g.mode != Mode::Idle;
	g.mode = Mode::Idle;
	g.resumeAfterLoad = false;
	g.closeScreensAfterRestore = false;
	if (g.save.Busy()) {
		// (the game's threads stay off and the save state stays until the save or the restore is over: switched on
		// or freed under a restore, they crashed the game. OnFrame finishes the stop.)
		g.stopWhenRestored = true;
		if (announce && wasActive) engine::Print("Practice stopped.\n");
		return;
	}
	g.stopWhenRestored = false;
	ReleaseGameSettings();
	// (the save state holds the level with the ghosts and texts that go now: a retry sets the attempt up afresh)
	DropSnapshot();
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
void BeginLeadIn() {
	ScheduleSnapshot();
	g.cursor = g.leadStart;
	SetPortals(Driver(), DriverTick(g.cursor));
	g.objectLastDriven.clear();
	MoveObjects(true);
	g.mode = g.watch ? Mode::Watch : Mode::LeadIn;
	SetSpeed(g_set.speed);
	Undim();
	for (Slot &slot : g.slots) {
		slot.progress = g.handover;
		slot.partStart = g.handover;
		slot.part = g.route.SegmentAt(g.handover);
		slot.finished = false;
	}
	SetUpRoute();
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
		Stop(false);
		return;
	}
	// Hands empty: what somebody still carries from an earlier attempt would stay out of the trainer's reach
	for (const Slot &slot : g.slots)
		if (slot.entIndex) DropCarried(slot);
	g.botPartner = false;
	for (const Slot &slot : g.slots)
		if (slot.entIndex && !slot.human) g.botPartner = true;
	g.screenHeld = true;
	KeepWholeScreen(true);
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
	// (in co-op only where the run did nothing to the level in the moments that joining skips)
	if (joined && g.route.coop && g.worldReached < clock && WorldEventsBetween(DriverTick(std::max(common, g.worldReached)), DriverTick(clock)))
		joined = false;
	if (g_logArrivals) engine::Print("[clock] attempt: level clock %d, first common tick %d, handover %d: %s\n", clock, common, g.handover, joined ? "joined" : "not joined");
	if (joined) g.leadStart = std::max(g.leadStart, clock);
	// (with instant retries too: a retry never reloads the level, but a level that hasn't seen the run's start yet,
	// a fresh one, still has to be brought up to the lead-in, or a button the run pressed before it stays up)
	bool unseen = g.worldReached < g.leadStart && WorldEventsBetween(DriverTick(g.worldReached), DriverTick(g.leadStart));
	if ((joined && clock < g.leadStart) || unseen) {
		// The map hasn't seen what the run did before this point (or its clock isn't there yet): play that
		// stretch through quickly
		g.cursor = joined ? clock : std::max(common, g.worldReached + 1);
		SetPortals(Driver(), DriverTick(g.cursor));
		g.objectLastDriven.clear();
		MoveObjects(true);
		g.mode = Mode::CatchUp;
		SetSpeed(8);
		engine::Script("::PP.Dim(1)");  // (the fast-forward is better not watched: the lead-in fades in from black)
		g.dimmed = true;
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
	g.reloadAskedAt = std::chrono::steady_clock::now();
	Flash(2, "RESTARTING THE LEVEL");
	engine::FlushScript();
	engine::Execute("unpause");  // (the finish board may still be up: the level change needs the game running)
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

// A handover while one of the run's players hangs in place with speed piling up (Conversion Intro: held at a
// portal's edge for three seconds, the speed growing with every portal shot until it lets go at 2240 u/s) can't
// be handed over: put where they were with that speed, a player flies off at once instead of staying stuck. Such a
// handover moves to just before the player stopped there, so the person gets stuck the way the run's player did.
int BeforeStuck(int handover) {
	constexpr int kStuckTicks = 6;      // still this long at the handover: stuck, not just turning around
	constexpr float kStill = 2;         // units a stuck player still moves per tick (the game nudges it to and fro)
	constexpr float kStuckSpeed = 50;   // the speed it has while not moving (from the first push on)
	constexpr int kBeforeStuck = 2;
	auto still = [&](int actor, int tick) {
		const Sample &s = g.route.At(actor, tick), &before = g.route.At(actor, tick - 1);
		return s.present && before.present && Distance(s.pos, before.pos) < kStill && Length(s.vel) > kStuckSpeed;
	};
	int moved = handover;
	for (size_t a = 0; a < g.route.actors.size(); a++) {
		int actor = static_cast<int>(a);
		bool stuck = handover - kStuckTicks > g.route.firstTick;
		for (int t = handover - kStuckTicks + 1; stuck && t <= handover; t++) stuck = still(actor, t);
		if (!stuck) continue;
		int t = handover;
		while (t > g.route.firstTick + 1 && still(actor, t)) t--;
		moved = std::min(moved, t - kBeforeStuck);
	}
	return moved;
}

// Starts an attempt whose handover is at `handover`; reloads the map first when its state is past that point.
void Start(int handover, bool watch) {
	// (a save or a restore asked for a moment ago is still on its way: ChocolateLoading holds one request at a time,
	// so a second one now would replace it halfway)
	if (g.save.Busy()) return;
	g.stopWhenRestored = false;
	if (!g.hasRoute) {
		engine::Warn("No route loaded. Use prac_load <demo> [partner's demo].\n");
		return;
	}
	if (!MapMatches()) {
		engine::Warn("The route is for %s: load it first (%s).\n", g.route.name.c_str(), LoadCommand().c_str());
		return;
	}
	int first = FirstCommonTick(g.route);
	handover = std::clamp(BeforePortalTrip(BeforeStuck(handover)), first, std::max(first, g.route.timerEnd - 1));
	if (RestoreSnapshot(handover, watch)) return;
	RestoreSpeed();
	g.watch = watch;
	g.handover = handover;
	g.segment = g.route.SegmentAt(g.handover);
	int leadStart = std::max(FirstCommonTick(g.route), g.handover - LeadTicks());

	bool past = g.worldReached > leadStart && WorldEventsBetween(DriverTick(leadStart), DriverTick(g.worldReached));
	// (the level's own clock can't be turned back either: past the attempt's start, only a fresh level has it right)
	int clock = LevelClock();
	// (not in co-op, where a restart takes long and both players with it: there a late attempt runs off the clock)
	bool late = !g.route.coop && clock != kNoClock && clock > g.handover - kShortestLeadTicks;
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

// Whether the level is practiced in Challenge Mode: a run made in it always; any other run too unless the
// setting says otherwise (a chamber run is a Challenge Mode run as far as the boards are concerned, and the
// flags give the time)
bool ChallengeMode() {
	return g.route.challenge || g_set.challenge;
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
	// (with save states: the worker threads go now, well before the lead-in's snapshot)
	if (SnapshotsUsable()) PinThreads();
	DriveSlots();
	MoveObjects(false);
	PlacePortals();
	MarkWorld();
	if (g.cursor % kHudInterval == 0) UpdateHud();
	if (++g.cursor >= g.leadStart) BeginLeadIn();
}

// The game's own Challenge Mode clock (bottom right) counts from the level's start, which includes the time
// before the run's start, waiting for the players, catching up and every retry. It is set to the trainer's
// clock instead, tick by tick, until the person's flags stop it.
void SyncChallengeClock() {
	int tick = g.mode == Mode::Run ? LiveTick() : std::max(g.cursor, g.route.timerStart);
	float seconds = std::max(0, std::min(tick, g.route.timerEnd) - g.route.timerStart) * g.route.tickInterval;
	for (const Slot &slot : g.slots)
		if (slot.entIndex && !slot.finished) engine::SetChallengeSeconds(slot.entIndex, seconds);
}

void TickLeadIn() {
	SyncChallengeClock();
	TakeSnapshotIfDue();
	DriveSlots();
	MoveObjects(false);
	PlacePortals();
	MoveGhosts();
	MarkWorld();
	if (g.cursor % kHudInterval == 0) UpdateHud();
	if (g.cursor > g.handover && FrameStartsNow()) {
		// The players have just been given the run's state at the handover, and a frame begins: it is theirs
		// (the snapshot of this attempt, if one was stored, is for the handover as it is now: a retry asks for it)
		if (g.save.handover == g.handover) g.save.handover = g.cursor - 1;
		g.handover = g.cursor - 1;  // (in single player the handover falls on the tick before a frame)
		CancelDueSnapshot();  // (a lead-in too short for its save state: the attempt goes without)
		for (Slot &slot : g.slots) slot.partStart = g.handover;  // (the first part's time counts from here)
		g.mode = Mode::Run;
		ShieldPlayers(true);  // the people are on their own again; bots stay protected
		HandOverObjects();
		Flash(0.8f, g.replaying ? "REPLAY" : "GO");
	}
	g.cursor++;
}

void TickWatch() {
	SyncChallengeClock();
	TakeSnapshotIfDue();
	bool atFlags = WatchFlags();
	DriveSlots();
	MoveObjects(false);
	PlacePortals();
	MoveGhosts();
	MarkWorld();
	if (g.cursor % kHudInterval == 0) UpdateHud();
	// (the run's players reaching the flags is its end too: the finish board pauses the game right after, so a last
	// tick or two of the run may never come - Ceiling Button's ride hung there, one tick short)
	constexpr int kFlagsBeforeEnd = 3;
	if (++g.cursor > g.route.timerEnd || (atFlags && g.cursor >= g.route.timerEnd - kFlagsBeforeEnd)) {
		Flash(3, "END OF THE RECORD" + std::string(kDot) + ShowAt(g.route.timerEnd));
		engine::Print("Watched the run to its end (%s).\n", ShowAt(g.route.timerEnd).c_str());
		ReleaseUseKeys();
		SetDuckKey(false);
		g.mode = Mode::Done;
		g.doneTicks = 0;
		CancelDueSnapshot();
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
		// (the speed against the run's a tick either side too, the nearest of the three: a demo now and then
		// holds a speed that is off for one tick - half of those around it in Cooperative Bridges)
		float speedOff = Distance(state.velocity, VelocityAt(g.route, static_cast<int>(a), next));
		for (int d : {-1, 1}) speedOff = std::min(speedOff, Distance(state.velocity, VelocityAt(g.route, static_cast<int>(a), next + d)));
		engine::Debug("handover: player %d is %.1f units and %.1f u/s from the run", slot.entIndex, Distance(state.origin, expected.pos), speedOff);
	}
}

// Development: "prac_debug follow <ticks>" prints, for that many ticks after each handover, every person next to the
// run's player at the same tick: position, velocity, speed. A person who presses nothing should stay on the run
// for as long as its player pressed nothing either. (g_followTicks is defined with PlacePortals, which uses it too.)
void FollowTrace() {
	int since = LiveTick() - g.handover;
	if (since < 1 || since > g_followTicks || g.replaying) return;
	for (size_t a = 0; a < g.slots.size(); a++) {
		const Slot &slot = g.slots[a];
		engine::PlayerState state;
		if (!engine::GetPlayer(slot.entIndex, state)) continue;
		if (!slot.human) {
			// (a driven player: where it is against the run's player at this tick)
			const Sample &run = Driver().At(DriverActor(a), DriverTick(LiveTick()));
			engine::Print("[follow] +%d bot off %.2f (live %.3f %.3f %.3f run %.3f %.3f %.3f) vz %.0f run vz %.0f ground %d flags %d\n", since,
			              Distance(state.origin, run.pos), state.origin.x, state.origin.y, state.origin.z, run.pos.x, run.pos.y, run.pos.z, state.velocity.z,
			              run.vel.z, run.onGround, state.flags);
			continue;
		}
		const Sample &s = g.route.At(static_cast<int>(a), LiveTick());
		Vec3 v = VelocityAt(g.route, static_cast<int>(a), LiveTick());
		engine::Print("[follow] +%d live %.4f %.4f %.3f v %.0f %.0f %.0f (%.0f) | run %.4f %.4f %.3f v %.0f %.0f %.0f (%.0f) | off %.1f keys %d flags %d\n", since,
		              state.origin.x, state.origin.y, state.origin.z, state.velocity.x, state.velocity.y, state.velocity.z, Length(state.velocity),
		              s.pos.x, s.pos.y, s.pos.z, v.x, v.y, v.z, Length(v), Distance(state.origin, s.pos), s.buttons, state.flags);
	}
}

void TickRun() {
	if (g.cursor == g.handover + 2 && !g.replaying) CheckHandover();
	SyncChallengeClock();
	FollowTrace();
	DriveSlots();
	MoveObjects(false);
	PlacePortals();
	MoveGhosts();
	MarkWorld();
	TrackPlayers();
	if (g.cursor % kHudInterval == 0) UpdateHud();
	if (WatchFlags()) {
		if (!g.replaying) {
			FinishAtFlags();
			return;
		}
		NoteReplayAtFlags();
	}
	if (AllTrackedFinished()) {
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
	UnshieldPlayers();  // (a run that was watched had everyone protected to its end)
}

void TickWaitForPlayers() {
	// After a restart: wait until everyone is back and standing in the level.
	size_t needed = g.route.coop ? 2 : 1;
	std::vector<LivePlayer> players = LivePlayers();
	size_t alive = 0;
	for (const LivePlayer &p : players) alive += p.state.alive ? 1 : 0;
	// (a partner still on the way in gets ten seconds: an attempt started without them has no place for them)
	bool partnerDue = players.size() < needed && std::chrono::steady_clock::now() - g.reloadAskedAt < std::chrono::seconds(10);
	if (alive < std::min(needed, players.size()) || players.empty() || partnerDue) {
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
	// (in co-op as soon as the level's clock allows: later, the attempt could no longer join it)
	int clock = g.route.coop ? LevelClock() : kNoClock;
	if (clock == kNoClock ? ++g.waitTicks < 90 : (++g.waitTicks < 8 || clock < FirstCommonTick(g.route))) return;
	int part = g.autoPart;
	float seconds = g.autoSeconds;
	g.autoPart = -1;
	g.autoSeconds = -1;
	g.replaying = false;
	if (seconds >= 0) Start(TickAtSeconds(seconds), g.autoWatch);
	else StartPart(part, g.autoWatch);
}

void WriteHelperScript() {
	namespace fs = std::filesystem;
	std::error_code ec;
	fs::path path = fs::u8path(engine::GameDirectory()) / helper::kRelativePath;
	fs::create_directories(path.parent_path(), ec);
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	file << helper::kSource;
	if (!file) engine::Warn("Couldn't write %s; practice won't work.\n", path.u8string().c_str());
}

// Closes the Challenge Mode boards and any other game screen, and lets the game run
void CloseGameScreens() {
	if (ChallengeMode()) engine::Execute("-leaderboard 4");
	engine::Execute("gameui_allowescape");
	engine::Execute("gameui_hide");
	engine::Execute("unpause");
}

void Init() {
	for (const Command &command : kCommands)
		if (!engine::RegisterCommand(command.name, command.callback, command.help)) engine::Warn("Couldn't register %s.\n", command.name);
	engine::RegisterCommand("prac_ghost_entity", CmdGhostEntity, "(used by P2 Practice's own script)", true);
	WriteHelperScript();
	engine::HoldClientCommands(HoldBoard);
}

void Shutdown() {
	if (g.helperLoaded) {
		g.placedBots.clear();
		SetBotMovement();  // (the bots move by themselves again)
	}
	ForgetSaveState();  // (no waiting for a save state on its way: the game gets its settings back now)
	Stop(false);
	g = Session{};
}

void OnLevelInit(const char *mapName) {
	// A new map load: nothing made in the old one exists any more
	ForgetSaveState();
	g.helperLoaded = false;
	g.levelTouched = false;
	g.startScreenClosed = false;
	g.ranSinceLoad = false;
	g.catapultMade = false;
	g.flagsUp = false;
	g.flagsGone = false;
	engine::DropHeldClientCommands();  // (for the level that is gone)
	g.boardHeldFrames = 0;
	g.dimmed = false;  // (the game clears the screen fade with the level)
	g.humanDead = false;
	g.registeredModels.clear();
	g.slots.clear();
	RestoreSpeed();
	// (a demo still to start again after a save state: SAR's own start on a level load comes first)
	if (g.demoToRestart) g.demoRestartIn = std::max(g.demoRestartIn, 120);
	if (g.hasRoute) g.worldReached = g.route.firstTick - 1;
	// The restart we asked for, or the same level coming back by itself in the middle of an attempt (the player
	// died, or the level restarted itself): the attempt starts again as soon as everyone is in.
	bool sameMap = g.hasRoute && mapName && _stricmp(mapName, g.route.map.c_str()) == 0;
	// (a level that comes back much later was loaded by the person, not restarted by the game)
	constexpr auto kRestartWithin = std::chrono::seconds(90);
	bool soon = std::chrono::steady_clock::now() - g.levelWentAt < kRestartWithin;
	if (sameMap && (g.mode == Mode::Reloading || (g.resumeAfterLoad && soon))) {
		if (g.resumeAfterLoad) engine::Print("The level restarted during the attempt: starting it again.\n");
		g.mode = Mode::WaitForPlayers;
		g.reloadAskedAt = std::chrono::steady_clock::now();
		g.waitTicks = 0;
	} else {
		// (a level the person went to themselves: practice is over, and the game is as they had it)
		g.mode = Mode::Idle;
		ReleaseGameSettings();
	}
	g.resumeAfterLoad = false;
}

void OnLevelShutdown() {
	// The level ending in the middle of an attempt with a person dead is Challenge Mode restarting it for the
	// death: the attempt waits for the level to come back. Alive, the person left for the menu (or another
	// map) themselves, and the attempt is over. (The game calls this more than once for one level change: what
	// the first call found stands.)
	bool active = g.mode == Mode::CatchUp || g.mode == Mode::LeadIn || g.mode == Mode::Run || g.mode == Mode::Watch;
	if (active && g.humanDead) {
		g.resumeAfterLoad = true;
		g.levelWentAt = std::chrono::steady_clock::now();
	}
	if (g.mode != Mode::Reloading) g.mode = Mode::Idle;
	// (a level the attempt does not come back to is the game's to split again)
	if (g.mode == Mode::Idle && !g.resumeAfterLoad) {
		g.botPartner = false;
		// (and the game has its own settings back: at the menu nothing else would give them back)
		g.save.phase = SavePhase::Idle;
		g.stopWhenRestored = false;
		ReleaseGameSettings();
	}
	g.screenHeld = false;
	KeepWholeScreen(false);
	ReleaseUseKeys();
	SetDuckKey(false);
	g.helperLoaded = false;
	g.slots.clear();
}

// Whether a person in the attempt is dead right now (read every running frame, for OnLevelShutdown, which comes
// when the level's entities may already be gone)
void NoteDeaths() {
	bool dead = false;
	for (const Slot &slot : g.slots) {
		engine::PlayerState state;
		if (slot.entIndex && slot.human && engine::GetPlayer(slot.entIndex, state) && !state.alive) dead = true;
	}
	g.humanDead = dead;
}

bool g_logArrivals = false;  // development: "prac_debug arrivals 1" prints when the players come into a level
int g_aliveLogged = -1;

void NoteArrival(const char *what) {
	if (g_logArrivals) engine::Print("[arrive] tick %d: a player %s\n", engine::TickCount(), what);
}

void LogAlivePlayers(bool simulating) {
	int alive = 0;
	for (int i = 1; i <= engine::MaxClients(); i++) {
		engine::PlayerState state;
		if (engine::GetPlayer(i, state) && state.alive) alive++;
	}
	if (alive == g_aliveLogged) return;
	g_aliveLogged = alive;
	engine::Print("[arrive] tick %d: %d alive%s\n", engine::TickCount(), alive, simulating ? "" : " (paused)");
}

// Development: frames a second and the plugin's share of each, since the last report
void PrintPerf() {
	perf::Counters &c = perf::g_counters;
	LARGE_INTEGER frequency;
	QueryPerformanceFrequency(&frequency);
	long long now = perf::Now();
	double seconds = c.since ? static_cast<double>(now - c.since) / frequency.QuadPart : 0;
	if (seconds > 0 && c.frames > 0) {
		double frameMs = seconds * 1000 / c.frames;
		engine::Print("[perf] %.1f s, %d frames: %.0f fps (%.3f ms a frame), mode %d\n", seconds, c.frames, c.frames / seconds, frameMs, static_cast<int>(g.mode));
		static const char *const names[] = {"game frame", "script", "paint"};
		for (int i = 0; i < perf::kSections; i++) {
			double ms = static_cast<double>(c.ticks[i]) * 1000 / frequency.QuadPart;
			engine::Print("[perf]   %-10s %6d calls, %.4f ms each, %.4f ms a frame (%.1f%%)\n", names[i], c.calls[i], c.calls[i] ? ms / c.calls[i] : 0,
			              ms / c.frames, ms / c.frames / frameMs * 100);
		}
	} else {
		engine::Print("[perf] measuring from now (nothing drawn since the last report)\n");
	}
	c = perf::Counters{};
	c.since = now;
}

void OnFrame(bool simulating) {
	if (g_logArrivals) LogAlivePlayers(simulating);
	SaveEvent saveEvent = TickSaveState();
	bool restored = saveEvent == SaveEvent::Restored;
	if (restored) {
		// (the restore took long enough for the short lead-in to end: the game is back in it)
		if (g.mode == Mode::Run) g.mode = Mode::LeadIn;
		// The shader API remembers what it has set on the graphics card, to skip setting it again: the restore takes
		// that memory back to the save, but not the card, which keeps what the discarded timeline left there. In
		// Turret Warehouse the room was then drawn with its projected light on and unshadowed (walls flat yellow,
		// "0 active flashlights" on the client) for seconds after a restore, once the run had gone on past the save.
		// ChocolateLoading now sets the card back itself, at the restore; one that doesn't (an older build, or the
		// option off) leaves it to us: the shader API is told to set everything on the card again.
		if (!engine::SnapshotsRestoreDeviceState()) engine::ResetRenderState();
		// The game is back at the snapshot's moment and the plugin's memory is not: what the plugin knows about
		// the level is set back to what held then. (The Challenge Mode start board, the flags, the screen fade.)
		if (g.save.startScreenOpen) g.startScreenClosed = false;
		g.pausedFrames = 0;
		g.flagsUp = ChallengeMode() && engine::EntityExists(kEndNode);
		g.flagsGone = false;
		g.dimmed = false;
		if (g.closeScreensAfterRestore) CloseGameScreens();
		g.closeScreensAfterRestore = false;
	}
	if (g_scanEdicts) ScanEdicts(engine::TickCount(), restored);
	if (g.stopWhenRestored && !g.save.Busy()) {
		Stop(false);  // (the stop that waited for the save or the restore to be over)
	} else if (saveEvent == SaveEvent::RestoreFailed) {
		// (the attempt was set up for a level that didn't come back: with the save state gone, this restarts it)
		Start(g.handover, g.watch);
	}
	if (g.save.handover >= 0 && !g.save.Busy() && engine::RecordingDemo()) g.demoSinceSnapshot = true;
	TickDemoRestart();
	ReleaseStaleBoard();
	if (g.retryAsked) {
		// (the finish board's Replay Map, or the menu's Restart Level: the board pauses the game, so this runs
		// whether or not the level is simulating)
		g.retryAsked = false;
		// (the board closes itself when its button is pressed; when it is still up for any reason, the person's
		// game sends no movement while it is, and the attempt would start without them. A save state comes back
		// a few frames later, so the closing is repeated once it has: CloseGameScreens)
		CloseGameScreens();
		CmdRetry(sdk::CCommand{});
		g.closeScreensAfterRestore = g.save.phase == SavePhase::Restoring;
		engine::FlushScript();
		return;
	}
	if (!simulating) {
		OnPausedFrame();
		return;
	}
	OnRunningFrame();
	KeepCheatsOn();
	NoteDeaths();
	DropSnapshotAtFlags();
	g.placedBots.clear();
	if (g.mode == Mode::Reloading || g.mode == Mode::Idle) {
		SetBotMovement();
		if (g.mode == Mode::Idle && g.hasRoute && g.autoPart >= 0) {
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
	SetBotMovement();
	// (every frame, in every mode: watching too)
	if (g.botPartner) KeepWholeScreen(g.screenHeld);
	engine::FlushScript();
}

// The game's own "restart the level" (the Challenge Mode finish board's Replay Map sends it, as does the pause
// menu's Restart Level): while practicing in the route's level it is a retry of the attempt instead, which keeps
// the lead-in, the save state and the bot. Out of practice the game restarts the level as usual.
bool OnRestartCommand() {
	// (savesOff: practice is on, from its first attempt until it is stopped)
	if (!g.hasRoute || !MapMatches() || g.attempts == 0 || !g.savesOff) return false;
	// The next attempt's level is already on its way (a retry that restarts the level, or the loop after a finish):
	// the game's own restart on top of it would load the level once more, and the trainer, finding a level it
	// hadn't asked for, would leave it alone (no ghost, no route). The restart is the one already coming. (Not
	// for long: a level that never gets its players back can still be restarted.)
	if (g.mode == Mode::Reloading || g.mode == Mode::WaitForPlayers)
		return std::chrono::steady_clock::now() - g.reloadAskedAt < std::chrono::seconds(30);
	g.retryAsked = true;
	return true;
}

const Settings &CurrentSettings() { return g_set; }
void UseDrawnHud(bool on) { g_drawnHud = on; }

Status CurrentStatus() {
	Status s;
	s.snapshotsReady = SnapshotsReady();
	if (!g.hasRoute) return s;
	const Route &r = g.route;
	s.loaded = true;
	s.chamber = r.name;
	s.map = r.map;
	s.files = r.files;
	s.coop = r.coop;
	s.runSeconds = r.RunTime();
	s.recordTime = ShowAt(r.timerEnd);
	for (const route::Segment &segment : r.segments) s.parts.push_back({segment.name, r.TimeAt(segment.startTick), r.TimeAt(segment.endTick)});
	s.mapLoaded = MapMatches();
	s.attempts = g.attempts;
	s.results = g.results;
	static const char *const modes[] = {"idle", "reloading", "waiting", "setting up", "lead-in", "run", "watch", "done"};
	s.mode = modes[static_cast<int>(g.mode)];
	s.active = g.mode != Mode::Idle;
	s.watching = g.watch;
	if (g.attempts) {
		s.part = g.segment;
		s.handoverSeconds = r.TimeAt(g.handover);
	}
	if (std::chrono::steady_clock::now() < g_flashUntil) s.flash = g_flashText;

	// The person at this computer: the host's player, or the first one shown text
	const Slot *person = nullptr;
	size_t index = 0;
	for (size_t a = 0; a < g.slots.size(); a++)
		if (g.slots[a].human && (g.slots[a].entIndex == 1 || !person)) {
			person = &g.slots[a];
			index = a;
		}
	const float dt = r.tickInterval;
	char text[64];
	if (g.mode == Mode::LeadIn || g.mode == Mode::CatchUp) {
		if (g.mode == Mode::CatchUp) s.note = "setting up the level";
		else {
			snprintf(text, sizeof(text), g.replaying ? "replay starts in %.1f" : "you take over in %.1f", (g.handover - g.cursor) * dt);
			s.note = text;
			s.clock = ShowAt(std::max(g.cursor, r.timerStart));
		}
	} else if (g.mode == Mode::Watch || (g.mode == Mode::Done && g.watch)) {
		s.clock = ShowAt(std::min(g.cursor, r.timerEnd));
		s.note = g.mode == Mode::Done ? "end of the record" : "watching the record";
	} else if ((g.mode == Mode::Run || g.mode == Mode::Done) && person) {
		int clock = person->finished ? person->finishCursor : LiveTick();
		s.finished = person->finished;
		s.clock = ShowAt(clock);
		s.gap = Gap(clock - r.timerStart, person->progress - r.timerStart);
		s.gapShown = true;
		s.ahead = clock < person->progress;
		engine::PlayerState state;
		if (!person->finished && engine::GetPlayer(person->entIndex, state)) {
			s.speed = g.replaying ? GroundSpeed(Driver(), static_cast<int>(index), DriverTick(g.cursor)) : Length({state.velocity.x, state.velocity.y, 0});
			s.recordSpeed = GroundSpeed(r, static_cast<int>(index), person->progress);
		}
	}
	return s;
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
