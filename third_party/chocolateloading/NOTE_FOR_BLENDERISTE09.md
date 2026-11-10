# Two ChocolateLoading fixes from P2 Practice

Hi Blenderiste09! Thanks again for letting P2 Practice ship ChocolateLoading. Its save states are built on your
snapshots: a retry puts the level back as it was a moment into the run, in about 10 ms. While testing that across
many maps we hit two bugs in ChocolateLoading. Both fixes are attached as patches against your commit 9220fe1
(2026-10-20), one commit each:

```
git am 0001-arena-align-every-payload-to-16-bytes-as-tier0-s-hea.patch
git am 0002-callsite-let-a-reservation-through-the-Steam-overlay.patch
```

Each one stands on its own, so you can take either without the other. `build.ps1 -Test` prints ALL OK with both.

## 1. Gel maps crash: arena blocks were only 8-byte aligned

**Symptom.** The game crashes in the paint-contact code in server.dll ("read of FFFFFFFF") on gel maps. It happens
whenever chocolateloading.dll is loaded, before any snapshot is taken. It reproduces on every load of
sp_a3_jump_intro.

**Cause.** Arena blocks were multiples of 8, with the payload 8 bytes after the header. tier0's own allocator
returns 16-byte aligned memory for anything bigger than 8 bytes, and some of the game's SSE code relies on that.

**Fix (arena.cpp).**
- `ALIGN` is now 16.
- The heap starts 8 bytes before a 16-byte boundary. Every header then sits at 8 mod 16, so every payload is
  16-byte aligned.
- `grow()`'s fallback near the end of the reservation is masked with `ALIGN - 1` instead of 7.

Every path that makes a block (split, coalesce, grow, in-place realloc, the free ranges a restore rebuilds) adds or
subtracts sizes that are multiples of 16, so the alignment holds on all of them. `Check()` and `arena_test` now
verify it. As a check of the test itself: with the old heap start, the test fails at once with "alloc not 16-byte
aligned".

**Costs worth knowing.**
- Small blocks now carry up to 15 bytes of padding instead of 7.
- Below 256 bytes, the TLSF classes are still 8 bytes wide, so only every other slot in FL0/FL1 gets used. Results
  are still exact; it just wastes some bitmap. If you want to tidy that up: `FL_SHIFT = SL_LOG + 4`, and
  `sl = size >> 4` for small sizes.
- On PC, `MemAlloc_AllocAligned` over-allocates through the plain `Alloc` call, so aligned allocations already
  worked and still do.

## 2. Steam overlay: the engine hunk was never tracked

**Symptom.** With the Steam overlay enabled, a restore crashes the game a moment later. With the overlay off, it
works.

**Cause.**
- gameoverlayrenderer.dll detours kernel32/kernelbase functions, VirtualAlloc among them. So every reservation the
  game makes has an overlay frame between kernelbase and the game module.
- `DirectGameCaller` stopped at that frame (class "other") and rejected the call. The engine's hunk (the 48 MB
  CMemoryStack) was therefore never tracked, and snapshots left it out.
- We found it by temporarily logging the callers of big untracked reservations, which showed
  `kernelbase:P gameoverlayrenderer:O engine:G`.

**Fix (callsite.cpp).**
- The overlay gets its own class, `C_HOOK`.
- An overlay frame is skipped only when the frame just inside it is kernelbase/kernel32, i.e. the detour is
  forwarding its caller's call.
- Two overlay frames in a row means the overlay's own code calling through its own detour. That is still rejected,
  so the overlay's private memory and files stay out of snapshots and fileguard's registry. Because the class is
  shared, fileguard gets the same rule.

**Tested.** Portal 2 with the overlay loaded, sp_a2_laser_stairs:
- The 48 MB engine.dll hunk is listed by vatrack.
- During startup, 35 overlay frames were skipped as forwarding and 20 overlay calls were rejected as its own.
- One save followed by 8 restores, all fine.

**Not covered.**
- Another injected DLL could detour VirtualAlloc the same way (Discord, RTSS, OBS, NVIDIA's overlay). That would
  bring the same symptom back. I didn't check whether any of them do.
- A more general fix would follow the `jmp` at the start of kernelbase!VirtualAlloc and treat whatever module it
  lands in as the hooker. That's more code, so I didn't do it.
- The rule still lets one case through: overlay code reached from a game call (e.g. its D3D Present detour) that
  calls VirtualAlloc straight through its trampoline, without passing its own detour. We saw no sign of that, but
  the stack walk can't tell it apart.

## A later one: wrong lighting after a restore

This one is patch 8 (`0008-d3dguard-save-the-device-s-state-with-a-snapshot-and.patch`). It builds on patch 7
(d3dguard counting objects made through another plugin's device hook), so it wants the patches before it applied.

**Symptom.** On Turret Warehouse (mp_coop_tbeam_end) with `-vulkan`, the walls were drawn flat yellow for a few
seconds after some restores: the room's projected light without its shadow map.

**Cause.** The shader API remembers what it last set on the device and skips a call that would set the same value
again. That memory is in the arena, so a restore takes it back to the save. The device itself keeps whatever the
discarded timeline left on it. The two disagree, and the engine skips exactly the calls that would fix it.

**Fix (d3dguard.cpp).** `OnSave` creates a `D3DSBT_ALL` state block for the slot (through the device's own
`CreateStateBlock`, not the hook) and `AfterRestore` applies it last, once the pinned objects have their
references back. The block is released when the slot is saved again or dropped, and before a device reset.
`snap_opt device_state 0` turns it off, and the DLL exports `ChocoRestoresDeviceState()` so a plugin can tell.

**Tested.** Turret Warehouse with `-vulkan` and p2boost, screenshots at several moments after each restore: yellow
frames with the option off, none with it on.

## Something we saw but did not fix in ChocolateLoading

**Particle crash after a restore.** A few seconds after some restores on sp_a2_laser_stairs, the game crashed on a
vstdlib job thread:
- Stack: client.dll `CParticleCollection::Simulate` → `C_OP_SetControlPointToPlayer`.
- The crash was sometimes `execute at <arena address>`, sometimes `read of 0000005D`.
- With `r_threaded_particles 0`, so particles are simulated on the main thread, 18 restores out of 18 were clean.
  Before that, about 1 restore in 5 crashed.
- P2 Practice now sets that cvar whenever snapshots are in use. Possibly the job pool still has particle work
  queued or in flight across the restore. You'd know better than us whether `snap_load`'s job-pool suspension
  should also cover that.

**fileguard overflow (seen once).** The log said `fileguard: ... REGISTRY OVERFLOWED`, meaning more than 2048 live
game files. It was in a test copy where we had been sending `snap_save` every few seconds during the map load. It
didn't come back in two clean reruns, so it may be nothing.

Thanks for ChocolateLoading. It's what makes instant retries possible.

PortalRex
