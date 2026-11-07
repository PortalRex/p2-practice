"""Sends console commands to a test copy of Portal 2 over its network console and prints what it answers.

    python netcon.py --port 2141 [--password p2practice] [--wait 1.5] [--until REGEX] [--timeout 30] cmd [cmd ...]

Each command is sent in order; output is read for --wait seconds after the last one, or until a line matches
--until (then exit 0) or --timeout passes (exit 2). The socket is drained the whole time it is open: the game
blocks when a netcon client stops reading.
"""
import argparse
import re
import socket
import sys
import time


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', type=int, required=True)
    ap.add_argument('--password', default='p2practice')
    ap.add_argument('--wait', type=float, default=1.0)
    ap.add_argument('--until')
    ap.add_argument('--timeout', type=float, default=30.0)
    ap.add_argument('--connect-timeout', type=float, default=60.0)
    ap.add_argument('--gap', type=float, default=0.0, help='seconds between commands')
    ap.add_argument('--quiet', action='store_true')
    ap.add_argument('commands', nargs='*')
    # (a key release such as "-forward" looks like an option: anything that isn't one of the options above is a
    # command, in the order given)
    args, _ = ap.parse_known_args()
    valued = {'--port', '--password', '--wait', '--until', '--timeout', '--connect-timeout', '--gap'}
    args.commands = []
    tokens = sys.argv[1:]
    i = 0
    while i < len(tokens):
        token = tokens[i]
        if token == '--':
            args.commands += tokens[i + 1:]
            break
        if token in valued:
            i += 2
            continue
        if token.split('=', 1)[0] in valued or token == '--quiet':
            i += 1
            continue
        args.commands.append(token)
        i += 1

    deadline = time.time() + args.connect_timeout
    sock = None
    while sock is None:
        try:
            sock = socket.create_connection(('127.0.0.1', args.port), timeout=2)
        except OSError:
            if time.time() > deadline:
                print('netcon: no connection', file=sys.stderr)
                return 3
            time.sleep(0.5)
    sock.settimeout(0.1)
    sock.sendall(f'PASS {args.password}\r\n'.encode())

    until = re.compile(args.until) if args.until else None
    pending = b''
    matched = False

    def drain(seconds: float) -> None:
        nonlocal pending, matched
        end = time.time() + seconds
        while time.time() < end and not (until and matched):
            try:
                data = sock.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                return
            if not data:
                return
            pending += data
            while b'\n' in pending:
                line, pending = pending.split(b'\n', 1)
                text = line.decode('utf-8', 'replace').rstrip('\r')
                if not args.quiet:
                    sys.stdout.buffer.write((text + '\n').encode('utf-8', 'replace'))
                if until and until.search(text):
                    matched = True

    drain(0.2)
    for cmd in args.commands:
        sock.sendall((cmd + '\r\n').encode())
        drain(max(args.gap, 0.05))
    if until:
        drain(args.timeout)
    else:
        drain(args.wait)
    sock.close()
    sys.stdout.flush()
    return 0 if (not until or matched) else 2


if __name__ == '__main__':
    sys.exit(main())
