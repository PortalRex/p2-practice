// On-screen text: the clock, the gap to the run, flashes.
#include "trainer_internal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace trainer {

// The plugin draws the host's HUD itself (ui/hud.cpp) when it can: the game's text lines are then for the partner
// over the network only, whose game doesn't run the plugin.
bool g_drawnHud = false;

void Say(int entIndex, int channel, float hold, const std::string &text) {
	if (g_drawnHud && entIndex == 1) return;
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

// The clock at the top of the screen with the gap to the record under it, flashes (GO, the parts, the finish) a
// little lower, and the speed in small print at the bottom left, out of the way
void SetupHud(int entIndex) {
	SetText(entIndex, kChClock, -1, kClockY, 255, 255, 255);
	SetText(entIndex, kChDelta, -1, kDeltaY, 170, 170, 170);
	SetText(entIndex, kChFlash, -1, kFlashY, 255, 255, 255);
	SetText(entIndex, kChSpeed, 0.045f, 0.905f, 150, 150, 150);
}

// How fast a run's player moved over the ground at `tick`
float GroundSpeed(const Route &route, int actor, int tick) {
	Vec3 velocity = VelocityAt(route, actor, tick);
	return Length({velocity.x, velocity.y, 0});
}

// The run's clock at `tick`, as shown (see Route::Show: level with the record reads exactly as the record does)
std::string ShowAt(int tick) { return g.route.Show(tick - g.route.timerStart); }

// The gap between two lengths of time: the difference of the two as shown, so a finish shown "+0.02" next to the
// record is the difference of the two times on screen. (Counted in hundredths cut off, as the times are.)
std::string Gap(int ticks, int recordTicks) {
	int hundredths = route::Hundredths(ticks) - route::Hundredths(recordTicks);
	char text[32];
	int size = std::abs(hundredths);
	snprintf(text, sizeof(text), "%c%d.%02d", hundredths < 0 ? '-' : '+', size / 100, size % 100);
	return text;
}

// The line under the clock: grey for a word on what is going on, the portal colours for the gap to the record
// (blue while ahead of it, orange while behind)
void SetDeltaColor(int entIndex, bool grey, bool ahead) {
	if (grey) SetText(entIndex, kChDelta, -1, kDeltaY, 170, 170, 170);
	else SetText(entIndex, kChDelta, -1, kDeltaY, ahead ? 60 : 255, ahead ? 170 : 150, ahead ? 255 : 30);
}

// `hold` is how long the lines stay up: by default just past the next update
void UpdateHud(float hold) {
	if (!g_set.hud) return;
	if (hold < 0) hold = kHudInterval * g.route.tickInterval + 0.25f;
	const float dt = g.route.tickInterval;
	for (size_t a = 0; a < g.slots.size(); a++) {
		const Slot &slot = g.slots[a];
		if (!ShowsHud(slot)) continue;
		int ent = slot.entIndex;

		if (g.mode == Mode::LeadIn || g.mode == Mode::CatchUp) {
			float left = (g.handover - g.cursor) * dt;
			char text[64];
			if (g.mode == Mode::CatchUp) snprintf(text, sizeof(text), "setting up the level...");
			else if (g.replaying) snprintf(text, sizeof(text), "replay starts in %.1f", left);
			else snprintf(text, sizeof(text), "you take over in %.1f", left);
			SetDeltaColor(ent, true, false);
			Say(ent, kChDelta, hold, text);
			if (g.mode == Mode::LeadIn) Say(ent, kChClock, hold, ShowAt(std::max(g.cursor, g.route.timerStart)));
			continue;
		}
		if (g.mode == Mode::Watch) {
			Say(ent, kChClock, hold, ShowAt(std::min(g.cursor, g.route.timerEnd)));
			SetDeltaColor(ent, true, false);
			Say(ent, kChDelta, hold, "watching the record");
			continue;
		}

		// Run (and the finish): the clock, and how far ahead of or behind the record this player is
		int clock = slot.finished ? slot.finishCursor : LiveTick();
		Say(ent, kChClock, hold, ShowAt(clock));
		SetDeltaColor(ent, false, clock < slot.progress);
		Say(ent, kChDelta, hold, Gap(clock - g.route.timerStart, slot.progress - g.route.timerStart));

		engine::PlayerState state;
		if (g_set.speedometer && !slot.finished && engine::GetPlayer(ent, state)) {
			// (a replayed run carries the player along: its speed is the replayed run's, the player's own is zero)
			float speed = g.replaying ? GroundSpeed(Driver(), static_cast<int>(a), DriverTick(g.cursor))
			                          : Length({state.velocity.x, state.velocity.y, 0});
			char text[96];
			snprintf(text, sizeof(text), "SPEED %.0f%sRECORD %.0f", speed, kDot, GroundSpeed(g.route, static_cast<int>(a), slot.progress));
			Say(ent, kChSpeed, hold, text);
		}
	}
}

std::string g_flashText;
std::chrono::steady_clock::time_point g_flashUntil;

void Flash(float seconds, const std::string &text) {
	g_flashText = text;
	g_flashUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int>(seconds * 1000));
	for (const Slot &slot : g.slots)
		if (ShowsHud(slot)) Say(slot.entIndex, kChFlash, seconds, text);
}

}  // namespace trainer
