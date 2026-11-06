#include "datatables.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace p2demo {
namespace detail {

namespace {

// Limits no real game reaches; they keep a hostile file from costing unbounded
// time or memory.  (The engine itself refuses more than 4096 props a class.)
constexpr size_t kMaxPropsPerClass = 4096;
constexpr size_t kMaxPropsAllClasses = 500000;
constexpr size_t kMaxNameBytesAllClasses = 32u << 20;
constexpr int kMaxTableDepth = 64;
constexpr uint64_t kMaxTableVisits = 4000000;

struct SendProp {
	int type = 0;
	std::string name;
	uint32_t flags = 0;
	int priority = 0;
	PropCoding coding;
	std::string tableName;  // data table props: the table; excludes: the table the excluded prop is in
};

struct SendTable {
	std::string name;
	std::vector<SendProp> props;
};

struct Limits {
	uint64_t visits = 0;
	size_t props = 0;
	size_t nameBytes = 0;

	void visit(size_t count) {
		visits += count;
		if (visits > kMaxTableVisits) throw DecodeError("the data tables are too tangled to flatten");
	}
};

class SendTables {
public:
	void add(SendTable &&table) {
		// a table sent twice: the later one counts
		byName_[table.name] = tables_.size();
		tables_.push_back(std::move(table));
	}
	const SendTable *find(const std::string &name) const {
		const auto it = byName_.find(name);
		return it == byName_.end() ? nullptr : &tables_[it->second];
	}

private:
	std::vector<SendTable> tables_;
	std::unordered_map<std::string, size_t> byName_;
};

// SendTable_Flatten: a class's props in wire order.  Excluded props are
// dropped, data table props are replaced by their table's props (collapsible
// ones in place, the others after the props around them), and the result is
// sorted by priority.
class Flattener {
public:
	Flattener(const SendTables &tables, Limits &limits) : tables_(tables), limits_(limits) {}

	std::vector<FlatProp> flatten(const SendTable &root) {
		excludes_.clear();
		out_.clear();
		gatherExcludes(root, 0);
		gather(root, std::string(), 0);
		sortByPriority();
		return std::move(out_);
	}

private:
	static std::string excludeKey(const std::string &tableName, const std::string &propName) {
		std::string key;
		key.reserve(tableName.size() + 1 + propName.size());
		key.append(tableName).push_back('\0');
		key.append(propName);
		return key;
	}

	void gatherExcludes(const SendTable &table, int depth) {
		if (depth > kMaxTableDepth) throw DecodeError("the data tables nest too deeply");
		limits_.visit(table.props.size());
		for (const SendProp &p : table.props) {
			if (p.type == PROP_DATATABLE) {
				if (const SendTable *sub = tables_.find(p.tableName)) gatherExcludes(*sub, depth + 1);
			} else if (p.flags & SPROP_EXCLUDE) {
				excludes_.insert(excludeKey(p.tableName, p.name));
			}
		}
	}

	void iterate(const SendTable &table, std::vector<FlatProp> &into, const std::string &prefix, int depth) {
		if (depth > kMaxTableDepth) throw DecodeError("the data tables nest too deeply");
		limits_.visit(table.props.size());
		for (size_t i = 0; i < table.props.size(); i++) {
			const SendProp &p = table.props[i];
			if (p.flags & (SPROP_EXCLUDE | SPROP_INSIDEARRAY)) continue;
			if (!excludes_.empty() && excludes_.count(excludeKey(table.name, p.name))) continue;
			if (p.type == PROP_DATATABLE) {
				const SendTable *sub = tables_.find(p.tableName);
				if (!sub) continue;
				if (p.flags & SPROP_COLLAPSIBLE) iterate(*sub, into, prefix, depth + 1);
				else gather(*sub, p.name.empty() ? prefix : prefix + p.name + ".", depth + 1);
				continue;
			}
			FlatProp flat;
			flat.name = prefix + p.name;
			flat.coding = p.coding;
			flat.priority = p.priority;
			// an array's element is described by the prop before it
			if (p.type == PROP_ARRAY && i > 0) {
				flat.element = table.props[i - 1].coding;
				flat.hasElement = true;
			}
			limits_.props++;
			limits_.nameBytes += flat.name.size();
			if (into.size() + out_.size() >= kMaxPropsPerClass || limits_.props > kMaxPropsAllClasses || limits_.nameBytes > kMaxNameBytesAllClasses) {
				throw DecodeError("the data tables describe more props than a game can send");
			}
			into.push_back(std::move(flat));
		}
	}

