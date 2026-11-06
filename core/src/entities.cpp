#include "entities.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>

#include "datatables.h"
#include "netmessages.h"
#include "props.h"
#include "stringtables.h"

namespace p2demo {
namespace detail {

namespace {

constexpr int kMaxEntities = 2048;
constexpr int kEdictBits = 11;
constexpr int kSerialBits = 10;
constexpr double kMaxCoord = 16384;
constexpr int kEffectNoDraw = 32;        // EF_NODRAW
constexpr int kMsgPortalFxSurface = 48;  // Portal 2's user message number (registration order)

// Bounds on what one demo may make the decoder hold.  Real demos stay far
// below them (a 16 MB demo: about 100 thousand prop slots, under a million
// keys); a hostile file hits them instead of exhausting memory.
constexpr size_t kMaxStateSlots = 2u << 20;
constexpr size_t kMaxTracks = 100000;
constexpr size_t kMaxKeys = 6000000;
constexpr size_t kMaxBaselineLookups = 20000000;  // class baseline entries looked through
constexpr size_t kMaxFailedFrames = 3;     // decoding stops there: the entities are lost at the first
constexpr size_t kMaxBaselineWarnings = 3;

// ---- number conversions (the reference works in JavaScript numbers) ----

// JavaScript's ToInt32, as `value & mask` applies it
int32_t ToInt32(double v) {
	if (v > -2147483649.0 && v < 2147483648.0) return int32_t(v);  // the usual case, without fmod
	if (!std::isfinite(v)) return 0;
	double m = std::fmod(std::trunc(v), 4294967296.0);
	if (m < 0) m += 4294967296.0;
	return int32_t(uint32_t(m));
}

int ToInt(double v) {
	if (std::isnan(v)) return 0;
	if (v >= double(std::numeric_limits<int>::max())) return std::numeric_limits<int>::max();
	if (v <= double(std::numeric_limits<int>::min())) return std::numeric_limits<int>::min();
	return int(v);
}

float ToFloat(double v) {
	if (v > double(FLT_MAX)) return std::numeric_limits<float>::infinity();
	if (v < -double(FLT_MAX)) return -std::numeric_limits<float>::infinity();
	return float(v);
}

Vec3 ToVec3(const double v[3]) {
	Vec3 out;
	out.x = ToFloat(v[0]);
	out.y = ToFloat(v[1]);
	out.z = ToFloat(v[2]);
	return out;
}

// An EHANDLE prop's entity index (the low 11 bits), -1 for "none" or unset
int HandleIndex(double handle) {
	if (!(handle >= 0)) return -1;
	const int index = ToInt32(handle) & (kMaxEntities - 1);
	return index == kMaxEntities - 1 ? -1 : index;
}

// ---- text ----

bool IsJsWhitespace(uint32_t c) {
	return (c >= 0x09 && c <= 0x0d) || c == 0x20 || c == 0xa0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 ||
		c == 0x202f || c == 0x205f || c == 0x3000 || c == 0xfeff;
}

// Decodes the UTF-8 sequence at s[pos]; 0 length when it isn't a valid one.
size_t DecodeUtf8(const std::string &s, size_t pos, uint32_t &codePoint) {
	const auto byte = [&s](size_t i) -> uint32_t { return uint8_t(s[i]); };
	const uint32_t lead = byte(pos);
	size_t length;
	if (lead < 0x80) {
		codePoint = lead;
		return 1;
	} else if ((lead & 0xe0) == 0xc0) {
		length = 2;
		codePoint = lead & 0x1f;
	} else if ((lead & 0xf0) == 0xe0) {
		length = 3;
		codePoint = lead & 0x0f;
	} else if ((lead & 0xf8) == 0xf0) {
		length = 4;
		codePoint = lead & 0x07;
	} else {
		return 0;
	}
	if (pos + length > s.size()) return 0;
	for (size_t i = 1; i < length; i++) {
		if ((byte(pos + i) & 0xc0) != 0x80) return 0;
		codePoint = (codePoint << 6) | (byte(pos + i) & 0x3f);
	}
	return length;
}

// String.prototype.trim on UTF-8 bytes
std::string TrimJsWhitespace(const std::string &s) {
	size_t begin = 0, end = s.size();
	while (begin < end) {
		uint32_t c = 0;
		const size_t length = DecodeUtf8(s, begin, c);
		if (length == 0 || begin + length > end || !IsJsWhitespace(c)) break;
		begin += length;
	}
	while (end > begin) {
		size_t start = end - 1;
		while (start > begin && end - start < 4 && (uint8_t(s[start]) & 0xc0) == 0x80) start--;
		uint32_t c = 0;
		const size_t length = DecodeUtf8(s, start, c);
		if (length == 0 || start + length != end || !IsJsWhitespace(c)) break;
		end = start;
	}
	return s.substr(begin, end - begin);
}

// ---- what is recorded ----

enum class TrackKind {
	NONE,    // nothing kept for this class
	PLAYER,  // CPortal_Player
	PORTAL,  // CProp_Portal
	MOVER,   // anything else with a model index
};

// A portal's or mover's state as the reference compares it, in the numbers
// as decoded; a key is added when one of these changes.
struct MotionKey {
	int tick = 0;
	double origin[3] = {0, 0, 0};
	double angles[3] = {0, 0, 0};
	int parent = -1;
	bool inSnapshot = true;
	bool removed = false;
	bool noDraw = false;
	// portals
	bool active = false;
	int linkageGroup = 0;
	int team = 0;
	int firedBy = -1;
	int linkedPortal = -1;

