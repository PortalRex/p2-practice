"""Development aid: the brush entities of a map (triggers and the like) whose volume holds a point, or comes
within a margin of it, with their outputs.

    python tools/testkit/bsptriggers.py <map> <x> <y> <z> [margin]
"""
import struct
import sys

from bspents import OUTPUT_SEPARATOR, entities, entity_text, find_map

MODEL_LUMP = 14
MODEL_SIZE = 48


def model_bounds(data, index):
    offset, _ = struct.unpack_from("<ii", data, 8 + MODEL_LUMP * 16)
    mins = struct.unpack_from("<3f", data, offset + index * MODEL_SIZE)
    maxs = struct.unpack_from("<3f", data, offset + index * MODEL_SIZE + 12)
    return mins, maxs


def main():
    path = find_map(sys.argv[1])
    point = [float(v) for v in sys.argv[2:5]]
    margin = float(sys.argv[5]) if len(sys.argv) > 5 else 0
    with open(path, "rb") as f:
        data = f.read()
    for pairs in entities(entity_text(path)):
        keys = dict(pairs)
        model = keys.get("model", "")
        if not model.startswith("*"):
            continue
        origin = [float(v) for v in keys.get("origin", "0 0 0").split()]
        mins, maxs = model_bounds(data, int(model[1:]))
        low = [origin[i] + mins[i] for i in range(3)]
        high = [origin[i] + maxs[i] for i in range(3)]
        if not all(low[i] - margin <= point[i] <= high[i] + margin for i in range(3)):
            continue
        print(f"{keys.get('classname')}  '{keys.get('targetname', '')}'  {model}  x {low[0]:.0f}..{high[0]:.0f}  y {low[1]:.0f}..{high[1]:.0f}  "
              f"z {low[2]:.0f}..{high[2]:.0f}")
        for key, value in pairs:
            if key in ("classname", "targetname", "origin", "hammerid", "model"):
                continue
            print(f"    {key} = {value.replace(OUTPUT_SEPARATOR, ' | ')}")


if __name__ == "__main__":
    main()
