#include "ui/library.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#include "engine.h"
#include "maps.h"
#include "p2demo/demo.h"
#include "route.h"

namespace fs = std::filesystem;

namespace library {
namespace {

constexpr const char *kCacheHeader = "P2PracticeLibrary 1";

// What the background reader hands over: the demos, and the runs made of them (pointing into `demos`)
struct Shelf {
	std::vector<Demo> demos;
	std::vector<Run> runs;
};

std::vector<std::string> g_folders;
bool g_foldersLoaded = false;

std::mutex g_gate;
std::unique_ptr<Shelf> g_fresh;        // handed over by the reader, taken by Runs()
std::unique_ptr<Shelf> g_shelf = std::make_unique<Shelf>();
std::atomic<int> g_version{0};
int g_seenVersion = -1;
std::atomic<bool> g_busy{false}, g_stop{false};
std::atomic<int> g_done{0}, g_total{0};
std::thread g_reader;
bool g_started = false;

std::wstring Wide(const std::string &utf8) {
	int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
	std::wstring out(size > 0 ? size - 1 : 0, L'\0');
	if (size > 1) MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &out[0], size);
	return out;
}

std::string Utf8(const std::wstring &wide) {
	int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
	std::string out(size > 0 ? size - 1 : 0, '\0');
	if (size > 1) WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, &out[0], size, nullptr, nullptr);
	return out;
}

std::string Lower(std::string text) {
	for (char &c : text) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
	return text;
}

std::string g_dataFolder;  // (worked out on the game's thread: the reader never asks the game anything)
std::string FoldersFile() { return g_dataFolder + "\\folders.txt"; }
std::string CacheFile() { return g_dataFolder + "\\library.cache"; }

void LoadFolders() {
	if (g_foldersLoaded) return;
	g_foldersLoaded = true;
	g_dataFolder = DataFolder();
	std::ifstream in(Wide(FoldersFile()));
	std::string line;
	bool any = false;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (!line.empty()) g_folders.push_back(line);
		any = true;
	}
	if (any) return;
	// first time: the game's demos folder (with the boards' downloads under it)
	std::string demos = engine::GameDirectory() + "\\demos";
	std::error_code ec;
	fs::create_directories(Wide(DownloadFolder()), ec);
	g_folders.push_back(demos);
}

void SaveFolders() {
	std::error_code ec;
	fs::create_directories(Wide(g_dataFolder), ec);
	std::ofstream out(Wide(FoldersFile()), std::ios::trunc);
	for (const std::string &folder : g_folders) out << folder << "\n";
}

// ---- the cache: one demo per line, tab-separated ----
std::map<std::string, Demo> LoadCache() {
	std::map<std::string, Demo> cache;
	std::ifstream in(Wide(CacheFile()), std::ios::binary);
	std::string line;
	if (!std::getline(in, line) || line != kCacheHeader) return cache;
	while (std::getline(in, line)) {
		std::vector<std::string> f;
		std::stringstream fields(line);
		std::string field;
		while (std::getline(fields, field, '\t')) f.push_back(field);
		if (f.size() < 12) continue;
		Demo d;
		d.path = f[0];
		d.size = _atoi64(f[1].c_str());
		d.modified = _atoi64(f[2].c_str());
		d.ok = f[3] == "1";
		d.map = f[4];
		d.player = f[5];
		d.coop = f[6] == "1";
		d.host = f[7] == "1";
		d.hasTime = f[8] == "1";
		d.ticks = atoi(f[9].c_str());
		d.seconds = static_cast<float>(atof(f[10].c_str()));
		d.error = f[11];
		d.name = Utf8(fs::path(Wide(d.path)).stem().wstring());
		cache[Lower(d.path)] = d;
	}
	return cache;
}

void SaveCache(const std::vector<Demo> &demos) {
	std::error_code ec;
	fs::create_directories(Wide(g_dataFolder), ec);
	std::string temp = CacheFile() + ".new";
	{
		std::ofstream out(Wide(temp), std::ios::binary | std::ios::trunc);
		out << kCacheHeader << "\n";
		auto clean = [](std::string s) {
			for (char &c : s)
				if (c == '\t' || c == '\n' || c == '\r') c = ' ';
			return s;
		};
		for (const Demo &d : demos)
			out << d.path << '\t' << d.size << '\t' << d.modified << '\t' << (d.ok ? 1 : 0) << '\t' << clean(d.map) << '\t' << clean(d.player) << '\t'
			    << (d.coop ? 1 : 0) << '\t' << (d.host ? 1 : 0) << '\t' << (d.hasTime ? 1 : 0) << '\t' << d.ticks << '\t' << d.seconds << '\t'
			    << clean(d.error) << "\n";
	}
	MoveFileExW(Wide(temp).c_str(), Wide(CacheFile()).c_str(), MOVEFILE_REPLACE_EXISTING);
}

