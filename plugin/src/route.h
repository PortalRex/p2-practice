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
};

// An object the run moved around: a cube, a turret, a core. Its recorded path can be replayed onto the same
// object in the live map.
struct MoverKey {
	int tick = 0;
	Vec3 pos, ang;
	bool present = true;  // false from the tick the object stopped existing (fizzled, destroyed)
};

struct Mover {
	int entIndex = 0;       // the object's entity index in the demo (map objects keep theirs from load to load)
	std::string className;  // the entity class in the map: prop_weighted_cube, npc_portal_turret_floor, ...
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

// A portal opening, moving or closing
struct PortalChange {
	int tick = 0;
	int actor = -1;      // whose portal it is (index into Route::actors), -1 when the map placed it or it's unknown
	int portalEnt = 0;   // the portal's entity index in the demo: the same portal across its changes
	bool portal2 = false;  // the second portal of its pair
	bool active = false;
	Vec3 pos, ang;
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
	std::string map;
	bool coop = false;
	std::vector<std::string> files;
	float tickInterval = 1.0f / 60;
	int firstTick = 0, lastTick = 0;    // demo ticks the samples cover
	int timerStart = 0, timerEnd = 0;   // the run: its time is (timerEnd - timerStart) ticks
	bool timerFromSar = false;          // timerStart comes from SAR's recorded time, not a guess
	std::vector<Actor> actors;          // one in single player; co-op: blue first, then orange
	std::vector<PortalChange> portals;  // in tick order
	std::vector<Mover> movers;          // objects the run moved, with their recorded paths
	std::vector<UseEvent> uses;         // in tick order
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
	int SegmentAt(int tick) const;
	int ActorForTeam(int team) const;  // -1 when the route has no such player
};

// `secondary` is the other player's demo of the same co-op run, or null.
bool Build(const p2demo::Demo &primary, const p2demo::Demo *secondary, Route &out, std::string &error);

// Reads one or two demo files and builds their route.
bool Load(const std::vector<std::string> &paths, Route &out, std::string &error);

// A few lines for the console: map, players, time, segments.
std::string Describe(const Route &route);

const char *TeamName(int team);       // "Blue", "Orange", or "Player"
std::string FormatTime(float seconds);  // 6.46, 1:02.35

}  // namespace route
