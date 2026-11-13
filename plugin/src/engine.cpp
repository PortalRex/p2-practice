#include "engine.h"
#include "perf.h"

#include <windows.h>

#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace engine {
namespace {

using sdk::CallVirtual;

void *g_cvar = nullptr;
void *g_engineClient = nullptr;
void *g_playerInfoManager = nullptr;
void *g_serverGame = nullptr;
void *g_engineServer = nullptr;
int g_scoreboardMessage = -1;  // the index of the ScoreboardTempUpdate user message, -2 when it isn't there
sdk::GlobalVars *g_globals = nullptr;

void *g_commandVtable = nullptr;  // an engine ConCommand's vtable, shared by ours
std::vector<sdk::ConCommand *> g_commands;
sdk::ConVar *g_svCheats = nullptr;

// A cvar HoldCvarAtLeast keeps up, with the lower limit it had before
struct HeldCvar {
	sdk::ConVar *var;
	bool hadMinimum;
	float minimum;
};
std::vector<HeldCvar> g_held;
void ReleaseHeld(const HeldCvar &held) {
	held.var->hasMinimum = held.hadMinimum;
	held.var->minimum = held.minimum;
}

// The hiding flags Init took off cvars, to put back should Init give up
struct UnhiddenCvar {
	sdk::ConVar *var;
	int flags;  // (the ones taken off)
};
std::vector<UnhiddenCvar> g_unhidden;

std::string g_scriptBatch;

// Development trace: with the environment variable P2PRACTICE_LOG set to a file path, everything the plugin
// prints and every command and script it runs is appended there, flushed line by line (so the last line before a
// crash is on disk).
FILE *g_trace = nullptr;

void Trace(const char *kind, const char *text) {
	if (!g_trace) return;
	fprintf(g_trace, "%7d %s %s", g_globals ? g_globals->tickcount : 0, kind, text);
	size_t length = strlen(text);
	if (!length || text[length - 1] != 10) fputc(10, g_trace);
	fflush(g_trace);
}
// One "script" call carries a command line of at most 512 characters, "script " included
constexpr size_t kScriptLimit = sdk::CCommand::kMaxLength - 8 - 1;

// tier0.dll's console output
struct Color {
	unsigned char r, g, b, a;
};
using ColorMsgFn = void(__cdecl *)(const Color &, const char *, ...);
using MsgFn = void(__cdecl *)(const char *, ...);
ColorMsgFn g_colorMsg = nullptr;
MsgFn g_msg = nullptr;
constexpr Color kPrefixColor{90, 200, 255, 255};
constexpr Color kTextColor{225, 235, 240, 255};
constexpr Color kWarnColor{255, 170, 60, 255};

// Where the fields we read live inside a player entity
struct PlayerOffsets {
	int origin = -1, eyePitch = -1, eyeYaw = -1, velocity = -1, flags = -1, team = -1, viewOffsetZ = -1, lifeState = -1;
	int laggedMovement = -1;  // optional: m_flLaggedMovementValue, the game's own movement speed for that player
	int activeWeapon = -1;  // handle of the weapon in hand (the portal gun)
	int challenge = -1;     // which challenge the player spawned into (0: none)
	int heldObject = -1;    // handle of the object in the player's hands
	int challengeSeconds = -1;  // m_StatsThisLevel.fNumSecondsTaken: the Challenge Mode clock in the corner
} g_player;

// ... and inside a portal gun: handles of the two portals it shoots
struct GunOffsets {
	int primaryPortal = -1, secondaryPortal = -1;
} g_gun;

// ... and inside a prop_dynamic: its pose parameters (an array of floats)
sdk::ServerClass *g_dynamicPropClass = nullptr;
int g_poseParameterOffset = -1;

constexpr int kMaxEdicts = 2048;
constexpr int kEdictChanged = 1 << 0, kEdictFullyChanged = 1 << 8;  // "look at every field of it when sending"
constexpr int kGetClassName = 3;  // IServerNetworkable's slot
constexpr unsigned kHandleIndexMask = 0xFFF;  // an entity handle is its index in the low 12 bits, a serial above

sdk::ServerClass *FindServerClass(void *serverGame, const char *name) {
	auto *cls = CallVirtual<sdk::ServerClass *>(serverGame, sdk::slot::GetAllServerClasses);
	for (; cls; cls = cls->next)
		if (cls->networkName && !strcmp(cls->networkName, name)) return cls;
	return nullptr;
}

// A networked field's offset inside its entity, by the name the game networks it under. Send tables nest
// (base classes, embedded structs); offsets add up along the way.
int FindPropOffset(const sdk::SendTable *table, const char *name, int depth = 0) {
	if (!table || !table->props || depth > 12) return -1;
	for (int i = 0; i < table->propCount; i++) {
		const sdk::SendProp &prop = table->props[i];
		if (prop.type == sdk::kPropDataTable) {
			int inner = FindPropOffset(prop.dataTable, name, depth + 1);
			if (inner >= 0) return prop.offset + inner;
		} else if (prop.varName && !strcmp(prop.varName, name)) {
			// Portal 2 keeps the field's offset in the low 16 bits; float and vector props carry other data above
			// them (seen: 0x30.... on floats, 0x50.... on vectors, nothing on ints)
			return prop.offset & 0xFFFF;
		}
	}
	return -1;
}

// The offset of a networked array's first element. An array is networked as a table of its own, named after
// the field, holding one entry per element.
int FindArrayOffset(const sdk::SendTable *table, const char *name, int depth = 0) {
	if (!table || !table->props || depth > 12) return -1;
	for (int i = 0; i < table->propCount; i++) {
		const sdk::SendProp &prop = table->props[i];
		if (prop.type != sdk::kPropDataTable || !prop.dataTable) continue;
		if (prop.varName && !strcmp(prop.varName, name) && prop.dataTable->propCount > 0) return prop.offset + (prop.dataTable->props[0].offset & 0xFFFF);
		int inner = FindArrayOffset(prop.dataTable, name, depth + 1);
		if (inner >= 0) return prop.offset + inner;
	}
	return -1;
}

void FindPropFields(void *serverGame) {
	g_dynamicPropClass = FindServerClass(serverGame, "CDynamicProp");
	if (g_dynamicPropClass) g_poseParameterOffset = FindArrayOffset(g_dynamicPropClass->table, "m_flPoseParameter");
}

bool FindPlayerOffsets(void *serverGame) {
	sdk::ServerClass *cls = FindServerClass(serverGame, "CPortal_Player");
	if (!cls) return false;
	// Optional: without these the trainer can't tell which portals are a player's own
	g_player.activeWeapon = FindPropOffset(cls->table, "m_hActiveWeapon");
	if (sdk::ServerClass *gun = FindServerClass(serverGame, "CWeaponPortalgun")) {
		g_gun.primaryPortal = FindPropOffset(gun->table, "m_hPrimaryPortal");
		g_gun.secondaryPortal = FindPropOffset(gun->table, "m_hSecondaryPortal");
	}
	g_player.challenge = FindPropOffset(cls->table, "m_iBonusChallenge");
	g_player.heldObject = FindPropOffset(cls->table, "m_hAttachedObject");
	g_player.origin = FindPropOffset(cls->table, "m_vecOrigin");
	g_player.eyePitch = FindPropOffset(cls->table, "m_angEyeAngles[0]");
	g_player.eyeYaw = FindPropOffset(cls->table, "m_angEyeAngles[1]");
	g_player.velocity = FindPropOffset(cls->table, "m_vecVelocity[0]");
	g_player.flags = FindPropOffset(cls->table, "m_fFlags");
	g_player.team = FindPropOffset(cls->table, "m_iTeamNum");
	g_player.viewOffsetZ = FindPropOffset(cls->table, "m_vecViewOffset[2]");
	g_player.lifeState = FindPropOffset(cls->table, "m_lifeState");
	g_player.laggedMovement = FindPropOffset(cls->table, "m_flLaggedMovementValue");
	g_player.challengeSeconds = FindPropOffset(cls->table, "fNumSecondsTaken");
	return g_player.origin >= 0 && g_player.eyePitch >= 0 && g_player.eyeYaw >= 0 && g_player.velocity >= 0 &&
	       g_player.flags >= 0 && g_player.team >= 0 && g_player.viewOffsetZ >= 0 && g_player.lifeState >= 0;
}

void *FindCommandBase(const char *name) {
	return g_cvar ? CallVirtual<void *>(g_cvar, sdk::slot::FindCommandBase, name) : nullptr;
}

bool IsCommand(void *commandBase) {
	return CallVirtual<bool>(commandBase, 1);
}

sdk::ConVar *FindVar(const char *name) {
	void *base = FindCommandBase(name);
	if (!base || IsCommand(base)) return nullptr;
	auto *var = static_cast<sdk::ConVar *>(base);
	return var->parent ? var->parent : var;
}

void Output(const Color &color, const char *format, va_list args) {
	char text[2048];
	vsnprintf(text, sizeof(text), format, args);
	Trace("print ", text);
	if (g_colorMsg) {
		g_colorMsg(kPrefixColor, "[practice] ");
		g_colorMsg(color, "%s", text);
	} else if (g_msg) {
		g_msg("[practice] %s", text);
	}
}

// Splits a command line into arguments the way the console does (whitespace, "quoted strings").
bool Tokenize(sdk::CCommand &cmd, const char *name, const char *arguments) {
	int written = snprintf(cmd.argSBuffer, sizeof(cmd.argSBuffer), "%s %s", name, arguments);
	if (written < 0 || written >= static_cast<int>(sizeof(cmd.argSBuffer))) return false;
	cmd.argv0Size = static_cast<int>(strlen(name)) + 1;
	cmd.argc = 0;
	char *out = cmd.argvBuffer;
	char *const end = cmd.argvBuffer + sizeof(cmd.argvBuffer);
	const char *in = cmd.argSBuffer;
	while (*in && cmd.argc < sdk::CCommand::kMaxArgc) {
		while (*in == ' ' || *in == '\t') in++;
		if (!*in) break;
		bool quoted = *in == '"';
		if (quoted) in++;
		cmd.argv[cmd.argc++] = out;
		while (*in && (quoted ? *in != '"' : (*in != ' ' && *in != '\t'))) {
			if (out + 1 >= end) return false;
			*out++ = *in++;
		}
		if (quoted && *in == '"') in++;
		if (out >= end) return false;
		*out++ = 0;
	}
	return cmd.argc > 0;
}

// Takes the flags that hide a cvar from the console off it, remembering which of them it had
void Unhide(sdk::ConVar *var) {
	constexpr int kFcvarDevelopmentOnly = 1 << 1, kFcvarHidden = 1 << 4;
	int hiding = var->flags & (kFcvarDevelopmentOnly | kFcvarHidden);
	if (!hiding) return;
	var->flags &= ~hiding;
	g_unhidden.push_back({var, hiding});
}

// Only for an Init that gives up. Unloading leaves the cvars usable: SAR makes the same ones usable, and when it
// was loaded after this plugin it found them so already and relies on that from then on.
void HideCvarsAgain() {
	for (const UnhiddenCvar &unhidden : g_unhidden) unhidden.var->flags |= unhidden.flags;
	g_unhidden.clear();
}

void CloseTrace() {
	if (g_trace) fclose(g_trace);
	g_trace = nullptr;
}

// Init gave up: nothing it changed or opened stays that way
bool FailInit() {
	HideCvarsAgain();
	CloseTrace();
	return false;
}

void Dispatch(sdk::ConCommand *command, const sdk::CCommand &args) {
	if (command->usingNewCommandCallback && !command->usingCommandCallbackInterface && command->callback)
		command->callback(args);
	else
		CallVirtual<void, const sdk::CCommand &>(command, 14, args);  // ConCommand::Dispatch
}

}  // namespace

