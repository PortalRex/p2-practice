// Where the players are against the run, the finish, and the game's finish board.
#include "trainer_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace trainer {

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
	for (size_t a = 0; a < g.slots.size(); a++) {
		Slot &slot = g.slots[a];
		if (!slot.entIndex || slot.finished || (!slot.human && !g.replaying)) continue;
		engine::PlayerState state;
		if (!engine::GetPlayer(slot.entIndex, state)) continue;
		slot.progress = FindProgress(static_cast<int>(a), state.origin, slot.progress);

		// Entering the next part: report this one
		while (slot.part + 1 < static_cast<int>(g.route.segments.size()) && slot.progress >= g.route.segments[slot.part].endTick) {
			const route::Segment &part = g.route.segments[slot.part];
			int ticks = LiveTick() - slot.partStart;
			int record = part.endTick - std::max(part.startTick, g.handover);
			bool best = false;
			if (!g.replaying && part.startTick >= g.handover) RecordBest(g.bestPart, slot.part, ticks, best);
			char text[128];
			snprintf(text, sizeof(text), "PART %d%s%s%s%s%s", slot.part + 1, kDot, route::FormatHundredths(route::Hundredths(ticks)).c_str(), kDot,
			         Gap(ticks, record).c_str(), best && g.attempts > 1 ? "  \xC2\xB7  BEST" : "");
			if (g_set.hud) Say(slot.entIndex, kChFlash, 2.5f, text);
			slot.part++;
			slot.partStart = LiveTick();
		}

		// (while the Challenge Mode flags stand, they are the finish: see WatchFlags)
		const Sample &end = g.route.At(static_cast<int>(a), g.route.timerEnd);
		float toEnd = Distance(state.origin, end.pos);
		// (a replay runs on a few ticks past its end while the flags stand, to see when it touches them: NoteReplayAtFlags)
		constexpr int kReplayFlagsTicks = 10;
		bool arrived = g.replaying ? g.cursor >= LastDrivenTick() + (g.flagsUp ? kReplayFlagsTicks : 0)
		                           : !g.flagsUp && (toEnd < kFinishRadius || (slot.progress >= g.route.timerEnd - 2 && toEnd < 2 * kFinishRadius));
		if (arrived) {
			slot.finished = true;
			// (a replay ends where its run ends, by definition; a person where they were seen arriving)
			slot.finishCursor = g.replaying ? LastDrivenTick() : LiveTick();
			slot.progress = g.route.timerEnd;  // level with the run's finish: the gap shown is now the gap in time
		}
	}
}

// The attempt is over: the time it would have made, next to the run's. From a part, the run's time up to the
// handover counts as the person's own (what the clock has been showing all along).
void Finish(bool atFlags) {
	int finish = 0;
	for (const Slot &slot : g.slots)
		if (slot.finished) finish = std::max(finish, slot.finishCursor);
	int ticks = finish - g.handover;
	bool best = false;
	if (!g.replaying) {
		RecordBest(g.bestFromTick, g.handover, ticks, best);
		Result result;
		result.part = g.segment;
		result.seconds = (finish - g.route.timerStart) * g.route.tickInterval;
		result.gap = (finish - g.route.timerEnd) * g.route.tickInterval;
		result.best = best;
		result.flags = atFlags;
		if (g.results.size() >= kResultsKept) g.results.erase(g.results.begin());
		g.results.push_back(result);
	}
	// (the clock and the gap stay up with their final values; the flash says what ended the attempt)
	UpdateHud(kLingerSeconds);
	Flash(4, std::string(atFlags ? "FLAGS" : "FINISH") + kDot + ShowAt(finish) +
	             (best && g.attempts > 1 ? std::string(kDot) + "PERSONAL BEST" : ""));
	engine::Print("%s from part %d: %s%s, record %s (%s)\n", g.replaying ? "Replayed run" : "Attempt", g.segment + 1,
	              ShowAt(finish).c_str(), atFlags ? " at the flags" : "", ShowAt(g.route.timerEnd).c_str(),
	              Gap(finish - g.route.timerStart, g.route.timerEnd - g.route.timerStart).c_str());
	ReleaseUseKeys();
	SetDuckKey(false);
	g.mode = Mode::Done;
	g.doneTicks = 0;
}

