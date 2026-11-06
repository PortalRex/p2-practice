// Send tables and server classes (the dem_datatables frame), and the
// flattened property list each class's entities are decoded with.
// Mirrors sar-web src/net/datatables.ts.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "readers.h"

namespace p2demo {
namespace detail {

enum PropType : int {
	PROP_INT = 0,
	PROP_FLOAT = 1,
	PROP_VEC3 = 2,
	PROP_VEC2 = 3,
	PROP_STRING = 4,
	PROP_ARRAY = 5,
	PROP_DATATABLE = 6,
};

// SPROP_* flags
enum PropFlag : uint32_t {
	SPROP_UNSIGNED = 1u << 0,
	SPROP_COORD = 1u << 1,
	SPROP_NOSCALE = 1u << 2,
	SPROP_NORMAL = 1u << 5,
	SPROP_EXCLUDE = 1u << 6,
	SPROP_INSIDEARRAY = 1u << 8,
	SPROP_COLLAPSIBLE = 1u << 11,
	SPROP_COORD_MP = 1u << 12,
	SPROP_COORD_MP_LOWPRECISION = 1u << 13,
	SPROP_COORD_MP_INTEGRAL = 1u << 14,
	SPROP_CELL_COORD = 1u << 15,
	SPROP_CELL_COORD_LOWPRECISION = 1u << 16,
	SPROP_CELL_COORD_INTEGRAL = 1u << 17,
	SPROP_CHANGES_OFTEN = 1u << 18,
};

// How one value is read off the wire
struct PropCoding {
	int type = 0;
	uint32_t flags = 0;
	int bits = 0;
	double low = 0, high = 0;
	int elements = 0;  // arrays: the most elements it can hold
};

struct FlatProp {
	std::string name;  // with the data tables it sits in: "localdata.m_vecVelocity[0]"
	PropCoding coding;
	PropCoding element;  // arrays: how each element is read
	bool hasElement = false;
	int priority = 0;
};

struct ServerClass {
	int id = 0;
	std::string name;       // CPortal_Player
	std::string tableName;  // DT_Portal_Player
	std::vector<FlatProp> props;

	// Index of the prop with this name, -1 if the class has none.  Of two
	// props with one name the later wins, as in the TypeScript index.
	int find(std::string_view propName) const;
	// The first of the names the class has
	int findFirst(std::initializer_list<std::string_view> propNames) const;
};

struct DataTables {
	std::vector<ServerClass> classes;  // entities name their class by position in this list
	int classBits = 0;                 // width of a class number on the wire
};

// highest set bit of v (-1 for 0)
int HighestBit(uint32_t v);

// Throws DecodeError when the blob is cut short or describes tables no game
// would send (nested too deep, too many props).
DataTables ParseDataTables(ByteSpan blob);

}  // namespace detail
}  // namespace p2demo