bool Init(sdk::CreateInterfaceFn interfaceFactory, sdk::CreateInterfaceFn serverFactory) {
	if (const char *path = getenv("P2PRACTICE_LOG")) g_trace = fopen(path, "a");
	if (HMODULE tier0 = GetModuleHandleA("tier0.dll")) {
		g_colorMsg = reinterpret_cast<ColorMsgFn>(GetProcAddress(tier0, "?ConColorMsg@@YAXABVColor@@PBDZZ"));
		g_msg = reinterpret_cast<MsgFn>(GetProcAddress(tier0, "Msg"));
	}
	g_cvar = interfaceFactory(sdk::kCvar, nullptr);
	g_engineClient = interfaceFactory(sdk::kEngineClient, nullptr);
	g_engineServer = interfaceFactory(sdk::kEngineServer, nullptr);  // (optional: without it the finish board keeps its "-")
	void *serverGame = g_serverGame = serverFactory(sdk::kServerGameDLL, nullptr);
	g_playerInfoManager = serverFactory(sdk::kPlayerInfoManager, nullptr);
	if (!g_cvar || !g_engineClient || !serverGame || !g_playerInfoManager) {
		Warn("This game doesn't offer the interfaces P2 Practice needs (cvar %p, client %p, server %p, players %p).\n",
		     g_cvar, g_engineClient, serverGame, g_playerInfoManager);
		return FailInit();
	}
	g_globals = CallVirtual<sdk::GlobalVars *>(g_playerInfoManager, sdk::slot::GetGlobalVars);

	if (void *listdemo = FindCommandBase("listdemo")) g_commandVtable = *static_cast<void **>(listdemo);
	g_svCheats = FindVar("sv_cheats");
	// Challenge Mode is the cvar sv_bonus_challenge, which the shipped game hides as development-only: the console
	// answers "Unknown command" to it unless something has made it usable (SAR does). Without its two hiding flags
	// it is an ordinary cvar, which the trainer sets when it loads a level in a run's mode.
	// host_thread_mode is hidden the same way; the trainer keeps it off with save states (see SingleThreadedRendering).
	// (They stay usable when the plugin is unloaded: see HideCvarsAgain.)
	for (const char *name : {"sv_bonus_challenge", "host_thread_mode"}) {
		void *base = FindCommandBase(name);
		if (!base || IsCommand(base)) continue;
		auto *var = static_cast<sdk::ConVar *>(base);
		Unhide(var);
		if (var->parent && var->parent != var) Unhide(var->parent);
	}
	if (!g_globals || !g_commandVtable || !g_svCheats || !FindCommandBase("script")) {
		Warn("Not a game P2 Practice understands (no VScript console or cvar system as expected).\n");
		return FailInit();
	}
	if (!FindPlayerOffsets(serverGame)) {
		Warn("Couldn't find the player's networked fields; this game version isn't supported.\n");
		return FailInit();
	}
	FindPropFields(serverGame);  // (optional: without them ghosts aren't animated)
	return true;
}

