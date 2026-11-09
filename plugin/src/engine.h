// Everything the trainer needs from the running game, behind plain functions.
//
// Reading goes through networked entity fields found by name at load time. Acting goes through the game's own
// console commands and VScript wherever they reach, which keeps the trainer working across game updates as long
// as those names exist and out of the way of SAR, p2boost and other plugins. The exceptions, each described where
// it is declared below: one hooked function-table slot (HoldClientCommands), the hiding flags taken off two cvars
// (Init), and a handful of fields written straight into entities (SetGodMode,
// SetMovementSpeed, SetChallengeSeconds, SetEyeHeight, SetPoseParameters). The in-game menu and HUD hook the
// game's drawing and input besides: ui/gfx.cpp says how.
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
std::string CvarString(const char *name);  // "" when there is no such cvar

// ---- VScript ----
// Statements are collected and run in as few "script" calls as fit (one call holds about 500 characters).
void Script(const char *format, ...);
void FlushScript();

// ---- game state ----
int MaxClients();
int TickCount();
float TickInterval();
const char *MapName();             // "" between maps
std::string GameDirectory();       // ...\Portal 2\portal2, as UTF-8 (for std::filesystem::u8path)
bool RecordingDemo();              // a demo is being recorded (by the person, SAR or a config)

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

// Commands the server sends to players' consoles (engine->ClientCommand: the Challenge Mode boards open that way)
// pass through `filter` first: sent, held until ReleaseClientCommands sends them, or dropped. Installed once (the
// engine's vtable slot is put back by Shutdown). HeldClientCommands says how many wait.
enum class ClientCommandFate { Send, Hold, Drop };
void HoldClientCommands(ClientCommandFate (*filter)(const char *command));
void ReleaseClientCommands();
size_t HeldClientCommands();
void DropHeldClientCommands();

// Makes sure the calling thread has vstdlib's coroutine list (made the first time a thread creates a coroutine,
// found again through a thread-local pointer that a snapshot restore doesn't bring back): see TakeSnapshotIfDue.
void MakeCoroutineList();

// Turns the game's god mode on or off for a player: the flag its own "god" command sets, which that command
// can only set for the player typing it. Written straight into the player, as the setters below write theirs.
void SetGodMode(int entIndex, bool on);

// Scales how far the game's own movement takes a player each tick (m_flLaggedMovementValue: 1 normal, 0 not at
// all), on the server and in the player's predicted copy on screen alike. False when it can't be set.
bool SetMovementSpeed(int entIndex, float scale);
// Has the shader API send all of its render state to the graphics card again (false: unknown shader API)
bool ResetRenderState();
// Sets a player's Challenge Mode clock (the time in the corner of the screen) to `seconds`
bool SetChallengeSeconds(int entIndex, float seconds);
// Sets how high above its origin a player's eyes are (m_vecViewOffset z: 64 standing, 28 crouching)
bool SetEyeHeight(int entIndex, float height);

// The entity indices of the two portals a player's portal gun shoots (-1 where it has none). False when the
// player holds no portal gun.
bool GetPlayerPortals(int playerEntIndex, int &primary, int &secondary);

// ---- the trainer's own props ----
// A model blends its walk between directions (and its aim between angles) by pose parameters, numbers from 0 to
// 1 that the game offers no console or script way to set. For a prop the trainer made itself (a ghost) they are
// written straight into the prop, and the prop is marked as changed so that the game sends them to the players.
// Never into anything that isn't a prop_dynamic: false when `entIndex` is not one.
constexpr int kPoseParameters = 24;
bool SetPoseParameters(int entIndex, const float values[kPoseParameters]);

// Development aid: the raw pointers and offsets GetPlayer works from, printed without following them.
void PrintRawState();
void PrintUserMessages();
bool SendTestChat(const char *text);
int FindBadEdict(char *detail, size_t size, int &inUse, int &highest);
int NoteEdictChanges(char *out, size_t size);

}  // namespace engine
