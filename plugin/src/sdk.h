// The small part of the Source engine's binary interface this plugin relies on.
//
// Portal 2 has no public SDK, so these layouts are declared by hand. They follow the Alien Swarm SDK (the nearest
// public branch) and were checked against Portal 2 build 10090; member offsets and vtable slots that SourceAutoRecord
// (MIT, https://github.com/p2sr/SourceAutoRecord) also uses are marked "SAR". Nothing here hooks or patches the game:
// everything is reached through CreateInterface factories and the plugin callbacks the engine itself calls.
#pragma once

#include <cstdint>

namespace sdk {

using CreateInterfaceFn = void *(*)(const char *name, int *returnCode);

// Calls virtual function `index` of a game object.
template <typename Ret, typename... Args>
inline Ret CallVirtual(void *object, int index, Args... args) {
	using Fn = Ret(__thiscall *)(void *, Args...);
	return (*reinterpret_cast<Fn **>(object))[index](object, args...);
}

// ---- interface versions ----
constexpr const char *kPluginCallbacks = "ISERVERPLUGINCALLBACKS003";
constexpr const char *kCvar = "VEngineCvar007";
constexpr const char *kEngineClient = "VEngineClient015";
constexpr const char *kServerGameDLL = "ServerGameDLL005";
constexpr const char *kEngineServer = "VEngineServer022";
constexpr const char *kPlayerInfoManager = "PlayerInfoManager002";

// ---- vtable slots (Windows) ----
namespace slot {
// ICvar (SAR)
constexpr int RegisterConCommand = 9;
constexpr int UnregisterConCommand = 10;
constexpr int FindCommandBase = 13;
// IVEngineClient (SAR)
constexpr int GetGameDirectory = 35;
constexpr int ExecuteClientCmd = 104;
// IServerGameDLL (SAR)
constexpr int GetAllServerClasses = 10;
constexpr int GetUserMessageInfo = 28;   // (the engine's MessageEnd calls it through this slot)
// IVEngineServer (the engine's vtable: slots 44 and 45 start and end a user message; 39 is ClientCommand,
// "void ClientCommand(edict_t *, const char *format, ...)", which the server's game rules call with
// "leaderboard_open 1" to open the Challenge Mode finish board)
constexpr int ClientCommand = 39;
constexpr int UserMessageBegin = 44;
constexpr int MessageEnd = 45;
// IPlayerInfoManager
constexpr int GetPlayerInfo = 0;
constexpr int GetGlobalVars = 1;
// IPlayerInfo
constexpr int GetNetworkIDString = 2;
}  // namespace slot

// ---- edicts and globals (SAR) ----
constexpr int kEdictFree = 1 << 1;

struct Edict {
	int stateFlags;
	int networkSerialNumber;
	void *networkable;
	void *unknown;  // IServerUnknown, which is the CBaseEntity itself

	bool InUse() const { return !(stateFlags & kEdictFree) && unknown; }
};

struct GlobalVars {
	float realtime;
	int framecount;
	float absoluteFrameTime;
	float curtime;
	float frametime;
	int maxClients;
	int tickcount;
	float intervalPerTick;
	float interpolationAmount;
	int simTicksThisFrame;
	int networkProtocol;
	void *saveData;
	bool client;
	int timestampNetworkingBase;
	int timestampRandomizeWindow;
	const char *mapname;
	int mapversion;
	const char *startspot;
	int loadType;
	char mapLoadFailed, deathmatch, coop, teamplay;
	int maxEntities;
	int serverCount;
	Edict *edicts;
};

// ---- console commands ----
// A tokenized command line, as the engine hands it to command callbacks.
struct CCommand {
	enum { kMaxArgc = 64, kMaxLength = 512 };
	int argc;
	int argv0Size;
	char argSBuffer[kMaxLength];
	char argvBuffer[kMaxLength];
	const char *argv[kMaxArgc];