// A save state that holds the demo being recorded (it couldn't be stopped for it: see StopDemoForSnapshot) goes as
// soon as anyone reaches the flags. SAR stops that demo a quarter of a second later and renames it with the time,
// which the open file would make fail, and SAR's error closes the game. Without the save state the demo is closed
// for real and renamed as usual; a retry after the finish restarts the level. (Called from every frame, paused or
// not, in every mode: the board pauses the game, and a run watched to its end or replayed reaches the flags too.)
void DropSnapshotAtFlags() {
	if (g.snapshotHandover < 0 || !g.snapshotHoldsDemo || !g.flagsUp || engine::EntityExists(kEndNode)) return;
	DropSnapshot();
	engine::Print("At the flags: the save state is dropped so the demo can be closed (the next attempt restarts the level).\n");
}

// In Challenge Mode the level's finish is its flags: the game puts an end node at the exit, ends the timer when
// a player touches it (in co-op, when both have), and removes it. With cheats on the game shows no time, so
// the attempt's own is shown instead: what the flags would have said. The node is watched for its removal (the
// touch was in the tick just simulated); a finish takes the flags away for good, so the next attempt reloads.
bool WatchFlags() {
	if (!g.flagsUp || engine::EntityExists(kEndNode)) return false;
	DropSnapshotAtFlags();
	g.flagsUp = false;
	g.flagsGone = true;
	return true;
}

// A replayed run is carried through the flags by the trainer, as its players went: the game's flags then say when
// it got there, measured exactly as an attempt's finish is. That time must be the replayed demo's own; this is the
// check that the trainer's clock and the demos' agree (prac_replay of the loaded run itself: "0 ticks").
void NoteReplayAtFlags() {
	const Route &replayed = g.replay;
	int tick = DriverTick(FlagsTick());
	int off = tick - replayed.timerEnd;
	engine::Print("Replayed run at the flags: %s (its demo: %s, %+d tick%s)\n", replayed.Show(tick - replayed.timerStart).c_str(),
	              replayed.Show(replayed.timerEnd - replayed.timerStart).c_str(), off, off == 1 || off == -1 ? "" : "s");
}

void FinishAtFlags() {
	for (Slot &slot : g.slots) {
		if (!slot.entIndex || !slot.human || slot.finished) continue;
		slot.finished = true;
		slot.finishCursor = FlagsTick();
		slot.progress = g.route.timerEnd;
	}
	TellBoard();
	Finish(true);
}

// The game's finish board shows the time of the run it recorded, so "-" here: at a finish at the flags it is
// told the attempt's time instead. Once, at the finish only: the board also keeps the best value it was ever
// told as the player's own entry in its list, so a running time sent earlier would stand there in its place.
// The board fills its Final box once, when it opens, from the best time it was told for the level. The game
// opens it ("leaderboard_open 1" sent to the players' consoles) in the frame the flags are touched, and the
// plugin only sees the touch a frame later: the board would be up before the time reaches it, and say "-".
// So while an attempt runs towards the flags, that command is held back (HoldBoard), and it goes out here,
// right after the time.
void TellBoard() {
	if (ChallengeMode() && !g.replaying) {
		// The time of the tick just simulated (the finish's own), in hundredths as the board shows times
		engine::SendScoreboard(-1, g.route.ShownHundredths(FlagsTick() - g.route.timerStart));
	}
	engine::ReleaseClientCommands();
	g.boardHeldFrames = 0;
}

// What happens to a command the server sends to a player's console. The finish board's opening waits for
// TellBoard while an attempt of the person's own runs towards the flags. The start board (the game sends
// "+leaderboard 4" a few seconds into every Challenge Mode level) never opens during practice: it pauses the game
// until closed, and an attempt taken over early in the level had that pause land around the handover. (The
// trainer used to close it a frame later; now it doesn't come at all.)
engine::ClientCommandFate HoldBoard(const char *command) {
	if (!strcmp(command, "leaderboard_open 1") && g.mode == Mode::Run && !g.replaying && ChallengeMode() && g.flagsUp)
		return engine::ClientCommandFate::Hold;
	if (!strncmp(command, "+leaderboard", 12) && g.hasRoute && MapMatches() && ((g.mode != Mode::Idle && g.mode != Mode::Done) || g.autoPart >= 0)) {
		g.startScreenClosed = true;
		return engine::ClientCommandFate::Drop;
	}
	return engine::ClientCommandFate::Send;
}

// A held board that no finish came for (the flags went some other way) goes out after a few frames anyway
void ReleaseStaleBoard() {
	if (engine::HeldClientCommands() == 0) return;
	if (++g.boardHeldFrames < kBoardHoldFrames) return;
	engine::ReleaseClientCommands();
	g.boardHeldFrames = 0;
}

}  // namespace trainer
