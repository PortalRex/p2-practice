// CUserCmd as CInput::EncodeUserCmdToBuffer writes it into dem_usercmd
// frames.  Mirrors sar-web src/core/usercmd.ts.
#pragma once

#include "p2demo/demo.h"
#include "readers.h"

namespace p2demo {
namespace detail {

// Never throws: a buffer that ends early leaves the remaining fields at zero.
UserCmd DecodeUserCmd(ByteSpan data, int tick, int slot);

}  // namespace detail
}  // namespace p2demo
