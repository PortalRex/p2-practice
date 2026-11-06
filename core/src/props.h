// The values of an entity update: a list of field indices, each followed by
// the prop's value.  Mirrors sar-web src/net/props.ts.
#pragma once

#include <cstdint>
#include <vector>

#include "datatables.h"
#include "readers.h"

namespace p2demo {
namespace detail {

// One prop's last value.  Numbers are kept as doubles, exactly as decoded
// (the reference decoder works in doubles; rounding to float only on output
// keeps "did it change" decisions identical).
struct PropValue {
	enum Kind : uint8_t {
		UNSET,   // never sent and not in the baseline
		NUMBER,  // int and float props: v[0]
		VECTOR,  // v[0..count-1], count 2 or 3
		OTHER,   // strings and arrays: read, not kept
	};
	Kind kind = UNSET;
	uint8_t count = 0;
	double v[3] = {0, 0, 0};
};

using EntityState = std::vector<PropValue>;

// The number if the prop is one, otherwise the fallback (also for index -1)
inline double NumberAt(const EntityState &state, int index, double fallback = 0) {
	if (index < 0 || size_t(index) >= state.size() || state[size_t(index)].kind != PropValue::NUMBER) return fallback;
	return state[size_t(index)].v[0];
}

// The vector if the prop is one, otherwise null
inline const PropValue *VectorAt(const EntityState &state, int index) {
	if (index < 0 || size_t(index) >= state.size() || state[size_t(index)].kind != PropValue::VECTOR) return nullptr;
	return &state[size_t(index)];
}

// An entity index delta / count in svc_PacketEntities
uint64_t ReadUBitInt(BitReader &br);

// Reads one entity's changed props into `state` (sized to the class's prop
// count) and lists their indices in `changed`.  On a DecodeError the props
// read so far stay written, as in the reference decoder.
void ReadEntityProps(BitReader &br, const std::vector<FlatProp> &props, EntityState &state, std::vector<int> &changed);

}  // namespace detail
}  // namespace p2demo
