"""A short regression pass over a running test copy (launch.ps1): a few minutes, not a soak.

    python regress.py --job vk7 [--only sp|coop] [--retries 3] [--no-perf]

What it checks, in one single-player level and one co-op level:
  - a run loads, and an attempt starts by itself once its map is up (prac_auto)
  - retries come back to a running attempt, through a save state (no level restart) when ChocolateLoading is in
  - the game's settings are back as they were after each retry, after prac_stop and at the menu
  - co-op: the first attempt joins the level's own clock
  - the frame rate in an attempt against standing in the level (perfcheck.py's measure)
  - no VScript error and no plugin warning anywhere in the console log of the pass
Exit code 0 when everything passed. The demos are looked for in the game's portal2\\demos\\p2practice\\boards
folder (--demos for another one).
"""
import argparse
import json
import os
import re
import socket
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))

SP = {'name': 'single player (Dual Lasers)', 'map': 'map sp_a2_dual_lasers', 'fallback': True, 'demos': ['DualLasers_1086_76561198095730281_308103.dem']}
COOP = {'name': 'co-op (Flings)', 'map': 'ss_map mp_coop_fling_3',
        'demos': ['Flings_1298_76561198295368421_310304.dem', 'Flings_1298_76561198823602829_310305.dem']}

# What the game's settings must read whenever the trainer is not in the middle of a save or restore
AS_BEFORE = None  # (whatever the game had before the run was loaded)
SETTINGS_IN_ATTEMPT = {'mat_queue_mode': AS_BEFORE, 'host_timescale': '1'}
SETTINGS_STOPPED = {'mat_queue_mode': AS_BEFORE, 'host_timescale': '1', 'save_disable': '0'}

# Console lines that mean something went wrong
LEVEL_LOAD = r'---- Host_(?:NewGame|Changelevel) ----'
LOST_SAVE_STATE = 'could not bring the save state back'
TROUBLE = re.compile(r'AN ERROR HAS OCCURED|could not store the save state|could not bring the save state back|Script not found|bad edict|, BAD ', re.I)


class Console:
    """One connection to the test copy's network console, kept open for the whole pass."""

    def __init__(self, port, password):
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=5)
        self.sock.settimeout(0.05)
        self.sock.sendall(f'PASS {password}\r\n'.encode())
        self.read(0.3)

    def read(self, seconds, until=None):
        """What the game printed in the next `seconds` (or up to a line matching `until`)."""
        text = b''
        end = time.time() + seconds
        while time.time() < end:
            try:
                data = self.sock.recv(65536)
            except socket.timeout:
                continue
            if not data:
                break
            text += data
            if until and re.search(until, text.decode('utf-8', 'replace')):
                break
        return text.decode('utf-8', 'replace')

    def send(self, *commands, wait=0.4, until=None):
        for command in commands:
            self.sock.sendall((command + '\r\n').encode())
        return self.read(wait, until)

    def state(self):
        """(map, tick, mode) as prac_debug state prints them; mode '?' when the game didn't answer."""
        found = re.search(r'map (\S*), tick (-?\d+), mode ([a-z -]+), cursor', self.send('prac_debug state', wait=1.0, until=r'mode [a-z -]+, cursor'))
        return (found.group(1), int(found.group(2)), found.group(3).strip()) if found else ('', 0, '?')

    def wait_for_mode(self, modes, seconds):
        deadline = time.time() + seconds
        mode = '?'
        while time.time() < deadline:
            mode = self.state()[2]
            if mode in modes:
                return mode
            time.sleep(0.25)
        return mode

    def cvars(self, names):
        # until the game has answered for the last of them (it can be busy for a moment right after a retry)
        text = self.send(*names, wait=3.0, until=rf'"{names[-1]}" = "')
        return {name: (re.search(rf'"{name}" = "([^"]*)"', text) or [None, None])[1] for name in names}


