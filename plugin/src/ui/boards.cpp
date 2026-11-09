#include "ui/boards.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "maps.h"
#include "ui/library.h"

#pragma comment(lib, "winhttp.lib")

namespace fs = std::filesystem;

namespace boards {
namespace {

constexpr const wchar_t *kHost = L"board.portal2.sr";

// the board's id of each chamber's map (a Portal 2 map not here isn't on the board)
const std::map<std::string, int> kIds = {
	{"sp_a1_intro1", 62761}, {"sp_a1_intro2", 62758}, {"sp_a1_intro3", 47458}, {"sp_a1_intro4", 47455},
	{"sp_a1_intro5", 47452}, {"sp_a1_intro6", 47106}, {"sp_a1_intro7", 62763}, {"sp_a1_wakeup", 62759},
	{"sp_a2_intro", 47735}, {"sp_a2_laser_intro", 62765}, {"sp_a2_laser_stairs", 47736},
	{"sp_a2_dual_lasers", 47738}, {"sp_a2_laser_over_goo", 47742}, {"sp_a2_catapult_intro", 62767},
	{"sp_a2_trust_fling", 47744}, {"sp_a2_pit_flings", 47465}, {"sp_a2_fizzler_intro", 47746},
	{"sp_a2_sphere_peek", 47748}, {"sp_a2_ricochet", 47751}, {"sp_a2_bridge_intro", 47752},
	{"sp_a2_bridge_the_gap", 47755}, {"sp_a2_turret_intro", 47756}, {"sp_a2_laser_relays", 47759},
	{"sp_a2_turret_blocker", 47760}, {"sp_a2_laser_vs_turret", 47763}, {"sp_a2_pull_the_rug", 47764},
	{"sp_a2_column_blocker", 47766}, {"sp_a2_laser_chaining", 47768}, {"sp_a2_triple_laser", 47770},
	{"sp_a2_bts1", 47773}, {"sp_a2_bts2", 47774}, {"sp_a2_bts3", 47776}, {"sp_a2_bts4", 47779},
	{"sp_a2_bts5", 47780}, {"sp_a2_core", 62771}, {"sp_a3_01", 47783}, {"sp_a3_03", 47784},
	{"sp_a3_jump_intro", 47787}, {"sp_a3_bomb_flings", 47468}, {"sp_a3_crazy_box", 47469},
	{"sp_a3_transition01", 47472}, {"sp_a3_speed_ramp", 47791}, {"sp_a3_speed_flings", 47793},
	{"sp_a3_portal_intro", 47795}, {"sp_a3_end", 47798}, {"sp_a4_intro", 88350}, {"sp_a4_tb_intro", 47800},
	{"sp_a4_tb_trust_drop", 47802}, {"sp_a4_tb_wall_button", 47804}, {"sp_a4_tb_polarity", 47806},
	{"sp_a4_tb_catch", 47808}, {"sp_a4_stop_the_box", 47811}, {"sp_a4_laser_catapult", 47813},
	{"sp_a4_laser_platform", 47815}, {"sp_a4_speed_tb_catch", 47817}, {"sp_a4_jump_polarity", 47819},
	{"sp_a4_finale1", 62776}, {"sp_a4_finale2", 47821}, {"sp_a4_finale3", 47824}, {"sp_a4_finale4", 47456},
	{"mp_coop_doors", 47741}, {"mp_coop_race_2", 47825}, {"mp_coop_laser_2", 47828},
	{"mp_coop_rat_maze", 47829}, {"mp_coop_laser_crusher", 45467}, {"mp_coop_teambts", 46362},
	{"mp_coop_fling_3", 47831}, {"mp_coop_infinifling_train", 47833}, {"mp_coop_come_along", 47835},
	{"mp_coop_fling_1", 47837}, {"mp_coop_catapult_1", 47840}, {"mp_coop_multifling_1", 47841},
	{"mp_coop_fling_crushers", 47844}, {"mp_coop_fan", 47845}, {"mp_coop_wall_intro", 47848},
	{"mp_coop_wall_2", 47849}, {"mp_coop_catapult_wall_intro", 47854}, {"mp_coop_wall_block", 47856},
	{"mp_coop_catapult_2", 47858}, {"mp_coop_turret_walls", 47861}, {"mp_coop_turret_ball", 52642},
	{"mp_coop_wall_5", 52660}, {"mp_coop_tbeam_redirect", 52662}, {"mp_coop_tbeam_drill", 52663},
	{"mp_coop_tbeam_catch_grind_1", 52665}, {"mp_coop_tbeam_laser_1", 52667}, {"mp_coop_tbeam_polarity", 52671},
	{"mp_coop_tbeam_polarity2", 52687}, {"mp_coop_tbeam_polarity3", 52689}, {"mp_coop_tbeam_maze", 52691},
	{"mp_coop_tbeam_end", 52777}, {"mp_coop_paint_come_along", 52694}, {"mp_coop_paint_redirect", 52711},
	{"mp_coop_paint_bridge", 52714}, {"mp_coop_paint_walljumps", 52715}, {"mp_coop_paint_speed_fling", 52717},
	{"mp_coop_paint_red_racer", 52735}, {"mp_coop_paint_speed_catch", 52738},
	{"mp_coop_paint_longjump_intro", 52740}, {"mp_coop_separation_1", 49341}, {"mp_coop_tripleaxis", 49343},
	{"mp_coop_catapult_catch", 49345}, {"mp_coop_2paints_1bridge", 49347}, {"mp_coop_paint_conversion", 49349},
	{"mp_coop_bridge_catch", 49351}, {"mp_coop_laser_tbeam", 52757}, {"mp_coop_paint_rat_maze", 52759},
	{"mp_coop_paint_crazy_box", 48287},
};

// the board's chapters and courses, each by the map that opens it
const char *const kChapterStarts[][2] = {
	{"sp_a1_intro1", "Chapter 1: The Courtesy Call"}, {"sp_a2_laser_intro", "Chapter 2: The Cold Boot"},
	{"sp_a2_sphere_peek", "Chapter 3: The Return"}, {"sp_a2_column_blocker", "Chapter 4: The Surprise"},
	{"sp_a2_bts3", "Chapter 5: The Escape"}, {"sp_a3_00", "Chapter 6: The Fall"}, {"sp_a3_speed_ramp", "Chapter 7: The Reunion"},
	{"sp_a4_intro", "Chapter 8: The Itch"}, {"sp_a4_finale1", "Chapter 9: The Part Where He Kills You"},
	{"mp_coop_doors", "Course 1: Team Building"}, {"mp_coop_fling_3", "Course 2: Mass and Velocity"},
	{"mp_coop_wall_intro", "Course 3: Hard-Light Surfaces"}, {"mp_coop_tbeam_redirect", "Course 4: Excursion Funnels"},
	{"mp_coop_paint_come_along", "Course 5: Mobility Gels"}, {"mp_coop_separation_1", "Course 6: Art Therapy"},
};

// ---- a small JSON reader (the board's answer: objects, strings, numbers, booleans, null) ----
struct Json {
	enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
	double number = 0;
	std::string text;
	std::vector<std::pair<std::string, Json>> members;  // objects (in order); arrays use the values only

