// A route: what a recorded run did, tick by tick, in a form the trainer can replay, race and cut into parts.
//
// Built from one demo, or from both players' demos of a co-op run. Engine-independent (only the demo reader), so
// the command-line tools and the launcher share it with the plugin.
#pragma once

#include <string>
#include <vector>

#include "p2demo/demo.h"

namespace route {

using p2demo::Vec3;

constexpr int kTeamOrange = 2;  // P-body
constexpr int kTeamBlue = 3;    // ATLAS

struct Sample {
	Vec3 pos;  // feet
	Vec3 vel;
	float pitch = 0, yaw = 0;
	int buttons = -1;  // IN_* bits (p2demo::Button); -1 when this player's inputs aren't known
	bool present = false;
	bool onGround = false;
	bool ducked = false;
	int held = -1;  // demo entity index of the object the player carries, -1 = none
};

struct Actor {
	int team = 0;      // kTeamBlue / kTeamOrange; 0 in single player
	int entIndex = 0;  // the player's entity index in the demo
	std::string name;
	bool hasInputs = false;  // buttons and per-tick aim come from the player's own demo
	std::vector<Sample> samples;  // index = tick - Route::firstTick
	// Co-op, the host's partner, when both demos are loaded: where the host's game had this player at each tick
	// (same index as `samples`; empty for everyone else). The samples are the partner's own smooth path, as its
	// own game saw it. The host's game, the one that decided when the flags were touched, moved the partner only
	// when its commands arrived, a few ticks' worth at a time, so it had the partner a little behind or ahead of
	// that path at any one tick. `seenFrom` is the tick from which the player stays near where it finished: from
	// there a player driven along this path is put where the host's game had it, and touches the flags on the
	// very tick the run's player did.
	std::vector<Vec3> seenPos;
	int seenFrom = 0;
	bool SeenAt(int tick, int firstTick, Vec3 &pos) const {
		size_t i = static_cast<size_t>(tick - firstTick);
		if (seenPos.empty() || tick < seenFrom || tick < firstTick || i >= seenPos.size()) return false;
		pos = seenPos[i];
		return true;
	}
};

// An object the run moved around: a cube, a turret, a core. Its recorded path can be replayed onto the same
// object in the live map.
struct MoverKey {
	int tick = 0;
	Vec3 pos, ang;
	bool present = true;  // false from the tick the object stopped existing (fizzled, destroyed) or left the demo's view
};

struct Mover {
	int entIndex = 0;       // the object's entity index in the demo (map objects keep theirs from load to load)
	std::string className;  // the entity class in the map: prop_weighted_cube, npc_portal_turret_floor, ...
	bool aimsLasers = false;  // a reflection cube: a laser leaves it the way it faces, so how it lies matters to the degree
	std::vector<MoverKey> keys;  // one per change, in tick order

	// Where the object is at `tick`; false before it first appears or once it is gone
	bool At(int tick, Vec3 &pos, Vec3 &ang) const;
	// The tick since which the object has been lying still at `tick`, or -1 while it moves or doesn't exist
	int RestingSince(int tick) const;
};

// A player using something: pressing a button, picking up a cube. From SAR's entity-input records.
struct UseEvent {
	int tick = 0;
	int actor = -1;
	std::string className;  // prop_button, prop_weighted_cube, ...
	std::string target;     // the thing's name in the map, "" when it has none
};

// A floor button a player pressed and released within one tick (the "button glitch"): its pressed output fired
// and then its released one, and a map whose released output closes something with a delay that the pressed one
// cancels keeps the thing open for good. Seen in SAR's record of the instance's io proxy firing both relays at once.
//
// The other way round too: released and then pressed again a tick or two later by a player already in the air
// leaving it (Team Retrieval's first button, bounced off at 9.3 s): the button stays pressed with nobody on it.
// A button placed in the map itself has no io proxy, and often no name (Gel Maze's, which holds the exit's
// fizzler open): there the record shows what its two outputs set off, and the button is found by its place.
struct ButtonGlitch {
	int tick = 0;
	int actor = -1;
	std::string button;  // the floor button's name in the map ("<instance>-button"); "" for a button without one
	Vec3 at;             // where the button is, which says which one when it has no name
	bool endsPressed = false;  // the released-then-pressed kind
};

// A portal opening, moving or closing
struct PortalChange {
	int tick = 0;
	int actor = -1;      // whose portal it is (index into Route::actors), -1 when the map placed it or it's unknown
	int portalEnt = 0;   // the portal's entity index in the demo: the same portal across its changes
	bool portal2 = false;  // the second portal of its pair
	bool active = false;
	Vec3 pos, ang;
	// A portal on something that moves (a panel on its way): where it was tick by tick, since `pos` only holds
	// for the moment it was placed. Empty for a portal that stays put.
	std::vector<std::pair<int, Vec3>> path;

