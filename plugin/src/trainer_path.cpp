// The route drawn in the level.
#include "trainer_internal.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace trainer {

constexpr int kPathLineBudget = 120;   // debug lines on screen at once, portal marks aside
constexpr int kPathBand = 3;           // lines drawn per stretch (a debug line is one pixel wide)
constexpr float kPathTolerance = 3;    // how far, in units, the thinned line may leave the run's path
constexpr int kPathBehindTicks = 30;   // how much of the route behind a player stays drawn
constexpr int kPathStride = 6;         // stretches a player passes before the drawn window moves on

struct PathStretch {
	int from = 0, to = 0;  // run ticks
};
struct ActorPath {
	std::vector<PathStretch> stretches;  // in tick order; empty for a robot whose route isn't drawn
	int shownFrom = 0;                   // the first stretch drawn
};
std::vector<ActorPath> g_paths;  // one per robot of the run, for the attempt in progress
int g_pathShare = 0;             // stretches drawn per robot at once; 0: the whole route is drawn

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

void DrawStretch(int actor, const PathStretch &s, Rgb c, float seconds) {
	const Vec3 p = g.route.At(actor, s.from).pos, q = g.route.At(actor, s.to).pos;
	// Three lines, a unit apart sideways and upwards, read as a band
	float dx = q.x - p.x, dy = q.y - p.y;
	float length = std::sqrt(dx * dx + dy * dy);
	float sx = length > 0.01f ? -dy / length : 1, sy = length > 0.01f ? dx / length : 0;
	static const float offsets[kPathBand][2] = {{0, 0}, {1, 0}, {-1, 1}};  // sideways, up
	for (const auto &offset : offsets)
		engine::Script("::PP.Line(%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%d,%d,%d,%.0f)", p.x + sx * offset[0], p.y + sy * offset[0], p.z + 4 + offset[1],
		               q.x + sx * offset[0], q.y + sy * offset[0], q.z + 4 + offset[1], c.r, c.g, c.b, seconds);
}

// A small cross where a portal of the run goes, and the line of the shot from the shooter's eye
void DrawPortalShot(int actor, const route::PortalChange &portal, float seconds) {
	const Sample &shooter = g.route.At(actor, portal.tick);
	for (int axis = 0; axis < 3; axis++) {
		Vec3 lo = portal.pos, hi = portal.pos;
		(&lo.x)[axis] -= 14;
		(&hi.x)[axis] += 14;
		engine::Script("::PP.Line(%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,255,255,255,%.0f)", lo.x, lo.y, lo.z, hi.x, hi.y, hi.z, seconds);
	}
	engine::Script("::PP.Line(%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,255,255,255,%.0f)", shooter.pos.x, shooter.pos.y, shooter.pos.z + 56, portal.pos.x,
	               portal.pos.y, portal.pos.z, seconds);
}

// Draws what is due of the routes now: all of them, or each one's stretch ahead of its player
void RedrawPaths() {
	engine::Execute("clear_debug_overlays");
	const float kForever = 100000;
	for (size_t a = 0; a < g_paths.size(); a++) {
		ActorPath &path = g_paths[a];
		if (path.stretches.empty()) continue;
		int count = static_cast<int>(path.stretches.size());
		int first = g_pathShare ? FirstStretchAhead(a) : 0;
		int last = g_pathShare ? std::min(count, first + g_pathShare) - 1 : count - 1;
		path.shownFrom = first;
		Rgb c = TeamColor(g.route.actors[a].team);
		for (int i = first; i <= last; i++) DrawStretch(static_cast<int>(a), path.stretches[i], c, kForever);
		// (the portals shot along the drawn stretch; of a route drawn whole, all from the handover on)
		int fromTick = g_pathShare ? path.stretches[first].from : g.handover;
		int toTick = g_pathShare ? path.stretches[last].to : g.route.timerEnd;
		for (const auto &portal : g.route.portals) {
			if (!portal.active || portal.actor != static_cast<int>(a) || portal.tick < fromTick || portal.tick > toTick) continue;
			DrawPortalShot(static_cast<int>(a), portal, kForever);
		}
	}
}

// Sets up the attempt's drawn routes (at the start of its lead-in)
void DrawRoute() {
	g_paths.clear();
	g_pathShare = 0;
	engine::Execute("clear_debug_overlays");
	if (!g_set.path) return;
	g_paths.resize(g.route.actors.size());
	int drawn = 0, stretches = 0;
	for (size_t a = 0; a < g_paths.size() && a < g.slots.size(); a++) {
		if (!g.slots[a].human && !g.replaying) continue;
		g_paths[a].stretches = BuildPath(static_cast<int>(a));
		if (g_paths[a].stretches.empty()) continue;
		drawn++;
		stretches += static_cast<int>(g_paths[a].stretches.size());
	}
	if (!drawn) return;
	if (stretches * kPathBand > kPathLineBudget) g_pathShare = std::max(kPathStride + 1, kPathLineBudget / kPathBand / drawn);
	RedrawPaths();
}

// Moves the drawn stretch of a long route on with its players (every tick of an attempt)
void FollowRoute() {
	if (!g_pathShare) return;
	for (size_t a = 0; a < g_paths.size(); a++) {
		const ActorPath &path = g_paths[a];
		if (path.stretches.empty()) continue;
		int first = FirstStretchAhead(a);
		// (on, or back: a player who went wrong is found again further back on the route)
		if (first >= path.shownFrom + kPathStride || first < path.shownFrom) {
			RedrawPaths();
			return;
		}
	}
}

}  // namespace trainer
