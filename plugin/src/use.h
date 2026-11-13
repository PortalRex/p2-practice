// The use trainer: when the player at this computer presses the use key and nothing is used, how far off it was.
//   - reach: how many units the nearest thing that can be used was beyond the key's reach
//   - aim: how many degrees it was outside the cone the key looks in
//   - timing: how many ticks before or after the press the key would have hit
// Whether a press hit, and when it would have, is not guessed: the player's own fields say what the game did with
// the press, and the game's own search (the function the key runs) is asked every tick what it would find. The
// reach and the aim are measured here, with the game's numbers, against the box of the nearest usable thing.
#pragma once

#include <string>

namespace use {

struct Miss {
	bool shown = false;     // there is something to say about the last press
	int tick = 0;           // the server tick it was noted on
	std::string thing;      // "cube", "button", ...: the usable thing nearest to the aim; "" when none was near
	float reachShort = 0;   // units it was beyond the key's reach (0: within)
	float aimOff = 0;       // degrees it was outside the key's cone (0: within)
	bool blocked = false;   // within reach and in the cone, and still not used: something stood in the way
	float locked = 0;       // seconds the key still picked nothing up (the second after it picked an object up)
	int ticksLate = 0;      // the key would have hit this many ticks before the press
	int ticksEarly = 0;     // ... this many ticks after it
	int caughtAfter = 0;    // the game picked the object up by itself this many ticks after the press
};

constexpr float kShownSeconds = 3.0f;  // how long the HUD keeps a miss up

void Tick();   // once per server tick (when the game simulates)
void Reset();  // a new level: nothing carries over
const Miss &Last();
void PrintState();  // development aid (prac_debug use): what the game would use now, and the last miss
void PrintProbe(float forward, float pitch, float yaw);  // development aid: what it would use from elsewhere

}  // namespace use