	const Json *Get(const char *key) const {
		for (const auto &m : members)
			if (m.first == key) return &m.second;
		return nullptr;
	}
	// (a number too large for an int, or not a number at all, is 0: converting it would be undefined)
	int Int() const {
		if (kind == String) return atoi(text.c_str());
		if (kind != Number && kind != Bool) return 0;
		return number >= INT_MIN && number <= INT_MAX ? static_cast<int>(number) : 0;
	}
	std::string Text() const {
		if (kind == String) return text;
		if (kind != Number || !(number >= -9e18 && number <= 9e18)) return "";
		return std::to_string(static_cast<long long>(number));
	}
};

class Reader {
public:
	explicit Reader(const std::string &s) : s_(s) {}
	bool Parse(Json &out, int depth = 0) {
		Blank();
		if (p_ >= s_.size() || depth > 64) return false;
		char c = s_[p_];
		if (c == '{' || c == '[') {
			bool object = c == '{';
			out.kind = object ? Json::Object : Json::Array;
			p_++;
			Blank();
			if (p_ < s_.size() && s_[p_] == (object ? '}' : ']')) return ++p_, true;
			for (;;) {
				std::string key;
				if (object) {
					Blank();
					if (!String(key)) return false;
					Blank();
					if (p_ >= s_.size() || s_[p_++] != ':') return false;
				}
				Json value;
				if (!Parse(value, depth + 1)) return false;
				out.members.emplace_back(std::move(key), std::move(value));
				Blank();
				if (p_ >= s_.size()) return false;
				if (s_[p_] == ',') {
					p_++;
					continue;
				}
				if (s_[p_] == (object ? '}' : ']')) return ++p_, true;
				return false;
			}
		}
		if (c == '"') {
			out.kind = Json::String;
			return String(out.text);
		}
		if (!s_.compare(p_, 4, "true")) return out.kind = Json::Bool, out.number = 1, p_ += 4, true;
		if (!s_.compare(p_, 5, "false")) return out.kind = Json::Bool, out.number = 0, p_ += 5, true;
		if (!s_.compare(p_, 4, "null")) return out.kind = Json::Null, p_ += 4, true;
		char *end = nullptr;
		out.number = strtod(s_.c_str() + p_, &end);
		if (end == s_.c_str() + p_) return false;
		out.kind = Json::Number;
		p_ = end - s_.c_str();
		return true;
	}

private:
	void Blank() {
		while (p_ < s_.size() && (s_[p_] == ' ' || s_[p_] == '\n' || s_[p_] == '\r' || s_[p_] == '\t')) p_++;
	}
	bool String(std::string &out) {
		if (p_ >= s_.size() || s_[p_] != '"') return false;
		p_++;
		while (p_ < s_.size() && s_[p_] != '"') {
			char c = s_[p_++];
			if (c != '\\') {
				out += c;
				continue;
			}
			if (p_ >= s_.size()) return false;
			char e = s_[p_++];
			if (e == 'u' && p_ + 4 <= s_.size()) {
				unsigned code = strtoul(s_.substr(p_, 4).c_str(), nullptr, 16);
				p_ += 4;
				// (surrogate pairs)
				if (code >= 0xD800 && code < 0xDC00 && p_ + 6 <= s_.size() && s_[p_] == '\\' && s_[p_ + 1] == 'u') {
					unsigned low = strtoul(s_.substr(p_ + 2, 4).c_str(), nullptr, 16);
					p_ += 6;
					code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
				}
				if (code < 0x80) out += static_cast<char>(code);
				else if (code < 0x800) out += static_cast<char>(0xC0 | code >> 6), out += static_cast<char>(0x80 | (code & 0x3F));
				else if (code < 0x10000)
					out += static_cast<char>(0xE0 | code >> 12), out += static_cast<char>(0x80 | (code >> 6 & 0x3F)), out += static_cast<char>(0x80 | (code & 0x3F));
				else
					out += static_cast<char>(0xF0 | code >> 18), out += static_cast<char>(0x80 | (code >> 12 & 0x3F)),
						out += static_cast<char>(0x80 | (code >> 6 & 0x3F)), out += static_cast<char>(0x80 | (code & 0x3F));
			} else
				out += e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e == 'b' ? '\b' : e == 'f' ? '\f' : e;
		}
		if (p_ >= s_.size()) return false;
		p_++;
		return true;
	}
	const std::string &s_;
	size_t p_ = 0;
};

// ---- HTTPS ----
// The requests on their way, for Shutdown to break off: closing a request's handle makes the call waiting on it
// return at once, where it would otherwise sit out its timeouts with the game waiting for the thread. A handle is
// closed by whoever takes it off the list, so never twice.
std::mutex g_requestGate;
std::vector<HINTERNET> g_requests;
std::atomic<bool> g_stop{false};  // the plugin is going: no new requests, and the ones on their way give up

const char kStopped[] = "stopped";

// False when the plugin is going (the request is not listed then, and stays the caller's to close)
bool ListRequest(HINTERNET request) {
	std::lock_guard<std::mutex> lock(g_requestGate);
	if (g_stop) return false;
	g_requests.push_back(request);
	return true;
}

void CloseRequest(HINTERNET request) {
	{
		std::lock_guard<std::mutex> lock(g_requestGate);
		auto listed = std::find(g_requests.begin(), g_requests.end(), request);
		if (listed == g_requests.end()) return;  // (Shutdown closed it)
		g_requests.erase(listed);
	}
	WinHttpCloseHandle(request);
}

void StopRequests() {
	std::lock_guard<std::mutex> lock(g_requestGate);
	g_stop = true;
	for (HINTERNET request : g_requests) WinHttpCloseHandle(request);
	g_requests.clear();
}

// GET https://board.portal2.sr<path>: the body into `body`, or into the file `toFile` (which is there afterwards
// only when all of it came). False with `error` set.
bool Get(const std::wstring &path, std::string *body, const std::wstring &toFile, std::string &error) {
	if (g_stop) {
		error = kStopped;
		return false;
	}
	bool ok = false;
	HINTERNET session = WinHttpOpen(L"P2Practice", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	HINTERNET connect = session ? WinHttpConnect(session, kHost, INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr;
	HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
	                                                 WINHTTP_FLAG_SECURE)
	                            : nullptr;
	if (request && !ListRequest(request)) {
		WinHttpCloseHandle(request);
		request = nullptr;
		error = kStopped;
	}
	// (on the request itself: timeouts set on the session reach only handles made from it afterwards)
	if (request) WinHttpSetTimeouts(request, 10000, 10000, 20000, 60000);
	DWORD status = 0, size = sizeof(status);
	if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr) &&
	    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) {
		if (status != 200) {
			error = "the board answered " + std::to_string(status);
		} else {
			FILE *file = toFile.empty() ? nullptr : _wfopen(toFile.c_str(), L"wb");
			if (!toFile.empty() && !file) error = "couldn't write the demo file";
			else {
				// (done only when the board says there is no more: a read that fails, or a disk that is full,
				// leaves half a file, which must not pass for the demo)
				char buffer[16384];
				for (;;) {
					DWORD read = 0;
					if (g_stop) {
						error = kStopped;
						break;
					}
					if (!WinHttpReadData(request, buffer, sizeof(buffer), &read)) {
						error = "the connection to board.portal2.sr broke off (error " + std::to_string(GetLastError()) + ")";
						break;
					}
					if (read == 0) {
						ok = true;
						break;
					}
					if (!file) body->append(buffer, read);
					else if (fwrite(buffer, 1, read, file) != read) {
						error = "couldn't write the demo file (is the disk full?)";
						break;
					}
				}
				if (file && fclose(file) != 0 && ok) {
					ok = false;
					error = "couldn't write the demo file (is the disk full?)";
				}
				if (file && !ok) _wremove(toFile.c_str());
			}
		}
	} else if (error.empty()) {
		error = g_stop ? std::string(kStopped) : "no answer from board.portal2.sr (error " + std::to_string(GetLastError()) + ")";
	}
	if (request) CloseRequest(request);
	if (connect) WinHttpCloseHandle(connect);
	if (session) WinHttpCloseHandle(session);
	return ok;
}

std::wstring Wide(const std::string &utf8) {
	int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
	std::wstring out(size > 0 ? size - 1 : 0, L'\0');
	if (size > 1) MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &out[0], size);
	return out;
}