	bool sameState(const MotionKey &o) const {
		return origin[0] == o.origin[0] && origin[1] == o.origin[1] && origin[2] == o.origin[2] && angles[0] == o.angles[0] &&
			angles[1] == o.angles[1] && angles[2] == o.angles[2] && parent == o.parent && inSnapshot == o.inSnapshot && removed == o.removed &&
			noDraw == o.noDraw && active == o.active && linkageGroup == o.linkageGroup && team == o.team && firedBy == o.firedBy &&
			linkedPortal == o.linkedPortal;
	}
	bool sameMotion(const MotionKey &o) const {
		return origin[0] == o.origin[0] && origin[1] == o.origin[1] && origin[2] == o.origin[2] && angles[0] == o.angles[0] &&
			angles[1] == o.angles[1] && angles[2] == o.angles[2];
	}
};

struct Track {
	TrackKind kind = TrackKind::NONE;
	int entIndex = 0;
	int classIndex = 0;
	std::string model;  // the first model the entity had
	bool isPortal2 = false;

	// players: `current` is the state as last recorded; a key is written
	// from it for every snapshot that holds the player.  Which copy of the
	// view offset counts is only known once the track is complete (see
	// ownerDataSeen), so the keys hold the owner's and the other is kept
	// beside them.
	std::vector<PlayerKey> playerKeys;
	std::vector<float> othersViewOffsetZ;  // one per key
	PlayerKey current;
	float currentOthersViewOffsetZ = 0;
	bool hasCurrent = false;
	bool currentRemoved = false;
	bool ownerDataSeen = false;  // the recording player's own entity

	// portals and movers
	std::vector<MotionKey> keys;
};

// Indices (into the class's flattened props) of the props read by name.
// -1: the class has no such prop.
struct OriginCopy {
	int origin = -1;  // m_vecOrigin: x, y and usually z
	int z = -1;       // m_vecOrigin[2], where z is sent on its own
	bool cellRelative = false;
};

struct ClassFields {
	TrackKind kind = TrackKind::NONE;
	// Every origin the class has.  Players have two: the owner's own
	// (local data) and the one everybody else is sent; an entity uses
	// whichever its updates carry.
	std::vector<OriginCopy> origins;
	int cellBits = -1, cellX = -1, cellY = -1, cellZ = -1;
	int angles = -1;
	int eyePitch = -1, eyeYaw = -1;
	int model = -1;
	int effects = -1;
	int moveParent = -1;

	// players
	int viewOffsetOwner = -1;   // a vector, in the owner's local data
	int viewOffsetOthers = -1;  // its z alone, as other clients get it
	int playerFlags = -1;
	int team = -1;
	int heldObject = -1;
	int velocity[3] = {-1, -1, -1};
	std::vector<uint8_t> ownerOnly;  // per prop: sent to the player it belongs to only