static void UnhookClientCommands();

void Shutdown() {
	UnhookClientCommands();
	for (sdk::ConCommand *command : g_commands) {
		CallVirtual<void>(g_cvar, sdk::slot::UnregisterConCommand, command);
		delete command;
	}
	g_commands.clear();
	for (const HeldCvar &held : g_held) ReleaseHeld(held);
	g_held.clear();
	g_scriptBatch.clear();
	g_unhidden.clear();
	CloseTrace();
}

void Print(const char *format, ...) {
	va_list args;
	va_start(args, format);
	Output(kTextColor, format, args);
	va_end(args);
}

void Warn(const char *format, ...) {
	va_list args;
	va_start(args, format);
	Output(kWarnColor, format, args);
	va_end(args);
}

void Debug(const char *format, ...) {
	if (!g_trace) return;
	char text[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	Trace("debug ", text);
}

bool RegisterCommand(const char *name, sdk::CommandCallback callback, const char *help, bool serverMayRun) {
	if (!g_cvar || !g_commandVtable || FindCommandBase(name)) return false;
	auto *command = new sdk::ConCommand{};
	command->vtable = g_commandVtable;
	command->name = name;
	command->helpString = help;
	command->callback = callback;
	if (serverMayRun) command->flags |= sdk::kFlagServerCanExecute;
	command->usingNewCommandCallback = true;
	CallVirtual<void>(g_cvar, sdk::slot::RegisterConCommand, command);
	g_commands.push_back(command);
	return true;
}

void Execute(const char *format, ...) {
	char line[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	Trace("exec  ", line);
	if (g_engineClient) CallVirtual<void, const char *>(g_engineClient, sdk::slot::ExecuteClientCmd, line);
}

void ExecuteLater(const char *format, ...) {
	char line[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	Trace("later ", line);
	if (g_engineClient) CallVirtual<void, const char *>(g_engineClient, sdk::slot::ClientCmdUnrestricted, line);
}

bool CallCommand(const char *name, const char *arguments) {
	void *base = FindCommandBase(name);
	if (!base || !IsCommand(base)) return false;
	sdk::CCommand args{};
	if (!Tokenize(args, name, arguments)) return false;
	Trace("call  ", args.argSBuffer);
	Dispatch(static_cast<sdk::ConCommand *>(base), args);
	return true;
}

bool CommandExists(const char *name) {
	void *base = FindCommandBase(name);
	return base && IsCommand(base);
}

bool CheatsOn() {
	return g_svCheats && g_svCheats->intValue != 0;
}

float CvarFloat(const char *name, float fallback) {
	sdk::ConVar *var = FindVar(name);
	return var ? var->floatValue : fallback;
}

std::string CvarString(const char *name) {
	sdk::ConVar *var = FindVar(name);
	return var && var->string ? var->string : "";
}

// The object that holds a cvar's value and limits (a cvar declared in several modules shares one)
sdk::ConVar *ValueOwner(const char *name) {
	sdk::ConVar *var = FindVar(name);
	return var && var->parent ? var->parent : var;
}

void HoldCvarAtLeast(const char *name, float minimum) {
	sdk::ConVar *var = ValueOwner(name);
	if (!var) return;
	bool held = false;
	for (const HeldCvar &h : g_held) held = held || h.var == var;
	if (!held) g_held.push_back({var, var->hasMinimum, var->minimum});
	var->hasMinimum = true;
	var->minimum = minimum;
	if (var->floatValue < minimum) Execute("%s %g", name, minimum);
}

void ReleaseCvar(const char *name) {
	sdk::ConVar *var = ValueOwner(name);
	for (size_t i = 0; i < g_held.size(); i++) {
		if (g_held[i].var != var) continue;
		ReleaseHeld(g_held[i]);
		g_held.erase(g_held.begin() + i);
		return;
	}
}

void Script(const char *format, ...) {
	char statement[sdk::CCommand::kMaxLength];
	va_list args;
	va_start(args, format);
	int length = vsnprintf(statement, sizeof(statement), format, args);
	va_end(args);
	if (length <= 0) return;
	if (static_cast<size_t>(length) > kScriptLimit) {
		Warn("Script statement too long (%d characters), dropped.\n", length);
		return;
	}
	if (g_scriptBatch.size() + length + 1 > kScriptLimit) FlushScript();
	g_scriptBatch.append(statement, length);
	g_scriptBatch.push_back(';');
}

void FlushScript() {
	if (g_scriptBatch.empty()) return;
	void *base = FindCommandBase("script");
	if (base && IsCommand(base)) {
		// The game's handler takes the raw line after "script", so the code goes through untokenized
		sdk::CCommand args{};
		snprintf(args.argSBuffer, sizeof(args.argSBuffer), "script %s", g_scriptBatch.c_str());
		args.argv0Size = 7;
		args.argc = 2;
		args.argv[0] = "script";
		args.argv[1] = args.argSBuffer + args.argv0Size;
		Trace("script", args.argv[1]);
		perf::Scope timed(perf::kScript);
		Dispatch(static_cast<sdk::ConCommand *>(base), args);
	}
	g_scriptBatch.clear();
}

int MaxClients() {
	return g_globals ? g_globals->maxClients : 0;
}

int TickCount() {
	return g_globals ? g_globals->tickcount : 0;
}

float TickInterval() {
	return g_globals && g_globals->intervalPerTick > 0 ? g_globals->intervalPerTick : 1.0f / 60;
}

const char *MapName() {
	return g_globals && g_globals->mapname ? g_globals->mapname : "";
}

// The engine gives the folder in Windows' ANSI code page; the plugin's paths are UTF-8 (std::filesystem::u8path
// throws on anything else), so it is converted here, once for every caller.
std::string GameDirectory() {
	const char *dir = g_engineClient ? CallVirtual<const char *>(g_engineClient, sdk::slot::GetGameDirectory) : nullptr;
	if (!dir || !*dir) return "";
	int wideLength = MultiByteToWideChar(CP_ACP, 0, dir, -1, nullptr, 0);
	if (wideLength <= 0) return "";
	std::vector<wchar_t> wide(static_cast<size_t>(wideLength));
	MultiByteToWideChar(CP_ACP, 0, dir, -1, wide.data(), wideLength);
	int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), -1, nullptr, 0, nullptr, nullptr);
	if (length <= 0) return "";
	std::string utf8(static_cast<size_t>(length), '\0');
	WideCharToMultiByte(CP_UTF8, 0, wide.data(), -1, utf8.data(), length, nullptr, nullptr);
	utf8.resize(static_cast<size_t>(length) - 1);  // (the count included the terminator)
	return utf8;
}

bool RecordingDemo() {
	return g_engineClient && CallVirtual<bool>(g_engineClient, sdk::slot::IsRecordingDemo);
}

// Prints every nested table on the way to a networked field, with the offset each adds.
static bool PrintPropPath(const sdk::SendTable *table, const char *name, int depth = 0) {
	if (!table || !table->props || depth > 12) return false;
	for (int i = 0; i < table->propCount; i++) {
		const sdk::SendProp &prop = table->props[i];
		if (prop.type == sdk::kPropDataTable) {
			if (PrintPropPath(prop.dataTable, name, depth + 1)) {
				Print("  in table %s via \"%s\": offset %d (%#x), flags %#x\n", table->netTableName, prop.varName, prop.offset, prop.offset, prop.flags);
				return true;
			}
		} else if (prop.varName && !strcmp(prop.varName, name)) {
			Print("%s: found in %s at offset %d (%#x), flags %#x\n", name, table->netTableName, prop.offset, prop.offset, prop.flags);
			return true;
		}
	}
	return false;
}

// The entity a handle stored in another entity points at, or null
static const char *EntityFromHandle(const char *owner, int handleOffset, int *index = nullptr) {
	if (!owner || handleOffset < 0 || !g_globals || !g_globals->edicts) return nullptr;
	unsigned handle;
	memcpy(&handle, owner + handleOffset, sizeof(handle));
	if (handle == 0xFFFFFFFF) return nullptr;
	int entIndex = static_cast<int>(handle & kHandleIndexMask);
	if (entIndex <= 0 || entIndex >= kMaxEdicts || entIndex >= g_globals->maxEntities) return nullptr;
	const sdk::Edict &edict = g_globals->edicts[entIndex];
	if (!edict.InUse()) return nullptr;
	if (index) *index = entIndex;
	return static_cast<const char *>(edict.unknown);
}

// The game's own finish board shows the time of a run it has just recorded, which with cheats on is never:
// the board then says "-". The time it shows comes to the client in a user message, ScoreboardTempUpdate
// (two 32-bit numbers: portals placed, time in hundredths; -1 shows "-"), which the plugin sends itself with
// the time the attempt would have made. Sent through the engine's own user message interface, before the
// board opens (it reads the numbers as it opens).
namespace {
// what the engine asks of a recipient filter (after a destructor in the first slot): reliable, not an init
// message, these players
// (Its layout is IRecipientFilter's under the MSVC x86 ABI the game is built with, as every vtable slot number in
// sdk.h is: the plugin is for that one toolchain.)
struct EveryoneFilter {
	virtual ~EveryoneFilter() {}
	virtual bool IsReliable() { return true; }
	virtual bool IsInitMessage() { return false; }
	virtual int GetRecipientCount() { return count; }
	virtual int GetRecipientIndex(int slot) { return slot >= 0 && slot < count ? players[slot] : 0; }
	int count = 0;
	int players[4] = {};
};

struct BitWriter {  // the engine's bf_write, as far as the message needs it
	unsigned char *data;
	int dataBytes, dataBits, curBit;
};

int FindScoreboardMessage() {
	if (g_scoreboardMessage != -1 || !g_serverGame) return g_scoreboardMessage;
	g_scoreboardMessage = -2;
	for (int i = 0; i < 200; i++) {
		char name[256] = "";
		int size = 0;
		if (!CallVirtual<bool, int, char *, int, int &>(g_serverGame, sdk::slot::GetUserMessageInfo, i, name, sizeof(name), size)) break;
		if (!strcmp(name, "ScoreboardTempUpdate")) {
			g_scoreboardMessage = i;
			break;
		}
	}
	return g_scoreboardMessage;
}
}  // namespace

// Development aid: the user messages the game registers, and whether the scoreboard one can be sent
void PrintUserMessages() {
	Print("engine server %p, scoreboard message %d\n", g_engineServer, FindScoreboardMessage());
	for (int i = 0; i < 80 && g_serverGame; i++) {
		char name[256] = "";
		int size = 0;
		if (!CallVirtual<bool, int, char *, int, int &>(g_serverGame, sdk::slot::GetUserMessageInfo, i, name, sizeof(name), size)) break;
		Print("  %d %s (%d)\n", i, name, size);
	}
}

// Sends a user message of `type` with these bytes to every person in the game, reliably
static bool SendUserMessage(int type, const char *name, const void *bytes, int count) {
	if (type < 0 || !g_engineServer || !g_globals) return false;
	EveryoneFilter filter;
	for (int i = 1; i <= g_globals->maxClients && filter.count < 4; i++) {
		PlayerState p;
		if (GetPlayer(i, p) && !p.bot) filter.players[filter.count++] = i;
	}
	if (filter.count == 0) return false;
	auto *bits = CallVirtual<BitWriter *, void *, int, const char *>(g_engineServer, sdk::slot::UserMessageBegin, &filter, type, name);
	if (!bits || !bits->data || bits->curBit % 8 != 0 || bits->curBit / 8 + count > bits->dataBytes) {
		if (bits) CallVirtual<void>(g_engineServer, sdk::slot::MessageEnd);
		return false;
	}
	memcpy(bits->data + bits->curBit / 8, bytes, static_cast<size_t>(count));
	bits->curBit += count * 8;
	CallVirtual<void>(g_engineServer, sdk::slot::MessageEnd);
	return true;
}

bool SendScoreboard(int portals, int hundredths) {
	int payload[2] = {portals, hundredths};
	return SendUserMessage(FindScoreboardMessage(), "ScoreboardTempUpdate", payload, 8);
}

// ---- commands to players' consoles, held back on request ----
// The engine's ClientCommand takes a format and arguments, so it is __cdecl with the interface as its first
// argument (as the game calls it: engine, edict, "%s", text).
namespace {
using ClientCommandFn = void(__cdecl *)(void *self, sdk::Edict *edict, const char *format, ...);
ClientCommandFn g_clientCommand = nullptr;  // the engine's own (or whoever had the slot before)
void **g_clientCommandSlot = nullptr;
ClientCommandFate (*g_holdCommand)(const char *) = nullptr;
struct HeldCommand {
	sdk::Edict *edict;
	std::string text;
};
std::vector<HeldCommand> g_heldCommands;

void __cdecl HookedClientCommand(void *self, sdk::Edict *edict, const char *format, ...) {
	char text[512];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	Trace("client", text);
	ClientCommandFate fate = g_holdCommand ? g_holdCommand(text) : ClientCommandFate::Send;
	if (fate == ClientCommandFate::Drop) return;
	if (fate == ClientCommandFate::Hold) {
		g_heldCommands.push_back({edict, text});
		return;
	}
	g_clientCommand(self, edict, "%s", text);
}

bool WriteSlot(void **slot, void *value) {
	DWORD old;
	if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) return false;
	*slot = value;
	VirtualProtect(slot, sizeof(void *), old, &old);
	return true;
}
}  // namespace