class Pass:
    def __init__(self, args, info):
        self.args = args
        self.console = Console(info['port'], info['password'])
        self.log_path = os.path.join(info['profile'], 'console.log')
        self.failures = 0
        self.started = time.time()
        self.before = self.console.cvars(['mat_queue_mode'])

    def check(self, ok, what, detail=''):
        self.failures += 0 if ok else 1
        print(f"  {'ok  ' if ok else 'FAIL'} {what}{(': ' + detail) if detail and (not ok or self.args.verbose) else ''}")
        return ok

    def log_size(self):
        return os.path.getsize(self.log_path) if os.path.exists(self.log_path) else 0

    def log_since(self, offset):
        with open(self.log_path, 'rb') as log:
            log.seek(offset)
            return log.read().decode('utf-8', 'replace')

    def settings(self, expected, when):
        expected = {name: self.before[name] if value is AS_BEFORE else value for name, value in expected.items()}
        got = self.console.cvars(list(expected))
        wrong = {name: value for name, value in got.items() if value != expected[name]}
        self.check(not wrong, f'settings {when}', f'{wrong}, expected {expected}' if wrong else str(got))

    def level(self, level):
        """One level: load the run, start by itself, retry a few times, measure, stop."""
        c = self.console
        print(level['name'])
        paths = [os.path.join(self.args.demos, name) for name in level['demos']]
        missing = [path for path in paths if not os.path.exists(path)]
        if missing:
            return self.check(False, 'demos found', ', '.join(missing))
        mark = self.log_size()
        c.send('prac_stop', 'disconnect', wait=4)
        loaded = c.send('prac_debug dev 1', 'prac_debug arrivals 1', 'prac_set partner 1', 'prac_role blue', 'prac_set reset auto',
                        'prac_set snapshots 1', 'prac_set leadin 1.5', 'prac_set loop 0', 'sv_lan 1', 'fps_max 0',
                        'prac_load ' + ' '.join(f'"{path}"' for path in paths), wait=3)
        if not self.check(level['map'] in loaded, 'run loaded', loaded.strip()[-300:]):
            return False
        c.send('prac_auto 1', level['map'], wait=1)
        if not self.check(c.wait_for_mode(['run'], 150) == 'run', 'first attempt started by itself', str(c.state())):
            return False
        loads = len(re.findall(LEVEL_LOAD, self.log_since(mark)))
        self.check(loads == 1, 'the level loaded once for it', f'{loads} loads')
        if 'ss_map' in level['map']:
            joined = re.findall(r'\[clock\] attempt: .*: (joined|not joined)', self.log_since(mark))
            self.check(joined[:1] == ['joined'], "first attempt on the level's clock", str(joined))
        self.settings(SETTINGS_IN_ATTEMPT, 'in the attempt')

        snapshots = 'snap_save' in c.send('find snap_save', wait=0.6)
        for attempt in range(self.args.retries):
            before = self.log_size()
            asked = time.time()
            c.send('prac_retry', wait=0.3)
            # (the retry first goes through the lead-in: wait for that to pass, then for the attempt)
            time.sleep(0.5)
            mode = c.wait_for_mode(['run'], 60)
            took = time.time() - asked
            restarts = len(re.findall(LEVEL_LOAD, self.log_since(before)))
            self.check(mode == 'run', f'retry {attempt + 1} back in an attempt', f'mode {mode} after {took:.1f} s')
            if snapshots:
                self.check(restarts == 0, f'retry {attempt + 1} through the save state', f'{restarts} level restart(s), {took:.1f} s')
            self.settings(SETTINGS_IN_ATTEMPT, f'after retry {attempt + 1}')

        if snapshots and level.get('fallback'):
            self.fallback()
        if not self.args.no_perf:
            self.perf()
        c.send('prac_stop', wait=1.5)
        self.check(c.state()[2] == 'idle', 'stopped')
        self.settings(SETTINGS_STOPPED, 'after prac_stop')
        c.send('disconnect', wait=4)
        self.settings(SETTINGS_STOPPED, 'at the menu')
        trouble = [line.strip() for line in self.log_since(mark).splitlines() if TROUBLE.search(line)]
        if level.get('fallback'):
            trouble = [line for line in trouble if LOST_SAVE_STATE not in line]  # (asked for, in fallback)
        self.check(not trouble, 'no errors in the console', ' | '.join(trouble[:5]))
        return True

    def fallback(self):
        """A save state that is gone when the retry asks for it: the retry restarts the level instead."""
        c = self.console
        before = self.log_size()
        for name in ('p2practice', 'p2practice_base'):  # (the attempt's and the level's base: either may hold it)
            c.send('snap_drop ' + name, wait=0.3)
        c.send('prac_retry', wait=0.8)
        mode = c.wait_for_mode(['run'], 60)
        log = self.log_since(before)
        restarted = len(re.findall(LEVEL_LOAD, log)) > 0
        self.check(mode == 'run' and restarted and LOST_SAVE_STATE in log, 'a lost save state falls back to a level restart',
                   f'mode {mode}, restarted {restarted}, said so {LOST_SAVE_STATE in log}')

    def fps(self, seconds):
        self.console.send('prac_debug perf', wait=0.3)  # (a call starts a new measurement and reports the one before it)
        time.sleep(seconds)
        found = re.search(r'\[perf\] .*?: (\d+) fps', self.console.send('prac_debug perf', wait=1.0, until=r'\[perf\] .*fps'))
        return int(found.group(1)) if found else 0

    def perf(self):
        """Frame rate in the attempt against standing in the level right after it."""
        c = self.console
        if c.state()[2] != 'run':
            c.send('prac_retry', wait=0.3)
            c.wait_for_mode(['run'], 60)
        attempt = self.fps(self.args.perf_seconds)
        c.send('prac_stop', wait=1.5)
        idle = self.fps(self.args.perf_seconds)
        ratio = attempt / idle if idle else 0
        self.check(ratio >= self.args.min_ratio, 'frame rate in an attempt', f'{attempt} fps against {idle} idle ({ratio:.0%}, at least {self.args.min_ratio:.0%} wanted)')
        if self.args.verbose or ratio < self.args.min_ratio:
            return
        print(f'       {attempt} fps in the attempt, {idle} idle ({ratio:.0%})')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--job', default='vk7', help="the test copy's job name (launch.ps1 -Job)")
    parser.add_argument('--only', choices=['sp', 'coop'])
    parser.add_argument('--retries', type=int, default=3)
    parser.add_argument('--demos', help='folder holding the test demos')
    parser.add_argument('--no-perf', action='store_true')
    parser.add_argument('--perf-seconds', type=float, default=3)
    parser.add_argument('--min-ratio', type=float, default=0.8)
    parser.add_argument('--verbose', action='store_true')
    args = parser.parse_args()

    state_file = os.path.join(REPO, 'build', 'test', args.job, 'state.json')
    if not os.path.exists(state_file):
        sys.exit(f'no test copy "{args.job}" (start one with launch.ps1)')
    with open(state_file, encoding='utf-8-sig') as file:
        info = json.load(file)
    if info.get('exited'):
        sys.exit(f'test copy "{args.job}" has exited')
    if not args.demos:
        # (the profile is <Portal 2>\p2practice-test\<job>\portal2)
        game = os.path.dirname(os.path.dirname(os.path.dirname(info['profile'])))
        args.demos = os.path.join(game, 'portal2', 'demos', 'p2practice', 'boards')

    run = Pass(args, info)
    cap = run.console.cvars(['fps_max'])['fps_max']
    try:
        for key, level in (('sp', SP), ('coop', COOP)):
            if args.only in (None, key):
                run.level(level)
    finally:
        run.console.send('prac_stop', 'prac_auto off',f'fps_max {cap or 300}', wait=0.5)
    print(f"{'PASSED' if not run.failures else f'FAILED ({run.failures})'} in {time.time() - run.started:.0f} s")
    sys.exit(1 if run.failures else 0)


if __name__ == '__main__':
    main()
