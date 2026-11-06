"""Runs test runs through a running test copy of Portal 2 and reports, per map, whether practice works on it.

    python maptest.py --job t3 [--port 2141] [--only Buttons] [--cm] [--out build/test/matrix.json]

For each case in demos/matrix/cases.json:
  1. loads the map (ss_map for co-op, map for single player), waits for the players;
  2. loads the run with prac_load, records a demo of the session, and rides the run with prac_watch;
  3. compares the map events (entity inputs SAR records) of the session's demo with the original run's: every
     door, button, relay and trigger the run fired should have fired in the session too, in the same order;
  4. starts practice from a middle part (prac_start), which restarts the level and replays up to the part, and
     reads how far the handed-over player was from the recorded position and speed;
  5. counts VScript errors in the console log.
The game is driven only through its network console. Needs build/x86 (demodump, p2route) and a test copy started
by launch.ps1 with SAR and the plugin loaded.
"""
import argparse
import json
import os
import re
import socket
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEMODUMP = os.path.join(ROOT, 'build', 'x86', 'tools', 'demodump', 'Release', 'demodump.exe')
P2ROUTE = os.path.join(ROOT, 'build', 'x86', 'tools', 'routeinfo', 'Release', 'p2route.exe')
MATRIX = os.path.join(ROOT, 'demos', 'matrix')

# Entity classes whose inputs don't change what the level is (sounds, lights, dialogue): as in route.cpp
COSMETIC = {
    'ambient_generic', 'logic_choreographed_scene', 'generic_actor', 'env_projectedtexture', 'info_particle_system', 'env_sprite',
    'npc_security_camera', 'point_servercommand', 'logic_script', 'vgui_screen', 'env_texturetoggle', 'env_global', 'env_fade',
    'env_soundscape', 'point_viewcontrol', 'env_tonemap_controller', 'light_spot', 'light', 'hot_potato_socket', 'point_clientcommand',
    'env_shake', 'ambient_music', 'info_overlay_accessor', 'vgui_movie_display', 'logic_autosave',
}
COSMETIC_INPUTS = {'EnableRefire', 'CancelPending', 'SetDefaultAnimation', 'Skin'}


class Console:
    """The game's network console, read continuously on a thread (the game blocks when nobody reads)."""

    def __init__(self, port, password):
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=5)
        self.sock.settimeout(0.2)
        self.sock.sendall(f'PASS {password}\r\n'.encode())
        self.lines = []
        self.lock = threading.Lock()
        self.alive = True
        self.thread = threading.Thread(target=self._read, daemon=True)
        self.thread.start()

    def _read(self):
        pending = b''
        while self.alive:
            try:
                data = self.sock.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                break
            if not data:
                break
            pending += data
            while b'\n' in pending:
                line, pending = pending.split(b'\n', 1)
                with self.lock:
                    self.lines.append(line.decode('utf-8', 'replace').rstrip('\r'))
        self.alive = False

    def mark(self):
        with self.lock:
            return len(self.lines)

    def since(self, mark):
        with self.lock:
            return self.lines[mark:]

    def send(self, *commands, gap=0.15):
        for command in commands:
            self.sock.sendall((command + '\r\n').encode())
            time.sleep(gap)

    def wait_for(self, pattern, mark, timeout):
        """The first line after `mark` matching `pattern`, or None after `timeout` seconds (or if the game went away)."""
        regex = re.compile(pattern)
        end = time.time() + timeout
        seen = mark
        while time.time() < end and self.alive:
            lines = self.since(seen)
            for line in lines:
                if regex.search(line):
                    return line
            seen += len(lines)
            time.sleep(0.2)
        return None

    def ask(self, command, wait=1.0):
        mark = self.mark()
        self.send(command)
        time.sleep(wait)
        return self.since(mark)


def run_json(args):
    out = subprocess.run(args, capture_output=True, timeout=300).stdout.decode('utf-8', 'replace')
    return json.loads(out)


