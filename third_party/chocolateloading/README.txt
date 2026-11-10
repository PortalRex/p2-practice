ChocolateLoading - RAM snapshots for Portal 2
by Blenderiste09: https://github.com/Blenderiste09/ChocolateLoading
Redistributed with P2 Practice with the author's permission.

P2 Practice uses it for save states (the launcher's SAVE STATES switch): a retry puts the whole level back as it
was a moment into the part, in about 10 ms.

This build is ChocolateLoading at commit 9220fe1 (2026-10-20) with fixes and additions of P2 Practice's. fixes.patch has
all of them together; patches\ has them as separate commits (git am). NOTE_FOR_BLENDERISTE09.md explains the fixes
for the author.

1. 16-byte alignment. The heap that holds the game's memory handed out blocks aligned to 8 bytes; tier0's own
   heap aligns to 16, and the game's SSE code relies on it. On gel maps the paint-contact code then faulted
   (server.dll, "read of FFFFFFFF") with ChocolateLoading merely loaded, no snapshot needed: reproduced on
   sp_a3_jump_intro. Blocks are now multiples of 16 with every payload on a 16-byte boundary.

2. Steam overlay. With the overlay enabled, Steam's gameoverlayrenderer.dll hooks VirtualAlloc and stood between
   the game and Windows on every memory reservation, so the check "was this reserved by game code?" rejected the
   engine's hunk (where the entity table lives). Snapshots then left the entity table out and the game crashed a
   moment after a restore. An overlay frame that is forwarding a call to kernelbase/kernel32 is now skipped; the
   overlay's own calls are still rejected.

3. Steam launch options. chocoload.exe can stand in Portal 2's launch options ("<Portal 2>\chocoload.exe"
   %command%), so Steam's Play starts the game with save states ready, no launcher needed. Steam puts the game's
   own path first: it is skipped. chocoload then hides its console window and waits for the game, as Steam counts
   the game as running while its launch command does. Run any other way, chocoload behaves as before.

4. Heap lock report. A game froze once with every thread waiting for the lock of the heap that holds the game's
   memory and no holder in sight. The lock now records its holder, and a thread that has waited more than 5 s
   writes the holder thread, whether it still exists and where it took the lock to chocolateloading.log.

5. Cvars of plugins. A plugin's cvars (SAR's sar_*) are objects in the plugin's memory, which a restore leaves
   alone, but their value strings are allocated in the snapshotted heap: a value set after the save was freed by
   the restore and the cvar kept pointing at it. SAR's sar_record_prefix then read as garbage: demos with mangled
   names, SAR's rename at the flags failing (the game closes), once a CRT fast fail in strftime. Before a restore
   the text of every such cvar is noted, after it each gets a fresh copy. The same commit copies the arena pages
   written since the save back with the copy workers, by 32 MB window (5-6 ms on the main thread, now ~3 ms).

6. No wait when the threads are already drained. A save or restore waited 4 frames for the threading cvars to
   take effect even when they had been 0 all along (P2 Practice keeps them off during a practice session): once
   they have been 0 for 4 frames, an operation runs at the next frame.

7. Device hooks of other plugins. A save pins every Direct3D object the engine made, and "the engine made it" was
   judged by the return address of the device call. p2boost (with -vulkan) wraps CreateTexture and passes the call
   on, so the return address was p2boost's: every texture made after it loaded was left out (176 pinned instead of
   about 900 on Turret Warehouse), and one freed after the save was used again after a restore (DXVK heap fault,
   the game closes after a few retries). A call that arrives from another module now counts as the engine's when
   the same arguments sit further up the stack under a return address in the shader API. Objects such a module
   creates for itself stay untracked; a save writes their number to the log.

8. Device state. The shader API remembers what it last set on the Direct3D device (render and sampler states,
   bound textures and shaders, shader constants) and skips a call that would set the same value again. A restore
   took that memory back to the save while the device kept what the discarded timeline had left on it, so the
   engine skipped exactly the calls that would have put the device right: on Turret Warehouse the room's projected
   light was drawn without its shadow map and the walls were flat yellow for seconds after a retry. A save now
   also stores the device's state in a Direct3D state block and a restore applies it. "snap_opt device_state 0"
   turns that off; the export ChocoRestoresDeviceState tells a plugin whether it is on (P2 Practice then no
   longer makes the engine send its render state again after a restore).

9. Queued rendering stays on. A save or a restore used to switch the game's queued rendering (mat_queue_mode)
   off and on again around itself: the render thread stopped and started every time, which cost a stutter, and
   restoring into a game whose render thread was still running broke the renderer (wrong lightmaps, crashes on
   that thread). An operation now runs inside the material system's own lock (IMaterialSystem::Lock, which waits
   for the render thread to finish its queue and holds it there), so the render thread is idle for exactly the
   length of the operation and nothing is switched. "snap_opt render_lock 0" gives the old behaviour back.

10. Status for plugins. The export ChocoGetSnapshotStatus tells a plugin how many saves and restores have been
   done and how many requests failed (refused, gone wrong, or dropped by a level change), and whether queued
   rendering stays on (9). P2 Practice compares the counts with what they were when it sent snap_save or
   snap_load, so it knows the moment its operation is done or has failed instead of waiting a number of frames,
   and it offers save states only with a ChocolateLoading that has this export.

Files: chocoload.exe (starts Portal 2 with the DLL loaded before the engine) and chocolateloading.dll. The
launcher copies both into the Portal 2 folder when save states are on.
