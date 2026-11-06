// The demo container (header and frames) and the public entry points.
// Mirrors readDem in sar-web src/core/dem.ts.
#include "p2demo/demo.h"

#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

#include "entities.h"
#include "readers.h"
#include "sardata.h"
#include "usercmd.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace p2demo {

namespace {

using detail::ByteReader;
using detail::ByteSpan;
using detail::DecodeError;
using detail::NetRecord;

constexpr char kMagic[8] = {'H', 'L', '2', 'D', 'E', 'M', 'O', '\0'};
constexpr size_t kHeaderSize = 1072;
constexpr size_t kPathSize = 260;        // the header's fixed-size strings
constexpr size_t kFrameHeaderSize = 6;   // type, tick, slot
constexpr size_t kCmdInfoSize = 76;      // democmdinfo_t, one per split-screen slot
constexpr size_t kCmdInfoSlots = 2;
constexpr int kPortal2DemoProtocol = 4;
constexpr size_t kMaxDataTablesSize = 4u << 20;  // Portal 2's are under 100 KB
constexpr long long kMaxFileSize = 1ll << 30;

enum FrameType : uint8_t {
	DEM_SIGNON = 1,
	DEM_PACKET = 2,
	DEM_SYNCTICK = 3,
	DEM_CONSOLECMD = 4,
	DEM_USERCMD = 5,
	DEM_DATATABLES = 6,
	DEM_STOP = 7,
	DEM_CUSTOMDATA = 8,
	DEM_STRINGTABLES = 9,
};

// A frame's variable part: a length, then that many bytes
struct Payload {
	ByteSpan data;
	bool cutShort = false;  // the file ends before the length is up; `data` is what there is
};

// A length no frame can have (the reference reader has no such check)
class BadFrameSize : public DecodeError {
public:
	BadFrameSize() : DecodeError("negative frame size") {}
};

Payload ReadPayload(ByteReader &r) {
	const int32_t size = r.i32();
	if (size < 0) throw BadFrameSize();
	Payload payload;
	payload.cutShort = size_t(size) > r.remaining();
	payload.data = r.bytes(payload.cutShort ? r.remaining() : size_t(size));
	return payload;
}

// The values of the FCVAR_DEMO cvars the engine writes when recording
// starts (CDemoRecorder::WriteDemoCvars) look like: name "value"
bool LooksLikeCvarValue(const std::string &text) {
	const size_t space = text.find(' ');
	if (space == std::string::npos || space == 0) return false;
	return text.size() >= space + 3 && text[space + 1] == '"' && text.back() == '"';
}

class ContainerReader {
public:
	ContainerReader(const uint8_t *data, size_t size, Demo &demo) : r_(data, size, sizeof kMagic), demo_(demo) {}

	void readHeader() {
		Header &h = demo_.header;
		h.demoProtocol = r_.i32();
		h.networkProtocol = r_.i32();
		h.server = r_.fixedString(kPathSize);
		h.client = r_.fixedString(kPathSize);
		h.map = r_.fixedString(kPathSize);
		h.gameDir = r_.fixedString(kPathSize);
		h.playbackTime = r_.f32();
		h.playbackTicks = r_.i32();
		h.playbackFrames = r_.i32();
		r_.i32();  // signon length

		if (h.demoProtocol != kPortal2DemoProtocol) {
			demo_.warnings.push_back("Demo protocol " + std::to_string(h.demoProtocol) + "; Portal 2's is " + std::to_string(kPortal2DemoProtocol) + ".");
		}
		if (h.playbackTicks > 0 && h.playbackTime > 0) demo_.intervalPerTick = float(double(h.playbackTime) / h.playbackTicks);
	}

	void readFrames() {
		bool stopped = false;
		while (!stopped && r_.remaining() >= kFrameHeaderSize) {
			const size_t frameStart = r_.pos();
			const uint8_t type = r_.u8();
			const int32_t tick = r_.i32();
			const uint8_t slot = r_.u8();
			try {
				stopped = !readFrame(type, tick, slot, frameStart);
			} catch (const BadFrameSize &) {
				demo_.warnings.push_back("The frame at byte " + std::to_string(frameStart) + " has an impossible size; stopped reading there.");
				stoppedEarly_ = true;
				stopped = true;
			} catch (const DecodeError &) {
				cutShort_ = true;
			}
			if (cutShort_) stopped = true;
		}
		if (cutShort_) {
			demo_.warnings.push_back("The demo ends in the middle of a frame (it was cut short).");
		} else if (!sawStop_ && !stoppedEarly_) {
			demo_.warnings.push_back("The demo has no stop frame (the recording wasn't finished).");
		}
		if (!sawView_) firstTick_ = lastTick_ = 0;
		demo_.firstTick = firstTick_;
		demo_.lastTick = lastTick_;
	}