void Read(Demo &d) {
	p2demo::Demo demo;
	p2demo::ParseOptions options;
	options.entities = false;  // the header and SAR's data are enough, and far quicker to read
	std::string error;
	if (!p2demo::ParseFile(d.path.c_str(), demo, error, options)) {
		d.ok = false;
		d.error = error;
		return;
	}
	int ticks = 0;
	for (const auto &result : demo.sarSpeedruns)
		if (result.complete && result.totalTicks > 0) ticks = result.totalTicks;
	d.hasTime = ticks > 0;
	if (!d.hasTime) ticks = demo.lastTick - demo.firstTick;
	d.ok = true;
	d.ticks = ticks;
	d.seconds = ticks * demo.intervalPerTick;
	d.map = demo.header.map;
	d.player = demo.header.client;
	d.coop = d.map.compare(0, 8, "mp_coop_") == 0;
	d.host = demo.header.server.compare(0, 9, "localhost") == 0;
}

// How much two demos look like the two halves of one run: the same folder, names that start the same and end
// in neighbouring numbers (doors_wr_359667 / doors_wr_359668)
int Likeness(const Demo &a, const Demo &b) {
	int score = 0;
	if (Lower(fs::path(Wide(a.path)).parent_path().u8string()) == Lower(fs::path(Wide(b.path)).parent_path().u8string())) score += 1000;
	size_t same = 0;
	while (same < a.name.size() && same < b.name.size() && tolower(static_cast<unsigned char>(a.name[same])) == tolower(static_cast<unsigned char>(b.name[same])))
		same++;
	score += static_cast<int>(std::min<size_t>(same, 200));
	auto trailing = [](const std::string &name, long long &n) {
		size_t start = name.size();
		while (start > 0 && isdigit(static_cast<unsigned char>(name[start - 1])) && name.size() - start < 18) start--;
		if (start == name.size()) return false;
		n = _atoi64(name.c_str() + start);
		return true;
	};
	long long na, nb;
	if (trailing(a.name, na) && trailing(b.name, nb) && (na - nb == 1 || nb - na == 1)) score += 100;
	return score;
}

Run MakeRun(const Demo *demo, const Demo *partner) {
	Run r;
	r.demo = demo;
	r.partner = partner;
	r.files = demo->name + ".dem" + (partner ? "  +  " + partner->name + ".dem" : "");
	if (!demo->ok) {
		r.chamber = "(can't be read)";
		r.order = 100000;
	} else {
		r.chamber = maps::ChamberName(demo->map);
		r.order = maps::ChamberOrder(demo->map);
		r.players = demo->player + (partner ? "  +  " + partner->player : "");
		r.mode = !demo->coop ? "SP" : partner ? "Co-op" : demo->host ? "Co-op (blue)" : "Co-op (orange)";
		r.timeText = demo->hasTime ? route::FormatTime(demo->seconds) : demo->seconds > 0 ? "~" + route::FormatTime(demo->seconds) : "";
	}
	r.search = Lower(r.chamber + "\n" + (demo->ok ? demo->map : "") + "\n" + r.players + "\n" + r.mode + "\n" + r.files);
	return r;
}

// Puts the demos together into runs: two co-op demos are one run when they are of the same map and the same
// run time, one recorded by the host and one by the client.
std::vector<Run> Pair(const std::vector<Demo> &demos) {
	std::vector<Run> runs;
	std::map<std::string, std::vector<const Demo *>> groups;
	for (const Demo &d : demos) {
		if (!d.ok || !d.coop || !d.hasTime) {
			runs.push_back(MakeRun(&d, nullptr));
			continue;
		}
		groups[Lower(d.map) + "\n" + std::to_string(d.ticks)].push_back(&d);
	}
	for (auto &entry : groups) {
		std::vector<const Demo *> hosts, clients;
		for (const Demo *d : entry.second) (d->host ? hosts : clients).push_back(d);
		std::vector<const Demo *> used;
		auto isUsed = [&](const Demo *d) { return std::find(used.begin(), used.end(), d) != used.end(); };
		if (!hosts.empty() && !clients.empty() && hosts.size() * clients.size() <= 250000) {
			struct Candidate {
				int score;
				size_t order;
				const Demo *host, *client;
			};
			std::vector<Candidate> candidates;
			for (const Demo *h : hosts)
				for (const Demo *c : clients) candidates.push_back({Likeness(*h, *c), candidates.size(), h, c});
			std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) { return a.score > b.score; });
			for (const Candidate &c : candidates) {
				if (isUsed(c.host) || isUsed(c.client)) continue;
				runs.push_back(MakeRun(c.host, c.client));
				used.push_back(c.host);
				used.push_back(c.client);
			}
		}
		for (const Demo *d : entry.second)
			if (!isUsed(d)) runs.push_back(MakeRun(d, nullptr));
	}
	// game order, then fastest first; unreadable demos last
	std::stable_sort(runs.begin(), runs.end(), [](const Run &a, const Run &b) {
		if (a.order != b.order) return a.order < b.order;
		if (a.demo->ok && b.demo->ok && a.demo->seconds != b.demo->seconds) return a.demo->seconds < b.demo->seconds;
		return a.files < b.files;
	});
	return runs;
}

