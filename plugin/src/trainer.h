// The practice session: loads a route, puts the players on it, runs the ghost, the partner bot, the clock and the
// on-screen text, and resets between attempts.
#pragma once

#include <string>
#include <vector>

#include "sdk.h"

namespace trainer {

void Init();      // registers the prac_ console commands
void Shutdown();  // ends any session and gives the game back as it was

void OnLevelInit(const char *mapName);
void OnLevelShutdown();
void OnFrame(bool simulating);  // once per server tick

// A prac_ command typed by a player who doesn't have the plugin (the co-op partner): true when handled.
bool OnClientCommand(const sdk::CCommand &args);

// The game's restart_level / mp_restart_level from a player's game (the finish board's Replay Map, the menu's
// Restart Level): true when it was taken as a retry of the attempt instead.
bool OnRestartCommand();

// ---- settings (prac_set) ----
enum ResetMode { kResetAuto, kResetSoft, kResetReload };
enum GhostStyle { kGhostSolid = 0, kGhostWire = 1, kGhostWireLow = 2, kGhostBasic = 3 };

struct Settings {
	float leadIn = 1.5f;     // seconds the run plays itself before the players take over
	bool ghost = true;       // show the recorded players
	bool challenge = true;   // practice every run in Challenge Mode (flags, the game's timer), not only runs made in it
	bool ghostColorSet = false;  // the ghosts in one colour of the player's choosing rather than their robots' colours
	int ghostColor[3] = {255, 255, 255};
	int ghostOpacity = 100;      // percent; below 100 the ghosts are drawn see-through
	int ghostStyle = kGhostBasic;  // how: kGhostSolid, kGhostWire, kGhostWireLow, kGhostBasic
	bool hud = true;         // on-screen clock, delta, part
	bool path = true;        // draw the route and its portal shots in the world (seen by the host)
	bool speedometer = true; // show the person's speed next to the record's
	bool loop = false;       // start again by itself after the finish
	float autoReset = 0;     // retry by itself when this many seconds behind (0 = never)
	float speed = 1;         // game speed while practicing
	int role = 0;            // solo co-op: the robot the player plays (team), 0 = the one they are
	bool partner = true;     // solo co-op: drive the other robot through its half of the route
	int reset = kResetAuto;  // how to get back to a part's start: auto, soft (teleport only), reload (map restart)
	bool snapshots = false;  // retry through ChocolateLoading's memory snapshots when that plugin is running
};
const Settings &CurrentSettings();

// The host's clock, gap, speed and flashes are drawn by the plugin's own HUD: the game's text lines (game_text)
// then go to the partner over the network only.
void UseDrawnHud(bool on);

// ---- what the session is doing, for the menu and the HUD ----
struct Part {
	std::string name;
	float start = 0, end = 0;  // seconds of the run's time
};

struct Status {
	// the loaded run
	bool loaded = false;
	std::string chamber, map, recordTime;  // "Doors", "mp_coop_doors", "6.46"
	std::vector<std::string> files;        // its demo(s)
	bool coop = false;
	float runSeconds = 0;
	std::vector<Part> parts;
	bool mapLoaded = false;   // the run's map is the one up now
	bool snapshotsReady = false;  // ChocolateLoading is running
	// the session
	bool active = false;      // practicing or watching
	bool watching = false;
	std::string mode;         // "lead-in", "run", "watch", "done", "reloading", ...
	int part = -1;            // the part the attempt is in
	float handoverSeconds = 0;
	int attempts = 0;
	// the person's clock (when the session is on)
	std::string clock, gap, note;  // "4.53", "-0.12" (gap to the record), "you take over in 0.8"
	bool ahead = false, gapShown = false;
	bool finished = false;
	float speed = -1, recordSpeed = -1;  // ground speeds; -1 when not shown
	std::string flash;        // the last message flashed (finish, part times), while it is up
};
Status CurrentStatus();

}  // namespace trainer