	void gather(const SendTable &table, const std::string &prefix, int depth) {
		std::vector<FlatProp> into;
		iterate(table, into, prefix, depth);
		for (FlatProp &f : into) out_.push_back(std::move(f));
	}

	// The engine's own passes: for each priority in ascending order, swap the
	// props with it to the front of what is left.  "Changes often" props count
	// as priority 64.
	void sortByPriority() {
		std::vector<int> priorities;
		priorities.push_back(64);
		for (const FlatProp &f : out_) priorities.push_back(f.priority);
		std::sort(priorities.begin(), priorities.end());
		priorities.erase(std::unique(priorities.begin(), priorities.end()), priorities.end());

		size_t start = 0;
		for (const int priority : priorities) {
			for (size_t i = start; i < out_.size(); i++) {
				const FlatProp &f = out_[i];
				if (f.priority == priority || ((f.coding.flags & SPROP_CHANGES_OFTEN) && priority == 64)) {
					if (i != start) std::swap(out_[start], out_[i]);
					start++;
				}
			}
		}
	}

	const SendTables &tables_;
	Limits &limits_;
	std::unordered_set<std::string> excludes_;
	std::vector<FlatProp> out_;
};

SendTable ReadSendTable(BitReader &br) {
	SendTable table;
	br.bit();  // needs decoder
	table.name = br.cstring();
	const int propCount = int(br.u(10));
	for (int i = 0; i < propCount; i++) {
		SendProp p;
		p.type = int(br.u(5));
		p.name = br.cstring();
		p.flags = br.u(19);
		p.priority = int(br.u(8));
		p.coding.type = p.type;
		p.coding.flags = p.flags;
		if (p.type == PROP_DATATABLE || (p.flags & SPROP_EXCLUDE)) {
			p.tableName = br.cstring();
		} else if (p.type == PROP_ARRAY) {
			p.coding.elements = int(br.u(10));
		} else {
			p.coding.low = double(br.f32());
			p.coding.high = double(br.f32());
			p.coding.bits = int(br.u(7));
		}
		table.props.push_back(std::move(p));
	}
	return table;
}

}  // namespace

int HighestBit(uint32_t v) {
	int i = -1;
	for (; v != 0; v >>= 1) i++;
	return i;
}

int ServerClass::find(std::string_view propName) const {
	for (size_t i = props.size(); i-- > 0;) {
		if (props[i].name == propName) return int(i);
	}
	return -1;
}

int ServerClass::findFirst(std::initializer_list<std::string_view> propNames) const {
	for (const std::string_view propName : propNames) {
		const int i = find(propName);
		if (i >= 0) return i;
	}
	return -1;
}

DataTables ParseDataTables(ByteSpan blob) {
	BitReader br(blob);
	SendTables tables;
	while (br.bit()) tables.add(ReadSendTable(br));

	DataTables result;
	const int classCount = int(br.u(16));
	for (int i = 0; i < classCount; i++) {
		ServerClass c;
		c.id = int(br.u(16));
		c.name = br.cstring();
		c.tableName = br.cstring();
		result.classes.push_back(std::move(c));
	}

	Limits limits;
	Flattener flattener(tables, limits);
	for (ServerClass &c : result.classes) {
		if (const SendTable *table = tables.find(c.tableName)) c.props = flattener.flatten(*table);
	}
	result.classBits = HighestBit(uint32_t(result.classes.size())) + 1;
	return result;
}

}  // namespace detail
}  // namespace p2demo
