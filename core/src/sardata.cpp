#include "sardata.h"

namespace p2demo {
namespace detail {

namespace {

constexpr size_t kCursorBytes = 8;  // the radial menu cursor position before SAR's record

enum SarRecord : uint8_t {
	SAR_TIMESCALE = 0x01,
	SAR_ENTITY_INPUT = 0x03,
	SAR_ENTITY_INPUT_SLOT = 0x04,
	SAR_PORTAL_PLACEMENT = 0x05,
	SAR_CM_FLAGS = 0x06,
	SAR_SPEEDRUN_TIME = 0x0a,     // a finished run
	SAR_SPEEDRUN_SUMMARY = 0x12,  // the timer's state (written at the start of a demo)
};

// Adds in 64 bits and wraps: tick counts come from the file
int SumTicks(int64_t sum) {
	return int(uint32_t(uint64_t(sum)));
}

void ReadSpeedrun(ByteReader &r, int tick, bool complete, Demo &demo) {
	SarSpeedrun run;
	run.tick = tick;
	run.complete = complete;
	int64_t total = 0;
	const int32_t splitCount = r.i32();
	for (int32_t i = 0; i < splitCount; i++) {
		SarSplit split;
		split.name = r.cstring();
		int64_t splitTicks = 0;
		const int32_t segmentCount = r.i32();
		for (int32_t j = 0; j < segmentCount; j++) {
			r.cstring();  // segment name
			splitTicks += r.i32();
		}
		split.ticks = SumTicks(splitTicks);
		total += splitTicks;
		run.splits.push_back(std::move(split));
	}
	// (the category's rules follow; not needed)
	run.totalTicks = SumTicks(total);
	demo.sarSpeedruns.push_back(std::move(run));
}

}  // namespace

void DecodeSarData(ByteSpan data, int tick, Demo &demo) {
	if (data.size < kCursorBytes + 1) return;
	demo.hasSarData = true;
	ByteReader r(data.data, data.size, kCursorBytes);
	const uint8_t type = r.u8();
	try {
		switch (type) {
		case SAR_TIMESCALE: {
			SarTimescale e;
			e.tick = tick;
			e.timescale = r.f32();
			demo.sarTimescales.push_back(e);
			break;
		}
		case SAR_ENTITY_INPUT:
		case SAR_ENTITY_INPUT_SLOT: {
			SarEntityInput e;
			e.tick = tick;
			if (type == SAR_ENTITY_INPUT_SLOT && r.remaining() > 0) e.slot = r.u8();
			e.target = r.cstring();
			e.className = r.cstring();
			e.input = r.cstring();
			e.param = r.cstring();
			demo.sarEntityInputs.push_back(std::move(e));
			break;
		}
		case SAR_PORTAL_PLACEMENT: {
			SarPortal e;
			e.tick = tick;
			e.slot = r.u8();
			e.orange = r.u8() != 0;
			e.pos.x = r.f32();
			e.pos.y = r.f32();
			e.pos.z = r.f32();
			demo.sarPortals.push_back(e);
			break;
		}
		case SAR_CM_FLAGS: {
			SarFlags e;
			e.tick = tick;
			e.slot = r.u8();
			demo.sarFlags.push_back(e);
			break;
		}
		case SAR_SPEEDRUN_TIME:
		case SAR_SPEEDRUN_SUMMARY:
			ReadSpeedrun(r, tick, type == SAR_SPEEDRUN_TIME, demo);
			break;
		default:
			break;
		}
	} catch (const DecodeError &) {
		// a record that ends early: skipped
	}
}

}  // namespace detail
}  // namespace p2demo
