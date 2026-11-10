// Save states (ChocolateLoading snapshots) for retries, and what they need of the game's threads and demos.
#include "trainer_internal.h"

#include <ctime>
#include <filesystem>

namespace trainer {

// ChocolateLoading (github.com/Blenderiste09/ChocolateLoading) keeps whole game states in memory: "snap_save"
// stores one, "snap_load" brings it back in a few milliseconds. When the player runs the game through it and has
// turned "prac_set snapshots 1" on, the trainer stores the level a moment into each lead-in and a retry of the
// same attempt returns to that instead of restarting the level and replaying the run up to the part. What is held
// and what is under way is in g.save (SaveState, trainer_internal.h).
constexpr const char *kSnapshotName = "p2practice";
int g_snapshotDelayTicks = 3;  // lets the portals and objects just placed (queued inputs) take effect first (prac_debug snapdelay)

// Whether ChocolateLoading is in the game and is a build the trainer can work with: one that says when a save or
// a restore is done, and keeps the game's queued rendering on through it. (The original switches queued rendering
// off and on around every operation: each switch stops or starts the render thread, which cost a stutter per
// retry, and the game crashed on that thread a few seconds after some restores. The build that comes with
// P2 Practice holds the render thread at a safe point instead, unless "snap_opt render_lock 0" turns that off.)
bool SnapshotsReady() {
	if (!engine::CommandExists("snap_save") || !engine::CommandExists("snap_load")) return false;
	engine::SnapshotStatus status = engine::SnapshotsStatus();
	return status.available && status.keepsRenderThread;
}

// (Not with a partner over the network: a snapshot holds this computer's game only, and the partner's game would
// not go back with it. Those retries restart the level instead.)
bool SnapshotsUsable() {
	return g_set.snapshots && !g.replaying && !RemotePerson() && SnapshotsReady();
}

// The game's worker threads that ChocolateLoading switches off around every save and restore (threaded host, async
// sound mixer, queued network packets) are kept off while practice uses save states: switched for each operation,
// the threads started and stopped every time and ChocolateLoading waited 4 frames before each restore for them to
// drain; kept off, a retry's restore comes at the next frame. Particles are kept off the job threads the same
// way: simulated on a worker, they crashed a moment after some restores (CParticleCollection::Simulate,
// sp_a2_laser_stairs). None of these costs frame rate, and none is saved: the next game start has the defaults.
const char *const kThreadCvars[] = {"host_thread_mode", "snd_mix_async", "net_queued_packet_thread"};
constexpr const char *kParticlesCvar = "r_threaded_particles";

void PinThreads() {
	if (g.threadsPinned) return;
	g.threadsPinned = true;
	for (size_t i = 0; i < std::size(kThreadCvars); i++) {
		g.savedThreadCvars[i] = static_cast<int>(engine::CvarFloat(kThreadCvars[i], 0));
		if (g.savedThreadCvars[i] != 0) engine::Execute("%s 0", kThreadCvars[i]);
	}
	g.savedThreadedParticles = static_cast<int>(engine::CvarFloat(kParticlesCvar, 1));
	if (g.savedThreadedParticles != 0) engine::Execute("%s 0", kParticlesCvar);
}

// (the cvars change when the game runs the command, a frame after it is given)
static bool ThreadsOff() {
	for (const char *name : kThreadCvars)
		if (engine::CvarFloat(name, 0) != 0) return false;
	return engine::CvarFloat(kParticlesCvar, 1) == 0;
}

void ReleaseThreads() {
	if (!g.threadsPinned) return;
	g.threadsPinned = false;
	for (size_t i = 0; i < std::size(kThreadCvars); i++)
		if (g.savedThreadCvars[i] != 0) engine::Execute("%s %d", kThreadCvars[i], g.savedThreadCvars[i]);
	if (g.savedThreadedParticles != 0) engine::Execute("%s %d", kParticlesCvar, g.savedThreadedParticles);
}

// Forgets the save state and has ChocolateLoading free it. (Not for the middle of a restore: see Stop.)
void DropSnapshot() {
	if (g.save.handover < 0) return;
	g.save.handover = -1;
	if (engine::CommandExists("snap_drop")) engine::Execute("snap_drop %s", kSnapshotName);
}

// The level is gone, and whatever was held or under way with it
void ForgetSaveState() {
	DropSnapshot();
	g.save.phase = SavePhase::Idle;
}

// Save states and demos. ChocolateLoading keeps every file open that a save state holds, so that a restore can go
// back to it, and a demo being recorded when the snapshot was taken was one of them: SAR then couldn't rename it
// at the flags (sar_challenge_autostop 2, set by srconfigs' Challenge Mode category) and its error closed the
// game. So no snapshot is taken while a demo is recording: the demo is stopped just before the save and a new one
// starts once the save is done (record "", which SAR names as srconfigs asks; in Challenge Mode the same name the
// level's own demo had). A restore stops the attempt's demo the same way and starts the next one after it. Every
// attempt then has a demo of its own, as it would with the level restarted for each; SAR stops and renames a
// finished one at the flags, and the save state stays for the next attempt.
constexpr int kDemoRestartFrames = 2;  // (the frame after the save or the restore is seen done)

static void StopDemoForSnapshot() {
	if (!engine::RecordingDemo()) return;
	engine::Execute("stop");
	g.demoToRestart = true;
}

// Before a restore: the attempt's demo stops, and the next attempt gets one when there was a demo since the
// snapshot, recording still or already stopped and renamed by SAR at the flags
static void StopDemoForRestore() {
	StopDemoForSnapshot();
	if (g.demoSinceSnapshot) g.demoToRestart = true;
	g.demoSinceSnapshot = false;
}

// Starts the demo stopped for a save state again, when it is due (every frame, paused or not)
void TickDemoRestart() {
	if (!g.demoToRestart) return;
	if (g.save.Busy()) return;  // (it starts once the save or the restore is done)
	if (--g.demoRestartIn > 0) return;
	if (!*engine::MapName()) {
		g.demoRestartIn = 1;
		return;
	}
	g.demoToRestart = false;
	if (engine::RecordingDemo()) return;  // (one started meanwhile: SAR's own on a level load)
	if (!engine::CvarString("sar_record_prefix").empty()) {
		engine::Execute("record \"\"");
		return;
	}
	// Without SAR's naming: demos/p2practice/<map>_<date and time>
	namespace fs = std::filesystem;
	std::error_code ec;
	fs::create_directories(fs::u8path(engine::GameDirectory() + "/demos/p2practice"), ec);
	char stamp[32];
	std::time_t now = std::time(nullptr);
	std::tm local{};
	localtime_s(&local, &now);
	std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &local);
	engine::Execute("record \"demos/p2practice/%s_%s\"", engine::MapName(), stamp);
}

