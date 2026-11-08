# P2 Practice

Practice a Portal 2 speedrun against a demo. Give it a run (a `.dem`, or both players' demos of a co-op run) and
it puts you on that run's route, in the real game:

- **Ghosts** of the recorded players run the route beside you, animated the way the game animates a player,
  drawn as wireframes in their portal colours (or as solid robots, if you prefer).
- **A live clock** shows how far ahead of or behind the run you are, with a split for every part and your speed
  next to the record's in small print. At the Challenge Mode flags you get the time your run would have had.
- **Parts.** The run is cut into parts at its portal shots, portal trips and doors. Start from any part, or from
  any time: the run plays itself up to that moment (you ride along), then hands you
  the player with the run's exact position, aim, speed and portals.
- **Co-op alone.** Pick ATLAS or P-body. The other robot is a real second player, driven through its half of
  the run from the demo: it moves, places its portals and presses its buttons on time, and you can stand on it.
- **Co-op together.** With a real partner, both of you get your own ghost and clock. Only the host needs the
  plugin; the partner sees the ghosts and text too, and can type the `prac_` commands.
- **Ride along.** Watch the whole run from the runner's eyes, inside the level, at any speed.
- **Replay and compare.** Play another run in the level against the loaded one, or print a part-by-part table
  of where it gains and loses.
- **The route drawn in the level**, with a mark and an aim line for every portal shot (host only).
- **An in-game menu** (Insert): your demos, the leaderboards, how to practice and every setting, without leaving the
  game. No launcher needed.

It is a standalone server plugin (`p2practice.dll`), with an optional launcher (`P2Practice.exe`). It works beside
SAR and p2boost. The practice itself goes through the game's own plugin callbacks, console commands and VScript;
only the menu and HUD hook the game (see below), the way SAR does, and both work whichever plugin loads first.

Practice moves players, objects and portals, so it turns `sv_cheats` on, and keeps it on in every level it has
worked in until that level is left or loaded again (also after `prac_stop`, also if you switch it off by hand).
With cheats on the game records no Challenge Mode score and SAR submits nothing to the boards: nothing done in
a practice level can end up on a leaderboard.

## Install

1. Unzip the release anywhere (`P2Practice.exe`, `p2practice.dll`, `p2route.exe`, `chocolateloading\`).
2. Start `P2Practice.exe`, pick a run, press **Launch**: it puts the plugin into the Portal 2 folder itself. Or
   copy `p2practice.dll` next to `portal2.exe` and type `plugin_load p2practice` in the game's console.

Load the plugin from a cfg or the console, not with `+plugin_load` if another plugin is loaded that way too:
the game honours only one `+plugin_load` on its command line.

## The in-game menu

`plugin_load p2practice` (in your autoexec, or the console), then **Insert** in the menu or in a level opens P2
Practice's menu; Insert or Esc closes it (another key: SETTINGS, or `prac_menukey`). While it is open the game gets no keys or clicks. Four pages:

- **PRACTICE**: the loaded run: its time, chamber and parts, a bar of the run with where you take over. Pick a part
  (or drag the take-over point), Practice or Watch, blue or orange, bot or real partner, lead-in and game speed,
  then START. When the run's map isn't up, START loads it (`ss_map` for co-op with the bot); with a real partner
  the run is armed and starts once you both are in. RETRY, NEXT PART and STOP while practicing.
- **RUNS**: every demo in your folders (searched recursively; the game's `demos` folder to begin with), co-op pairs
  as one run, read in the background and cached in `portal2/p2practice/library.cache`. Type to search; FOLDERS
  adds a folder (paste its path with Ctrl+V) or removes one. Double-click a run, or LOAD THIS RUN.
- **BOARDS**: the Challenge Mode leaderboards at board.portal2.sr by chapter. Pick a chamber, double-click a run
  (or GET DEMO): its demo(s) are downloaded into `portal2/demos/p2practice/boards` and loaded.
- **SETTINGS**: ghosts (style, colour, opacity), what's on screen, retries, save states and the menu's key.

Settings are kept in `cfg/p2practice/settings.cfg` (run when the plugin loads). The HUD (clock and gap, part,
speed, flashes) is drawn by the plugin; a partner over the network still gets the game's text lines.

Console: `prac_menu [practice|runs|boards [map]|settings|close]`, `prac_menukey <key>`, `prac_folder_add
<folder>`, `prac_folders`, `prac_menu_tour [record <video>]` (a scripted walk through the menu, recorded with
SAR's renderer, for showing it).

**Save states without the launcher.** ChocolateLoading has to start with the game. Copy `chocoload.exe` and
`chocolateloading.dll` (from the release's `chocolateloading` folder) next to `portal2.exe`, and once, in Steam:
Portal 2 > Properties > Launch Options, put `"<Portal 2 folder>\chocoload.exe" %command%` first (the menu's
SETTINGS page shows the exact line and copies it). Steam's Play then starts the game with save states ready.
With another wrapper already there, chain them: `"...\P2Multi.exe" "...\chocoload.exe" %command% ...`.

How it draws: after the game paints its 2D panels each frame, the plugin draws with the game's own surface
(`VGUI_Surface031`) and reads the mouse and keys from its input system. While the menu is open it frees the
cursor and keeps keys and clicks from the game and its menus. That takes four patched function-table slots
(engine VGUI paint, the surface's cursor lock, the client's key events, VGUI's button input), each patched in
the table the object uses at that moment (SAR swaps in copies of these tables) and put back when the plugin
unloads; the DLL then stays loaded until the game closes, as a copy SAR made may still hold its hooks. The
approach follows [Photon](https://github.com/aIIison/photon)'s menu (written anew, not copied). If any interface
is missing, the plugin works on without its menu, with the game's text HUD.

## The launcher

`P2Practice.exe` lists the demos in the folders you add (co-op pairs are shown as one run), shows the selected
run's parts, and starts the game with everything set up: which robot you play, bot or real partner, which part
to start from (or any second of the run: TAKE OVER AT), practice or watch, what is shown and in which colour
and opacity the ghosts are drawn. If the game is already running it writes `cfg/p2practice/launch.cfg` and
puts `exec p2practice/launch` on the clipboard for you to paste into the console.

**SAVE AS CFG** writes the same session under a name of your own (`cfg/p2practice/<name>.cfg` in the game
folder): later, `exec p2practice/<name>` in the game's console starts that practice again without the launcher,
from the menu or inside a level. Cfgs saved by earlier versions straight into `cfg/` are moved into
`cfg/p2practice/` the next time you launch or save one. The demo is used where it is (copied under `demos/p2practice/saved` only when the game can't read its
path). The cfg's first line loads the plugin; when it is already loaded that line just says so.

**BOARDS** shows the Challenge Mode leaderboards from [board.portal2.sr](https://board.portal2.sr): pick a
chamber, and every run on its board is listed with its time, players, date and whether a demo is there. Enter or
Get demo fetches the demo (both robots' for co-op) into `demosoards` next to the launcher, after which the run
is read and ready to launch like any other; a run whose demo is already in one of your folders shows as saved.
Co-op runs are paired by their time and date, so a run whose partner has since set a better time appears alone
(its one demo still works: the partner's path then comes from the host's snapshots).

The window follows Windows' light or dark mode; Game > Light / Dark sets it.

## Console commands

| Command | What it does |
| --- | --- |
| `prac_load <demo> [partner's demo]` | the run to practice against (path, or a name under `portal2/` or `portal2/demos/`) |
| `prac_info` | its parts and events |
| `prac_start [part]` | practice from a part |
| `prac_goto <seconds>` | practice from any time of the run |
| `prac_nudge <seconds>` | take over this many seconds later in the run (negative: earlier) and start again |
| `prac_retry`, `prac_next`, `prac_prev` | again, next part, previous part |
| `prac_watch [part]` | ride along with the run |
| `prac_role <blue\|orange\|auto>` | co-op alone: the robot you play |
| `prac_replay <demo> [partner's demo]` | play another run in the level against the loaded one |
| `prac_compare <demo> [partner's demo]` | part-by-part times of another run against the loaded one |
| `prac_set [name value]` | settings (below) |
| `prac_auto [watch] <part> \| [watch] at <seconds> \| off` | start by itself once the run's map is up (the launcher uses this) |
| `prac_stop` | stop |

Co-op alone needs the level in split screen: `ss_map mp_coop_doors`. Single player: `map sp_a2_laser_stairs`.
Maps are shown by their chamber names (Doors, Laser Stairs); commands take the file names as always.

Settings (`prac_set` alone lists them): `leadin` (seconds the run plays itself before you take over, 1.5),
`ghost`, `ghoststyle` (`basic`: a wireframe of the model's coarsest mesh with its small triangles left out;
`wirelow`: that mesh whole; `wire`: the full mesh; `solid`), `ghostcolor` (`team` for the robots' own colours, or
three numbers 0-255), `ghostopacity` (percent; below 100 the ghosts are see-through),
`hud` (the clock, the gap to the record and the flashes), `speedometer` (your speed next to the record's, bottom
left), `path`, `partner` (the bot), `loop`, `autoreset` (retry when this far behind), `speed` (game
speed), `reset` (`auto`, `soft`, `reload`), `challenge` (practice every run in Challenge Mode, 1), `snapshots`.

The wireframe ghosts are copies of the game's own player models that the plugin writes once, under the game
folder (`models/p2practice/`, `materials/p2practice/ghost/`). Chell's model has no coarser mesh (the game never
shows her from outside), so her ghost is always the full mesh. A partner over the network doesn't have those
files, so with one connected the ghosts are shown solid.

Useful binds: `bind f5 prac_retry`, `bind f6 prac_next`, `bind f7 prac_prev`, `bind f8 "prac_nudge -1"`,
`bind f9 "prac_nudge 1"` (take over a second earlier or later).

## How a part starts

Getting back to the start of a part means getting the level back too: doors, panels, cubes, and the things a
level does on its own clock (the lift you arrive in, droppers, timed doors). The plugin knows from the demo
when the level's state changed (SAR's records) and what the level's clock showed when the recording began.

- If the run records no change of the level (a door, a button, a panel) between the lead-in's start and where
  your last attempt got, and the level's own clock hasn't run past the part's start (in single player it always
  has after an attempt), a retry is instant: players, objects and portals are put back.
