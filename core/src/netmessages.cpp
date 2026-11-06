#include "netmessages.h"

#include "datatables.h"

namespace p2demo {
namespace detail {

namespace {

constexpr int kMessageTypeBits = 6;
constexpr int kEdictBits = 11;  // entity indices: 2048 entities
constexpr int kUserMessageLengthBits = 12;
constexpr int kModelIndexBits = 11;
constexpr int kCoordIntBits = 14, kCoordFracBits = 5;

// net_* and svc_* message numbers of Portal 2
enum NetMessage : uint32_t {
	NET_NOP = 0,
	NET_DISCONNECT = 1,
	NET_FILE = 2,
	NET_SPLITSCREENUSER = 3,
	NET_TICK = 4,
	NET_STRINGCMD = 5,
	NET_SETCONVAR = 6,
	NET_SIGNONSTATE = 7,
	SVC_SERVERINFO = 8,
	SVC_SENDTABLE = 9,
	SVC_CLASSINFO = 10,
	SVC_SETPAUSE = 11,
	SVC_CREATESTRINGTABLE = 12,
	SVC_UPDATESTRINGTABLE = 13,
	SVC_VOICEINIT = 14,
	SVC_VOICEDATA = 15,
	SVC_PRINT = 16,
	SVC_SOUNDS = 17,
	SVC_SETVIEW = 18,
	SVC_FIXANGLE = 19,
	SVC_CROSSHAIRANGLE = 20,
	SVC_BSPDECAL = 21,
	SVC_SPLITSCREEN = 22,
	SVC_USERMESSAGE = 23,
	SVC_ENTITYMESSAGE = 24,
	SVC_GAMEEVENT = 25,
	SVC_PACKETENTITIES = 26,
	SVC_TEMPENTITIES = 27,
	SVC_PREFETCH = 28,
	SVC_MENU = 29,
	SVC_GAMEEVENTLIST = 30,
	SVC_GETCVARVALUE = 31,
	SVC_CMDKEYVALUES = 32,
	SVC_PAINTMAPDATA = 33,
};

// a length of `lengthBits` bits, then that many units
void SkipCounted(BitReader &br, int lengthBits, uint64_t unitBits) {
	const uint64_t count = br.u(lengthBits);
	br.skip(count * unitBits);
}

void ReadCreateStringTable(BitReader &br, StringTables &tables, NetHandler &handler) {
	const std::string name = br.cstring();
	const uint32_t maxEntries = br.u(16);
	const uint32_t entryCount = br.u(HighestBit(maxEntries) + 1);
	const uint32_t dataBits = br.u(20);
	const bool fixedSize = br.bit();
	uint32_t userDataBits = 0;
	if (fixedSize) {
		br.skip(12);  // user data size in bytes
		userDataBits = br.u(4);
	}
	const uint32_t flags = br.u(2);

	// (the table exists from here on, whether or not its entries can be read:
	// later updates name tables by their position)
	StringTable &table = tables.create(name);
	table.maxEntries = int(maxEntries);
	table.fixedSize = fixedSize;
	table.userDataBits = int(userDataBits);

	BitReader blob = br.sub(dataBits);
	std::vector<uint8_t> unpacked;
	if (flags & 1) {
		// compressed: u32 unpacked size, u32 packed size, then "LZSS", u32 size, data
		blob.u(32);
		const uint32_t packedSize = blob.u(32);
		if (uint64_t(packedSize) * 8 > blob.remaining()) throw DecodeError("compressed string table runs past its message");
		std::vector<uint8_t> packed(packedSize);
		for (uint32_t i = 0; i < packedSize; i++) packed[i] = uint8_t(blob.u(8));
		const size_t headerSize = packedSize < 8 ? packedSize : 8;
		unpacked = LzssDecompress(ByteSpan{packed.data() + headerSize, packedSize - headerSize});
		blob = BitReader(unpacked.data(), unpacked.size());
	}
	tables.applyEntries(table, blob, entryCount);
	handler.onTableChange(table);
}

void ReadUpdateStringTable(BitReader &br, StringTables &tables, NetHandler &handler) {
	const uint32_t tableIndex = br.u(5);
	const uint32_t entryCount = br.bit() ? br.u(16) : 1;
	const uint32_t dataBits = br.u(20);
	BitReader blob = br.sub(dataBits);
	if (StringTable *table = tables.at(tableIndex)) {
		tables.applyEntries(*table, blob, entryCount);
		handler.onTableChange(*table);
	}
}

void SkipBspDecal(BitReader &br) {
	// a position: per axis an optional coordinate
	const bool hasX = br.bit();
	const bool hasY = br.bit();
	const bool hasZ = br.bit();
	for (const bool has : {hasX, hasY, hasZ}) {
		if (!has) continue;
		const bool hasInt = br.bit();
		const bool hasFrac = br.bit();
		if (hasInt || hasFrac) {
			br.bit();  // sign
			if (hasInt) br.skip(kCoordIntBits);
			if (hasFrac) br.skip(kCoordFracBits);
		}
	}
	br.skip(9);  // decal texture
	if (br.bit()) br.skip(kEdictBits + kModelIndexBits);
	br.skip(1);  // low priority
}

void ReadPacketEntities(BitReader &br, NetHandler &handler) {
	PacketEntitiesHeader header;
	header.maxEntries = int(br.u(kEdictBits));
	header.isDelta = br.bit();
	header.deltaFrom = header.isDelta ? br.s(32) : -1;
	header.baseline = br.bit() ? 1 : 0;
	header.updatedEntries = int(br.u(kEdictBits));
	const uint32_t dataBits = br.u(20);
	header.updateBaseline = br.bit();
	handler.onPacketEntities(header, br.sub(dataBits));
}

}  // namespace

void WalkPacket(ByteSpan packet, StringTables &tables, int classBits, NetHandler &handler) {
	BitReader br(packet);
	while (br.remaining() >= kMessageTypeBits) {
		const uint32_t type = br.u(kMessageTypeBits);
		switch (type) {
		case NET_NOP:
			break;
		case NET_DISCONNECT:
			br.skipCString();
			break;
		case NET_FILE:
			br.skip(32);
			br.skipCString();
			br.skip(2);
			break;
		case NET_SPLITSCREENUSER:
			br.skip(1);
			break;
		case NET_TICK:
			br.skip(64);
			break;
		case NET_STRINGCMD:
			br.skipCString();
			break;
		case NET_SETCONVAR: {
			const uint32_t count = br.u(8);
			for (uint32_t i = 0; i < count; i++) {
				br.skipCString();
				br.skipCString();
			}
			break;
		}
		case NET_SIGNONSTATE:
			br.skip(72);
			SkipCounted(br, 32, 8);  // player network ids
			SkipCounted(br, 32, 8);  // map name
			break;
		case SVC_SERVERINFO:
			br.skip(16 + 32 + 1 + 1 + 32 + 32 + 16 + 32 + 8 + 8 + 32 + 8);
			br.skipCString();  // game dir
			br.skipCString();  // map
			br.skipCString();  // sky
			br.skipCString();  // host name
			break;
		case SVC_SENDTABLE:
			throw DecodeError("svc_SendTable in a packet");
		case SVC_CLASSINFO: {
			const uint32_t count = br.u(16);
			if (!br.bit()) {  // not "create on client": the classes follow
				for (uint32_t i = 0; i < count; i++) {
					br.skip(uint64_t(classBits));
					br.skipCString();
					br.skipCString();
				}
			}
			break;
		}
		case SVC_SETPAUSE:
			br.skip(1);
			break;
		case SVC_CREATESTRINGTABLE:
			ReadCreateStringTable(br, tables, handler);
			break;
		case SVC_UPDATESTRINGTABLE:
			ReadUpdateStringTable(br, tables, handler);
			break;
		case SVC_VOICEINIT:
			br.skipCString();
			if (br.u(8) == 255) br.skip(16);
			break;
		case SVC_VOICEDATA:
			br.skip(16);
			SkipCounted(br, 16, 1);
			break;
		case SVC_PRINT:
			br.skipCString();
			break;
		case SVC_SOUNDS: {
			const bool reliable = br.bit();
			if (!reliable) br.skip(8);  // sound count
			SkipCounted(br, reliable ? 8 : 16, 1);
			break;
		}
		case SVC_SETVIEW:
			br.skip(kEdictBits);
			break;
		case SVC_FIXANGLE:
			br.skip(49);
			break;
		case SVC_CROSSHAIRANGLE:
			br.skip(48);
			break;
		case SVC_BSPDECAL:
			SkipBspDecal(br);
			break;
		case SVC_SPLITSCREEN:
			br.skip(2 + kEdictBits);
			break;
		case SVC_USERMESSAGE: {
			const int messageType = int(br.u(8));
			const uint32_t bits = br.u(kUserMessageLengthBits);
			handler.onUserMessage(messageType, br.sub(bits));
			break;
		}
		case SVC_ENTITYMESSAGE:
			br.skip(kEdictBits + 9);
			SkipCounted(br, 11, 1);
			break;
		case SVC_GAMEEVENT:
			SkipCounted(br, 11, 1);
			break;
		case SVC_PACKETENTITIES:
			ReadPacketEntities(br, handler);
			break;
		case SVC_TEMPENTITIES:
			br.skip(8);  // entry count
			SkipCounted(br, 17, 1);
			break;
		case SVC_PREFETCH:
			br.skip(13);
			break;
		case SVC_MENU:
			br.skip(16);
			SkipCounted(br, 32, 1);
			break;
		case SVC_GAMEEVENTLIST:
			br.skip(9);
			SkipCounted(br, 20, 1);
			break;
		case SVC_GETCVARVALUE:
			br.skip(32);
			br.skipCString();
			break;
		case SVC_CMDKEYVALUES:
			SkipCounted(br, 32, 8);
			break;
		case SVC_PAINTMAPDATA:
			SkipCounted(br, 32, 1);
			break;
		default:
			throw DecodeError("unknown net message " + std::to_string(type));
		}
	}
}

}  // namespace detail
}  // namespace p2demo