// "2024-05-01 12:34:56" -> seconds, for pairing co-op entries
long long Seconds(const std::string &date) {
	struct tm t = {};
	if (sscanf(date.c_str(), "%d-%d-%d %d:%d:%d", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min, &t.tm_sec) < 3) return 0;
	t.tm_year -= 1900;
	t.tm_mon -= 1;
	return static_cast<long long>(_mkgmtime(&t));
}

// A Steam id as the board gives it: digits only. (It becomes part of a demo's file name.)
bool IsSteamId(const std::string &text) {
	return !text.empty() && text.size() <= 20 && std::all_of(text.begin(), text.end(), [](unsigned char c) { return c >= '0' && c <= '9'; });
}

std::vector<Entry> ReadBoard(const std::string &map, const std::string &json, std::string &error) {
	std::vector<Entry> entries;
	Json top;
	if (!Reader(json).Parse(top) || top.kind != Json::Object) {
		error = "the board's answer can't be read";
		return entries;
	}
	std::vector<long long> when;
	for (const auto &row : top.members) {
		const Json *score = row.second.Get("scoreData"), *user = row.second.Get("userData");
		if (!score || !IsSteamId(row.first)) continue;
		Entry e;
		e.steamId = row.first;
		e.player = user && user->Get("boardname") ? user->Get("boardname")->Text() : "";
		e.score = score->Get("score") ? score->Get("score")->Int() : 0;
		e.rank = score->Get("scoreRank") ? score->Get("scoreRank")->Int() : 0;
		e.changelogId = score->Get("changelogId") ? score->Get("changelogId")->Int() : 0;
		e.hasDemo = score->Get("hasDemo") && score->Get("hasDemo")->Int() != 0;
		e.date = score->Get("date") ? score->Get("date")->Text() : "";
		if (e.changelogId > 0 && e.score > 0) entries.push_back(e);
	}
	std::stable_sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) { return a.score != b.score ? a.score < b.score : a.date < b.date; });
	if (map.compare(0, 3, "mp_") == 0) {
		// the two robots' entries of one run have the same time and date (seconds apart at most)
		std::vector<Entry> paired;
		for (size_t i = 0; i < entries.size(); i++) {
			Entry e = entries[i];
			e.coop = true;
			if (i + 1 < entries.size() && entries[i + 1].score == e.score && llabs(Seconds(entries[i + 1].date) - Seconds(e.date)) < 90) {
				const Entry &p = entries[i + 1];
				e.partnerPlayer = p.player;
				e.partnerSteamId = p.steamId;
				e.partnerChangelogId = p.changelogId;
				e.partnerHasDemo = p.hasDemo;
				i++;
			}
			paired.push_back(e);
		}
		entries = std::move(paired);
	}
	for (Entry &e : entries)
		if (e.date.size() > 10) e.date.resize(10);
	return entries;
}

