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


VALUED_OPTIONS = {'--port', '--password', '--wait', '--until', '--timeout', '--connect-timeout', '--gap'}
FLAG_OPTIONS = {'--quiet', '-h', '--help'}


def split_arguments(tokens):
    """The command line as (options, commands). A key release such as "-forward" looks like an option to
    argparse, so the split is made here: whatever isn't one of this script's options (or the value of one) is a
    command, in the order given, as is everything after a lone "--"."""
    options, commands = [], []
    i = 0
    while i < len(tokens):
        token = tokens[i]
        if token == '--':
            commands += tokens[i + 1:]
            break
        if token in VALUED_OPTIONS:
            options += tokens[i:i + 2]
            i += 2
        elif token in FLAG_OPTIONS or token.split('=', 1)[0] in VALUED_OPTIONS:
            options.append(token)
            i += 1
        else:
            commands.append(token)
            i += 1
    return options, commands


def main() -> int:
    ap = argparse.ArgumentParser(usage='%(prog)s --port N [options] cmd [cmd ...]', allow_abbrev=False)
    ap.add_argument('--port', type=int, required=True)
    ap.add_argument('--password', default='p2practice')
    ap.add_argument('--wait', type=float, default=1.0)
    ap.add_argument('--until')
    ap.add_argument('--timeout', type=float, default=30.0)
    ap.add_argument('--connect-timeout', type=float, default=60.0)
    ap.add_argument('--gap', type=float, default=0.0, help='seconds between commands')
    ap.add_argument('--quiet', action='store_true')
    options, commands = split_arguments(sys.argv[1:])
    args = ap.parse_args(options)

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
    for cmd in commands:
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