	// portals
	int activated = -1;
	int isPortal2 = -1;
	int linkageGroup = -1;
	int firedBy = -1;
	int linkedPortal = -1;
};

bool StartsWith(const std::string &s, std::string_view prefix) {
	return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

ClassFields FieldsFor(const ServerClass &cls, const ParseOptions &options) {
	ClassFields f;
	// (a rope's "model" is its material: not something that moves about)
	if (cls.name == "CPortal_Player") f.kind = TrackKind::PLAYER;
	else if (cls.name == "CProp_Portal") f.kind = TrackKind::PORTAL;
	else if (options.movers && cls.name != "CRopeKeyframe" && cls.find("m_nModelIndex") >= 0) f.kind = TrackKind::MOVER;
	if (f.kind == TrackKind::NONE) return f;

	static constexpr std::string_view kOriginSources[] = {
		"m_vecOrigin", "portallocaldata.m_vecOrigin", "localdata.m_vecOrigin", "portalnonlocaldata.m_vecOrigin", "nonlocaldata.m_vecOrigin",
	};
	for (const std::string_view source : kOriginSources) {
		OriginCopy copy;
		copy.origin = cls.find(source);
		if (copy.origin < 0) continue;
		copy.z = cls.find(std::string(source) + "[2]");
		copy.cellRelative =
			(cls.props[size_t(copy.origin)].coding.flags & (SPROP_CELL_COORD | SPROP_CELL_COORD_LOWPRECISION | SPROP_CELL_COORD_INTEGRAL)) != 0;
		f.origins.push_back(copy);
	}
	f.cellBits = cls.find("m_cellbits");
	f.cellX = cls.find("m_cellX");
	f.cellY = cls.find("m_cellY");
	f.cellZ = cls.find("m_cellZ");
	f.angles = cls.findFirst({"m_angRotation", "m_angAbsRotation"});
	f.eyePitch = cls.find("m_angEyeAngles[0]");
	f.eyeYaw = cls.find("m_angEyeAngles[1]");
	f.model = cls.find("m_nModelIndex");
	f.effects = cls.find("m_fEffects");
	f.moveParent = cls.findFirst({"moveparent", "m_hMoveParent"});

	if (f.kind == TrackKind::PLAYER) {
		f.viewOffsetOwner = cls.findFirst({"portallocaldata.m_vecViewOffset", "localdata.m_vecViewOffset"});
		f.viewOffsetOthers = cls.findFirst({"portalnonlocaldata.m_vecViewOffset[2]", "m_vecViewOffset[2]"});
		f.playerFlags = cls.find("m_fFlags");
		f.team = cls.find("m_iTeamNum");
		f.heldObject = cls.find("m_hAttachedObject");
		f.velocity[0] = cls.find("localdata.m_vecVelocity[0]");
		f.velocity[1] = cls.find("localdata.m_vecVelocity[1]");
		f.velocity[2] = cls.find("localdata.m_vecVelocity[2]");
		f.ownerOnly.resize(cls.props.size());
		for (size_t i = 0; i < cls.props.size(); i++) {
			f.ownerOnly[i] = StartsWith(cls.props[i].name, "localdata.") || StartsWith(cls.props[i].name, "portallocaldata.");
		}
	} else if (f.kind == TrackKind::PORTAL) {
		f.activated = cls.find("m_bActivated");
		f.isPortal2 = cls.find("m_bIsPortal2");
		f.linkageGroup = cls.find("m_iLinkageGroupID");
		f.team = cls.find("m_iTeamNum");
		f.firedBy = cls.find("m_hFiredByPlayer");
		f.linkedPortal = cls.find("m_hLinkedPortal");
	}
	return f;
}

// Counts the prop slots held by entity states and baselines.
class StateBudget {
public:
	void take(size_t slots) {
		if (slots > kMaxStateSlots - used_) throw DecodeError("the entities hold more state than a game can send");
		used_ += slots;
	}
	void give(size_t slots) { used_ -= slots; }

private:
	size_t used_ = 0;
};

// A set of prop values counted against the budget for as long as it lives
struct CountedState {
	CountedState(StateBudget &budget, size_t slots) : budget_(budget), slots_(slots) {
		budget_.take(slots_);
		values.resize(slots_);
	}
	CountedState(StateBudget &budget, const EntityState &copyOf) : budget_(budget), slots_(copyOf.size()) {
		budget_.take(slots_);
		values = copyOf;
	}
	~CountedState() { budget_.give(slots_); }
	CountedState(const CountedState &) = delete;
	CountedState &operator=(const CountedState &) = delete;

	EntityState values;

private:
	StateBudget &budget_;
	size_t slots_;
};

// The state new entities start from: per class (the "instancebaseline"
// string table) or per entity (kept from an earlier snapshot)
struct Baseline {
	Baseline(StateBudget &budget, int classIndex, const EntityState &state) : classIndex(classIndex), state(budget, state) {}
	Baseline(StateBudget &budget, int classIndex, size_t slots) : classIndex(classIndex), state(budget, slots) {}
	int classIndex;
	CountedState state;
};

struct LiveEntity {
	LiveEntity(StateBudget &budget, int classIndex, int serial, const EntityState &base) : classIndex(classIndex), serial(serial), state(budget, base) {}
	int classIndex;
	int serial;
	CountedState state;
	Track *track = nullptr;   // null: an entity of a class nothing is recorded for
	bool inSnapshot = true;   // false: out of the recording player's view, its last state kept
	bool removed = false;
	int originCopy = -1;      // which of the class's origins its updates carry (-1: none seen yet)
	size_t liveSlot = 0;      // where it is in the decoder's list of live entities
};

class EntityDecoder final : public NetHandler {
public:
	EntityDecoder(ByteSpan dataTables, const ParseOptions &options) : dataTables_(ParseDataTables(dataTables)) {
		fields_.reserve(dataTables_.classes.size());
		for (const ServerClass &cls : dataTables_.classes) fields_.push_back(FieldsFor(cls, options));
		instanceBaselines_.resize(dataTables_.classes.size());
		live_.resize(kMaxEntities);
		replaced_.resize(kMaxEntities);
		entityBaselines_[0].resize(kMaxEntities);
		entityBaselines_[1].resize(kMaxEntities);
	}

	void run(const std::vector<NetRecord> &records) {
		for (const NetRecord &record : records) {
			tick_ = record.tick;
			try {
				handle(record);
			} catch (const DecodeError &e) {
				// (the frame a cut-short demo ends in is expected not to read through)
				if (record.cutShort) continue;
				errors_.push_back(std::string(record.kind == NetRecord::PACKET ? "packet" : "string tables") + " at tick " + std::to_string(record.tick) + ": " +
					e.what());
				// One failed frame already means no entities are given (later
				// ones build on it); a few more are read for the warnings.
				if (errors_.size() >= kMaxFailedFrames) return;
			}
		}
	}

	void finish(Demo &demo);

	// ---- NetHandler ----

	void onTableChange(StringTable &table) override {
		tableByName_[table.name] = &table;
		if (table.name == "modelprecache") modelPrecache_ = &table;
		// class baselines are decoded on first use and dropped when the table changes
		if (table.name == "instancebaseline") {
			for (const int classIndex : decodedBaselines_) instanceBaselines_[size_t(classIndex)].reset();
			decodedBaselines_.clear();
		}
	}

	void onPacketEntities(const PacketEntitiesHeader &header, BitReader data) override {
		try {
			applyPacketEntities(header, data);
		} catch (...) {
			endSnapshot();
			throw;
		}
		endSnapshot();
	}

	void onUserMessage(int type, BitReader data) override {
		if (type != kMsgPortalFxSurface) return;
		try {
			readPortalShot(data);
		} catch (const DecodeError &) {
			// a short message: ignore it
		}
	}

private:
	void handle(const NetRecord &record) {
		if (record.kind == NetRecord::PACKET) {
			WalkPacket(record.data, tables_, dataTables_.classBits, *this);
			return;
		}
		std::vector<StringTable *> byName;
		for (size_t i = 0; i < tables_.count(); i++) byName.push_back(tables_.at(i));
		std::vector<std::unique_ptr<StringTable>> created;
		ReadStringTableSnapshot(record.data, tables_, byName, created);
		for (auto &table : created) tables_.adopt(std::move(table));
		// (in creation order, so that of two tables with one name the later is the one found by name)
		for (StringTable *table : byName) onTableChange(*table);
	}

	const StringTable *tableNamed(const std::string &name) const {
		const auto it = tableByName_.find(name);
		return it == tableByName_.end() ? nullptr : it->second;
	}

	std::string modelName(double index) const {
		const StringTable *models = modelPrecache_;
		if (!models || !(index >= 0) || index >= double(models->entries.size())) return std::string();
		const size_t entry = size_t(index);
		return double(entry) == index ? models->entries[entry].name : std::string();
	}

	// PortalFX_Surface: a portal shot reaching a surface (the portal, its
	// shooter, team, which portal, what became of it, where, facing where)
	void readPortalShot(BitReader &data) {
		const auto coord = [&data]() -> double {
			const bool hasInt = data.bit();
			const bool hasFrac = data.bit();
			if (!hasInt && !hasFrac) return 0;
			const bool negative = data.bit();
			const double whole = hasInt ? double(data.u(14)) + 1 : 0;
			const double frac = hasFrac ? double(data.u(5)) : 0;
			const double v = whole + frac / 32;
			return negative ? -v : v;
		};
		const auto vec3Coord = [&data, &coord]() -> Vec3 {
			const bool hasX = data.bit();
			const bool hasY = data.bit();
			const bool hasZ = data.bit();
			Vec3 v;
			if (hasX) v.x = float(coord());
			if (hasY) v.y = float(coord());
			if (hasZ) v.z = float(coord());
			return v;
		};
		PortalShot shot;
		shot.tick = tick_;
		data.u(16);  // the portal entity
		shot.shooter = int(data.u(16));
		shot.team = int(data.u(8));
		shot.portalNum = int(data.u(8));
		shot.result = int(data.u(8));
		shot.origin = vec3Coord();
		shot.angles = vec3Coord();
		portalShots_.push_back(shot);
	}

	// ---- baselines ----

	const EntityState &instanceBaseline(int classIndex) {
		std::shared_ptr<const Baseline> &cached = instanceBaselines_[size_t(classIndex)];
		if (cached) return cached->state.values;

		const ServerClass &cls = dataTables_.classes[size_t(classIndex)];
		auto baseline = std::make_shared<Baseline>(stateBudget_, classIndex, cls.props.size());
		if (const StringTable *table = tableNamed("instancebaseline")) {
			// the entry named after the class's number
			const std::string wanted = std::to_string(cls.id);
			const StringTableEntry *found = nullptr;
			for (const StringTableEntry &entry : table->entries) {
				if (++baselineLookups_ > kMaxBaselineLookups) throw DecodeError("the class baselines change more than a game's do");
				if (!entry.exists) throw DecodeError("the class baseline table has a gap");
				if (entry.name == wanted) {
					found = &entry;
					break;
				}
			}
			if (found && found->hasData) {
				try {
					BitReader br(found->data.data(), found->data.size());
					ReadEntityProps(br, cls.props, baseline->state.values, baselineChanged_);
				} catch (const DecodeError &e) {
					// (what was read stays, as in the reference; seen in real demos of
					// game versions where two classes share a name)
					if (baselineErrors_.size() < kMaxBaselineWarnings) baselineErrors_.push_back("baseline for " + cls.name + ": " + e.what());
				}
			}
		}
		cached = std::move(baseline);
		decodedBaselines_.push_back(classIndex);
		return cached->state.values;
	}

	// "The other baseline becomes a copy of this one": the two only differ at
	// the indices written since the last copy.
	void copyEntityBaselines(int from) {
		for (const int index : baselineDiffers_) entityBaselines_[1 - from][size_t(index)] = entityBaselines_[from][size_t(index)];
		baselineDiffers_.clear();
	}

	// ---- svc_PacketEntities ----

	void applyPacketEntities(const PacketEntitiesHeader &h, BitReader &br) {
		if (!h.isDelta) {
			// A full snapshot replaces everything.  The entities it holds are
			// made anew; one that was there before (same class and serial)
			// carries on in the same track.
			for (const int index : liveIndices_) leave(index, false);
			for (const int index : liveIndices_) replaced_[size_t(index)] = std::move(live_[size_t(index)]);
			replacedIndices_.swap(liveIndices_);
			liveIndices_.clear();
			livePlayers_.clear();
		}
		if (h.updateBaseline) copyEntityBaselines(h.baseline);

		int64_t index = -1;
		for (int n = 0; n < h.updatedEntries; n++) {
			index += 1 + int64_t(ReadUBitInt(br));
			const uint32_t updateType = br.u(2);
			LiveEntity *e = index < kMaxEntities ? live_[size_t(index)].get() : nullptr;
			if (updateType == 0) {
				// changed props of an entity in the snapshot
				if (!e) throw DecodeError("update for entity " + std::to_string(index) + ", which doesn't exist");
				readProps(br, *e, true);
				record(*e, false);
			} else if (updateType == 2) {
				// enters the snapshot: new, or back in view
				const int classIndex = int(br.u(dataTables_.classBits));
				const int serial = int(br.u(kSerialBits));
				if (size_t(classIndex) >= dataTables_.classes.size()) {
					throw DecodeError("entity " + std::to_string(index) + " has unknown class " + std::to_string(classIndex));
				}
				if (index >= kMaxEntities) throw DecodeError("entity index " + std::to_string(index) + " is out of range");
				if (!e || e->classIndex != classIndex || e->serial != serial) {
					if (e) leave(int(index), true);
					e = create(int(index), classIndex, serial, h.baseline);
				}
				e->inSnapshot = true;
				e->removed = false;
				readProps(br, *e, false);
				if (h.updateBaseline) {
					entityBaselines_[1 - h.baseline][size_t(index)] = std::make_shared<Baseline>(stateBudget_, classIndex, e->state.values);
					baselineDiffers_.push_back(int(index));
				}
				record(*e, false);
			} else if (e) {
				// 1: leaves the recording player's view, 3: deleted
				leave(int(index), updateType == 3);
			}
		}
		if (h.isDelta) {
			// explicit deletions
			while (br.remaining() > 0 && br.bit()) {
				const int deleted = int(br.u(kEdictBits));
				if (live_[size_t(deleted)]) leave(deleted, true);
			}
		}
	}

	LiveEntity *create(int index, int classIndex, int serial, int baselineSet) {
		const std::shared_ptr<const Baseline> &kept = entityBaselines_[baselineSet][size_t(index)];
		const EntityState &base = kept && kept->classIndex == classIndex ? kept->state.values : instanceBaseline(classIndex);
		auto entity = std::make_unique<LiveEntity>(stateBudget_, classIndex, serial, base);

		const TrackKind kind = fields_[size_t(classIndex)].kind;
		if (kind != TrackKind::NONE) {
			const LiveEntity *before = replaced_[size_t(index)].get();
			if (before && before->classIndex == classIndex && before->serial == serial) {
				entity->track = before->track;
			} else {
				if (tracks_.size() >= kMaxTracks) throw DecodeError("the demo creates more entities than a game can");
				tracks_.push_back(std::make_unique<Track>());
				entity->track = tracks_.back().get();
				entity->track->kind = kind;
				entity->track->entIndex = index;
				entity->track->classIndex = classIndex;
			}
			if (kind == TrackKind::PLAYER) livePlayers_.push_back(index);
		}
		entity->liveSlot = liveIndices_.size();
		liveIndices_.push_back(index);
		live_[size_t(index)] = std::move(entity);
		return live_[size_t(index)].get();
	}

	// `isDelta`: the changes of an entity already in the snapshot, not the
	// full state of one entering it
	void readProps(BitReader &br, LiveEntity &e, bool isDelta) {
		ReadEntityProps(br, dataTables_.classes[size_t(e.classIndex)].props, e.state.values, changed_);
		if (!e.track) return;
		const ClassFields &f = fields_[size_t(e.classIndex)];
		const auto wasChanged = [this](int prop) { return prop >= 0 && std::find(changed_.begin(), changed_.end(), prop) != changed_.end(); };
		if (f.origins.size() >= 2) {
			for (size_t i = 0; i < f.origins.size(); i++) {
				if (wasChanged(f.origins[i].origin) || wasChanged(f.origins[i].z)) {
					e.originCopy = int(i);
					break;
				}
			}
		}
		// Owner-only props reach other clients too when the player enters
		// their snapshot (the engine then sends every prop), but changes to
		// them only go to the owner: the recording player's own entity is the
		// one that keeps getting them (its tick base, every tick).
		if (isDelta && !e.track->ownerDataSeen && !f.ownerOnly.empty()) {
			for (const int prop : changed_) {
				if (f.ownerOnly[size_t(prop)]) {
					e.track->ownerDataSeen = true;
					break;
				}
			}
		}
	}

	void leave(int index, bool remove) {
		LiveEntity &e = *live_[size_t(index)];
		e.inSnapshot = false;
		e.removed = remove;
		record(e, true);
		if (!remove) return;
		if (e.track && e.track->kind == TrackKind::PLAYER) {
			livePlayers_.erase(std::remove(livePlayers_.begin(), livePlayers_.end(), index), livePlayers_.end());
		}
		// (out of the list of live entities: the last one takes its place)
		const int moved = liveIndices_.back();
		liveIndices_[e.liveSlot] = moved;
		live_[size_t(moved)]->liveSlot = e.liveSlot;
		liveIndices_.pop_back();
		live_[size_t(index)].reset();
	}

	// After each svc_PacketEntities, read through or not
	void endSnapshot() {
		// (what a full snapshot didn't bring back is gone)
		for (const int index : replacedIndices_) replaced_[size_t(index)].reset();
		replacedIndices_.clear();
		// one key for every player the snapshot holds
		for (const int index : livePlayers_) {
			Track &track = *live_[size_t(index)]->track;
			if (track.hasCurrent && track.current.inSnapshot) writePlayerKey(track);
		}
	}

	// ---- recording ----

	void originOf(const ClassFields &f, const LiveEntity &e, double origin[3]) const {
		const EntityState &s = e.state.values;
		const OriginCopy *copy = f.origins.empty() ? nullptr : &f.origins[size_t(e.originCopy >= 0 ? e.originCopy : 0)];
		const PropValue *o = copy ? VectorAt(s, copy->origin) : nullptr;
		const double x = o ? o->v[0] : 0;
		const double y = o ? o->v[1] : 0;
		const double z = o && o->count > 2 ? o->v[2] : NumberAt(s, copy ? copy->z : -1);
		if (copy && copy->cellRelative && f.cellX >= 0) {
			// relative to a cell of a grid over the map (m_cellbits: the cells' size)
			const double cellBits = NumberAt(s, f.cellBits, 5);
			const unsigned wholeBits = cellBits >= 0 && cellBits < 31 ? unsigned(cellBits) : 0;
			const double cellWidth = double(wholeBits) == cellBits ? double(1u << wholeBits) : std::pow(2.0, cellBits);
			origin[0] = NumberAt(s, f.cellX, 512) * cellWidth - kMaxCoord + x;
			origin[1] = NumberAt(s, f.cellY, 512) * cellWidth - kMaxCoord + y;
			origin[2] = NumberAt(s, f.cellZ, 512) * cellWidth - kMaxCoord + z;
		} else {
			origin[0] = x;
			origin[1] = y;
			origin[2] = z;
		}
	}

	// Notes the entity's state after an update (`leaving`: it left the
	// snapshot or was deleted instead).
	void record(LiveEntity &e, bool leaving) {
		if (!e.track) return;
		Track &track = *e.track;
		const ClassFields &f = fields_[size_t(e.classIndex)];
		const EntityState &s = e.state.values;

		if (track.model.empty() && f.model >= 0) track.model = modelName(NumberAt(s, f.model));
		double origin[3];
		originOf(f, e, origin);

		if (track.kind == TrackKind::PLAYER) {
			recordPlayer(track, f, e, origin, leaving);
			return;
		}

		MotionKey key;
		key.tick = tick_;
		std::copy(origin, origin + 3, key.origin);
		if (const PropValue *angles = VectorAt(s, f.angles)) std::copy(angles->v, angles->v + 3, key.angles);
		key.parent = f.moveParent >= 0 ? HandleIndex(NumberAt(s, f.moveParent, -1)) : -1;
		key.inSnapshot = e.inSnapshot;
		key.removed = e.removed;
		if (track.kind == TrackKind::PORTAL) {
			key.active = NumberAt(s, f.activated) != 0;
			key.linkageGroup = ToInt(NumberAt(s, f.linkageGroup));
			key.team = ToInt(NumberAt(s, f.team));
			key.firedBy = HandleIndex(NumberAt(s, f.firedBy, -1));
			key.linkedPortal = HandleIndex(NumberAt(s, f.linkedPortal, -1));
			if (f.isPortal2 >= 0) track.isPortal2 = NumberAt(s, f.isPortal2) != 0;
		} else {
			key.noDraw = (ToInt32(NumberAt(s, f.effects)) & kEffectNoDraw) != 0;
		}
		addMotionKey(track.keys, key);
	}

	// A key only when something changed; of two on one tick the later stays.
	void addMotionKey(std::vector<MotionKey> &keys, const MotionKey &key) {
		if (!keys.empty() && keys.back().sameState(key)) return;
		if (!keys.empty() && keys.back().tick == key.tick) {
			keys.pop_back();
			keyCount_--;
			if (!keys.empty() && keys.back().sameState(key)) return;
		}
		countKey();
		keys.push_back(key);
	}

	void recordPlayer(Track &track, const ClassFields &f, const LiveEntity &e, const double origin[3], bool leaving) {
		const EntityState &s = e.state.values;
		PlayerKey key;
		key.tick = tick_;
		key.origin = ToVec3(origin);
		key.eyePitch = ToFloat(NumberAt(s, f.eyePitch));
		key.eyeYaw = ToFloat(NumberAt(s, f.eyeYaw));
		// (the owner's view offset and velocity; finishPlayer drops them for other players)
		const PropValue *ownerOffset = VectorAt(s, f.viewOffsetOwner);
		key.viewOffsetZ = ownerOffset && ownerOffset->count > 2 ? ToFloat(ownerOffset->v[2]) : 0;
		key.flags = ToInt32(NumberAt(s, f.playerFlags));
		key.team = ToInt(NumberAt(s, f.team));
		key.heldEntity = HandleIndex(NumberAt(s, f.heldObject, -1));
		key.hasVelocity = f.velocity[0] >= 0 && f.velocity[1] >= 0 && f.velocity[2] >= 0;
		if (key.hasVelocity) {
			key.velocity.x = ToFloat(NumberAt(s, f.velocity[0]));
			key.velocity.y = ToFloat(NumberAt(s, f.velocity[1]));
			key.velocity.z = ToFloat(NumberAt(s, f.velocity[2]));
		}
		key.inSnapshot = e.inSnapshot;

		// Out of the snapshot: one key to say so (its state as last sent),
		// then none until it is back.
		const bool wasInSnapshot = !track.hasCurrent || track.current.inSnapshot;
		const bool nowRemoved = e.removed && !track.currentRemoved;
		track.current = key;
		track.currentOthersViewOffsetZ = ToFloat(NumberAt(s, f.viewOffsetOthers));
		track.hasCurrent = true;
		track.currentRemoved = e.removed;
		if (!key.inSnapshot && (wasInSnapshot || nowRemoved || !leaving)) writePlayerKey(track);
	}

	void writePlayerKey(Track &track) {
		PlayerKey key = track.current;
		key.tick = tick_;
		if (!track.playerKeys.empty() && track.playerKeys.back().tick == key.tick) {
			track.playerKeys.back() = key;
			track.othersViewOffsetZ.back() = track.currentOthersViewOffsetZ;
			return;
		}
		countKey();
		track.playerKeys.push_back(key);
		track.othersViewOffsetZ.push_back(track.currentOthersViewOffsetZ);
	}

	// The view offset and the velocity are sent in the owner's local data:
	// current for the recording player's own entity, stale (as of when the
	// player last entered the snapshot) for anyone else, who has the view
	// offset's z in a copy of its own and no velocity at all.
	void finishPlayer(Track &track) const {
		const ClassFields &f = fields_[size_t(track.classIndex)];
		if (track.ownerDataSeen) return;
		const bool hasOthersCopy = f.viewOffsetOthers >= 0;
		for (size_t i = 0; i < track.playerKeys.size(); i++) {
			PlayerKey &key = track.playerKeys[i];
			if (hasOthersCopy) key.viewOffsetZ = track.othersViewOffsetZ[i];
			key.hasVelocity = false;
			key.velocity = Vec3();
		}
	}

	void countKey() {
		if (++keyCount_ > kMaxKeys) throw DecodeError("the demo holds more entity movement than a game can record");
	}

	std::unordered_map<int, std::string> playerNames() const;

	// (declared first: the states below give their slots back when destroyed)
	StateBudget stateBudget_;
	DataTables dataTables_;
	std::vector<ClassFields> fields_;
	StringTables tables_;
	std::unordered_map<std::string, StringTable *> tableByName_;  // the tables whose entries have been read
	const StringTable *modelPrecache_ = nullptr;  // tableByName_["modelprecache"], looked up on every update
	std::vector<std::shared_ptr<const Baseline>> instanceBaselines_;  // per class, null until used
	std::vector<int> decodedBaselines_;  // the classes with one
	size_t baselineLookups_ = 0;
	std::vector<std::shared_ptr<const Baseline>> entityBaselines_[2];  // per entity index
	std::vector<int> baselineDiffers_;  // the entity indices at which the two sets may differ
	std::vector<std::unique_ptr<LiveEntity>> live_;      // per entity index
	std::vector<int> liveIndices_;                       // the indices in use, in no order
	std::vector<std::unique_ptr<LiveEntity>> replaced_;  // during a full snapshot: what was there before it
	std::vector<int> replacedIndices_;
	std::vector<int> livePlayers_;  // entity indices of the players in live_
	std::vector<std::unique_ptr<Track>> tracks_;  // in creation order
	std::vector<PortalShot> portalShots_;
	std::vector<int> changed_, baselineChanged_;  // scratch
	std::vector<std::string> errors_, baselineErrors_;
	size_t keyCount_ = 0;
	int tick_ = 0;
};

// Each client's player_info_t in "userinfo" (its slot is one less than its
// entity index): an 8-byte XUID, then the name, 32 bytes.
std::unordered_map<int, std::string> EntityDecoder::playerNames() const {
	std::unordered_map<int, std::string> names;
	const StringTable *table = tableNamed("userinfo");
	if (!table) return names;
	for (size_t i = 0; i < table->entries.size(); i++) {
		const StringTableEntry &entry = table->entries[i];
		if (!entry.exists || !entry.hasData || entry.data.size() < 40) continue;
		const char *field = reinterpret_cast<const char *>(entry.data.data()) + 8;
		size_t length = 0;
		while (length < 32 && field[length] != 0) length++;
		std::string name = TrimJsWhitespace(std::string(field, length));
		if (!name.empty()) names[int(i) + 1] = std::move(name);
	}
	return names;
}

// The key in effect at a tick, or null before the first
const PlayerKey *KeyAt(const PlayerTrack &track, int tick) {
	const auto after = std::upper_bound(track.keys.begin(), track.keys.end(), tick, [](int t, const PlayerKey &k) { return t < k.tick; });
	return after == track.keys.begin() ? nullptr : &*(after - 1);
}

// In co-op either player can be the one recording: the one whose entity
// follows the recorded view.
int FindRecorder(const std::vector<PlayerTrack> &players, const std::vector<View> &views) {
	if (players.empty()) return 0;
	const PlayerTrack *recorder = &players[0];
	if (players.size() > 1 && !views.empty()) {
		const size_t step = std::max<size_t>(1, views.size() / 50);
		double best = std::numeric_limits<double>::infinity();
		for (const PlayerTrack &player : players) {
			double sum = 0;
			size_t count = 0;
			for (size_t i = 0; i < views.size(); i += step) {
				const View &view = views[i];
				const PlayerKey *key = KeyAt(player, view.tick);
				if (!key) continue;
				const double dx = double(key->origin.x) - view.origin.x, dy = double(key->origin.y) - view.origin.y, dz = double(key->origin.z) - view.origin.z;
				sum += std::sqrt(dx * dx + dy * dy + dz * dz);
				count++;
			}
			const double score = count ? sum / double(count) : std::numeric_limits<double>::infinity();
			if (score < best) {
				best = score;
				recorder = &player;
			}
		}
	}
	return recorder->entIndex;
}

void EntityDecoder::finish(Demo &demo) {
	for (const std::string &error : baselineErrors_) demo.warnings.push_back("Entities: " + error);

	if (!errors_.empty()) {
		demo.entitiesDecoded = false;
		demo.warnings.push_back("The network data couldn't be decoded; no entities are given.");
		for (const std::string &error : errors_) demo.warnings.push_back("Entities: " + error);
		return;
	}

	const std::unordered_map<int, std::string> names = playerNames();
	for (const auto &owned : tracks_) {
		Track &track = *owned;
		if (track.kind == TrackKind::PLAYER) {
			if (track.playerKeys.empty()) continue;
			finishPlayer(track);
			PlayerTrack out;
			out.entIndex = track.entIndex;
			const auto name = names.find(track.entIndex);
			if (name != names.end()) out.name = name->second;
			out.model = std::move(track.model);
			out.keys = std::move(track.playerKeys);
			demo.players.push_back(std::move(out));
			continue;
		}
		if (track.keys.empty()) continue;
		if (track.kind == TrackKind::PORTAL) {
			PortalTrack out;
			out.entIndex = track.entIndex;
			out.isPortal2 = track.isPortal2;
			out.keys.reserve(track.keys.size());
			for (const MotionKey &k : track.keys) {
				PortalKey key;
				key.tick = k.tick;
				key.active = k.active;
				key.origin = ToVec3(k.origin);
				key.angles = ToVec3(k.angles);
				key.linkageGroup = k.linkageGroup;
				key.team = k.team;
				key.firedBy = k.firedBy;
				key.linkedPortal = k.linkedPortal;
				key.inSnapshot = k.inSnapshot;
				key.removed = k.removed;
				out.keys.push_back(key);
			}
			demo.portals.push_back(std::move(out));
			continue;
		}
		// movers: a model, and it moved or turned at least once
		if (track.model.empty()) continue;
		const MotionKey &first = track.keys.front();
		const bool moved = std::any_of(track.keys.begin(), track.keys.end(), [&first](const MotionKey &k) { return !k.sameMotion(first); });
		if (!moved) continue;
		MoverTrack out;
		out.entIndex = track.entIndex;
		out.className = dataTables_.classes[size_t(track.classIndex)].name;
		out.model = std::move(track.model);
		out.keys.reserve(track.keys.size());
		for (const MotionKey &k : track.keys) {
			MoverKey key;
			key.tick = k.tick;
			key.origin = ToVec3(k.origin);
			key.angles = ToVec3(k.angles);
			key.parent = k.parent;
			key.inSnapshot = k.inSnapshot;
			key.removed = k.removed;
			key.noDraw = k.noDraw;
			out.keys.push_back(key);
		}
		demo.movers.push_back(std::move(out));
	}
	demo.portalShots = std::move(portalShots_);

	demo.recorderEntIndex = FindRecorder(demo.players, demo.views);
	for (PlayerTrack &player : demo.players) player.isRecorder = player.entIndex == demo.recorderEntIndex;
	demo.entitiesDecoded = true;
}

}  // namespace

void DecodeEntities(const std::vector<NetRecord> &records, ByteSpan dataTables, const ParseOptions &options, Demo &demo) {
	EntityDecoder decoder(dataTables, options);
	decoder.run(records);
	decoder.finish(demo);
}

}  // namespace detail
}  // namespace p2demo
