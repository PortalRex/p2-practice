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
	bool challenge = false;  // spawned into Challenge Mode
	int held = -1;           // entity index of the object in the player's hands, -1 = none
	bool bot = false;  // no human behind it: the second split-screen player, or a fake client
};

// False when no player holds that entity index (1-based).
bool GetPlayer(int entIndex, PlayerState &out);

// Whether the level holds an entity of this class (the map's class name, as "challenge_mode_end_node")
bool EntityExists(const char *className);

// Puts a time (in hundredths of a second; -1 for none) and a portal count on the game's Challenge Mode finish
// board, which otherwise shows "-" for a run it didn't record. False when the game offers no way.
bool SendScoreboard(int portals, int hundredths);

// Turns the game's god mode on or off for a player: the flag its own "god" command sets, which that command
// can only set for the player typing it. This is the one place the plugin writes into the game's memory.
void SetGodMode(int entIndex, bool on);

// The entity indices of the two portals a player's portal gun shoots (-1 where it has none). False when the
// player holds no portal gun.
bool GetPlayerPortals(int playerEntIndex, int &primary, int &secondary);

// ---- the trainer's own props ----
// A model blends its walk between directions (and its aim between angles) by pose parameters, numbers from 0 to
// 1 that the game offers no console or script way to set. For a prop the trainer made itself (a ghost) they are
// written straight into the prop, and the prop is marked as changed so that the game sends them to the players.
// Besides god mode this is the only place the plugin writes into the game's memory, and never into anything
// that isn't its own prop_dynamic. False when `entIndex` is not a prop_dynamic.
constexpr int kPoseParameters = 24;
bool SetPoseParameters(int entIndex, const float values[kPoseParameters]);

// Development aid: the raw pointers and offsets GetPlayer works from, printed without following them.
void PrintRawState();
void PrintUserMessages();
bool SendTestChat(const char *text);
int FindBadEdict(char *detail, size_t size, int &inUse, int &highest);
int NoteEdictChanges(char *out, size_t size);

}  // namespace engine
