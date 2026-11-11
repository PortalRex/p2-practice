// What the trainer's files share: the session's state and the functions one file has for the others.
// (trainer.h is what the rest of the plugin sees.)
#pragma once

#include <chrono>
#include <climits>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "engine.h"
#include "route.h"
#include "trainer.h"

namespace trainer {

using engine::Vec3;
using route::Route;
using route::Sample;

enum class Mode {
	Idle,            // nothing running (a route may be loaded)
	Reloading,       // waiting for the map restart we asked for
	WaitForPlayers,  // map is up again, players still spawning
	CatchUp,         // replaying the run quickly from its start so the map's doors and triggers are as the run had them
	LeadIn,          // the run plays itself up to the handover
	Run,             // the players are in control
	Watch,           // the run plays itself to the end
	Done,            // finished: the result stays up for a moment
	GoingBack,       // the level's base save state is on its way back: the attempt is set up in it when it is there
};

// The save state for retries (a ChocolateLoading snapshot of the level a moment into a lead-in) and the one
// operation on it that can be under way. ChocolateLoading does a save or a restore at the end of a frame some
// frames after it is asked, and reports it in its counts (engine::SnapshotsStatus): Saving and Restoring last
// until the count of that kind, or the count of failures, has moved past what it was at the request.
//
//   Idle --(a lead-in begins)--> Due --(snap_save)--> Saving --(saved, or failed)--> Idle
//   Idle, a state held --(retry: snap_load)--> Restoring --(restored, or failed: the level restarts)--> Idle
enum class SavePhase {
	Idle,       // nothing under way
	Due,        // the lead-in that just began gets a snapshot in `dueIn` ticks
	Saving,     // snap_save is asked for
	Restoring,  // snap_load is asked for
};

// The settings an attempt's level is set up with: who plays which robot, whether the partner is driven, the
// ghosts, the lead-in's length, the game's speed. A save state holds the level as they made it, so it only serves
// attempts that have the same ones (changed since, the next attempt is set up afresh and takes a new save state).
struct SetupChoices {
	int role = 0, ghostStyle = 0;
	bool partner = false, ghost = false, challenge = false;
	float leadIn = 0, speed = 0;

	bool operator==(const SetupChoices &o) const {
		return role == o.role && ghostStyle == o.ghostStyle && partner == o.partner && ghost == o.ghost && challenge == o.challenge &&
		       leadIn == o.leadIn && speed == o.speed;
	}
};

struct SaveState {
	SavePhase phase = SavePhase::Idle;
	int dueIn = 0;
	unsigned doneBefore = 0, failedBefore = 0;  // ChocolateLoading's counts when the save or restore was asked for
	// The state held (being stored, while Saving)
	int handover = -1;             // the handover its attempt had, -1: none held
	bool watch = false;
	SetupChoices choices;          // the settings its level was set up with (see SetupChoices)
	int cursor = 0;                // the run tick it was taken at
	bool startScreenOpen = false;  // the start board hadn't come and gone yet when it was taken
	bool holdsDemo = false;        // a demo was being recorded when it was taken (see DropSnapshotAtFlags)
	bool atBase = false;           // it is the base state itself (the attempt whose lead-in the base was taken in)

	// The base state: the first one stored in a level that the run hadn't been further in yet, kept beside the
	// attempt's own. An attempt that starts before where the level has got to since (a door the run opened later
	// stands open) has the level brought back to the base and set up from there: see RestoreBase.
	int baseCursor = -1;           // the run tick it was taken at, -1: none held
	int baseClock = 0;             // the level's own clock at that moment (LevelClock)
	bool baseStartScreenOpen = false;
	bool baseHoldsDemo = false;
	std::set<std::string> baseModels;  // the ghost models registered with the game by then
	bool toBase = false;           // the restore under way is one to the base state

