#include "props.h"

#include <cmath>

namespace p2demo {
namespace detail {

namespace {

constexpr int kCoordIntBits = 14, kCoordFracBits = 5;
constexpr int kCoordIntBitsMp = 11, kCoordFracBitsMpLowPrecision = 3;
constexpr int kNormalFracBits = 11;
constexpr int kStringLengthBits = 9;
constexpr int kEndOfFields = 0xfff;

// (every read is its own statement: the order the bits are taken in matters)
double ReadFloat(BitReader &br, const PropCoding &p) {
	const uint32_t flags = p.flags;
	if (flags & SPROP_COORD) {
		const bool hasInt = br.bit();
		const bool hasFrac = br.bit();
		if (!hasInt && !hasFrac) return 0;
		const bool negative = br.bit();
		const double whole = hasInt ? double(br.u(kCoordIntBits)) + 1 : 0;
		const double frac = hasFrac ? double(br.u(kCoordFracBits)) : 0;
		const double v = whole + frac / (1 << kCoordFracBits);
		return negative ? -v : v;
	}
	if (flags & (SPROP_COORD_MP | SPROP_COORD_MP_LOWPRECISION | SPROP_COORD_MP_INTEGRAL)) {
		const bool inBounds = br.bit();
		const int intBits = inBounds ? kCoordIntBitsMp : kCoordIntBits;
		if (flags & SPROP_COORD_MP_INTEGRAL) {
			if (!br.bit()) return 0;
			const bool negative = br.bit();
			const double v = double(br.u(intBits)) + 1;
			return negative ? -v : v;
		}
		const bool hasInt = br.bit();
		const bool negative = br.bit();
		const double whole = hasInt ? double(br.u(intBits)) + 1 : 0;
		const int fracBits = (flags & SPROP_COORD_MP_LOWPRECISION) ? kCoordFracBitsMpLowPrecision : kCoordFracBits;
		const double frac = double(br.u(fracBits));
		const double v = whole + frac / (1 << fracBits);
		return negative ? -v : v;
	}
	if (flags & SPROP_NOSCALE) return double(br.f32());
	if (flags & SPROP_NORMAL) {
		const bool negative = br.bit();
		const double v = double(br.u(kNormalFracBits)) / ((1 << kNormalFracBits) - 1);
		return negative ? -v : v;
	}
	if (flags & (SPROP_CELL_COORD | SPROP_CELL_COORD_LOWPRECISION)) {
		const double whole = br.number(p.bits, false);
		const int fracBits = (flags & SPROP_CELL_COORD) ? kCoordFracBits : kCoordFracBitsMpLowPrecision;
		const double frac = double(br.u(fracBits));
		return whole + frac / (1 << fracBits);
	}
	if (flags & SPROP_CELL_COORD_INTEGRAL) return br.number(p.bits, false);
	// quantized between low and high
	const double raw = br.number(p.bits, false);
	return p.low + (p.high - p.low) * (raw / (std::ldexp(1.0, p.bits) - 1));
}

void ReadValue(BitReader &br, const PropCoding &p, const PropCoding *element, PropValue &out) {
	switch (p.type) {
	case PROP_INT:
		out.v[0] = br.number(p.bits, !(p.flags & SPROP_UNSIGNED));
		out.kind = PropValue::NUMBER;
		out.count = 1;
		return;
	case PROP_FLOAT:
		out.v[0] = ReadFloat(br, p);
		out.kind = PropValue::NUMBER;
		out.count = 1;
		return;
	case PROP_VEC3: {
		const double x = ReadFloat(br, p);
		const double y = ReadFloat(br, p);
		double z;
		if (p.flags & SPROP_NORMAL) {
			// a unit vector: z is implied, only its sign is sent
			const bool negative = br.bit();
			const double rest = 1 - x * x - y * y;
			z = std::isnan(rest) ? rest : std::sqrt(rest > 0 ? rest : 0);
			if (negative) z = -z;
		} else {
			z = ReadFloat(br, p);
		}
		out.v[0] = x;
		out.v[1] = y;
		out.v[2] = z;
		out.kind = PropValue::VECTOR;
		out.count = 3;
		return;
	}
	case PROP_VEC2: {
		const double x = ReadFloat(br, p);
		const double y = ReadFloat(br, p);
		out.v[0] = x;
		out.v[1] = y;
		out.v[2] = 0;
		out.kind = PropValue::VECTOR;
		out.count = 2;
		return;
	}
	case PROP_STRING: {
		const uint32_t length = br.u(kStringLengthBits);
		br.skip(uint64_t(length) * 8);
		out.kind = PropValue::OTHER;
		out.count = 0;
		return;
	}
	case PROP_ARRAY: {
		const uint32_t count = br.u(HighestBit(uint32_t(p.elements)) + 1);
		PropValue ignored;
		for (uint32_t i = 0; i < count; i++) {
			if (!element) throw DecodeError("array prop without an element prop");
			ReadValue(br, *element, nullptr, ignored);
		}
		out.kind = PropValue::OTHER;
		out.count = 0;
		return;
	}
	default:
		throw DecodeError("bad prop type " + std::to_string(p.type));
	}
}

// The next changed field's index, or -1 after the last.
int ReadFieldIndex(BitReader &br, int last, bool newWay) {
	if (newWay && br.bit()) return last + 1;
	uint32_t step;
	if (newWay && br.bit()) {
		step = br.u(3);
	} else {
		step = br.u(5);
		switch (br.u(2)) {
		case 1: step |= br.u(2) << 5; break;
		case 2: step |= br.u(4) << 5; break;
		case 3: step |= br.u(7) << 5; break;
		default: break;
		}
	}
	return step == kEndOfFields ? -1 : last + 1 + int(step);
}

}  // namespace

uint64_t ReadUBitInt(BitReader &br) {
	const uint64_t low = br.u(4);
	switch (br.u(2)) {
	case 0: return low;
	case 1: return low | (uint64_t(br.u(4)) << 4);
	case 2: return low | (uint64_t(br.u(8)) << 4);
	default: return low + uint64_t(br.u(28)) * 16;
	}
}

void ReadEntityProps(BitReader &br, const std::vector<FlatProp> &props, EntityState &state, std::vector<int> &changed) {
	changed.clear();
	if (state.size() != props.size()) state.resize(props.size());
	const bool newWay = br.bit();
	int index = -1;
	for (;;) {
		index = ReadFieldIndex(br, index, newWay);
		if (index == -1) break;
		if (index < 0 || size_t(index) >= props.size()) {
			throw DecodeError("bad field index " + std::to_string(index) + " (" + std::to_string(props.size()) + " props)");
		}
		const FlatProp &prop = props[size_t(index)];
		ReadValue(br, prop.coding, prop.hasElement ? &prop.element : nullptr, state[size_t(index)]);
		changed.push_back(index);
	}
}

}  // namespace detail
}  // namespace p2demo
