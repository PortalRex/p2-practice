// Walks the net messages inside a demo packet (Portal 2, network protocol
// 2001): string table changes, svc_PacketEntities and user messages go to
// the handler, everything else is skipped.  Mirrors sar-web
// src/net/messages.ts.
#pragma once

#include "readers.h"
#include "stringtables.h"

namespace p2demo {
namespace detail {

struct PacketEntitiesHeader {
	int maxEntries = 0;
	bool isDelta = false;         // false: a full snapshot that replaces everything
	int deltaFrom = -1;
	int baseline = 0;             // which of the two baselines new entities start from
	int updatedEntries = 0;
	bool updateBaseline = false;  // the other baseline becomes this snapshot's state
};

class NetHandler {
public:
	virtual ~NetHandler() = default;
	virtual void onTableChange(StringTable &table) = 0;
	virtual void onPacketEntities(const PacketEntitiesHeader &header, BitReader data) = 0;
	virtual void onUserMessage(int type, BitReader data) = 0;
	virtual void onServerTick(int /*serverTick*/) {}  // the server's own tick count, sent with every packet
};

// Throws DecodeError at a message it can't read; the handler has then seen
// the messages before it.
void WalkPacket(ByteSpan packet, StringTables &tables, int classBits, NetHandler &handler);

}  // namespace detail
}  // namespace p2demo
