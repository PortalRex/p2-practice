#include "engine.h"

#include <windows.h>

#include <cstdarg>
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
} g_player;

// ... and inside a portal gun: handles of the two portals it shoots
struct GunOffsets {
	int primaryPortal = -1, secondaryPortal = -1;
} g_gun;

// ... and inside a prop_dynamic: its pose parameters (an array of floats)
sdk::ServerClass *g_dynamicPropClass = nullptr;
int g_poseParameterOffset = -1;

constexpr int kMaxEdicts = 2048;
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
		return false;
	}
	g_globals = CallVirtual<sdk::GlobalVars *>(g_playerInfoManager, sdk::slot::GetGlobalVars);

	if (void *listdemo = FindCommandBase("listdemo")) g_commandVtable = *static_cast<void **>(listdemo);
	g_svCheats = FindVar("sv_cheats");
	// Challenge Mode is the cvar sv_bonus_challenge, which the shipped game hides as development-only: the console
	// answers "Unknown command" to it unless something has made it usable (SAR does). Without its two hiding flags
	// it is an ordinary cvar, which the trainer sets when it loads a level in a run's mode.
	constexpr int kFcvarDevelopmentOnly = 1 << 1, kFcvarHidden = 1 << 4;
	if (void *base = FindCommandBase("sv_bonus_challenge"); base && !IsCommand(base)) {
		auto *var = static_cast<sdk::ConVar *>(base);
		var->flags &= ~(kFcvarDevelopmentOnly | kFcvarHidden);
		if (var->parent) var->parent->flags &= ~(kFcvarDevelopmentOnly | kFcvarHidden);
	}
	if (!g_globals || !g_commandVtable || !g_svCheats || !FindCommandBase("script")) {
		Warn("Not a game P2 Practice understands (no VScript console or cvar system as expected).\n");
		return false;
	}
	if (!FindPlayerOffsets(serverGame)) {
		Warn("Couldn't find the player's networked fields; this game version isn't supported.\n");
		return false;
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
	g_scriptBatch.clear();
	if (g_trace) fclose(g_trace);
	g_trace = nullptr;
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

bool RegisterCommand(const char *name, sdk::CommandCallback callback, const char *help) {
	if (!g_cvar || !g_commandVtable || FindCommandBase(name)) return false;
	auto *command = new sdk::ConCommand{};
	command->vtable = g_commandVtable;
	command->name = name;
	command->helpString = help;
	command->callback = callback;
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

std::string GameDirectory() {
	const char *dir = g_engineClient ? CallVirtual<const char *>(g_engineClient, sdk::slot::GetGameDirectory) : nullptr;
	return dir ? dir : "";
}

// Prints every nested table on the way to a networked field, with the offset each adds.
bool PrintPropPath(const sdk::SendTable *table, const char *name, int depth = 0) {
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
	constexpr int kGetClassName = 3;  // IServerNetworkable
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
	constexpr int kEdictChanged = 1 << 0, kEdictFullyChanged = 1 << 8;  // "look at every field of it when sending"
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
	constexpr int kEdictChanged = 1 << 0, kEdictFullyChanged = 1 << 8;  // "look at every field of it when sending"
	constexpr int kGetServerClass = 1;                                  // IServerNetworkable
	if (!g_globals || !g_globals->edicts || g_poseParameterOffset < 0 || entIndex <= g_globals->maxClients || entIndex >= kMaxEdicts) return false;
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
	constexpr int kGetClassName = 3;  // IServerNetworkable
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

}  // namespace engine