// A lead-in begins: its snapshot is taken a few ticks in
void ScheduleSnapshot() {
	if (g.save.Busy()) return;
	g.save.phase = SnapshotsUsable() ? SavePhase::Due : SavePhase::Idle;
	g.save.dueIn = g_snapshotDelayTicks;
	if (g.save.phase == SavePhase::Due) PinThreads();
}

// The lead-in ended before its snapshot was due: the attempt goes without (and an older one held stays as it is)
void CancelDueSnapshot() {
	if (g.save.phase == SavePhase::Due) g.save.phase = SavePhase::Idle;
}

static void BeginOperation(SavePhase phase) {
	engine::SnapshotStatus status = engine::SnapshotsStatus();
	g.save.phase = phase;
	g.save.doneBefore = phase == SavePhase::Saving ? status.saved : status.restored;
	g.save.failedBefore = status.failed;
}

// Called every tick of a lead-in: asks for the snapshot when it is due
void TakeSnapshotIfDue() {
	if (g.save.phase != SavePhase::Due) return;
	if (!SnapshotsUsable()) {
		g.save.phase = SavePhase::Idle;
		return;
	}
	if (--g.save.dueIn > 0) return;
	if (!ThreadsOff()) {
		g.save.dueIn = 1;  // (a tick later, when the game has run the commands that switch them off)
		return;
	}
	// (the one held until now is replaced: ChocolateLoading keeps one snapshot per name)
	g.save.handover = g.handover;
	g.save.watch = g.watch;
	engine::FlushScript();
	// The game keeps its coroutines (Steam's game coordinator jobs run in them) in a list per thread, made the
	// first time a thread creates one and found again through a thread-local pointer, which a restore doesn't bring
	// back: a list first made after the save was freed by the restore while the pointer still led there, and the
	// next job crashed the game (Coroutine_Create, mp_coop_fling_3 at the flags, a few seconds after a restore).
	// So the main thread's list is made before every save.
	engine::MakeCoroutineList();
	StopDemoForSnapshot();
	g.save.holdsDemo = engine::RecordingDemo();
	g.demoSinceSnapshot = false;
	engine::Execute("snap_save %s", kSnapshotName);
	BeginOperation(SavePhase::Saving);
}