std::string DemoPath(const std::string &folder, const std::string &map, int score, const std::string &steamId, int changelogId) {
	std::string chamber = maps::ChamberName(map);
	chamber.erase(std::remove(chamber.begin(), chamber.end(), ' '), chamber.end());
	return folder + "\\" + chamber + "_" + std::to_string(score) + "_" + steamId + "_" + std::to_string(changelogId) + ".dem";
}

bool Exists(const std::string &path) {
	std::error_code ec;
	return fs::exists(Wide(path), ec);
}

// ---- background work ----
std::mutex g_gate;
std::thread g_boardThread, g_downloadThread;
std::atomic<State> g_boardState{State::Idle}, g_downloadState{State::Idle};
std::string g_boardMap, g_boardError, g_downloadError;
std::string g_pendingMap;  // a chamber picked while another's board was still on its way: fetched next
std::vector<Entry> g_entries;
std::vector<std::string> g_files;
// What a thread hands over, under g_gate, with its state: BoardState and DownloadState take it, so everything
// above is the game thread's alone.
std::vector<Entry> g_freshEntries;
std::vector<std::string> g_freshFiles;
std::string g_freshBoardError, g_freshDownloadError;
bool g_freshBoard = false, g_freshDownload = false;

void Join(std::thread &t) {
	if (t.joinable()) t.join();
}

}  // namespace