	bool Busy() const { return phase == SavePhase::Saving || phase == SavePhase::Restoring; }
};

// One player of the route and whoever stands in for them now
struct Slot {
	int entIndex = 0;    // live player's entity index, 0 when nobody plays this robot
	bool human = false;  // a person controls it (otherwise the plugin drives it)
	int progress = 0;    // the route tick this player is level with
	int partStart = 0;   // run clock (cursor) when they entered their current part
	int part = 0;        // the part they are in
	bool finished = false;
	int finishCursor = 0;
};

// A ghost: the prop that shows where one of the run's players was
struct Ghost {
	int entIndex = 0;                 // the prop's entity number, once the script that made it has reported it
	bool placed = false;              // false until it has been put somewhere (its body then faces where it looks)
	float feetYaw = 0;                // where its body faces; the upper body turns from there to where it looks
	float feetGoal = 0;               // where its body is turning to
	bool hidden = false;              // not drawn: it stands inside somebody
	const char *animation = nullptr;  // the sequence it is playing
	const char *wanted = nullptr;     // the sequence its movement calls for, and for how many ticks it has
	int wantedTicks = 0;
	float rate = 1;                   // how fast the sequence plays
};

struct Session {
	Route route;         // the run being practiced against
	bool hasRoute = false;
	Route replay;        // another run that drives the players against it (prac_replay)
	bool replaying = false;

	Mode mode = Mode::Idle;
	bool watch = false;
	int cursor = 0;      // "now" on the recorded run's clock, in its demo ticks
	int handover = 0;    // tick at which the players take over
	int leadStart = 0;   // tick at which the lead-in begins
	int segment = 0;     // the part this attempt starts from
	std::vector<Slot> slots;

