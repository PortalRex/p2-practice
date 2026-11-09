"""Picks one good recent run per test map from the local board demo archive and copies it into demos/matrix.

    python pick_runs.py [--all]

The archive (H:\\Board-Demos\\demos, named Map_time_steamid_id.dem) is only read; copies go to demos/matrix.
A run qualifies when its demo is the host's and carries SAR's time (so it has SAR's entity-input records, which
the map test compares against), and when it was made on the level as installed: the archive also holds runs made
on altered maps, which no practice session on the real level can follow. For co-op, the partner's demo of the same run is copied too when it exists.
Writes demos/matrix/cases.json for maptest.py (with --all: one case per Challenge Mode map on the boards, written to
cases_all.json).
"""
import json
import os
import re
import shutil
import subprocess
import sys

from bspents import entity_text, find_map

ARCHIVE = r'H:\Board-Demos\demos'
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, 'demos', 'matrix')
P2ROUTE = os.path.join(ROOT, 'build', 'x86', 'tools', 'routeinfo', 'Release', 'p2route.exe')
DEMODUMP = os.path.join(ROOT, 'build', 'x86', 'tools', 'demodump', 'Release', 'demodump.exe')

# One map per single-player chapter and per co-op course: (group, board names to try in order)
CASES = [
    ('Chapter 1: The Courtesy Call', 'CubeMomentum'),
    ('Chapter 2: The Cold Boot', 'LaserStairs'),
    ('Chapter 3: The Return', 'TurretBlocker'),
    ('Chapter 4: The Surprise', 'ColumnBlocker'),
    ('Chapter 5: The Escape', 'TurretFactory'),
    ('Chapter 6: The Fall', 'RepulsionIntro'),
    ('Chapter 7: The Reunion', 'PropulsionIntro'),
    ('Chapter 8: The Itch', 'FunnelIntro'),
    ('Chapter 9: The Part Where He Kills You', 'Finale1'),
    ('Course 1: Team Building', 'Buttons'),
    ('Course 2: Mass and Velocity', 'Flings'),
    ('Course 3: Hard-Light Surfaces', 'BridgeSwap CooperativeBridges CatapultBlock BridgeFling FlingBlock TurretWalls BridgeTesting'),
    ('Course 4: Excursion Funnels', 'FunnelDrill CooperativeFunnels FunnelCatch FunnelHop FunnelLaser CooperativePolarity AdvancedPolarity'),
    ('Course 5: Mobility Gels', 'RepulsionJumps'),
    ('Course 6: Art Therapy', 'TripleAxis Separation CatapultCatch BridgeGels DoubleLift BridgeCatch Maintenance'),
]

CANDIDATES = 80  # the fastest runs looked at per map
MIN_YEAR = 2025  # recent runs: current game build and a SAR that records entity inputs


def scan(path):
    try:
        out = subprocess.run([P2ROUTE, 'scan', path], capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=60).stdout
        return json.loads(out)
    except Exception:
        return {'ok': False}


def on_installed_level(path, map_name):
    """Whether the entities the run's level fired inputs at are the installed map's. (Names made at run time, with
    a number or built by scripts, are left out; a few misses are allowed for those the test doesn't recognise.)"""
    out = subprocess.run([DEMODUMP, path, '--no-movers'], capture_output=True, timeout=120).stdout.decode('utf-8', 'replace')
    try:
        inputs = json.loads(out).get('sarEntityInputs', [])
    except ValueError:
        return False
    level = entity_text(find_map(map_name))
    names = {e['target'] for e in inputs if e['target'] and '&' not in e['target'] and not re.search(r'_\d+$', e['target'])}
    missing = [n for n in names if f'"{n}"' not in level]
    return len(missing) <= max(2, len(names) // 20)


def main():
    os.makedirs(OUT, exist_ok=True)
    names = os.listdir(ARCHIVE)
    by_map = {}
    for name in names:
        parts = name[:-4].split('_')
        if not name.endswith('.dem') or len(parts) != 4 or not (parts[1].isdigit() and parts[3].isdigit()):
            continue
        by_map.setdefault(parts[0], []).append((int(parts[1]), int(parts[3]), name))
    every_map = '--all' in sys.argv
    plan = [(b, b) for b in sorted(by_map) if b] if every_map else CASES
    cases = []
    for group, board_names in plan:
      picked = None
      for board_name in board_names.split():
        if picked:
            break
        runs = sorted(by_map.get(board_name, []))
        for time, run_id, name in runs[:CANDIDATES]:
            path = os.path.join(ARCHIVE, name)
            year = __import__('datetime').datetime.fromtimestamp(os.path.getmtime(path)).year
            if year < MIN_YEAR:
                continue
            info = scan(path)
            if not info.get('ok') or not info.get('host') or not info.get('hasTime'):
                continue
            if not on_installed_level(path, info['map']):
                print(f'  (skipping {name}: not made on the installed {info["map"]})')
                continue
            picked = (time, run_id, name, info, board_name, runs)
            break
      if True:
        if not picked:
            print(f'{group}: no qualifying run among the fastest {CANDIDATES} of {board_names}')
            continue
        time, run_id, name, info, board_name, runs = picked
        files = [name]
        if info['coop']:
            # the partner's demo: same time, neighbouring id, recorded by the other player
            for other_time, other_id, other_name in runs:
                if other_time == time and abs(other_id - run_id) == 1:
                    other = scan(os.path.join(ARCHIVE, other_name))
                    if other.get('ok') and not other.get('host') and other.get('map') == info['map']:
                        files.append(other_name)
                        break
        for f in files:
            shutil.copy2(os.path.join(ARCHIVE, f), os.path.join(OUT, f))
        cases.append({'group': group, 'board': board_name, 'map': info['map'], 'coop': info['coop'], 'time': info['timeText'],
                      'player': info['player'], 'demos': files})
        print(f"{group}: {board_name} {info['timeText']} by {info['player']} ({info['map']}), {len(files)} demo(s)")
    with open(os.path.join(OUT, 'cases_all.json' if every_map else 'cases.json'), 'w', encoding='utf-8') as f:
        json.dump(cases, f, indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main())