	int ArgC() const { return argc; }
	const char *Arg(int i) const { return i >= 0 && i < argc ? argv[i] : ""; }
	const char *ArgS() const { return argv0Size ? argSBuffer + argv0Size : ""; }
};

using CommandCallback = void (*)(const CCommand &args);

// ConCommandBase + ConCommand. The vtable pointer is copied from one of the engine's own commands when ours is
// registered, so the engine's virtual calls land in its own code and read these fields.
struct ConCommand {
	void *vtable;
	ConCommand *next;
	bool registered;
	const char *name;
	const char *helpString;
	int flags;
	CommandCallback callback;
	void *completionCallback;
	bool hasCompletionCallback : 1;
	bool usingNewCommandCallback : 1;
	bool usingCommandCallbackInterface : 1;
};

// The start of a ConVar, enough to read its value (SAR)
struct ConVar {
	void *vtable;
	ConVar *next;
	bool registered;
	const char *name;
	const char *helpString;
	int flags;
	void *iconvarVtable;
	ConVar *parent;
	const char *defaultValue;
	char *string;
	int stringLength;
	float floatValue;
	int intValue;
};

constexpr int kFcvarCheat = 1 << 14;

// ---- network send tables: used only to find where a few entity fields live, by name (SAR) ----
enum SendPropType { kPropInt, kPropFloat, kPropVector, kPropVectorXY, kPropString, kPropArray, kPropDataTable, kPropInt64 };

struct SendTable;

struct SendProp {
	void *vtable;
	void *matchingRecvProp;
	SendPropType type;
	int bits;
	float lowValue, highValue;
	SendProp *arrayProp;
	void *arrayLengthProxy;
	int elements;
	int elementStride;
	const char *excludeDTName;
	const char *parentArrayPropName;
	const char *varName;
	float highLowMul;
	char priority;
	int flags;
	void *proxyFn;
	void *dataTableProxyFn;
	SendTable *dataTable;
	int offset;
	const void *extraData;
};

struct SendTable {
	SendProp *props;
	int propCount;
	const char *netTableName;
	void *precalc;
	bool flags;
};

struct ServerClass {
	const char *networkName;
	SendTable *table;
	ServerClass *next;
	int classId;
	int instanceBaselineIndex;
};

// ---- plugin callbacks ----
enum PluginResult { kPluginContinue = 0, kPluginOverride = 1, kPluginStop = 2 };

// Slot order as Portal 2's engine calls it (CServerPlugin in engine.dll)
class IServerPluginCallbacks {
public:
	virtual bool Load(CreateInterfaceFn interfaceFactory, CreateInterfaceFn gameServerFactory) = 0;
	virtual void Unload() = 0;
	virtual void Pause() = 0;
	virtual void UnPause() = 0;
	virtual const char *GetPluginDescription() = 0;
	virtual void LevelInit(const char *mapName) = 0;
	virtual void ServerActivate(Edict *edictList, int edictCount, int clientMax) = 0;
	virtual void GameFrame(bool simulating) = 0;
	virtual void LevelShutdown() = 0;
	virtual void ClientActive(Edict *entity) = 0;
	virtual void ClientFullyConnect(Edict *entity) = 0;
	virtual void ClientDisconnect(Edict *entity) = 0;
	virtual void ClientPutInServer(Edict *entity, const char *playerName) = 0;
	virtual void SetCommandClient(int index) = 0;
	virtual void ClientSettingsChanged(Edict *entity) = 0;
	virtual PluginResult ClientConnect(bool *allowConnect, Edict *entity, const char *name, const char *address, char *reject, int maxRejectLen) = 0;
	virtual PluginResult ClientCommand(Edict *entity, const CCommand &args) = 0;
	virtual PluginResult NetworkIDValidated(const char *userName, const char *networkId) = 0;
	virtual void OnQueryCvarValueFinished(int cookie, Edict *playerEntity, int status, const char *cvarName, const char *cvarValue) = 0;
	virtual void OnEdictAllocated(Edict *edict) = 0;
	virtual void OnEdictFreed(const Edict *edict) = 0;
};

}  // namespace sdk
