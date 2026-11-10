"""Measures what the plugin costs in frame rate: the test copy's fps standing in the level against its fps in an
attempt, with the route, ghosts and HUD drawn. Fails (exit 1) when the attempt runs at less than --min-ratio of
the idle rate.

    python perfcheck.py --port 2187 [--part 1] [--seconds 4] [--min-ratio 0.85]

The test copy (launch.ps1) must be in the route's map with the run loaded (prac_load) and everyone spawned. The
frame cap is lifted for the measurement and put back afterwards. Both numbers move with whatever else the machine
is doing, the player's own game included: only the ratio means something.
"""
import argparse
import os
import re
import subprocess
import sys
import time

NETCON = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'netcon.py')


def console(args, *commands, wait=1.0):
    """Sends console commands to the test copy and returns what it printed."""
    command = [sys.executable, '-I', NETCON, '--port', str(args.port), '--password', args.password, '--wait', str(wait), *commands]
    return subprocess.run(command, capture_output=True, text=True, errors='replace').stdout


def measure(args):
    """The frame rate over the next --seconds, and the plugin's mode at the end of them."""
    console(args, 'prac_debug perf')  # (a call starts a new measurement and reports the one before it)
    time.sleep(args.seconds)
    report = console(args, 'prac_debug perf')
    found = re.search(r'\[perf\] .*?: (\d+) fps .*mode (\d+)', report)
    if not found:
        sys.exit('no answer to prac_debug perf: is the test copy up, with the plugin loaded?')
    return int(found.group(1)), int(found.group(2)), report


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--port', type=int, default=2141)
    parser.add_argument('--password', default='p2practice')
    parser.add_argument('--part', type=int, default=1, help='the part of the run to practise while measuring')
    parser.add_argument('--seconds', type=float, default=4, help='how long each measurement lasts')
    parser.add_argument('--min-ratio', type=float, default=0.85, help='attempt fps / idle fps below which the check fails')
    parser.add_argument('--verbose', action='store_true', help="print the plugin's own timings as well")
    args = parser.parse_args()

    cap = re.search(r'"fps_max" = "([\d.]+)"', console(args, 'fps_max'))
    console(args, 'prac_debug dev 1', 'prac_stop', 'fps_max 0')
    try:
        time.sleep(1)
        idle, idle_mode, _ = measure(args)
        if idle_mode != 0:
            sys.exit(f'the plugin did not stop (mode {idle_mode})')

        console(args, f'prac_start {args.part}')
        # (a level restart and the lead-in come first: wait until the attempt itself is running)
        deadline = time.time() + 60
        running = False
        while time.time() < deadline and not running:
            time.sleep(1)
            running = 'mode run' in console(args, 'prac_debug state', wait=0.5)
        if not running:
            sys.exit('the attempt did not start: is the run loaded and its map up?')
        attempt, _, report = measure(args)
    finally:
        console(args, 'prac_stop', f'fps_max {cap.group(1) if cap else 300}')

    ratio = attempt / idle if idle else 0
    print(f'idle {idle} fps, in an attempt {attempt} fps: {ratio:.0%}')
    if args.verbose:
        print(report.strip())
    if ratio < args.min_ratio:
        sys.exit(f'FAILED: below {args.min_ratio:.0%} of the idle frame rate')


if __name__ == '__main__':
    main()