const std::vector<Chapter> &Chapters() {
	static std::vector<Chapter> chapters;
	if (!chapters.empty()) return chapters;
	for (bool coop : {false, true})
		for (const auto &chamber : maps::Chambers(coop)) {
			for (const auto &start : kChapterStarts)
				if (chamber.first == start[0]) chapters.push_back({start[1], {}});
			if (!chapters.empty() && OnBoard(chamber.first)) chapters.back().chambers.push_back(chamber);
		}
	return chapters;
}

bool OnBoard(const std::string &map) { return kIds.count(map) != 0; }

std::string FormatScore(int score) {
	char text[32];
	int cs = score % 100, s = score / 100 % 60, m = score / 6000;
	if (m > 0) snprintf(text, sizeof(text), "%d:%02d.%02d", m, s, cs);
	else snprintf(text, sizeof(text), "%d.%02d", s, cs);
	return text;
}

void Fetch(const std::string &map) {
	if (g_boardState == State::Busy) {
		// (not dropped: the board on its way would otherwise be shown, and its demos named, as this chamber's)
		g_pendingMap = map;
		return;
	}
	g_pendingMap.clear();
	Join(g_boardThread);
	g_freshBoard = false;  // (a board that came and was never looked at: this one replaces it. No thread runs here.)
	auto id = kIds.find(map);
	g_boardMap = map;
	if (id == kIds.end()) {
		g_boardError = maps::ChamberName(map) + " isn't on the board";
		g_boardState = State::Failed;
		return;
	}
	g_boardState = State::Busy;
	g_boardThread = std::thread([map, id = id->second] {
		std::string json, error;
		std::vector<Entry> entries;
		if (Get(L"/chamber/" + std::to_wstring(id) + L"/json", &json, L"", error)) entries = ReadBoard(map, json, error);
		std::lock_guard<std::mutex> lock(g_gate);
		g_freshEntries = std::move(entries);
		g_freshBoardError = error;
		g_freshBoard = true;
		g_boardState = error.empty() ? State::Ready : State::Failed;
	});
}

