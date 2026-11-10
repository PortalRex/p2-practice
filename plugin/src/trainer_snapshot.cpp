// Save states (ChocolateLoading snapshots) for retries, and what they need of the game's threads and demos.
#include "trainer_internal.h"

#include <chrono>
#include <ctime>
#include <filesystem>

namespace trainer {

// ChocolateLoading (github.com/Blenderiste09/ChocolateLoading) keeps whole game states in memory: "snap_save"
// stores one, "snap_load" brings it back in a few milliseconds. When the player runs the game through it and has
// turned "prac_set snapshots 1" on, the trainer stores the level a moment into each lead-in and a retry of the
// same attempt returns to that instead of restarting the level and replaying the run up to the part.
constexpr const char *kSnapshotName = "p2practice";
int g_snapshotDelayTicks = 3;  // lets the portals and objects just placed (queued inputs) take effect first (prac_debug snapdelay)

// (Not with a partner over the network: a snapshot holds this computer's game only, and the partner's game would
// not go back with it. Those retries restart the level instead.)
bool SnapshotsUsable() {
	return g_set.snapshots && !g.replaying && !RemotePerson() && engine::CommandExists("snap_save") && engine::CommandExists("snap_load");
}

// ChocolateLoading switches the game's queued rendering off for every save and restore and back on after it. Each
// switch stops or starts the render thread, which costs a stutter, and the game crashed on that thread a few
// seconds after a restore ("pure virtual function call" in the queued render calls, mp_coop_laser_2). So the
// plugin makes the switch itself, with time for it to take effect on either side: off well before a save or a
// restore, on again a moment after it (kQueueResumeFrames). In between the game renders on its own thread as
// usual: on the main thread all the time, it ran a third slower (Turret Warehouse: 530 fps down to 360). The
// price is a retry that comes g_settleMs later.
//
// The switch is a queued command and the material system changes over at the end of a frame, so a snapshot taken
// in the same frame (as the first one of a session was) held the game with its render thread still running, and
// a restore into that went wrong on the renderer: lightmaps drawn yellow all over Turret Warehouse, then a crash
// in the video texture update (valve_avi -> shaderapivk, a call through a freed object). So the switch is made as
// soon as an attempt with save states begins, and a snapshot waits until it has been in effect for a moment
// (ThreadsSettled). Particles are kept off the job threads the same way: simulated on a worker, they crashed a
// moment after some restores (CParticleCollection::Simulate, sp_a2_laser_stairs). Neither cvar is saved, so the
// next game start has the defaults again.
//
// The other threading cvars ChocolateLoading switches off around every save and restore (threaded host, async
// sound mixer, queued network packets) are kept off for the session too: switched for each operation, the threads
// started and stopped every time, and ChocolateLoading waited 4 frames before each restore for them to drain;
// kept off, a retry's restore comes at the next frame.
const char *const kThreadCvars[] = {"mat_queue_mode", "host_thread_mode", "snd_mix_async", "net_queued_packet_thread"};

int g_settleMs = 100;                  // how long the render thread gets to stop (prac_debug settle)

// Switches the game's threads off for a save or a restore: the render thread until a moment after it, the others
// (which cost no frame rate) until practice stops
void SingleThreadedRendering() {
	g.queueResumeIn = 0;
	if (!g.queuePinned) {
		g.queuePinned = true;
		for (size_t i = 0; i < std::size(kThreadCvars); i++) {
			g.savedThreadCvars[i] = static_cast<int>(engine::CvarFloat(kThreadCvars[i], 0));
			if (i > 0 && g.savedThreadCvars[i] != 0) engine::Execute("%s 0", kThreadCvars[i]);
		}
		g.savedThreadedParticles = static_cast<int>(engine::CvarFloat("r_threaded_particles", 1));
		engine::Execute("r_threaded_particles 0");
	}
	if (g.renderThreadOff) return;
	g.renderThreadOff = true;
	g.queuePinnedAt = std::chrono::steady_clock::now();
	if (g.savedThreadCvars[0] != 0) engine::Execute("mat_queue_mode 0");
}

bool ThreadsSettled() {
	if (!g.renderThreadOff) return false;
	// (nothing to wait for in a game that had queued rendering off anyway)
	bool waited = std::chrono::steady_clock::now() - g.queuePinnedAt >= std::chrono::milliseconds(g_settleMs);
	if (g.savedThreadCvars[0] != 0 && !waited) return false;
	for (const char *name : kThreadCvars)
		if (engine::CvarFloat(name, 0) != 0) return false;
	return engine::CvarFloat("r_threaded_particles", 1) == 0;
}

// Queued rendering goes back on when the save or restore it was switched off for is well over (every frame)
void TickQueuedRendering() {
	if (RestoreOnItsWay() || g.restorePending || g.snapshotIn > 0 || g.mode == Mode::CatchUp) return;
	if (!g.renderThreadOff) {
		g.queueResumeIn = 0;
		return;
	}
	// (switched off for a save or a restore that never came - the level changed, the restore failed, the save
	// state was gone: it goes back on all the same)
	if (g.queueResumeIn <= 0) g.queueResumeIn = kQueueResumeFrames;
	if (--g.queueResumeIn > 0) return;
	g.renderThreadOff = false;
	if (g.savedThreadCvars[0] != 0) engine::Execute("mat_queue_mode %d", g.savedThreadCvars[0]);
}

void RestoreRendering() {
	g.queueResumeIn = 0;
	g.restorePending = false;
	if (!g.queuePinned) return;
	g.queuePinned = false;
	g.renderThreadOff = false;
	for (size_t i = 0; i < std::size(kThreadCvars); i++)
		if (g.savedThreadCvars[i] != 0) engine::Execute("%s %d", kThreadCvars[i], g.savedThreadCvars[i]);
	if (g.savedThreadedParticles != 0) engine::Execute("r_threaded_particles %d", g.savedThreadedParticles);
}

// Forgets the save state and has ChocolateLoading free it
void DropSnapshot() {
	if (g.snapshotHandover < 0) return;
	g.snapshotHandover = -1;
	if (engine::CommandExists("snap_drop")) engine::Execute("snap_drop %s", kSnapshotName);
}

// Save states and demos. ChocolateLoading keeps every file open that a save state holds, so that a restore can go
// back to it, and a demo being recorded when the snapshot was taken was one of them: SAR then couldn't rename it
// at the flags (sar_challenge_autostop 2, set by srconfigs' Challenge Mode category) and its error closed the
// game. So no snapshot is taken while a demo is recording: the demo is stopped just before the save and a new one
// starts once the save is done (record "", which SAR names as srconfigs asks; in Challenge Mode the same name the
// level's own demo had). A restore stops the attempt's demo the same way and starts the next one after it. Every
// attempt then has a demo of its own, as it would with the level restarted for each; SAR stops and renames a
// finished one at the flags, and the save state stays for the next attempt.
constexpr int kDemoAfterSaveFrames = 20;    // (ChocolateLoading takes the snapshot within the next few frames)
constexpr int kDemoAfterRestoreFrames = 2;

void StopDemoForSnapshot() {
	if (!engine::RecordingDemo()) return;
	engine::Execute("stop");
	g.demoToRestart = true;
}

// Before a restore: the attempt's demo stops, and the next attempt gets one when there was a demo since the
// snapshot, recording still or already stopped and renamed by SAR at the flags
void StopDemoForRestore() {
	StopDemoForSnapshot();
	if (g.demoSinceSnapshot) g.demoToRestart = true;
	g.demoSinceSnapshot = false;
}

// Starts the demo stopped for a save state again, when it is due (every frame, paused or not)
void TickDemoRestart() {
	if (!g.demoToRestart) return;
	// (not while a restore is on its way: it is started after it)
	if (g.snapshotLoading && std::chrono::steady_clock::now() - g.snapshotAskedAt < std::chrono::seconds(3)) return;
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

// Called every tick of a lead-in: takes the snapshot when it is due. The trainer's own bookkeeping is set before
// the snapshot is stored, so that it holds the same values whether or not a restore brings this plugin's memory
// back along with the game's.
void TakeSnapshotIfDue() {
	if (g.snapshotIn <= 0) return;
	if (!SnapshotsUsable()) {
		g.snapshotIn = 0;
		return;
	}
	SingleThreadedRendering();  // (normally on already, since the catch-up: see TickCatchUp)
	if (--g.snapshotIn > 0) return;
	if (!ThreadsSettled()) {
		g.snapshotIn = 1;  // (a tick later, until the render thread is gone)
		return;
	}
	g.snapshotHandover = g.handover;
	g.snapshotStartScreenOpen = !g.startScreenClosed;
	g.snapshotWatch = g.watch;
	g.snapshotCursor = g.cursor;
	engine::FlushScript();
	// The game keeps its coroutines (Steam's game coordinator jobs run in them) in a list per thread, made the
	// first time a thread creates one and found again through a thread-local pointer, which a restore doesn't bring
	// back: a list first made after the save was freed by the restore while the pointer still led there, and the
	// next job crashed the game (Coroutine_Create, mp_coop_fling_3 at the flags, a few seconds after a restore).
	// So the main thread's list is made before every save.
	engine::MakeCoroutineList();
	StopDemoForSnapshot();
	g.snapshotHoldsDemo = engine::RecordingDemo();
	g.demoSinceSnapshot = false;
	engine::Execute("snap_save %s", kSnapshotName);
	g.demoRestartIn = kDemoAfterSaveFrames;
	g.queueResumeIn = kDemoAfterSaveFrames + kQueueResumeFrames;
}

// Returns to the snapshot of this very attempt, if there is one. True when it did.
bool RestoreSnapshot(int handover, bool watch) {
	if (!SnapshotsUsable() || g.snapshotHandover != handover || g.snapshotWatch != watch || g.slots.empty()) return false;
	SingleThreadedRendering();
	if (!ThreadsSettled()) {
		// (the render thread has to stop first: the attempt goes on until it has, see TickPendingRestore)
		g.restorePending = true;
		g.pendingHandover = handover;
		g.pendingWatch = watch;
		return true;
	}
	g.watch = watch;
	g.handover = handover;
	g.segment = g.route.SegmentAt(handover);
	g.cursor = g.snapshotCursor;
	g.mode = watch ? Mode::Watch : Mode::LeadIn;
	g.snapshotIn = 0;
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
	g.snapshotLoading = true;
	g.snapshotAskedAt = std::chrono::steady_clock::now();
	g.demoRestartIn = kDemoAfterRestoreFrames;
	return true;
}

}  // namespace trainer