void Publish(std::vector<Demo> demos) {
	auto shelf = std::make_unique<Shelf>();
	shelf->demos = std::move(demos);
	shelf->runs = Pair(shelf->demos);
	std::lock_guard<std::mutex> lock(g_gate);
	g_fresh = std::move(shelf);
	g_version++;
}

void ReadAll(std::vector<std::string> folders) {
	// (lowest priority: this runs while the player plays)
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
	std::map<std::string, Demo> cache = LoadCache();
	std::vector<Demo> found;
	std::set<std::string> seen;
	for (const std::string &folder : folders) {
		std::error_code ec;
		fs::recursive_directory_iterator it(Wide(folder), fs::directory_options::skip_permission_denied, ec), end;
		for (; !ec && it != end && !g_stop; it.increment(ec)) {
			const fs::directory_entry &entry = *it;
			std::error_code e2;
			if (!entry.is_regular_file(e2)) continue;
			std::wstring ext = entry.path().extension().wstring();
			if (_wcsicmp(ext.c_str(), L".dem") != 0) continue;
			Demo d;
			d.path = Utf8(entry.path().wstring());
			std::string key = Lower(d.path);
			if (!seen.insert(key).second) continue;
			d.name = Utf8(entry.path().stem().wstring());
			d.size = static_cast<int64_t>(entry.file_size(e2));
			d.modified = static_cast<int64_t>(entry.last_write_time(e2).time_since_epoch().count());
			found.push_back(d);
		}
	}
	g_total = static_cast<int>(found.size());
	g_done = 0;
	// what the cache knows is shown at once; the rest is read and shown as it comes
	std::vector<Demo> known, unknown;
	for (Demo &d : found) {
		auto hit = cache.find(Lower(d.path));
		if (hit != cache.end() && hit->second.size == d.size && hit->second.modified == d.modified) {
			Demo c = hit->second;
			c.name = d.name;
			known.push_back(c);
		} else
			unknown.push_back(d);
	}
	g_done = static_cast<int>(known.size());
	Publish(known);
	std::vector<Demo> all = known;
	auto last = GetTickCount();
	for (Demo &d : unknown) {
		if (g_stop) return;
		Read(d);
		all.push_back(d);
		g_done++;
		if (GetTickCount() - last > 1500) {
			Publish(all);
			last = GetTickCount();
		}
	}
	Publish(all);
	SaveCache(all);
}

void Start() {
	LoadFolders();
	if (g_busy) return;
	if (g_reader.joinable()) g_reader.join();
	g_busy = true;
	g_stop = false;
	g_started = true;
	g_reader = std::thread([folders = g_folders] {
		ReadAll(folders);
		g_busy = false;
	});
}

}  // namespace

std::string DataFolder() { return engine::GameDirectory() + "\\p2practice"; }
std::string DownloadFolder() { return engine::GameDirectory() + "\\demos\\p2practice\\boards"; }

const std::vector<std::string> &Folders() {
	LoadFolders();
	return g_folders;
}

bool AddFolder(const std::string &path) {
	LoadFolders();
	std::string clean = path;
	while (!clean.empty() && (clean.back() == '\\' || clean.back() == '/' || clean.back() == ' ')) clean.pop_back();
	while (!clean.empty() && (clean.front() == '"' || clean.front() == ' ')) clean.erase(0, 1);
	if (!clean.empty() && clean.back() == '"') clean.pop_back();
	std::error_code ec;
	if (clean.empty() || !fs::is_directory(Wide(clean), ec)) return false;
	for (const std::string &f : g_folders)
		if (Lower(f) == Lower(clean)) return false;
	g_folders.push_back(clean);
	SaveFolders();
	Refresh();
	return true;
}

void RemoveFolder(size_t index) {
	LoadFolders();
	if (index >= g_folders.size()) return;
	g_folders.erase(g_folders.begin() + static_cast<long>(index));
	SaveFolders();
	Refresh();
}

void Refresh() {
	if (g_busy) g_stop = true;
	if (g_reader.joinable()) g_reader.join();
	g_busy = false;
	Start();
}

void Shutdown() {
	g_stop = true;
	if (g_reader.joinable()) g_reader.join();
	std::lock_guard<std::mutex> lock(g_gate);
	g_fresh.reset();
	g_shelf = std::make_unique<Shelf>();
	g_started = false;
}

const std::vector<Run> &Runs() {
	if (!g_started) Start();
	if (g_version != g_seenVersion) {
		std::lock_guard<std::mutex> lock(g_gate);
		if (g_fresh) g_shelf = std::move(g_fresh);
		g_seenVersion = g_version;
	}
	return g_shelf->runs;
}

int Version() { return g_seenVersion; }
bool Busy() { return g_busy; }
void Progress(int &done, int &total) {
	done = g_done;
	total = g_total;
}

std::string LoadCommand(const Run &run) {
	std::string arguments = "\"" + run.demo->path + "\"";
	if (run.partner) arguments += " \"" + run.partner->path + "\"";
	return arguments;
}

}  // namespace library