void HoldClientCommands(ClientCommandFate (*hold)(const char *command)) {
	g_holdCommand = hold;
	if (g_clientCommandSlot || !g_engineServer) return;
	void **slot = *static_cast<void ***>(g_engineServer) + sdk::slot::ClientCommand;
	void *original = *slot;
	// (already there after a reload, in a copy of the table another plugin made while the hook was in it: the
	// engine's own function is still the one remembered then, and taking the hook for it would call itself)
	if (original == reinterpret_cast<void *>(&HookedClientCommand)) {
		if (g_clientCommand) g_clientCommandSlot = slot;
		return;
	}
	if (!WriteSlot(slot, reinterpret_cast<void *>(&HookedClientCommand))) return;
	g_clientCommand = reinterpret_cast<ClientCommandFn>(original);
	g_clientCommandSlot = slot;
}

void ReleaseClientCommands() {
	std::vector<HeldCommand> held;
	held.swap(g_heldCommands);
	for (const HeldCommand &command : held) g_clientCommand(g_engineServer, command.edict, "%s", command.text.c_str());
}

size_t HeldClientCommands() { return g_heldCommands.size(); }

void DropHeldClientCommands() { g_heldCommands.clear(); }

void MakeCoroutineList() {
	using CoroutineFunc = void(__cdecl *)(void *);
	using CreateFn = unsigned(__cdecl *)(CoroutineFunc, void *);
	using CancelFn = void(__cdecl *)(unsigned);
	HMODULE vstdlib = GetModuleHandleA("vstdlib.dll");
	if (!vstdlib) return;
	auto create = reinterpret_cast<CreateFn>(GetProcAddress(vstdlib, "Coroutine_Create"));
	auto cancel = reinterpret_cast<CancelFn>(GetProcAddress(vstdlib, "Coroutine_Cancel"));
	if (!create || !cancel) return;
	// (created, never run, dropped again: only the list stays)
	unsigned handle = create([](void *) {}, nullptr);
	if (handle != static_cast<unsigned>(-1)) cancel(handle);
}

