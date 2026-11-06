// Everything the trainer needs from the running game, behind plain functions.
//
// Reading goes through networked entity fields found by name at load time. Acting goes through the game's own
// console commands and VScript, so the plugin installs no hooks and patches nothing: it cannot collide with SAR,
// p2boost or any other plugin, and it keeps working across game updates as long as those names exist.
#pragma once

#include <string>

#include "p2demo/demo.h"
#include "sdk.h"

namespace engine {

using p2demo::Vec3;

// Looks up the interfaces; false (with a reason printed) when the game is not one this plugin understands.
bool Init(sdk::CreateInterfaceFn interfaceFactory, sdk::CreateInterfaceFn serverFactory);
void Shutdown();

// ---- console ----
void Print(const char *format, ...);
void Warn(const char *format, ...);  // same, in the warning colour
void Debug(const char *format, ...); // only into the development trace file (P2PRACTICE_LOG), never the console
bool RegisterCommand(const char *name, sdk::CommandCallback callback, const char *help);

// Runs one console line now, as if typed (cvars and commands; ';' separates commands).
void Execute(const char *format, ...);
// Calls a console command's handler directly with `arguments` as its raw argument string: nothing is split on
// ';' or checked against sv_cheats, which VScript code needs. False when no such command exists.
bool CallCommand(const char *name, const char *arguments);

bool CommandExists(const char *name);  // a console command of that name is registered (by the game or a plugin)
bool CheatsOn();
float CvarFloat(const char *name, float fallback = 0);

// ---- VScript ----
// Statements are collected and run in as few "script" calls as fit (one call holds about 500 characters).
void Script(const char *format, ...);
void FlushScript();

// ---- game state ----
int MaxClients();
int TickCount();
float TickInterval();
const char *MapName();             // "" between maps
std::string GameDirectory();       // ...\Portal 2\portal2

enum Team { kTeamNone = 0, kTeamOrange = 2, kTeamBlue = 3 };

struct PlayerState {
	Vec3 origin;       // feet
	Vec3 velocity;
	float pitch = 0, yaw = 0;
	float viewOffsetZ = 0;
	int flags = 0;     // FL_ONGROUND 1, FL_DUCKING 2
	int team = 0;
	bool alive = false;
	bool bot = false;  // no human behind it: the second split-screen player, or a fake client
};

// False when no player holds that entity index (1-based).
bool GetPlayer(int entIndex, PlayerState &out);

// The entity indices of the two portals a player's portal gun shoots (-1 where it has none). False when the
// player holds no portal gun.
bool GetPlayerPortals(int playerEntIndex, int &primary, int &secondary);

// Development aid: the raw pointers and offsets GetPlayer works from, printed without following them.
void PrintRawState();

}  // namespace engine
