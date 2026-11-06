// Network string tables: the entries of svc_CreateStringTable and
// svc_UpdateStringTable, and the dem_stringtables snapshot.  The decoder
// needs "instancebaseline" (class baselines), "modelprecache" (model names)
// and "userinfo" (player names).  Mirrors sar-web src/net/stringtables.ts.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "readers.h"

namespace p2demo {
namespace detail {

struct StringTableEntry {
	bool exists = false;  // false: a gap (an index no message has filled yet)
	bool hasData = false;
	std::string name;
	std::vector<uint8_t> data;
};

struct StringTable {
	std::string name;
	int maxEntries = 0;
	bool fixedSize = false;  // every entry's data is userDataBits bits
	int userDataBits = 0;
	std::vector<StringTableEntry> entries;
};

// Every table of one demo.  Tables are created in order (updates name them
// by that order) and never destroyed; the caps keep a hostile file from
// growing them without bound.
class StringTables {
public:
	StringTable &create(const std::string &name);
	// Takes over a table read from a snapshot
	void adopt(std::unique_ptr<StringTable> table);
	// The table at this creation index, or null
	StringTable *at(size_t index) { return index < tables_.size() ? tables_[index].get() : nullptr; }
	size_t count() const { return tables_.size(); }

	// Applies an entry list (the data of a create or update message)
	void applyEntries(StringTable &table, BitReader &br, uint32_t count);

	// The entry at `index`, created (with the gaps before it) if missing
	StringTableEntry &entryAt(StringTable &table, size_t index);

private:
	std::vector<std::unique_ptr<StringTable>> tables_;
	size_t totalEntries_ = 0;
};

// dem_stringtables: every table's entries as they stood when recording
// began.  `byName` lists the known tables in creation order (of two with one
// name the later is the one written to); a table the snapshot names that is
// not among them is made, appended to `byName` and handed back in `created`.
// On a DecodeError the entries written so far stay written.
void ReadStringTableSnapshot(ByteSpan data, StringTables &tables, std::vector<StringTable *> &byName, std::vector<std::unique_ptr<StringTable>> &created);

// Valve's LZSS, as string table data is packed when it is large.  `src`
// starts after the 8-byte "LZSS" + size header.
std::vector<uint8_t> LzssDecompress(ByteSpan src);

}  // namespace detail
}  // namespace p2demo