def world_events(demo, first_tick=None, last_tick=None):
    events = []
    for e in demo.get('sarEntityInputs', []):
        if e['className'] in COSMETIC or e['input'] in COSMETIC_INPUTS:
            continue
        if first_tick is not None and not (first_tick <= e['tick'] <= last_tick):
            continue
        # things spawned from a template get a running number after their name (panel_1_train&3771) that differs
        # from one load to the next
        target = re.sub(r'&\d+', '', e['target'])
        events.append((e['tick'], f"{e['className']} {target} {e['input']} {e['param']}".strip()))
    return events


def compare_events(reference, session):
    """Matches the reference's events against the session's, keeping their order (the longest sequence the two
    have in common; the session may hold extra events). Returns how many matched, the reference events that
    didn't, how far the matched ones drift apart in time (ticks from the typical offset), and that typical
    offset (session tick minus reference tick)."""
    import difflib
    matcher = difflib.SequenceMatcher(None, [e[1] for e in reference], [e[1] for e in session], autojunk=False)
    matched_reference = set()
    offsets = []
    for block in matcher.get_matching_blocks():
        for k in range(block.size):
            matched_reference.add(block.a + k)
            offsets.append(session[block.b + k][0] - reference[block.a + k][0])
    missing = [e for i, e in enumerate(reference) if i not in matched_reference]
    spread = 0
    middle = 0
    if offsets:
        middle = sorted(offsets)[len(offsets) // 2]
        # the bulk of the events, ignoring a few stragglers (a door that was already open, say)
        ordered = sorted(abs(o - middle) for o in offsets)
        spread = ordered[int(len(ordered) * 0.9)] if len(ordered) >= 10 else ordered[-1]
    return len(matched_reference), missing, spread, middle


# Inputs that set a state, grouped by kind (as in route.cpp): the latest one of a kind on an entity is its state
STATE_KINDS = [('Open', 'Close'), ('Enable', 'Disable'), ('TurnOn', 'TurnOff'), ('Lock', 'Unlock'), ('Check', 'Uncheck'),
               ('EnableDraw', 'DisableDraw'), ('ShowSprite', 'HideSprite'), ('SetStateATrue', 'SetStateAFalse'),
               ('SetStateBTrue', 'SetStateBFalse'), ('EnableMotion', 'DisableMotion'), ('EnablePortalFunnel', 'DisablePortalFunnel'),
               ('Start', 'Stop'), ('SetValue',), ('SetAnimation',), ('Skin',), ('SetTextureIndex',), ('SetLinearForce',), ('Kill',)]
STATE_KIND_OF = {name: i for i, names in enumerate(STATE_KINDS) for name in names}
NOT_STATE_CLASSES = {'ambient_generic', 'logic_choreographed_scene', 'generic_actor', 'info_particle_system', 'env_fade', 'point_servercommand',
                     'point_clientcommand', 'logic_script', 'vgui_movie_display', 'env_shake', 'ambient_music', 'game_text', 'logic_timer',
                     'logic_relay', 'func_tracktrain', 'path_track', 'logic_case'}


def level_state(demo, last_tick=None, first_tick=None):
    """The level's switchable state at the end: {(entity, kind): (input, parameter)} from the latest state inputs
    (those from `first_tick` on: what the level did on its own before the players could move is not the
    trainer's doing either way)."""
    state = {}
    for e in demo.get('sarEntityInputs', []):
        if last_tick is not None and e['tick'] > last_tick:
            break
        if first_tick is not None and e['tick'] < first_tick:
            continue
        kind = STATE_KIND_OF.get(e['input'])
        if kind is None or e['className'] in NOT_STATE_CLASSES or not e['target'] or '&' in e['target']:
            continue
        state[(e['target'], kind)] = (e['input'], e['param'])
    return state


def compare_state(reference, session):
    """How many of the run's entity states the session ended with, and the ones it didn't. Both are
    {(entity, kind): (input, parameter)}."""
    wrong = [(key, value, session.get(key)) for key, value in reference.items() if session.get(key) != value]
    return len(reference) - len(wrong), wrong


def recorded_from(demo):
    """The first tick a session demo has entity inputs for (the level did things before the recording began)."""
    ticks = [e['tick'] for e in demo.get('sarEntityInputs', [])]
    return min(ticks) if ticks else 0


def present_events(reference, session):
    """How many of the reference's events happened in the session at all, whatever the order."""
    from collections import Counter
    have = Counter(e[1] for e in session)
    count = 0
    for _, signature in reference:
        if have[signature] > 0:
            have[signature] -= 1
            count += 1
    return count


OBJECT_CLASSES = {'CPropWeightedCube', 'CPropMonsterBox', 'CNPC_Portal_FloorTurret', 'CPhysicsProp', 'CPropGlassFutbol', 'CNPC_Personality_Core'}


def final_objects(demo, tick, resting_only):
    """Where each movable object is at `tick`: (class, (x, y, z)) for those that still exist. With `resting_only`,
    only objects lying still by then (one in mid-air is somewhere else a tick later, so can't be compared)."""
    def position(key):
        origin = key['origin']
        return (origin['x'], origin['y'], origin['z']) if isinstance(origin, dict) else tuple(origin)

    objects = []
    for mover in demo.get('movers', []):
        if mover['className'] not in OBJECT_CLASSES:
            continue
        keys = [k for k in mover['keys'] if k['tick'] <= tick]
        if not keys or keys[-1].get('removed'):
            continue
        now = position(keys[-1])
        if resting_only:
            earlier = [k for k in keys if k['tick'] <= tick - 30]
            if not earlier or sum((a - b) ** 2 for a, b in zip(now, position(earlier[-1]))) ** 0.5 > 16:
                continue
        objects.append((mover['className'], now))
    return objects


def objects_in_place(reference, session, tolerance=48):
    """How many of the run's objects have a session object of the same kind lying within `tolerance` units."""
    placed = 0
    used = set()
    for kind, position in reference:
        best = None
        for i, (other_kind, other) in enumerate(session):
            if other_kind != kind or i in used:
                continue
            distance = sum((a - b) ** 2 for a, b in zip(position, other)) ** 0.5
            if distance <= tolerance and (best is None or distance < best[1]):
                best = (i, distance)
        if best:
            used.add(best[0])
            placed += 1
    return placed


def wait_for_players(console, count, timeout):
    end = time.time() + timeout
    while time.time() < end and console.alive:
        lines = console.ask('prac_debug', 1.0)
        players = [l for l in lines if re.search(r'\] player \d+:', l) and '(dead)' not in l]
        if len(players) >= count:
            return True
        time.sleep(1.5)
    return False


def run_case(console, case, state):
    result = {'group': case['group'], 'board': case['board'], 'map': case['map'], 'time': case['time'], 'ok': False, 'problems': []}
    problems = result['problems']
    log_path = os.path.join(state['profile'], 'console.log')
    log_start = os.path.getsize(log_path) if os.path.exists(log_path) else 0
    trace_path = os.path.join(ROOT, 'build', 'test', state['job'], 'trace.log')
    demos = [os.path.join(MATRIX, d).replace('\\', '/') for d in case['demos']]

    # 1. the map
    # (leave the previous game first: going straight from a single-player level into split screen drops back to
    # the menu; and a second try for the same reason)
    console.send('prac_stop', 'disconnect')
    time.sleep(3)
    console.send('sv_lan 1', 'voice_enable 0')
    loaded = False
    for attempt in range(2):
        console.send(('ss_map ' if case['coop'] else 'map ') + case['map'])
        time.sleep(6)
        if wait_for_players(console, 2 if case['coop'] else 1, 90):
            loaded = True
            break
    if not loaded:
        problems.append('the map did not load or the players did not spawn')
        return result
    time.sleep(2)
    info = run_json([P2ROUTE, 'info'] + demos)
    result['challenge'] = bool(info.get('challenge'))

    # 2. the run
    mark = console.mark()
    console.send('sv_cheats 1', 'prac_set reset auto', 'prac_load ' + ' '.join(f'"{d}"' for d in demos))
    line = console.wait_for(r'Ready\. prac_start|Couldn.t read|Demo not found|Load .* to practice', mark, 30)
    if not line or 'Ready.' not in line:
        problems.append(f'prac_load failed: {line}')
        return result
    loaded = console.since(mark)
    result['parts'] = len([l for l in loaded if re.search(r'\]\s+part \d+', l)])
    result['notes'] = [l.split('note:', 1)[1].strip() for l in loaded if 'note:' in l]

    # 3. ride the whole run, recording the session
    session_name = 'mt_' + case['board'].lower()
    session_demo = os.path.join(state['profile'], session_name + '.dem')
    if os.path.exists(session_demo):
        os.remove(session_demo)
    seconds = float(info['time'])
    mark = console.mark()
    # (the trainer restarts the level where needed - into Challenge Mode, back onto the level's own clock - and
    # begins the recording itself when the ride starts, so the demo holds the whole of it)
    console.send(f'prac_debug record {session_name}', 'prac_watch 1')
    line = console.wait_for(r'Watched the run to its end', mark, seconds * 4 + 180)
    console.send('stop')
    time.sleep(1.5)
    if not line:
        problems.append('the ride-along did not reach the end of the run' + ('' if console.alive else ' (the game went away)'))
        return result

    # ... and compare what the level did with what it did in the original run
    samples = subprocess.run([P2ROUTE, 'samples'] + demos + ['0', '0'], capture_output=True, timeout=120).stdout.decode('utf-8', 'replace')
    timer = re.search(r'timer (-?\d+)\.\.(-?\d+)', samples)
    # from the first moment of control (the ride in on the lift or down the tube happens on the level's own clock)
    control = int(timer.group(1)) + int(round(float(info['parts'][0]['start']) * 60)) + 1
    reference_demo = run_json([DEMODUMP, demos[0]])
    reference = world_events(reference_demo, control, int(timer.group(2)))
    if not os.path.exists(session_demo):
        problems.append('the session demo was not written')
        return result
    session_demo_data = run_json([DEMODUMP, session_demo])
    session = world_events(session_demo_data)
    matched, missing, spread, session_offset = compare_events(reference, session)
    present = present_events(reference, session)
    result.update(events=len(reference), present=present, matched=matched, timing_spread_ticks=spread, missing=[f'{t}: {s}' for t, s in missing[:12]])
    # What matters for practice is the state the level ends in: every door, panel, trigger, laser the run switched
    # must be as the run left it. (How many of the run's events fired, and in which order, is reported too, but
    # dialogue and machinery on their own clocks make those differ without mattering.)
    # (both levels at the moment the run ended: the session's demo runs on a little longer. A door that shuts
    # behind the player in the run's last instant may fall just before or just after that moment, so the run's
    # state a few ticks either side counts too.)
    run_end = int(timer.group(2))
    kMoment = 4
    run_state = level_state(reference_demo, run_end, first_tick=control)
    alternatives = [level_state(reference_demo, run_end + d, first_tick=control) for d in (-kMoment, kMoment)]
    session_state = level_state(session_demo_data, run_end + session_offset)
    same, wrong = compare_state(run_state, session_state)
    wrong = [(key, value, other) for key, value, other in wrong if all(a.get(key) != other for a in alternatives)]
    same = len(run_state) - len(wrong)
    result.update(states=len(run_state), states_same=same,
                  states_wrong=[f'{key[0]}: run {value[0]} {value[1]}'.strip() + f', session {other[0] + " " + other[1] if other else "never set"}'.strip()
                                for key, value, other in wrong[:12]])
    if same < len(run_state):
        problems.append(f'{len(run_state) - same} of {len(run_state)} entity states differ from the run at the end')
    # ... and the things the run moved should lie where the run left them
    run_objects = final_objects(reference_demo, int(timer.group(2)), True)
    session_objects = final_objects(session_demo_data, session_demo_data.get('lastTick', 0), False)
    placed = objects_in_place(run_objects, session_objects)
    result.update(objects=len(run_objects), objects_in_place=placed)
    if placed < len(run_objects):
        problems.append(f'{len(run_objects) - placed} of {len(run_objects)} objects did not end up where the run left them')

    # 4. practice from a middle part: level restart, replay up to the part, handover
    if result['parts'] >= 2:
        part = max(2, result['parts'] // 2 + 1)
        trace_start = os.path.getsize(trace_path) if os.path.exists(trace_path) else 0
        mark = console.mark()
        console.send(f'prac_start {part}')
        end = time.time() + 240
        handed = False
        while time.time() < end and console.alive and not handed:
            time.sleep(1.5)
            if os.path.exists(trace_path):
                with open(trace_path, 'r', encoding='utf-8', errors='replace') as f:
                    f.seek(trace_start)
                    for l in f:
                        m = re.search(r'handover: player (\d+) is ([\d.]+) units and ([\d.]+) u/s from the run', l)
                        if m:
                            handed = True
                            result.setdefault('handover', []).append({'player': int(m.group(1)), 'units': float(m.group(2)), 'speed': float(m.group(3))})
        result['practice_part'] = part
        if not handed:
            problems.append(f'practice from part {part} never reached the handover' + ('' if console.alive else ' (the game went away)'))
        else:
            worst = max(result['handover'], key=lambda h: h['units'])
            if worst['units'] > 24 or worst['speed'] > 80:
                problems.append(f"handover off by {worst['units']:.1f} units, {worst['speed']:.0f} u/s")
        time.sleep(2)
        console.send('prac_stop')

    # 5. script errors
    if os.path.exists(log_path):
        with open(log_path, 'r', encoding='utf-8', errors='replace') as f:
            f.seek(log_start)
            text = f.read()
        result['script_errors'] = text.count('AN ERROR HAS OCCURED')
        if result['script_errors']:
            problems.append(f"{result['script_errors']} VScript errors")
    if not console.alive:
        problems.append('the game went away')
    result['ok'] = not problems
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--job', required=True)
    ap.add_argument('--port', type=int, default=2141)
    ap.add_argument('--password', default='p2practice')
    ap.add_argument('--only', default='')
    ap.add_argument('--out', default=os.path.join(ROOT, 'build', 'test', 'matrix.json'))
    args = ap.parse_args()

    with open(os.path.join(ROOT, 'build', 'test', args.job, 'state.json'), encoding='utf-8-sig') as f:
        state = json.load(f)
    with open(os.path.join(MATRIX, 'cases.json'), encoding='utf-8') as f:
        cases = [c for c in json.load(f) if args.only.lower() in (c['board'] + c['group']).lower()]
    console = Console(args.port, args.password)
    results = []
    for case in cases:
        if not console.alive:
            results.append({'group': case['group'], 'board': case['board'], 'ok': False, 'problems': ['not run: the game went away']})
            continue
        started = time.time()
        try:
            result = run_case(console, case, state)
        except Exception as error:  # a broken case must not stop the others
            result = {'group': case['group'], 'board': case['board'], 'ok': False, 'problems': [f'test error: {error!r}']}
        result['seconds'] = round(time.time() - started)
        results.append(result)
        verdict = 'OK  ' if result['ok'] else 'FAIL'
        detail = (f"level state {result.get('states_same', '-')}/{result.get('states', '-')}, events {result.get('present', '-')}/{result.get('events', '-')} "
                  f"(in order {result.get('matched', '-')}, timing within {result.get('timing_spread_ticks', '-')} ticks), "
                  f"objects {result.get('objects_in_place', '-')}/{result.get('objects', '-')}, handover {result.get('handover', '-')}")
        print(f"{verdict} {case['group']} - {case['board']} ({result['seconds']} s): {detail}", flush=True)
        for problem in result['problems']:
            print(f'       {problem}', flush=True)
        for line in result.get('states_wrong', [])[:8]:
            print(f'       state {line}', flush=True)
        with open(args.out, 'w', encoding='utf-8') as f:
            json.dump(results, f, indent=1)
    return 0 if all(r['ok'] for r in results) else 1


if __name__ == '__main__':
    sys.exit(main())