	// The furthest tick of the run this map load has lived through: the map's own state (doors, panels, relays)
	// is as the run had it there.
	int worldReached = 0;
	bool helperLoaded = false;  // this map load
	bool speedChanged = false;
	SaveState save;               // the save state for retries, and the save or restore under way
	bool demoToRestart = false;   // a demo was stopped for a save state: a new one starts in demoRestartIn frames
	int demoRestartIn = 0;
	bool demoSinceSnapshot = false;  // a demo was recorded at some moment since the snapshot (SAR stops it at the flags)
	bool threadsPinned = false;   // the game's worker threads are switched off for save states (see PinThreads)
	int savedThreadCvars[3] = {}; // host_thread_mode, snd_mix_async, net_queued_packet_thread as they were
	int savedThreadedParticles = 1;  // r_threaded_particles as it was
	bool retryAsked = false;      // the game's own restart (the finish board's Replay Map) asked for a retry
	bool closeScreensAfterRestore = false;  // that retry is a save state on its way: close the boards again when it lands
	bool stopWhenRestored = false;  // practice was stopped with a save or a restore under way: the rest of the stop follows it
	bool savesOff = false;        // the game's saving was switched off for practice (and is switched back on at the end)
	bool levelTouched = false;    // the trainer has worked in this map load: nothing done in it may count as a run
	bool resumeAfterLoad = false; // the level went away in the middle of an attempt: start it again when it is back
	std::chrono::steady_clock::time_point levelWentAt{};  // when it went (a restart brings it back within seconds)
	std::chrono::steady_clock::time_point reloadAskedAt{};  // when the trainer last asked for (or waited on) a fresh level
	bool humanDead = false;       // a person was dead at the last frame (in Challenge Mode the level restarts for it)
	bool botPartner = false;      // a split-screen bot plays the other robot: the person's view fills the screen
	bool screenHeld = false;      // ... and the game cannot split it (from the attempt's set-up to the level's end)
	std::vector<int> placedBots;  // the bots the trainer put in place this tick, with the game's movement off for them
	int waitTicks = 0;
	std::vector<Ghost> ghosts;  // one per robot of the run
	std::set<std::string> registeredModels;  // ghost models registered with the game in this map load
	std::string recordNext;  // development: a demo name to start recording under when the next attempt begins
	// Challenge Mode's leaderboard screens (see GameScreenDue)
	int pausedFrames = 0;
	bool startScreenClosed = false;  // this level's start board has been closed
	bool ranSinceLoad = false;       // the level has simulated a frame since it was loaded
	bool catapultMade = false;       // this level has the trainer's catapult (see MakeCatapult)
	bool flagsUp = false;            // the Challenge Mode finish flags stood when the attempt began (see WatchFlags)
	bool flagsGone = false;          // a finish took them: the next attempt needs a fresh level
	bool dimmed = false;             // the screen is black for a catch-up (::PP.Dim)
	bool quiet = false;              // the game's sound is faded out for a catch-up (Quiet)
	int doneTicks = 0;
	int boardHeldFrames = 0;         // frames the finish board's opening has been held back (TellBoard)
	int attempts = 0;
	std::vector<int> objectLastDriven;  // per route object: the last tick a driven player carried it
	std::vector<int> useKeyDown;        // players (entity indices) whose use key the trainer is holding this tick
	std::map<int, int> usePressedAt;    // player (entity index) -> the server tick the trainer last pressed its use key
	bool jumpKeyDown = false;           // the trainer is holding the jump key of the player at this computer
	bool duckKeyDown = false;           // ... and its duck key (SetDuckKey)
	int autoPart = -1;   // prac_auto: start this part by itself once the route's map is up (-1 = not armed)
	float autoSeconds = -1;  // prac_auto at <seconds>: that moment of the run instead of a part's start (-1 = the part)
	bool autoWatch = false;
	std::map<int, int> bestFromTick;  // the run's tick an attempt started at -> best ticks to the finish
	std::map<int, int> bestPart;      // part -> best ticks through it
	std::vector<Result> results;      // finished attempts, for the menu (the latest kResultsKept)
};

// Text channels (game_text has six; the game draws channel 3 in a larger font than the others and channel 2 in a
// smaller one): the clock gets the large one, the speed line the small one
enum Channel { kChDelta = 0, kChSpeed = 2, kChClock = 3, kChFlash = 4 };

struct LivePlayer {
	int entIndex;
	engine::PlayerState state;
};

struct Rgb {
	int r, g, b;
};

struct Command {
	const char *name;
	sdk::CommandCallback callback;
	const char *help;
	bool partnerMayUse;  // safe for the co-op partner to run on the host
};

constexpr size_t kResultsKept = 100;

constexpr int kHudInterval = 6;  // ticks between text updates
constexpr int kBoardHoldFrames = 10;  // frames the finish board's opening waits at most for the attempt's time (see TellBoard)
constexpr int kButtonGlitchDelay = 10;  // ticks after a recorded button glitch that the trainer repeats it (see DriveSlots)
constexpr float kFinishRadius = 56;
constexpr float kGhostHideDistance = 44;

constexpr float kClockY = 0.045f, kDeltaY = 0.108f, kFlashY = 0.215f;  // the lines, from the top of the screen
constexpr float kLingerSeconds = 600;  // how long the finish stays on screen (until the next attempt or a stop)
constexpr const char *kDot = "  \xC2\xB7  ";  // a middle dot between the parts of a line

constexpr const char *kEndNode = "challenge_mode_end_node";  // the Challenge Mode flags

// trainer.cpp
extern Settings g_set;
extern Session g;
extern int g_positionDecimals;
extern int g_frameStartParity;
extern bool g_noPose;
extern bool g_scanEdicts;
extern int g_lastBadEdict;
extern bool g_noGhostMove;
extern bool g_logArrivals;

// trainer_hud.cpp
extern bool g_drawnHud;
extern std::string g_flashText;
extern std::chrono::steady_clock::time_point g_flashUntil;

// trainer_snapshot.cpp
extern int g_snapshotDelayTicks;

// trainer_drive.cpp
extern bool g_botPlacedOnly;
extern bool g_replayPickups;
extern int g_portalLead;
extern int g_followTicks;

// trainer_commands.cpp
extern const std::vector<Command> kCommands;

inline Vec3 Sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline float Length(Vec3 a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
inline float Distance(Vec3 a, Vec3 b) { return Length(Sub(a, b)); }

inline int LeadTicks() { return static_cast<int>(g_set.leadIn / g.route.tickInterval + 0.5f); }

// The tick of the run the live game is at. The plugin is called before the game simulates the tick `cursor`, so
// what it reads of the players (where they are, whether they touched the flags) is the outcome of the tick before.
// Every time measured in the attempt is taken there: the run's own times are where its players were at the end of
// a tick, and a person exactly on the run reads 0.00, not a tick slow.
inline int LiveTick() { return g.cursor - 1; }

// The run's tick of a finish at the flags, counted as the demos count it (SAR's flags record, the end of every
// record's time): the tick whose outcome the plugin reads the flags gone in. A record replayed through its flags
// reads its own time, in single player (Triple Laser, Laser Stairs, Bridge Intro) and in co-op (Turret Warehouse,
// Flings, Team Retrieval, Vertical Flings, Cooperative Polarity), as long as the host's partner is where the
// host's game had it when it reaches them: see route::Actor::seenPos.
inline int FlagsTick() { return LiveTick(); }

// trainer.cpp
void MovePlayer(int entIndex, const Sample &s, const Vec3 &velocity);
void ScanEdicts(int tick, bool restored);
bool FrameStartsNow();
std::string Quote(const std::string &text);
std::vector<LivePlayer> LivePlayers();
bool Driven(const Slot &slot);
const Route &Driver();
int DriverTick(int cursor);
int DriverActor(size_t slotIndex);
std::vector<int> PlayersOf(const Route &route);
int LastDrivenTick();
std::string LoadCommand();
bool MapMatches();
bool BusyRestoring();
void Stop(bool announce);
void Start(int handover, bool watch);
void Approach();
bool WorldEventsBetween(int after, int upTo);
constexpr int kNoClock = INT_MIN;          // LevelClock: the run's demo doesn't say what the level's clock showed
constexpr int kShortestLeadTicks = 10;     // the least the run plays itself before the handover (a sixth of a second)
int LevelClock();
void StartPart(int part, bool watch);
bool ChallengeMode();
void CloseGameScreens();
void PrintPerf();

// trainer_ghosts.cpp
Rgb TeamColor(int team);
void RecolorGhosts();
void ClearGhosts();
bool RemotePerson();
void RegisterGhostModels();
void CreateGhosts();
Vec3 VelocityAt(const Route &route, int actor, int tick);
void MoveGhosts();

// trainer_path.cpp
void SetUpRoute();
void ClearRoute();

// trainer_hud.cpp
void Say(int entIndex, int channel, float hold, const std::string &text);
bool ShowsHud(const Slot &slot);
void SetupHud(int entIndex);
float GroundSpeed(const Route &route, int actor, int tick);
std::string ShowAt(int tick);
std::string Gap(int ticks, int recordTicks);
void UpdateHud(float hold = -1);
void Flash(float seconds, const std::string &text);

// trainer_snapshot.cpp
bool SnapshotsReady();
bool SnapshotsUsable();
void PinThreads();
void ReleaseThreads();
void DropSnapshot();
void ForgetSaveState();
void TickDemoRestart();
void ScheduleSnapshot();
void CancelDueSnapshot();
void TakeSnapshotIfDue();
bool RestoreSnapshot(int handover, bool watch);
bool RestoreBase(int leadStart);
void ContinueFromBase();
enum class SaveEvent { None, Saved, SaveFailed, Restored, RestoreFailed };
SaveEvent TickSaveState();

// trainer_drive.cpp
void SetPortals(const Route &route, int tick);
void DropCarried(const Slot &slot);
void ReleaseUseKeys();
void SetDuckKey(bool down);
void SetBotMovement();
void DriveSlots();
void MakeCatapult();
void MoveObjects(bool all);
void HandOverObjects();
void ShieldPlayers(bool onlyDriven);
void PlacePortals();

// trainer_finish.cpp
void RecordBest(std::map<int, int> &table, int key, int ticks, bool &isBest);
void TrackPlayers();
void Finish(bool atFlags);
void DropSnapshotAtFlags();
bool WatchFlags();
void NoteReplayAtFlags();
void FinishAtFlags();
void TellBoard();
engine::ClientCommandFate HoldBoard(const char *command);
void ReleaseStaleBoard();

// trainer_commands.cpp
int TickAtSeconds(float seconds);
void CmdRetry(const sdk::CCommand &);
void CmdGhostEntity(const sdk::CCommand &args);

}  // namespace trainer