- Otherwise the level restarts and the run is replayed quickly from the level's first tick up to the part
  ("setting up the level..."). The real players stand in for the run's players, on the level's own clock. They
  are put where the run's player was when each game frame began and the game's own movement carries them
  through it, so they ride the lift and touch every trigger in the same frame the run did, tricks included (a
  button pressed and released within one frame, a trigger skipped at speed). What the run's players carried is
  moved along the path it took; what moved on its own (thrown, rolling, in a funnel) is thrown the same way
  and left to the game's physics, put back on its path only if it strays. Every button, door and laser ends up
  as the run had it.
- Every run is practiced in Challenge Mode (the restart switches to it), whether or not it was made in it: the
  finish flags and the game's own timer are there, no saves are kept, and a death starts the level again. The
  leaderboard the game opens a few seconds into the level (which pauses the game: it would stall the run playing
  itself, or land on an early handover) is kept from opening while practicing; the one at the flags comes up as
  it always does, with the attempt's time in its Final box (the game itself shows "-" there with cheats on), and
  the plugin's time next to the record's shows once it is closed. Its **Replay Map** button (and the menu's
  Restart Level) starts the same attempt again, as `prac_retry` does, instead of restarting the level. In
  single player a death restarts the level and the attempt starts again by itself (in co-op a robot just
  respawns). `prac_set challenge 0` keeps a run's own mode instead.