// The engine's ClientCommand back in its slot (what was held goes out first), unless someone hooked it over
// ours since
static void UnhookClientCommands() {
	if (g_clientCommandSlot && *g_clientCommandSlot == reinterpret_cast<void *>(&HookedClientCommand)) {
		ReleaseClientCommands();
		WriteSlot(g_clientCommandSlot, reinterpret_cast<void *>(g_clientCommand));
	}
	g_clientCommandSlot = nullptr;
	g_holdCommand = nullptr;
}

// Development aid: a chat line (SayText: sender's entity index, the text, whether it is chat), to see that
// messages sent this way reach the client
bool SendTestChat(const char *text) {
	std::vector<char> payload;
	payload.push_back(1);
	payload.insert(payload.end(), text, text + strlen(text) + 1);
	payload.push_back(1);
	return SendUserMessage(3, "SayText", payload.data(), static_cast<int>(payload.size()));
}

bool EntityExists(const char *className) {
	if (!g_globals || !g_globals->edicts) return false;
	for (int i = g_globals->maxClients + 1; i < g_globals->maxEntities && i < kMaxEdicts; i++) {
		const sdk::Edict &edict = g_globals->edicts[i];
		if (!edict.InUse() || !edict.networkable) continue;
		const char *name = CallVirtual<const char *>(edict.networkable, kGetClassName);
		if (name && !strcmp(name, className)) return true;
	}
	return false;
}

void SetGodMode(int entIndex, bool on) {
	constexpr int kFlagGodMode = 1 << 14;  // FL_GODMODE in Portal 2 (checked against what "god" toggles)
	if (!g_globals || !g_globals->edicts || entIndex < 1 || entIndex > g_globals->maxClients) return;
	sdk::Edict &edict = g_globals->edicts[entIndex];
	if (!edict.InUse()) return;
	char *field = static_cast<char *>(edict.unknown) + g_player.flags;
	int flags;
	memcpy(&flags, field, sizeof(flags));
	flags = on ? flags | kFlagGodMode : flags & ~kFlagGodMode;
	memcpy(field, &flags, sizeof(flags));
}

bool SetMovementSpeed(int entIndex, float scale) {
	if (!g_globals || !g_globals->edicts || g_player.laggedMovement < 0 || entIndex < 1 || entIndex > g_globals->maxClients) return false;
	sdk::Edict &edict = g_globals->edicts[entIndex];
	if (!edict.InUse()) return false;
	char *field = static_cast<char *>(edict.unknown) + g_player.laggedMovement;
	float now;
	memcpy(&now, field, sizeof(now));
	if (now == scale) return true;
	memcpy(field, &scale, sizeof(scale));
	edict.stateFlags |= kEdictChanged | kEdictFullyChanged;  // (the screens' copy of the player has to hear of it)
	return true;
}

bool SnapshotsRestoreDeviceState() {
	HMODULE module = GetModuleHandleA("chocolateloading.dll");
	auto restores = module ? reinterpret_cast<int(__cdecl *)()>(GetProcAddress(module, "ChocoRestoresDeviceState")) : nullptr;
	return restores && restores() != 0;
}

SnapshotStatus SnapshotsStatus() {
	// (ChocolateLoading's own struct: the caller sets its size, so a later build can add to the end)
	struct ChocoSnapshotStatus {
		unsigned size, saved, restored, failed, keepsRenderThread;
	};
	SnapshotStatus status;
	HMODULE module = GetModuleHandleA("chocolateloading.dll");
	auto get = module ? reinterpret_cast<int(__cdecl *)(ChocoSnapshotStatus *)>(GetProcAddress(module, "ChocoGetSnapshotStatus")) : nullptr;
	ChocoSnapshotStatus choco{};
	choco.size = sizeof(choco);
	if (!get || get(&choco) != 1) return status;
	status.available = true;
	status.keepsRenderThread = choco.keepsRenderThread != 0;
	status.saved = choco.saved;
	status.restored = choco.restored;
	status.failed = choco.failed;
	return status;
}

// IShaderAPI::ResetRenderState(bool bFullReset), slot 144 of ShaderApi029 in Portal 2's shader API: the slot holds
// an adjustor thunk (sub ecx,0x248; jmp CShaderAPIDx8::ResetRenderState), checked byte for byte so a different
// build is left alone. The same in shaderapidx9.dll (DirectX 9) and shaderapivk.dll (-vulkan: built from the same
// code, the function identical byte for byte).
bool ResetRenderState() {
	static void *shaderApi = nullptr;
	static bool looked = false;
	if (!looked) {
		looked = true;
		static const unsigned char kThunk[] = {0x81, 0xE9, 0x48, 0x02, 0x00, 0x00, 0xE9};
		for (const char *name : {"shaderapidx9.dll", "shaderapivk.dll"}) {
			HMODULE module = GetModuleHandleA(name);
			using Factory = void *(__cdecl *)(const char *, int *);
			auto factory = module ? reinterpret_cast<Factory>(GetProcAddress(module, "CreateInterface")) : nullptr;
			void *api = factory ? factory("ShaderApi029", nullptr) : nullptr;
			if (api && memcmp((*static_cast<void ***>(api))[144], kThunk, sizeof(kThunk)) == 0) {
				shaderApi = api;
				break;
			}
		}
		if (!shaderApi) Warn("The renderer couldn't be reset after save states (unknown shader API).\n");
	}
	if (!shaderApi) return false;
	reinterpret_cast<void(__thiscall *)(void *, bool)>((*static_cast<void ***>(shaderApi))[144])(shaderApi, true);
	return true;
}

bool SetEyeHeight(int entIndex, float height) {
	if (!g_globals || !g_globals->edicts || g_player.viewOffsetZ < 0 || entIndex < 1 || entIndex > g_globals->maxClients) return false;
	sdk::Edict &edict = g_globals->edicts[entIndex];
	if (!edict.InUse()) return false;
	char *field = static_cast<char *>(edict.unknown) + g_player.viewOffsetZ;
	float now;
	memcpy(&now, field, sizeof(now));
	if (now == height) return true;
	memcpy(field, &height, sizeof(height));
	edict.stateFlags |= kEdictChanged | kEdictFullyChanged;
	return true;
}

