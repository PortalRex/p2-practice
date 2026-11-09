#include "bhop.h"

#include <cmath>

#include "engine.h"
#include "trainer.h"

namespace bhop {
namespace {

using p2demo::Vec3;

constexpr size_t kRecentKept = 12;
constexpr float kChange = 0.05f;    // u/s: a tick's change in speed smaller than this counts as neither
constexpr float kThrown = 60;       // u/s in one tick: more than strafing gives (or friction takes) in the air
constexpr float kTeleported = 128;  // units in one tick: a trip through a portal
constexpr int kFlagOnGround = 1;

Status g;
bool g_hadPlayer = false, g_wasOnGround = true;
float g_lastSpeed = 0;
Vec3 g_lastOrigin;
int g_groundTicks = -1;  // ticks on the ground since the last landing (-1: none yet, or the chain was broken)

float Horizontal(const Vec3 &v) { return std::sqrt(v.x * v.x + v.y * v.y); }

}  // namespace

void Reset() {
	g = Status{};
	g_hadPlayer = false;
	g_wasOnGround = true;
	g_groundTicks = -1;
}

const Status &Current() { return g; }

void Tick() {
	if (!trainer::CurrentSettings().strafe) {
		if (g_hadPlayer) Reset();
		return;
	}
	engine::PlayerState state;
	if (!engine::GetPlayer(1, state) || !state.alive) {
		Reset();
		return;
	}
	float speed = Horizontal(state.velocity);
	bool onGround = (state.flags & kFlagOnGround) != 0;
	if (!g_hadPlayer) {
		g_hadPlayer = true;
		g_wasOnGround = onGround;
		g_lastSpeed = speed;
		g_lastOrigin = state.origin;
		return;
	}
	float dx = state.origin.x - g_lastOrigin.x, dy = state.origin.y - g_lastOrigin.y, dz = state.origin.z - g_lastOrigin.z;
	bool teleported = std::sqrt(dx * dx + dy * dy + dz * dz) > kTeleported;

	if (!onGround && g_wasOnGround) {
		// take-off
		g.current = Jump{};
		g.current.takeOff = g_lastSpeed;
		g.current.groundTicks = g_groundTicks;
		g.inAir = true;
		if (g_groundTicks < 0 || g_groundTicks > kChainGroundTicks) g.chain = 0;
	}
	if (!onGround && g.inAir) {
		float change = speed - g_lastSpeed;
		g.current.airTicks++;
		if (teleported || std::fabs(change) > kThrown) g.current.thrown = true;
		else if (change > kChange) g.current.gainingTicks++;
		else if (change < -kChange) g.current.losingTicks++;
	}
	if (onGround && !g_wasOnGround && g.inAir) {
		// landing: the jump is done
		g.current.landing = speed;
		g.inAir = false;
		if (!g.current.thrown) g.chain++;
		g.recent.push_back(g.current);
		if (g.recent.size() > kRecentKept) g.recent.erase(g.recent.begin());
		g_groundTicks = 0;
	}
	if (onGround) {
		if (g_groundTicks >= 0) g_groundTicks++;
		if (g_groundTicks > kChainGroundTicks) g.chain = 0;
	}
	g_wasOnGround = onGround;
	g_lastSpeed = speed;
	g_lastOrigin = state.origin;
}

}  // namespace bhop