- At the handover you get the run's exact position, aim, speed and portals, and whatever the run was carrying
  at that moment is put in your hands.

The launcher's RETRY row picks between these: Auto (as above), Instant (`prac_set reset soft`: no restart for
the level's sake; buttons, doors and panels stay as the last attempt left them while what the run carried or
threw is put back, so only for parts where that doesn't matter) and Restart the level (`prac_set reset reload`:
always). Whatever the choice, the level restarts after a finish at the flags (they are gone; with save states
the retry goes back to the save state instead) and when it isn't in the run's mode. In co-op the driven partner keeps acting the run out after the handover, so with Auto most
retries of a part with buttons or doors in it restart the level; in single player the level's clock is past the
part after any attempt, so there Auto restarts every time.

### Save states

With [ChocolateLoading](https://github.com/Blenderiste09/ChocolateLoading) (RAM snapshots of the whole game)
the retry rules above don't matter: the plugin stores a snapshot a moment into each lead-in, and a retry puts
the whole level back as it was then, cubes, doors, buttons and all, in about 10 ms, then plays the rest of the
lead-in. The launcher's **SAVE STATES** switch does it all: it puts ChocolateLoading's `chocoload.exe` and
`chocolateloading.dll` (shipped in `chocolateloading\` next to the launcher) into the Portal 2 folder, starts
the game through `chocoload.exe` (Steam must be running: it starts `portal2.exe` itself) and sets
`prac_set snapshots 1`. From the console it is the same two things: start the game through `chocoload.exe`
and set `prac_set snapshots 1`. Without the launcher: Steam launch options, as in "The in-game menu" above.

ChocolateLoading is Blenderiste09's, shipped with the release with their permission. The build shipped carries
two fixes (`third_party/chocolateloading/fixes.patch`, explained in `third_party/chocolateloading/README.txt`):
with the Steam overlay enabled the engine's entity table was left out of snapshots (a crash a moment after a
restore), and its heap aligned memory to 8 bytes where the game's SSE code needs 16 (a crash on gel maps with
ChocolateLoading merely loaded). The stock build has both problems. A third change lets `chocoload.exe` run as
Portal 2's Steam launch options (`... %command%`), for save states without the launcher.

Snapshots are off with a partner over the network (the snapshot holds this computer's game only): retries then
restart the level as without save states. Each snapshot takes about 350 MB of memory (pagefile-backed); the
plugin keeps one, replaced at every new part. While save states are in use the plugin sets
`r_threaded_particles 0` (particles simulated on the main thread): particles simulated on the job threads
crashed the game a moment after some restores (seen on Laser Stairs). The setting lasts until the game closes.
It also sets `mat_queue_mode 0` (rendering on the main thread) until practice stops: ChocolateLoading switches
the render thread off and on around every save and restore, and the game crashed on that thread a few seconds
after restores (Lasers).

**Demos with save states.** A snapshot keeps every file open that was open when it was taken, the demo being
recorded too, so the demo is stopped just before each snapshot and before each restore, and a new one starts
right after (`record ""`, named by SAR as your srconfigs category asks: in Challenge Mode the name the level's
demo had; without SAR, `demos/p2practice/<map>_<date>_<time>`). Every attempt is in a demo of its own, as when
the level restarts for each, and SAR stops and renames a finished one at the flags as usual. The plugin only
starts a demo where one was being recorded.

## What a demo gives

- A **host's demo** recorded with SAR gives everything: both players' paths, portals, the level's events and
  what was used. A co-op partner's path then comes from 20 snapshots a second and has no key display.
- Adding the **partner's own demo** gives their exact inputs and aim too.
- A demo without SAR's data still works, but the clock starts at the spawn rather than the timer, and retries
  can't tell when the level changed (use `prac_set reset reload`).
- **Times.** A run ends where its demo shows the flags touched, and lasts exactly its board time when the demo
  has the boards' name (`<Chamber>_<hundredths>_<steam id>_<id>.dem`, as the launcher downloads them): the two
  players' SAR timers are a tick off the board in about one run in twenty, and some demos have none. Without
  a board time, the longer of the two SAR timers is used (the board's choice in every such pair checked).
  Attempts are counted exactly as the demos count the record: a record replayed against itself
  (`prac_replay` of the loaded demos) prints its time at the flags and "+0 ticks". Times show as the boards
  show them, and a time level with the record reads exactly as the record does.

## Limits

- Objects: a demo holds an object's position about 20 times a second while the recording player can see it,
  and nothing while they can't (the plugin then leaves the object alone). A prop (cube, ball, box) gets the
  run's speed through a catapult trigger of the plugin's; a turret can only be placed. From the handover on,
  everything you touch follows the game's physics.
- Co-op: the game makes a robot that flies fast catch what it looks at (no key needed). Driven robots do the
  same, and let go when the run's robot did.
- In co-op the level waits for both players before it starts, so there is no level clock to join: the run's
  clock starts when both robots stand in the level.
- The finish is the level's own Challenge Mode flags: touching them (both robots, in co-op) ends the attempt and
  shows the time the flags would have shown, on the game's own finish board too (with cheats on the game would
  show "-" there); nothing is saved or submitted. Where a level has no flags (or `prac_set challenge 0` keeps a run out of Challenge Mode), the
  finish is where the run's player finished.
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
- `tools/package.ps1` packs a release zip into `dist/`. It takes ChocolateLoading's two files from
  `launcher/bin/chocolateloading/`: build them from its repository with `third_party/chocolateloading/fixes.patch`
  applied.
- `prac_debug` alone prints the plugin's state for bug reports; its development switches (`edicts`, `messages`,
  `nopose`, ...) answer only after `prac_debug dev 1`.