bool SetChallengeSeconds(int entIndex, float seconds) {
	if (!g_globals || !g_globals->edicts || g_player.challengeSeconds < 0 || entIndex < 1 || entIndex > g_globals->maxClients) return false;
	sdk::Edict &edict = g_globals->edicts[entIndex];
	if (!edict.InUse()) return false;
	char *field = static_cast<char *>(edict.unknown) + g_player.challengeSeconds;
	memcpy(field, &seconds, sizeof(seconds));
	edict.stateFlags |= kEdictChanged | kEdictFullyChanged;
	return true;
}

bool GetPlayerPortals(int playerEntIndex, int &primary, int &secondary) {
	primary = secondary = -1;
	if (!g_globals || !g_globals->edicts || playerEntIndex < 1 || playerEntIndex > g_globals->maxClients) return false;
	const sdk::Edict &edict = g_globals->edicts[playerEntIndex];
	if (!edict.InUse() || g_gun.primaryPortal < 0 || g_gun.secondaryPortal < 0) return false;
	const char *gun = EntityFromHandle(static_cast<const char *>(edict.unknown), g_player.activeWeapon);
	if (!gun) return false;
	EntityFromHandle(gun, g_gun.primaryPortal, &primary);
	EntityFromHandle(gun, g_gun.secondaryPortal, &secondary);
	return primary > 0 || secondary > 0;
}

bool SetPoseParameters(int entIndex, const float values[kPoseParameters]) {
	constexpr int kGetServerClass = 1;  // IServerNetworkable
	if (!g_globals || !g_globals->edicts || g_poseParameterOffset < 0) return false;
	if (entIndex <= g_globals->maxClients || entIndex >= kMaxEdicts || entIndex >= g_globals->maxEntities) return false;
	sdk::Edict &edict = g_globals->edicts[entIndex];
	if (!edict.InUse() || !edict.networkable) return false;
	if (CallVirtual<sdk::ServerClass *>(edict.networkable, kGetServerClass) != g_dynamicPropClass) return false;
	char *field = static_cast<char *>(edict.unknown) + g_poseParameterOffset;
	if (!memcmp(field, values, sizeof(float) * kPoseParameters)) return true;
	memcpy(field, values, sizeof(float) * kPoseParameters);
	edict.stateFlags |= kEdictChanged | kEdictFullyChanged;
	return true;
}

// Development aid: the edict table checked the way the engine's entity packing walks it (every edict in use
// has its entity and networkable followed): the first one whose pointers can't be read, described, or -1.
// `inUse` and `highest` count the table. (After a memory-snapshot restore a corrupted edict shows up here
// before the engine trips over it.)
int FindBadEdict(char *detail, size_t size, int &inUse, int &highest) {
	inUse = 0;
	highest = -1;
	if (!g_globals || !g_globals->edicts) return -1;
	for (int i = 0; i < g_globals->maxEntities && i < kMaxEdicts; i++) {
		const sdk::Edict &edict = g_globals->edicts[i];
		if ((edict.stateFlags & sdk::kEdictFree) || !edict.unknown) continue;
		inUse++;
		highest = i;
		bool badEntity = IsBadReadPtr(edict.unknown, 8);
		bool badNetworkable = !edict.networkable || IsBadReadPtr(edict.networkable, 8) || IsBadReadPtr(*static_cast<void *const *>(edict.networkable), 8);
		if (badEntity || badNetworkable) {
			snprintf(detail, size, "edict %d: flags %08x serial %d networkable %p entity %p (%s)", i, edict.stateFlags, edict.networkSerialNumber,
			         edict.networkable, edict.unknown, badEntity ? "entity unreadable" : "networkable unreadable");
			return i;
		}
	}
	return -1;
}

// Development aid: the edicts that came and went since the last call, as "+139 prop_dynamic@054E3E10" and
// "-139 prop_dynamic" entries in `out` (how many, or 0). The first call only takes stock.
namespace {
struct EdictMark {
	int serial = 0;
	const void *entity = nullptr;
	char cls[40] = "";
};
EdictMark g_marks[kMaxEdicts];
bool g_marksTaken = false;
}  // namespace

int NoteEdictChanges(char *out, size_t size) {
	if (!g_globals || !g_globals->edicts) return 0;
	int changes = 0;
	size_t used = 0;
	out[0] = 0;
	for (int i = 0; i < g_globals->maxEntities && i < kMaxEdicts; i++) {
		const sdk::Edict &edict = g_globals->edicts[i];
		bool inUse = !(edict.stateFlags & sdk::kEdictFree) && edict.unknown;
		EdictMark now;
		if (inUse) {
			now.serial = edict.networkSerialNumber;
			now.entity = edict.unknown;
			const char *name = edict.networkable && !IsBadReadPtr(edict.networkable, 8) && !IsBadReadPtr(*static_cast<void *const *>(edict.networkable), 16)
			                       ? CallVirtual<const char *>(edict.networkable, kGetClassName) : nullptr;
			snprintf(now.cls, sizeof(now.cls), "%s", name && !IsBadReadPtr(name, 4) ? name : "?");
		}
		EdictMark &was = g_marks[i];
		if (g_marksTaken && (inUse != (was.entity != nullptr) || (inUse && (now.serial != was.serial || now.entity != was.entity)))) {
			changes++;
			if (used < size - 1) {
				if (was.entity) used += snprintf(out + used, size - used, " -%d %s", i, was.cls);
				if (inUse && used < size - 1) used += snprintf(out + used, size - used, " +%d %s@%p", i, now.cls, now.entity);
				if (used > size - 1) used = size - 1;
			}
		}
		was = now;
	}
	g_marksTaken = true;
	return changes;
}

void PrintRawState() {
	Print("prop_dynamic pose parameters at %d\n", g_poseParameterOffset);
	Print("portal gun fields: active weapon %d, primary portal %d, secondary portal %d\n", g_player.activeWeapon, g_gun.primaryPortal, g_gun.secondaryPortal);
	for (int i = 1; i <= MaxClients(); i++) {
		int primary, secondary;
		bool found = GetPlayerPortals(i, primary, secondary);
		Print("player %d portals: %s, entities %d and %d\n", i, found ? "found" : "none", primary, secondary);
	}
	if (g_serverGame) {
		sdk::ServerClass *cls = FindServerClass(g_serverGame, "CPortal_Player");
		if (cls)
			for (const char *name : {"m_vecOrigin", "m_angEyeAngles[0]", "m_vecVelocity[0]", "m_vecViewOffset[2]", "m_fFlags"})
				PrintPropPath(cls->table, name);
	}
	Print("globals %p, edicts %p, maxClients %d, tick %d\n", g_globals, g_globals ? g_globals->edicts : nullptr, MaxClients(), TickCount());
	Print("player field offsets: origin %d, eye %d %d, velocity %d, flags %d, team %d, view offset %d, life %d\n", g_player.origin,
	      g_player.eyePitch, g_player.eyeYaw, g_player.velocity, g_player.flags, g_player.team, g_player.viewOffsetZ, g_player.lifeState);
	for (int i = 1; i <= MaxClients(); i++) {
		PlayerState state;
		if (GetPlayer(i, state)) Print("player %d: challenge field at %d, challenge mode %s\n", i, g_player.challenge, state.challenge ? "on" : "off");
	}
	if (!g_globals || !g_globals->edicts) return;
	for (int i = 0; i <= MaxClients() && i < 34; i++) {
		const sdk::Edict &edict = g_globals->edicts[i];
		Print("edict %d: flags %#x, serial %d, networkable %p, entity %p\n", i, edict.stateFlags, edict.networkSerialNumber, edict.networkable,
		      edict.unknown);
	}
}

