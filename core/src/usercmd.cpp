#include "usercmd.h"

namespace p2demo {
namespace detail {

namespace {
constexpr int kEdictBits = 11;
constexpr int kWeaponSubtypeBits = 6;
}  // namespace

UserCmd DecodeUserCmd(ByteSpan data, int tick, int slot) {
	UserCmd cmd;
	cmd.tick = tick;
	cmd.slot = slot;
	try {
		BitReader br(data);
		// every field: a "changed" bit, then the value (relative to an all-zero command)
		if (br.bit()) cmd.commandNumber = int(br.u(32));
		if (br.bit()) br.u(32);  // tick count
		if (br.bit()) cmd.viewAngles.x = br.f32();
		if (br.bit()) cmd.viewAngles.y = br.f32();
		if (br.bit()) cmd.viewAngles.z = br.f32();
		if (br.bit()) cmd.forwardMove = br.f32();
		if (br.bit()) cmd.sideMove = br.f32();
		if (br.bit()) cmd.upMove = br.f32();
		if (br.bit()) cmd.buttons = int(br.u(32));
		if (br.bit()) cmd.impulse = int(br.u(8));
		if (br.bit()) {
			br.u(kEdictBits);  // weapon select
			if (br.bit()) br.u(kWeaponSubtypeBits);
		}
		if (br.bit()) cmd.mouseDx = br.s(16);
		if (br.bit()) cmd.mouseDy = br.s(16);
	} catch (const DecodeError &) {
		// a short buffer just leaves the remaining fields at zero
	}
	return cmd;
}

}  // namespace detail
}  // namespace p2demo
