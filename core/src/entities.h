// Decodes a demo's network data into what the Demo struct holds about
// entities: the players' and portals' states over time, the things that
// moved, portal shots.  Mirrors decodeEntities in sar-web
// src/net/entities.ts.
#pragma once

#include <vector>

#include "p2demo/demo.h"
#include "readers.h"

namespace p2demo {
namespace detail {

// One demo frame of network data, in file order
struct NetRecord {
	enum Kind {
		PACKET,                 // dem_signon / dem_packet: net messages
		STRING_TABLE_SNAPSHOT,  // dem_stringtables
	};
	Kind kind = PACKET;
	int tick = 0;
	ByteSpan data;
	bool cutShort = false;  // the file ends inside this frame; `data` is what there was of it
};

// Fills demo.players, portals, movers, portalShots, recorderEntIndex and
// entitiesDecoded (needs demo.views for the recorder), and adds warnings.
// When a complete frame can't be decoded the entity lists are left empty and
// entitiesDecoded false.  Throws DecodeError only for unusable data tables.
void DecodeEntities(const std::vector<NetRecord> &records, ByteSpan dataTables, const ParseOptions &options, Demo &demo);

}  // namespace detail
}  // namespace p2demo
