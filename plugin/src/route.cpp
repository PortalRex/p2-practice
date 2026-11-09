#include "route.h"

#include "maps.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

namespace route {
namespace {

using p2demo::Demo;

Vec3 Add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 Sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 Scale(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
Vec3 Lerp(Vec3 a, Vec3 b, float t) { return Add(a, Scale(Sub(b, a), t)); }
float Length(Vec3 a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
float Distance(Vec3 a, Vec3 b) { return Length(Sub(a, b)); }

float LerpAngle(float a, float b, float t) {
	float d = std::fmod(b - a, 360.0f);
	if (d > 180) d -= 360;
	if (d < -180) d += 360;
	return a + d * t;
}

// A player's state at one tick, as far as a demo knows it
struct Key {
	int tick = 0;
	Vec3 pos, vel;
	bool hasVel = false;
	float pitch = 0, yaw = 0;
	bool onGround = false, ducked = false;
};

// The fastest a player plausibly moves (flings top out near the engine's 3500 u/s velocity clamp)
constexpr float kMaxSpeed = 3500;
// Snapshots further apart than this mean the player wasn't being sent: don't invent a path between them
constexpr int kMaxKeyGap = 30;

// Whether the player went through a portal between two keys: further than its speed can explain.
bool IsTeleport(const Key &a, const Key &b, float dt) {
	float span = (b.tick - a.tick) * dt;
	float speed = a.hasVel && b.hasVel ? std::max(Length(a.vel), Length(b.vel)) * 1.5f + 200 : kMaxSpeed;
	return Distance(a.pos, b.pos) > 64 + speed * span;
}

std::vector<Key> KeysFromTrack(const p2demo::PlayerTrack &track, int tickOffset) {
	std::vector<Key> keys;
	keys.reserve(track.keys.size());
	for (const p2demo::PlayerKey &k : track.keys) {
		if (!k.inSnapshot) continue;
		Key key;
		key.tick = k.tick + tickOffset;
		key.pos = k.origin;
		key.vel = k.velocity;
		key.hasVel = k.hasVelocity;
		key.pitch = k.eyePitch;
		key.yaw = k.eyeYaw;
		key.onGround = (k.flags & p2demo::FL_ONGROUND) != 0;
		key.ducked = (k.flags & p2demo::FL_DUCKING) != 0;
		if (!keys.empty() && keys.back().tick >= key.tick) continue;
		keys.push_back(key);
	}
	return keys;
}

// The recording player's own position on every packet (every tick in the host's demo)
std::vector<Key> KeysFromViews(const Demo &demo, int tickOffset) {
	std::vector<Key> keys;
	keys.reserve(demo.views.size());
	for (const p2demo::View &v : demo.views) {
		if (v.tick < 0) continue;
		Key key;
		key.tick = v.tick + tickOffset;
		key.pos = v.origin;
		key.pitch = v.angles.x;
		key.yaw = v.angles.y;
		if (!keys.empty() && keys.back().tick == key.tick) keys.back() = key;
		else if (keys.empty() || keys.back().tick < key.tick) keys.push_back(key);
	}
	return keys;
}

// Fills every tick from keys: exact on a key, a curve between two close keys, a jump where a portal was crossed.
void Resample(const std::vector<Key> &keys, int firstTick, float dt, std::vector<Sample> &samples) {
	size_t next = 0;
	for (size_t i = 0; i < samples.size(); i++) {
		int tick = firstTick + static_cast<int>(i);
		while (next < keys.size() && keys[next].tick <= tick) next++;
		if (next == 0) continue;  // before the player existed
		const Key &a = keys[next - 1];
		Sample &s = samples[i];
		s.present = true;
		s.onGround = a.onGround;
		s.ducked = a.ducked;
		s.pos = a.pos;
		s.vel = a.vel;
		s.pitch = a.pitch;
		s.yaw = a.yaw;
		if (a.tick == tick || next >= keys.size()) continue;
		const Key &b = keys[next];
		int gap = b.tick - a.tick;
		if (gap > kMaxKeyGap) continue;
		float span = gap * dt;
		float t = static_cast<float>(tick - a.tick) / gap;
		s.pitch = a.pitch + (b.pitch - a.pitch) * t;
		s.yaw = LerpAngle(a.yaw, b.yaw, t);
		if (IsTeleport(a, b, dt)) {
			// Each side of the portal continues on its own velocity
			if (a.hasVel && b.hasVel) {
				if (t < 0.5f) {
					s.pos = Add(a.pos, Scale(a.vel, (tick - a.tick) * dt));
				} else {
					s.pos = Sub(b.pos, Scale(b.vel, (b.tick - tick) * dt));
					s.vel = b.vel;
				}
			}
			continue;
		}
		if (a.hasVel && b.hasVel) {
			// Cubic Hermite through both positions with both velocities
			float t2 = t * t, t3 = t2 * t;
			float h00 = 2 * t3 - 3 * t2 + 1, h10 = t3 - 2 * t2 + t, h01 = -2 * t3 + 3 * t2, h11 = t3 - t2;
			s.pos = Add(Add(Scale(a.pos, h00), Scale(a.vel, h10 * span)), Add(Scale(b.pos, h01), Scale(b.vel, h11 * span)));
			s.vel = Lerp(a.vel, b.vel, t);
		} else {
			s.pos = Lerp(a.pos, b.pos, t);
			s.vel = Scale(Sub(b.pos, a.pos), 1 / span);
		}
	}
}

// Velocities for a path known only by position: the change per tick, not taken across portal jumps.
void DifferentiateVelocity(std::vector<Sample> &samples, float dt) {
	auto jump = [&](size_t i, size_t j) { return Distance(samples[i].pos, samples[j].pos) > 64 + kMaxSpeed * dt; };
	for (size_t i = 0; i < samples.size(); i++) {
		if (!samples[i].present) continue;
		bool prev = i > 0 && samples[i - 1].present && !jump(i - 1, i);
		bool next = i + 1 < samples.size() && samples[i + 1].present && !jump(i, i + 1);
		if (prev && next) samples[i].vel = Scale(Sub(samples[i + 1].pos, samples[i - 1].pos), 1 / (2 * dt));
		else if (next) samples[i].vel = Scale(Sub(samples[i + 1].pos, samples[i].pos), 1 / dt);
		else if (prev) samples[i].vel = Scale(Sub(samples[i].pos, samples[i - 1].pos), 1 / dt);
		else samples[i].vel = {};
	}
}

// The recording player's aim and buttons on each tick, from their own user commands
void ApplyUserCmds(const Demo &demo, int tickOffset, int firstTick, std::vector<Sample> &samples) {
	int buttons = 0;
	size_t filled = 0;
	for (const p2demo::UserCmd &cmd : demo.userCmds) {
		if (cmd.slot != 0) continue;
		int index = cmd.tick + tickOffset - firstTick;
		if (index < 0 || index >= static_cast<int>(samples.size())) continue;
		for (; filled < static_cast<size_t>(index); filled++) samples[filled].buttons = buttons;
		buttons = cmd.buttons;
		samples[index].buttons = buttons;
		samples[index].pitch = cmd.viewAngles.x;
		samples[index].yaw = cmd.viewAngles.y;
		filled = index + 1;
	}
	for (; filled < samples.size(); filled++) samples[filled].buttons = buttons;
}

// Ground and duck state aren't in the view data: take them from the player's entity snapshots
void ApplyFlags(const p2demo::PlayerTrack &track, int tickOffset, int firstTick, std::vector<Sample> &samples) {
	size_t next = 0;
	const auto &keys = track.keys;
	for (size_t i = 0; i < samples.size(); i++) {
		int tick = firstTick + static_cast<int>(i) - tickOffset;
		while (next < keys.size() && keys[next].tick <= tick) next++;
		if (next == 0) continue;
		const p2demo::PlayerKey &k = keys[next - 1];
		samples[i].onGround = (k.flags & p2demo::FL_ONGROUND) != 0;
		samples[i].ducked = (k.flags & p2demo::FL_DUCKING) != 0;
		samples[i].held = k.heldEntity;
		// the networked velocity is exact where a snapshot carries it
		if (k.tick == tick && k.hasVelocity) samples[i].vel = k.velocity;
	}
}

// What each player carries, from their entity snapshots (the view data doesn't say)
void ApplyHeld(const p2demo::PlayerTrack &track, int tickOffset, int firstTick, std::vector<Sample> &samples) {
	size_t next = 0;
	const auto &keys = track.keys;
	for (size_t i = 0; i < samples.size(); i++) {
		int tick = firstTick + static_cast<int>(i) - tickOffset;
		while (next < keys.size() && keys[next].tick <= tick) next++;
		if (next > 0) samples[i].held = keys[next - 1].heldEntity;
	}
}

// The map's entity class for a networked class whose objects the trainer can put back on their recorded paths:
// things physics moves or players carry. Doors, lifts and other map machinery are not here: the map moves those
// itself when its triggers fire.
// The class an object has in the map, from the class the game networks it as (and, where several map classes
// share one, its model): the live object is looked up by its map class.
const char *MoverClass(const std::string &serverClass, const std::string &model) {
	// (a frankenturret is networked as a plain physics prop)
	if (serverClass == "CPhysicsProp" && model.find("npcs/monsters/monster_") != std::string::npos) return "prop_monster_box";
	// (Wheatley's bombs are networked as paint bombs, with the football's model)
	if (serverClass == "CPropPaintBomb" && model.find("futbol") != std::string::npos) return "prop_exploding_futbol";
	static const struct {
		const char *server, *map;
	} classes[] = {
		{"CPropWeightedCube", "prop_weighted_cube"},
		{"CPropMonsterBox", "prop_monster_box"},
		{"CNPC_Portal_FloorTurret", "npc_portal_turret_floor"},
		{"CPhysicsProp", "prop_physics"},
		{"CPhysicsPropMultiplayer", "prop_physics_multiplayer"},
		{"CPropGlassFutbol", "prop_glass_futbol"},
		{"CNPC_PersonalityCore", "npc_personality_core"},  // (as the game networks it: no underscore before Core)
		{"CPropPaintBomb", "prop_paint_bomb"},
	};
	for (const auto &c : classes)
		if (serverClass == c.server) return c.map;
	return nullptr;
}

const p2demo::PlayerTrack *FindTrack(const Demo &demo, int entIndex) {
	for (const auto &track : demo.players)
		if (track.entIndex == entIndex) return &track;
	return nullptr;
}

int TrackTeam(const p2demo::PlayerTrack &track) {
	for (const auto &k : track.keys)
		if (k.team) return k.team;
	return 0;
}

bool IsHostDemo(const Demo &demo) {
	return demo.header.server.compare(0, 9, "localhost") == 0;
}

// The smallest turn between two angles, in degrees (0..180)
float AngleApart(float a, float b) {
	float d = std::fabs(std::fmod(a - b, 360.0f));
	return d > 180 ? 360 - d : d;
}

// One moment of a player, for lining two demos up: where it looked and (optionally) where it stood
struct SyncPoint {
	int tick;
	float yaw;
	bool hasPosition;
	Vec3 position;
};

// How badly `points` (in one demo's ticks) fit the player as the other demo saw it when `offset` is added to
// their ticks: the mean distance in view direction (degrees) plus a share of the distance in position.
// Negative when too few moments can be compared.
float SyncError(const std::vector<SyncPoint> &points, const std::vector<const p2demo::PlayerKey *> &seenAt, int seenFirstTick, int offset) {
	constexpr float kPositionWeight = 0.2f;  // degrees per unit
	constexpr int kFewest = 30;
	float error = 0;
	int used = 0;
	for (const SyncPoint &point : points) {
		int index = point.tick + offset - seenFirstTick;
		if (index < 0 || index >= static_cast<int>(seenAt.size()) || !seenAt[index]) continue;
		const p2demo::PlayerKey &seen = *seenAt[index];
		error += AngleApart(point.yaw, seen.eyeYaw);
		if (point.hasPosition) error += kPositionWeight * Distance({point.position.x, point.position.y, 0}, {seen.origin.x, seen.origin.y, 0});
		used++;
	}
	return used >= kFewest ? error / used : -1;
}

// The offset in [first, last] that fits best, and how well it fits (false when nothing could be compared)
bool BestSync(const std::vector<SyncPoint> &points, const std::vector<const p2demo::PlayerKey *> &seenAt, int seenFirstTick, int first, int last,
              int &offset, float &error) {
	bool found = false;
	for (int candidate = first; candidate <= last; candidate++) {
		float candidateError = SyncError(points, seenAt, seenFirstTick, candidate);
		if (candidateError < 0 || (found && candidateError >= error)) continue;
		found = true;
		error = candidateError;
		offset = candidate;
	}
	return found;
}

// How many ticks to add to `b`'s ticks to get `a`'s (a: the host's demo, b: the partner's). Two answers:
// `stateOffset` for what b's demo shows of the game (its own player included), and `inputOffset` for what b's
// player did (keys, aim), which the host's game acts on a moment later: the time b's inputs take to arrive.
//
// Both are found by sliding b's record of its own player along a's record of that player until they agree:
// speedrunners flick their view around so much that the fit is sharp to the tick. (Portal shots both demos saw
// are not a good clock: each demo notes them when its own game shows them.)
bool TickOffset(const Demo &a, const Demo &b, int &stateOffset, int &inputOffset) {
	constexpr float kGoodFit = 25;      // degrees: anything worse is two records that don't belong together
	constexpr int kAroundGuess = 90;    // ticks searched either side of what the demos' server ticks suggest
	constexpr int kAnywhere = 1800;     // ... and when they suggest nothing
	constexpr int kLongestDelay = 60;   // inputs arrive within a second
	const p2demo::PlayerTrack *seen = FindTrack(a, b.recorderEntIndex);
	const p2demo::PlayerTrack *own = FindTrack(b, b.recorderEntIndex);
	if (seen && own && seen->keys.size() >= 8 && own->keys.size() >= 8) {
		// a's view of the player, tick by tick (a snapshot holds for the couple of ticks until the next)
		constexpr int kSnapshotHolds = 3;
		int seenFirstTick = seen->keys.front().tick;
		std::vector<const p2demo::PlayerKey *> seenAt(static_cast<size_t>(seen->keys.back().tick - seenFirstTick + 1), nullptr);
		for (const p2demo::PlayerKey &key : seen->keys) {
			if (!key.inSnapshot) continue;
			for (int hold = 0; hold < kSnapshotHolds; hold++) {
				size_t index = static_cast<size_t>(key.tick - seenFirstTick + hold);
				if (index < seenAt.size() && (hold == 0 || !seenAt[index])) seenAt[index] = &key;
			}
		}
		std::vector<SyncPoint> states, inputs;
		for (const p2demo::PlayerKey &key : own->keys)
			if (key.inSnapshot) states.push_back({key.tick, key.eyeYaw, true, key.origin});
		for (const p2demo::UserCmd &cmd : b.userCmds)
			if (cmd.slot == 0) inputs.push_back({cmd.tick, cmd.viewAngles.y, false, {}});

		bool guessed = a.serverTickAtZero != Demo::kNoServerTick && b.serverTickAtZero != Demo::kNoServerTick;
		int guess = guessed ? b.serverTickAtZero - a.serverTickAtZero : 0;
		int reach = guessed ? kAroundGuess : kAnywhere;
		float error = 0;
		if (BestSync(states, seenAt, seenFirstTick, guess - reach, guess + reach, stateOffset, error) && error <= kGoodFit) {
			inputOffset = stateOffset;
			int delayed = stateOffset;
			if (BestSync(inputs, seenAt, seenFirstTick, stateOffset, stateOffset + kLongestDelay, delayed, error) && error <= kGoodFit) inputOffset = delayed;
			return true;
		}
	}
	// No record of the player to compare: the portal shots both demos saw land
	std::map<int, int> votes;
	for (const auto &sa : a.portalShots)
		for (const auto &sb : b.portalShots)
			if (sa.result == sb.result && sa.portalNum == sb.portalNum && sa.team == sb.team && Distance(sa.origin, sb.origin) < 1)
				votes[sa.tick - sb.tick]++;
	int bestVotes = 0;
	for (const auto &[diff, count] : votes)
		if (count > bestVotes) {
			bestVotes = count;
			stateOffset = diff;
		}
	inputOffset = stateOffset;
	return bestVotes > 0;
}

std::string SurfaceName(Vec3 angles) {
	float pitch = std::fmod(angles.x + 360.0f, 360.0f);
	if (std::fabs(pitch - 90) < 30) return "ceiling";
	if (std::fabs(pitch - 270) < 30) return "floor";
	return "wall";
}

// Entity inputs that change what the map looks like to the next attempt. Sounds, lights, dialogue and the like
// don't; everything else is assumed to.
bool ChangesWorld(const p2demo::SarEntityInput &input) {
	static const char *const cosmetic[] = {
		"ambient_generic", "logic_choreographed_scene", "generic_actor", "env_projectedtexture", "info_particle_system",
		"env_sprite", "npc_security_camera", "point_servercommand", "logic_script", "vgui_screen", "env_texturetoggle",
		"env_global", "env_fade", "env_soundscape", "point_viewcontrol", "env_tonemap_controller", "light_spot", "light",
		"hot_potato_socket", "point_clientcommand", "env_shake", "ambient_music", "info_overlay_accessor"};
	for (const char *name : cosmetic)
		if (input.className == name) return false;
	if (input.input == "EnableRefire" || input.input == "CancelPending" || input.input == "SetDefaultAnimation") return false;
	return true;
}

// A demo without SAR's record of entity inputs (older SAR, or none) says nothing about the map's own state
// changing. What the recording player saw of the map's machinery moving stands in for it: buttons going down,
// doors, droppers, lifts. (Objects the trainer moves itself are left out; a turn or shift of a degree or half
// a unit is the network's rounding, not a change.)
void AddMachineryTicks(Route &route, const Demo &demo) {
	std::vector<int> ticks;
	for (const auto &track : demo.movers) {
		std::string model = track.model;
		std::transform(model.begin(), model.end(), model.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		if (MoverClass(track.className, model)) continue;
		for (size_t i = 1; i < track.keys.size(); i++) {
			const auto &a = track.keys[i - 1], &b = track.keys[i];
			if (a.parent != b.parent || a.removed || b.removed) continue;
			float turned = 0;
			for (float d : {b.angles.x - a.angles.x, b.angles.y - a.angles.y, b.angles.z - a.angles.z}) {
				d = std::fmod(std::fabs(d), 360.0f);
				turned = std::max(turned, std::min(d, 360 - d));
			}
			if (Distance(a.origin, b.origin) > 0.5f || turned > 1) ticks.push_back(b.tick);
		}
	}
	ticks.insert(ticks.end(), route.worldTicks.begin(), route.worldTicks.end());
	std::sort(ticks.begin(), ticks.end());
	ticks.erase(std::unique(ticks.begin(), ticks.end()), ticks.end());
	route.worldTicks = std::move(ticks);
}

void AddEvent(Route &route, int tick, EventKind kind, int actor, std::string label) {
	Event event;
	event.tick = tick;
	event.kind = kind;
	event.actor = actor;
	event.label = std::move(label);
	route.events.push_back(std::move(event));
}

// The first tick at which the players can do something. Maps start with a ride: down a dropper tube in co-op,
// up in a lift in single player. Both are vertical, so the ride is over at the first landing after a real fall,
// or when a player first runs sideways, whichever comes first.
int ControlTick(const Route &route) {
	// (speeds, not the on-ground flag: the game keeps that set inside the tube)
	constexpr int kFallTicks = 20;      // shorter than this is a hop, not a ride
	constexpr float kFallSpeed = 150;
	constexpr float kRunSpeed = 90;     // the tube nudges players sideways at less than this
	constexpr int kRunTicks = 6;
	int control = -1;
	auto earlier = [&control](int tick) {
		if (control < 0 || tick < control) control = tick;
	};
	for (size_t a = 0; a < route.actors.size(); a++) {
		int fallTicks = 0, runTicks = 0;
		for (int tick = std::max(route.timerStart, route.firstTick); tick <= route.timerEnd; tick++) {
			const Sample &s = route.At(static_cast<int>(a), tick);
			if (!s.present) continue;
			if (s.vel.z < -kFallSpeed) {
				fallTicks++;
				runTicks = 0;
				continue;
			}
			if (fallTicks >= kFallTicks) {
				earlier(tick);
				break;
			}
			fallTicks = 0;
			runTicks = Length({s.vel.x, s.vel.y, 0}) > kRunSpeed ? runTicks + 1 : 0;
			if (runTicks >= kRunTicks) {
				earlier(tick - kRunTicks + 1);
				break;
			}
		}
	}
	return control >= 0 ? control : std::max(route.timerStart, route.firstTick);
}

void FindEvents(Route &route, const Demo &primary) {
	const float dt = route.tickInterval;
	int control = ControlTick(route);
	// Single-player chambers begin in the arrival lift, which also travels sideways: there the ride is over when
	// the lift opens (the chambers all use the same lift, whose relay is named arrival_elevator-open)
	for (const auto &input : primary.sarEntityInputs)
		if (input.target == "arrival_elevator-open" && input.input == "Trigger" && input.tick >= route.timerStart && input.tick < route.timerEnd) {
			control = input.tick;
			break;
		}
	AddEvent(route, control, EventKind::Start, -1, "Start");

	for (const PortalChange &p : route.portals) {
		if (!p.active || p.tick <= control || p.tick >= route.timerEnd) continue;
		const char *which = p.portal2 ? "second" : "first";
		// (a portal nobody shot is the level's own: the single-portal chambers open the other one themselves)
		std::string label = p.actor >= 0 ? TeamName(route.actors[p.actor].team) + std::string(" places ") + which + " portal on the "
		                                 : std::string("The level opens a portal on the ");
		AddEvent(route, p.tick, EventKind::Portal, p.actor, label + SurfaceName(p.ang));
	}
	for (size_t a = 0; a < route.actors.size(); a++) {
		const auto &samples = route.actors[a].samples;
		for (int tick = std::max(control, route.firstTick + 1); tick < route.timerEnd; tick++) {
			const Sample &before = samples[tick - 1 - route.firstTick], &after = samples[tick - route.firstTick];
			if (!before.present || !after.present) continue;
			if (Distance(before.pos, after.pos) > 64 + kMaxSpeed * dt)
				AddEvent(route, tick, EventKind::Teleport, static_cast<int>(a), std::string(TeamName(route.actors[a].team)) + " goes through a portal");
		}
	}
	for (const auto &input : primary.sarEntityInputs) {
		if (input.tick <= control || input.tick >= route.timerEnd) continue;
		if (input.className == "prop_testchamber_door" && input.input == "Open") AddEvent(route, input.tick, EventKind::Door, -1, "Door opens");
	}
	AddEvent(route, route.timerEnd, EventKind::Finish, -1, "Finish");
	std::stable_sort(route.events.begin(), route.events.end(), [](const Event &a, const Event &b) { return a.tick < b.tick; });
}

// Parts start at events, but not closer together than a player could meaningfully practice apart.
void CutSegments(Route &route) {
	constexpr int kMinTicks = 20;
	int last = -1;
	for (const Event &event : route.events) {
		if (event.kind == EventKind::Finish) continue;
		if (last >= 0 && event.tick - last < kMinTicks) continue;
		if (route.timerEnd - event.tick < kMinTicks && !route.segments.empty()) continue;
		if (!route.segments.empty()) route.segments.back().endTick = event.tick;
		Segment segment;
		segment.startTick = event.tick;
		segment.endTick = route.timerEnd;
		segment.name = event.label;
		route.segments.push_back(std::move(segment));
		last = event.tick;
	}
}

}  // namespace

bool Mover::At(int tick, Vec3 &pos, Vec3 &ang) const {
	if (keys.empty() || tick < keys.front().tick) return false;
	// keys are changes: the last one at or before the tick holds until the next
	size_t lo = 0, hi = keys.size();
	while (hi - lo > 1) {
		size_t mid = (lo + hi) / 2;
		if (keys[mid].tick <= tick) lo = mid;
		else hi = mid;
	}
	const MoverKey &a = keys[lo];
	if (!a.present) return false;
	pos = a.pos;
	ang = a.ang;
	// Snapshots come a few ticks apart: between two close ones the object was moving, so glide between them. Not
	// across a trip through a portal (farther than anything flies in the time): halfway between the two portals is
	// somewhere in the middle of the level, where a cube put for a tick may land in a fizzler or a pit's respawn
	// trigger (Catapult Catch). It stays at the entrance until it shows at the exit.
	constexpr int kSnapshotGap = 6;
	constexpr float kMaxUnitsPerTick = 80;  // (faster than the game lets anything fly, 3500 u/s at 60 ticks)
	if (lo + 1 < keys.size() && keys[lo + 1].present && keys[lo + 1].tick - a.tick <= kSnapshotGap &&
	    Distance(a.pos, keys[lo + 1].pos) <= kMaxUnitsPerTick * (keys[lo + 1].tick - a.tick)) {
		const MoverKey &b = keys[lo + 1];
		float t = static_cast<float>(tick - a.tick) / (b.tick - a.tick);
		pos = Lerp(a.pos, b.pos, t);
		ang = {LerpAngle(a.ang.x, b.ang.x, t), LerpAngle(a.ang.y, b.ang.y, t), LerpAngle(a.ang.z, b.ang.z, t)};
	}
	return true;
}

int Mover::RestingSince(int tick) const {
	if (keys.empty() || tick < keys.front().tick) return -1;
	size_t lo = 0, hi = keys.size();
	while (hi - lo > 1) {
		size_t mid = (lo + hi) / 2;
		if (keys[mid].tick <= tick) lo = mid;
		else hi = mid;
	}
	if (!keys[lo].present) return -1;
	// A lying object still shifts a little (physics settling, a button sinking under it): it rests for as long
	// as it has stayed within a few units of where it is now
	constexpr float kStill = 6;
	size_t first = lo;
	while (first > 0 && keys[first - 1].present && Distance(keys[first - 1].pos, keys[lo].pos) < kStill) first--;
	return keys[first].tick;
}

int Route::SegmentAt(int tick) const {
	for (size_t i = segments.size(); i-- > 0;)
		if (tick >= segments[i].startTick) return static_cast<int>(i);
	return 0;
}

int Route::ActorForTeam(int team) const {
	for (size_t i = 0; i < actors.size(); i++)
		if (actors[i].team == team) return static_cast<int>(i);
	return -1;
}

const char *TeamName(int team) {
	return team == kTeamBlue ? "Blue" : team == kTeamOrange ? "Orange" : "Player";
}

int Hundredths(int ticks) {
	return ticks < 0 ? -Hundredths(-ticks) : 5 * ticks / 3;
}

int BoardTicks(int hundredths) {
	if (hundredths <= 0) return 0;
	// the hundredths below the exact time; a time that is a whole hundredth can come out one less on the board
	int ticks = (3 * hundredths + 4) / 5;  // the first tick count at or past the board time
	return 5 * ticks <= 3 * (hundredths + 1) ? ticks : 0;
}

std::string FormatHundredths(int hundredths) {
	char text[32];
	bool negative = hundredths < 0;
	if (negative) hundredths = -hundredths;
	int minutes = hundredths / 6000;
	if (minutes > 0) snprintf(text, sizeof(text), "%s%d:%02d.%02d", negative ? "-" : "", minutes, hundredths / 100 % 60, hundredths % 100);
	else snprintf(text, sizeof(text), "%s%d.%02d", negative ? "-" : "", hundredths / 100, hundredths % 100);
	return text;
}

int Route::ShownHundredths(int ticks) const {
	int record = timerEnd - timerStart;
	if (recordHundredths <= 0) return Hundredths(ticks);
	int shown = recordHundredths + Hundredths(ticks) - Hundredths(record);
	return ticks >= 0 ? std::max(0, shown) : shown;  // (the board's hundredth less never makes the start -0.01)
}

std::string Route::Show(int ticks) const { return FormatHundredths(ShownHundredths(ticks)); }

std::string FormatTime(float seconds) {
	char text[32];
	bool negative = seconds < 0;
	if (negative) seconds = -seconds;
	// hundredths, cut off rather than rounded, as the game's challenge mode clock and the boards show times
	int hundredths = static_cast<int>(seconds * 100 + 0.001f);
	int minutes = hundredths / 6000;
	if (minutes > 0) snprintf(text, sizeof(text), "%s%d:%02d.%02d", negative ? "-" : "", minutes, hundredths / 100 % 60, hundredths % 100);
	else snprintf(text, sizeof(text), "%s%d.%02d", negative ? "-" : "", hundredths / 100, hundredths % 100);
	return text;
}

bool Build(const Demo &primary, const Demo *secondary, Route &out, std::string &error, int boardHundredths) {
	Route route;
	route.map = primary.header.map;
	route.name = maps::ChamberName(route.map);
	route.tickInterval = primary.intervalPerTick > 0 ? primary.intervalPerTick : 1.0f / 60;
	route.firstTick = std::max(0, primary.firstTick);
	route.levelTickAtZero = primary.serverTickAtZero;
	route.lastTick = primary.lastTick;
	if (route.lastTick <= route.firstTick || route.Ticks() > 60 * 60 * 60 * 4) {
		error = "the demo has no playable ticks";
		return false;
	}
	const float dt = route.tickInterval;
	const size_t count = static_cast<size_t>(route.Ticks());

	int secondaryOffset = 0, secondaryInputOffset = 0;
	if (secondary) {
		if (secondary->header.map != primary.header.map) {
			error = "the two demos are of different maps (" + primary.header.map + ", " + secondary->header.map + ")";
			return false;
		}
		if (secondary->recorderEntIndex == primary.recorderEntIndex) {
			route.notes.push_back("Both demos are from the same player; the second one was ignored.");
			secondary = nullptr;
		} else if (!TickOffset(primary, *secondary, secondaryOffset, secondaryInputOffset)) {
			route.notes.push_back("Couldn't line up the second demo with the first (nothing of the partner in common); it was ignored.");
			secondary = nullptr;
		}
	}

	// Who is in the run
	struct Entry {
		int entIndex, team;
		std::string name;
	};
	std::vector<Entry> entries;
	for (const auto &track : primary.players) entries.push_back({track.entIndex, TrackTeam(track), track.name});
	if (entries.empty()) {
		// No entity data: all that's known is each recording player's own view
		bool coop = route.map.compare(0, 8, "mp_coop_") == 0;
		entries.push_back({primary.recorderEntIndex, coop ? (IsHostDemo(primary) ? kTeamBlue : kTeamOrange) : 0, primary.header.client});
		if (secondary) entries.push_back({secondary->recorderEntIndex, IsHostDemo(*secondary) ? kTeamBlue : kTeamOrange, secondary->header.client});
		route.notes.push_back("This demo's entity data couldn't be read: portals and the partner's path are missing.");
	}
	std::stable_sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) { return a.team > b.team; });
	if (entries.size() > 2) entries.resize(2);
	route.coop = entries.size() > 1 || entries[0].team != 0;
	// Challenge Mode: the players say so themselves; SAR's record of the finish flags says so too
	route.challenge = !primary.sarFlags.empty();
	for (const auto &player : primary.players) route.challenge = route.challenge || player.challenge;

	for (const Entry &entry : entries) {
		Actor actor;
		actor.team = entry.team;
		actor.entIndex = entry.entIndex;
		actor.name = entry.name;
		actor.samples.resize(count);
		const Demo *own = entry.entIndex == primary.recorderEntIndex ? &primary
		                  : secondary && entry.entIndex == secondary->recorderEntIndex ? secondary : nullptr;
		int offset = own == secondary ? secondaryOffset : 0;
		int inputOffset = own == secondary ? secondaryInputOffset : 0;
		const p2demo::PlayerTrack *seen = FindTrack(primary, entry.entIndex);

		if (own) {
			const p2demo::PlayerTrack *ownTrack = FindTrack(*own, entry.entIndex);
			// A host's demo has its own position every tick; a client's only on the packets it received, where
			// its entity snapshots (with velocity) describe the path better.
			bool dense = own->views.size() * 10 >= static_cast<size_t>(own->lastTick - own->firstTick) * 9;
			if (dense || !ownTrack) {
				Resample(KeysFromViews(*own, offset), route.firstTick, dt, actor.samples);
				DifferentiateVelocity(actor.samples, dt);
				if (ownTrack) ApplyFlags(*ownTrack, offset, route.firstTick, actor.samples);
			} else {
				Resample(KeysFromTrack(*ownTrack, offset), route.firstTick, dt, actor.samples);
			}
			ApplyUserCmds(*own, inputOffset, route.firstTick, actor.samples);
			actor.hasInputs = !own->userCmds.empty();
		} else if (seen) {
			Resample(KeysFromTrack(*seen, 0), route.firstTick, dt, actor.samples);
			route.notes.push_back(std::string(TeamName(actor.team)) + "'s path comes from 20 snapshots a second and has no inputs; add that player's own demo for both.");
		}
		if (seen) ApplyHeld(*seen, 0, route.firstTick, actor.samples);
		route.actors.push_back(std::move(actor));
	}

	// Portals: every change of every portal the demo shows
	for (const auto &track : primary.portals) {
		bool wasActive = false;
		Vec3 lastPos, lastAng;
		for (const auto &k : track.keys) {
			bool moved = k.active && (!wasActive || Distance(k.origin, lastPos) > 0.5f || Distance(k.angles, lastAng) > 0.5f);
			bool closed = !k.active && wasActive;
			if (moved || closed) {
				PortalChange change;
				change.tick = k.tick;
				change.portalEnt = track.entIndex;
				change.portal2 = track.isPortal2;
				change.active = k.active;
				change.pos = k.origin;
				change.ang = k.angles;
				// Whose portal: the game networks who fired it; a shot landing here at this moment says the same
				for (size_t a = 0; a < route.actors.size(); a++)
					if (route.actors[a].entIndex == k.firedBy) change.actor = static_cast<int>(a);
				for (const auto &shot : primary.portalShots) {
					if (change.actor >= 0 || shot.result != 0 || std::abs(shot.tick - k.tick) > 2 || Distance(shot.origin, k.origin) > 8) continue;
					for (size_t a = 0; a < route.actors.size(); a++)
						if (route.actors[a].entIndex == shot.shooter) change.actor = static_cast<int>(a);
				}
				route.portals.push_back(change);
			}
			wasActive = k.active;
			lastPos = k.origin;
			lastAng = k.angles;
		}
	}
	// A portal belongs to whoever shot it at any point of the run
	for (PortalChange &change : route.portals)
		if (change.actor < 0)
			for (const PortalChange &other : route.portals)
				if (other.portalEnt == change.portalEnt && other.actor >= 0) change.actor = other.actor;
	std::stable_sort(route.portals.begin(), route.portals.end(), [](const PortalChange &a, const PortalChange &b) { return a.tick < b.tick; });

	// The run: SAR marks its end (challenge mode flags) and knows its length. Its end is where the attempts end too
	// (the trainer watches the same flags), so the run's length is counted back from there.
	route.timerEnd = route.lastTick;
	bool endKnown = false;
	int sarTicks = 0;
	auto readSar = [&](const Demo &demo, int offset) {
		if (!endKnown && !demo.sarFlags.empty()) {
			route.timerEnd = demo.sarFlags.front().tick + offset;
			endKnown = true;
		}
		for (const auto &result : demo.sarSpeedruns)
			if (result.complete) {
				if (!endKnown) {
					route.timerEnd = result.tick + offset;
					endKnown = true;
				}
				// The two players' SAR timers can start a tick apart, and the boards' time is then the longer one
				// (every such pair in the board archive)
				sarTicks = std::max(sarTicks, result.totalTicks);
			}
	};
	readSar(primary, 0);
	if (secondary) readSar(*secondary, secondaryOffset);
	route.timerEnd = route.ClampTick(route.timerEnd);

	// Without SAR's time, the run starts at the first tick every player exists
	int spawned = route.firstTick;
	for (const Actor &actor : route.actors)
		for (size_t i = 0; i < actor.samples.size(); i++)
			if (actor.samples[i].present) {
				spawned = std::max(spawned, route.firstTick + static_cast<int>(i));
				break;
			}

	// The board's own time, where known, is the run's length: the demos' SAR timers are a tick off it in about one
	// run in twenty, and some demos have no SAR timer at all. (Unless it is far from what the demo shows: then the
	// board's entry and the demo don't belong together, and the demo is what the trainer plays.)
	// (the clock may have started a little before the demo did: single-player demos begin a few ticks into the map)
	constexpr int kMaxTicksBeforeDemo = 180;
	int length = sarTicks;
	int boardTicks = BoardTicks(boardHundredths);
	if (boardTicks > 0 && endKnown) {
		int shown = sarTicks > 0 ? sarTicks : route.timerEnd - spawned;
		int tolerance = sarTicks > 0 ? 6 : 120;
		if (std::abs(boardTicks - shown) <= tolerance && route.timerEnd - boardTicks >= route.firstTick - kMaxTicksBeforeDemo) {
			if (sarTicks > 0 && boardTicks != sarTicks)
				route.notes.push_back("The board time (" + FormatHundredths(boardHundredths) + ") is " + std::to_string(std::abs(boardTicks - sarTicks)) +
				                      (std::abs(boardTicks - sarTicks) == 1 ? " tick" : " ticks") + " off the demo's SAR timer (" +
				                      FormatHundredths(Hundredths(sarTicks)) + "): the board's is used.");
			length = boardTicks;
			route.recordHundredths = boardHundredths;
		} else {
			route.notes.push_back("The board says " + FormatHundredths(boardHundredths) + " but this demo's run is " + FormatHundredths(Hundredths(shown)) +
			                      " long: the demo's time is used.");
		}
	}
	if (length > 0 && route.timerEnd - length >= route.firstTick - kMaxTicksBeforeDemo) {
		route.timerStart = route.timerEnd - length;
		route.timerFromSar = true;
	} else {
		route.timerStart = spawned;
		route.notes.push_back("No SAR time in this demo: the clock starts when the players spawn, which may not match the board time.");
	}
	if (route.timerEnd <= route.timerStart) {
		error = "couldn't find the run inside the demo";
		return false;
	}

	for (const auto &input : primary.sarEntityInputs)
		if (ChangesWorld(input) && (route.worldTicks.empty() || route.worldTicks.back() != input.tick)) route.worldTicks.push_back(input.tick);
	if (primary.sarEntityInputs.empty()) AddMachineryTicks(route, primary);

	// What the players used
	route.hasUses = !primary.sarEntityInputs.empty();
	for (const auto &input : primary.sarEntityInputs) {
		if (input.input != "Use" || input.slot < 0) continue;
		UseEvent use;
		use.tick = input.tick;
		use.className = input.className;
		use.target = input.target;
		for (size_t a = 0; a < route.actors.size(); a++)
			if (route.actors[a].entIndex == input.slot + 1) use.actor = static_cast<int>(a);
		if (use.actor >= 0) route.uses.push_back(std::move(use));
	}

	// Named objects dissolved (once each: the record has an input per entity class that handles it). Not those a
	// dropper dissolves when it drops the next one (by FireUser1 just before): the new cube has the same name, and
	// the dropper does that itself anyway.
	const auto &inputs = primary.sarEntityInputs;
	for (size_t i = 0; i < inputs.size(); i++) {
		const auto &input = inputs[i];
		if (input.input != "Dissolve" || input.target.empty() || input.target.find('&') != std::string::npos) continue;
		if (input.className != "prop_weighted_cube" && input.className != "prop_monster_box" && input.className != "prop_physics") continue;
		if (!route.dissolves.empty() && route.dissolves.back().target == input.target && route.dissolves.back().tick == input.tick) continue;
		bool byDropper = false;
		for (size_t j = i; j-- > 0 && inputs[j].tick >= input.tick - 2;)
			if (inputs[j].target == input.target && inputs[j].input == "FireUser1") byDropper = true;
		if (!byDropper) route.dissolves.push_back({input.tick, input.target});
	}

	// Floor buttons pressed and released within one tick by a player: the io proxy of the button's instance fires
	// its first relay (pressed) and its second (released) on the same tick
	for (size_t i = 0; i < primary.sarEntityInputs.size(); i++) {
		const auto &a = primary.sarEntityInputs[i];
		const std::string suffix = "-proxy";
		if (a.slot < 0 || a.className != "func_instance_io_proxy" || a.input != "OnProxyRelay1" || a.target.size() <= suffix.size() ||
		    a.target.compare(a.target.size() - suffix.size(), suffix.size(), suffix) != 0)
			continue;
		// (both relays in one tick, in either order: which of the two is the press depends on how the instance is
		// wired - Future Starter's second button fires its second relay when pressed)
		bool released = false;
		for (size_t j = i + 1; j < primary.sarEntityInputs.size() && primary.sarEntityInputs[j].tick == a.tick; j++) {
			const auto &b = primary.sarEntityInputs[j];
			if (b.target == a.target && b.input == "OnProxyRelay2" && b.slot == a.slot) released = true;
		}
		for (size_t j = i; j-- > 0 && primary.sarEntityInputs[j].tick == a.tick;) {
			const auto &b = primary.sarEntityInputs[j];
			if (b.target == a.target && b.input == "OnProxyRelay2" && b.slot == a.slot) released = true;
		}
		// (the released-then-pressed kind: this press follows the same player's release by a tick or two, with no
		// press of that button in between)
		constexpr int kReleasedThenPressed = 4;
		bool endsPressed = false;
		for (size_t j = i; j-- > 0 && !released && primary.sarEntityInputs[j].tick >= a.tick - kReleasedThenPressed;) {
			const auto &b = primary.sarEntityInputs[j];
			if (b.target != a.target) continue;
			if (b.input == "OnProxyRelay2" && b.slot == a.slot && b.tick < a.tick) endsPressed = true;
			break;
		}
		// (and the button stays pressed: a player hopping on it lets it go again soon after)
		constexpr int kStaysPressed = 60;
		for (size_t j = i + 1; endsPressed && j < primary.sarEntityInputs.size() && primary.sarEntityInputs[j].tick <= a.tick + kStaysPressed; j++)
			if (primary.sarEntityInputs[j].target == a.target && primary.sarEntityInputs[j].input == "OnProxyRelay2") endsPressed = false;
		if (!released && !endsPressed) continue;
		ButtonGlitch glitch;
		glitch.endsPressed = endsPressed;
		glitch.tick = a.tick;
		glitch.button = a.target.substr(0, a.target.size() - suffix.size()) + "-button";
		for (size_t k = 0; k < route.actors.size(); k++)
			if (route.actors[k].entIndex == a.slot + 1) glitch.actor = static_cast<int>(k);
		if (glitch.actor >= 0) route.buttonGlitches.push_back(std::move(glitch));
	}

	// Objects the run moved. An object attached to something (parent) has an origin relative to it, which says
	// nothing here: those stretches are left out and the object stays where it last was.
	for (const auto &track : primary.movers) {
		std::string model = track.model;
		std::transform(model.begin(), model.end(), model.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		const char *mapClass = MoverClass(track.className, model);
		if (!mapClass) continue;
		Mover mover;
		mover.entIndex = track.entIndex;
		mover.className = mapClass;
		mover.aimsLasers = model.find("reflection_cube") != std::string::npos;
		for (const auto &k : track.keys) {
			if (k.parent >= 0 && !k.removed) continue;
			MoverKey key;
			key.tick = k.tick;
			key.pos = k.origin;
			key.ang = k.angles;
			// (out of the recording player's view the demo says nothing about the object: from there until it is
			// seen again it counts as not there, and the trainer keeps its hands off the live one)
			key.present = !k.removed && k.inSnapshot;
			if (!mover.keys.empty() && mover.keys.back().tick >= key.tick) continue;
			mover.keys.push_back(key);
		}
		if (mover.keys.size() >= 2) route.movers.push_back(std::move(mover));
	}

	FindEvents(route, primary);
	CutSegments(route);
	if (route.segments.empty()) {
		error = "couldn't cut the run into parts";
		return false;
	}
	out = std::move(route);
	return true;
}

int BoardTimeFromFileName(const std::string &path, const std::string &map) {
	std::string name = path.substr(path.find_last_of("/\\") + 1);
	if (name.size() < 4 || _stricmp(name.c_str() + name.size() - 4, ".dem") != 0) return 0;
	name.resize(name.size() - 4);
	// <Chamber>_<hundredths>_<steam id>_<id>, the chamber's name without its spaces
	size_t idAt = name.rfind('_');
	size_t steamAt = idAt == std::string::npos || idAt == 0 ? std::string::npos : name.rfind('_', idAt - 1);
	size_t timeAt = steamAt == std::string::npos || steamAt == 0 ? std::string::npos : name.rfind('_', steamAt - 1);
	if (timeAt == std::string::npos) return 0;
	auto digits = [&](size_t from, size_t to) {
		if (to <= from || to - from > 18) return false;
		for (size_t i = from; i < to; i++)
			if (name[i] < '0' || name[i] > '9') return false;
		return true;
	};
	if (!digits(timeAt + 1, steamAt) || !digits(steamAt + 1, idAt) || !digits(idAt + 1, name.size()) || steamAt - timeAt - 1 > 7) return 0;
	std::string chamber = maps::ChamberName(map);
	chamber.erase(std::remove(chamber.begin(), chamber.end(), ' '), chamber.end());
	if (_stricmp(chamber.c_str(), name.substr(0, timeAt).c_str()) != 0) return 0;
	return std::stoi(name.substr(timeAt + 1, steamAt - timeAt - 1));
}

bool Load(const std::vector<std::string> &paths, Route &out, std::string &error) {
	if (paths.empty() || paths.size() > 2) {
		error = "give one demo, or both players' demos of a co-op run";
		return false;
	}
	std::vector<Demo> demos(paths.size());
	for (size_t i = 0; i < paths.size(); i++) {
		std::string problem;
		if (!p2demo::ParseFile(paths[i].c_str(), demos[i], problem)) {
			error = paths[i] + ": " + problem;
			return false;
		}
	}
	// The host's demo knows the most (its own path every tick, SAR's entity inputs): build on it
	size_t primary = demos.size() == 2 && !IsHostDemo(demos[0]) && IsHostDemo(demos[1]) ? 1 : 0;
	const Demo *secondary = demos.size() == 2 ? &demos[1 - primary] : nullptr;
	// A demo downloaded from the boards carries the run's board time in its name (both of a co-op pair the same)
	int boardHundredths = 0;
	for (size_t i = 0; i < demos.size(); i++) {
		int time = BoardTimeFromFileName(paths[i], demos[i].header.map);
		if (i == 0) boardHundredths = time;
		else if (time != boardHundredths) boardHundredths = 0;
	}
	if (!Build(demos[primary], secondary, out, error, boardHundredths)) return false;
	out.files = paths;
	for (const Demo &demo : demos)
		for (const std::string &warning : demo.warnings) out.notes.push_back(warning);
	return true;
}

std::string Describe(const Route &route) {
	std::string text;
	char line[256];
	snprintf(line, sizeof(line), "%s  %s  (%s%s, %d demo%s)\n", route.name.c_str(), route.Show(route.timerEnd - route.timerStart).c_str(),
	         route.coop ? "co-op" : "single player", route.challenge ? ", Challenge Mode" : "", static_cast<int>(route.files.size()),
	         route.files.size() == 1 ? "" : "s");
	text += line;
	for (const Actor &actor : route.actors) {
		snprintf(line, sizeof(line), "  %-6s %s%s\n", TeamName(actor.team), actor.name.c_str(), actor.hasInputs ? "" : "  (no inputs)");
		text += line;
	}
	for (size_t i = 0; i < route.segments.size(); i++) {
		const Segment &s = route.segments[i];
		snprintf(line, sizeof(line), "  part %d  %6s - %-6s  %s\n", static_cast<int>(i) + 1, route.Show(s.startTick - route.timerStart).c_str(),
		         route.Show(s.endTick - route.timerStart).c_str(), s.name.c_str());
		text += line;
	}
	for (const std::string &note : route.notes) text += "  note: " + note + "\n";
	return text;
}

}  // namespace route