	// Where the portal was at tick `at`
	Vec3 PosAt(int at) const;
};

enum class EventKind { Start, Portal, Teleport, Door, Finish };

struct Event {
	int tick = 0;
	EventKind kind = EventKind::Start;
	int actor = -1;
	std::string label;
};

struct Segment {
	int startTick = 0, endTick = 0;
	std::string name;
};

struct Route {
	std::string map;   // the map's file name: sp_a2_laser_stairs
	std::string name;  // the chamber's name, for showing: Laser Stairs
	bool coop = false;
	bool challenge = false;  // made in Challenge Mode, where levels are set up differently in places
	std::vector<std::string> files;
	float tickInterval = 1.0f / 60;
	int firstTick = 0, lastTick = 0;    // demo ticks the samples cover
	int timerStart = 0, timerEnd = 0;   // the run: its time is (timerEnd - timerStart) ticks
	// The server's tick count, counted from the map's load, at demo tick 0 (p2demo::Demo::kNoServerTick when the
	// demo doesn't say). A level's lifts, doors and timers run on that count.
	int levelTickAtZero = p2demo::Demo::kNoServerTick;
	bool timerFromSar = false;          // timerStart comes from SAR's recorded time (or the board's), not a guess
	int recordHundredths = 0;           // the run's time as its board shows it, when known (0: not known)
	std::vector<Actor> actors;          // one in single player; co-op: blue first, then orange
	std::vector<PortalChange> portals;  // in tick order
	std::vector<Mover> movers;          // objects the run moved, with their recorded paths
	std::vector<UseEvent> uses;         // in tick order
	std::vector<ButtonGlitch> buttonGlitches;  // in tick order
	// Objects the run's fizzlers dissolved, by name (a carried cube put along its path by the trainer passes a
	// fizzler without the fizzler noticing it: Smooth Jazz's box, whose fizzling opens the door)
	struct Dissolve {
		int tick = 0;
		std::string target;
	};
	std::vector<Dissolve> dissolves;  // in tick order
	bool hasUses = false;               // the demo records uses (SAR, host's demo); otherwise they are guessed from the keys
	std::vector<Event> events;          // in tick order
	std::vector<Segment> segments;      // consecutive, from the first moment of control to the finish
	std::vector<int> worldTicks;        // ticks at which the map's own state changed (doors, panels, relays)
	std::vector<std::string> notes;     // things the user should know about this route's data

	int Ticks() const { return lastTick - firstTick + 1; }
	int ClampTick(int tick) const { return tick < firstTick ? firstTick : tick > lastTick ? lastTick : tick; }
	const Sample &At(int actor, int tick) const { return actors[actor].samples[ClampTick(tick) - firstTick]; }
	float RunTime() const { return (timerEnd - timerStart) * tickInterval; }
	float TimeAt(int tick) const { return (tick - timerStart) * tickInterval; }
	// A time of the run's clock (`ticks` from its start) as the trainer shows it: in hundredths, and anchored to the
	// board's own time of the run where that is known, so that a time level with the record reads exactly as the
	// record does and two times shown differ by the difference of what is shown (see BoardTicks)
	int ShownHundredths(int ticks) const;
	std::string Show(int ticks) const;
	int SegmentAt(int tick) const;
	int ActorForTeam(int team) const;  // -1 when the route has no such player
};

// `secondary` is the other player's demo of the same co-op run, or null. `boardHundredths` is the run's time on the
// boards when known (0 if not): the run is then taken to be exactly that long, ending where the demo shows it end.
bool Build(const p2demo::Demo &primary, const p2demo::Demo *secondary, Route &out, std::string &error, int boardHundredths = 0);

// Reads one or two demo files and builds their route.
bool Load(const std::vector<std::string> &paths, Route &out, std::string &error);

// A few lines for the console: map, players, time, segments.
std::string Describe(const Route &route);

// What kind of run a demo holds, for the lists of runs: made in Challenge Mode or not, and whether it got to the
// finish flags. Only SAR records the flags being touched, so a demo without SAR's data doesn't say (finishKnown).
struct DemoKind {
	bool challenge = false;
	bool finishKnown = false;
	bool finished = false;
};
DemoKind KindOf(const p2demo::Demo &demo);
// Reads a demo for a list of runs, as quickly as that allows: its header and SAR's data and, only where those
// don't say which mode it was made in, the players up to where they show it. (No paths, portals or objects.)
bool ReadForList(const char *path, p2demo::Demo &demo, std::string &error);

const char *TeamName(int team);       // "Blue", "Orange", or "Player"
std::string FormatTime(float seconds);  // 6.46, 1:02.35
std::string FormatHundredths(int hundredths);
int Hundredths(int ticks);  // the hundredths below the exact time of `ticks` (60 a second)

// The tick count a board time stands for (0 when none gives it). The boards show the hundredths below the exact
// time, except that a time that is exactly a whole hundredth sometimes shows one less (the game's float clock
// comes out a hair under): 633 ticks, 10.55 s, are 10.54 on one run's board entry and 537 ticks 8.95 on another's.
// So a board time stands for exactly one tick count, but a tick count doesn't always give the same board time.
int BoardTicks(int hundredths);
// The board time a run of `name` (the boards' file name: <Chamber>_<hundredths>_<steam id>_<id>.dem) has, if it
// is one of their demos of `map`; 0 otherwise
int BoardTimeFromFileName(const std::string &path, const std::string &map);

}  // namespace route
