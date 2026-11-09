"""Development aid: prints the entities of a map (from the .bsp's entity lump) whose keys or values contain all
of the given words.

    python tools/testkit/bspents.py <map name or .bsp path> <word> [word ...]
"""
import os
import re
import struct
import sys

# (P2_GAME_DIR: the Portal 2 folder, where Steam isn't in its usual place)
GAME = os.environ.get("P2_GAME_DIR") or r"C:\Program Files (x86)\Steam\steamapps\common\Portal 2"
ENTITY_LUMP = 0
OUTPUT_SEPARATOR = "\x1b"  # between the fields of an output in a compiled map


def entity_text(path):
    with open(path, "rb") as f:
        data = f.read()
    offset, length = struct.unpack_from("<ii", data, 8 + ENTITY_LUMP * 16)
    return data[offset:offset + length].rstrip(b"\0").decode("latin-1")


def find_map(name):
    if os.path.isfile(name):
        return name
    for folder in ("portal2", "portal2_dlc1", "portal2_dlc2"):
        path = os.path.join(GAME, folder, "maps", name + ".bsp")
        if os.path.isfile(path):
            return path
    raise SystemExit(f"no such map: {name}")


def entities(text):
    for block in re.findall(r"\{[^{}]*\}", text):
        yield re.findall(r'"([^"]*)"\s+"([^"]*)"', block)


def main():
    words = [w.lower() for w in sys.argv[2:]]
    for pairs in entities(entity_text(find_map(sys.argv[1]))):
        flat = " ".join(k + " " + v for k, v in pairs).lower()
        if not all(w in flat for w in words):
            continue
        keys = dict(pairs)
        print(f"{keys.get('classname', '?')}  {keys.get('targetname', '')}  origin {keys.get('origin', '-')}")
        for key, value in pairs:
            if key in ("classname", "targetname", "origin", "hammerid"):
                continue
            print(f"    {key} = {value.replace(OUTPUT_SEPARATOR, ' | ')}")


if __name__ == "__main__":
    main()
