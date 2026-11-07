ChocolateLoading - RAM snapshots for Portal 2
by Blenderiste09: https://github.com/Blenderiste09/ChocolateLoading
Redistributed with P2 Practice with the author's permission.

P2 Practice uses it for save states (the launcher's SAVE STATES switch): a retry puts the whole level back as it
was a moment into the part, in about 10 ms.

This build is ChocolateLoading at commit 9220fe1 (2026-10-20) with two fixes. fixes.patch has both together;
patches\ has them as two commits (git am). NOTE_FOR_BLENDERISTE09.md explains them for the author.

1. 16-byte alignment. The heap that holds the game's memory handed out blocks aligned to 8 bytes; tier0's own
   heap aligns to 16, and the game's SSE code relies on it. On gel maps the paint-contact code then faulted
   (server.dll, "read of FFFFFFFF") with ChocolateLoading merely loaded, no snapshot needed: reproduced on
   sp_a3_jump_intro. Blocks are now multiples of 16 with every payload on a 16-byte boundary.

2. Steam overlay. With the overlay enabled, Steam's gameoverlayrenderer.dll hooks VirtualAlloc and stood between
   the game and Windows on every memory reservation, so the check "was this reserved by game code?" rejected the
   engine's hunk (where the entity table lives). Snapshots then left the entity table out and the game crashed a
   moment after a restore. An overlay frame that is forwarding a call to kernelbase/kernel32 is now skipped; the
   overlay's own calls are still rejected.

Files: chocoload.exe (starts Portal 2 with the DLL loaded before the engine) and chocolateloading.dll. The
launcher copies both into the Portal 2 folder when save states are on.
