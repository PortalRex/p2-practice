// What SAR (SourceAutoRecord) writes into a demo: custom data frames with
// callback 0, holding the game's own 8-byte co-op radial menu cursor
// position, then a type byte and the record.  Mirrors sar-web
// src/core/sardata.ts (see SAR's RecordData callers for the formats).
#pragma once

#include "p2demo/demo.h"
#include "readers.h"

namespace p2demo {
namespace detail {

// Adds the record to the matching list of `demo` (records of other types,
// and ones that end early, are skipped).  Never throws DecodeError.
void DecodeSarData(ByteSpan data, int tick, Demo &demo);

}  // namespace detail
}  // namespace p2demo
