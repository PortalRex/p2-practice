// The route drawn in the level: a ribbon along the run's path ahead of each player, and a mark where each portal
// of the run goes. Drawn by the plugin itself each frame (gfx's drawing in the level), so the whole of it costs a
// few batches of triangles however long the route is.
#include "trainer_internal.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "ui/gfx.h"

namespace trainer {

constexpr float kPathTolerance = 3;    // how far, in units, the thinned line may leave the run's path
constexpr int kPathBehindTicks = 30;   // how much of the route behind a player stays drawn
constexpr float kPathAhead = 20;       // seconds of the route drawn ahead of a player...
constexpr float kPathClear = 8;        // ...of which this much at full strength; from there it fades out
constexpr int kPathAlpha = 220;
constexpr float kPathHalfWidth = 1.5f;
constexpr float kPathLift = 4;         // above the floor the player stood on
constexpr float kMarkSize = 14, kMarkHalfWidth = 0.75f, kShotHalfWidth = 0.4f;
constexpr float kEyeHeight = 56;

struct PathStretch {
	int from = 0, to = 0;  // run ticks
};
struct ActorPath {
	std::vector<PathStretch> stretches;  // in tick order; empty for a robot whose route isn't drawn
};
std::vector<ActorPath> g_paths;          // one per robot of the run, for the attempt in progress
std::vector<gfx::WorldVertex> g_shapes;  // what a frame draws (kept between frames for its memory)

float DistanceToStretch(Vec3 p, Vec3 a, Vec3 b) {
	Vec3 ab = Sub(b, a), ap = Sub(p, a);
	float lengthSquared = ab.x * ab.x + ab.y * ab.y + ab.z * ab.z;
	float along = lengthSquared > 0 ? std::clamp((ab.x * ap.x + ab.y * ap.y + ab.z * ap.z) / lengthSquared, 0.0f, 1.0f) : 0;
	return Distance(p, {a.x + ab.x * along, a.y + ab.y * along, a.z + ab.z * along});
}

// Thins the unbroken piece of robot `actor`'s path between two ticks to straight stretches that stay within
// kPathTolerance of it (Douglas-Peucker: split at the sample furthest from the straight line, until none is far).
void ThinPath(int actor, int first, int last, std::vector<PathStretch> &out) {
	std::vector<PathStretch> todo{{first, last}};
	while (!todo.empty()) {
		PathStretch s = todo.back();
		todo.pop_back();
		const Vec3 a = g.route.At(actor, s.from).pos, b = g.route.At(actor, s.to).pos;
		float furthest = 0;
		int split = -1;
		for (int tick = s.from + 1; tick < s.to; tick++) {
			float off = DistanceToStretch(g.route.At(actor, tick).pos, a, b);
			if (off > furthest) {
				furthest = off;
				split = tick;
			}
		}
		if (furthest <= kPathTolerance) {
			out.push_back(s);
		} else {
			// (the later half goes on the stack first: the stretches come out in tick order)
			todo.push_back({split, s.to});
			todo.push_back({s.from, split});
		}
	}
}

// The straight stretches of robot `actor`'s path from the handover to the finish. The path breaks where the
// player wasn't in the level or went through a portal: nothing is drawn across a break.
std::vector<PathStretch> BuildPath(int actor) {
	std::vector<PathStretch> stretches;
	const float kBreak = 64 + 3500 * g.route.tickInterval;  // further than a player gets in a tick
	int pieceStart = -1;
	for (int tick = g.handover; tick <= g.route.timerEnd; tick++) {
		const Sample &s = g.route.At(actor, tick);
		bool joined = pieceStart >= 0 && s.present && Distance(g.route.At(actor, tick - 1).pos, s.pos) <= kBreak;
		if (!joined) {
			if (pieceStart >= 0 && tick - 1 > pieceStart) ThinPath(actor, pieceStart, tick - 1, stretches);
			pieceStart = s.present ? tick : -1;
		}
	}
	if (pieceStart >= 0 && g.route.timerEnd > pieceStart) ThinPath(actor, pieceStart, g.route.timerEnd, stretches);
	return stretches;
}

// The tick of the run robot `actor`'s player has got to: the drawn route starts just behind it
int PathProgress(size_t actor) {
	if (g.mode == Mode::Run && !g.replaying && g.slots[actor].human) return g.slots[actor].progress;
	return std::clamp(g.cursor, g.handover, g.route.timerEnd);
}

// The first stretch still of use to robot `actor`'s player
int FirstStretchAhead(size_t actor) {
	const std::vector<PathStretch> &stretches = g_paths[actor].stretches;
	int behind = PathProgress(actor) - kPathBehindTicks;
	int first = 0;
	while (first + 1 < static_cast<int>(stretches.size()) && stretches[first].to <= behind) first++;
	return first;
}

gfx::WorldVertex Corner(Vec3 at, float sx, float sy, float sz, float scale, gfx::Color color) {
	gfx::WorldVertex v;
	v.x = at.x + sx * scale;
	v.y = at.y + sy * scale;
	v.z = at.z + sz * scale;
	v.color = color;
	return v;
}

// A straight beam from p to q: two flat strips crossing each other along it, so it has width from every side
void AddBeam(Vec3 p, Vec3 q, float halfWidth, gfx::Color atP, gfx::Color atQ) {
	Vec3 along = Sub(q, p);
	float length = std::sqrt(along.x * along.x + along.y * along.y + along.z * along.z);
	if (length < 0.01f || (!atP.a && !atQ.a)) return;
	along = {along.x / length, along.y / length, along.z / length};
	// sideways: level and square to the beam (any level direction for a beam going straight up or down)
	float sx = -along.y, sy = along.x;
	float level = std::sqrt(sx * sx + sy * sy);
	if (level < 0.01f) {
		sx = 1;
		sy = 0;
	} else {
		sx /= level;
		sy /= level;
	}
	// the other way across: square to both
	float ux = sy * along.z, uy = -sx * along.z, uz = sx * along.y - sy * along.x;
	for (int strip = 0; strip < 2; strip++) {
		float ax = strip ? ux : sx, ay = strip ? uy : sy, az = strip ? uz : 0;
		g_shapes.push_back(Corner(p, ax, ay, az, -halfWidth, atP));
		g_shapes.push_back(Corner(p, ax, ay, az, halfWidth, atP));
		g_shapes.push_back(Corner(q, ax, ay, az, halfWidth, atQ));
		g_shapes.push_back(Corner(q, ax, ay, az, -halfWidth, atQ));
	}
}

// How strongly the route shows at a run tick, for a player who has got to `progress`
int PathAlpha(int tick, int progress) {
	float ahead = (tick - progress) * g.route.tickInterval;
	if (ahead <= kPathClear) return kPathAlpha;
	if (ahead >= kPathAhead) return 0;
	return static_cast<int>(kPathAlpha * (kPathAhead - ahead) / (kPathAhead - kPathClear));
}

// A small cross where a portal of the run goes, and the line of the shot from the shooter's eye
void AddPortalShot(int actor, const route::PortalChange &portal, int alpha) {
	const gfx::Color white(255, 255, 255, alpha);
	for (int axis = 0; axis < 3; axis++) {
		Vec3 lo = portal.pos, hi = portal.pos;
		(&lo.x)[axis] -= kMarkSize;
		(&hi.x)[axis] += kMarkSize;
		AddBeam(lo, hi, kMarkHalfWidth, white, white);
	}
	Vec3 eye = g.route.At(actor, portal.tick).pos;
	eye.z += kEyeHeight;
	AddBeam(eye, portal.pos, kShotHalfWidth, white.Alpha(alpha / 2), white.Alpha(alpha / 2));
}

void ClearRoute() {
	g_paths.clear();
	g_shapes.clear();
}

// Sets up the attempt's drawn routes (at the start of its lead-in, and when the setting changes in an attempt)
void SetUpRoute() {
	ClearRoute();
	if (!g_set.path) return;
	g_paths.resize(g.route.actors.size());
	for (size_t a = 0; a < g_paths.size() && a < g.slots.size(); a++)
		if (g.slots[a].human || g.replaying) g_paths[a].stretches = BuildPath(static_cast<int>(a));
}

// Draws the routes in the level: once per view of it in each frame (see gfx::SetWorldCallback)
void DrawRouteInLevel() {
	if (g_paths.empty() || (g.mode != Mode::LeadIn && g.mode != Mode::Run && g.mode != Mode::Watch)) return;
	g_shapes.clear();
	for (size_t a = 0; a < g_paths.size() && a < g.route.actors.size(); a++) {
		const std::vector<PathStretch> &stretches = g_paths[a].stretches;
		if (stretches.empty()) continue;
		const int actor = static_cast<int>(a);
		const int progress = PathProgress(a);
		const int lastTick = progress + static_cast<int>(kPathAhead / g.route.tickInterval);
		const Rgb team = TeamColor(g.route.actors[a].team);
		const gfx::Color color(team.r, team.g, team.b);
		const size_t first = static_cast<size_t>(FirstStretchAhead(a));
		for (size_t i = first; i < stretches.size() && stretches[i].from < lastTick; i++) {
			Vec3 p = g.route.At(actor, stretches[i].from).pos, q = g.route.At(actor, stretches[i].to).pos;
			p.z += kPathLift;
			q.z += kPathLift;
			AddBeam(p, q, kPathHalfWidth, color.Alpha(PathAlpha(stretches[i].from, progress)), color.Alpha(PathAlpha(stretches[i].to, progress)));
		}
		for (const auto &portal : g.route.portals) {
			if (!portal.active || portal.actor != actor || portal.tick < stretches[first].from || portal.tick >= lastTick) continue;
			AddPortalShot(actor, portal, PathAlpha(portal.tick, progress));
		}
	}
	if (!g_shapes.empty()) gfx::WorldQuads(g_shapes.data(), static_cast<int>(g_shapes.size() / 4));
}

}  // namespace trainer