bool GetPlayer(int entIndex, PlayerState &out) {
	if (!g_globals || !g_globals->edicts || entIndex < 1 || entIndex > g_globals->maxClients) return false;
	sdk::Edict &edict = g_globals->edicts[entIndex];
	if (!edict.InUse()) return false;
	const char *entity = static_cast<const char *>(edict.unknown);
	auto read = [entity](int offset, void *to, size_t size) { memcpy(to, entity + offset, size); };
	read(g_player.origin, &out.origin, sizeof(Vec3));
	read(g_player.velocity, &out.velocity, sizeof(Vec3));
	read(g_player.eyePitch, &out.pitch, sizeof(float));
	read(g_player.eyeYaw, &out.yaw, sizeof(float));
	read(g_player.viewOffsetZ, &out.viewOffsetZ, sizeof(float));
	read(g_player.flags, &out.flags, sizeof(int));
	read(g_player.team, &out.team, sizeof(int));
	unsigned char lifeState = 0;
	read(g_player.lifeState, &lifeState, 1);
	out.alive = lifeState == 0;
	int challenge = 0;
	if (g_player.challenge >= 0) read(g_player.challenge, &challenge, sizeof(int));
	out.challenge = challenge != 0;
	out.held = -1;
	if (g_player.heldObject >= 0) EntityFromHandle(entity, g_player.heldObject, &out.held);
	out.bot = false;
	if (void *info = CallVirtual<void *, sdk::Edict *>(g_playerInfoManager, sdk::slot::GetPlayerInfo, &edict)) {
		const char *id = CallVirtual<const char *>(info, sdk::slot::GetNetworkIDString);
		out.bot = id && !strcmp(id, "BOT");
	}
	return true;
}