	const std::vector<NetRecord> &netRecords() const { return net_; }
	bool hasDataTables() const { return dataTables_.data != nullptr && !dataTablesCutShort_; }
	ByteSpan dataTables() const { return dataTables_; }

private:
	// false: stop reading
	bool readFrame(uint8_t type, int32_t tick, uint8_t slot, size_t frameStart) {
		// The cvar values follow the sync tick, the custom data callback
		// table and the string tables; anything else ends them.
		const bool cvarValuesMayFollow = cvarValuesMayFollow_;
		cvarValuesMayFollow_ = false;

		switch (type) {
		case DEM_SIGNON:
		case DEM_PACKET: {
			if (type == DEM_SIGNON && synced_) {
				// A new connection: another map.  (Portal 2 starts a new demo file
				// per map, so this is not expected.)
				demo_.warnings.push_back("A second map starts at byte " + std::to_string(frameStart) + "; only the first is read.");
				stoppedEarly_ = true;
				return false;
			}
			View view;
			view.tick = tick;
			readCmdInfo(view);
			r_.i32();  // sequence numbers
			r_.i32();
			const Payload payload = ReadPayload(r_);
			addNetRecord(NetRecord::PACKET, tick, payload);
			if (type == DEM_PACKET && synced_ && tick >= 0) {
				demo_.views.push_back(view);
				if (!sawView_ || tick < firstTick_) firstTick_ = tick;
				if (!sawView_ || tick > lastTick_) lastTick_ = tick;
				sawView_ = true;
			}
			return true;
		}
		case DEM_SYNCTICK:
			synced_ = true;
			cvarValuesMayFollow_ = true;
			return true;
		case DEM_CONSOLECMD: {
			const Payload payload = ReadPayload(r_);
			cutShort_ = payload.cutShort;
			ConsoleCmd cmd;
			cmd.tick = tick;
			cmd.slot = slot;
			size_t length = payload.data.size;
			while (length > 0 && payload.data.data[length - 1] == 0) length--;
			cmd.text.assign(reinterpret_cast<const char *>(payload.data.data), length);
			if (cvarValuesMayFollow && LooksLikeCvarValue(cmd.text)) {
				cvarValuesMayFollow_ = true;
			} else {
				demo_.consoleCmds.push_back(std::move(cmd));
			}
			return true;
		}
		case DEM_USERCMD: {
			r_.i32();  // outgoing sequence
			const Payload payload = ReadPayload(r_);
			cutShort_ = payload.cutShort;
			if (tick >= 0) demo_.userCmds.push_back(detail::DecodeUserCmd(payload.data, tick, slot));
			return true;
		}
		case DEM_DATATABLES: {
			const Payload payload = ReadPayload(r_);
			cutShort_ = payload.cutShort;
			dataTables_ = payload.data;
			dataTablesCutShort_ = payload.cutShort;
			return true;
		}
		case DEM_STRINGTABLES: {
			const Payload payload = ReadPayload(r_);
			addNetRecord(NetRecord::STRING_TABLE_SNAPSHOT, tick, payload);
			cvarValuesMayFollow_ = cvarValuesMayFollow;
			return true;
		}
		case DEM_CUSTOMDATA: {
			const int32_t callback = r_.i32();
			const Payload payload = ReadPayload(r_);
			cutShort_ = payload.cutShort;
			// callback 0 is what SAR writes through; -1 is the engine's table of callback names
			if (callback == 0) detail::DecodeSarData(payload.data, tick, demo_);
			if (callback == -1) cvarValuesMayFollow_ = cvarValuesMayFollow;
			return true;
		}
		case DEM_STOP:
			sawStop_ = true;
			return false;
		default:
			demo_.warnings.push_back("Unknown frame type " + std::to_string(type) + " at byte " + std::to_string(frameStart) + "; stopped reading there.");
			stoppedEarly_ = true;
			return false;
		}
	}

	// democmdinfo_t per split-screen slot: flags, view origin, view angles,
	// local view angles, then the same again for a second (interpolated)
	// view.  The first slot's origin and angles are the recorded view.
	void readCmdInfo(View &view) {
		const ByteSpan info = r_.bytes(kCmdInfoSize * kCmdInfoSlots);
		ByteReader first(info.data, info.size);
		first.i32();  // flags
		view.origin.x = first.f32();
		view.origin.y = first.f32();
		view.origin.z = first.f32();
		view.angles.x = first.f32();
		view.angles.y = first.f32();
		view.angles.z = first.f32();
	}

	void addNetRecord(NetRecord::Kind kind, int32_t tick, const Payload &payload) {
		NetRecord record;
		record.kind = kind;
		record.tick = tick > 0 ? tick : 0;
		record.data = payload.data;
		record.cutShort = payload.cutShort;
		net_.push_back(record);
		cutShort_ = payload.cutShort;
	}

