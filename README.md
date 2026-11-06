# P2 Practice

Practice a Portal 2 speedrun against a demo. Give it a run (a `.dem`, or both players' demos of a co-op run) and
it puts you on that run's route, in the real game:

- **Ghosts** of the recorded players run the route beside you, animated the way the game animates a player,
  drawn as wireframes in their portal colours (or as solid robots, if you prefer).
- **A live clock** shows how far ahead of or behind the run you are, with a split for every part. At the
  Challenge Mode flags you get the time your run would have had.
- **Parts.** The run is cut into parts at its portal shots, portal trips and doors. Start from any part, or from
  any time: the run plays itself up to that moment (you ride along and see the keys it holds), then hands you
  the player with the run's exact position, aim, speed and portals.
- **Co-op alone.** Pick ATLAS or P-body. The other robot is a real second player, driven through its half of
  the run from the demo: it moves, places its portals and presses its buttons on time, and you can stand on it.
- **Co-op together.** With a real partner, both of you get your own ghost and clock. Only the host needs the
  plugin; the partner sees the ghosts and text too, and can type the `prac_` commands.
- **Ride along.** Watch the whole run from the runner's eyes, inside the level, at any speed.
- **Replay and compare.** Play another run in the level against the loaded one, or print a part-by-part table
  of where it gains and loses.
- **The route drawn in the level**, with a mark and an aim line for every portal shot (host only).

It is a standalone server plugin (`p2practice.dll`) with a small launcher (`P2Practice.exe`). It works beside
SAR and p2boost: it installs no hooks and patches nothing. Everything it does goes through the game's own
plugin callbacks, console commands and VScript, so there is nothing for another plugin to collide with.

Practice moves players, objects and portals, so it turns `sv_cheats` on, and keeps it on in every level it has
worked in until that level is left or loaded again (also after `prac_stop`, also if you switch it off by hand).
With cheats on the game records no Challenge Mode score and SAR submits nothing to the boards: nothing done in
a practice level can end up on a leaderboard.

## Install

1. Copy `p2practice.dll` into the Portal 2 folder (next to `portal2.exe`), or let the launcher do it.
2. Start the launcher, pick a run, press **Launch**. Or, in the game's console: `plugin_load p2practice`.

Load the plugin from a cfg or the console, not with `+plugin_load` if another plugin is loaded that way too:
the game honours only one `+plugin_load` on its command line.

## The launcher

`P2Practice.exe` lists the demos in the folders you add (co-op pairs are shown as one run), shows the selected
run's parts, and starts the game with everything set up: which robot you play, bot or real partner, which part
to start from, practice or watch. If the game is already running it writes `cfg/p2practice_launch.cfg` and puts
`exec p2practice_launch` on the clipboard for you to paste into the console.

## Console commands

| Command | What it does |
| --- | --- |
| `prac_load <demo> [partner's demo]` | the run to practice against (path, or a name under `portal2/` or `portal2/demos/`) |
| `prac_info` | its parts and events |
| `prac_start [part]` | practice from a part |
| `prac_goto <seconds>` | practice from any time of the run |
| `prac_retry`, `prac_next`, `prac_prev` | again, next part, previous part |
| `prac_watch [part]` | ride along with the run |
| `prac_role <blue\|orange\|auto>` | co-op alone: the robot you play |
| `prac_replay <demo> [partner's demo]` | play another run in the level against the loaded one |
| `prac_compare <demo> [partner's demo]` | part-by-part times of another run against the loaded one |
| `prac_set [name value]` | settings (below) |
| `prac_auto <part> \| watch <part> \| off` | start by itself once the run's map is up (the launcher uses this) |
| `prac_stop` | stop |

Co-op alone needs the level in split screen: `ss_map mp_coop_doors`. Single player: `map sp_a2_laser_stairs`.
Maps are shown by their chamber names (Doors, Laser Stairs); commands take the file names as always.

Settings (`prac_set` alone lists them): `leadin` (seconds the run plays itself before you take over, 1.5),
`ghost`, `ghoststyle` (`wirelow`: a wireframe of the model's coarsest mesh; `wire`: of its full mesh; `solid`),
`hud`, `keys`, `path`, `partner` (the bot), `loop`, `autoreset` (retry when this far behind), `speed` (game
speed), `reset` (`auto`, `soft`, `reload`), `snapshots`.

The wireframe ghosts are copies of the game's own player models that the plugin writes once, under the game
folder (`models/p2practice/`, `materials/p2practice/ghost/`). Chell's model has one level of detail, so her
`wirelow` ghost is the full mesh. A partner over the network doesn't have those files, so with one connected
the ghosts are shown solid.

Useful binds: `bind f5 prac_retry`, `bind f6 prac_next`, `bind f7 prac_prev`.

## How a part starts

Getting back to the start of a part means getting the level back too: doors, panels, cubes, and the things a
level does on its own clock (the lift you arrive in, droppers, timed doors). The plugin knows from the demo
when the level's state changed (SAR's records) and what the level's clock showed when the recording began.

- If your last attempt didn't get past a change of the level, and the level's clock hasn't run past the part's
  start, a retry is instant: players, objects and portals are put back.
- Otherwise the level restarts and the run is replayed quickly from the level's first tick up to the part
  ("setting up the level..."). The real players stand in for the run's players, on the level's own clock. They
  are put where the run's player was when each game frame began and the game's own movement carries them
  through it, so they ride the lift and touch every trigger in the same frame the run did, tricks included (a
  button pressed and released within one frame, a trigger skipped at speed). What the run's players carried is
  moved along the path it took; what moved on its own (thrown, rolling, in a funnel) is thrown the same way
  and left to the game's physics, put back on its path only if it strays. Every button, door and laser ends up
  as the run had it.
- A run made in Challenge Mode is practiced in Challenge Mode (the restart switches to it): the finish flags and
  the game's own timer are there, no saves are kept, and a death starts the level again. The leaderboard screens
  the game opens at the start and the finish of a Challenge Mode level are closed again by the plugin.
- At the handover you get the run's exact position, aim, speed and portals, and whatever the run was carrying
  at that moment is put in your hands.

`prac_set reset soft` never restarts, `reload` always.

With [ChocolateLoading](https://github.com/Blenderiste09/ChocolateLoading) running and `prac_set snapshots 1`,
the plugin stores a memory snapshot at the start of each lead-in and a retry returns to it in milliseconds
instead. This path is written but has not been tested yet.

## What a demo gives

- A **host's demo** recorded with SAR gives everything: both players' paths, portals, the level's events and
  what was used. A co-op partner's path then comes from 20 snapshots a second and has no key display.
- Adding the **partner's own demo** gives their exact inputs and aim too.
- A demo without SAR's data still works, but the clock starts at the spawn rather than the timer, and retries
  can't tell when the level changed (use `prac_set reset reload`).

## Limits

- Objects: a demo holds an object's position about 20 times a second while the recording player can see it,
  and nothing while they can't (the plugin then leaves the object alone). A prop (cube, ball, box) gets the
  run's speed through a catapult trigger of the plugin's; a turret can only be placed. From the handover on,
  everything you touch follows the game's physics.
- Co-op: the game makes a robot that flies fast catch what it looks at (no key needed). Driven robots do the
  same, and let go when the run's robot did.
- In co-op the level waits for both players before it starts, so there is no level clock to join: the run's
  clock starts when both robots stand in the level.
- In a Challenge Mode run the finish is the level's own flags: touching them (both robots, in co-op) ends the
  attempt and shows the time the flags would have shown. The game itself shows none with cheats on, and nothing
  is saved or submitted. A run made outside Challenge Mode has no flags: the finish is where the run's player
  finished.
- A demo recorded on an altered copy of a map can't be followed on the real one (the level does different
  things); the plugin doesn't detect this, the run just won't line up.
- The route line and portal marks are debug overlays: only the host sees them.

## Building

Visual Studio 2019 or newer (C++ x86), CMake, and for the launcher the .NET Framework 4.8 compiler that ships
with Windows.

```
cmake -S . -B build\x86 -G "Visual Studio 16 2019" -A Win32
cmake --build build\x86 --config Release
launcher\build.bat
```

- `core/` reads demos (no dependencies; ported from the demo viewer's parser and checked against it).
- `plugin/` is the plugin: `route.cpp` builds the route from demos, `trainer.cpp` runs the session,
  `engine.cpp` is the only file that touches the game, `helper_nut.h` is its VScript half.
- `tools/routeinfo` is `p2route.exe`, which describes demos for the launcher (`p2route info --text <demo>`).
- `tools/testkit` starts hidden test copies of the game and runs the map tests (`maptest.py`).