// ---- the use key ----
namespace {

// A class's save description (the game's datamap_t and typedescription_t, as far as a field's name and offset):
// every field the game saves, by name, base classes linked. Used only to find where a few fields live that the
// game doesn't network.
struct DataField {
	int type;
	const char *name;
	int offset;
	char rest[0x3C - 12];
};
struct DataMap {
	DataField *fields;
	int count;
	const char *className;
	DataMap *base;
};

// An entity's save description. The entity's GetDataDescMap is "mov eax, <map>; ret" in every class: anything
// else there is not the function expected, and nothing is called.
const DataMap *DataMapOf(const void *entity) {
	constexpr int kGetDataDescMap = 11;
	const auto *code = static_cast<const unsigned char *>((*static_cast<void **const *>(entity))[kGetDataDescMap]);
	if (IsBadReadPtr(code, 6) || code[0] != 0xB8 || code[5] != 0xC3) return nullptr;
	const DataMap *map;
	memcpy(&map, code + 1, sizeof(map));
	return IsBadReadPtr(map, sizeof(DataMap)) ? nullptr : map;
}

int FindDataField(const void *entity, const char *name) {
	int depth = 0;
	for (const DataMap *map = DataMapOf(entity); map && depth < 16; map = map->base, depth++) {
		if (map->count < 0 || map->count > 4096 || (map->count && IsBadReadPtr(map->fields, sizeof(DataField) * map->count))) return -1;
		for (int i = 0; i < map->count; i++)
			if (map->fields[i].name && !strcmp(map->fields[i].name, name)) return map->fields[i].offset;
	}
	return -1;
}

struct UseOffsets {
	bool looked = false;
	int buttonsPressed = -1, startTime = -1, cooldownTime = -1, lockOutTime = -1;
	int grabPortal = -1;  // m_hPortalThroughWhichGrabOccured, which the game's search writes
	int absOrigin = -1;   // m_vecAbsOrigin: where the player is in the world
	// The angles the player looks along (v_angle of the player's state, which the game doesn't network or save:
	// it is the field after that state's networked "deadflag", as in the public SDK's playerstate.h). Used only
	// while it reads the same as the networked eye angles.
	int viewAngles = -1;
} g_use;

void FindUseOffsets(const void *player) {
	if (g_use.looked) return;
	g_use.looked = true;
	g_use.buttonsPressed = FindDataField(player, "m_afButtonPressed");
	g_use.startTime = FindDataField(player, "m_flUseKeyStartTime");
	g_use.cooldownTime = FindDataField(player, "m_flUseKeyCooldownTime");
	g_use.lockOutTime = FindDataField(player, "m_flAutoGrabLockOutTime");
	g_use.grabPortal = FindDataField(player, "m_hPortalThroughWhichGrabOccured");
	g_use.absOrigin = FindDataField(player, "m_vecAbsOrigin");
	if (sdk::ServerClass *cls = FindServerClass(g_serverGame, "CPortal_Player")) {
		int deadFlag = FindPropOffset(cls->table, "deadflag");
		if (deadFlag >= 0) g_use.viewAngles = deadFlag + 4;
	}
}

// The game's search for something to use (CPortal_Player's, which the use key runs: one argument, where it
// puts the portal it reached through). It has no name to look up, but it is the one function of server.dll
// that reads the cvar sv_player_use_cone_size straight from its static object: that read is looked for, and
// the function is the one it sits in (its first bytes are those of a function that aligns its stack).
using FindUseEntityFn = void *(__thiscall *)(void *player, void **throughPortal);
FindUseEntityFn g_findUseEntity = nullptr;
bool g_lookedForFindUseEntity = false;

// Where `bytes` first sits in a loaded module's sections of one kind (code, or everything else), or null
const unsigned char *FindInModule(HMODULE module, bool code, const void *bytes, size_t count, const unsigned char **sectionStart = nullptr) {
	const auto *base = reinterpret_cast<const unsigned char *>(module);
	const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
	const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
	const IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
	const unsigned char first = *static_cast<const unsigned char *>(bytes);
	for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
		if (((section[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0) != code || (section[i].Characteristics & IMAGE_SCN_MEM_DISCARDABLE)) continue;
		const unsigned char *start = base + section[i].VirtualAddress;
		size_t size = section[i].Misc.VirtualSize;
		for (size_t at = 0; at + count <= size; at++) {
			if (start[at] != first || memcmp(start + at, bytes, count)) continue;
			if (sectionStart) *sectionStart = start;
			return start + at;
		}
	}
	return nullptr;
}

FindUseEntityFn LocateFindUseEntity() {
	HMODULE server = GetModuleHandleA("server.dll");
	if (!server) return nullptr;
	// server.dll's own object of the cvar (the game has one in client.dll too, and the cvar system hands out
	// whichever was registered first): the object whose name pointer is server.dll's copy of the name
	static const char kName[] = "sv_player_use_cone_size";
	const unsigned char *name = FindInModule(server, false, kName, sizeof(kName));
	const unsigned char *namePointer = name ? FindInModule(server, false, &name, sizeof(name)) : nullptr;
	if (!namePointer) return nullptr;
	const auto *cone = reinterpret_cast<const sdk::ConVar *>(namePointer - offsetof(sdk::ConVar, name));
	if (IsBadReadPtr(cone, sizeof(sdk::ConVar)) || !cone->parent) return nullptr;
	const unsigned char *text = nullptr;
	// "mov eax, [the cvar's parent pointer]", once in the whole of the code
	unsigned char read[5] = {0xA1};
	sdk::ConVar *const *parent = &cone->parent;
	memcpy(read + 1, &parent, 4);
	const unsigned char *found = FindInModule(server, true, read, sizeof(read), &text);
	if (!found || !text) return nullptr;
	// push ebx; mov ebx, esp; sub esp, 8; and esp, -16
	static const unsigned char kStart[] = {0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08, 0x83, 0xE4, 0xF0};
	constexpr ptrdiff_t kLongest = 0x1000;  // (the read is 0x6A4 into the function in the game this was written for)
	for (const unsigned char *at = found - (reinterpret_cast<uintptr_t>(found) & 15); at >= text && found - at < kLongest; at -= 16)
		if (!memcmp(at, kStart, sizeof(kStart)) && (at == text || at[-1] == 0xCC || at[-1] == 0xC3))
			return reinterpret_cast<FindUseEntityFn>(const_cast<unsigned char *>(at));
	return nullptr;
}

struct BoxOffsets {
	bool looked = false;
	int origin = -1, angles = -1, mins = -1, maxs = -1;
} g_box;

}  // namespace

bool GetUseKey(int entIndex, UseKey &out) {
	if (!g_globals || !g_globals->edicts || entIndex < 1 || entIndex > g_globals->maxClients) return false;
	const sdk::Edict &edict = g_globals->edicts[entIndex];
	if (!edict.InUse()) return false;
	const char *player = static_cast<const char *>(edict.unknown);
	FindUseOffsets(player);
	if (g_use.buttonsPressed < 0 || g_use.startTime < 0 || g_use.cooldownTime < 0 || g_use.lockOutTime < 0) return false;
	int pressed = 0;
	memcpy(&pressed, player + g_use.buttonsPressed, sizeof(pressed));
	out.pressed = (pressed & p2demo::IN_USE) != 0;
	memcpy(&out.startTime, player + g_use.startTime, sizeof(float));
	// (two clocks: one the game sets ahead to switch the key off, and the time the key last picked an object up,
	// after which it picks nothing up for a second: an object let go of at once can't be caught again)
	constexpr float kLockOut = 1.0f;
	float cooldown = 0, lockOut = 0;
	memcpy(&cooldown, player + g_use.cooldownTime, sizeof(float));
	memcpy(&lockOut, player + g_use.lockOutTime, sizeof(float));
	out.lockedUntil = cooldown > lockOut + kLockOut ? cooldown : lockOut + kLockOut;
	out.now = g_globals->curtime;
	return true;
}

int FindUseTarget(int entIndex, const UseView *view) {
	if (!g_globals || !g_globals->edicts || entIndex < 1 || entIndex > g_globals->maxClients) return -1;
	const sdk::Edict &edict = g_globals->edicts[entIndex];
	if (!edict.InUse()) return -1;
	char *player = static_cast<char *>(edict.unknown);
	FindUseOffsets(player);
	if (!g_lookedForFindUseEntity) {
		g_lookedForFindUseEntity = true;
		g_findUseEntity = LocateFindUseEntity();
		Debug("use search: %p, grab portal field %d\n", g_findUseEntity, g_use.grabPortal);
	}
	if (!g_findUseEntity || g_use.grabPortal < 0 || (view && (g_use.absOrigin < 0 || g_use.viewAngles < 0))) return -1;
	// (the search notes the portal it looked through in the player; the use key's own code clears that again)
	unsigned grabPortal;
	memcpy(&grabPortal, player + g_use.grabPortal, sizeof(grabPortal));
	Vec3 origin;
	float angles[2], eyeAngles[2];
	if (view) {
		memcpy(&origin, player + g_use.absOrigin, sizeof(origin));
		memcpy(angles, player + g_use.viewAngles, sizeof(angles));
		memcpy(&eyeAngles[0], player + g_player.eyePitch, sizeof(float));
		memcpy(&eyeAngles[1], player + g_player.eyeYaw, sizeof(float));
		auto apart = [](float a, float b) { return std::fabs(std::remainder(a - b, 360.0f)) > 0.5f; };
		if (apart(angles[0], eyeAngles[0]) || apart(angles[1], eyeAngles[1])) return -1;  // (not the field it was taken for)
		Vec3 there = {origin.x + view->moved.x, origin.y + view->moved.y, origin.z + view->moved.z};
		float look[2] = {view->pitch, view->yaw};
		memcpy(player + g_use.absOrigin, &there, sizeof(there));
		memcpy(player + g_use.viewAngles, look, sizeof(look));
	}
	void *throughPortal = nullptr;
	const void *found = g_findUseEntity(player, &throughPortal);
	if (view) {
		memcpy(player + g_use.absOrigin, &origin, sizeof(origin));
		memcpy(player + g_use.viewAngles, angles, sizeof(angles));
	}
	memcpy(player + g_use.grabPortal, &grabPortal, sizeof(grabPortal));
	if (!found) return 0;
	for (int i = 1; i < EntityLimit(); i++)
		if (g_globals->edicts[i].unknown == found && g_globals->edicts[i].InUse()) return i;
	return kUseTargetUnnamed;
}

int EntityLimit() { return g_globals ? (g_globals->maxEntities < kMaxEdicts ? g_globals->maxEntities : kMaxEdicts) : 0; }

bool GetEntityBox(int entIndex, EntityBox &out) {
	if (!g_globals || !g_globals->edicts || entIndex < 1 || entIndex >= EntityLimit()) return false;
	if (!g_box.looked) {
		g_box.looked = true;
		if (sdk::ServerClass *cls = FindServerClass(g_serverGame, "CBaseEntity")) {
			g_box.origin = FindPropOffset(cls->table, "m_vecOrigin");
			g_box.angles = FindPropOffset(cls->table, "m_angRotation");
			g_box.mins = FindPropOffset(cls->table, "m_vecMins");
			g_box.maxs = FindPropOffset(cls->table, "m_vecMaxs");
		}
	}
	if (g_box.origin < 0 || g_box.angles < 0 || g_box.mins < 0 || g_box.maxs < 0) return false;
	const sdk::Edict &edict = g_globals->edicts[entIndex];
	if (!edict.InUse() || !edict.networkable) return false;
	const char *entity = static_cast<const char *>(edict.unknown);
	const char *name = CallVirtual<const char *>(edict.networkable, kGetClassName);
	out.className = name ? name : "";
	float angles[3];
	memcpy(&out.origin, entity + g_box.origin, sizeof(Vec3));
	memcpy(angles, entity + g_box.angles, sizeof(angles));
	out.pitch = angles[0];
	out.yaw = angles[1];
	out.roll = angles[2];
	memcpy(&out.mins, entity + g_box.mins, sizeof(Vec3));
	memcpy(&out.maxs, entity + g_box.maxs, sizeof(Vec3));
	return true;
}

}  // namespace engine
