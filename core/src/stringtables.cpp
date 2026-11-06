#include "stringtables.h"

#include <array>

#include "datatables.h"

namespace p2demo {
namespace detail {

namespace {

constexpr int kSubstringBits = 5;
constexpr int kMaxUserDataBits = 14;
constexpr size_t kHistorySize = 32;

// The engine has 32 tables of at most 65536 entries; these leave room and
// still bound what a hostile file can make the decoder allocate.
constexpr size_t kMaxTables = 64;
constexpr size_t kMaxEntriesAllTables = 1u << 20;

// An entry's data: whole bytes, then the odd bits in a last byte
std::vector<uint8_t> ReadBits(BitReader &br, uint64_t bits) {
	if (bits > br.remaining()) throw DecodeError("string table data runs past the end");
	std::vector<uint8_t> out(size_t((bits + 7) / 8));
	size_t i = 0;
	for (; bits >= 8; bits -= 8) out[i++] = uint8_t(br.u(8));
	if (bits) out[i] = uint8_t(br.u(int(bits)));
	return out;
}

}  // namespace

StringTable &StringTables::create(const std::string &name) {
	if (tables_.size() >= kMaxTables) throw DecodeError("too many string tables");
	tables_.push_back(std::make_unique<StringTable>());
	tables_.back()->name = name;
	return *tables_.back();
}

void StringTables::adopt(std::unique_ptr<StringTable> table) {
	if (tables_.size() >= kMaxTables) throw DecodeError("too many string tables");
	tables_.push_back(std::move(table));
}

StringTableEntry &StringTables::entryAt(StringTable &table, size_t index) {
	if (index >= table.entries.size()) {
		const size_t added = index + 1 - table.entries.size();
		if (added > kMaxEntriesAllTables - totalEntries_) throw DecodeError("the string tables are too large");
		totalEntries_ += added;
		table.entries.resize(index + 1);
	}
	StringTableEntry &entry = table.entries[index];
	entry.exists = true;
	return entry;
}

void StringTables::applyEntries(StringTable &table, BitReader &br, uint32_t count) {
	if (br.bit()) throw DecodeError("string table " + table.name + ": dictionary encoding isn't supported");

	// the last 32 names, oldest first: a name can be sent as a prefix of one
	// of them plus the rest
	std::array<std::string, kHistorySize> history;
	size_t historyStart = 0, historyCount = 0;

	const int indexBits = HighestBit(uint32_t(table.maxEntries));
	int64_t index = -1;
	for (uint32_t n = 0; n < count; n++) {
		index++;
		if (!br.bit()) index = br.u(indexBits);

		bool hasName = false;
		std::string name;
		if (br.bit()) {
			hasName = true;
			if (br.bit()) {
				const uint32_t from = br.u(kSubstringBits);
				const uint32_t length = br.u(kSubstringBits);
				if (from < historyCount) name = history[(historyStart + from) % kHistorySize].substr(0, length);
				name += br.cstring();
			} else {
				name = br.cstring();
			}
		}

		StringTableEntry &entry = entryAt(table, size_t(index));
		if (hasName) entry.name = std::move(name);

		if (historyCount == kHistorySize) {
			historyStart = (historyStart + 1) % kHistorySize;
			historyCount--;
		}
		history[(historyStart + historyCount) % kHistorySize] = entry.name;
		historyCount++;

		if (br.bit()) {
			const uint64_t bits = table.fixedSize ? uint64_t(table.userDataBits) : uint64_t(br.u(kMaxUserDataBits)) * 8;
			entry.data = ReadBits(br, bits);
			entry.hasData = true;
		}
	}
}

void ReadStringTableSnapshot(ByteSpan data, StringTables &tables, std::vector<StringTable *> &byName, std::vector<std::unique_ptr<StringTable>> &created) {
	BitReader br(data);
	const uint32_t tableCount = br.u(8);
	for (uint32_t i = 0; i < tableCount; i++) {
		const std::string name = br.cstring();
		StringTable *table = nullptr;
		for (StringTable *known : byName) {
			if (known->name == name) table = known;
		}
		if (!table) {
			created.push_back(std::make_unique<StringTable>());
			table = created.back().get();
			table->name = name;
			byName.push_back(table);
		}

		const uint32_t entryCount = br.u(16);
		for (uint32_t e = 0; e < entryCount; e++) {
			std::string entryName = br.cstring();
			StringTableEntry &entry = tables.entryAt(*table, e);
			entry.name = std::move(entryName);
			if (br.bit()) {
				const uint64_t bytes = br.u(16);
				entry.data = ReadBits(br, bytes * 8);
				entry.hasData = true;
			}
		}

		// the client-side entries: not needed
		if (br.bit()) {
			const uint32_t clientCount = br.u(16);
			for (uint32_t e = 0; e < clientCount; e++) {
				br.skipCString();
				if (br.bit()) {
					const uint64_t bytes = br.u(16);
					br.skipToEndAtMost(bytes * 8);
				}
			}
		}
	}
}

std::vector<uint8_t> LzssDecompress(ByteSpan src) {
	// (a read past the end counts as a zero byte, as in the reference)
	const auto at = [&src](size_t i) -> uint32_t { return i < src.size ? src.data[i] : 0; };

	std::vector<uint8_t> out;
	size_t p = 0;
	uint32_t command = 0;
	int commandBit = 0;
	for (;;) {
		if (commandBit == 0) command = at(p++);
		commandBit = (commandBit + 1) & 7;
		if (command & 1) {
			// a copy of 2..16 bytes from up to 4096 bytes back; a length of 1 ends the data
			const size_t position = (at(p) << 4) | (at(p + 1) >> 4);
			const size_t count = (at(p + 1) & 0x0f) + 1;
			p += 2;
			if (count == 1) break;
			for (size_t i = 0; i < count; i++) {
				// (a copy from before the start reads as zero)
				const bool inRange = out.size() >= position + 1;
				out.push_back(inRange ? out[out.size() - position - 1] : uint8_t(0));
			}
		} else {
			out.push_back(uint8_t(at(p++)));
		}
		command >>= 1;
		if (p > src.size) break;
	}
	return out;
}

}  // namespace detail
}  // namespace p2demo