	ByteReader r_;
	Demo &demo_;
	std::vector<NetRecord> net_;
	ByteSpan dataTables_;
	bool dataTablesCutShort_ = false;
	bool synced_ = false;
	bool cvarValuesMayFollow_ = false;
	bool sawStop_ = false;
	bool stoppedEarly_ = false;  // at something that isn't the end of a recording
	bool cutShort_ = false;      // the file ends inside a frame
	bool sawView_ = false;
	int firstTick_ = 0, lastTick_ = 0;
};

bool ParseChecked(const uint8_t *data, size_t size, Demo &out, std::string &error, const ParseOptions &options) {
	if (!data || size < sizeof kMagic || std::memcmp(data, kMagic, sizeof kMagic) != 0) {
		error = "Not a Source demo (.dem).";
		return false;
	}
	if (size < kHeaderSize) {
		error = "The demo ends inside its header.";
		return false;
	}

	ContainerReader container(data, size, out);
	container.readHeader();
	container.readFrames();

	if (!options.entities) return true;
	if (!container.hasDataTables()) {
		out.warnings.push_back("The demo has no (complete) data tables; entities weren't decoded.");
		return true;
	}
	if (container.dataTables().size > kMaxDataTablesSize) {
		out.warnings.push_back("The data tables are implausibly large; entities weren't decoded.");
		return true;
	}
	try {
		detail::DecodeEntities(container.netRecords(), container.dataTables(), options, out);
	} catch (const DecodeError &e) {
		out.entitiesDecoded = false;
		out.warnings.push_back(std::string("The data tables couldn't be read (") + e.what() + "); entities weren't decoded.");
	} catch (const std::bad_alloc &) {
		out.entitiesDecoded = false;
		out.warnings.push_back("Ran out of memory decoding the network data; entities weren't decoded.");
	}
	return true;
}

// Clears what a failed entity decode may have left half-filled
void ClearEntities(Demo &demo) {
	demo.entitiesDecoded = false;
	demo.recorderEntIndex = 0;
	demo.players.clear();
	demo.portals.clear();
	demo.movers.clear();
	demo.portalShots.clear();
}

FILE *OpenForReading(const char *path) {
#ifdef _WIN32
	// UTF-8 paths first (the usual case); a path that isn't valid UTF-8 is
	// taken to be in the ANSI code page.
	const int wideLength = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, nullptr, 0);
	if (wideLength > 0) {
		std::vector<wchar_t> wide(static_cast<size_t>(wideLength));
		if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide.data(), wideLength) > 0) {
			FILE *file = nullptr;
			if (_wfopen_s(&file, wide.data(), L"rb") == 0) return file;
			return nullptr;
		}
	}
	FILE *file = nullptr;
	if (fopen_s(&file, path, "rb") == 0) return file;
	return nullptr;
#else
	return std::fopen(path, "rb");
#endif
}

long long FileSize(FILE *file) {
#ifdef _WIN32
	if (_fseeki64(file, 0, SEEK_END) != 0) return -1;
	const long long size = _ftelli64(file);
	if (_fseeki64(file, 0, SEEK_SET) != 0) return -1;
	return size;
#else
	if (fseeko(file, 0, SEEK_END) != 0) return -1;
	const long long size = ftello(file);
	if (fseeko(file, 0, SEEK_SET) != 0) return -1;
	return size;
#endif
}

}  // namespace

bool Parse(const uint8_t *data, size_t size, Demo &out, std::string &error, const ParseOptions &options) {
	try {
		out = Demo();
		error.clear();
		const bool ok = ParseChecked(data, size, out, error, options);
		if (!out.entitiesDecoded) ClearEntities(out);
		return ok;
	} catch (const std::bad_alloc &) {
		error = "Out of memory reading the demo.";
	} catch (const std::exception &e) {
		try {
			error = e.what();
		} catch (...) {
		}
	} catch (...) {
		error = "Unexpected failure reading the demo.";
	}
	return false;
}

bool ParseFile(const char *path, Demo &out, std::string &error, const ParseOptions &options) {
	try {
		out = Demo();
		error.clear();
		if (!path || !*path) {
			error = "No file name.";
			return false;
		}
		FILE *file = OpenForReading(path);
		if (!file) {
			error = std::string("Can't open ") + path + ".";
			return false;
		}
		std::vector<uint8_t> bytes;
		const long long size = FileSize(file);
		bool readOk = size >= 0 && size <= kMaxFileSize;
		if (readOk) {
			try {
				bytes.resize(size_t(size));
			} catch (const std::bad_alloc &) {
				std::fclose(file);
				error = "Out of memory reading the demo.";
				return false;
			}
			readOk = bytes.empty() || std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size();
		}
		std::fclose(file);
		if (!readOk) {
			error = size > kMaxFileSize ? std::string("The file is too large to be a demo.") : std::string("Can't read ") + path + ".";
			return false;
		}
		return Parse(bytes.data(), bytes.size(), out, error, options);
	} catch (const std::bad_alloc &) {
		error = "Out of memory reading the demo.";
	} catch (...) {
		error = "Unexpected failure reading the demo.";
	}
	return false;
}

}  // namespace p2demo
