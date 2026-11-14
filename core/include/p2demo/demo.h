// Reads a Portal 2 demo (.dem) into plain data: who was where on every tick,
// what they pressed, where portals went, and what SAR wrote into the file.
//
// No engine or SDK dependency: the same code builds into the plugin, the
// command-line tools and the launcher's helper.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace p2demo {

struct Vec3 {
	float x = 0, y = 0, z = 0;
};

struct Header {
	std::string server;   // "localhost:27015" in the host's own demo
	std::string client;   // the recording player's name
	std::string map;
	std::string gameDir;  // portal2, portal_stories, ...
	int demoProtocol = 0;
	int networkProtocol = 0;
	float playbackTime = 0;
	int playbackTicks = 0;
	int playbackFrames = 0;
};

// The recording player's position and aim, one per network packet.  The
// origin is the player's feet (the engine writes the origin, not the eye).
struct View {
	int tick = 0;
	Vec3 origin;
	Vec3 angles;  // pitch, yaw, roll
};

// IN_* bits of UserCmd::buttons
enum Button : int {
	IN_ATTACK = 1 << 0,
	IN_JUMP = 1 << 1,
	IN_DUCK = 1 << 2,
	IN_FORWARD = 1 << 3,
	IN_BACK = 1 << 4,
	IN_USE = 1 << 5,
	IN_MOVELEFT = 1 << 9,
	IN_MOVERIGHT = 1 << 10,
	IN_ATTACK2 = 1 << 11,
};

struct UserCmd {
	int tick = 0;
	int slot = 0;  // split-screen slot of the demo message
	int commandNumber = 0;
	Vec3 viewAngles;
	float forwardMove = 0, sideMove = 0, upMove = 0;
	int buttons = 0;
	int impulse = 0;
	int mouseDx = 0, mouseDy = 0;
};

struct ConsoleCmd {
	int tick = 0;
	int slot = 0;
	std::string text;
};

// ---- what SAR records in the demo (custom data) ----

struct SarPortal {  // a portal was placed
	int tick = 0;
	int slot = 0;  // 0 = blue player (host), 1 = orange player
	bool orange = false;  // the player's second portal
	Vec3 pos;
};

struct SarEntityInput {  // an entity input fired on the server (host demos only)
	int tick = 0;
	int slot = -1;  // activating player's slot, -1 when none
	std::string target;  // targetname ("" when unnamed)
	std::string className;
	std::string input;
	std::string param;
};

struct SarFlags {  // challenge mode flags touched: the run ended
	int tick = 0;
	int slot = 0;
};

struct SarSplit {
	std::string name;
	int ticks = 0;
};

struct SarSpeedrun {  // SAR's speedrun timer result
	int tick = 0;
	bool complete = false;
	int totalTicks = 0;  // sum of every split's segments
	std::vector<SarSplit> splits;
};

struct SarTimescale {
	int tick = 0;
	float timescale = 1;
};

// ---- entities, decoded from the network data ----

// FL_* bits of PlayerKey::flags
enum PlayerFlag : int {
	FL_ONGROUND = 1 << 0,
	FL_DUCKING = 1 << 1,
};

struct PlayerKey {
	int tick = 0;
	Vec3 origin;          // feet, world space
	float eyePitch = 0, eyeYaw = 0;
	float viewOffsetZ = 0;  // eye height above the origin (lower while ducked)
	int flags = 0;          // m_fFlags
	bool hasVelocity = false;  // only the recording player's own velocity is sent
	Vec3 velocity;
	int team = 0;           // 2 = orange (P-body), 3 = blue (ATLAS); 0 in single player
	int heldEntity = -1;    // entity index of the carried object, -1 = none
	bool inSnapshot = true; // false: out of the recorder's view, state as last sent
};

struct PlayerTrack {
	int entIndex = 0;  // 1 = first player (host), 2 = second
	std::string name;  // from "userinfo"
	std::string model;
	bool isRecorder = false;
	bool challenge = false;  // the player was in Challenge Mode (the level is set up differently in places)
	std::vector<PlayerKey> keys;  // in tick order, one per snapshot that held the player
	// (and one with inSnapshot false on the tick the player left the snapshot or was deleted)
};

