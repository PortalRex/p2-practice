# p2demo

Reads a Portal 2 demo (`.dem`) into plain data: the recorded view, the user
commands, console commands, what SAR wrote into the file, and — decoded from
the network data — where the players, portals and moving things were on every
tick, and every portal shot.

A dependency-free C++17 static library. The whole interface is
[`include/p2demo/demo.h`](include/p2demo/demo.h):

```cpp
p2demo::Demo demo;
std::string error;
if (!p2demo::ParseFile(path, demo, error)) { /* error says why */ }
```

`Parse` / `ParseFile` never throw and never read outside the buffer they are
given; they keep no global state, so demos can be parsed on several threads at
once. A demo that is cut short is read up to the cut. A demo whose network data
can't be decoded still returns its container data, with `entitiesDecoded`
false, the entity lists empty and the reason in `warnings`.

It is a port of the parser of the sar-web demo viewer (`H:\Claude\sar-web\src`,
`core/dem.ts` and `net/*.ts`); each source file names the TypeScript file it
mirrors.

| file | what |
| --- | --- |
| `src/demo.cpp` | the container (header, frames) and `Parse` / `ParseFile` |
| `src/readers.h` | bounds-checked byte and bit readers |
| `src/usercmd.cpp`, `src/sardata.cpp` | user commands, SAR's custom data |
| `src/datatables.cpp` | send tables → each class's flattened prop list |
| `src/props.cpp` | reading prop values |
| `src/stringtables.cpp` | string tables (class baselines, model names, player names) |
| `src/netmessages.cpp` | the net messages inside a packet |
| `src/entities.cpp` | `svc_PacketEntities` → players, portals, movers, portal shots |

## What the entity lists hold

- **Players** (`CPortal_Player`): one key for every snapshot that holds the
  player, plus one key with `inSnapshot` false on the tick it leaves the
  recorder's view (or is deleted), then none until it is back. The velocity and
  the exact view offset are only sent to the player they belong to, so
  `hasVelocity` is true for the recording player only; for the other player the
  view offset is the copy other clients get.
- **Portals** (`CProp_Portal`) and **movers** (any other entity with a model
  whose origin or angles changed at least once): a key only when one of the
  key's fields changed. Portal 2 doesn't network `m_iLinkageGroupID`
  (`linkageGroup` stays 0); `firedBy`, `team` and `linkedPortal` tell the
  portals apart.
- A full snapshot in the middle of a demo doesn't start new tracks: an entity
  with the same index, class and serial carries on in its track.
- **Console commands**: without the `name "value"` lines the engine writes for
  its `FCVAR_DEMO` cvars when recording starts.

## Build

Visual Studio 2019 and CMake, from `H:\Claude\p2-practice`:

```
cmake -S . -B build\x86 -G "Visual Studio 16 2019" -A Win32
cmake --build build\x86 --config Release
```

(`-A x64 -B build\x64` for a 64-bit build.) Targets: `p2demo` (this library,
`/W4`, static CRT), `demodump`, `demostress`. In another project:
`add_subdirectory(core)` and `target_link_libraries(<target> PRIVATE p2demo)`.

## Tools

- `demodump <file.dem>` prints the whole parsed `Demo` as JSON
  (`--no-entities`, `--no-movers` as in `ParseOptions`). Exit code 1 and a
  message when the file isn't a demo.
- `tools/tsdump.ts <file.dem>` prints the same JSON from the TypeScript parser. It imports that parser from the
  sar-web project, which has to be checked out beside this one (`H:\Claude\sar-web` beside `H:\Claude\p2-practice`).
- `tools/compare.mjs` runs both on demos or folders of demos and reports the
  differences, one line per demo and a summary:

  ```
  node tools\compare.mjs demos demos\board
  ```

  Decimal fields may differ by 0.02 (floats here, doubles there); everything
  else must be equal. Warnings are not compared. Needs Node 22.6 or later and,
  as it runs `tsdump.ts`, the sar-web sources beside this project (`..\sar-web\src`
  seen from the project folder).
- `demostress <file.dem>...` parses cut-short and corrupted copies of demos
  (by default 50 cuts, 300 with changed bytes and 75 with both, per demo) and
  fails on a slow parse; any crash ends it. Run it from an AddressSanitizer
  build to catch reads outside the buffer:

  ```
  cmake -S . -B build\asan-x86 -G "Visual Studio 16 2019" -A Win32 -DP2_ASAN=ON
  cmake --build build\asan-x86 --config Release
  build\asan-x86\tools\demostress\Release\demostress.exe demos\doors_wr_359667.dem
  ```

  `demostress --time <file.dem>...` prints parse times instead, and
  `demostress --threads 8 <file.dem>...` parses the files on 8 threads at once
  and checks every result against the single-threaded one.

## Where it differs from the TypeScript parser

On purpose, all visible in `tools/tsdump.ts` (which adapts the TypeScript
output to match):

- The file's last frame, when the file ends inside it: the C++ reads the bytes
  that are there; the TypeScript reads zeros for the rest.
- Limits on what a file can make the decoder allocate or loop over (entity
  indices under 2048, at most 64 string tables, 4096 props a class, and so on):
  beyond them the network data counts as undecodable.
- A second map in one file (a signon frame after the sync tick) stops the
  reading with a warning. Portal 2 writes one file per map, so this hasn't been
  seen.
- The view offset of the player who isn't recording (see above), tracks that
  continue through a full snapshot, one player key per snapshot.