// Returns to the snapshot of this very attempt, if there is one. True when it is on its way: the attempt is set
// up as it was at the snapshot now, and the level follows when ChocolateLoading has brought it back (TickSaveState).
bool RestoreSnapshot(int handover, bool watch) {
	if (g.save.phase != SavePhase::Idle || g.save.handover != handover || g.save.watch != watch) return false;
	if (!SnapshotsUsable() || g.slots.empty()) return false;
	PinThreads();
	g.watch = watch;
	g.handover = handover;
	g.segment = g.route.SegmentAt(handover);
	g.cursor = g.save.cursor;
	g.mode = watch ? Mode::Watch : Mode::LeadIn;
	g.objectLastDriven.clear();
	g.attempts++;
	for (Slot &slot : g.slots) {
		slot.progress = handover;
		slot.partStart = handover;
		slot.part = g.route.SegmentAt(handover);
		slot.finished = false;
	}
	engine::FlushScript();
	StopDemoForRestore();
	engine::Execute("snap_load %s", kSnapshotName);
	BeginOperation(SavePhase::Restoring);
	return true;
}

// Every frame, before anything else: sees the save or the restore asked for come to its end. The first frame
// after a save is the moment the snapshot holds, and the first frame after a restore is that same moment again:
// the trainer has run on in between (its own memory is not part of the snapshot), so the run's cursor is noted
// at the one and set back at the other. What else has to follow a restore is the caller's (OnFrame).
SaveEvent TickSaveState() {
	if (!g.save.Busy()) return SaveEvent::None;
	bool saving = g.save.phase == SavePhase::Saving;
	engine::SnapshotStatus status = engine::SnapshotsStatus();
	bool done = (saving ? status.saved : status.restored) != g.save.doneBefore;
	bool failed = !done && (!status.available || status.failed != g.save.failedBefore);
	if (!done && !failed) return SaveEvent::None;
	g.save.phase = SavePhase::Idle;
	g.demoRestartIn = kDemoRestartFrames;
	if (saving) {
		if (failed) {
			g.save.handover = -1;
			engine::Warn("ChocolateLoading could not store the save state: the next retry restarts the level.\n");
			return SaveEvent::SaveFailed;
		}
		if (g.save.handover < 0) {
			// (dropped while it was being stored: at the flags, see DropSnapshotAtFlags)
			if (engine::CommandExists("snap_drop")) engine::Execute("snap_drop %s", kSnapshotName);
			return SaveEvent::Saved;
		}
		g.save.cursor = g.cursor;
		g.save.startScreenOpen = !g.startScreenClosed;
		return SaveEvent::Saved;
	}
	if (failed) {
		DropSnapshot();
		engine::Warn("ChocolateLoading could not bring the save state back: restarting the level instead.\n");
		return SaveEvent::RestoreFailed;
	}
	g.cursor = g.save.cursor;
	return SaveEvent::Restored;
}

}  // namespace trainer