struct PortalKey {
	int tick = 0;
	bool active = false;  // m_bActivated
	Vec3 origin;
	Vec3 angles;
	int linkageGroup = 0;  // m_iLinkageGroupID (Portal 2 doesn't send it: 0; see team and firedBy)
	int team = 0;
	int firedBy = -1;       // entity index of the player who shot it (m_hFiredByPlayer), -1 = none
	int linkedPortal = -1;  // entity index of the portal it leads to (m_hLinkedPortal), -1 = none
	int parent = -1;        // entity index of what it rides on (a moving panel): origin and angles are relative to it
	bool inSnapshot = true; // false: out of the recorder's view (state as last sent) or deleted
	bool removed = false;   // the entity was deleted on this tick (the track's last key)
};

struct PortalTrack {
	int entIndex = 0;
	bool isPortal2 = false;  // the pair's second (orange in single player) portal
	std::vector<PortalKey> keys;  // one per change of any field above
};

// Anything else with a model whose origin or angles changed during the demo
// (cubes, turrets, moving brushes, ...)
struct MoverKey {
	int tick = 0;
	Vec3 origin;
	Vec3 angles;
	int parent = -1;  // entity index when attached to another entity (origin is then relative)
	bool inSnapshot = true;  // false: out of the recorder's view (state as last sent) or deleted
	bool removed = false;    // the entity was deleted on this tick (fizzled, killed): the track's last key
	bool noDraw = false;     // EF_NODRAW: the entity exists but isn't drawn
};

struct MoverTrack {
	int entIndex = 0;
	std::string className;  // server class: CPropWeightedCube, CFuncMoveLinear, ...
	std::string model;      // "*12" for brush models
	std::vector<MoverKey> keys;  // one per change
};

// A portal shot reaching a surface ("PortalFX_Surface" user message)
struct PortalShot {
	int tick = 0;
	int shooter = 0;    // entity index of the player
	int team = 0;
	int portalNum = 0;  // 1 or 2
	int result = 0;     // 0 = placed, 4 = bad surface, 7 = an old portal closing, ...
	Vec3 origin;
	Vec3 angles;
};

struct Demo {
	Header header;
	float intervalPerTick = 1.0f / 60;
	int firstTick = 0, lastTick = 0;
	int recorderEntIndex = 0;  // the recording player's entity index
	// The server's tick count (counted from the map's load) at this demo's tick 0: what the level's own clock
	// showed when the recording began. kNoServerTick when the demo doesn't say.
	static constexpr int kNoServerTick = -1000000;
	int serverTickAtZero = kNoServerTick;

	std::vector<View> views;
	std::vector<UserCmd> userCmds;
	std::vector<ConsoleCmd> consoleCmds;  // typed/bound commands only, not the cvar values written at the start

	bool hasSarData = false;
	std::vector<SarPortal> sarPortals;
	std::vector<SarEntityInput> sarEntityInputs;
	std::vector<SarFlags> sarFlags;
	std::vector<SarSpeedrun> sarSpeedruns;
	std::vector<SarTimescale> sarTimescales;

	bool entitiesDecoded = false;  // false when the network data couldn't be read (see warnings)
	std::vector<PlayerTrack> players;
	std::vector<PortalTrack> portals;
	std::vector<MoverTrack> movers;
	std::vector<PortalShot> portalShots;

	std::vector<std::string> warnings;
};

struct ParseOptions {
	bool entities = true;  // decode the network data (players, portals, movers, shots)
	bool movers = true;    // keep MoverTracks (off: players and portals only)
	// Stop decoding the network data once it says which mode the players are in (PlayerTrack::challenge): for
	// lists of demos, which ask nothing else of it. The tracks then end there.
	bool modeOnly = false;
};

// Never throws and never reads outside [data, data + size).  On failure
// returns false with a message in `error`; `out` then holds whatever was read
// before the problem.  A demo whose container reads fine but whose network
// data can't be decoded still returns true, with entitiesDecoded = false and
// a warning.
bool Parse(const uint8_t *data, size_t size, Demo &out, std::string &error, const ParseOptions &options = {});
bool ParseFile(const char *path, Demo &out, std::string &error, const ParseOptions &options = {});

}  // namespace p2demo
