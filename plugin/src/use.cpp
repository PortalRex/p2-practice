#include "use.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "engine.h"
#include "trainer.h"

namespace use {
namespace {

using p2demo::Vec3;

constexpr float kReach = 100;     // units: how far the game's use key reaches
constexpr float kNear = 150;      // units beyond that within which a thing counts as what the press was for
constexpr float kStep = 4;        // units between the places a press is tried from, before the exact one is closed in on
constexpr int kTurnSteps = 16;    // ... and the turns towards a thing it is tried with
constexpr int kCloseIn = 6;       // halvings: to a sixteenth of a unit, or of a turn step
constexpr int kWatchTicks = 60;   // how long before and after a press a hit counts as "late" or "early"
constexpr int kHistory = 128;     // ticks of "what would the key find" kept (more than kWatchTicks)
constexpr float kDegrees = 57.29578f;

// What to call the things a level has to use. (Anything else the game's search finds goes by its class name.)
struct Kind {
	const char *className;
	bool prefix;
	const char *word;
};
constexpr Kind kKinds[] = {
    {"prop_weighted_cube", false, "cube"},      {"prop_monster_box", false, "box"},        {"npc_portal_turret_floor", false, "turret"},
    {"npc_personality_core", false, "core"},    {"prop_glass_futbol", false, "ball"},      {"prop_exploding_futbol", false, "ball"},
    {"prop_paint_bomb", false, "paint bomb"},   {"prop_physics", true, "object"},          {"prop_button", false, "button"},
    {"prop_under_button", false, "button"},     {"func_button", false, "button"},          {"func_rot_button", false, "button"},
    {"momentary_rot_button", false, "button"},
};

const char *WordFor(const char *className) {
	for (const Kind &kind : kKinds)
		if (kind.prefix ? !strncmp(className, kind.className, strlen(kind.className)) : !strcmp(className, kind.className)) return kind.word;
	return nullptr;
}

// What to call the entity the game's search found
std::string NameOf(int entity) {
	engine::EntityBox box;
	if (entity == engine::kUseTargetUnnamed || !engine::GetEntityBox(entity, box)) return "something";
	const char *word = WordFor(box.className);
	return word ? word : box.className;
}

struct State {
	bool hadPlayer = false;
	bool wasPressed = false, wasHolding = false;
	float lastStart = 0;
	int targets[kHistory] = {};  // what the game's search found, by tick
	int targetTicks[kHistory] = {};
	bool pending = false;        // the last miss still waits for what the ticks after it bring
	Miss miss;
} g;

float Dot(const Vec3 &a, const Vec3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 Sub(const Vec3 &a, const Vec3 &b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 Scaled(const Vec3 &v, float by) { return {v.x * by, v.y * by, v.z * by}; }
float Length(const Vec3 &v) { return std::sqrt(Dot(v, v)); }
float Component(const Vec3 &v, int axis) { return axis == 0 ? v.x : axis == 1 ? v.y : v.z; }

Vec3 Direction(float pitch, float yaw) {
	float cp = std::cos(pitch / kDegrees);
	return {cp * std::cos(yaw / kDegrees), cp * std::sin(yaw / kDegrees), -std::sin(pitch / kDegrees)};
}

// An entity's own axes in the world (forward, left, up), from its angles
void Axes(float pitch, float yaw, float roll, Vec3 axes[3]) {
	float sp = std::sin(pitch / kDegrees), cp = std::cos(pitch / kDegrees);
	float sy = std::sin(yaw / kDegrees), cy = std::cos(yaw / kDegrees);
	float sr = std::sin(roll / kDegrees), cr = std::cos(roll / kDegrees);
	axes[0] = {cp * cy, cp * sy, -sp};
	axes[1] = {sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, sr * cp};
	axes[2] = {cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp};
}

// What the use key would find from `forward` units further along `aim`, looking along `aim`: the game's own
// search, run with the player put there (0 nothing, below 0 when it can't be asked)
int Probe(const Vec3 &aim, float forward) {
	engine::UseView view;
	view.moved = Scaled(aim, forward);
	view.pitch = -std::asin(std::clamp(aim.z, -1.0f, 1.0f)) * kDegrees;
	view.yaw = std::atan2(aim.y, aim.x) * kDegrees;
	return engine::FindUseTarget(1, &view);
}

// How many units further along `aim` the key first finds something (and what, into `entity`); -1 when nowhere
// within kNear
float ReachNeeded(const Vec3 &aim, int &entity) {
	for (float tried = 0; tried <= kNear; tried += kStep) {
		int found = Probe(aim, tried);
		if (found <= 0) continue;
		entity = found;
		if (tried == 0) return 0;
		float miss = tried - kStep, hit = tried;
		for (int i = 0; i < kCloseIn; i++) {
			float middle = (miss + hit) / 2;
			int there = Probe(aim, middle);
			if (there > 0) {
				hit = middle;
				entity = there;
			} else {
				miss = middle;
			}
		}
		return hit;
	}
	return -1;
}

// The direction `share` of the way round from `from` to `to` (two unit vectors `angle` radians apart)
Vec3 Turned(const Vec3 &from, const Vec3 &to, float angle, float share) {
	float sine = std::sin(angle);
	if (sine < 1e-4f) return to;
	float a = std::sin((1 - share) * angle) / sine, b = std::sin(share * angle) / sine;
	return {from.x * a + to.x * b, from.y * a + to.y * b, from.z * a + to.z * b};
}

// How many degrees the aim has to turn towards `toward` for the key to find something from where the player
// stands; -1 when not even looking straight along `toward` does
float TurnNeeded(const Vec3 &aim, const Vec3 &toward, float angle, int &entity) {
	for (int step = 1; step <= kTurnSteps; step++) {
		int found = Probe(Turned(aim, toward, angle, static_cast<float>(step) / kTurnSteps), 0);
		if (found <= 0) continue;
		entity = found;
		float miss = static_cast<float>(step - 1) / kTurnSteps, hit = static_cast<float>(step) / kTurnSteps;
		for (int i = 0; i < kCloseIn; i++) {
			float middle = (miss + hit) / 2;
			int there = Probe(Turned(aim, toward, angle, middle), 0);
			if (there > 0) {
				hit = middle;
				entity = there;
			} else {
				miss = middle;
			}
		}
		return hit * angle * kDegrees;
	}
	return -1;
}

// The thing of a usable kind the press most likely was for: the one nearest to the aim, a thing further away
// than the key reaches counting for less. False when there is none within kNear of the key's reach.
struct Thing {
	const char *word = nullptr;
	Vec3 middle;         // of its box, in the world
	float distance = 0;  // from the eye to the nearest point of its box
};
bool NearestThing(const engine::PlayerState &player, const Vec3 &eye, const Vec3 &aim, Thing &out) {
	float bestScore = 0;
	for (int index = engine::MaxClients() + 1; index < engine::EntityLimit(); index++) {
		engine::EntityBox box;
		if (index == player.held || !engine::GetEntityBox(index, box)) continue;
		const char *word = WordFor(box.className);
		if (!word) continue;
		Vec3 axes[3];
		Axes(box.pitch, box.yaw, box.roll, axes);
		Vec3 offset = Sub(eye, box.origin), middle = box.origin;
		float distanceSquared = 0;
		for (int axis = 0; axis < 3; axis++) {
			float low = Component(box.mins, axis), high = Component(box.maxs, axis), along = Dot(offset, axes[axis]);
			float outside = along - std::clamp(along, low, high);
			distanceSquared += outside * outside;
			float centre = (low + high) / 2;
			middle = {middle.x + axes[axis].x * centre, middle.y + axes[axis].y * centre, middle.z + axes[axis].z * centre};
		}
		float distance = std::sqrt(distanceSquared);
		if (distance > kReach + kNear) continue;
		Vec3 to = Sub(middle, eye);
		float length = Length(to);
		float angle = length > 0.01f ? std::acos(std::clamp(Dot(to, aim) / length, -1.0f, 1.0f)) * kDegrees : 0;
		float score = angle + std::max(0.0f, distance - kReach) / 2;
		if (out.word && score >= bestScore) continue;
		bestScore = score;
		out.word = word;
		out.middle = middle;
		out.distance = distance;
	}
	return out.word != nullptr;
}

// How far off a press that used nothing was, into `miss`: how much closer along the aim it would have used
// something; else how far the aim had to turn towards the nearest usable thing; else both
void Measure(const engine::PlayerState &player, Miss &miss) {
	Vec3 eye = {player.origin.x, player.origin.y, player.origin.z + player.viewOffsetZ};
	Vec3 aim = Direction(player.pitch, player.yaw);
	int entity = 0;
	float reach = ReachNeeded(aim, entity);
	if (reach >= 0) {
		miss.thing = NameOf(entity);
		miss.reachShort = reach;
		return;
	}
	Thing thing;
	if (!NearestThing(player, eye, aim, thing)) return;
	Vec3 toward = Sub(thing.middle, eye);
	float length = Length(toward);
	if (length < 0.01f) return;
	toward = Scaled(toward, 1 / length);
	float angle = std::acos(std::clamp(Dot(aim, toward), -1.0f, 1.0f));
	float turn = TurnNeeded(aim, toward, angle, entity);
	if (turn >= 0) {
		miss.thing = NameOf(entity);
		miss.aimOff = turn;
		return;
	}
	reach = ReachNeeded(toward, entity);
	if (reach >= 0) {
		miss.thing = NameOf(entity);
		miss.aimOff = angle * kDegrees;
		miss.reachShort = reach;
	} else if (thing.distance < kReach) {
		// (within reach, and the game's search doesn't find it from anywhere near)
		miss.thing = thing.word;
		miss.blocked = true;
	}
}

void Remember(int tick, int target) {
	g.targets[tick % kHistory] = target;
	g.targetTicks[tick % kHistory] = tick;
}

// What the game's search found on `tick` (0 when that tick wasn't looked at)
int TargetAt(int tick) { return tick >= 0 && g.targetTicks[tick % kHistory] == tick ? g.targets[tick % kHistory] : 0; }

// A press that used nothing
void NoteMiss(int tick, const engine::PlayerState &player, const engine::UseKey &key, int targetNow) {
	g.miss = Miss{};
	g.miss.tick = tick;
	g.miss.locked = std::max(0.0f, key.lockedUntil - key.now);
	Measure(player, g.miss);
	for (int back = 1; back <= kWatchTicks && !g.miss.ticksLate; back++)
		if (TargetAt(tick - back) > 0) g.miss.ticksLate = back;
	// (the game looked during the tick, this looks after it: something that came into reach in between is a tick away)
	if (targetNow > 0 && g.miss.locked == 0) g.miss.ticksEarly = 1;
	g.pending = true;
	g.miss.shown = !g.miss.thing.empty() || g.miss.ticksLate || g.miss.ticksEarly;
}

}  // namespace

void Reset() { g = State{}; }

const Miss &Last() { return g.miss; }

void Tick() {
	if (!trainer::CurrentSettings().use) {
		if (g.hadPlayer) Reset();
		return;
	}
	engine::PlayerState player;
	engine::UseKey key;
	if (!engine::GetPlayer(1, player) || !player.alive || !engine::GetUseKey(1, key)) {
		Reset();
		return;
	}
	int tick = engine::TickCount();
	bool holding = player.held >= 0;
	// (with something in the hands the key lets go of it: there is nothing to find)
	int target = holding ? 0 : engine::FindUseTarget(1);
	Remember(tick, target);
	if (!g.hadPlayer) {
		g.hadPlayer = true;
		g.wasPressed = key.pressed;
		g.wasHolding = holding;
		g.lastStart = key.startTime;
		return;
	}

	// A press that used nothing starts the press's clock: the game sets it to the time and goes on looking. (With
	// a partner in the level the clock also starts by itself at speed, so the key has to have gone down as well.)
	// While the key is off the clock stays as it is, and the key going down is all there is to see.
	float sinceStart = key.now - key.startTime;
	bool started = key.startTime != g.lastStart && key.startTime > 0 && sinceStart >= 0 && sinceStart < 3 * engine::TickInterval();
	bool wentDown = key.pressed && !g.wasPressed;
	bool missed = started && (key.pressed || engine::MaxClients() == 1);
	bool pressedWhileOff = wentDown && !started && key.now < key.lockedUntil;
	if ((missed || pressedWhileOff) && !g.wasHolding && !holding) {
		NoteMiss(tick, player, key, target);
	} else if (wentDown) {
		g.pending = false;  // (a press that used something, or let go of what was held)
	} else if (g.pending) {
		// The miss before: what the ticks after it bring. For a while after a press the game picks up an object
		// that comes into reach by itself, a tick after the search first finds it: so a press that came early is
		// watched on, and counts as caught when the object is in the hands after all.
		int age = tick - g.miss.tick;
		if (holding || key.startTime < 0) {
			g.miss.caughtAfter = g.miss.ticksEarly ? g.miss.ticksEarly : std::max(1, age);
			g.miss.ticksEarly = 0;
			g.miss.shown = true;
			g.pending = false;
		} else if (target > 0 && !g.miss.ticksEarly) {
			g.miss.ticksEarly = age;
			g.miss.shown = true;
		} else if (age >= kWatchTicks) {
			g.pending = false;
		}
	}
	g.wasPressed = key.pressed;
	g.wasHolding = holding;
	g.lastStart = key.startTime;
}

void PrintState() {
	engine::PlayerState player;
	engine::UseKey key;
	if (!engine::GetPlayer(1, player) || !engine::GetUseKey(1, key)) {
		engine::Print("[use] no player, or this game version doesn't have the use key's fields\n");
		return;
	}
	int target = engine::FindUseTarget(1);
	engine::Print("[use] time %.3f, press clock %.3f, locked until %.3f, pressed %d, holding %d; the key would use: %s\n", key.now, key.startTime,
	              key.lockedUntil, key.pressed, player.held, target < 0 ? "(the game's search wasn't found)" : target == 0 ? "nothing" : NameOf(target).c_str());
	Miss now;
	Measure(player, now);
	if (now.thing.empty()) engine::Print("[use] from here: nothing usable near\n");
	else engine::Print("[use] from here: %s, %.2f units out of reach, %.2f degrees off, blocked %d\n", now.thing.c_str(), now.reachShort, now.aimOff, now.blocked);
	const Miss &m = g.miss;
	if (m.tick)
		engine::Print("[use] last miss at tick %d (now %d): %s, %.2f units out of reach, %.2f degrees off, blocked %d, locked %.2f s, late %d, early %d, caught after %d, shown %d\n",
		              m.tick, engine::TickCount(), m.thing.empty() ? "nothing near" : m.thing.c_str(), m.reachShort, m.aimOff, m.blocked, m.locked, m.ticksLate,
		              m.ticksEarly, m.caughtAfter, m.shown);
}

void PrintProbe(float forward, float pitch, float yaw) {
	int target = Probe(Direction(pitch, yaw), forward);
	engine::Print("[use] %.1f units along pitch %.1f yaw %.1f: %s\n", forward, pitch, yaw, target < 0 ? "(can't be asked)" : target == 0 ? "nothing" : NameOf(target).c_str());
}

}  // namespace use