State BoardState() {
	State state;
	{
		// (the state under the same lock as the entries: "ready" is never told with the board before still there)
		std::lock_guard<std::mutex> lock(g_gate);
		if (g_freshBoard) {
			g_entries = std::move(g_freshEntries);
			g_boardError = std::move(g_freshBoardError);
			g_freshBoard = false;
		}
		state = g_boardState;
	}
	if (g_pendingMap.empty() || state == State::Busy) return state;
	// the chamber picked while that board was coming
	std::string next = g_pendingMap;  // (a copy: Fetch clears g_pendingMap)
	Fetch(next);
	return BoardState();
}

const std::string &BoardMap() { return g_boardMap; }
const std::vector<Entry> &Entries() { return g_entries; }
const std::string &BoardError() { return g_boardError; }

bool Downloaded(const std::string &map, const Entry &e) {
	std::string folder = library::DownloadFolder();
	return Exists(DemoPath(folder, map, e.score, e.steamId, e.changelogId)) &&
	       (!e.partnerChangelogId || Exists(DemoPath(folder, map, e.score, e.partnerSteamId, e.partnerChangelogId)));
}

void Download(const std::string &map, const Entry &entry) {
	if (g_downloadState == State::Busy) return;
	Join(g_downloadThread);
	g_downloadState = State::Busy;
	g_downloadThread = std::thread([map, entry, folder = library::DownloadFolder()] {
		std::vector<std::string> files;
		std::string error;
		std::error_code ec;
		fs::create_directories(Wide(folder), ec);
		struct One {
			std::string steamId;
			int changelogId;
		};
		std::vector<One> ones = {{entry.steamId, entry.changelogId}};
		if (entry.partnerChangelogId) ones.push_back({entry.partnerSteamId, entry.partnerChangelogId});
		for (const One &one : ones) {
			std::string path = DemoPath(folder, map, entry.score, one.steamId, one.changelogId);
			if (Exists(path)) {
				files.push_back(path);
				continue;
			}
			std::wstring temp = Wide(path + ".part");
			if (!Get(L"/getDemo?id=" + std::to_wstring(one.changelogId), nullptr, temp, error)) break;  // (and no file is left)
			char head[8] = {};
			if (FILE *f = _wfopen(temp.c_str(), L"rb")) {
				fread(head, 1, 8, f);
				fclose(f);
			}
			if (memcmp(head, "HL2DEMO", 7) != 0) {
				_wremove(temp.c_str());
				error = "the board sent no demo for this run (it may not be public)";
				break;
			}
			if (!MoveFileExW(temp.c_str(), Wide(path).c_str(), MOVEFILE_REPLACE_EXISTING)) {
				_wremove(temp.c_str());
				error = "couldn't save the demo file";
				break;
			}
			files.push_back(path);
		}
		std::lock_guard<std::mutex> lock(g_gate);
		g_freshFiles = std::move(files);
		g_freshDownloadError = error;
		g_freshDownload = true;
		g_downloadState = error.empty() ? State::Ready : State::Failed;
	});
}

State DownloadState() {
	std::lock_guard<std::mutex> lock(g_gate);
	if (g_freshDownload) {
		g_files = std::move(g_freshFiles);
		g_downloadError = std::move(g_freshDownloadError);
		g_freshDownload = false;
	}
	return g_downloadState;
}

const std::vector<std::string> &DownloadedFiles() { return g_files; }
const std::string &DownloadError() { return g_downloadError; }

void Shutdown() {
	// (requests on their way are broken off, so the threads end at once; a download cut short leaves no file)
	StopRequests();
	Join(g_boardThread);
	Join(g_downloadThread);
	g_stop = false;
}

}  // namespace boards
