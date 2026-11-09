// The bhop trainer: how the player at this computer's jumps are going, measured every server tick from their
// speed and whether they stand on the ground (nothing else is needed, and nothing is guessed):
//   - gain: horizontal speed at landing against take-off
//   - gaining: how much of the time in the air the speed went up (a strafe turned too sharply, or the wrong
//     key held, loses speed tick by tick: this is the share of ticks that didn't)
//   - hop: ticks spent on the ground before jumping again (the ground's friction takes speed every one of them)
// Jumps something else threw (a portal, a faith plate, gel: the speed jumps by more than strafing can give) are
// left out of the gain and the share.
#pragma once

#include <vector>

namespace bhop {

struct Jump {
	float takeOff = 0, landing = 0;  // horizontal speeds, u/s
	int airTicks = 0, gainingTicks = 0, losingTicks = 0;
	int groundTicks = 0;  // on the ground before this jump (since the last landing; -1: the first jump of a chain)
	bool thrown = false;  // something else changed the speed (see above)
	float Gain() const { return landing - takeOff; }
	int GainingPercent() const { return airTicks ? gainingTicks * 100 / airTicks : 0; }
};

struct Status {
	bool inAir = false;
	Jump current;              // the jump in the air now (when inAir), its numbers so far
	std::vector<Jump> recent;  // the latest jumps, oldest first (up to 12)
	int chain = 0;             // jumps in a row with at most kChainGroundTicks on the ground between them
};

constexpr int kChainGroundTicks = 3;  // more on the ground than this and the hop chain is broken

void Tick();   // once per server tick (when the game simulates)
void Reset();  // a new level: nothing carries over
const Status &Current();

}  // namespace bhop
