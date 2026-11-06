# P2 Practice

Practise a Portal 2 speedrun against a demo. Give it a run (a `.dem`, or both players' demos of a co-op run) and
it puts you on that run's route, in the real game:

- **Ghosts** of the recorded players run the route beside you.
- **A live clock** shows how far ahead of or behind the run you are, with a split for every part.
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

Practice moves players and places portals, so it turns `sv_cheats` on. Runs made while practising are flagged
as cheated by SAR and the boards, as they should be.

## Install

1. Copy `p2practice.dll` into the Portal 2 folder (next to `portal2.exe`), or let the launcher do it.
2. Start the launcher, pick a run, press **Launch**. Or, in the game's console: `plugin_load p2practice`.

Load the plugin from a cfg or the console, not with `+plugin_load` if another plugin is loaded that way too:
the game honours only one `+plugin_load` on its command line.

## The launcher

`P2Practice.exe` lists the demos in the folders you add (co-op pairs are shown as one run), shows the selected
run's parts, and starts the game with everything set up: which robot you play, bot or real partner, which part
to start from, practise or watch. If the game is already running it writes `cfg/p2practice_launch.cfg` and puts
`exec p2practice_launch` on the clipboard for you to paste into the console.

## Console commands

| Command | What it does |
| --- | --- |
| `prac_load <demo> [partner's demo]` | the run to practise against (path, or a name under `portal2/` or `portal2/demos/`) |
| `prac_info` | its parts and events |
| `prac_start [part]` | practise from a part |
| `prac_goto <seconds>` | practise from any time of the run |
| `prac_retry`, `prac_next`, `prac_prev` | again, next part, previous part |
| `prac_watch [part]` | ride along with the run |
| `prac_role <blue\|orange\|auto>` | co-op alone: the robot you play |
| `prac_replay <demo> [partner's demo]` | play another run in the level against the loaded one |
| `prac_compare <demo> [partner's demo]` | part-by-part times of another run against the loaded one |
| `prac_set [name value]` | settings (below) |
| `prac_auto <part> \| watch <part> \| off` | start by itself once the run's map is up (the launcher uses this) |
| `prac_stop` | stop |

Co-op alone needs the level in split screen: `ss_map mp_coop_doors`. Single player: `map sp_a2_laser_stairs`.

Settings (`prac_set` alone lists them): `leadin` (seconds the run plays itself before you take over, 1.5),
`ghost`, `hud`, `keys`, `path`, `partner` (the bot), `loop`, `autoreset` (retry when this far behind), `speed`
(game speed), `reset` (`auto`, `soft`, `reload`), `snapshots`.

Useful binds: `bind f5 prac_retry`, `bind f6 prac_next`, `bind f7 prac_prev`.

## How a part starts

Getting back to the start of a part means getting the level back too: doors, panels, cubes. The plugin knows
from the demo (SAR's records) when the level's state changed. If your last attempt didn't get past such a moment,
a retry is instant: players and portals are put back. Otherwise the level restarts and the run is replayed
quickly from its beginning up to the part ("setting up the level..."), with the real players as stand-ins, so
every trigger, button and door ends up as the run had it. `prac_set reset soft` never restarts, `reload` always.

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

- Objects: driven players pick up, carry and drop what the recorded players did, and an object that came to
  rest in the run is put on its spot. An object in flight follows the game's physics, which can differ.
- When you take over from a moment where the run is carrying something, the driven player hands it to you only
  if the lead-in included the pick-up.
- The finish is reached when you get to where the run finished, not by the level's own end trigger.
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
